#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "esp_audio_simple_dec.h"
#include "esp_audio_types.h"

class Music;
class AudioCodec;
class Display;
class Lyrics;
class HttpStream;
struct DataChunk;
struct RingBuffer;

class MusicPlayer {
public:
    enum class PlayControlMode : uint8_t {
        kUnknown,
        kControlHandled,
        kPause,
        kResume,
        kStop,
        kNext,
        kPrevious,
    };

    enum class LoopMode : uint8_t {
        kPlayOnce,  // 播放一遍
        kLoop,      // 循环播放
        kShuffle,   // 随机播放
    };

    // 播放器状态
    enum class PlayState : uint8_t {
        kIdle,      // 空闲
        kPlaying,   // 播放中
        kPausing,   // 暂停请求中（等待解码线程确认）
        kPaused,    // 已暂停（解码线程已确认）
        kResuming,  // 恢复请求中（等待解码线程确认）
    };

    MusicPlayer();
    virtual ~MusicPlayer();

    static std::unique_ptr<MusicPlayer> NewMusicPlayer();

    virtual bool Play(Music* music, LoopMode mode = LoopMode::kPlayOnce);
    virtual void Play(const std::vector<Music*>& music_list, LoopMode mode = LoopMode::kPlayOnce);
    virtual bool ChangePlayControlMode(const PlayControlMode& mode);

    LoopMode GetLoopMode() const { return loop_mode_; }
    int32_t current_position_ms() const { return current_position_ms_; }
    int32_t total_duration_ms() const { return total_duration_ms_; }
    bool IsPlaying() const { return play_state_ != PlayState::kIdle; }
    PlayState GetPlayState() const { return play_state_; }

    // 从原始数据流检测音频类型（支持 MP3 / AAC(ADTS) / M4A / FLAC）
    static esp_audio_simple_dec_type_t DetectAudioType(const uint8_t* data, size_t size);

protected:
    void ResetPlaybackProgress();

    // 播放控制
    void PlayMusicLoop();
    void DecodePlayLoop(Music& music);
    void PreparePlayState();
    bool HandleControlSignal();
    bool BreakDecodePlayLoop();
    bool CanChangePlayControlMode(const PlayControlMode& mode);
    void CleanupResources();

    bool HandlePauseState();
    void HandleResumeState(const std::string& url, RingBuffer& data_buffer);

    void HandleBufferUnderrun(int silence_duration_ms = 80, int fade_duration_ms = 5);
    void FadeOutAndStop(int fade_duration_ms = 10);
    void OutputAudioWithFadeIn(int output_samples);

    // 解码器钩子
    bool OpenDecoder(esp_audio_simple_dec_type_t type);
    void CloseDecoder();
    bool DecodeAndPlayFrame(RingBuffer& buffer);
    bool HandleFastForward(int samples, const esp_audio_simple_dec_info_t& info);

    // PCM 转换与时间更新
    void ConvertPcmIfNeeded(int input_rate, int input_channels, int input_samples,
                            int& output_samples, int& output_channels);
    void UpdateTimeInfo(int codec_output_rate, int output_samples, int output_channels,
                        int bitrate);

    // 歌词
    void DownloadLyrics(Music& music);
    void ShowLyrics();
    void ShuffleArray(std::vector<int>& arr, int size);

    // 事件回调
    bool OnWakeWordDetected(void* data);

    // 数据处理
    bool ProcessReceivedChunk(DataChunk& chunk, RingBuffer& buffer, bool& track_complete,
                              bool& track_error, const char* log_tag = "Received");

    static void PlayMusicTask(void* arg);

    // 播放进度跟踪
    std::atomic<int32_t> total_duration_ms_{0};
    std::atomic<int32_t> current_position_ms_{0};
    LoopMode loop_mode_{LoopMode::kPlayOnce};
    std::atomic<PlayState> play_state_{PlayState::kIdle};

    // 基础设施
    AudioCodec* audio_codec_{nullptr};
    Display* display_{nullptr};
    std::mutex mutex_;
    std::condition_variable pause_cv_;
    std::vector<Music*> current_music_list_;
    std::atomic<PlayControlMode> current_control_mode_{PlayControlMode::kUnknown};
    Lyrics* lyrics_{nullptr};
    HttpStream* http_stream_{nullptr};

    // 任务管理
    TaskHandle_t decode_task_handle_{nullptr};
    std::atomic<int> current_track_index_{0};
    std::string current_url_;

    // 事件监听
    int wake_word_listener_id_{-1};
    int toggle_chat_listener_id_{-1};
    int state_machine_listener_id_{-1};

    // 同步
    SemaphoreHandle_t pause_ack_semaphore_{nullptr};

    // 解码状态
    bool track_complete_{false};
    bool track_error_{false};

    // 统一解码器
    esp_audio_simple_dec_handle_t decoder_{nullptr};
    esp_audio_simple_dec_type_t decoder_type_{ESP_AUDIO_SIMPLE_DEC_TYPE_NONE};
    int32_t fast_forward_to_ms_{0};  // M4A 断点续播快进目标时间，0 表示不需要快进
    static constexpr int MAX_CONSECUTIVE_SKIPS = 100;
    int consecutive_skip_count_{0};

    // 预分配的PCM输出缓冲区
    static constexpr int MAX_PCM_OUTPUT_SAMPLES = 8192;
    std::vector<int16_t> output_pcm_buffer_;

    // 静音状态追踪
    bool was_outputting_silence_ = false;

    // 队列水位常量
    static constexpr int QUEUE_SIZE = 16;
    static constexpr int HIGH_WATER_MARK = 8;
    static constexpr int CRITICAL_WATER_MARK = 14;
};