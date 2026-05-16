#pragma once

#include <string>
#include <vector>
#include "music.h"

class MusicPlayer {
public:
    enum PlayControlMode {
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
    // 检查是否正在播放
    virtual bool IsPlaying() const = 0;
    virtual ~MusicPlayer() {}

protected:
    bool IsNeedWaitPalySattus() const;
private:
};