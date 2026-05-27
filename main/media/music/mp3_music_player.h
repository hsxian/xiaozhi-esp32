#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <condition_variable>
#include "music_player.h"
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "device_state.h"

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

    bool Play(const Music& music) override;
    void Play(const std::vector<Music>& music_list) override;
    bool ChangePlayControlMode(const PlayControlMode& mode) override;
    bool IsPlaying() const override;
    
private:
    // 处理接收到的MP3数据块，返回是否继续处理（true=继续，false=结束）
    bool ProcessReceivedChunk(DataChunk& chunk, std::vector<uint8_t>& mp3_buffer,
                              size_t& mp3_data_offset, size_t& mp3_data_size,
                              bool& track_complete, bool& track_error,
                              const char* log_tag = "Received");
    
    // 解码播放线程函数
    static void DecodePlayTask(void* arg);
    void DecodePlayLoop();
    
    // 播放控制函数
    void SkipId3Tag(std::vector<uint8_t>& mp3_buffer, size_t& mp3_data_size, size_t& mp3_data_offset);
    void ConvertPcmIfNeeded(const MP3FrameInfo& frame_info,
                            const std::vector<int16_t>& pcm_buffer,
                            std::vector<int16_t>& output_pcm, int& output_samples,
                            int& output_channels);
    bool DecodeAndPlayFrame(HMP3Decoder decoder,
                            std::vector<uint8_t>& mp3_buffer, size_t& mp3_data_offset,
                            size_t& mp3_data_size, std::vector<int16_t>& pcm_buffer,
                            int& consecutive_skip_count);
    
    bool CanChangePlayControlMode(const PlayControlMode& mode);
    // 清理资源
    void CleanupResources();
    void OnStateMachineCallback(DeviceState old_state, DeviceState new_state);
    
    bool TrySetControlModeToHandled(int task_flag);
    void ResetHandledTaskListFlag();
    void WaitPalySattus();  // 是否需要等待播放状态
    void UpdateTimeInfo(int codec_output_rate, int output_samples, int output_channels,const MP3FrameInfo& frame_info);
    void DownloadLyrics(const Music& music);
    void ShowLyrics();

    static constexpr int MAX_CONSECUTIVE_SKIPS = 100;  // 跳过超过100次则停止该曲目
    static constexpr int BUFFER_SIZE = 1 * 1024;       // 减小缓冲区到1KB，降低内存压力并减缓下载速度
    static constexpr int PCM_BUFFER_SIZE = 8 * 1024;   // PCM输出缓冲区
    static constexpr int QUEUE_SIZE = 16;              // 队列大小
    static constexpr int HIGH_WATER_MARK = 8;          // 降低高水位标记，更早开始节流
    static constexpr int CRITICAL_WATER_MARK = 14;     // 临界水位，接近满队列
    static constexpr int TASK_LIST_FLAG = 0b01;        // 任务列表标志位

    AudioCodec* audio_codec_{nullptr};
    Display* display_{nullptr};
    std::atomic<bool> is_playing_{false};
    std::atomic<bool> is_downloading_{false};
    std::atomic<bool> is_paused_{false};
    std::mutex mutex_;
    std::mutex http_mutex_;
    std::condition_variable pause_cv_;
    std::vector<Music> current_music_list_;
    std::atomic<MusicPlayer::PlayControlMode> current_control_mode_{MusicPlayer::PlayControlMode::kUnknown};
    Lyrics* lyrics_{nullptr};
    HttpStream* http_stream_{nullptr};

    // 共享资源
    TaskHandle_t decode_task_handle_{nullptr};
    std::atomic<int> current_track_index_{0};
    std::atomic<int> handled_task_list_flag_{0};
    std::string current_url_;
    size_t download_bytes_received_{0};
    int listener_id_{-1};
};