#ifndef DUAL_NETWORK_BOARD_H
#define DUAL_NETWORK_BOARD_H

#include "board.h"
#include "wifi_board.h"
#include "ml307_board.h"
#include <atomic>
#include <memory>
#include <esp_timer.h>

//enum NetworkType
enum class NetworkType {
    WIFI,
    ML307
};

// 双网络板卡类，可以在WiFi和ML307之间切换
class DualNetworkBoard : public Board {
private:
    // 使用基类指针存储当前活动的板卡
    std::unique_ptr<Board> current_board_;
    NetworkType network_type_ = NetworkType::ML307;  // Default to ML307

    // ML307的引脚配置
    gpio_num_t ml307_tx_pin_;
    gpio_num_t ml307_rx_pin_;
    gpio_num_t ml307_dtr_pin_;
    int ml307_baud_rate_;
    bool automatic_failover_capable_ = false;
    bool prefer_default_on_power_on_ = false;
    std::atomic_bool automatic_failover_paused_{false};
    std::atomic_bool network_connected_{false};
    std::atomic_bool failover_pending_{false};
    std::atomic_bool failover_in_progress_{false};
    std::atomic_int cloud_failure_streak_{0};
    std::atomic_bool cloud_failover_triggered_{false};
    std::atomic_bool cloud_failover_suppressed_{false};
    esp_timer_handle_t failover_timer_ = nullptr;
    NetworkEventCallback network_event_callback_;
    
    // 从Settings加载网络类型
    NetworkType LoadNetworkTypeFromSettings(int32_t default_net_type);
    
    // 保存网络类型到Settings
    void SaveNetworkTypeToSettings(NetworkType type);

    // 初始化当前网络类型对应的板卡
    void InitializeCurrentBoard();

    static void OnFailoverTimer(void* arg);
    void HandleNetworkEvent(NetworkEvent event, const std::string& data);
    void StartFailoverTimer(uint64_t delay_us, const char* reason);
    void CancelFailoverTimer();
    void ExecuteAutomaticFailover();
    void ResetAutomaticFailureState();
    void ReturnTo4GAfterCloudFallback(const char* reason);
 
public:
    DualNetworkBoard(gpio_num_t ml307_tx_pin, gpio_num_t ml307_rx_pin,
                     gpio_num_t ml307_dtr_pin = GPIO_NUM_NC,
                     int32_t default_net_type = 1,
                     int ml307_baud_rate = 921600,
                     bool automatic_failover = false,
                     bool prefer_default_on_power_on = false);
    virtual ~DualNetworkBoard();
 
    // 切换网络类型
    void SwitchNetworkType();
    
    // 获取当前网络类型
    NetworkType GetNetworkType() const { return network_type_; }
    bool IsAutomaticFailoverEnabled() const { return automatic_failover_capable_; }
    bool IsNetworkConnected() const { return network_connected_.load(); }
    void ReportInternetAccessResult(bool success);
    
    // 获取当前活动的板卡引用
    Board& GetCurrentBoard() const { return *current_board_; }
    
    // 重写Board接口
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual NetworkInterface* GetNetwork() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
    virtual std::string GetBoardJson() override;
    virtual std::string GetDeviceStatusJson() override;
};

#endif // DUAL_NETWORK_BOARD_H
