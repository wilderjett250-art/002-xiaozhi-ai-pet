#include "music_player.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iterator>
#include <memory>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "application.h"
#include "audio_service.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"
#include "ogg_demuxer.h"
#include "settings.h"

#include "esp_ae_rate_cvt.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"

namespace {

constexpr char TAG[] = "MusicPlayer";
constexpr float kPi = 3.14159265358979323846f;
constexpr int kPcmChunkMs = 40;

struct Note {
    float frequency;
    int duration_ms;
};

struct BuiltinTrack {
    const char* id;
    const char* title;
    const char* description;
    const char* lyric;
    bool singing;
    const Note* notes;
    size_t note_count;
    int repeats;
};

constexpr Note kCatBreezeNotes[] = {
    {523.25f, 260}, {659.25f, 260}, {783.99f, 360}, {659.25f, 260},
    {587.33f, 260}, {698.46f, 260}, {880.00f, 360}, {0.0f, 160},
    {783.99f, 260}, {698.46f, 260}, {659.25f, 360}, {523.25f, 260},
    {587.33f, 260}, {659.25f, 260}, {523.25f, 520}, {0.0f, 180},
};

constexpr Note kStarryDeskNotes[] = {
    {392.00f, 420}, {493.88f, 420}, {587.33f, 620}, {493.88f, 320},
    {440.00f, 420}, {523.25f, 420}, {659.25f, 620}, {0.0f, 220},
    {587.33f, 420}, {523.25f, 420}, {493.88f, 620}, {392.00f, 720},
};

constexpr Note kHappyStepsNotes[] = {
    {659.25f, 170}, {783.99f, 170}, {987.77f, 250}, {783.99f, 170},
    {698.46f, 170}, {880.00f, 170}, {1046.50f, 250}, {0.0f, 90},
    {987.77f, 170}, {880.00f, 170}, {783.99f, 250}, {659.25f, 170},
    {698.46f, 170}, {783.99f, 170}, {659.25f, 340}, {0.0f, 100},
};

constexpr Note kXiaoMiaoSongNotes[] = {
    {440.00f, 320}, {554.37f, 320}, {659.25f, 500}, {0.0f, 120},
    {659.25f, 280}, {739.99f, 280}, {659.25f, 520}, {0.0f, 120},
    {554.37f, 320}, {659.25f, 320}, {880.00f, 520}, {739.99f, 320},
    {659.25f, 320}, {554.37f, 320}, {440.00f, 620}, {0.0f, 180},
};

constexpr Note kMorningMiaoNotes[] = {
    {523.25f, 300}, {659.25f, 300}, {783.99f, 420}, {0.0f, 100},
    {587.33f, 300}, {698.46f, 300}, {880.00f, 420}, {0.0f, 100},
    {783.99f, 300}, {987.77f, 300}, {880.00f, 420}, {783.99f, 300},
    {698.46f, 300}, {659.25f, 300}, {523.25f, 620}, {0.0f, 180},
};

constexpr BuiltinTrack kTracks[] = {
    {"cat_breeze", "猫猫微风", "轻快舒缓的桌面背景音乐", "", false,
        kCatBreezeNotes, std::size(kCatBreezeNotes), 4},
    {"starry_desk", "星光书桌", "安静专注的桌面背景音乐", "", false,
        kStarryDeskNotes, std::size(kStarryDeskNotes), 3},
    {"happy_steps", "开心猫步", "活泼明亮的短节奏", "", false,
        kHappyStepsNotes, std::size(kHappyStepsNotes), 5},
    {"xiao_miao_song", "小喵之歌", "桌宠原创猫咪唱歌", "小喵小喵，陪你看世界", true,
        kXiaoMiaoSongNotes, std::size(kXiaoMiaoSongNotes), 3},
    {"morning_miao", "早安喵", "桌宠原创早安小歌", "早安呀，今天也要开心喵", true,
        kMorningMiaoNotes, std::size(kMorningMiaoNotes), 3},
};

const BuiltinTrack* FindTrack(const std::string& id, bool singing) {
    for (const auto& track : kTracks) {
        if (track.singing == singing && (id == track.id || id == track.title)) {
            return &track;
        }
    }
    return nullptr;
}

std::vector<const BuiltinTrack*> TracksOfKind(bool singing) {
    std::vector<const BuiltinTrack*> result;
    for (const auto& track : kTracks) {
        if (track.singing == singing) {
            result.push_back(&track);
        }
    }
    return result;
}

const char* StateName(MusicPlayer::State state) {
    switch (state) {
        case MusicPlayer::State::Stopped: return "stopped";
        case MusicPlayer::State::Buffering: return "buffering";
        case MusicPlayer::State::Playing: return "playing";
        case MusicPlayer::State::Paused: return "paused";
        case MusicPlayer::State::Completed: return "completed";
        case MusicPlayer::State::Error: return "error";
    }
    return "unknown";
}

esp_audio_simple_dec_type_t DecoderType(const std::string& format, const std::string& url,
                                         const std::string& content_type) {
    std::string value = format + " " + url + " " + content_type;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (value.find("flac") != std::string::npos) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    }
    if (value.find("aac") != std::string::npos) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    }
    if (value.find("wav") != std::string::npos || value.find("wave") != std::string::npos) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

bool IsOggOpus(const std::string& format, const std::string& url, const std::string& content_type) {
    std::string value = format + " " + url + " " + content_type;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value.find("opus") != std::string::npos || value.find("audio/ogg") != std::string::npos ||
           value.find(".ogg") != std::string::npos || value.find(".oga") != std::string::npos;
}

std::vector<int16_t> ToMono16(const uint8_t* data, size_t size,
                              const esp_audio_simple_dec_info_t& info) {
    const int channels = std::max(1, static_cast<int>(info.channel));
    const int bytes_per_sample = info.bits_per_sample / 8;
    if (bytes_per_sample < 2 || bytes_per_sample > 4) {
        return {};
    }
    const size_t frame_bytes = static_cast<size_t>(bytes_per_sample * channels);
    const size_t frames = size / frame_bytes;
    std::vector<int16_t> mono(frames);
    for (size_t frame = 0; frame < frames; ++frame) {
        int64_t sum = 0;
        for (int channel = 0; channel < channels; ++channel) {
            const uint8_t* sample = data + frame * frame_bytes + channel * bytes_per_sample;
            int32_t value = 0;
            if (bytes_per_sample == 2) {
                value = static_cast<int16_t>(sample[0] | (sample[1] << 8));
            } else if (bytes_per_sample == 3) {
                value = sample[0] | (sample[1] << 8) | (sample[2] << 16);
                if ((value & 0x800000) != 0) {
                    value |= static_cast<int32_t>(0xff000000);
                }
                value >>= 8;
            } else {
                uint32_t packed = static_cast<uint32_t>(sample[0]) |
                    (static_cast<uint32_t>(sample[1]) << 8) |
                    (static_cast<uint32_t>(sample[2]) << 16) |
                    (static_cast<uint32_t>(sample[3]) << 24);
                value = static_cast<int32_t>(packed);
                value >>= 16;
            }
            sum += value;
        }
        mono[frame] = static_cast<int16_t>(std::clamp<int64_t>(sum / channels, -32768, 32767));
    }
    return mono;
}

}  // namespace

MusicPlayer& MusicPlayer::GetInstance() {
    static MusicPlayer instance;
    return instance;
}

void MusicPlayer::Initialize(AudioService* audio_service, Display* display) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return;
    }
    audio_service_ = audio_service;
    display_ = display;
    auto ret = esp_audio_simple_dec_register_default();
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "Audio decoder registration returned %d", ret);
    }
    BaseType_t task_result = xTaskCreate([](void* arg) {
        static_cast<MusicPlayer*>(arg)->WorkerTask();
    }, "music_player", 12288, this, 3, &worker_task_);
    initialized_ = task_result == pdPASS;
    if (!initialized_) {
        last_error_ = "music worker task creation failed";
        ESP_LOGE(TAG, "%s", last_error_.c_str());
    }
}

void MusicPlayer::RegisterTools() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tools_registered_) {
        return;
    }
    tools_registered_ = true;
    auto& server = McpServer::GetInstance();

    server.AddTool("self.media.control",
        "Control all music and singing features with one action. Use play_music for 播放音乐/放首歌, sing for 唱歌, "
        "ai_sing for server-generated singing, pause/resume/stop/next/previous for playback control, "
        "repeat/once for play mode, status for current state, list_music/list_songs for the free original catalog, "
        "and configure_ai_singing to save the HTTPS AI singing endpoint and bearer token. "
        "Direct HTTPS MP3, Ogg Opus, AAC, FLAC and WAV URLs are supported. Built-in defaults are cat_breeze and xiao_miao_song.",
        PropertyList({
            Property("action", kPropertyTypeString, std::string("play_music")),
            Property("source", kPropertyTypeString, std::string("")),
            Property("url", kPropertyTypeString, std::string("")),
            Property("format", kPropertyTypeString, std::string("auto")),
            Property("title", kPropertyTypeString, std::string("")),
            Property("prompt", kPropertyTypeString, std::string("")),
            Property("voice", kPropertyTypeString, std::string("cat")),
            Property("endpoint", kPropertyTypeString, std::string("")),
            Property("token", kPropertyTypeString, std::string("")),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            const std::string action = properties["action"].value<std::string>();
            if (action == "pause") {
                Pause(false);
                return StatusJson();
            }
            if (action == "resume") {
                Resume();
                return StatusJson();
            }
            if (action == "stop") {
                Stop();
                return StatusJson();
            }
            if (action == "next") {
                PlayAdjacent(1);
                return StatusJson();
            }
            if (action == "previous") {
                PlayAdjacent(-1);
                return StatusJson();
            }
            if (action == "repeat" || action == "once") {
                {
                    std::lock_guard<std::mutex> state_lock(mutex_);
                    play_mode_ = action;
                }
                return StatusJson();
            }
            if (action == "status") {
                return StatusJson();
            }
            if (action == "list_music") {
                return CatalogJson(false);
            }
            if (action == "list_songs") {
                return CatalogJson(true);
            }
            if (action == "configure_ai_singing") {
                Settings settings("ai_singing", true);
                settings.SetString("endpoint", properties["endpoint"].value<std::string>());
                settings.SetString("token", properties["token"].value<std::string>());
                auto result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "configured",
                    !properties["endpoint"].value<std::string>().empty());
                cJSON_AddStringToObject(result, "request_schema",
                    "POST {prompt,voice,format}; response {audio_url,title,format}");
                return result;
            }

            Request request;
            request.kind = (action == "sing" || action == "ai_sing") ? Kind::Singing : Kind::Music;
            request.source = properties["source"].value<std::string>();
            request.url = properties["url"].value<std::string>();
            request.format = properties["format"].value<std::string>();
            request.title = properties["title"].value<std::string>();
            request.prompt = properties["prompt"].value<std::string>();
            request.voice = properties["voice"].value<std::string>();
            request.ai_synthesis = action == "ai_sing";
            if (request.source.empty()) {
                request.source = request.kind == Kind::Singing ? "xiao_miao_song" : "cat_breeze";
            }
            QueueRequest(std::move(request));
            return StatusJson();
        });
}

bool MusicPlayer::QueueRequest(Request request) {
    if (audio_service_ != nullptr && Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        audio_service_->EnableAmbientCapture(false);
        audio_service_->EnableVoiceProcessing(false);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || audio_service_ == nullptr) {
        last_error_ = "music player is not initialized";
        state_ = State::Error;
        return false;
    }

    const BuiltinTrack* track = nullptr;
    if (request.ai_synthesis) {
        request.source = "ai_singing";
        if (request.prompt.empty()) {
            request.prompt = "唱一首轻快可爱的原创猫咪短歌";
        }
        if (request.title.empty()) {
            request.title = "AI 原创歌曲";
        }
    } else if (request.url.empty()) {
        track = FindTrack(request.source, request.kind == Kind::Singing);
        if (track == nullptr) {
            request.source = request.kind == Kind::Singing ? "xiao_miao_song" : "cat_breeze";
            track = FindTrack(request.source, request.kind == Kind::Singing);
        }
        if (request.title.empty() && track != nullptr) {
            request.title = track->title;
        }
    } else if (request.title.empty()) {
        request.title = request.kind == Kind::Singing ? "网络歌曲" : "网络音乐";
    }

    request.valid = true;
    request_ = std::move(request);
    state_ = State::Buffering;
    last_error_.clear();
    position_ms_.store(0);
    last_screen_second_.store(-1);
    paused_.store(false);
    interrupted_.store(false);
    generation_.fetch_add(1);
    audio_service_->ClearPlaybackQueue();
    xTaskNotifyGive(worker_task_);
    return true;
}

void MusicPlayer::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        request_.valid = false;
        state_ = State::Stopped;
        last_error_.clear();
        position_ms_.store(0);
        paused_.store(false);
        interrupted_.store(false);
        generation_.fetch_add(1);
    }
    if (audio_service_ != nullptr) {
        audio_service_->ClearPlaybackQueue();
    }
    if (worker_task_ != nullptr) {
        xTaskNotifyGive(worker_task_);
    }
    if (display_ != nullptr && Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        display_->SetStatus("待命");
        display_->SetEmotion("neutral");
        display_->SetChatMessage("assistant", "音乐已停止");
    }
    Application::GetInstance().RefreshIdleAudioMode();
}

void MusicPlayer::Pause(bool interrupted) {
    Request request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::Playing && state_ != State::Buffering) {
            return;
        }
        paused_.store(true);
        interrupted_.store(interrupted);
        state_ = State::Paused;
        request = request_;
    }
    if (audio_service_ != nullptr) {
        audio_service_->ClearPlaybackQueue();
    }
    if (!interrupted) {
        UpdatePlaybackScreen(request, true);
    }
}

bool MusicPlayer::Resume() {
    Request request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::Paused || !request_.valid) {
            return false;
        }
        paused_.store(false);
        interrupted_.store(false);
        state_ = State::Playing;
        request = request_;
    }
    UpdatePlaybackScreen(request, true);
    return true;
}

void MusicPlayer::InterruptForConversation() {
    Pause(true);
}

bool MusicPlayer::IsActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::Buffering || state_ == State::Playing || state_ == State::Paused;
}

MusicPlayer::State MusicPlayer::GetState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool MusicPlayer::PlayRemotePrompt(const std::string& url, const std::string& title) {
    if (url.rfind("https://", 0) != 0 || url.size() > 1024) {
        return false;
    }
    Request request;
    request.kind = Kind::Music;
    request.source = "proactive_prompt";
    request.title = title.empty() ? "小喵主动问候" : title;
    request.url = url;
    request.format = "wav";
    return QueueRequest(std::move(request));
}

bool MusicPlayer::PlayShowcaseSample(bool singing) {
    Request request;
    request.kind = singing ? Kind::Singing : Kind::Music;
    request.source = singing ? "xiao_miao_song" : "cat_breeze";
    request.title = singing ? "小喵之歌" : "猫猫微风";
    return QueueRequest(std::move(request));
}

void MusicPlayer::StopShowcaseSample() {
    Stop();
}

void MusicPlayer::PlayAdjacent(int direction) {
    auto tracks = TracksOfKind(false);
    if (tracks.empty()) {
        return;
    }
    std::string current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current = request_.source;
    }
    int index = 0;
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (current == tracks[i]->id || current == tracks[i]->title) {
            index = static_cast<int>(i);
            break;
        }
    }
    index = (index + direction + static_cast<int>(tracks.size())) % static_cast<int>(tracks.size());
    Request request;
    request.kind = Kind::Music;
    request.source = tracks[index]->id;
    request.title = tracks[index]->title;
    QueueRequest(std::move(request));
}

void MusicPlayer::WorkerTask() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (true) {
            Request request;
            uint32_t generation = 0;
            std::string mode;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!request_.valid) {
                    break;
                }
                request = request_;
                generation = generation_.load();
                mode = play_mode_;
            }

            bool ok = PlayRequest(request, generation);
            if (!IsCurrent(generation)) {
                break;
            }
            if (!ok) {
                break;
            }
            if (mode != "repeat") {
                SetState(State::Completed);
                ShowFinished(request);
                break;
            }
            position_ms_.store(0);
        }
        Application::GetInstance().RefreshIdleAudioMode();
    }
}

bool MusicPlayer::PlayRequest(const Request& request, uint32_t generation) {
    Request resolved = request;
    if (resolved.ai_synthesis) {
        SetState(State::Buffering);
        ShowPlaying(resolved);
        if (!ResolveAiSinging(resolved, generation)) {
            if (IsCurrent(generation)) {
                std::string error;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    error = last_error_;
                }
                ShowError(error);
            }
            return false;
        }
    }
    SetState(resolved.url.empty() ? State::Playing : State::Buffering);
    ShowPlaying(resolved);
    bool ok = resolved.url.empty() ? PlayBuiltin(resolved, generation) : PlayUrl(resolved, generation);
    if (!ok && IsCurrent(generation)) {
        std::string error;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            error = last_error_.empty() ? "playback failed" : last_error_;
        }
        ShowError(error);
    }
    return ok;
}

bool MusicPlayer::WaitWhilePaused(uint32_t generation) {
    while (paused_.load()) {
        if (!IsCurrent(generation)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return IsCurrent(generation);
}

bool MusicPlayer::PlayBuiltin(const Request& request, uint32_t generation) {
    const BuiltinTrack* track = FindTrack(request.source, request.kind == Kind::Singing);
    if (track == nullptr) {
        SetState(State::Error, "built-in track not found");
        return false;
    }
    const int sample_rate = audio_service_->GetOutputSampleRate();
    if (sample_rate <= 0) {
        SetState(State::Error, "invalid output sample rate");
        return false;
    }

    double phase = 0.0;
    int64_t played_samples = 0;
    const int chunk_samples = sample_rate * kPcmChunkMs / 1000;
    for (int repeat = 0; repeat < track->repeats; ++repeat) {
        for (size_t note_index = 0; note_index < track->note_count; ++note_index) {
            const Note& note = track->notes[note_index];
            const int total_samples = sample_rate * note.duration_ms / 1000;
            for (int offset = 0; offset < total_samples; offset += chunk_samples) {
                if (!WaitWhilePaused(generation)) {
                    return false;
                }
                const int count = std::min(chunk_samples, total_samples - offset);
                std::vector<int16_t> pcm(count);
                for (int i = 0; i < count; ++i) {
                    const float progress = static_cast<float>(offset + i) / std::max(1, total_samples);
                    const float attack = std::min(1.0f, progress / 0.08f);
                    const float release = std::min(1.0f, (1.0f - progress) / 0.18f);
                    const float envelope = std::max(0.0f, std::min(attack, release));
                    float sample = 0.0f;
                    if (note.frequency > 0.0f) {
                        float frequency = note.frequency;
                        if (track->singing) {
                            const float glide = 1.16f - 0.28f * progress;
                            const float vibrato = 1.0f + 0.018f * std::sin(2.0f * kPi * 5.5f * progress);
                            frequency *= glide * vibrato;
                        }
                        phase += 2.0 * kPi * frequency / sample_rate;
                        if (phase > 2.0 * kPi) {
                            phase -= 2.0 * kPi;
                        }
                        if (track->singing) {
                            sample = std::sin(phase) + 0.42f * std::sin(2.0 * phase) +
                                     0.18f * std::sin(3.0 * phase) + 0.08f * std::sin(5.0 * phase);
                            sample *= 0.58f;
                        } else {
                            sample = std::sin(phase) + 0.28f * std::sin(2.0 * phase) +
                                     0.10f * std::sin(3.0 * phase);
                            sample *= 0.54f;
                        }
                    }
                    pcm[i] = static_cast<int16_t>(std::clamp(sample * envelope * 7200.0f, -12000.0f, 12000.0f));
                }
                if (!audio_service_->PushPcmToPlaybackQueue(std::move(pcm), true)) {
                    SetState(State::Error, "audio output queue stopped");
                    return false;
                }
                played_samples += count;
                position_ms_.store(played_samples * 1000 / sample_rate);
                UpdatePlaybackScreen(request);
            }
        }
    }
    audio_service_->WaitForPlaybackQueueEmpty();
    return IsCurrent(generation);
}

bool MusicPlayer::PlayUrl(const Request& request, uint32_t generation) {
    if (request.url.rfind("https://", 0) != 0) {
        SetState(State::Error, "music URL must use HTTPS");
        return false;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        SetState(State::Error, "network is unavailable");
        return false;
    }
    auto http = network->CreateHttp(4);
    if (http == nullptr) {
        SetState(State::Error, "cannot create music connection");
        return false;
    }
    http->SetTimeout(8000);
    http->SetHeader("Accept", "audio/mpeg,audio/ogg,audio/opus,audio/aac,audio/flac,audio/wav,*/*");
    http->SetHeader("User-Agent", "XiaoMiao-Pet/2.3");
    if (!http->Open("GET", request.url)) {
        SetState(State::Error, "cannot open music URL");
        return false;
    }
    struct HttpGuard {
        Http* http;
        ~HttpGuard() { http->Close(); }
    } http_guard{http.get()};

    const int status = http->GetStatusCode();
    if (status != 200) {
        SetState(State::Error, "music URL returned HTTP " + std::to_string(status));
        return false;
    }

    const std::string content_type = http->GetResponseHeader("Content-Type");
    if (IsOggOpus(request.format, request.url, content_type)) {
        return PlayOggOpus(http.get(), request, generation);
    }

    auto decoder_type = DecoderType(request.format, request.url, content_type);
    esp_audio_simple_dec_cfg_t decoder_config = {
        .dec_type = decoder_type,
        .dec_cfg = nullptr,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t decoder = nullptr;
    auto ret = esp_audio_simple_dec_open(&decoder_config, &decoder);
    if (ret != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        SetState(State::Error, "unsupported or unavailable audio decoder");
        return false;
    }
    struct DecoderGuard {
        esp_audio_simple_dec_handle_t decoder;
        ~DecoderGuard() { esp_audio_simple_dec_close(decoder); }
    } decoder_guard{decoder};

    std::vector<uint8_t> input(2048);
    std::vector<uint8_t> output(8192);
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    int resampler_source_rate = 0;
    int64_t played_samples = 0;
    const int output_rate = audio_service_->GetOutputSampleRate();
    SetState(State::Playing);

    while (IsCurrent(generation)) {
        if (!WaitWhilePaused(generation)) {
            break;
        }
        int read = http->Read(reinterpret_cast<char*>(input.data()), input.size());
        if (read < 0) {
            if (resampler != nullptr) {
                esp_ae_rate_cvt_close(resampler);
            }
            SetState(State::Error, "music network read timed out");
            return false;
        }
        if (read == 0) {
            break;
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = input.data(),
            .len = static_cast<uint32_t>(read),
            .eos = false,
            .consumed = 0,
            .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
        };
        int guard = 0;
        while (raw.len > 0 && guard++ < 64 && IsCurrent(generation)) {
            esp_audio_simple_dec_out_t decoded = {
                .buffer = output.data(),
                .len = static_cast<uint32_t>(output.size()),
                .needed_size = 0,
                .decoded_size = 0,
            };
            ret = esp_audio_simple_dec_process(decoder, &raw, &decoded);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                output.resize(decoded.needed_size);
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                if (resampler != nullptr) {
                    esp_ae_rate_cvt_close(resampler);
                }
                SetState(State::Error, "audio decode failed: " + std::to_string(ret));
                return false;
            }
            if (decoded.decoded_size > 0) {
                esp_audio_simple_dec_info_t info = {};
                if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK) {
                    if (resampler != nullptr) {
                        esp_ae_rate_cvt_close(resampler);
                    }
                    SetState(State::Error, "audio stream information is unavailable");
                    return false;
                }
                auto mono = ToMono16(decoded.buffer, decoded.decoded_size, info);
                if (mono.empty()) {
                    if (resampler != nullptr) {
                        esp_ae_rate_cvt_close(resampler);
                    }
                    SetState(State::Error, "audio bit depth is unsupported");
                    return false;
                }
                if (static_cast<int>(info.sample_rate) != output_rate) {
                    if (resampler == nullptr || resampler_source_rate != static_cast<int>(info.sample_rate)) {
                        if (resampler != nullptr) {
                            esp_ae_rate_cvt_close(resampler);
                        }
                        esp_ae_rate_cvt_cfg_t cfg = {
                            .src_rate = info.sample_rate,
                            .dest_rate = static_cast<uint32_t>(output_rate),
                            .channel = 1,
                            .bits_per_sample = 16,
                            .complexity = 2,
                            .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
                        };
                        if (esp_ae_rate_cvt_open(&cfg, &resampler) != ESP_AE_ERR_OK || resampler == nullptr) {
                            SetState(State::Error, "audio resampler creation failed");
                            return false;
                        }
                        resampler_source_rate = info.sample_rate;
                    }
                    uint32_t target_samples = 0;
                    esp_ae_rate_cvt_get_max_out_sample_num(resampler, mono.size(), &target_samples);
                    std::vector<int16_t> converted(target_samples);
                    uint32_t actual_samples = target_samples;
                    if (esp_ae_rate_cvt_process(resampler, mono.data(), mono.size(),
                                                converted.data(), &actual_samples) != ESP_AE_ERR_OK) {
                        esp_ae_rate_cvt_close(resampler);
                        SetState(State::Error, "audio resampling failed");
                        return false;
                    }
                    converted.resize(actual_samples);
                    mono = std::move(converted);
                }
                played_samples += mono.size();
                position_ms_.store(played_samples * 1000 / output_rate);
                UpdatePlaybackScreen(request);
                if (!audio_service_->PushPcmToPlaybackQueue(std::move(mono), true)) {
                    if (resampler != nullptr) {
                        esp_ae_rate_cvt_close(resampler);
                    }
                    SetState(State::Error, "audio output queue stopped");
                    return false;
                }
            }
            if (raw.consumed == 0) {
                break;
            }
            raw.buffer += raw.consumed;
            raw.len -= std::min(raw.len, raw.consumed);
        }
    }

    if (resampler != nullptr) {
        esp_ae_rate_cvt_close(resampler);
    }
    if (!IsCurrent(generation)) {
        return false;
    }
    audio_service_->WaitForPlaybackQueueEmpty();
    return IsCurrent(generation);
}

bool MusicPlayer::PlayOggOpus(Http* http, const Request& request, uint32_t generation) {
    OggDemuxer demuxer;
    std::atomic<bool> stream_ok{true};
    demuxer.OnDemuxerFinished([this, &request, generation, &stream_ok](
            const uint8_t* data, int sample_rate, size_t size) {
        if (!WaitWhilePaused(generation)) {
            stream_ok.store(false);
            return;
        }
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate > 0 ? sample_rate : 48000;
        packet->frame_duration = 60;
        packet->timestamp = 0;
        packet->payload.assign(data, data + size);
        if (!audio_service_->PushPacketToDecodeQueue(std::move(packet), true)) {
            stream_ok.store(false);
            return;
        }
        position_ms_.fetch_add(20);
        UpdatePlaybackScreen(request);
    });

    std::vector<uint8_t> input(2048);
    SetState(State::Playing);
    while (IsCurrent(generation) && stream_ok.load()) {
        if (!WaitWhilePaused(generation)) {
            return false;
        }
        const int read = http->Read(reinterpret_cast<char*>(input.data()), input.size());
        if (read < 0) {
            SetState(State::Error, "Opus network read timed out");
            return false;
        }
        if (read == 0) {
            break;
        }
        const size_t processed = demuxer.Process(input.data(), static_cast<size_t>(read));
        if (processed != static_cast<size_t>(read)) {
            SetState(State::Error, "invalid Ogg Opus stream");
            return false;
        }
    }
    if (!stream_ok.load() || !IsCurrent(generation)) {
        return false;
    }
    audio_service_->WaitForPlaybackQueueEmpty();
    return IsCurrent(generation);
}

bool MusicPlayer::ResolveAiSinging(Request& request, uint32_t generation) {
    Settings settings("ai_singing", false);
    const std::string endpoint = settings.GetString("endpoint", "");
    const std::string token = settings.GetString("token", "");
    if (endpoint.empty()) {
        SetState(State::Error, "AI singing endpoint is not configured");
        return false;
    }
    if (endpoint.rfind("https://", 0) != 0) {
        SetState(State::Error, "AI singing endpoint must use HTTPS");
        return false;
    }
    if (!IsCurrent(generation)) {
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    auto http = network == nullptr ? nullptr : network->CreateHttp(5);
    if (http == nullptr) {
        SetState(State::Error, "cannot create AI singing connection");
        return false;
    }
    http->SetTimeout(30000);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Accept", "application/json");
    if (!token.empty()) {
        http->SetHeader("Authorization", "Bearer " + token);
    }

    auto payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "prompt", request.prompt.c_str());
    cJSON_AddStringToObject(payload, "voice", request.voice.c_str());
    cJSON_AddStringToObject(payload, "format", "opus");
    char* payload_text = cJSON_PrintUnformatted(payload);
    std::string body = payload_text == nullptr ? "{}" : payload_text;
    cJSON_free(payload_text);
    cJSON_Delete(payload);
    http->SetContent(std::move(body));
    if (!http->Open("POST", endpoint)) {
        SetState(State::Error, "cannot open AI singing endpoint");
        return false;
    }
    struct HttpGuard {
        Http* http;
        ~HttpGuard() { http->Close(); }
    } guard{http.get()};

    if (http->GetStatusCode() != 200) {
        SetState(State::Error, "AI singing endpoint returned HTTP " +
            std::to_string(http->GetStatusCode()));
        return false;
    }
    const std::string response = http->ReadAll();
    auto root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        SetState(State::Error, "AI singing response is not JSON");
        return false;
    }
    cJSON* data = cJSON_GetObjectItem(root, "data");
    cJSON* container = cJSON_IsObject(data) ? data : root;
    cJSON* audio_url = cJSON_GetObjectItem(container, "audio_url");
    if (!cJSON_IsString(audio_url)) {
        audio_url = cJSON_GetObjectItem(container, "url");
    }
    cJSON* title = cJSON_GetObjectItem(container, "title");
    cJSON* format = cJSON_GetObjectItem(container, "format");
    if (!cJSON_IsString(audio_url) || audio_url->valuestring == nullptr) {
        cJSON_Delete(root);
        SetState(State::Error, "AI singing response has no audio_url");
        return false;
    }
    request.url = audio_url->valuestring;
    request.format = cJSON_IsString(format) ? format->valuestring : "opus";
    if (cJSON_IsString(title) && title->valuestring != nullptr) {
        request.title = title->valuestring;
    }
    cJSON_Delete(root);
    return IsCurrent(generation);
}

bool MusicPlayer::IsCurrent(uint32_t generation) const {
    return generation_.load() == generation;
}

void MusicPlayer::SetState(State state, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
    if (!error.empty()) {
        last_error_ = error;
    }
}

void MusicPlayer::ShowPlaying(const Request& request) {
    if (display_ == nullptr) {
        return;
    }
    UpdatePlaybackScreen(request, true);
    const BuiltinTrack* track = request.url.empty() ? FindTrack(request.source, request.kind == Kind::Singing) : nullptr;
    if (track != nullptr && track->singing && std::strlen(track->lyric) > 0) {
        display_->SetChatMessage("assistant", track->lyric);
    } else {
        display_->SetChatMessage("assistant", request.title.c_str());
    }
}

void MusicPlayer::UpdatePlaybackScreen(const Request& request, bool force) {
    if (display_ == nullptr || Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        return;
    }
    const int64_t seconds = position_ms_.load() / 1000;
    if (!force && last_screen_second_.exchange(seconds) == seconds) {
        return;
    }
    if (force) {
        last_screen_second_.store(seconds);
    }

    State state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
    }
    const char* status = "正在播放";
    if (state == State::Buffering) {
        status = request.ai_synthesis ? "AI 正在创作" : "正在缓冲";
    } else if (state == State::Paused) {
        status = "音乐暂停";
    } else if (request.kind == Kind::Singing) {
        status = "小喵唱歌中";
    }

    char line[128];
    snprintf(line, sizeof(line), "%s  %02lld:%02lld", request.title.c_str(),
        static_cast<long long>(seconds / 60), static_cast<long long>(seconds % 60));
    display_->SetStatus(status);
    display_->SetChatMessage("assistant", line);
    if (request.kind == Kind::Singing) {
        display_->SetEmotion((seconds % 2) == 0 ? "loving" : "happy");
    } else {
        display_->SetEmotion(state == State::Paused ? "neutral" : "happy");
    }
}

void MusicPlayer::ShowFinished(const Request& request) {
    if (display_ == nullptr || Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        return;
    }
    display_->SetStatus("播放完成");
    display_->SetEmotion("happy");
    display_->SetChatMessage("assistant", (request.title + " 播放完成").c_str());
}

void MusicPlayer::ShowError(const std::string& error) {
    if (display_ == nullptr) {
        return;
    }
    display_->SetStatus("播放失败");
    display_->SetEmotion("sad");
    display_->ShowNotification("音频播放失败，请检查链接或网络", 5000);
    ESP_LOGE(TAG, "%s", error.c_str());
}

cJSON* MusicPlayer::StatusJson() const {
    auto root = cJSON_CreateObject();
    Settings ai_settings("ai_singing", false);
    const bool ai_configured = !ai_settings.GetString("endpoint", "").empty();
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON_AddStringToObject(root, "state", StateName(state_));
    cJSON_AddStringToObject(root, "kind", request_.kind == Kind::Singing ? "singing" : "music");
    cJSON_AddStringToObject(root, "source", request_.source.c_str());
    cJSON_AddStringToObject(root, "title", request_.title.c_str());
    cJSON_AddStringToObject(root, "format", request_.format.c_str());
    cJSON_AddStringToObject(root, "play_mode", play_mode_.c_str());
    cJSON_AddNumberToObject(root, "position_ms", position_ms_.load());
    cJSON_AddNumberToObject(root, "output_sample_rate", audio_service_ == nullptr ? 0 : audio_service_->GetOutputSampleRate());
    cJSON_AddBoolToObject(root, "network_stream", !request_.url.empty());
    cJSON_AddBoolToObject(root, "ai_singing_configured", ai_configured);
    cJSON_AddStringToObject(root, "supported_network_formats", "mp3,ogg-opus,aac,flac,wav");
    cJSON_AddBoolToObject(root, "interrupted_by_wake_word", interrupted_.load());
    cJSON_AddStringToObject(root, "last_error", last_error_.c_str());
    return root;
}

cJSON* MusicPlayer::CatalogJson(bool singing_only) const {
    auto root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "license", "original_free_builtin_audio");
    cJSON_AddStringToObject(root, "network_playback", "direct_https_url");
    cJSON_AddStringToObject(root, "network_formats", "mp3,ogg-opus,aac,flac,wav");
    auto items = cJSON_AddArrayToObject(root, singing_only ? "songs" : "tracks");
    for (const auto& track : kTracks) {
        if (track.singing != singing_only) {
            continue;
        }
        auto item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", track.id);
        cJSON_AddStringToObject(item, "title", track.title);
        cJSON_AddStringToObject(item, "description", track.description);
        cJSON_AddItemToArray(items, item);
    }
    return root;
}
