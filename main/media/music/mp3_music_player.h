#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include "music_player.h"
#include <vector>

class AudioCodec;
class Http;
class NetworkInterface;
typedef struct _MP3FrameInfo MP3FrameInfo;
typedef void* HMP3Decoder;

class Mp3MusicPlayer : public MusicPlayer {
public:
    Mp3MusicPlayer();
    ~Mp3MusicPlayer();

    bool Play(const Music& music) override;
    void Play(const std::vector<Music>& music_list) override;
    bool ChangePlayControlMode(const PlayControlMode& mode) override;
    bool IsPlaying() const override;

private:
    void PlayInternal(const Music& music);
    bool PreparePlayback(const Music& music,  NetworkInterface*& network,
                         std::string& final_url, std::unique_ptr<Http>& http);
    void ConfigureHttpHeaders(Http* http, const std::string& url);
    bool ReadInitialBuffer(Http* http, std::vector<uint8_t>& mp3_buffer, size_t& mp3_data_size);
    void SkipId3Tag(std::vector<uint8_t>& mp3_buffer, size_t& mp3_data_size, size_t& mp3_data_offset);
    bool RefillBuffer(Http* http, std::vector<uint8_t>& mp3_buffer, size_t& mp3_data_offset,
                      size_t& mp3_data_size);
    void ConvertPcmIfNeeded( const MP3FrameInfo& frame_info,
                            const std::vector<int16_t>& pcm_buffer,
                            std::vector<int16_t>& output_pcm, int& output_samples,
                            int& output_channels);
    bool DecodeAndPlayFrame(HMP3Decoder decoder,
                            std::vector<uint8_t>& mp3_buffer, size_t& mp3_data_offset,
                            size_t& mp3_data_size, std::vector<int16_t>& pcm_buffer,
                            int& consecutive_skip_count);

    static void PlayTask(void* arg);

    static constexpr int MAX_CONSECUTIVE_SKIPS = 100;  // 跳过超过100次则停止该曲目
    AudioCodec* audio_codec_;
    std::atomic<bool> is_playing_{false};
    std::mutex mutex_;
    std::vector<Music> current_music_list_;

    std::atomic < MusicPlayer::PlayControlMode > current_control_mode_{MusicPlayer::PlayControlMode::kUnknown};
};