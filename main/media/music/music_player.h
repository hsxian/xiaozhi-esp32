#pragma once

#include <string>
#include <vector>
#include "music.h"
#include <atomic>

class MusicPlayer {
public:
    enum class PlayControlMode: uint8_t {
        kUnknown,
        kControlHandled,
        kPause,
        kResume,
        kStop,
        kNext,
        kPrevious,
    };
    virtual bool Play(const Music& music) = 0;
    virtual void Play(const std::vector<Music>& music_list) = 0;
    virtual bool ChangePlayControlMode(const PlayControlMode& mode) = 0;
    int32_t current_position_ms() const { return current_position_ms_; }
    int32_t total_duration_ms() const { return total_duration_ms_; }
    // 检查是否正在播放
    virtual bool IsPlaying() const = 0;
    virtual ~MusicPlayer() {}

protected:
    bool IsNeedWaitDeviceState() const;
    void ResetPlaybackProgress();
    // 播放进度跟踪
    std::atomic<int32_t> total_duration_ms_{0};       // 当前歌曲总时长（毫秒）
    std::atomic<int32_t> current_position_ms_{0};     // 当前播放位置（毫秒）

private:
};