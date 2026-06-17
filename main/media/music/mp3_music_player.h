#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "device_state.h"
#include "music_player.h"

class AudioCodec;
class Http;
class Display;
class NetworkInterface;
class Lyrics;
class HttpStream;
struct DataChunk;
typedef struct _MP3FrameInfo MP3FrameInfo;
typedef void* HMP3Decoder;

class Mp3MusicPlayer : public MusicPlayer {
public:
    Mp3MusicPlayer();
    ~Mp3MusicPlayer();

    bool Play(Music* music, LoopMode mode = LoopMode::kPlayOnce) override;
    void Play(const std::vector<Music*>& music_list,
              LoopMode mode = LoopMode::kPlayOnce) override;
    bool ChangePlayControlMode(const PlayControlMode& mode) override;

private:
    // 处理接收到的MP3数据块，返回是否继续处理（true=继续，false=结束）
    bool ProcessReceivedChunk(DataChunk& chunk, std::vector<uint8_t>& mp3_buffer,
                              size_t& mp3_data_offset, size_t& mp3_data_size, bool& track_complete,
                              bool& track_error, const char* log_tag = "Received");

    // 解码播放线程函数
    static void PlayMusicTask(void* arg);
    void PlayMusicLoop();
    void DecodePlayLoop(Music& music);

    // 播放控制函数
    void SkipId3Tag(std::vector<uint8_t>& mp3_buffer, size_t& mp3_data_size,
                    size_t& mp3_data_offset);
    void ConvertPcmIfNeeded(const MP3FrameInfo& frame_info, const std::vector<int16_t>& pcm_buffer,
                            std::vector<int16_t>& output_pcm, int& output_samples,
                            int& output_channels);
    bool DecodeAndPlayFrame(HMP3Decoder decoder, std::vector<uint8_t>& mp3_buffer,
                            size_t& mp3_data_offset, size_t& mp3_data_size,
                            std::vector<int16_t>& pcm_buffer, int& consecutive_skip_count);

    bool CanChangePlayControlMode(const PlayControlMode& mode);
    // 清理资源
    void CleanupResources();
    bool OnWakeWordDetected(void* data);

    void UpdateTimeInfo(int codec_output_rate, int output_samples, int output_channels,
                        const MP3FrameInfo& frame_info);
    void DownloadLyrics(Music& music);
    void ShowLyrics();
    bool HandleControlSignal();
    bool BreakDecodePlayLoop();

    // 准备播放的状态
    void PreparePlayState();

    static constexpr int MAX_CONSECUTIVE_SKIPS = 100;  // 跳过超过100次则停止该曲目
    static constexpr int BUFFER_SIZE = 1 * 1024;      // 减小缓冲区到1KB，降低内存压力并减缓下载速度
    static constexpr int PCM_BUFFER_SIZE = 8 * 1024;  // PCM输出缓冲区
    static constexpr int QUEUE_SIZE = 16;             // 队列大小
    static constexpr int HIGH_WATER_MARK = 8;         // 降低高水位标记，更早开始节流
    static constexpr int CRITICAL_WATER_MARK = 14;    // 临界水位，接近满队列

    bool IsPlaying() const override { return play_state_ != PlayState::kIdle; }

    AudioCodec* audio_codec_{nullptr};
    Display* display_{nullptr};
    std::mutex mutex_;
    std::condition_variable pause_cv_;
    std::vector<Music*> current_music_list_;
    std::atomic<MusicPlayer::PlayControlMode> current_control_mode_{
        MusicPlayer::PlayControlMode::kUnknown};
    Lyrics* lyrics_{nullptr};
    HttpStream* http_stream_{nullptr};

    // 共享资源
    TaskHandle_t decode_task_handle_{nullptr};
    std::atomic<int> current_track_index_{0};
    std::string current_url_;
    int wake_word_listener_id_{-1};
    int state_machine_listener_id_{-1};

    SemaphoreHandle_t pause_ack_semaphore_{nullptr};

    // 预分配的PCM输出缓冲区，避免运行时内存分配失败
    static constexpr int MAX_PCM_OUTPUT_SAMPLES = 8192;  // 最大PCM输出样本数
    std::vector<int16_t> output_pcm_buffer_;             // 预分配的输出缓冲区

    // 静音状态追踪，用于淡入淡出过渡防止炸音
    bool was_outputting_silence_ = false;
};