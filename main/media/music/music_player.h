#pragma once

#include <string>
#include <vector>
#include "music.h"


class MusicPlayer {
public:
    virtual bool Play(const Music& music) = 0;
    virtual void Play(const std::vector<Music>& music_list) = 0;
    // 停止播放
    virtual void Stop() = 0;
    virtual void Pause() = 0;
    virtual void Resume() = 0;
    // 检查是否正在播放
    virtual bool IsPlaying() const = 0;
    virtual ~MusicPlayer() {}

    bool IsNeedWaitPalySattus() const;
private:
};