#pragma once

#include "../music_player.h"

typedef struct _MP3FrameInfo MP3FrameInfo;
typedef void* HMP3Decoder;

class Mp3MusicPlayer : public MusicPlayer {
public:
    Mp3MusicPlayer();
    ~Mp3MusicPlayer() override;

private:
    bool OpenDecoder() override;
    void CloseDecoder() override;
    bool DecodeAndPlayFrame(RingBuffer& buffer) override;
    void SkipId3Tag(RingBuffer& buffer);

    void ConvertPcmIfNeeded(const MP3FrameInfo& frame_info, const std::vector<int16_t>& pcm_buffer,
                            std::vector<int16_t>& output_pcm, int& output_samples,
                            int& output_channels);
    void UpdateTimeInfo(int codec_output_rate, int output_samples, int output_channels,
                        const MP3FrameInfo& frame_info);

    HMP3Decoder decoder_{nullptr};
    std::vector<int16_t> pcm_buffer_{PCM_BUFFER_SIZE / 2};
    int consecutive_skip_count_{0};

    static constexpr int MAX_CONSECUTIVE_SKIPS = 100;
    static constexpr int PCM_BUFFER_SIZE = 8 * 1024;
};