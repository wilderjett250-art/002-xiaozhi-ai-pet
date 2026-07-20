#include "dual_network_board.h"
#include "application.h"
#include "display.h"
#include "assets/lang_config.h"
#include "settings.h"
#include <ssid_manager.h>
#include <esp_log.h>
#include <esp_system.h>

static const char *TAG = "DualNetworkBoard";

namespace {
constexpr uint64_t kWifiStartupFailoverUs = 18ULL * 1000 * 1000;
constexpr uint64_t kRuntimeFailoverUs = 12ULL * 1000 * 1000;
constexpr uint64_t kImmediateFailoverUs = 2ULL * 1000 * 1000;
constexpr int32_t kWifiFailureBit = 1;
constexpr int32_t kMl307FailureBit = 2;
constexpr int32_t kFailoverStateVersion = 2;
}  // namespace

DualNetworkBoard::DualNetworkBoard(gpio_num_t ml307_tx_pin, gpio_num_t ml307_rx_pin,
                                   gpio_num_t ml307_dtr_pin, int32_t default_net_type,
                                   int ml307_baud_rate, bool automatic_failover,
                                   bool prefer_default_on_power_on)
    : Board(), 
      ml307_tx_pin_(ml307_tx_pin), 
      ml307_rx_pin_(ml307_rx_pin), 
      ml307_dtr_pin_(ml307_dtr_pin),
      ml307_baud_rate_(ml307_baud_rate),
      automatic_failover_capable_(automatic_failover),
      prefer_default_on_power_on_(prefer_default_on_power_on) {
    if (automatic_failover_capable_) {
        Settings settings("network", true);
        if (settings.GetInt("failover_ver", 0) != kFailoverStateVersion) {
            const int32_t old_failed_mask = settings.GetInt("failed_mask", 0);
            const bool stranded_in_wifi = settings.GetInt("type", default_net_type) == 0 &&
                old_failed_mask == (kWifiFailureBit | kMl307FailureBit);
            settings.SetInt("failed_mask", 0);
            settings.SetInt("failover_ver", kFailoverStateVersion);
            if (stranded_in_wifi) {
                settings.SetInt("type", default_net_type);
                settings.SetBool("cloud_cooldown", true);
            }
        }
        cloud_failover_suppressed_.store(settings.GetBool("cloud_cooldown", false));
        settings.SetBool("cloud_cooldown", false);
        const bool manual_once = settings.GetBool("manual_once", false);
        automatic_failover_paused_.store(manual_once);
        settings.SetBool("manual_once", false);
        if (prefer_default_on_power_on_ && !manual_once &&
            esp_reset_reason() == ESP_RST_POWERON) {
            settings.SetInt("type", default_net_type);
            settings.SetInt("failed_mask", 0);
            ESP_LOGI(TAG, "Power-on reset: selecting preferred network %s",
                     default_net_type == 1 ? "4G" : "WiFi");
        }

        esp_timer_create_args_t timer_args = {};
        timer_args.callback = OnFailoverTimer;
        timer_args.arg = this;
        timer_args.dispatch_method = ESP_TIMER_TASK;
        timer_args.name = "net_failover";
        timer_args.skip_unhandled_events = true;
        if (esp_timer_create(&timer_args, &failover_timer_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create automatic failover timer");
            automatic_failover_capable_ = false;
        }
    }
    
    // 从Settings加载网络类型
    network_type_ = LoadNetworkTypeFromSettings(default_net_type);
    
    // 只初始化当前网络类型对应的板卡
    InitializeCurrentBoard();
}

DualNetworkBoard::~DualNetworkBoard() {
    CancelFailoverTimer();
    if (failover_timer_ != nullptr) {
        esp_timer_delete(failover_timer_);
        failover_timer_ = nullptr;
    }
}

NetworkType DualNetworkBoard::LoadNetworkTypeFromSettings(int32_t default_net_type) {
    Settings settings("network", true);
    int network_type = settings.GetInt("type", default_net_type); // 默认使用ML307 (1)
    return network_type == 1 ? NetworkType::ML307 : NetworkType::WIFI;
}

void DualNetworkBoard::SaveNetworkTypeToSettings(NetworkType type) {
    Settings settings("network", true);
    int network_type = (type == NetworkType::ML307) ? 1 : 0;
    settings.SetInt("type", network_type);
}

void DualNetworkBoard::InitializeCurrentBoard() {
    if (network_type_ == NetworkType::ML307) {
        ESP_LOGI(TAG, "Initialize ML307 board");
        current_board_ = std::make_unique<Ml307Board>(ml307_tx_pin_, ml307_rx_pin_, ml307_dtr_pin_, ml307_baud_rate_);
    } else {
        ESP_LOGI(TAG, "Initialize WiFi board");
        current_board_ = std::make_unique<WifiBoard>();
    }
}

void DualNetworkBoard::SwitchNetworkType() {
    auto display = GetDisplay();
    if (automatic_failover_capable_) {
        {
            Settings settings("network", true);
            settings.SetInt("failed_mask", 0);
            settings.SetBool("manual_once", true);
        }
    }
    if (network_type_ == NetworkType::WIFI) {    
        SaveNetworkTypeToSettings(NetworkType::ML307);
        display->ShowNotification(Lang::Strings::SWITCH_TO_4G_NETWORK);
    } else {
        SaveNetworkTypeToSettings(NetworkType::WIFI);
        display->ShowNotification(Lang::Strings::SWITCH_TO_WIFI_NETWORK);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    auto& app = Application::GetInstance();
    app.Reboot();
}

 
std::string DualNetworkBoard::GetBoardType() {
    return current_board_->GetBoardType();
}

void DualNetworkBoard::StartNetwork() {
    auto display = Board::GetInstance().GetDisplay();
    
    if (network_type_ == NetworkType::WIFI) {
        display->SetStatus(Lang::Strings::CONNECTING);
    } else {
        display->SetStatus(Lang::Strings::DETECTING_MODULE);
    }
    current_board_->StartNetwork();

    if (automatic_failover_capable_ && network_type_ == NetworkType::WIFI &&
        !automatic_failover_paused_.load() && !failover_pending_.load()) {
        StartFailoverTimer(kWifiStartupFailoverUs, "WiFi startup timeout");
    }
}

void DualNetworkBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
    current_board_->SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        HandleNetworkEvent(event, data);
    });
}

void DualNetworkBoard::HandleNetworkEvent(NetworkEvent event, const std::string& data) {
    if (automatic_failover_capable_) {
        switch (event) {
            case NetworkEvent::Connected:
                network_connected_.store(true);
                cloud_failure_streak_.store(0);
                cloud_failover_triggered_.store(false);
                automatic_failover_paused_.store(false);
                CancelFailoverTimer();
                ResetAutomaticFailureState();
                ESP_LOGI(TAG, "Automatic failover armed on %s",
                         network_type_ == NetworkType::ML307 ? "4G" : "WiFi");
                break;
            case NetworkEvent::Disconnected:
                if (network_connected_.exchange(false)) {
                    StartFailoverTimer(kRuntimeFailoverUs, "active network disconnected");
                }
                break;
            case NetworkEvent::WifiConfigModeEnter:
                network_connected_.store(false);
                if (network_type_ == NetworkType::WIFI) {
                    StartFailoverTimer(kImmediateFailoverUs, "WiFi unavailable");
                }
                break;
            case NetworkEvent::ModemErrorNoSim:
            case NetworkEvent::ModemErrorRegDenied:
            case NetworkEvent::ModemErrorInitFailed:
                network_connected_.store(false);
                if (network_type_ == NetworkType::ML307) {
                    StartFailoverTimer(kImmediateFailoverUs, "4G unavailable");
                }
                break;
            case NetworkEvent::ModemErrorTimeout:
                network_connected_.store(false);
                if (network_type_ == NetworkType::ML307) {
                    StartFailoverTimer(kRuntimeFailoverUs, "4G registration timeout");
                }
                break;
            default:
                break;
        }
    }

    if (network_event_callback_) {
        network_event_callback_(event, data);
    }

    if (!automatic_failover_capable_ && network_type_ == NetworkType::ML307 &&
        event == NetworkEvent::ModemErrorInitFailed) {
        ESP_LOGW(TAG, "4G modem unavailable, saving WiFi fallback and rebooting");
        SaveNetworkTypeToSettings(NetworkType::WIFI);
        if (auto display = GetDisplay(); display != nullptr) {
            display->ShowNotification(Lang::Strings::SWITCH_TO_WIFI_NETWORK);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        Application::GetInstance().Reboot();
    }
}

void DualNetworkBoard::ReportInternetAccessResult(bool success) {
    if (!automatic_failover_capable_) {
        return;
    }

    if (success) {
        cloud_failure_streak_.store(0);
        if (network_type_ == NetworkType::WIFI) {
            Settings settings("network", true);
            settings.SetBool("cloud_probe", false);
        }
        if (cloud_failover_triggered_.exchange(false)) {
            network_connected_.store(true);
            CancelFailoverTimer();
            ESP_LOGI(TAG, "Cloud HTTPS recovered before network failover");
        }
        return;
    }

    if (cloud_failover_suppressed_.load()) {
        return;
    }

    const int failures = cloud_failure_streak_.fetch_add(1) + 1;
    ESP_LOGW(TAG, "Cloud HTTPS failure streak: %d/3", failures);
    if (failures < 3 ||
        automatic_failover_paused_.load() || failover_in_progress_.load() ||
        failover_pending_.load() || cloud_failover_triggered_.load()) {
        return;
    }

    if (network_type_ == NetworkType::WIFI) {
        Settings settings("network", true);
        if (settings.GetBool("cloud_probe", false)) {
            ReturnTo4GAfterCloudFallback("WiFi cloud HTTPS unavailable");
        }
        return;
    }

    if (SsidManager::GetInstance().GetSsidList().empty()) {
        cloud_failure_streak_.store(0);
        ESP_LOGW(TAG, "Cloud failover skipped because no WiFi credentials are saved");
        return;
    }

    cloud_failover_triggered_.store(true);
    {
        Settings settings("network", true);
        settings.SetBool("cloud_probe", true);
    }
    network_connected_.store(false);
    StartFailoverTimer(kImmediateFailoverUs, "4G cloud HTTPS unavailable");
}

void DualNetworkBoard::ReturnTo4GAfterCloudFallback(const char* reason) {
    Settings settings("network", true);
    settings.SetBool("cloud_probe", false);
    settings.SetBool("cloud_cooldown", true);
    settings.SetInt("failed_mask", 0);
    SaveNetworkTypeToSettings(NetworkType::ML307);
    cloud_failover_suppressed_.store(true);
    failover_pending_.store(false);
    failover_in_progress_.store(false);
    ESP_LOGW(TAG, "WiFi fallback failed, restoring 4G: %s", reason);
    if (auto display = GetDisplay(); display != nullptr) {
        display->ShowNotification(Lang::Strings::SWITCH_TO_4G_NETWORK);
    }
    vTaskDelay(pdMS_TO_TICKS(1200));
    Application::GetInstance().Reboot();
}

void DualNetworkBoard::OnFailoverTimer(void* arg) {
    auto* board = static_cast<DualNetworkBoard*>(arg);
    board->failover_pending_.store(false);
    Application::GetInstance().Schedule([board]() {
        board->ExecuteAutomaticFailover();
    });
}

void DualNetworkBoard::StartFailoverTimer(uint64_t delay_us, const char* reason) {
    if (!automatic_failover_capable_ || failover_timer_ == nullptr ||
        automatic_failover_paused_.load() || network_connected_.load() ||
        failover_in_progress_.load()) {
        return;
    }

    if (esp_timer_is_active(failover_timer_)) {
        esp_timer_stop(failover_timer_);
    }
    failover_pending_.store(true);
    const uint32_t delay_ms = static_cast<uint32_t>(delay_us / 1000);
    ESP_LOGW(TAG, "Automatic failover scheduled in %lu ms: %s",
             static_cast<unsigned long>(delay_ms), reason);
    if (esp_timer_start_once(failover_timer_, delay_us) != ESP_OK) {
        failover_pending_.store(false);
        ESP_LOGE(TAG, "Failed to start automatic failover timer");
    }
}

void DualNetworkBoard::CancelFailoverTimer() {
    failover_pending_.store(false);
    if (failover_timer_ != nullptr && esp_timer_is_active(failover_timer_)) {
        esp_timer_stop(failover_timer_);
    }
}

void DualNetworkBoard::ResetAutomaticFailureState() {
    Settings settings("network", true);
    settings.SetInt("failed_mask", 0);
}

void DualNetworkBoard::ExecuteAutomaticFailover() {
    bool expected = false;
    if (!failover_in_progress_.compare_exchange_strong(expected, true)) {
        return;
    }
    if (network_connected_.load() || automatic_failover_paused_.load()) {
        failover_in_progress_.store(false);
        return;
    }

    if (network_type_ == NetworkType::WIFI) {
        Settings settings("network", true);
        if (settings.GetBool("cloud_probe", false)) {
            ReturnTo4GAfterCloudFallback("saved WiFi is unavailable");
            return;
        }
    }

    int32_t failed_mask = 0;
    {
        Settings settings("network", true);
        failed_mask = settings.GetInt("failed_mask", 0);
        failed_mask |= network_type_ == NetworkType::WIFI ? kWifiFailureBit : kMl307FailureBit;
        settings.SetInt("failed_mask", failed_mask);
    }

    if (failed_mask == (kWifiFailureBit | kMl307FailureBit) &&
        network_type_ == NetworkType::WIFI) {
        automatic_failover_paused_.store(true);
        ESP_LOGE(TAG, "Both WiFi and 4G failed; staying in WiFi configuration mode");
        if (auto display = GetDisplay(); display != nullptr) {
            display->ShowNotification("WiFi / 4G unavailable");
        }
        failover_in_progress_.store(false);
        return;
    }

    const NetworkType target = network_type_ == NetworkType::WIFI
        ? NetworkType::ML307
        : NetworkType::WIFI;
    SaveNetworkTypeToSettings(target);
    ESP_LOGW(TAG, "Automatic failover: %s -> %s",
             network_type_ == NetworkType::WIFI ? "WiFi" : "4G",
             target == NetworkType::WIFI ? "WiFi" : "4G");
    if (auto display = GetDisplay(); display != nullptr) {
        display->ShowNotification(target == NetworkType::WIFI
            ? Lang::Strings::SWITCH_TO_WIFI_NETWORK
            : Lang::Strings::SWITCH_TO_4G_NETWORK);
    }
    vTaskDelay(pdMS_TO_TICKS(1200));
    Application::GetInstance().Reboot();
}

NetworkInterface* DualNetworkBoard::GetNetwork() {
    return current_board_->GetNetwork();
}

const char* DualNetworkBoard::GetNetworkStateIcon() {
    return current_board_->GetNetworkStateIcon();
}

void DualNetworkBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    current_board_->SetPowerSaveLevel(level);
}

std::string DualNetworkBoard::GetBoardJson() {   
    return current_board_->GetBoardJson();
}

std::string DualNetworkBoard::GetDeviceStatusJson() {
    return current_board_->GetDeviceStatusJson();
}
