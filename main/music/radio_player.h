#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <mutex>
#include <string>

struct RadioStation {
    const char* name;
    const char* url;
};

// 网络电台播放器（线路一）：设备自己用 HTTP 拉 MP3 直播流，
// 本地用 minimp3 解码成 PCM 后直接写进音频输出，不经过服务器。
class RadioPlayer {
public:
    static RadioPlayer& GetInstance();

    int StationCount() const;
    std::string StationName(int index) const;
    std::string CurrentStationName() const;
    bool IsPlaying() const;

    // 播放指定频道（从 0 开始）。已在播放时会先停掉当前流。
    bool Play(int index);
    // 按台名关键词切台，支持“中国之声”“交通”“音乐”这种模糊匹配。
    bool PlayByName(const std::string& keyword);
    bool Next();
    bool Prev();
    bool Stop(int timeout_ms = 4000);

private:
    RadioPlayer() = default;
    ~RadioPlayer() = default;
    static void TaskEntry(void* arg);
    void Task();
    bool StreamOne(int index);

    mutable std::mutex mutex_;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> stop_requested_{false};
    bool active_ = false;
    int index_ = 0;
    std::string name_;
};
