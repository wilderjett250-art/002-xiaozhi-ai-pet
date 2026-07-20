#include "dual_network_board.h"
#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "esp32_camera.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "system_telemetry.h"
#include "audio/music_player.h"
#include "companion_cloud.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(LCD_TYPE_ILI9341_SERIAL)
#include "esp_lcd_ili9341.h"
#endif

#if defined(LCD_TYPE_GC9A01_SERIAL)
#include "esp_lcd_gc9a01.h"
static const gc9a01_lcd_init_cmd_t gc9107_lcd_init_cmds[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb1, (uint8_t[]){0x80}, 1, 0},
    {0xb2, (uint8_t[]){0x27}, 1, 0},
    {0xb3, (uint8_t[]){0x13}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x05}, 1, 0},
    {0xac, (uint8_t[]){0xc8}, 1, 0},
    {0xab, (uint8_t[]){0x0f}, 1, 0},
    {0x3a, (uint8_t[]){0x05}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x08}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xea, (uint8_t[]){0x02}, 1, 0},
    {0xe8, (uint8_t[]){0x2A}, 1, 0},
    {0xe9, (uint8_t[]){0x47}, 1, 0},
    {0xe7, (uint8_t[]){0x5f}, 1, 0},
    {0xc6, (uint8_t[]){0x21}, 1, 0},
    {0xc7, (uint8_t[]){0x15}, 1, 0},
    {0xf0,
    (uint8_t[]){0x1D, 0x38, 0x09, 0x4D, 0x92, 0x2F, 0x35, 0x52, 0x1E, 0x0C,
                0x04, 0x12, 0x14, 0x1f},
    14, 0},
    {0xf1,
    (uint8_t[]){0x16, 0x40, 0x1C, 0x54, 0xA9, 0x2D, 0x2E, 0x56, 0x10, 0x0D,
                0x0C, 0x1A, 0x14, 0x1E},
    14, 0},
    {0xf4, (uint8_t[]){0x00, 0x00, 0xFF}, 3, 0},
    {0xba, (uint8_t[]){0xFF, 0xFF}, 2, 0},
};
#endif
 
#define TAG "CompactWifiBoardS3Cam"

class CatPetState {
public:
    explicit CatPetState(Display* display) : display_(display) {
        Settings settings("cat_pet", false);
        energy_.store(ClampPercent(settings.GetInt("energy", 72)));
        affinity_.store(ClampPercent(settings.GetInt("affinity", 35)));
        curiosity_.store(ClampPercent(settings.GetInt("curiosity", 55)));
        {
            std::lock_guard<std::mutex> lock(mood_mutex_);
            mood_ = NormalizeMood(settings.GetString("mood", "relaxed"));
        }

        xTaskCreate([](void* arg) {
            auto state = static_cast<CatPetState*>(arg);
            state->Run();
            vTaskDelete(nullptr);
        }, "cat_pet_state", 4096, this, 1, &task_handle_);
    }

    void RegisterTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.pet.control",
            "Control the local cat pet with one action. Actions: status returns mood and stats; "
            "set_mood updates the expression; care handles praise, comfort or feeding; reset restores defaults. "
            "Valid moods: happy, loving, sleepy, thinking, surprised, relaxed, neutral, sad, confident.",
            PropertyList({
                Property("action", kPropertyTypeString, std::string("status")),
                Property("mood", kPropertyTypeString, std::string("relaxed")),
                Property("note", kPropertyTypeString, std::string("Mood updated")),
                Property("care_action", kPropertyTypeString, std::string("care")),
                Property("energy_delta", kPropertyTypeInteger, 8, -50, 50),
                Property("affinity_delta", kPropertyTypeInteger, 5, -50, 50)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();
                if (action == "status") {
                    return StateJson();
                }
                if (action == "set_mood") {
                    SetMood(properties["mood"].value<std::string>(),
                            properties["note"].value<std::string>(), true);
                    return StateJson();
                }
                if (action == "care") {
                    AdjustStats(properties["energy_delta"].value<int>(),
                                properties["affinity_delta"].value<int>(), -4);
                    SetMood("loving", "Care: " + properties["care_action"].value<std::string>(), true);
                    return StateJson();
                }
                if (action == "reset") {
                    energy_.store(72);
                    affinity_.store(35);
                    curiosity_.store(55);
                    SetMood("relaxed", "Pet state reset", true);
                    Save();
                    return StateJson();
                }
                return std::string("Unsupported pet action: ") + action;
            });
    }

    void OnObservation(bool interesting, const std::string& category) {
        if (interesting) {
            AdjustStats(-2, 1, -12);
            if (category == "person") {
                SetMood("surprised", "Someone is here", false);
            } else if (category == "pet") {
                SetMood("loving", "A pet is nearby", false);
            } else if (category == "text") {
                SetMood("thinking", "Reading desk", false);
            } else {
                SetMood("curious", "Noticed desk change", false);
            }
        } else {
            AdjustStats(-1, 0, 3);
            AutoMood(false);
        }
    }

    void OnReminder(const std::string& type) {
        AdjustStats(-1, 1, 0);
        if (type == "focus_done") {
            SetMood("confident", "Focus complete", false);
        } else {
            SetMood("happy", "Reminder", false);
        }
    }

    cJSON* StateJson() {
        auto root = cJSON_CreateObject();
        std::string mood;
        {
            std::lock_guard<std::mutex> lock(mood_mutex_);
            mood = mood_;
        }
        cJSON_AddStringToObject(root, "mood", mood.c_str());
        cJSON_AddNumberToObject(root, "energy", energy_.load());
        cJSON_AddNumberToObject(root, "affinity", affinity_.load());
        cJSON_AddNumberToObject(root, "curiosity", curiosity_.load());
        cJSON_AddStringToObject(root, "screen_status", StatusForMood(mood).c_str());
        return root;
    }

private:
    Display* display_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;
    std::atomic<int> energy_{72};
    std::atomic<int> affinity_{35};
    std::atomic<int> curiosity_{55};
    std::mutex mood_mutex_;
    std::string mood_ = "relaxed";
    int tick_count_ = 0;

    static int ClampPercent(int value) {
        return std::min(std::max(value, 0), 100);
    }

    static std::string NormalizeMood(const std::string& mood) {
        if (mood == "happy" || mood == "loving" || mood == "sleepy" ||
            mood == "thinking" || mood == "surprised" || mood == "relaxed" ||
            mood == "neutral" || mood == "sad" || mood == "confident" ||
            mood == "winking") {
            return mood;
        }
        if (mood == "curious") {
            return "thinking";
        }
        return "relaxed";
    }

    static std::string StatusForMood(const std::string& mood) {
        if (mood == "sleepy") {
            return "Sleepy";
        }
        if (mood == "thinking") {
            return "Curious";
        }
        if (mood == "loving") {
            return "Warm";
        }
        if (mood == "happy" || mood == "confident") {
            return "Happy";
        }
        if (mood == "surprised") {
            return "Alert";
        }
        if (mood == "sad") {
            return "Quiet";
        }
        return "Cozy";
    }

    static bool CanUseIdleDisplay() {
        auto& app = Application::GetInstance();
        return app.GetDeviceState() == kDeviceStateIdle && app.CanEnterSleepMode();
    }

    void AdjustStats(int energy_delta, int affinity_delta, int curiosity_delta) {
        energy_.store(ClampPercent(energy_.load() + energy_delta));
        affinity_.store(ClampPercent(affinity_.load() + affinity_delta));
        curiosity_.store(ClampPercent(curiosity_.load() + curiosity_delta));
        Save();
    }

    void Save() {
        Settings settings("cat_pet", true);
        settings.SetInt("energy", energy_.load());
        settings.SetInt("affinity", affinity_.load());
        settings.SetInt("curiosity", curiosity_.load());
        std::lock_guard<std::mutex> lock(mood_mutex_);
        settings.SetString("mood", mood_);
    }

    void SetMood(const std::string& mood, const std::string& note, bool notify) {
        std::string normalized = NormalizeMood(mood);
        {
            std::lock_guard<std::mutex> lock(mood_mutex_);
            mood_ = normalized;
        }
        Save();
        ApplyToDisplay(normalized, note, notify);
    }

    void ApplyToDisplay(const std::string& mood, const std::string& note, bool notify) {
        if (display_ == nullptr || !CanUseIdleDisplay()) {
            return;
        }
        display_->SetStatus(StatusForMood(mood).c_str());
        display_->SetEmotion(mood.c_str());
        if (notify && !note.empty()) {
            display_->ShowNotification(note);
        }
    }

    void AutoMood(bool notify) {
        std::string mood = "relaxed";
        int energy = energy_.load();
        int affinity = affinity_.load();
        int curiosity = curiosity_.load();
        if (energy < 25) {
            mood = "sleepy";
        } else if (curiosity > 75) {
            mood = "thinking";
        } else if (affinity > 70) {
            mood = "loving";
        } else if (energy > 85) {
            mood = "happy";
        }
        SetMood(mood, "Pet mood updated", notify);
    }

    void Run() {
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(30000));
            ++tick_count_;
            if (tick_count_ % 2 != 0) {
                continue;
            }
            AdjustStats(-1, 0, 1);
            AutoMood(false);
        }
    }
};

class StartupSelfTest {
public:
    StartupSelfTest(Esp32Camera* camera, Display* display)
        : camera_(camera), display_(display) {
        Start(true);
    }

    bool IsRunning() const {
        return running_.load();
    }

    bool Passed() const {
        return completed_.load() && passed_.load();
    }

    void RegisterTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.self_test",
            "Control the hardware self-test. action=status reads display, camera, PSRAM, microphone, audio and "
            "network results; action=run starts a new test and announces the result locally.",
            PropertyList({
                Property("action", kPropertyTypeString, std::string("status"))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();
                if (action == "status") {
                    return StatusJson();
                }
                if (action != "run") {
                    return std::string("Unsupported self-test action: ") + action;
                }
                bool started = Start(false);
                auto root = StatusJson();
                cJSON_AddBoolToObject(root, "started", started);
                return root;
            });
    }

private:
    static constexpr int kBootIdleWaitSeconds = 150;
    static constexpr int kManualIdleWaitSeconds = 45;
    static constexpr int kMicWaitSeconds = 8;
    static constexpr size_t kMinimumPsramBytes = 4 * 1024 * 1024;

    Esp32Camera* camera_ = nullptr;
    Display* display_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> boot_run_{true};
    std::atomic<bool> completed_{false};
    std::atomic<bool> passed_{false};
    std::atomic<bool> display_ok_{false};
    std::atomic<bool> camera_ok_{false};
    std::atomic<bool> psram_ok_{false};
    std::atomic<bool> microphone_ok_{false};
    std::atomic<bool> audio_ok_{false};
    std::atomic<bool> network_ok_{false};
    std::atomic<int> microphone_peak_{0};
    std::atomic<int64_t> last_run_ms_{0};
    std::atomic<size_t> psram_bytes_{0};
    std::mutex message_mutex_;
    std::string message_ = "Self-test waiting";

    bool Start(bool boot_run) {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return false;
        }
        boot_run_.store(boot_run);
        completed_.store(false);
        {
            std::lock_guard<std::mutex> lock(message_mutex_);
            message_ = "Self-test running";
        }
        BaseType_t result = xTaskCreate([](void* arg) {
            auto self = static_cast<StartupSelfTest*>(arg);
            self->Run(self->boot_run_.load());
            self->running_.store(false);
            self->task_handle_ = nullptr;
            vTaskDelete(nullptr);
        }, "startup_self_test", 4096 * 2, this, 2, &task_handle_);
        if (result != pdPASS) {
            running_.store(false);
            task_handle_ = nullptr;
            std::lock_guard<std::mutex> lock(message_mutex_);
            message_ = "Self-test task could not start";
            return false;
        }
        return true;
    }

    static bool WaitForIdle(int wait_seconds) {
        auto& app = Application::GetInstance();
        for (int i = 0; i < wait_seconds; ++i) {
            DeviceState state = app.GetDeviceState();
            if (state == kDeviceStateIdle) {
                return true;
            }
            if (state == kDeviceStateFatalError) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        return false;
    }

    static void AddFailure(std::vector<std::string>& failures, const char* name, bool ok) {
        if (!ok) {
            failures.emplace_back(name);
        }
    }

    static std::string JoinFailures(const std::vector<std::string>& failures) {
        std::string message = "Self-test issue: ";
        for (size_t i = 0; i < failures.size(); ++i) {
            if (i != 0) {
                message += ", ";
            }
            message += failures[i];
        }
        return message;
    }

    void Run(bool boot_run) {
        auto& app = Application::GetInstance();
        auto& board = Board::GetInstance();
        network_ok_.store(WaitForIdle(boot_run ? kBootIdleWaitSeconds : kManualIdleWaitSeconds));

        if (network_ok_.load()) {
            vTaskDelay(pdMS_TO_TICKS(2500));
        }

        display_ok_.store(display_ != nullptr);
        audio_ok_.store(board.GetAudioCodec() != nullptr);

        size_t psram_bytes = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        psram_bytes_.store(psram_bytes);
        psram_ok_.store(psram_bytes >= kMinimumPsramBytes);

        int microphone_peak = 0;
        for (int i = 0; i < kMicWaitSeconds; ++i) {
            microphone_peak = SystemTelemetry::GetMicPeak();
            if (microphone_peak > 0) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        microphone_peak_.store(microphone_peak);
        microphone_ok_.store(microphone_peak > 0);

        bool camera_ok = false;
        if (camera_ != nullptr && app.GetDeviceState() == kDeviceStateIdle) {
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            camera_ok = camera_->CaptureSilent();
            board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        }
        camera_ok_.store(camera_ok);

        std::vector<std::string> failures;
        AddFailure(failures, "DISPLAY", display_ok_.load());
        AddFailure(failures, "CAMERA", camera_ok_.load());
        AddFailure(failures, "PSRAM", psram_ok_.load());
        AddFailure(failures, "MIC", microphone_ok_.load());
        AddFailure(failures, "AUDIO", audio_ok_.load());
        AddFailure(failures, "NETWORK", network_ok_.load());

        bool passed = failures.empty();
        std::string message = passed ? "这次开机无故障" : JoinFailures(failures);
        passed_.store(passed);
        completed_.store(true);
        last_run_ms_.store(esp_timer_get_time() / 1000);
        {
            std::lock_guard<std::mutex> lock(message_mutex_);
            message_ = message;
        }

        app.Schedule([this, passed, message]() {
            auto& app = Application::GetInstance();
            if (display_ != nullptr) {
                display_->SetStatus(passed ? "自检通过" : "自检异常");
                display_->SetEmotion(passed ? "happy" : "sad");
                display_->SetChatMessage(passed ? "assistant" : "system", message.c_str());
                display_->ShowNotification(message, 7000);
            }
            app.PlaySound(passed ? Lang::Sounds::OGG_BOOT_SELF_TEST_OK : Lang::Sounds::OGG_EXCLAMATION);
        });

        ESP_LOGI(TAG,
            "Self-test complete: passed=%d display=%d camera=%d psram=%d(%u) mic=%d(peak=%d) audio=%d network=%d",
            passed, display_ok_.load(), camera_ok_.load(), psram_ok_.load(),
            static_cast<unsigned>(psram_bytes_.load()), microphone_ok_.load(), microphone_peak_.load(),
            audio_ok_.load(), network_ok_.load());
    }

    cJSON* StatusJson() {
        auto root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "version", "startup_self_test_v1");
        cJSON_AddBoolToObject(root, "running", running_.load());
        cJSON_AddBoolToObject(root, "completed", completed_.load());
        cJSON_AddBoolToObject(root, "passed", passed_.load());
        cJSON_AddBoolToObject(root, "display_ok", display_ok_.load());
        cJSON_AddBoolToObject(root, "camera_ok", camera_ok_.load());
        cJSON_AddBoolToObject(root, "psram_ok", psram_ok_.load());
        cJSON_AddNumberToObject(root, "psram_bytes", static_cast<double>(psram_bytes_.load()));
        cJSON_AddBoolToObject(root, "microphone_ok", microphone_ok_.load());
        cJSON_AddNumberToObject(root, "microphone_peak", microphone_peak_.load());
        cJSON_AddBoolToObject(root, "audio_ok", audio_ok_.load());
        cJSON_AddBoolToObject(root, "network_ok", network_ok_.load());
        cJSON_AddNumberToObject(root, "last_run_ms", last_run_ms_.load());
        std::lock_guard<std::mutex> lock(message_mutex_);
        cJSON_AddStringToObject(root, "message", message_.c_str());
        return root;
    }
};

class AutoObserver {
public:
    AutoObserver(Esp32Camera* camera, Display* display, CatPetState* pet_state,
                 StartupSelfTest* startup_self_test)
        : camera_(camera), display_(display), pet_state_(pet_state),
          startup_self_test_(startup_self_test) {
        Settings settings("auto_observe", true);
        const int profile_version = settings.GetInt("profile_version", 0);
        if (profile_version < kProfileVersion) {
            enabled_.store(true);
            interval_seconds_.store(kDefaultIntervalSeconds);
            cooldown_seconds_.store(kDefaultCooldownSeconds);
            adaptive_enabled_.store(true);
            max_interval_seconds_.store(kDefaultMaxIntervalSeconds);
            settings.SetInt("profile_version", kProfileVersion);
            settings.SetBool("enabled", true);
            settings.SetInt("interval", kDefaultIntervalSeconds);
            settings.SetInt("cooldown", kDefaultCooldownSeconds);
            settings.SetBool("adaptive", true);
            settings.SetInt("max_interval", kDefaultMaxIntervalSeconds);
        } else {
            enabled_.store(settings.GetBool("enabled", true));
            interval_seconds_.store(ClampInterval(settings.GetInt("interval", kDefaultIntervalSeconds)));
            cooldown_seconds_.store(ClampCooldown(settings.GetInt("cooldown", kDefaultCooldownSeconds)));
            adaptive_enabled_.store(settings.GetBool("adaptive", true));
            max_interval_seconds_.store(ClampMaxInterval(
                settings.GetInt("max_interval", kDefaultMaxIntervalSeconds), interval_seconds_.load()));
        }
        effective_interval_seconds_.store(interval_seconds_.load());

        xTaskCreate([](void* arg) {
            auto observer = static_cast<AutoObserver*>(arg);
            observer->Run();
            vTaskDelete(nullptr);
        }, "auto_observer", 4096 * 2, this, 2, &task_handle_);
    }

    void RegisterTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.camera.auto_observe",
            "Control continuous camera observation v4 with one action. Actions: status, configure, observe_now, "
            "history, clear_history. Configure changes enabled, interval, cooldown and adaptive timing. "
            "Unchanged frames stay local; images are displayed only when the user explicitly requests a photo.",
            PropertyList({
                Property("action", kPropertyTypeString, std::string("status")),
                Property("enabled", kPropertyTypeBoolean, true),
                Property("interval_seconds", kPropertyTypeInteger, kDefaultIntervalSeconds, kMinIntervalSeconds, kMaxIntervalSeconds),
                Property("cooldown_seconds", kPropertyTypeInteger, kDefaultCooldownSeconds, kMinCooldownSeconds, kMaxCooldownSeconds),
                Property("adaptive", kPropertyTypeBoolean, true),
                Property("max_interval_seconds", kPropertyTypeInteger, kDefaultMaxIntervalSeconds,
                         kMinMaxIntervalSeconds, kMaxMaxIntervalSeconds),
                Property("limit", kPropertyTypeInteger, kHistoryLimit, 1, kHistoryLimit)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();
                if (action == "status") {
                    return StatusJson();
                }
                if (action == "observe_now") {
                    return ObserveOnce(true);
                }
                if (action == "history") {
                    return HistoryJson(properties["limit"].value<int>());
                }
                if (action == "clear_history") {
                    std::lock_guard<std::mutex> lock(history_mutex_);
                    history_.clear();
                    return true;
                }
                if (action != "configure") {
                    return std::string("Unsupported auto-observe action: ") + action;
                }
                bool enabled = properties["enabled"].value<bool>();
                int interval_seconds = ClampInterval(properties["interval_seconds"].value<int>());
                int cooldown_seconds = ClampCooldown(properties["cooldown_seconds"].value<int>());
                bool adaptive = properties["adaptive"].value<bool>();
                int max_interval_seconds = ClampMaxInterval(
                    properties["max_interval_seconds"].value<int>(), interval_seconds);
                SetEnabled(enabled, interval_seconds, cooldown_seconds, adaptive, max_interval_seconds);
                return StatusJson();
            });
    }

    cJSON* RunShowcaseObservation() {
        return ObserveOnce(true);
    }

private:
    static constexpr int kProfileVersion = 4;
    static constexpr int kDefaultIntervalSeconds = 30;
    static constexpr int kMinIntervalSeconds = 10;
    static constexpr int kMaxIntervalSeconds = 600;
    static constexpr int kDefaultCooldownSeconds = 120;
    static constexpr int kMinCooldownSeconds = 30;
    static constexpr int kMaxCooldownSeconds = 1800;
    static constexpr int kDefaultMaxIntervalSeconds = 300;
    static constexpr int kMinMaxIntervalSeconds = 10;
    static constexpr int kMaxMaxIntervalSeconds = 1800;
    static constexpr int kHistoryLimit = 10;
    static constexpr int kSceneUnchangedBits = 4;
    static constexpr int kCloudAnalysisIntervalSeconds = 120;
    static constexpr size_t kMinInternalHeapForAutoObserve = 64 * 1024;

    struct ObservationEvent {
        int64_t timestamp_ms;
        std::string status;
        std::string category;
        std::string text;
        uint32_t scene_signature;
        bool notified;
    };

    Esp32Camera* camera_ = nullptr;
    Display* display_ = nullptr;
    CatPetState* pet_state_ = nullptr;
    StartupSelfTest* startup_self_test_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;
    std::atomic<bool> enabled_{false};
    std::atomic<int> interval_seconds_{kDefaultIntervalSeconds};
    std::atomic<int> cooldown_seconds_{kDefaultCooldownSeconds};
    std::atomic<bool> adaptive_enabled_{true};
    std::atomic<bool> observing_{false};
    std::atomic<int> max_interval_seconds_{kDefaultMaxIntervalSeconds};
    std::atomic<int> effective_interval_seconds_{kDefaultIntervalSeconds};
    std::atomic<int> stable_streak_{0};
    std::atomic<int64_t> last_observed_us_{0};
    std::atomic<int64_t> last_analyzed_us_{0};
    std::atomic<int64_t> last_notified_us_{0};
    std::atomic<uint32_t> last_scene_signature_{0};
    std::atomic<int> failure_count_{0};
    std::atomic<int> skipped_unchanged_count_{0};
    std::atomic<int> cooldown_skip_count_{0};
    std::atomic<int> notice_count_{0};
    std::atomic<int> quiet_count_{0};
    std::atomic<int> duplicate_skip_count_{0};
    std::atomic<int> analysis_throttle_count_{0};
    std::mutex last_result_mutex_;
    std::string last_result_;
    std::string last_category_ = "none";
    std::string last_notified_text_;
    std::string last_notified_category_ = "none";
    std::mutex history_mutex_;
    std::deque<ObservationEvent> history_;

    static int ClampInterval(int interval_seconds) {
        return std::min(std::max(interval_seconds, kMinIntervalSeconds), kMaxIntervalSeconds);
    }

    static int ClampCooldown(int cooldown_seconds) {
        return std::min(std::max(cooldown_seconds, kMinCooldownSeconds), kMaxCooldownSeconds);
    }

    static int ClampMaxInterval(int max_interval_seconds, int base_interval_seconds) {
        int clamped = std::min(std::max(max_interval_seconds, kMinMaxIntervalSeconds),
                               kMaxMaxIntervalSeconds);
        return std::max(clamped, base_interval_seconds);
    }

    static int CountBits(uint32_t value) {
        int count = 0;
        while (value != 0) {
            count += value & 1u;
            value >>= 1;
        }
        return count;
    }

    void ResetAdaptiveInterval() {
        stable_streak_.store(0);
        effective_interval_seconds_.store(interval_seconds_.load());
    }

    void IncreaseAdaptiveInterval() {
        if (!adaptive_enabled_.load()) {
            ResetAdaptiveInterval();
            return;
        }
        stable_streak_.fetch_add(1);
        int base = interval_seconds_.load();
        int current = std::max(base, effective_interval_seconds_.load());
        int next = std::min(max_interval_seconds_.load(), current * 2);
        effective_interval_seconds_.store(std::max(base, next));
    }

    void RecordEvent(const char* status, const std::string& text, const std::string& category,
                     uint32_t scene_signature, bool notified) {
        ObservationEvent event{
            .timestamp_ms = esp_timer_get_time() / 1000,
            .status = status,
            .category = category,
            .text = LimitUtf8(text, 160),
            .scene_signature = scene_signature,
            .notified = notified,
        };
        std::lock_guard<std::mutex> lock(history_mutex_);
        history_.push_front(std::move(event));
        while (history_.size() > kHistoryLimit) {
            history_.pop_back();
        }
    }

    cJSON* HistoryJson(int limit) {
        auto root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "mode", "continuous_v4");
        auto events = cJSON_AddArrayToObject(root, "events");
        std::lock_guard<std::mutex> lock(history_mutex_);
        int count = 0;
        for (const auto& event : history_) {
            if (count++ >= limit) {
                break;
            }
            auto item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "timestamp_ms", event.timestamp_ms);
            cJSON_AddStringToObject(item, "status", event.status.c_str());
            cJSON_AddStringToObject(item, "category", event.category.c_str());
            cJSON_AddStringToObject(item, "text", event.text.c_str());
            cJSON_AddNumberToObject(item, "scene_signature", event.scene_signature);
            cJSON_AddBoolToObject(item, "notified", event.notified);
            cJSON_AddItemToArray(events, item);
        }
        cJSON_AddNumberToObject(root, "count", std::min(static_cast<int>(history_.size()), limit));
        return root;
    }

    bool IsDuplicateNotice(const std::string& text, const std::string& category) {
        std::lock_guard<std::mutex> lock(last_result_mutex_);
        if (last_notified_text_.empty() || text != last_notified_text_ || category != last_notified_category_) {
            return false;
        }
        int64_t last = last_notified_us_.load();
        int64_t duplicate_window_us = static_cast<int64_t>(
            std::max(cooldown_seconds_.load() * 3, 600)) * 1000000;
        return last != 0 && esp_timer_get_time() - last < duplicate_window_us;
    }

    void RememberNotice(const std::string& text, const std::string& category) {
        std::lock_guard<std::mutex> lock(last_result_mutex_);
        last_notified_text_ = text;
        last_notified_category_ = category;
    }

    static std::string LimitUtf8(const std::string& text, size_t max_bytes) {
        if (text.size() <= max_bytes) {
            return text;
        }
        size_t end = max_bytes;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
            --end;
        }
        return text.substr(0, end);
    }

    static std::string FirstStringField(cJSON* json, const char* name) {
        auto item = cJSON_GetObjectItem(json, name);
        if (cJSON_IsString(item) && item->valuestring != nullptr) {
            return item->valuestring;
        }
        return "";
    }

    static std::string ExtractObservationText(const std::string& result) {
        auto root = cJSON_Parse(result.c_str());
        if (root == nullptr) {
            return LimitUtf8(result, 160);
        }

        std::string text;
        if (cJSON_IsObject(root)) {
            text = FirstStringField(root, "text");
            if (text.empty()) {
                text = FirstStringField(root, "answer");
            }
            if (text.empty()) {
                text = FirstStringField(root, "message");
            }
            if (text.empty()) {
                text = FirstStringField(root, "description");
            }
            if (text.empty()) {
                auto content = cJSON_GetObjectItem(root, "content");
                if (cJSON_IsArray(content)) {
                    auto first = cJSON_GetArrayItem(content, 0);
                    if (cJSON_IsObject(first)) {
                        text = FirstStringField(first, "text");
                    }
                }
            }
        }

        cJSON_Delete(root);
        if (text.empty()) {
            return LimitUtf8(result, 160);
        }
        return LimitUtf8(text, 160);
    }

    static bool IsQuietObservation(const std::string& text) {
        return text.find("[normal]") != std::string::npos ||
               text.find("environment normal") != std::string::npos ||
               text.find("normal") != std::string::npos ||
               text.find("环境正常") != std::string::npos ||
               text.find("没有明显变化") != std::string::npos ||
               text.find("nothing important") != std::string::npos ||
               text.find("no obvious") != std::string::npos;
    }

    static std::string ExtractCategory(const std::string& text) {
        if (text.find("[person]") != std::string::npos || text.find("person") != std::string::npos) {
            return "person";
        }
        if (text.find("[pet]") != std::string::npos || text.find("pet") != std::string::npos ||
            text.find("cat") != std::string::npos || text.find("dog") != std::string::npos ||
            text.find("猫") != std::string::npos || text.find("狗") != std::string::npos) {
            return "pet";
        }
        if (text.find("[text]") != std::string::npos || text.find("text") != std::string::npos) {
            return "text";
        }
        if (text.find("[object]") != std::string::npos || text.find("object") != std::string::npos) {
            return "object";
        }
        if (text.find("[desk]") != std::string::npos || text.find("desk") != std::string::npos) {
            return "desk";
        }
        if (text.find("[normal]") != std::string::npos || text.find("normal") != std::string::npos) {
            return "normal";
        }
        return "unknown";
    }

    bool IsSceneUnchanged(uint32_t scene_signature) const {
        uint32_t last_signature = last_scene_signature_.load();
        if (scene_signature == 0 || last_signature == 0) {
            return false;
        }
        return CountBits(scene_signature ^ last_signature) <= kSceneUnchangedBits;
    }

    bool IsInNotificationCooldown() const {
        int64_t last = last_notified_us_.load();
        if (last == 0) {
            return false;
        }
        int64_t now = esp_timer_get_time();
        return now - last < static_cast<int64_t>(cooldown_seconds_.load()) * 1000000;
    }

    void SetEnabled(bool enabled, int interval_seconds, int cooldown_seconds,
                    bool adaptive, int max_interval_seconds) {
        interval_seconds_.store(interval_seconds);
        cooldown_seconds_.store(cooldown_seconds);
        adaptive_enabled_.store(adaptive);
        max_interval_seconds_.store(ClampMaxInterval(max_interval_seconds, interval_seconds));
        enabled_.store(enabled);
        failure_count_.store(0);
        ResetAdaptiveInterval();

        Settings settings("auto_observe", true);
        settings.SetInt("profile_version", kProfileVersion);
        settings.SetBool("enabled", enabled);
        settings.SetInt("interval", interval_seconds);
        settings.SetInt("cooldown", cooldown_seconds);
        settings.SetBool("adaptive", adaptive);
        settings.SetInt("max_interval", max_interval_seconds_.load());

        if (display_ != nullptr) {
            display_->ShowNotification(enabled ? "持续观察 3.0 已开启" : "持续观察已关闭");
            display_->SetEmotion(enabled ? "winking" : "neutral");
        }
        ESP_LOGI(TAG, "Continuous observe v4 %s, scan=%ds, cloud_min=%ds, cooldown=%ds, adaptive=%d, max_interval=%ds",
            enabled ? "enabled" : "disabled", interval_seconds, kCloudAnalysisIntervalSeconds,
            cooldown_seconds, adaptive, max_interval_seconds_.load());
    }

    cJSON* StatusJson() {
        auto root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "mode", "continuous_v4");
        cJSON_AddBoolToObject(root, "enabled", enabled_.load());
        cJSON_AddBoolToObject(root, "observing", observing_.load());
        cJSON_AddNumberToObject(root, "interval_seconds", interval_seconds_.load());
        cJSON_AddNumberToObject(root, "effective_interval_seconds", effective_interval_seconds_.load());
        cJSON_AddNumberToObject(root, "max_interval_seconds", max_interval_seconds_.load());
        cJSON_AddBoolToObject(root, "adaptive", adaptive_enabled_.load());
        cJSON_AddNumberToObject(root, "stable_streak", stable_streak_.load());
        cJSON_AddNumberToObject(root, "cooldown_seconds", cooldown_seconds_.load());
        cJSON_AddNumberToObject(root, "last_observed_ms", last_observed_us_.load() / 1000);
        cJSON_AddNumberToObject(root, "last_analyzed_ms", last_analyzed_us_.load() / 1000);
        cJSON_AddNumberToObject(root, "cloud_analysis_min_interval_seconds", kCloudAnalysisIntervalSeconds);
        cJSON_AddNumberToObject(root, "last_notified_ms", last_notified_us_.load() / 1000);
        cJSON_AddNumberToObject(root, "last_scene_signature", last_scene_signature_.load());
        cJSON_AddNumberToObject(root, "failure_count", failure_count_.load());
        cJSON_AddNumberToObject(root, "skipped_unchanged_count", skipped_unchanged_count_.load());
        cJSON_AddNumberToObject(root, "cooldown_skip_count", cooldown_skip_count_.load());
        cJSON_AddNumberToObject(root, "notice_count", notice_count_.load());
        cJSON_AddNumberToObject(root, "quiet_count", quiet_count_.load());
        cJSON_AddNumberToObject(root, "duplicate_skip_count", duplicate_skip_count_.load());
        cJSON_AddNumberToObject(root, "analysis_throttle_count", analysis_throttle_count_.load());
        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            cJSON_AddNumberToObject(root, "history_count", history_.size());
        }
        int64_t last = last_observed_us_.load();
        int64_t elapsed_seconds = last == 0 ? 0 : (esp_timer_get_time() - last) / 1000000;
        int next_due_seconds = last == 0 ? 0 :
            std::max(0, effective_interval_seconds_.load() - static_cast<int>(elapsed_seconds));
        cJSON_AddNumberToObject(root, "next_due_seconds", next_due_seconds);
        std::lock_guard<std::mutex> lock(last_result_mutex_);
        cJSON_AddStringToObject(root, "last_result", last_result_.c_str());
        cJSON_AddStringToObject(root, "last_category", last_category_.c_str());
        return root;
    }

    cJSON* ObservationJson(bool ok, const char* status, const std::string& text,
            const std::string& category = "unknown", uint32_t scene_signature = 0) {
        auto root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", ok);
        cJSON_AddStringToObject(root, "status", status);
        cJSON_AddStringToObject(root, "text", text.c_str());
        cJSON_AddStringToObject(root, "category", category.c_str());
        cJSON_AddNumberToObject(root, "scene_signature", scene_signature);
        cJSON_AddNumberToObject(root, "effective_interval_seconds", effective_interval_seconds_.load());
        return root;
    }

    static void RestoreLowPowerIfIdle() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateIdle) {
            Board::GetInstance().SetPowerSaveLevel(
                app.CanEnterSleepMode() ? PowerSaveLevel::LOW_POWER : PowerSaveLevel::BALANCED);
        }
    }

    bool ShouldObserve() const {
        auto& app = Application::GetInstance();
        return enabled_.load() &&
               !observing_.load() &&
               (startup_self_test_ == nullptr || !startup_self_test_->IsRunning()) &&
               app.GetDeviceState() == kDeviceStateIdle;
    }

    void Run() {
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (!enabled_.load()) {
                continue;
            }

            const int interval = effective_interval_seconds_.load();
            const int64_t now = esp_timer_get_time();
            const int64_t last = last_observed_us_.load();
            if (last != 0 && now - last < static_cast<int64_t>(interval) * 1000000) {
                continue;
            }

            if (!ShouldObserve()) {
                continue;
            }

            cJSON_Delete(ObserveOnce(false));
        }
    }

    cJSON* ObserveOnce(bool notify_on_quiet) {
        if (camera_ == nullptr || display_ == nullptr) {
            return ObservationJson(false, "unavailable", "Camera or display is not ready");
        }
        if (startup_self_test_ != nullptr && startup_self_test_->IsRunning()) {
            return ObservationJson(false, "busy", "Startup self-test is running");
        }
        bool expected = false;
        if (!observing_.compare_exchange_strong(expected, true)) {
            return ObservationJson(false, "busy", "Another observation is running");
        }
        struct ObservationGuard {
            std::atomic<bool>& observing;
            ~ObservationGuard() {
                observing.store(false);
            }
        } guard{observing_};

        auto& app = Application::GetInstance();
        auto& board = Board::GetInstance();

        if (!notify_on_quiet) {
            const size_t free_internal =
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (app.GetDeviceState() != kDeviceStateIdle ||
                free_internal < kMinInternalHeapForAutoObserve) {
                last_observed_us_.store(esp_timer_get_time());
                IncreaseAdaptiveInterval();
                ESP_LOGI(TAG, "Auto observe deferred: state=%d, free_internal=%u",
                    static_cast<int>(app.GetDeviceState()), static_cast<unsigned>(free_internal));
                return ObservationJson(true, "deferred", "conversation or memory has priority");
            }
        }

        bool background_network_acquired = false;
        const int acquire_attempts = notify_on_quiet ? 20 : 1;
        for (int attempt = 0; attempt < acquire_attempts; ++attempt) {
            if (CompanionCloud::GetInstance().TryAcquireBackgroundNetwork()) {
                background_network_acquired = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!background_network_acquired) {
            last_observed_us_.store(esp_timer_get_time());
            IncreaseAdaptiveInterval();
            ESP_LOGI(TAG, "Auto observe deferred: background network is busy");
            return ObservationJson(true, "deferred", "background network is busy");
        }
        struct BackgroundNetworkGuard {
            ~BackgroundNetworkGuard() {
                CompanionCloud::GetInstance().ReleaseBackgroundNetwork();
            }
        } background_network_guard;

        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (notify_on_quiet) {
            display_->SetStatus("正在观察");
            display_->SetEmotion("thinking");
        }

        try {
            if (!camera_->CaptureSilent()) {
                throw std::runtime_error("Failed to capture photo");
            }

            uint32_t scene_signature = camera_->GetFrameSignature();
            if (!notify_on_quiet && IsSceneUnchanged(scene_signature)) {
                skipped_unchanged_count_.fetch_add(1);
                last_observed_us_.store(esp_timer_get_time());
                IncreaseAdaptiveInterval();
                {
                    std::lock_guard<std::mutex> lock(last_result_mutex_);
                    last_result_ = "scene unchanged";
                    last_category_ = "normal";
                }
                if (pet_state_ != nullptr) {
                    pet_state_->OnObservation(false, "normal");
                }
                ESP_LOGI(TAG, "Auto observe skipped unchanged scene, signature=0x%08lx",
                    static_cast<unsigned long>(scene_signature));
                RecordEvent("unchanged", "scene unchanged", "normal", scene_signature, false);
                RestoreLowPowerIfIdle();
                return ObservationJson(true, "unchanged", "scene unchanged", "normal", scene_signature);
            }
            const int64_t now = esp_timer_get_time();
            const int64_t last_analyzed = last_analyzed_us_.load();
            if (!notify_on_quiet && last_analyzed != 0 &&
                now - last_analyzed < static_cast<int64_t>(kCloudAnalysisIntervalSeconds) * 1000000) {
                analysis_throttle_count_.fetch_add(1);
                last_observed_us_.store(now);
                {
                    std::lock_guard<std::mutex> lock(last_result_mutex_);
                    last_result_ = "scene change pending analysis";
                    last_category_ = "pending";
                }
                RecordEvent("pending", "scene change pending analysis", "pending", scene_signature, false);
                RestoreLowPowerIfIdle();
                return ObservationJson(true, "pending", "scene change pending analysis", "pending", scene_signature);
            }

            if (!notify_on_quiet && app.GetDeviceState() != kDeviceStateIdle) {
                last_observed_us_.store(now);
                RestoreLowPowerIfIdle();
                ESP_LOGI(TAG, "Auto observe cloud analysis skipped for active conversation");
                return ObservationJson(true, "interrupted", "conversation has priority", "pending", scene_signature);
            }

            if (!notify_on_quiet &&
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) <
                    kMinInternalHeapForAutoObserve) {
                last_observed_us_.store(now);
                IncreaseAdaptiveInterval();
                RestoreLowPowerIfIdle();
                ESP_LOGI(TAG, "Auto observe cloud analysis skipped for low internal memory");
                return ObservationJson(true, "deferred", "memory has priority", "pending", scene_signature);
            }

            std::string question =
                "Observe this desktop camera image and report only notable content. "
                "Answer in concise Chinese with one short sentence and prefix exactly one tag: "
                "[person], [pet], [object], [text], [desk], or [normal]. "
                "Protect privacy: do not infer identity or sensitive attributes. "
                "If nothing important is present, reply exactly: [normal] 环境正常。";
            std::string result = camera_->Explain(question);
            std::string text = ExtractObservationText(result);
            std::string category = ExtractCategory(text);
            last_scene_signature_.store(scene_signature);
            last_analyzed_us_.store(esp_timer_get_time());
            {
                std::lock_guard<std::mutex> lock(last_result_mutex_);
                last_result_ = text;
                last_category_ = category;
            }
            last_observed_us_.store(esp_timer_get_time());
            failure_count_.store(0);

            if (IsQuietObservation(text)) {
                quiet_count_.fetch_add(1);
                if (!notify_on_quiet) {
                    IncreaseAdaptiveInterval();
                }
                ESP_LOGI(TAG, "Auto observe quiet: %s", text.c_str());
                if (pet_state_ != nullptr) {
                    pet_state_->OnObservation(false, category);
                }
                if (notify_on_quiet) {
                    display_->ShowNotification(text);
                }
                RecordEvent("quiet", text, category, scene_signature, notify_on_quiet);
                RestoreLowPowerIfIdle();
                return ObservationJson(true, "quiet", text, category, scene_signature);
            }

            if (!notify_on_quiet && IsInNotificationCooldown()) {
                cooldown_skip_count_.fetch_add(1);
                ResetAdaptiveInterval();
                if (pet_state_ != nullptr) {
                    pet_state_->OnObservation(true, category);
                }
                ESP_LOGI(TAG, "Auto observe suppressed by cooldown: %s", text.c_str());
                RecordEvent("cooldown", text, category, scene_signature, false);
                RestoreLowPowerIfIdle();
                return ObservationJson(true, "cooldown", text, category, scene_signature);
            }

            if (!notify_on_quiet && IsDuplicateNotice(text, category)) {
                duplicate_skip_count_.fetch_add(1);
                ResetAdaptiveInterval();
                if (pet_state_ != nullptr) {
                    pet_state_->OnObservation(true, category);
                }
                ESP_LOGI(TAG, "Auto observe suppressed duplicate result: %s", text.c_str());
                RecordEvent("duplicate", text, category, scene_signature, false);
                RestoreLowPowerIfIdle();
                return ObservationJson(true, "duplicate", text, category, scene_signature);
            }

            notice_count_.fetch_add(1);
            last_notified_us_.store(esp_timer_get_time());
            ResetAdaptiveInterval();
            RememberNotice(text, category);
            if (pet_state_ != nullptr) {
                pet_state_->OnObservation(true, category);
            }
            display_->SetStatus("Observed");
            display_->SetEmotion("surprised");
            display_->SetChatMessage("assistant", text.c_str());
            display_->ShowNotification("Auto observe found something");
            app.PlaySound(Lang::Sounds::OGG_POPUP);
            ESP_LOGI(TAG, "Auto observe result: %s", text.c_str());
            RecordEvent("noticed", text, category, scene_signature, true);
            RestoreLowPowerIfIdle();
            return ObservationJson(true, "noticed", text, category, scene_signature);
        } catch (const std::exception& e) {
            int failures = failure_count_.fetch_add(1) + 1;
            const int64_t failed_at = esp_timer_get_time();
            last_observed_us_.store(failed_at);
            last_analyzed_us_.store(failed_at);
            IncreaseAdaptiveInterval();
            ESP_LOGW(TAG, "Auto observe failed (%d): %s", failures, e.what());
            if (notify_on_quiet && failures <= 1) {
                display_->ShowNotification("Auto observe waiting for vision");
            }
            RecordEvent("error", e.what(), "error", 0, false);
            RestoreLowPowerIfIdle();
            return ObservationJson(false, "error", e.what());
        }
    }
};

class CatReminderManager {
public:
    CatReminderManager(Display* display, CatPetState* pet_state)
        : display_(display), pet_state_(pet_state) {
        Settings settings("pet_reminders", false);
        enabled_.store(settings.GetBool("enabled", false));
        water_minutes_.store(ClampMinutes(settings.GetInt("water_min", kDefaultWaterMinutes)));
        stretch_minutes_.store(ClampMinutes(settings.GetInt("stretch_min", kDefaultStretchMinutes)));
        int64_t now = esp_timer_get_time();
        last_water_us_.store(now);
        last_stretch_us_.store(now);

        xTaskCreate([](void* arg) {
            auto manager = static_cast<CatReminderManager*>(arg);
            manager->Run();
            vTaskDelete(nullptr);
        }, "cat_reminders", 4096, this, 1, &task_handle_);
    }

    void RegisterTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.pet.reminders",
            "Control local reminders with one action. Actions: status, configure, start_focus, trigger. "
            "Configure enables drink-water and stretch reminders; trigger types: water, stretch, focus_done.",
            PropertyList({
                Property("action", kPropertyTypeString, std::string("status")),
                Property("enabled", kPropertyTypeBoolean, false),
                Property("water_minutes", kPropertyTypeInteger, kDefaultWaterMinutes, 1, 180),
                Property("stretch_minutes", kPropertyTypeInteger, kDefaultStretchMinutes, 1, 180),
                Property("minutes", kPropertyTypeInteger, kDefaultFocusMinutes, 1, 180),
                Property("type", kPropertyTypeString, std::string("water"))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();
                if (action == "status") {
                    return StatusJson();
                }
                if (action == "start_focus") {
                    StartFocus(ClampMinutes(properties["minutes"].value<int>()));
                    return StatusJson();
                }
                if (action == "trigger") {
                    Trigger(properties["type"].value<std::string>(), true);
                    return StatusJson();
                }
                if (action != "configure") {
                    return std::string("Unsupported reminder action: ") + action;
                }
                bool enabled = properties["enabled"].value<bool>();
                int water_minutes = ClampMinutes(properties["water_minutes"].value<int>());
                int stretch_minutes = ClampMinutes(properties["stretch_minutes"].value<int>());
                SetEnabled(enabled, water_minutes, stretch_minutes);
                return StatusJson();
            });
    }

    void TriggerForShowcase(const std::string& type) {
        Trigger(type, true);
    }

private:
    static constexpr int kDefaultWaterMinutes = 45;
    static constexpr int kDefaultStretchMinutes = 60;
    static constexpr int kDefaultFocusMinutes = 25;

    Display* display_ = nullptr;
    CatPetState* pet_state_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;
    std::atomic<bool> enabled_{false};
    std::atomic<int> water_minutes_{kDefaultWaterMinutes};
    std::atomic<int> stretch_minutes_{kDefaultStretchMinutes};
    std::atomic<bool> focus_active_{false};
    std::atomic<int> focus_minutes_{kDefaultFocusMinutes};
    std::atomic<int64_t> focus_started_us_{0};
    std::atomic<int64_t> last_water_us_{0};
    std::atomic<int64_t> last_stretch_us_{0};
    std::atomic<int64_t> last_reminder_us_{0};
    std::mutex reminder_mutex_;
    std::string last_reminder_ = "none";

    static int ClampMinutes(int minutes) {
        return std::min(std::max(minutes, 1), 180);
    }

    static bool CanInterruptIdle() {
        auto& app = Application::GetInstance();
        return app.GetDeviceState() == kDeviceStateIdle && app.CanEnterSleepMode();
    }

    static std::string NormalizeType(const std::string& type) {
        if (type == "stretch" || type == "focus_done") {
            return type;
        }
        return "water";
    }

    static std::string MessageForType(const std::string& type) {
        if (type == "stretch") {
            return "Stretch break";
        }
        if (type == "focus_done") {
            return "Focus complete";
        }
        return "Drink water";
    }

    static std::string MoodForType(const std::string& type) {
        if (type == "focus_done") {
            return "confident";
        }
        if (type == "stretch") {
            return "winking";
        }
        return "happy";
    }

    void SetEnabled(bool enabled, int water_minutes, int stretch_minutes) {
        enabled_.store(enabled);
        water_minutes_.store(water_minutes);
        stretch_minutes_.store(stretch_minutes);
        int64_t now = esp_timer_get_time();
        last_water_us_.store(now);
        last_stretch_us_.store(now);

        Settings settings("pet_reminders", true);
        settings.SetBool("enabled", enabled);
        settings.SetInt("water_min", water_minutes);
        settings.SetInt("stretch_min", stretch_minutes);

        if (display_ != nullptr) {
            display_->SetEmotion(enabled ? "happy" : "relaxed");
            display_->ShowNotification(enabled ? "Reminders on" : "Reminders off");
        }
        ESP_LOGI(TAG, "Pet reminders %s, water=%d min, stretch=%d min",
            enabled ? "enabled" : "disabled", water_minutes, stretch_minutes);
    }

    void StartFocus(int minutes) {
        focus_minutes_.store(minutes);
        focus_started_us_.store(esp_timer_get_time());
        focus_active_.store(true);
        if (display_ != nullptr) {
            display_->SetStatus("Focus");
            display_->SetEmotion("confident");
            display_->ShowNotification("Focus started");
        }
        ESP_LOGI(TAG, "Focus timer started: %d min", minutes);
    }

    cJSON* StatusJson() {
        auto root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "enabled", enabled_.load());
        cJSON_AddNumberToObject(root, "water_minutes", water_minutes_.load());
        cJSON_AddNumberToObject(root, "stretch_minutes", stretch_minutes_.load());
        cJSON_AddBoolToObject(root, "focus_active", focus_active_.load());
        cJSON_AddNumberToObject(root, "focus_minutes", focus_minutes_.load());
        cJSON_AddNumberToObject(root, "last_reminder_ms", last_reminder_us_.load() / 1000);
        std::lock_guard<std::mutex> lock(reminder_mutex_);
        cJSON_AddStringToObject(root, "last_reminder", last_reminder_.c_str());
        return root;
    }

    void Trigger(const std::string& raw_type, bool force) {
        std::string type = NormalizeType(raw_type);
        if (!force && !CanInterruptIdle()) {
            return;
        }

        std::string message = MessageForType(type);
        std::string mood = MoodForType(type);
        {
            std::lock_guard<std::mutex> lock(reminder_mutex_);
            last_reminder_ = type;
        }
        last_reminder_us_.store(esp_timer_get_time());

        if (pet_state_ != nullptr) {
            pet_state_->OnReminder(type);
        }
        if (display_ != nullptr) {
            display_->SetStatus("Reminder");
            display_->SetEmotion(mood.c_str());
            display_->SetChatMessage("assistant", message.c_str());
            display_->ShowNotification(message, 5000);
        }
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
        ESP_LOGI(TAG, "Pet reminder: %s", type.c_str());
    }

    void Run() {
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            int64_t now = esp_timer_get_time();

            if (focus_active_.load()) {
                int64_t elapsed = now - focus_started_us_.load();
                if (elapsed >= static_cast<int64_t>(focus_minutes_.load()) * 60 * 1000000) {
                    focus_active_.store(false);
                    Trigger("focus_done", false);
                }
            }

            if (!enabled_.load() || !CanInterruptIdle()) {
                continue;
            }

            if (now - last_water_us_.load() >= static_cast<int64_t>(water_minutes_.load()) * 60 * 1000000) {
                last_water_us_.store(now);
                Trigger("water", false);
                continue;
            }

            if (now - last_stretch_us_.load() >= static_cast<int64_t>(stretch_minutes_.load()) * 60 * 1000000) {
                last_stretch_us_.store(now);
                Trigger("stretch", false);
            }
        }
    }
};

class FeatureShowcase {
public:
    static constexpr int kTotalStages = 7;

    FeatureShowcase(Display* display, StartupSelfTest* self_test,
                    AutoObserver* auto_observer, CatReminderManager* reminder_manager)
        : display_(display), self_test_(self_test), auto_observer_(auto_observer),
          reminder_manager_(reminder_manager) {
        Application::GetInstance().RegisterSttCallback([this](const std::string& text) {
            if (text.find("一键展示功能") != std::string::npos ||
                text.find("展示一下功能") != std::string::npos ||
                text.find("再展示一次功能") != std::string::npos ||
                text.find("全部功能展示") != std::string::npos) {
                Start("voice_phrase");
            }
        });
    }

    void RegisterTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.demo.control",
            "Control the complete local product acceptance showcase. Use action=run when the user says "
            "'一键展示功能', '展示一下功能', '再展示一次功能', or asks for a one-click/full feature demonstration. "
            "Use action=status to read progress. Run only once; the device will demonstrate self-test, cat expressions, network and microphone "
            "status, music and singing, camera observation, reminders, and companion-cloud status automatically.",
            PropertyList({
                Property("action", kPropertyTypeString, std::string("status"))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();
                if (action == "status") {
                    return StatusJson();
                }
                if (action != "run") {
                    return std::string("Unsupported demo action: ") + action;
                }
                bool started = Start("mcp");
                auto root = StatusJson();
                cJSON_AddBoolToObject(root, "started", started);
                return root;
            });
    }

private:
    Display* display_ = nullptr;
    StartupSelfTest* self_test_ = nullptr;
    AutoObserver* auto_observer_ = nullptr;
    CatReminderManager* reminder_manager_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<int> stage_{0};
    std::atomic<int64_t> last_started_ms_{0};
    std::mutex status_mutex_;
    std::string status_ = "ready";
    std::string trigger_ = "none";

    bool Start(const char* trigger) {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            ESP_LOGI(TAG, "Feature showcase already running");
            return false;
        }

        stage_.store(0);
        last_started_ms_.store(esp_timer_get_time() / 1000);
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_ = "waiting";
            trigger_ = trigger;
        }

        BaseType_t result = xTaskCreate([](void* arg) {
            auto self = static_cast<FeatureShowcase*>(arg);
            self->Run();
            self->running_.store(false);
            self->task_handle_ = nullptr;
            vTaskDelete(nullptr);
        }, "feature_showcase", 4096 * 2, this, 2, &task_handle_);
        if (result != pdPASS) {
            running_.store(false);
            task_handle_ = nullptr;
            SetStatus("task_error");
            return false;
        }

        ESP_LOGI(TAG, "Feature showcase requested by %s", trigger);
        return true;
    }

    void SetStatus(const char* status) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_ = status;
    }

    cJSON* StatusJson() {
        auto root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "version", "feature_showcase_v3");
        cJSON_AddBoolToObject(root, "running", running_.load());
        cJSON_AddNumberToObject(root, "stage", stage_.load());
        cJSON_AddNumberToObject(root, "total_stages", kTotalStages);
        cJSON_AddBoolToObject(root, "music_active", MusicPlayer::GetInstance().IsActive());
        cJSON_AddBoolToObject(root, "cloud_enabled", CompanionCloud::GetInstance().IsEnabled());
        cJSON_AddBoolToObject(root, "cloud_online", CompanionCloud::GetInstance().IsOnline());
        cJSON_AddNumberToObject(root, "last_started_ms", last_started_ms_.load());
        std::lock_guard<std::mutex> lock(status_mutex_);
        cJSON_AddStringToObject(root, "status", status_.c_str());
        cJSON_AddStringToObject(root, "trigger", trigger_.c_str());
        return root;
    }

    void WaitForConversation() {
        auto& app = Application::GetInstance();
        for (int i = 0; i < 8; ++i) {
            DeviceState state = app.GetDeviceState();
            if (state == kDeviceStateIdle) {
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    void ShowStage(int stage, const char* status, const char* message, const char* emotion,
                   int duration_ms = 2500) {
        stage_.store(stage);
        SetStatus(status);
        if (display_ != nullptr) {
            display_->SetStatus(status);
            display_->SetEmotion(emotion);
            display_->SetChatMessage("assistant", message);
            display_->ShowNotification(message, duration_ms);
        }
        ESP_LOGI(TAG, "Feature showcase stage %d/%d: %s", stage, kTotalStages, status);
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
    }

    void Run() {
        auto& app = Application::GetInstance();
        WaitForConversation();

        const bool self_test_passed = self_test_ != nullptr && self_test_->Passed();
        ShowStage(1, "演示 1/7", self_test_passed ? "开机自检与语音播报正常" : "正在等待开机自检",
                  self_test_passed ? "happy" : "thinking", 3500);
        if (self_test_passed) {
            app.PlaySound(Lang::Sounds::OGG_BOOT_SELF_TEST_OK);
            vTaskDelay(pdMS_TO_TICKS(2500));
        }

        ShowStage(2, "演示 2/7", "猫咪表情与桌宠状态", "happy", 1800);
        static constexpr const char* kDemoEmotions[] = {
            "happy", "loving", "thinking", "surprised", "sleepy"
        };
        for (const char* emotion : kDemoEmotions) {
            if (display_ != nullptr) {
                display_->SetEmotion(emotion);
            }
            vTaskDelay(pdMS_TO_TICKS(1100));
        }

        auto& board = Board::GetInstance();
        auto dual_board = static_cast<DualNetworkBoard*>(&board);
        const char* network = dual_board->GetNetworkType() == NetworkType::ML307 ? "4G" : "WiFi";
        const char* link_state = dual_board->IsNetworkConnected() ? "已连接" : "检测中";
        char network_message[160];
        snprintf(network_message, sizeof(network_message),
                 "双网自动切换已启用：4G 主用、WiFi 备用；当前 %s %s，顶部显示麦克风分贝",
                 network, link_state);
        ShowStage(3, "演示 3/7", network_message, "confident", 3500);

        auto& music_player = MusicPlayer::GetInstance();
        ShowStage(4, "演示 4/7", "原创音乐与小喵唱歌", "happy", 1600);
        if (music_player.PlayShowcaseSample(false)) {
            vTaskDelay(pdMS_TO_TICKS(4500));
            music_player.StopShowcaseSample();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (display_ != nullptr) {
            display_->SetStatus("演示 4/7");
            display_->SetChatMessage("assistant", "小喵正在唱原创歌曲");
            display_->SetEmotion("loving");
        }
        if (music_player.PlayShowcaseSample(true)) {
            vTaskDelay(pdMS_TO_TICKS(5500));
            music_player.StopShowcaseSample();
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        ShowStage(5, "演示 5/7", "摄像头正在拍照并识别周围", "thinking", 1800);
        if (auto_observer_ != nullptr) {
            cJSON* observation = auto_observer_->RunShowcaseObservation();
            cJSON_Delete(observation);
        }
        vTaskDelay(pdMS_TO_TICKS(2500));

        ShowStage(6, "演示 6/7", "喝水、伸展与专注提醒", "winking", 2200);
        if (reminder_manager_ != nullptr) {
            reminder_manager_->TriggerForShowcase("water");
            vTaskDelay(pdMS_TO_TICKS(2200));
            reminder_manager_->TriggerForShowcase("stretch");
            vTaskDelay(pdMS_TO_TICKS(2200));
            reminder_manager_->TriggerForShowcase("focus_done");
            vTaskDelay(pdMS_TO_TICKS(2200));
        }

        auto& cloud = CompanionCloud::GetInstance();
        const char* cloud_message = !cloud.IsEnabled()
            ? "云端管理等待公网服务器接入"
            : (cloud.IsOnline()
                ? "聊天记录、语音照片与后台拍照已连接"
                : "云端管理已配置，正在连接服务器");
        ShowStage(7, "演示 7/7", cloud_message, cloud.IsOnline() ? "confident" : "thinking", 4000);

        SetStatus("completed");
        if (display_ != nullptr) {
            display_->SetStatus("展示完成");
            display_->SetEmotion("happy");
            display_->SetChatMessage("assistant", "七项功能演示完成");
            display_->ShowNotification("七项功能演示完成", 6000);
        }
        app.PlaySound(Lang::Sounds::OGG_POPUP);
        ESP_LOGI(TAG, "Feature showcase completed");
    }
};

class CatPetLcdDisplay : public SpiLcdDisplay {
public:
    CatPetLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                    int width, int height, int offset_x, int offset_y,
                    bool mirror_x, bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {}

    void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        DisplayLockGuard lock(this);
        EnsureTelemetryLabelLocked();
        ApplyPetChromeLocked();
        UpdateTelemetryLocked(true);
        StartIdleBreathingLocked();
    }

    void SetTheme(Theme* theme) override {
        SpiLcdDisplay::SetTheme(theme);
        DisplayLockGuard lock(this);
        EnsureTelemetryLabelLocked();
        ApplyPetChromeLocked();
        UpdateTelemetryLocked(true);
        StartIdleBreathingLocked();
    }

    void UpdateStatusBar(bool update_all = false) override {
        SpiLcdDisplay::UpdateStatusBar(update_all);
        DisplayLockGuard lock(this);
        EnsureTelemetryLabelLocked();
        UpdateTelemetryLocked(update_all);
    }

    void SetPreviewImage(std::unique_ptr<LvglImage> image) override {
        DisplayLockGuard lock(this);
        if (preview_image_ == nullptr) {
            ESP_LOGE(TAG, "Preview image is not initialized");
            return;
        }

        if (image == nullptr) {
            esp_timer_stop(preview_timer_);
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
            SetPreviewChromeVisibleLocked(false);
            preview_image_cached_.reset();
            if (gif_controller_) {
                gif_controller_->Start();
            }
            return;
        }

        preview_image_cached_ = std::move(image);
        auto img_dsc = preview_image_cached_->image_dsc();
        lv_image_set_src(preview_image_, img_dsc);
        if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
            lv_image_set_scale(preview_image_, CoverPreviewScale(img_dsc));
        }
        lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);

        if (gif_controller_) {
            gif_controller_->Stop();
        }
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        SetPreviewChromeVisibleLocked(true);
        esp_timer_stop(preview_timer_);
        ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
    }

private:
    lv_obj_t* telemetry_bar_ = nullptr;
    lv_obj_t* telemetry_label_ = nullptr;
    std::string telemetry_text_;

    int PetBoxSize() const {
        int size = width_ < height_ ? width_ - 48 : height_ - 54;
        int max_size = (width_ <= 240 && height_ <= 240) ? 160 : 190;
        if (size > max_size) {
            size = max_size;
        }
        if (size < 128) {
            size = width_ < height_ ? width_ - 16 : height_ - 16;
        }
        return size;
    }

    int CoverPreviewScale(const lv_image_dsc_t* img_dsc) const {
        int scale_w = 256 * width_ / img_dsc->header.w;
        int scale_h = 256 * height_ / img_dsc->header.h;
        int scale = scale_w > scale_h ? scale_w : scale_h;
        return scale > 0 ? scale : 1;
    }

    static int PeakToDbfs(int peak) {
        if (peak <= 0) {
            return -90;
        }
        double ratio = static_cast<double>(peak) / 32767.0;
        int db = static_cast<int>(std::round(20.0 * std::log10(std::max(ratio, 0.00003))));
        return std::min(0, std::max(-90, db));
    }

    static const char* NetworkName() {
        auto& board = Board::GetInstance();
        auto dual_board = static_cast<DualNetworkBoard*>(&board);
        return dual_board->GetNetworkType() == NetworkType::ML307 ? "4G" : "WiFi";
    }

    void EnsureTelemetryLabelLocked() {
        if (telemetry_label_ != nullptr) {
            return;
        }
        auto screen = lv_screen_active();
        if (screen == nullptr) {
            return;
        }

        const int bar_width = width_ > 160 ? width_ - 46 : width_ - 28;
        telemetry_bar_ = lv_obj_create(screen);
        lv_obj_set_size(telemetry_bar_, bar_width, 20);
        lv_obj_align(telemetry_bar_, LV_ALIGN_TOP_MID, 0, 26);
        lv_obj_set_style_radius(telemetry_bar_, 10, 0);
        lv_obj_set_style_bg_color(telemetry_bar_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(telemetry_bar_, LV_OPA_80, 0);
        lv_obj_set_style_border_width(telemetry_bar_, 1, 0);
        lv_obj_set_style_border_color(telemetry_bar_, lv_color_hex(0xFFD2DF), 0);
        lv_obj_set_style_pad_all(telemetry_bar_, 0, 0);
        lv_obj_set_scrollbar_mode(telemetry_bar_, LV_SCROLLBAR_MODE_OFF);

        telemetry_label_ = lv_label_create(telemetry_bar_);
        lv_label_set_text(telemetry_label_, "4G MIC --");
        lv_obj_set_width(telemetry_label_, bar_width > 32 ? bar_width - 16 : bar_width);
        lv_label_set_long_mode(telemetry_label_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(telemetry_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(telemetry_label_, lv_color_hex(0x6B4051), 0);
        lv_obj_set_style_text_opa(telemetry_label_, LV_OPA_90, 0);
        lv_obj_center(telemetry_label_);
    }

    void UpdateTelemetryLocked(bool force) {
        if (telemetry_label_ == nullptr) {
            return;
        }
        int peak = SystemTelemetry::GetMicPeak();
        int dbfs = PeakToDbfs(peak);
        char text[24];
        snprintf(text, sizeof(text), "%s MIC %ddB", NetworkName(), dbfs);
        if (force || telemetry_text_ != text) {
            telemetry_text_ = text;
            lv_label_set_text(telemetry_label_, telemetry_text_.c_str());
        }
    }

    void SetPreviewChromeVisibleLocked(bool previewing) {
        if (top_bar_ != nullptr) {
            if (previewing) {
                lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (status_bar_ != nullptr) {
            if (previewing) {
                lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (telemetry_bar_ != nullptr) {
            if (previewing) {
                lv_obj_add_flag(telemetry_bar_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(telemetry_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (bottom_bar_ != nullptr) {
            lv_obj_move_foreground(bottom_bar_);
        }
    }

    void ApplyPetChromeLocked() {
        auto screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFF5E8), 0);
        lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0xFFEAF3), 0);
        lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);

        if (container_ != nullptr) {
            lv_obj_set_style_bg_color(container_, lv_color_hex(0xFFF5E8), 0);
            lv_obj_set_style_bg_grad_color(container_, lv_color_hex(0xFFEAF3), 0);
            lv_obj_set_style_bg_grad_dir(container_, LV_GRAD_DIR_VER, 0);
            lv_obj_set_style_border_width(container_, 0, 0);
        }

        if (emoji_box_ != nullptr) {
            const int box_size = PetBoxSize();
            lv_obj_set_size(emoji_box_, box_size, box_size);
            lv_obj_set_style_radius(emoji_box_, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(emoji_box_, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_70, 0);
            lv_obj_set_style_border_width(emoji_box_, 2, 0);
            lv_obj_set_style_border_color(emoji_box_, lv_color_hex(0xFFD2DF), 0);
            lv_obj_set_style_shadow_width(emoji_box_, 18, 0);
            lv_obj_set_style_shadow_opa(emoji_box_, LV_OPA_20, 0);
            lv_obj_set_style_shadow_color(emoji_box_, lv_color_hex(0xE88EAA), 0);
            lv_obj_set_style_shadow_ofs_y(emoji_box_, 6, 0);
            lv_obj_set_scrollbar_mode(emoji_box_, LV_SCROLLBAR_MODE_OFF);
            lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 10);
        }

        if (emoji_image_ != nullptr) {
            lv_image_set_scale(emoji_image_, 258);
            lv_obj_center(emoji_image_);
        }

        if (emoji_label_ != nullptr) {
            lv_obj_set_style_text_color(emoji_label_, lv_color_hex(0x6B4051), 0);
            lv_obj_center(emoji_label_);
        }

        if (top_bar_ != nullptr) {
            lv_obj_set_style_bg_color(top_bar_, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(top_bar_, LV_OPA_40, 0);
        }

        if (status_label_ != nullptr) {
            lv_obj_set_style_text_color(status_label_, lv_color_hex(0x6B4051), 0);
            lv_obj_set_style_text_opa(status_label_, LV_OPA_90, 0);
        }
        if (notification_label_ != nullptr) {
            lv_obj_set_style_text_color(notification_label_, lv_color_hex(0x6B4051), 0);
            lv_obj_set_style_bg_color(notification_label_, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(notification_label_, LV_OPA_80, 0);
            lv_obj_set_style_radius(notification_label_, 10, 0);
            lv_obj_set_style_pad_hor(notification_label_, 8, 0);
            lv_obj_set_style_pad_ver(notification_label_, 3, 0);
        }
        if (network_label_ != nullptr) {
            lv_obj_set_style_text_color(network_label_, lv_color_hex(0x8A5B6D), 0);
        }
        if (telemetry_bar_ != nullptr) {
            const int bar_width = width_ > 160 ? width_ - 46 : width_ - 28;
            lv_obj_set_size(telemetry_bar_, bar_width, 20);
            lv_obj_align(telemetry_bar_, LV_ALIGN_TOP_MID, 0, 26);
            lv_obj_set_style_radius(telemetry_bar_, 10, 0);
            lv_obj_set_style_bg_color(telemetry_bar_, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(telemetry_bar_, LV_OPA_80, 0);
            lv_obj_set_style_border_width(telemetry_bar_, 1, 0);
            lv_obj_set_style_border_color(telemetry_bar_, lv_color_hex(0xFFD2DF), 0);
            lv_obj_move_foreground(telemetry_bar_);
        }
        if (telemetry_label_ != nullptr) {
            const int bar_width = width_ > 160 ? width_ - 46 : width_ - 28;
            lv_obj_set_width(telemetry_label_, bar_width > 32 ? bar_width - 16 : bar_width);
            lv_obj_set_style_text_color(telemetry_label_, lv_color_hex(0x6B4051), 0);
            lv_obj_set_style_text_opa(telemetry_label_, LV_OPA_90, 0);
            lv_obj_center(telemetry_label_);
        }
        if (mute_label_ != nullptr) {
            lv_obj_set_style_text_color(mute_label_, lv_color_hex(0x8A5B6D), 0);
        }
        if (battery_label_ != nullptr) {
            lv_obj_set_style_text_color(battery_label_, lv_color_hex(0x8A5B6D), 0);
        }

        if (bottom_bar_ != nullptr) {
            lv_obj_set_width(bottom_bar_, LV_HOR_RES - 24);
            lv_obj_set_style_radius(bottom_bar_, 14, 0);
            lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_80, 0);
            lv_obj_set_style_border_width(bottom_bar_, 1, 0);
            lv_obj_set_style_border_color(bottom_bar_, lv_color_hex(0xFFD2DF), 0);
            lv_obj_set_style_shadow_width(bottom_bar_, 10, 0);
            lv_obj_set_style_shadow_opa(bottom_bar_, LV_OPA_10, 0);
            lv_obj_set_style_shadow_color(bottom_bar_, lv_color_hex(0xD97898), 0);
            lv_obj_set_style_shadow_ofs_y(bottom_bar_, 3, 0);
            lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -4);
        }

        if (chat_message_label_ != nullptr) {
            lv_obj_set_width(chat_message_label_, LV_HOR_RES - 48);
            lv_obj_set_style_text_color(chat_message_label_, lv_color_hex(0x5D3D49), 0);
            lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
            lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
        }
    }

    void StartIdleBreathingLocked() {
        if (emoji_image_ == nullptr) {
            return;
        }
        lv_anim_del(emoji_image_, nullptr);

        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, emoji_image_);
        lv_anim_set_values(&anim, 250, 268);
        lv_anim_set_duration(&anim, 1500);
        lv_anim_set_playback_duration(&anim, 1500);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&anim, [](void* obj, int32_t value) {
            lv_image_set_scale(static_cast<lv_obj_t*>(obj), value);
        });
        lv_anim_start(&anim);
    }
};

class CompactWifiBoardS3Cam : public DualNetworkBoard {
private:
 
    Button boot_button_;
    LcdDisplay* display_;
    Esp32Camera* camera_;
    CatPetState* pet_state_;
    StartupSelfTest* startup_self_test_;
    AutoObserver* auto_observer_;
    CatReminderManager* reminder_manager_;
    FeatureShowcase* feature_showcase_;

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
#if defined(LCD_TYPE_ILI9341_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
#elif defined(LCD_TYPE_GC9A01_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
        gc9a01_vendor_config_t gc9107_vendor_config = {
            .init_cmds = gc9107_lcd_init_cmds,
            .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
        };        
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
#endif
        
        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
#ifdef  LCD_TYPE_GC9A01_SERIAL
        panel_config.vendor_config = &gc9107_vendor_config;
#endif
        display_ = new CatPetLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeCamera() {
        camera_config_t config = {};
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = CAMERA_PIN_SIOD;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 0;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
        camera_ = new Esp32Camera(config);
        camera_->SetHMirror(false);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                if (GetNetworkType() == NetworkType::WIFI) {
                    auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                    wifi_board.EnterWifiConfigMode();
                    return;
                }
            }
            app.ToggleChatState();
        });

        boot_button_.OnLongPress([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting ||
                app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                SwitchNetworkType();
            }
        });
    }

    void RegisterNetworkTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.network.control",
            "Control connectivity. action=status reports the active and standby links; action=switch manually "
            "selects the standby link for Wi-Fi provisioning or diagnostics. Automatic failover resumes after "
            "the selected link connects.",
            PropertyList({
                Property("action", kPropertyTypeString, std::string("status"))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();
                if (action == "switch") {
                    SwitchNetworkType();
                    return true;
                }
                if (action != "status") {
                    return std::string("Unsupported network action: ") + action;
                }
                auto root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "type", GetNetworkType() == NetworkType::ML307 ? "4g" : "wifi");
                cJSON_AddStringToObject(root, "primary", "4g");
                cJSON_AddStringToObject(root, "standby", GetNetworkType() == NetworkType::ML307 ? "wifi" : "4g");
                cJSON_AddBoolToObject(root, "automatic_failover", IsAutomaticFailoverEnabled());
                cJSON_AddBoolToObject(root, "connected", IsNetworkConnected());
                cJSON_AddStringToObject(root, "handover", "controlled_reconnect");
                return root;
            });
    }

public:
    CompactWifiBoardS3Cam() :
        DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, ML307_DTR_PIN, 1,
                         ML307_BAUD_RATE, true, true),
        boot_button_(BOOT_BUTTON_GPIO),
        display_(nullptr),
        camera_(nullptr),
        pet_state_(nullptr),
        startup_self_test_(nullptr),
        auto_observer_(nullptr),
        reminder_manager_(nullptr),
        feature_showcase_(nullptr) {
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeCamera();
        pet_state_ = new CatPetState(display_);
        pet_state_->RegisterTools();
        startup_self_test_ = new StartupSelfTest(camera_, display_);
        startup_self_test_->RegisterTools();
        auto_observer_ = new AutoObserver(camera_, display_, pet_state_, startup_self_test_);
        auto_observer_->RegisterTools();
        reminder_manager_ = new CatReminderManager(display_, pet_state_);
        reminder_manager_->RegisterTools();
        feature_showcase_ = new FeatureShowcase(display_, startup_self_test_, auto_observer_, reminder_manager_);
        feature_showcase_->RegisterTools();
        RegisterNetworkTools();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
        
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(CompactWifiBoardS3Cam);
