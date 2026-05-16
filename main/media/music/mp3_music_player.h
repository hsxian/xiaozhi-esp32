#pragma once

#include <string>
#include <atomic>
#include <mutex>
#include "music_player.h"


class Mp3MusicPlayer : public MusicPlayer {
public:
    Mp3MusicPlayer();
    ~Mp3MusicPlayer();

    bool Play(const Music& music) override;
    void Play(const std::vector<Music>& music_list) override;
    void Stop() override;
    void Pause() override;
    void Resume() override;
    bool IsPlaying() const override;

private:
    void PlayInternal(const Music& music);
    static void PlayTask(void* arg);
    std::atomic<bool> is_playing_{false};
    std::atomic<bool> stop_requested_{false};
    std::mutex mutex_;
    std::vector<Music> current_music_list_;
};