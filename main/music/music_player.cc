#include "music_player.h"

#include <array>
#include <cstring>
#include <memory>
#include <string>

#include <esp_log.h>

#include "application.h"
#include "board.h"
#include "http.h"
#include "ogg_demuxer.h"

namespace {

const char* TAG = "MusicPlayer";

constexpr int kHttpTimeoutMs = 3000;
constexpr size_t kHttpReadBufferSize = 1024;
constexpr uint32_t kTaskStackSize = 6144;
constexpr UBaseType_t kTaskPriority = 3;
// 开始播放前最多等多久让当前语音播完（避免和 TTS 抢解码队列）。
constexpr int kMaxWaitPlaybackIdleMs = 20000;

// 播放列表：把下面每一条的 url 换成你自己的 OGG(Opus) 直链即可。
// 要求：Ogg 封装 + Opus 编码（.ogg / .opus），不支持 mp3 / m4a / Ogg Vorbis。
std::vector<MusicTrack> DefaultPlaylist() {
    return {
        {"示例曲目一", "https://filesamples.com/samples/audio/opus/sample1.opus"},
        {"示例曲目二", "https://filesamples.com/samples/audio/opus/sample3.opus"},
    };
}

}  // namespace

MusicPlayer::MusicPlayer() { playlist_ = DefaultPlaylist(); }

MusicPlayer& MusicPlayer::GetInstance() {
    static MusicPlayer instance;
    return instance;
}

void MusicPlayer::SetPlaylist(const std::vector<MusicTrack>& tracks) {
    std::lock_guard<std::mutex> lock(mutex_);
    playlist_ = tracks;
}

std::vector<MusicTrack> MusicPlayer::GetPlaylist() {
    std::lock_guard<std::mutex> lock(mutex_);
    return playlist_;
}

bool MusicPlayer::IsStopRequested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_requested_;
}

bool MusicPlayer::IsActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

bool MusicPlayer::IsPaused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return paused_;
}

bool MusicPlayer::IsPlaying() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ && !paused_;
}

std::string MusicPlayer::CurrentTitle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return title_;
}

size_t MusicPlayer::CurrentIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_;
}

void MusicPlayer::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_) {
        paused_ = true;
    }
}

void MusicPlayer::Resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_) {
        paused_ = false;
    }
}

bool MusicPlayer::Stop(int timeout_ms) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
        paused_ = false;
        if (task_ == nullptr) {
            active_ = false;
            return true;
        }
    }
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (task_ == nullptr) {
                active_ = false;
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "player task is still running, cannot restart now");
    return false;
}

bool MusicPlayer::Play(size_t index) {
    Stop(3000);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playlist_.empty()) {
            ESP_LOGW(TAG, "playlist is empty");
            return false;
        }
        if (index >= playlist_.size()) {
            index = 0;
        }
        index_ = index;
        title_ = playlist_[index].title;
        paused_ = false;
        stop_requested_ = false;
        active_ = true;
    }
    BaseType_t ret = xTaskCreate(TaskEntry, "music_player", kTaskStackSize, this, kTaskPriority, &task_);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create music task");
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        task_ = nullptr;
        return false;
    }
    return true;
}

bool MusicPlayer::PlayDefault() { return Play(0); }

bool MusicPlayer::Next() {
    size_t next_index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playlist_.empty()) {
            return false;
        }
        next_index = (index_ + 1) % playlist_.size();
    }
    return Play(next_index);
}

void MusicPlayer::TaskEntry(void* arg) { static_cast<MusicPlayer*>(arg)->Task(); }

void MusicPlayer::Task() {
    AudioService* audio = &Application::GetInstance().GetAudioService();
    while (true) {
        MusicTrack track;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_requested_ || playlist_.empty()) {
                break;
            }
            index_ = index_ < playlist_.size() ? index_ : 0;
            track = playlist_[index_];
            title_ = track.title;
        }
        ESP_LOGI(TAG, "playing: %s (%s)", track.title.c_str(), track.url.c_str());
        bool ok = PlayOne(track, audio);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_requested_) {
                break;
            }
            if (!ok) {
                ESP_LOGW(TAG, "track failed, stop playing");
                break;
            }
            if (index_ + 1 >= playlist_.size() && !loop_) {
                break;
            }
            index_ = (index_ + 1) % playlist_.size();
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        paused_ = false;
        task_ = nullptr;
    }
    ESP_LOGI(TAG, "music task exited");
    vTaskDelete(nullptr);
}

bool MusicPlayer::PlayOne(const MusicTrack& track, AudioService* audio) {
    if (track.url.empty()) {
        return false;
    }
    // 先等当前语音播放结束，避免和 TTS 的音频包混在一起。
    for (int waited = 0; waited < kMaxWaitPlaybackIdleMs; waited += 200) {
        if (IsStopRequested()) {
            return false;
        }
        if (audio->IsPlaybackIdle()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (!http) {
        ESP_LOGE(TAG, "failed to create http client");
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "audio/ogg, application/ogg");
    http->SetHeader("Accept-Encoding", "identity");

    bool success = false;
    auto opened = http->Open("GET", track.url);
    if (opened) {
        auto status = http->GetStatusCode();
        if (status && *status >= 200 && *status < 300) {
            bool packet_error = false;
            auto demuxer = std::make_unique<OggDemuxer>();
            demuxer->OnPacket([this, audio, &packet_error](const uint8_t* data, int sample_rate,
                                                           int frame_duration_ms, size_t size) {
                if (packet_error || IsStopRequested()) {
                    packet_error = true;
                    return;
                }
                auto packet = std::make_unique<AudioStreamPacket>();
                packet->sample_rate = sample_rate;
                packet->frame_duration = frame_duration_ms;
                packet->payload.assign(data, data + size);
                if (!audio->PushPacketToDecodeQueue(std::move(packet), true)) {
                    packet_error = true;
                }
            });

            std::array<char, kHttpReadBufferSize> buffer;
            while (!packet_error && !IsStopRequested()) {
                if (IsPaused()) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
                auto size = http->Read(buffer.data(), buffer.size());
                if (!size) {
                    ESP_LOGE(TAG, "read failed: %s", size.error().ToString().c_str());
                    break;
                }
                if (*size == 0) {
                    success = demuxer->Finish();
                    if (!success) {
                        ESP_LOGW(TAG, "ogg stream ended without a complete opus stream");
                    }
                    break;
                }
                demuxer->Process(reinterpret_cast<const uint8_t*>(buffer.data()), *size);
                if (demuxer->HasError()) {
                    packet_error = true;
                }
            }
        } else if (!status) {
            ESP_LOGE(TAG, "status failed: %s", status.error().ToString().c_str());
        } else {
            ESP_LOGE(TAG, "http status %d", *status);
        }
    } else {
        ESP_LOGE(TAG, "open failed: %s", opened.error().ToString().c_str());
    }
    http->Close();
    http.reset();
    return success;
}
