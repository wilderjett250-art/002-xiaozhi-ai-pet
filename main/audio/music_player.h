#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class AudioService;
class Display;
class Http;
class PropertyList;
struct cJSON;

class MusicPlayer {
public:
    enum class State {
        Stopped,
        Buffering,
        Playing,
        Paused,
        Completed,
        Error,
    };

    static MusicPlayer& GetInstance();

    void Initialize(AudioService* audio_service, Display* display);
    void RegisterTools();
    void InterruptForConversation();
    bool IsActive() const;
    State GetState() const;
    bool PlayRemotePrompt(const std::string& url, const std::string& title);
    bool PlayShowcaseSample(bool singing);
    void StopShowcaseSample();

private:
    enum class Kind {
        Music,
        Singing,
    };

    struct Request {
        bool valid = false;
        Kind kind = Kind::Music;
        std::string source;
        std::string title;
        std::string url;
        std::string format = "auto";
        std::string prompt;
        std::string voice = "cat";
        bool ai_synthesis = false;
    };

    MusicPlayer() = default;
    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    AudioService* audio_service_ = nullptr;
    Display* display_ = nullptr;
    TaskHandle_t worker_task_ = nullptr;
    mutable std::mutex mutex_;
    Request request_;
    State state_ = State::Stopped;
    std::string play_mode_ = "once";
    std::string last_error_;
    std::atomic<uint32_t> generation_{0};
    std::atomic<int64_t> position_ms_{0};
    std::atomic<int64_t> last_screen_second_{-1};
    std::atomic<bool> paused_{false};
    std::atomic<bool> interrupted_{false};
    bool initialized_ = false;
    bool tools_registered_ = false;

    bool QueueRequest(Request request);
    void Stop();
    void Pause(bool interrupted);
    bool Resume();
    void PlayAdjacent(int direction);
    void WorkerTask();
    bool PlayRequest(const Request& request, uint32_t generation);
    bool PlayBuiltin(const Request& request, uint32_t generation);
    bool PlayUrl(const Request& request, uint32_t generation);
    bool PlayOggOpus(Http* http, const Request& request, uint32_t generation);
    bool ResolveAiSinging(Request& request, uint32_t generation);
    bool WaitWhilePaused(uint32_t generation);
    bool IsCurrent(uint32_t generation) const;
    void SetState(State state, const std::string& error = "");
    void ShowPlaying(const Request& request);
    void UpdatePlaybackScreen(const Request& request, bool force = false);
    void ShowFinished(const Request& request);
    void ShowError(const std::string& error);
    cJSON* StatusJson() const;
    cJSON* CatalogJson(bool singing_only) const;
};

#endif
