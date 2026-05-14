#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include "music.h"

class MusicManager {
public:
    static MusicManager& GetInstance();

    // 流式播放 MP3 URL（不下载整个文件）
    bool PlayMp3Url(const std::string& url, int volume = 80);
    
    // 停止播放
    void Stop();
    
    // 检查是否正在播放
    bool IsPlaying() const { return is_playing_; }

private:
    MusicManager();
    ~MusicManager() = default;

    // 播放任务入口
    static void PlayTask(void* arg);
    
    // 内部播放逻辑
    void PlayMp3UrlInternal(const std::string& url, int volume);

    std::atomic<bool> is_playing_{false};
    std::atomic<bool> stop_requested_{false};
    std::mutex mutex_;
    std::string current_url_;
};