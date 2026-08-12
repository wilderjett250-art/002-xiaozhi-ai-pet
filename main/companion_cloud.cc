#include "companion_cloud.h"

#include <cstring>
#include <cctype>
#include <ctime>
#include <utility>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "sdkconfig.h"
#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#if !CONFIG_IDF_TARGET_ESP32
#include "dual_network_board.h"
#endif
#include "mcp_server.h"
#include "music_player.h"
#include "settings.h"
#include "system_info.h"

#define TAG "CompanionCloud"

namespace {
constexpr size_t kMaxQueuedChats = 64;
constexpr size_t kMaxQueuedPhotos = 6;
constexpr int64_t kHeartbeatIntervalUs = 30000000;
constexpr int64_t kCommandPollIntervalUs = 5000000;
constexpr int64_t kInitialNetworkDelayUs = 15000000;
constexpr int64_t kNetworkOperationGapUs = 1500000;
constexpr int64_t kCloudFailureBackoffUs = 60000000;
// The S3 camera build keeps most data in PSRAM and has about 25-30 KiB of
// internal heap while idle. Keep enough headroom for the ML307 HTTP wrapper,
// but do not permanently suppress heartbeat and command polling.
constexpr size_t kMinInternalHeapForCloud = 20 * 1024;
constexpr int kFailedUploadRetryMs = 5000;
constexpr int kMaxPhotoUploadAttempts = 5;
constexpr int kMaxAmbientUploadAttempts = 3;

void ReportCloudReachability(bool success) {
#if CONFIG_IDF_TARGET_ESP32
    (void)success;
#else
    auto* dual_board = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    if (dual_board != nullptr) {
        dual_board->ReportInternetAccessResult(success);
    }
#endif
}

bool IsSafeGatewayAction(const std::string& action) {
    if (action.empty() || action.size() > 64) {
        return false;
    }
    for (unsigned char ch : action) {
        if (!std::isalnum(ch) && ch != '_' && ch != '.' && ch != '-') {
            return false;
        }
    }
    return true;
}

bool IsHttpsUrl(const std::string& value) {
    return value.rfind("https://", 0) == 0 && value.size() <= 1024;
}

bool IsSha256(const std::string& value) {
    if (value.size() != 64) {
        return false;
    }
    for (unsigned char ch : value) {
        if (!std::isxdigit(ch)) {
            return false;
        }
    }
    return true;
}

std::string JsonString(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    std::string value = text == nullptr ? "{}" : text;
    if (text != nullptr) {
        cJSON_free(text);
    }
    return value;
}

void AddMultipartField(std::string& body, const std::string& boundary,
                       const char* name, const std::string& value) {
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"" + std::string(name) + "\"\r\n\r\n";
    body += value + "\r\n";
}
}  // namespace

CompanionCloud& CompanionCloud::GetInstance() {
    static CompanionCloud instance;
    return instance;
}

CompanionCloud::CompanionCloud()
    : base_url_(CONFIG_COMPANION_CLOUD_URL),
      device_token_(CONFIG_COMPANION_CLOUD_DEVICE_TOKEN),
      device_id_(SystemInfo::GetMacAddress()),
      device_name_(CONFIG_COMPANION_CLOUD_DEVICE_NAME) {
    while (!base_url_.empty() && base_url_.back() == '/') {
        base_url_.pop_back();
    }
    Settings settings("companion_cloud", false);
    ambient_requested_.store(settings.GetBool("ambient", true));
}

bool CompanionCloud::IsEnabled() const {
    return !base_url_.empty() && !device_token_.empty();
}

bool CompanionCloud::IsOnline() const {
    return IsEnabled() && online_.load();
}

void CompanionCloud::RegisterTools() {
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool("self.private_server.gateway",
        "Call the owner's private companion server through one controlled gateway. Actions: help, status, "
        "recent_chats, recent_photos, latest_photo, ota_status. Use this for server records and private cloud "
        "features; do not use it for local volume, screen, camera capture, music or network controls.",
        PropertyList({
            Property("action", kPropertyTypeString, std::string("status")),
            Property("request", kPropertyTypeString, std::string("")),
            Property("parameters", kPropertyTypeString, std::string("{}"))
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            return CallGateway(properties["action"].value<std::string>(),
                               properties["request"].value<std::string>(),
                               properties["parameters"].value<std::string>());
        });
}

std::string CompanionCloud::CallGateway(const std::string& action, const std::string& request,
                                        const std::string& parameters) {
    if (!IsEnabled()) {
        return "Private server is not configured";
    }
    if (!IsSafeGatewayAction(action) || request.size() > 1200 || parameters.size() > 1600) {
        return "Invalid private server gateway request";
    }
    if (!TryAcquireBackgroundNetwork()) {
        return "Private server is busy; retry shortly";
    }

    auto root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "action", action.c_str());
    cJSON_AddStringToObject(root, "request", request.c_str());
    cJSON_AddStringToObject(root, "parameters", parameters.c_str());
    std::string body = JsonString(root);
    cJSON_Delete(root);

    std::string response;
    bool ok = PostJson("/api/v1/device/gateway", std::move(body), &response, 12000);
    ReleaseBackgroundNetwork();
    if (!ok) {
        return "Private server request failed";
    }
    return response.size() <= 4000 ? response : response.substr(0, 4000);
}

bool CompanionCloud::TryAcquireBackgroundNetwork() {
    bool expected = false;
    return background_network_busy_.compare_exchange_strong(expected, true);
}

void CompanionCloud::ReleaseBackgroundNetwork() {
    background_network_busy_.store(false);
}

void CompanionCloud::Start() {
    if (!IsEnabled()) {
        online_.store(false);
        ESP_LOGI(TAG, "Cloud archive disabled: server URL or device token is empty");
        return;
    }
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return;
    }
    if (xTaskCreate(TaskEntry, "companion_cloud", 8192, this, 1, &task_handle_) != pdPASS) {
        started_.store(false);
        task_handle_ = nullptr;
        ESP_LOGE(TAG, "Failed to start companion cloud task");
        return;
    }
    ESP_LOGI(TAG, "Cloud archive enabled: %s", base_url_.c_str());
}

void CompanionCloud::TaskEntry(void* arg) {
    static_cast<CompanionCloud*>(arg)->Run();
    vTaskDelete(nullptr);
}

int64_t CompanionCloud::WallClockMs() {
    time_t now = time(nullptr);
    return now > 1700000000 ? static_cast<int64_t>(now) * 1000 : 0;
}

void CompanionCloud::RecordChat(const std::string& role, const std::string& content,
                                const std::string& session_id) {
    if (!IsEnabled() || content.empty() || (role != "user" && role != "assistant")) {
        return;
    }
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (chat_queue_.size() >= kMaxQueuedChats) {
        chat_queue_.pop_front();
    }
    chat_queue_.push_back({role, content, session_id, WallClockMs()});
}

bool CompanionCloud::QueueVoicePhoto(CameraPhoto photo, const std::string& prompt,
                                     const std::string& analysis) {
    if (!IsEnabled()) {
        ESP_LOGW(TAG, "Voice photo archive skipped: cloud is disabled");
        return false;
    }
    if (photo.data == nullptr || photo.size == 0) {
        ESP_LOGW(TAG, "Voice photo archive skipped: JPEG encoding produced no data");
        return false;
    }
    PhotoEvent event;
    event.photo = std::move(photo);
    event.source = "voice";
    event.prompt = prompt;
    event.analysis = analysis;
    event.captured_at_ms = WallClockMs();

    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (photo_queue_.size() >= kMaxQueuedPhotos) {
        ESP_LOGW(TAG, "Photo archive queue is full");
        return false;
    }
    photo_queue_.push_back(std::move(event));
    ESP_LOGI(TAG, "Voice photo queued for archive: bytes=%u pending=%u",
             static_cast<unsigned>(photo_queue_.back().photo.size),
             static_cast<unsigned>(photo_queue_.size()));
    if (auto display = Board::GetInstance().GetDisplay(); display != nullptr) {
        display->ShowNotification("照片正在保存");
    }
    return true;
}

std::string CompanionCloud::ApiUrl(const std::string& path) const {
    return base_url_ + path;
}

bool CompanionCloud::PostJson(const std::string& path, std::string body,
                              std::string* response, int timeout_ms) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    auto http = network->CreateHttp(6);
    if (http == nullptr) {
        return false;
    }
    http->SetTimeout(timeout_ms);
    http->SetHeader("Authorization", "Bearer " + device_token_);
    http->SetHeader("X-Device-Id", device_id_);
    http->SetHeader("Content-Type", "application/json");
    http->SetContent(std::move(body));
    if (!http->Open("POST", ApiUrl(path))) {
        ReportCloudReachability(false);
        ESP_LOGW(TAG, "POST %s open failed", path.c_str());
        return false;
    }
    int status = http->GetStatusCode();
    std::string payload = http->ReadAll();
    http->Close();
    if (response != nullptr) {
        *response = std::move(payload);
    }
    const bool success = status >= 200 && status < 300;
    ReportCloudReachability(success);
    if (!success) {
        ESP_LOGW(TAG, "POST %s failed with status %d", path.c_str(), status);
    }
    return success;
}

bool CompanionCloud::SendHeartbeat() {
    auto root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", device_name_.c_str());
    cJSON_AddStringToObject(root, "firmware", SystemInfo::GetUserAgent().c_str());
    const char* network = "wifi";
#if !CONFIG_IDF_TARGET_ESP32
    auto dual_board = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    network = dual_board != nullptr && dual_board->GetNetworkType() == NetworkType::ML307
        ? "4g" : "wifi";
#endif
    cJSON_AddStringToObject(root, "network", network);
    if (dual_board != nullptr) {
        cJSON_AddStringToObject(root, "network_primary", "4g");
        cJSON_AddStringToObject(root, "network_standby",
                                dual_board->GetNetworkType() == NetworkType::ML307 ? "wifi" : "4g");
        cJSON_AddBoolToObject(root, "automatic_failover", dual_board->IsAutomaticFailoverEnabled());
        cJSON_AddBoolToObject(root, "network_connected", dual_board->IsNetworkConnected());
    }
    cJSON_AddStringToObject(root, "state",
                            std::to_string(static_cast<int>(Application::GetInstance().GetDeviceState())).c_str());
    cJSON_AddBoolToObject(root, "ambient_enabled", ambient_requested_.load());
    std::string body = JsonString(root);
    cJSON_Delete(root);
    std::string response;
    if (!PostJson("/api/v1/device/heartbeat", std::move(body), &response)) {
        if (ambient_effective_.exchange(false)) {
            Application::GetInstance().Schedule([]() {
                Application::GetInstance().RefreshIdleAudioMode();
            });
        }
        return false;
    }

    bool effective = false;
    auto response_root = cJSON_Parse(response.c_str());
    if (response_root != nullptr) {
        auto ambient = cJSON_GetObjectItem(response_root, "ambient");
        auto supported = cJSON_GetObjectItem(ambient, "supported");
        auto enabled = cJSON_GetObjectItem(ambient, "enabled");
        auto speech_ready = cJSON_GetObjectItem(ambient, "speech_ready");
        effective = cJSON_IsTrue(supported) && cJSON_IsTrue(enabled) &&
                    cJSON_IsTrue(speech_ready) && ambient_requested_.load();
        cJSON_Delete(response_root);
    }
    if (ambient_effective_.exchange(effective) != effective) {
        ESP_LOGI(TAG, "Ambient listening is %s", effective ? "ready" : "disabled");
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().RefreshIdleAudioMode();
        });
    }
    return true;
}

bool CompanionCloud::SendChat(const ChatEvent& event) {
    auto root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", event.session_id.c_str());
    cJSON_AddStringToObject(root, "role", event.role.c_str());
    cJSON_AddStringToObject(root, "content", event.content.c_str());
    cJSON_AddNumberToObject(root, "occurred_at_ms", static_cast<double>(event.occurred_at_ms));
    std::string body = JsonString(root);
    cJSON_Delete(root);
    return PostJson("/api/v1/device/chats", std::move(body));
}

bool CompanionCloud::UploadAmbientAudio(const std::vector<uint8_t>& data, int duration_ms) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr || data.empty()) {
        return false;
    }
    auto http = network->CreateHttp(6);
    if (http == nullptr) {
        return false;
    }
    http->SetTimeout(30000);
    http->SetHeader("Authorization", "Bearer " + device_token_);
    http->SetHeader("X-Device-Id", device_id_);
    http->SetHeader("Content-Type", "application/x-cat-opus");
    http->SetHeader("X-Ambient-Duration-Ms", std::to_string(duration_ms));
    http->SetContent(std::string(reinterpret_cast<const char*>(data.data()), data.size()));
    if (!http->Open("POST", ApiUrl("/api/v1/device/ambient-audio"))) {
        ReportCloudReachability(false);
        return false;
    }
    const int status = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();
    const bool success = status >= 200 && status < 300;
    ReportCloudReachability(success);
    if (!success) {
        ESP_LOGW(TAG, "Ambient upload failed with status %d: %.160s", status, response.c_str());
    } else {
        ESP_LOGI(TAG, "Ambient speech uploaded: %d ms, %u bytes",
                 duration_ms, static_cast<unsigned>(data.size()));
    }
    return success;
}

int CompanionCloud::UploadPhoto(const PhotoEvent& event) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr || event.photo.data == nullptr || event.photo.size == 0) {
        return 0;
    }
    auto http = network->CreateHttp(6);
    if (http == nullptr) {
        return 0;
    }
    const std::string boundary = "----CAT_COMPANION_PHOTO";
    http->SetTimeout(30000);
    http->SetHeader("Authorization", "Bearer " + device_token_);
    http->SetHeader("X-Device-Id", device_id_);
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

    std::string body;
    body.reserve(event.photo.size + 1024 + event.prompt.size() + event.analysis.size());
    AddMultipartField(body, boundary, "source", event.source);
    AddMultipartField(body, boundary, "prompt", event.prompt);
    AddMultipartField(body, boundary, "analysis", event.analysis);
    AddMultipartField(body, boundary, "width", std::to_string(event.photo.width));
    AddMultipartField(body, boundary, "height", std::to_string(event.photo.height));
    AddMultipartField(body, boundary, "captured_at_ms", std::to_string(event.captured_at_ms));
    if (event.command_id > 0) {
        AddMultipartField(body, boundary, "command_id", std::to_string(event.command_id));
    }
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"image\"; filename=\"capture.jpg\"\r\n";
    body += "Content-Type: image/jpeg\r\n\r\n";
    body.append(reinterpret_cast<const char*>(event.photo.data), event.photo.size);
    body += "\r\n--" + boundary + "--\r\n";

    ESP_LOGI(TAG, "Uploading photo: source=%s bytes=%u attempt=%d",
             event.source.c_str(), static_cast<unsigned>(event.photo.size), event.attempts + 1);
    http->SetContent(std::move(body));
    if (!http->Open("POST", ApiUrl("/api/v1/device/photos"))) {
        ReportCloudReachability(false);
        return 0;
    }
    int status = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();
    const bool success = status >= 200 && status < 300;
    ReportCloudReachability(success);
    if (!success) {
        ESP_LOGW(TAG, "Photo upload failed with status %d: %.256s", status, response.c_str());
        return 0;
    }

    int photo_id = 0;
    auto root = cJSON_Parse(response.c_str());
    if (root != nullptr) {
        auto item = cJSON_GetObjectItem(root, "photo_id");
        if (cJSON_IsNumber(item)) {
            photo_id = item->valueint;
        }
        cJSON_Delete(root);
    }
    ESP_LOGI(TAG, "Photo archived: source=%s id=%d bytes=%u",
             event.source.c_str(), photo_id, static_cast<unsigned>(event.photo.size));
    return photo_id;
}

bool CompanionCloud::CompleteCommand(int command_id, bool success, int photo_id,
                                     const std::string& error) {
    auto root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", success ? "completed" : "failed");
    if (photo_id > 0) {
        cJSON_AddNumberToObject(root, "photo_id", photo_id);
    }
    cJSON_AddStringToObject(root, "error", error.c_str());
    std::string body = JsonString(root);
    cJSON_Delete(root);
    return PostJson("/api/v1/device/commands/" + std::to_string(command_id) + "/complete", std::move(body));
}

void CompanionCloud::HandleRemoteCapture(int command_id) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto camera = board.GetCamera();
    if (display != nullptr) {
        display->SetStatus("远程拍照");
        display->ShowNotification("后台正在拍照");
    }
    if (camera == nullptr) {
        CompleteCommand(command_id, false, 0, "camera unavailable");
        return;
    }

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    CameraPhoto photo;
    bool encoded = camera->CaptureAndGetJpeg(photo, 78, true);
    board.SetPowerSaveLevel(PowerSaveLevel::BALANCED);
    if (!encoded) {
        CompleteCommand(command_id, false, 0, "capture failed");
        if (display != nullptr) {
            display->ShowNotification("远程拍照失败");
        }
        return;
    }

    PhotoEvent event;
    event.photo = std::move(photo);
    event.source = "admin";
    event.prompt = "后台远程拍照";
    event.command_id = command_id;
    event.captured_at_ms = WallClockMs();
    int photo_id = UploadPhoto(event);
    if (photo_id > 0) {
        if (display != nullptr) {
            display->ShowNotification("照片已保存");
        }
    } else {
        CompleteCommand(command_id, false, 0, "photo upload failed");
        if (display != nullptr) {
            display->ShowNotification("照片上传失败");
        }
    }
}

void CompanionCloud::HandleRemoteGreeting(int command_id, const std::string& message) {
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateIdle) {
        CompleteCommand(command_id, false, 0, "device is busy");
        return;
    }
    if (!CompleteCommand(command_id, true, 0, "")) {
        ESP_LOGW(TAG, "Greeting command acknowledgement failed: id=%d", command_id);
        return;
    }

    const std::string greeting = message.empty() ? "你在干嘛呀？" : message;
    if (auto display = Board::GetInstance().GetDisplay(); display != nullptr) {
        display->SetStatus("主动问候");
        display->SetEmotion("happy");
        display->SetChatMessage("assistant", greeting.c_str());
    }
    ESP_LOGI(TAG, "Remote greeting accepted: id=%d message=%s", command_id, greeting.c_str());
    app.PlaySound(Lang::Sounds::OGG_PROACTIVE_GREETING);
    app.GetAudioService().WaitForPlaybackQueueEmpty();
    app.StartListening(kListeningModeAutoStop);
}

void CompanionCloud::HandleAmbientConfig(int command_id, bool enabled) {
    Settings settings("companion_cloud", true);
    settings.SetBool("ambient", enabled);
    ambient_requested_.store(enabled);
    if (!enabled) {
        ambient_effective_.store(false);
    }
    CompleteCommand(command_id, true, 0, "");
    Application::GetInstance().Schedule([]() {
        Application::GetInstance().RefreshIdleAudioMode();
    });
    ESP_LOGI(TAG, "Ambient listening preference updated: %s", enabled ? "enabled" : "disabled");
}

void CompanionCloud::HandleProactiveSpeak(int command_id, const std::string& text,
                                           const std::string& audio_url) {
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateIdle) {
        CompleteCommand(command_id, false, 0, "device is busy");
        return;
    }
    if (!IsHttpsUrl(audio_url)) {
        CompleteCommand(command_id, false, 0, "proactive speech requires HTTPS audio");
        return;
    }

    if (auto display = Board::GetInstance().GetDisplay(); display != nullptr) {
        display->SetStatus("小喵想和你聊聊");
        display->SetEmotion("happy");
        display->SetChatMessage("assistant", text.c_str());
    }
    app.GetAudioService().EnableAmbientCapture(false);
    app.GetAudioService().EnableVoiceProcessing(false);
    auto& player = MusicPlayer::GetInstance();
    if (!player.PlayRemotePrompt(audio_url, "小喵主动问候")) {
        CompleteCommand(command_id, false, 0, "cannot start proactive speech");
        app.RefreshIdleAudioMode();
        return;
    }

    const int64_t deadline = esp_timer_get_time() + 45 * 1000000LL;
    while (esp_timer_get_time() < deadline) {
        const auto state = player.GetState();
        if (state == MusicPlayer::State::Completed || state == MusicPlayer::State::Error ||
            state == MusicPlayer::State::Stopped) {
            break;
        }
        if (app.GetDeviceState() != kDeviceStateIdle) {
            CompleteCommand(command_id, true, 0, "");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    const bool played = player.GetState() == MusicPlayer::State::Completed;
    CompleteCommand(command_id, played, 0, played ? "" : "proactive speech playback failed");
    if (played && app.GetDeviceState() == kDeviceStateIdle) {
        app.StartListening(kListeningModeAutoStop);
    } else {
        app.RefreshIdleAudioMode();
    }
}

void CompanionCloud::HandleRemoteUpgrade(int command_id, const std::string& url,
                                         const std::string& version, const std::string& sha256) {
    if (!IsHttpsUrl(url) || !IsSha256(sha256)) {
        CompleteCommand(command_id, false, 0, "OTA requires HTTPS URL and SHA-256");
        return;
    }

    Application::GetInstance().Schedule([this, command_id, url, version, sha256]() {
        auto& app = Application::GetInstance();
        bool success = app.UpgradeFirmware(url, version, sha256, false);
        CompleteCommand(command_id, success, 0, success ? "" : "firmware upgrade failed");
        if (success) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            app.Reboot();
        }
    });
}

bool CompanionCloud::PollRemoteCommand(int wait_seconds) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    auto http = network->CreateHttp(6);
    if (http == nullptr) {
        return false;
    }
    http->SetTimeout((wait_seconds + 5) * 1000);
    http->SetHeader("Authorization", "Bearer " + device_token_);
    http->SetHeader("X-Device-Id", device_id_);
    if (!http->Open("GET", ApiUrl("/api/v1/device/commands/next?wait_seconds=" +
                                   std::to_string(wait_seconds)))) {
        ReportCloudReachability(false);
        ESP_LOGW(TAG, "Command poll open failed");
        return false;
    }
    int status = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();
    const bool success = status == 200;
    ReportCloudReachability(success);
    if (!success) {
        ESP_LOGW(TAG, "Command poll failed with status %d", status);
        return false;
    }

    auto root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Command poll returned invalid JSON");
        return false;
    }
    auto command = cJSON_GetObjectItem(root, "command");
    if (cJSON_IsObject(command)) {
        auto id = cJSON_GetObjectItem(command, "id");
        auto name = cJSON_GetObjectItem(command, "command");
        if (cJSON_IsNumber(id) && cJSON_IsString(name)) {
            int command_id = id->valueint;
            std::string command_name = name->valuestring;
            if (command_name == "capture") {
                cJSON_Delete(root);
                HandleRemoteCapture(command_id);
                return true;
            }
            if (command_name == "greet") {
                auto payload = cJSON_GetObjectItem(command, "payload");
                auto message = cJSON_GetObjectItem(payload, "message");
                std::string message_value = cJSON_IsString(message) ? message->valuestring : "";
                cJSON_Delete(root);
                HandleRemoteGreeting(command_id, message_value);
                return true;
            }
            if (command_name == "ambient_config") {
                auto payload = cJSON_GetObjectItem(command, "payload");
                auto enabled = cJSON_GetObjectItem(payload, "enabled");
                const bool enabled_value = cJSON_IsTrue(enabled);
                cJSON_Delete(root);
                HandleAmbientConfig(command_id, enabled_value);
                return true;
            }
            if (command_name == "proactive_speak") {
                auto payload = cJSON_GetObjectItem(command, "payload");
                auto text = cJSON_GetObjectItem(payload, "text");
                auto audio_url = cJSON_GetObjectItem(payload, "audio_url");
                std::string text_value = cJSON_IsString(text) ? text->valuestring : "";
                std::string audio_url_value = cJSON_IsString(audio_url) ? audio_url->valuestring : "";
                cJSON_Delete(root);
                HandleProactiveSpeak(command_id, text_value, audio_url_value);
                return true;
            }
            if (command_name == "firmware_update") {
                auto payload = cJSON_GetObjectItem(command, "payload");
                auto url = cJSON_GetObjectItem(payload, "url");
                auto version = cJSON_GetObjectItem(payload, "version");
                auto sha256 = cJSON_GetObjectItem(payload, "sha256");
                std::string url_value = cJSON_IsString(url) ? url->valuestring : "";
                std::string version_value = cJSON_IsString(version) ? version->valuestring : "";
                std::string sha256_value = cJSON_IsString(sha256) ? sha256->valuestring : "";
                cJSON_Delete(root);
                HandleRemoteUpgrade(command_id, url_value, version_value, sha256_value);
                return true;
            }
            cJSON_Delete(root);
            CompleteCommand(command_id, false, 0, "unsupported remote command");
            return true;
        }
    }
    cJSON_Delete(root);
    return true;
}

void CompanionCloud::Run() {
    int64_t now_us = esp_timer_get_time();
    int64_t next_heartbeat_us = now_us + kInitialNetworkDelayUs;
    int64_t next_command_poll_us = now_us + kInitialNetworkDelayUs;
    int64_t next_network_operation_us = now_us;

    while (true) {
        now_us = esp_timer_get_time();
        const auto state = Application::GetInstance().GetDeviceState();
        const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (state != kDeviceStateIdle || free_internal < kMinInternalHeapForCloud ||
            now_us < next_network_operation_us) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        bool has_chat = false;
        bool has_photo = false;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            has_chat = !chat_queue_.empty();
            has_photo = !photo_queue_.empty();
        }
        const bool has_ambient = ambient_effective_.load() &&
                                 Application::GetInstance().GetAudioService().HasAmbientAudio();

        const bool heartbeat_due = now_us >= next_heartbeat_us;
        const bool command_poll_due = now_us >= next_command_poll_us;
        if (!has_photo && !has_chat && !has_ambient && !heartbeat_due && !command_poll_due) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        if (!TryAcquireBackgroundNetwork()) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        bool operation_failed = false;
        if (heartbeat_due) {
            const bool heartbeat_ok = SendHeartbeat();
            online_.store(heartbeat_ok);
            if (!heartbeat_ok) {
                operation_failed = true;
                ESP_LOGW(TAG, "Heartbeat failed, free internal heap=%u",
                         static_cast<unsigned>(free_internal));
                const int64_t retry_at = esp_timer_get_time() + kCloudFailureBackoffUs;
                next_heartbeat_us = retry_at;
                if (next_command_poll_us < retry_at) {
                    next_command_poll_us = retry_at;
                }
            } else {
                next_heartbeat_us = esp_timer_get_time() + kHeartbeatIntervalUs;
            }
        } else if (command_poll_due) {
            const bool command_ok = PollRemoteCommand(0);
            if (!command_ok) {
                operation_failed = true;
                ESP_LOGW(TAG, "Command poll failed, free internal heap=%u",
                         static_cast<unsigned>(free_internal));
                const int64_t retry_at = esp_timer_get_time() + kCloudFailureBackoffUs;
                next_command_poll_us = retry_at;
                if (next_heartbeat_us < retry_at) {
                    next_heartbeat_us = retry_at;
                }
            } else {
                next_command_poll_us = esp_timer_get_time() + kCommandPollIntervalUs;
            }
        } else if (has_photo) {
            PhotoEvent photo;
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                photo = std::move(photo_queue_.front());
                photo_queue_.pop_front();
            }
            if (UploadPhoto(photo) == 0) {
                photo.attempts++;
                if (photo.attempts < kMaxPhotoUploadAttempts) {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    photo_queue_.push_back(std::move(photo));
                } else {
                    ESP_LOGE(TAG, "Photo archive abandoned after %d attempts",
                             photo.attempts);
                    if (auto display = Board::GetInstance().GetDisplay(); display != nullptr) {
                        display->ShowNotification("照片保存失败");
                    }
                }
                operation_failed = true;
            } else if (auto display = Board::GetInstance().GetDisplay(); display != nullptr) {
                display->ShowNotification("照片已保存到云端");
            }
        } else if (has_ambient) {
            auto segment = Application::GetInstance().GetAudioService().PopAmbientAudio();
            if (segment != nullptr && !UploadAmbientAudio(segment->data, segment->duration_ms)) {
                segment->attempts++;
                if (segment->attempts < kMaxAmbientUploadAttempts && ambient_effective_.load()) {
                    Application::GetInstance().GetAudioService().RequeueAmbientAudio(std::move(segment));
                } else {
                    ESP_LOGW(TAG, "Ambient speech segment dropped after upload failures");
                }
                operation_failed = true;
            }
        } else if (has_chat) {
            ChatEvent chat;
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                chat = chat_queue_.front();
            }
            if (SendChat(chat)) {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                if (!chat_queue_.empty()) {
                    chat_queue_.pop_front();
                }
            } else {
                operation_failed = true;
            }
        }

        ReleaseBackgroundNetwork();
        const int64_t gap_us = operation_failed
            ? static_cast<int64_t>(kFailedUploadRetryMs) * 1000
            : kNetworkOperationGapUs;
        next_network_operation_us = esp_timer_get_time() + gap_us;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
