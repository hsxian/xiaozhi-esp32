#pragma once

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>
#include <algorithm>
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

// 固定容量的环形缓冲区，避免动态增长导致内存分配失败
// 数据始终保持连续，写入时若head_>0则自动compact
class RingBuffer {
public:
    static constexpr size_t DEFAULT_CAPACITY = 64 * 1024;  // 64KB，匹配队列容量(16×4KB)

    explicit RingBuffer(size_t capacity = DEFAULT_CAPACITY)
        : buffer_(capacity), head_(0), size_(0) {}

    size_t size() const { return size_; }
    size_t capacity() const { return buffer_.size(); }
    bool empty() const { return size_ == 0; }

    uint8_t* data() { return buffer_.data(); }
    const uint8_t* data() const { return buffer_.data(); }
    uint8_t* read_ptr() { return buffer_.data() + head_; }
    const uint8_t* read_ptr() const { return buffer_.data() + head_; }

    size_t write_available() const {
        size_t used = head_ + size_;
        return used >= buffer_.size() ? 0 : buffer_.size() - used;
    }
    size_t head() const { return head_; }

    // 写入数据，若空间不足先compact，仍不足则截断。返回实际写入字节数
    size_t write(const uint8_t* src, size_t len) {
        if (len == 0) return 0;
        if (head_ > 0) compact();
        size_t space = buffer_.size() - (head_ + size_);
        if (len > space) {
            ESP_LOGW("RingBuffer", "write truncated: requested=%u, available=%u, used=%u/%u",
                     (unsigned int)len, (unsigned int)space, (unsigned int)size_,
                     (unsigned int)buffer_.size());
            len = space;
        }
        if (len == 0) return 0;
        memcpy(buffer_.data() + head_ + size_, src, len);
        size_ += len;
        return len;
    }

    // 从头部消费n字节
    void consume(size_t n) {
        n = std::min(n, size_);
        head_ += n;
        size_ -= n;
    }

    // 跳过前部 n 字节。当前实现使用 head_ 作为逻辑起点，因此直接前移 head_ 即可。
    void skip_front(size_t n) {
        consume(n);
    }

    // 将数据压缩到缓冲区前端，head_置0
    void compact() {
        if (head_ == 0) return;
        if (size_ > 0) {
            memmove(buffer_.data(), buffer_.data() + head_, size_);
        }
        head_ = 0;
    }

    // 确保数据连续（不回绕），若回绕则compact
    void ensure_contiguous() {
        if (head_ + size_ > buffer_.size()) compact();
    }

    bool is_contiguous() const { return head_ + size_ <= buffer_.size(); }

    uint8_t& operator[](size_t i) { return buffer_[head_ + i]; }
    const uint8_t& operator[](size_t i) const { return buffer_[head_ + i]; }

    void clear() { head_ = 0; size_ = 0; }

private:
    std::vector<uint8_t> buffer_;
    size_t head_;
    size_t size_;
};

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
    bool ProcessReceivedChunk(DataChunk& chunk, RingBuffer& mp3_buffer, bool& track_complete,
                              bool& track_error, const char* log_tag = "Received");

    // 解码播放线程函数
    static void PlayMusicTask(void* arg);
    void PlayMusicLoop();
    void DecodePlayLoop(Music& music);

    // 播放控制函数
    void SkipId3Tag(RingBuffer& mp3_buffer);
    void ConvertPcmIfNeeded(const MP3FrameInfo& frame_info, const std::vector<int16_t>& pcm_buffer,
                            std::vector<int16_t>& output_pcm, int& output_samples,
                            int& output_channels);
    bool DecodeAndPlayFrame(HMP3Decoder decoder, RingBuffer& mp3_buffer,
                            std::vector<int16_t>& pcm_buffer, int& consecutive_skip_count);

    bool CanChangePlayControlMode(const PlayControlMode& mode);
    // 清理资源
    void CleanupResources();
    bool OnWakeWordDetected(void* data);
    bool OnToggleChatEvent(void* data);

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
    int toggle_chat_listener_id_{-1};
    int state_machine_listener_id_{-1};

    SemaphoreHandle_t pause_ack_semaphore_{nullptr};

    // 预分配的PCM输出缓冲区，避免运行时内存分配失败
    static constexpr int MAX_PCM_OUTPUT_SAMPLES = 8192;  // 最大PCM输出样本数
    std::vector<int16_t> output_pcm_buffer_;             // 预分配的输出缓冲区

    // 静音状态追踪，用于淡入淡出过渡防止炸音
    bool was_outputting_silence_ = false;
};