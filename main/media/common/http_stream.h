#pragma once

#include <cstdint>
#include <string>
#include "esp_http_client.h"
#include <atomic>
#include <freertos/queue.h>

// 数据块状态枚举（使用 uint8_t 节省内存）
enum class DataStatus : uint8_t {
    kNormal,   // 正常数据
    kEos,      // 流结束
    kError,    // 错误
    kTimeout,  // 超时错误
};

// MP3数据块结构
struct DataChunk {
    uint8_t* data;
    size_t size;
    DataStatus status;
    DataChunk() : data(nullptr), size(0), status(DataStatus::kNormal) {}
    DataChunk(uint8_t* d, size_t s) : data(d), size(s), status(DataStatus::kNormal) {}
};

class HttpStream {
public:
    HttpStream();
    ~HttpStream();
    bool Init(int timeout_ms);
    bool Open(const std::string& url);
    void ClearDataQueue();

private:
    static constexpr int MAX_CONSECUTIVE_SKIPS = 100;  // 跳过超过100次则停止该曲目
    static constexpr int BUFFER_SIZE = 1 * 1024;      // 减小缓冲区到1KB，降低内存压力并减缓下载速度
    static constexpr int PCM_BUFFER_SIZE = 8 * 1024;  // PCM输出缓冲区
    static constexpr int QUEUE_SIZE = 16;             // 队列大小
    static constexpr int HIGH_WATER_MARK = 8;         // 降低高水位标记，更早开始节流
    static constexpr int CRITICAL_WATER_MARK = 14;    // 临界水位，接近满队列

    static void OpenTask(void* arg);
    static esp_err_t http_event_handler(esp_http_client_event_t* evt);
    
    esp_http_client_handle_t client_;

    std::string url_str_;
    TaskHandle_t task_handle_{nullptr};
    QueueHandle_t data_queue_;  // MP3数据队列
    size_t download_bytes_received_{0};
    void SendError();
    void SendEos();
    void SendData(DataChunk chunk);
};