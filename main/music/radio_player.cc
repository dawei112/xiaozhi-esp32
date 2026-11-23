#include "radio_player.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include <esp_log.h>

#include "application.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "board.h"
#include "http.h"

namespace {

const char* TAG = "RadioPlayer";

constexpr int kHttpTimeoutMs = 15000;
constexpr size_t kReadBufferSize = 4096;
constexpr size_t kPrebufferBytes = 8192;
// minimp3 的 mp3dec_t 和单帧 PCM 都在栈上，8K 会爆栈
constexpr uint32_t kTaskStackSize = 12288;
constexpr UBaseType_t kTaskPriority = 3;
constexpr int kMaxWaitPlaybackIdleMs = 20000;
constexpr int kRetryDelayMs = 2000;
constexpr int kMaxRetry = 2;

// 实测可用的蜻蜓FM 直播流（64k MP3，返回 audio/mpeg）。
std::vector<RadioStation> BuildStations() {
    return {
        {"襄阳文化教育广播", "https://lhttp.qtfm.cn/live/5057/64k.mp3"},
        {"咸宁综合广播", "https://lhttp.qtfm.cn/live/5067/64k.mp3"},
        {"咸宁交通广播", "https://lhttp.qtfm.cn/live/5068/64k.mp3"},
        {"公安人民广播电台", "https://lhttp.qtfm.cn/live/5063/64k.mp3"},
        {"南京音乐广播", "https://lhttp.qtfm.cn/live/4963/64k.mp3"},
        {"997金陵之声", "https://lhttp.qtfm.cn/live/5056/64k.mp3"},
        {"江苏新闻综合广播", "https://lhttp.qtfm.cn/live/5055/64k.mp3"},
        {"扬州新闻广播", "https://lhttp.qtfm.cn/live/5000/64k.mp3"},
        {"宿迁交通广播", "https://lhttp.qtfm.cn/live/5004/64k.mp3"},
        {"宿豫人民广播电台", "https://lhttp.qtfm.cn/live/5005/64k.mp3"},
        {"绍兴综合广播93.6", "https://lhttp.qtfm.cn/live/5052/64k.mp3"},
        {"绍兴交通广播94.1", "https://lhttp.qtfm.cn/live/5053/64k.mp3"},
        {"绍兴音乐广播103.5", "https://lhttp.qtfm.cn/live/5054/64k.mp3"},
        {"绍兴故事广播", "https://lhttp.qtfm.cn/live/4956/64k.mp3"},
        {"杭州芒果音乐台89.3", "https://lhttp.qtfm.cn/live/4979/64k.mp3"},
        {"福州新闻广播", "https://lhttp.qtfm.cn/live/5025/64k.mp3"},
        {"福州交通之声", "https://lhttp.qtfm.cn/live/5026/64k.mp3"},
        {"惠州综合广播FM100", "https://lhttp.qtfm.cn/live/5016/64k.mp3"},
        {"开平广播电台", "https://lhttp.qtfm.cn/live/5037/64k.mp3"},
        {"新会人民广播电台", "https://lhttp.qtfm.cn/live/5061/64k.mp3"},
        {"宣城综合广播", "https://lhttp.qtfm.cn/live/5022/64k.mp3"},
        {"宣城交通1061", "https://lhttp.qtfm.cn/live/5023/64k.mp3"},
        {"芜湖交通经济广播", "https://lhttp.qtfm.cn/live/5027/64k.mp3"},
        {"芜湖音乐故事广播", "https://lhttp.qtfm.cn/live/5028/64k.mp3"},
        {"芜湖综合广播", "https://lhttp.qtfm.cn/live/5029/64k.mp3"},
        {"黑龙江音乐广播", "https://lhttp.qtfm.cn/live/4969/64k.mp3"},
        {"黑龙江生活广播", "https://lhttp.qtfm.cn/live/4970/64k.mp3"},
        {"黑龙江交通广播", "https://lhttp.qtfm.cn/live/4973/64k.mp3"},
        {"龙广新闻台", "https://lhttp.qtfm.cn/live/4974/64k.mp3"},
        {"黑龙江老年少儿广播", "https://lhttp.qtfm.cn/live/4972/64k.mp3"},
        {"长春交通之声", "https://lhttp.qtfm.cn/live/4967/64k.mp3"},
        {"长春新闻广播", "https://lhttp.qtfm.cn/live/5013/64k.mp3"},
        {"长春城市生活广播1063", "https://lhttp.qtfm.cn/live/4984/64k.mp3"},
        {"松原新闻综合广播", "https://lhttp.qtfm.cn/live/5079/64k.mp3"},
        {"松原大众生活广播", "https://lhttp.qtfm.cn/live/5082/64k.mp3"},
        {"白山交通广播FM950", "https://lhttp.qtfm.cn/live/5083/64k.mp3"},
        {"德阳综合广播", "https://lhttp.qtfm.cn/live/4987/64k.mp3"},
        {"钦州新闻综合广播", "https://lhttp.qtfm.cn/live/5042/64k.mp3"},
        {"贺州畅听882", "https://lhttp.qtfm.cn/live/5043/64k.mp3"},
        {"黔西南综合广播107.9", "https://lhttp.qtfm.cn/live/5045/64k.mp3"},
        {"青海经济生活广播", "https://lhttp.qtfm.cn/live/5008/64k.mp3"},
        {"青海交通音乐", "https://lhttp.qtfm.cn/live/5009/64k.mp3"},
        {"邯郸新闻综合广播", "https://lhttp.qtfm.cn/live/5072/64k.mp3"},
    };
}

std::vector<RadioStation>& Stations() {
    static std::vector<RadioStation> stations = BuildStations();
    return stations;
}

}  // namespace

RadioPlayer& RadioPlayer::GetInstance() {
    static RadioPlayer instance;
    return instance;
}

int RadioPlayer::StationCount() const { return static_cast<int>(Stations().size()); }

std::string RadioPlayer::StationName(int index) const {
    const auto& stations = Stations();
    if (index < 0 || index >= static_cast<int>(stations.size())) {
        return std::string();
    }
    return stations[index].name;
}

std::string RadioPlayer::CurrentStationName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return name_;
}

bool RadioPlayer::IsPlaying() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

bool RadioPlayer::Play(int index) {
    const auto& stations = Stations();
    if (stations.empty()) {
        return false;
    }
    if (index < 0 || index >= static_cast<int>(stations.size())) {
        index = 0;
    }
    Stop(3000);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        index_ = index;
        name_ = stations[index].name;
        stop_requested_ = false;
        active_ = true;
    }
    BaseType_t ret = xTaskCreate(TaskEntry, "radio_player", kTaskStackSize, this, kTaskPriority, &task_);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create radio task");
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        task_ = nullptr;
        return false;
    }
    return true;
}

bool RadioPlayer::PlayByName(const std::string& keyword) {
    const auto& stations = Stations();
    if (keyword.empty()) {
        return Play(0);
    }
    for (size_t i = 0; i < stations.size(); i++) {
        if (std::string(stations[i].name).find(keyword) != std::string::npos) {
            return Play(static_cast<int>(i));
        }
    }
    ESP_LOGW(TAG, "no station matches: %s", keyword.c_str());
    return false;
}

bool RadioPlayer::Next() {
    int next = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next = (index_ + 1) % static_cast<int>(Stations().size());
    }
    return Play(next);
}

bool RadioPlayer::Prev() {
    int prev = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prev = (index_ - 1 + static_cast<int>(Stations().size())) % static_cast<int>(Stations().size());
    }
    return Play(prev);
}

bool RadioPlayer::Stop(int timeout_ms) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
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
    ESP_LOGW(TAG, "radio task still running");
    return false;
}

void RadioPlayer::TaskEntry(void* arg) { static_cast<RadioPlayer*>(arg)->Task(); }

void RadioPlayer::Task() {
    int station_index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        station_index = index_;
    }
    bool ok = false;
    for (int attempt = 0; attempt <= kMaxRetry; attempt++) {
        if (stop_requested_) {
            break;
        }
        ok = StreamOne(station_index);
        if (ok || stop_requested_) {
            break;
        }
        ESP_LOGW(TAG, "stream failed, retry %d/%d", attempt + 1, kMaxRetry);
        vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
    }
    if (!ok) {
        ESP_LOGE(TAG, "radio stream gave up");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        task_ = nullptr;
    }
    ESP_LOGI(TAG, "radio task exited");
    vTaskDelete(nullptr);
}

bool RadioPlayer::StreamOne(int index) {
    const auto& stations = Stations();
    if (index < 0 || index >= static_cast<int>(stations.size())) {
        return false;
    }
    const RadioStation& station = stations[index];

    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGE(TAG, "no audio codec");
        return false;
    }
    int target_rate = codec->output_sample_rate();
    if (target_rate <= 0) {
        target_rate = 16000;
    }

    // 先等当前语音（TTS）播完，避免和语音抢喇叭。
    AudioService* audio = &Application::GetInstance().GetAudioService();
    for (int waited = 0; waited < kMaxWaitPlaybackIdleMs; waited += 200) {
        if (stop_requested_) {
            return false;
        }
        if (audio == nullptr || audio->IsPlaybackIdle()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (!http) {
        ESP_LOGE(TAG, "failed to create http client");
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Accept-Encoding", "identity");
    http->SetHeader("Icy-MetaData", "0");
    http->SetHeader("User-Agent", "ESP32-Xiaozhi-Radio/1.0");

    ESP_LOGI(TAG, "tuning: %s (%s)", station.name, station.url);
    auto opened = http->Open("GET", station.url);
    if (!opened) {
        ESP_LOGE(TAG, "open failed: %s", opened.error().ToString().c_str());
        http->Close();
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status < 200 || *status >= 300) {
        ESP_LOGE(TAG, "http status: %s", status ? std::to_string(*status).c_str() : "no status");
        http->Close();
        return false;
    }

    mp3dec_t dec;
    mp3dec_init(&dec);

    std::vector<uint8_t> input;
    input.reserve(kPrebufferBytes * 2);
    size_t consumed = 0;

    std::vector<int16_t> pending;
    pending.reserve(2048);
    std::vector<int16_t> out_frame;
    out_frame.reserve(4096);

    const size_t chunk_samples = static_cast<size_t>(target_rate / 20);  // 50ms
    double prev_sample = 0.0;

    bool got_audio = false;
    std::array<char, kReadBufferSize> read_buffer;

    while (!stop_requested_) {
        // 数据不足就继续读
        if (input.size() - consumed < kPrebufferBytes) {
            if (consumed > 0) {
                input.erase(input.begin(), input.begin() + consumed);
                consumed = 0;
            }
            auto size = http->Read(read_buffer.data(), read_buffer.size());
            if (!size) {
                ESP_LOGE(TAG, "read failed: %s", size.error().ToString().c_str());
                break;
            }
            if (*size == 0) {
                ESP_LOGW(TAG, "stream closed");
                break;
            }
            input.insert(input.end(), read_buffer.data(), read_buffer.data() + *size);
            continue;
        }

        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        int samples = mp3dec_decode_frame(&dec, input.data() + consumed,
                                         static_cast<int>(input.size() - consumed), pcm, &info);
        if (info.frame_bytes == 0) {
            // 还没凑够一帧，等更多数据
            consumed = 0;
            continue;
        }
        consumed += static_cast<size_t>(info.frame_bytes);
        if (samples <= 0 || info.channels <= 0 || info.sample_rate <= 0) {
            continue;
        }
        got_audio = true;

        // 声道混合成单声道（喇叭是单声道播放）
        int channels = info.channels;
        out_frame.resize(static_cast<size_t>(samples));
        if (channels == 1) {
            memcpy(out_frame.data(), pcm, sizeof(int16_t) * static_cast<size_t>(samples));
        } else {
            int frames = samples / channels;
            out_frame.resize(static_cast<size_t>(frames));
            for (int i = 0; i < frames; i++) {
                int32_t sum = 0;
                for (int c = 0; c < channels; c++) {
                    sum += pcm[i * channels + c];
                }
                out_frame[i] = static_cast<int16_t>(sum / channels);
            }
            samples = frames;
        }

        // 线性插值重采样到喇叭采样率
        double step = static_cast<double>(info.sample_rate) / static_cast<double>(target_rate);
        if (step < 0.0001) {
            step = 1.0;
        }
        double pos = 0.0;
        const int n = samples;
        while (pos < static_cast<double>(n)) {
            int i0 = static_cast<int>(pos);
            double frac = pos - i0;
            double v0 = (i0 <= 0) ? prev_sample : static_cast<double>(out_frame[i0 - 1]);
            double v1 = static_cast<double>(out_frame[i0]);
            double v = v0 + (v1 - v0) * frac;
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            pending.push_back(static_cast<int16_t>(v));
            pos += step;
            if (pending.size() >= chunk_samples) {
                if (stop_requested_) {
                    break;
                }
                codec->OutputData(pending);
                pending.clear();
            }
        }
        prev_sample = static_cast<double>(out_frame[n - 1]);

        if (!pending.empty() && pending.size() > chunk_samples) {
            // 兜底，避免堆积
            pending.resize(chunk_samples);
        }
    }

    if (!pending.empty() && !stop_requested_) {
        codec->OutputData(pending);
        pending.clear();
    }
    http->Close();
    http.reset();
    ESP_LOGI(TAG, "stream stopped, got_audio=%d", got_audio ? 1 : 0);
    // 直播流本身就是一直播的，走到这里说明流断了或用户停止
    return got_audio && !stop_requested_;
}
