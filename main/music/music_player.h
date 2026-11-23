#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <mutex>
#include <string>
#include <vector>

class AudioService;

struct MusicTrack {
    std::string title;
    std::string url;  // 必须是 Opus 编码的 OGG 直链（.ogg / .opus）
};

// 线路一：设备自己用 HTTP 拉取 OGG(Opus) 音频流，复用现有的 OggDemuxer + AudioService 解码播放通路。
// 不触碰 ASR / LLM / TTS / 协议通道，只往解码队列里塞音频包。
class MusicPlayer {
public:
    static MusicPlayer& GetInstance();

    void SetPlaylist(const std::vector<MusicTrack>& tracks);
    std::vector<MusicTrack> GetPlaylist();

    // 播放指定曲目（会自动停掉正在播放的曲目）。
    bool Play(size_t index);
    // 播放列表中的第一首。
    bool PlayDefault();
    // 下一首。
    bool Next();

    void Pause();
    void Resume();
    // 停止播放，最多等待 timeout_ms 让播放任务退出；返回是否已完全停止。
    bool Stop(int timeout_ms = 3000);

    bool IsActive() const;
    bool IsPaused() const;
    bool IsPlaying() const;
    std::string CurrentTitle() const;
    size_t CurrentIndex() const;

private:
    MusicPlayer();
    ~MusicPlayer() = default;
    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    static void TaskEntry(void* arg);
    void Task();
    bool PlayOne(const MusicTrack& track, AudioService* audio);
    bool IsStopRequested() const;

    mutable std::mutex mutex_;
    std::vector<MusicTrack> playlist_;
    TaskHandle_t task_ = nullptr;
    size_t index_ = 0;
    std::string title_;
    bool active_ = false;          // 播放任务在运行（播放或暂停）
    bool paused_ = false;          // 暂停中
    bool stop_requested_ = false;  // 请求退出播放任务
    bool loop_ = true;             // 列表播完后循环
};

#endif  // MUSIC_PLAYER_H
