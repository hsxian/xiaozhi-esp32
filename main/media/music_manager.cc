#include "music_manager.h"
#include "board.h"
#include "audio_codec.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "MusicManager"
#define BUFFER_SIZE (4 * 1024)  // 4KB缓冲区
#define PCM_BUFFER_SIZE (2 * 1024)  // PCM输出缓冲区

MusicManager::MusicManager() {}

MusicManager& MusicManager::GetInstance()
{
    static MusicManager instance;
    return instance;
}

bool MusicManager::PlayMp3Url(const std::string& url, int volume)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (is_playing_) {
        ESP_LOGW(TAG, "Already playing, stop current first");
        return false;
    }

    current_url_ = url;
    stop_requested_ = false;
    is_playing_ = true;

    // 创建播放任务
    xTaskCreate(PlayTask, "mp3_play_task", 8192, this, 5, nullptr);

    ESP_LOGI(TAG, "Started playing MP3 from URL: %s", url.c_str());
    return true;
}

void MusicManager::Stop()
{
    stop_requested_ = true;
}

void MusicManager::PlayTask(void* arg)
{
    MusicManager* manager = static_cast<MusicManager*>(arg);
    const std::string url = manager->current_url_;
    const int volume = 80;  // 可以改为参数

    manager->PlayMp3UrlInternal(url, volume);

    manager->is_playing_ = false;
    vTaskDelete(nullptr);
}

void MusicManager::PlayMp3UrlInternal(const std::string& url, int volume)
{
    auto& board = Board::GetInstance();
    auto audio_codec = board.GetAudioCodec();
    auto network = board.GetNetwork();

    if (!audio_codec || !network) {
        ESP_LOGE(TAG, "Audio codec or network not available");
        return;
    }

    // 创建HTTP客户端
    auto http = network->CreateHttp(30);  // 30秒超时
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection for: %s", url.c_str());
        http->Close();
        return;
    }

    // 设置必要的请求头（支持流式传输）
    http->SetHeader("Accept", "audio/mpeg");
    http->SetHeader("Connection", "keep-alive");

    // 获取响应状态码
    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed, status code: %d", status_code);
        http->Close();
        return;
    }

    // 设置音量
    audio_codec->SetOutputVolume(volume);
    audio_codec->EnableOutput(true);

    // 分配缓冲区
    std::vector<uint8_t> mp3_buffer(BUFFER_SIZE);
    std::vector<int16_t> pcm_buffer(PCM_BUFFER_SIZE / 2);  // 16-bit PCM

    // 读取并播放MP3数据流
    while (!stop_requested_) {
        // 从HTTP读取数据
        int bytes_read = http->Read(reinterpret_cast<char*>(mp3_buffer.data()), BUFFER_SIZE);
        
        if (bytes_read <= 0) {
            ESP_LOGI(TAG, "End of stream or connection closed");
            break;
        }

        ESP_LOGD(TAG, "Read %d bytes from stream", bytes_read);

        // 注意：这里需要MP3解码器将MP3数据解码为PCM
        // 由于缺少MP3解码库，这里演示基本的流式框架
        // 实际项目中需要添加MP3解码支持（如使用libmad或ESP音频解码API）

        // 临时：填充静音数据作为占位符
        // 在实际实现中，这里应该调用MP3解码器
        std::fill(pcm_buffer.begin(), pcm_buffer.end(), 0);

        // 播放PCM数据（如果有解码器，这里播放解码后的PCM）
        if (!stop_requested_) {
            audio_codec->OutputData(pcm_buffer);
        }

        // 小延迟，避免过快消耗数据流
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 清理
    audio_codec->EnableOutput(false);
    http->Close();
    
    ESP_LOGI(TAG, "Finished playing MP3");
}