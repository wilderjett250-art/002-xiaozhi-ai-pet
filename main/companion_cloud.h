#ifndef COMPANION_CLOUD_H
#define COMPANION_CLOUD_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "camera.h"

class CompanionCloud {
public:
    static CompanionCloud& GetInstance();

    bool IsEnabled() const;
    bool IsOnline() const;
    bool IsAmbientListeningEnabled() const { return ambient_effective_.load(); }
    void RegisterTools();
    void Start();
    void RecordChat(const std::string& role, const std::string& content, const std::string& session_id);
    bool QueueVoicePhoto(CameraPhoto photo, const std::string& prompt, const std::string& analysis);
    bool TryAcquireBackgroundNetwork();
    void ReleaseBackgroundNetwork();

private:
    struct ChatEvent {
        std::string role;
        std::string content;
        std::string session_id;
        int64_t occurred_at_ms = 0;
    };

    struct PhotoEvent {
        CameraPhoto photo;
        std::string source;
        std::string prompt;
        std::string analysis;
        int command_id = 0;
        int64_t captured_at_ms = 0;
        int attempts = 0;
    };

    CompanionCloud();
    static void TaskEntry(void* arg);
    void Run();
    bool PostJson(const std::string& path, std::string body, std::string* response = nullptr, int timeout_ms = 10000);
    bool SendHeartbeat();
    bool SendChat(const ChatEvent& event);
    bool UploadAmbientAudio(const std::vector<uint8_t>& data, int duration_ms);
    int UploadPhoto(const PhotoEvent& event);
    bool PollRemoteCommand(int wait_seconds);
    void HandleRemoteCapture(int command_id);
    void HandleRemoteGreeting(int command_id, const std::string& message);
    void HandleAmbientConfig(int command_id, bool enabled);
    void HandleProactiveSpeak(int command_id, const std::string& text, const std::string& audio_url);
    void HandleRemoteUpgrade(int command_id, const std::string& url,
                             const std::string& version, const std::string& sha256);
    std::string CallGateway(const std::string& action, const std::string& request,
                            const std::string& parameters);
    bool CompleteCommand(int command_id, bool success, int photo_id, const std::string& error);
    std::string ApiUrl(const std::string& path) const;
    static int64_t WallClockMs();

    std::string base_url_;
    std::string device_token_;
    std::string device_id_;
    std::string device_name_;
    std::atomic<bool> started_{false};
    std::atomic<bool> online_{false};
    std::atomic<bool> background_network_busy_{false};
    std::atomic<bool> ambient_requested_{true};
    std::atomic<bool> ambient_effective_{false};
    TaskHandle_t task_handle_ = nullptr;
    std::mutex queue_mutex_;
    std::deque<ChatEvent> chat_queue_;
    std::deque<PhotoEvent> photo_queue_;
};

#endif
