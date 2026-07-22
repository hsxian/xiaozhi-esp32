#pragma once

#include <esp_log.h>
#include <string.h>
#include <algorithm>
#include <memory>
#include <vector>

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
        if (len == 0)
            return 0;
        if (head_ > 0)
            compact();
        size_t space = buffer_.size() - (head_ + size_);
        if (len > space) {
            ESP_LOGW("RingBuffer", "write truncated: requested=%u, available=%u, used=%u/%u",
                     (unsigned int)len, (unsigned int)space, (unsigned int)size_,
                     (unsigned int)buffer_.size());
            len = space;
        }
        if (len == 0)
            return 0;
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
    void skip_front(size_t n) { consume(n); }

    // 将数据压缩到缓冲区前端，head_置0
    void compact() {
        if (head_ == 0)
            return;
        if (size_ > 0) {
            memmove(buffer_.data(), buffer_.data() + head_, size_);
        }
        head_ = 0;
    }

    // 确保数据连续（不回绕），若回绕则compact
    void ensure_contiguous() {
        if (head_ + size_ > buffer_.size())
            compact();
    }

    bool is_contiguous() const { return head_ + size_ <= buffer_.size(); }

    uint8_t& operator[](size_t i) { return buffer_[head_ + i]; }
    const uint8_t& operator[](size_t i) const { return buffer_[head_ + i]; }

    void clear() {
        head_ = 0;
        size_ = 0;
    }

private:
    std::vector<uint8_t> buffer_;
    size_t head_;
    size_t size_;
};