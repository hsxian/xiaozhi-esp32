#include "mp3_music_player.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "media/common/restful_client.h"
#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "esp_wifi.h"

extern "C" {
#include <mp3dec.h>
}

#define TAG "Mp3MusicPlayer"

Mp3MusicPlayer::Mp3MusicPlayer() : audio_codec_(Board::GetInstance().GetAudioCodec()) {
    // 创建MP3数据队列
    mp3_queue_ = xQueueCreate(QUEUE_SIZE, sizeof(Mp3DataChunk));
    if (!mp3_queue_) {
        ESP_LOGE(TAG, "Failed to create MP3 queue");
    }

    auto& state_machine = Application::GetInstance().GetStateMachine();
    listener_id_ = state_machine.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        this->OnStateMachineCallback(old_state, new_state);
    });
}

Mp3MusicPlayer::~Mp3MusicPlayer() {
    auto& state_machine = Application::GetInstance().GetStateMachine();
    state_machine.RemoveStateChangeListener(listener_id_);
    CleanupResources();
    if (mp3_queue_) {
        vQueueDelete(mp3_queue_);
    }
}

bool Mp3MusicPlayer::Play(const Music& music) {
    std::vector<Music> music_list = {music};

    Play(music_list);
    return true;
}

void Mp3MusicPlayer::Play(const std::vector<Music>& music_list) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (IsPlaying()) {
        ESP_LOGW(TAG, "Already playing, stop current first");
        return;
    }

    current_music_list_ = music_list;
    current_control_mode_ = MusicPlayer::PlayControlMode::kUnknown;
    is_playing_ = true;
    current_track_index_ = 0;

    // 创建下载线程和解码播放线程
    xTaskCreate(DownloadTask, "mp3_download_task", 8192, this, 2, &download_task_handle_);
    xTaskCreate(DecodePlayTask, "mp3_decode_task", 8192, this, 5, &decode_task_handle_);

    ESP_LOGI(TAG, "Started playing MP3 %d tracks", current_music_list_.size());
}

// 下载线程
void Mp3MusicPlayer::DownloadTask(void* arg) {
    auto* player = static_cast<Mp3MusicPlayer*>(arg);
    player->DownloadLoop();
    vTaskDelete(nullptr);
}

void Mp3MusicPlayer::DownloadLoop() {
    while (is_playing_) {
        // 检查曲目索引
        if (current_track_index_ < 0 ||
            current_track_index_ >= static_cast<int>(current_music_list_.size())) {
            ESP_LOGI(TAG, "No more tracks to download");
            break;
        }

        WaitPalySattus();

        const auto& music = current_music_list_[current_track_index_];
        ESP_LOGI(TAG, "Starting download for track %d/%d: %s", 1 + current_track_index_,
                 static_cast<int>(current_music_list_.size()), music.ToString().c_str());

        // 准备播放（建立HTTP连接）
        if (!PreparePlayback(music)) {
            // 发送错误信号
            Mp3DataChunk error_chunk;
            error_chunk.is_error = true;
            xQueueSend(mp3_queue_, &error_chunk, portMAX_DELAY);
            continue;
        }

        // 开始流式下载
        is_downloading_ = true;
        std::vector<uint8_t> buffer(BUFFER_SIZE);
        bool connection_closed = false;

        while (is_playing_ && !connection_closed) {
            // 检查控制命令
            if (current_control_mode_ == MusicPlayer::PlayControlMode::kStop) {
                ESP_LOGI(TAG, "Download stopped by user");
                break;
            } else if (current_control_mode_ == MusicPlayer::PlayControlMode::kNext ||
                       current_control_mode_ == MusicPlayer::PlayControlMode::kPrevious) {
                ESP_LOGI(TAG, "Track change requested, stopping download");
                break;
            }

            if (is_paused_) {
                // 暂停时不要消耗流数据，否则会丢弃音频并导致播放失败
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            // 读取数据
            int bytes_read = 0;
            {
                std::lock_guard<std::mutex> lock(http_mutex_);
                if (http_) {
                    bytes_read = http_->Read(reinterpret_cast<char*>(buffer.data()), BUFFER_SIZE);
                }
            }

            if (bytes_read <= 0) {
                int last_err = 0;
                std::string conn_hdr;
                size_t content_length = 0;
                std::string accept_ranges;
                {
                    std::lock_guard<std::mutex> lock(http_mutex_);
                    if (http_) {
                        last_err = http_->GetLastError();
                        conn_hdr = http_->GetResponseHeader("Connection");
                        content_length = http_->GetBodyLength();
                        accept_ranges = http_->GetResponseHeader("Accept-Ranges");
                    }
                }
                ESP_LOGI(TAG,
                         "End of stream or connection closed (bytes_read=%d, last_err=0x%x, "
                         "Connection=%s, received=%u, content_length=%u, accept_ranges=%s)",
                         bytes_read, last_err, conn_hdr.c_str(), (unsigned)download_bytes_received_,
                         (unsigned)content_length, accept_ranges.c_str());

                // 尝试断点续传（如果服务器支持且未下载完整）
                if (content_length > 0 && download_bytes_received_ < content_length &&
                    accept_ranges.find("bytes") != std::string::npos) {
                    const int max_retries = 3;
                    int attempt = 0;
                    bool resumed = false;
                    for (; attempt < max_retries && is_playing_; ++attempt) {
                        ESP_LOGI(TAG, "Attempting resume download, attempt %d, offset=%u",
                                 attempt + 1, (unsigned)download_bytes_received_);
                        // 关闭旧连接
                        {
                            std::lock_guard<std::mutex> lock(http_mutex_);
                            if (http_) {
                                http_->Close();
                                http_.reset();
                            }
                        }

                        vTaskDelay(pdMS_TO_TICKS(200));

                        // 建立新连接并设置 Range 头
                        {
                            std::lock_guard<std::mutex> lock(http_mutex_);
                            http_ = network_->CreateHttp(3);
                            if (!http_) {
                                ESP_LOGE(TAG, "Failed to create HTTP for resume");
                                continue;
                            }
                            ConfigureHttpHeaders(http_.get(), current_url_);
                            http_->SetHeader(
                                "Range", "bytes=" + std::to_string(download_bytes_received_) + "-");
                            if (!http_->Open("GET", current_url_)) {
                                ESP_LOGW(TAG, "Resume Open failed");
                                http_->Close();
                                http_.reset();
                                continue;
                            }
                        }

                        // 如果成功打开，继续读取
                        ESP_LOGI(TAG, "Resume connection opened");
                        resumed = true;
                        break;
                    }

                    if (resumed) {
                        // 继续循环读取
                        continue;
                    }
                }

                connection_closed = true;
                break;
            }

            // 发送数据到队列
            Mp3DataChunk chunk;
            chunk.data = new uint8_t[bytes_read];
            memcpy(chunk.data, buffer.data(), bytes_read);
            chunk.size = bytes_read;
            chunk.is_eos = false;
            chunk.is_error = false;

            UBaseType_t queue_count = uxQueueMessagesWaiting(mp3_queue_);

            // 智能流量控制：根据队列深度动态调整等待时间
            // 当队列深度超过高水位时，主动等待解码线程消费
            if (queue_count >= HIGH_WATER_MARK) {
                // 等待时间与队列深度成正比，指数增长
                int wait_ms = (queue_count - HIGH_WATER_MARK + 1) * 100;
                ESP_LOGD(TAG, "Queue deep (%d/%d), throttling download for %dms", queue_count,
                         QUEUE_SIZE, wait_ms);
                vTaskDelay(pdMS_TO_TICKS(wait_ms));

                // 重新检查队列深度
                queue_count = uxQueueMessagesWaiting(mp3_queue_);
            }

            // 当队列接近满时，使用较长的超时时间
            TickType_t timeout;
            if (queue_count >= CRITICAL_WATER_MARK) {
                // 接近满队列时，等待更长时间让解码线程消费
                timeout = pdMS_TO_TICKS(2000);
            } else if (queue_count >= HIGH_WATER_MARK) {
                timeout = pdMS_TO_TICKS(1000);
            } else {
                timeout = pdMS_TO_TICKS(100);
            }

            BaseType_t send_result = xQueueSend(mp3_queue_, &chunk, timeout);
            if (send_result != pdPASS) {
                // 队列仍然满，改为阻塞式等待（不丢包）
                ESP_LOGW(TAG, "Queue full after %dms wait, blocking until space available",
                         timeout / portTICK_PERIOD_MS);
                // 使用 portMAX_DELAY 无限等待，直到队列有空间
                if (xQueueSend(mp3_queue_, &chunk, portMAX_DELAY) != pdPASS) {
                    // 理论上不会到达这里，除非队列被删除
                    ESP_LOGE(TAG, "Failed to send chunk even with infinite wait!");
                    delete[] chunk.data;
                }
            }

            download_bytes_received_ += (size_t)bytes_read;
            ESP_LOGD(TAG, "Downloaded %d bytes, total received %u, queue space available",
                     bytes_read, (unsigned)download_bytes_received_);
        }

        // 发送EOS信号
        Mp3DataChunk eos_chunk;
        eos_chunk.is_eos = true;
        xQueueSend(mp3_queue_, &eos_chunk, portMAX_DELAY);

        // 关闭HTTP连接
        {
            std::lock_guard<std::mutex> lock(http_mutex_);
            if (http_) {
                http_->Close();
                http_.reset();
            }
        }
        is_downloading_ = false;
        TrySetControlModeToHandled(2);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Download task exiting");
}

// 解码播放线程
void Mp3MusicPlayer::DecodePlayTask(void* arg) {
    auto* player = static_cast<Mp3MusicPlayer*>(arg);
    player->DecodePlayLoop();
    vTaskDelete(nullptr);
}

void Mp3MusicPlayer::DecodePlayLoop() {
    auto& app = Application::GetInstance();
    auto& audio_service = app.GetAudioService();
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    while (is_playing_) {
        // 检查曲目索引
        if (current_track_index_ < 0 ||
            current_track_index_ >= static_cast<int>(current_music_list_.size())) {
            ESP_LOGI(TAG, "No more tracks to play");
            break;
        }

        const auto& music = current_music_list_[current_track_index_];
        ESP_LOGI(TAG, "Playing track %d/%d: %s", 1 + current_track_index_,
                 static_cast<int>(current_music_list_.size()), music.ToString().c_str());

        display->SetChatMessage("music", ("Playing: " + music.ToString()).c_str());

        audio_codec_->SetOutputVolume(std::max(30, audio_codec_->output_volume()));
        audio_codec_->EnableOutput(true);

        // 重置播放进度
        ResetPlaybackProgress();

        // 初始化解码器
        auto decoder = MP3InitDecoder();
        if (!decoder) {
            ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
            audio_codec_->EnableOutput(false);
            ESP_LOGI(TAG, "Decode: Waiting for data from queue...");
            // 等待并处理EOS
            Mp3DataChunk chunk;
            while (xQueueReceive(mp3_queue_, &chunk, portMAX_DELAY) == pdPASS) {
                if (chunk.data)
                    delete[] chunk.data;
                if (chunk.is_eos || chunk.is_error)
                    break;
            }
            continue;
        }

        std::vector<uint8_t> mp3_buffer(BUFFER_SIZE * 2);
        std::vector<int16_t> pcm_buffer(PCM_BUFFER_SIZE / 2);
        size_t mp3_data_offset = 0;
        size_t mp3_data_size = 0;
        int consecutive_skip_count = 0;
        bool track_complete = false;
        bool track_error = false;

        ESP_LOGI(TAG, "Decode: Entering main decode loop");
        while (is_playing_ && !track_complete && !track_error) {
            // 检查暂停
            if (is_paused_) {
                ESP_LOGI(TAG, "Paused, waiting for resume");
                std::unique_lock<std::mutex> lock(mutex_);
                pause_cv_.wait(lock, [this]() {
                    return !is_paused_ ||
                           current_control_mode_ == MusicPlayer::PlayControlMode::kStop;
                });
                ESP_LOGI(TAG, "Resumed or stopped");
            }

            // 检查控制命令
            if (current_control_mode_ == MusicPlayer::PlayControlMode::kStop) {
                ESP_LOGI(TAG, "Decode play stopped by user");
                break;
            }
            if (current_control_mode_ == MusicPlayer::PlayControlMode::kNext ||
                current_control_mode_ == MusicPlayer::PlayControlMode::kPrevious) {
                ESP_LOGI(TAG, "Track change requested");
                break;
            }

            // 等待播放状态
            while (IsNeedWaitDeviceSattus()) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            
            // 从队列获取数据 - 使用较长超时时间确保能获取到数据
            Mp3DataChunk chunk;
            BaseType_t receive_result = xQueueReceive(mp3_queue_, &chunk, pdMS_TO_TICKS(500));

            if (receive_result == pdPASS) {
                if (chunk.is_eos) {
                    ESP_LOGI(TAG, "Received EOS signal");
                    track_complete = true;
                    break;
                }
                if (chunk.is_error) {
                    ESP_LOGE(TAG, "Received error signal");
                    track_error = true;
                    break;
                }

                // 将数据添加到缓冲区。若数据已消费，需要先压缩剩余数据到缓冲区前端。
                if (mp3_data_offset > 0) {
                    if (mp3_data_size > 0) {
                        memmove(mp3_buffer.data(), mp3_buffer.data() + mp3_data_offset,
                                mp3_data_size);
                    }
                    mp3_data_offset = 0;
                }

                if (mp3_data_size + chunk.size > mp3_buffer.size()) {
                    mp3_buffer.resize(mp3_data_size + chunk.size + BUFFER_SIZE);
                }
                memcpy(mp3_buffer.data() + mp3_data_size, chunk.data, chunk.size);
                mp3_data_size += chunk.size;
                delete[] chunk.data;

                ESP_LOGD(TAG, "Received %d bytes, total buffered: %u", (int)chunk.size,
                         (unsigned int)mp3_data_size);
            } else {
                // 队列超时，检查是否真的没有数据
                UBaseType_t queue_count = uxQueueMessagesWaiting(mp3_queue_);
                if (queue_count > 0) {
                    ESP_LOGW(TAG, "Queue has %d items but receive timed out!", queue_count);
                }
                // 缓冲区数据不足时继续等待，不要跳过
                if (mp3_data_size < 512) {
                    ESP_LOGD(TAG, "Waiting for more data, buffered: %u",
                             (unsigned int)mp3_data_size);
                    continue;
                }
            }

            audio_service.UpdateLastOutputTime();
            if (false == audio_codec_->output_enabled()) {
                audio_codec_->EnableOutput(true);
            }

            // 解码播放循环：持续解码直到缓冲区数据不足
            while (mp3_data_size >= 512 && is_playing_ && !is_paused_) {
                if (!DecodeAndPlayFrame(decoder, mp3_buffer, mp3_data_offset, mp3_data_size,
                                        pcm_buffer, consecutive_skip_count)) {
                    ESP_LOGW(TAG, "Decode failed, skipping");
                    break;
                }

                // 解码过程中尝试预取下一批数据，提高效率
                Mp3DataChunk next_chunk;
                if (xQueueReceive(mp3_queue_, &next_chunk, 0) == pdPASS) {
                    if (next_chunk.is_eos) {
                        ESP_LOGI(TAG, "Received EOS during decode");
                        track_complete = true;
                        break;
                    }
                    if (next_chunk.is_error) {
                        ESP_LOGE(TAG, "Received error during decode");
                        track_error = true;
                        break;
                    }
                    // 压缩缓冲区并添加新数据
                    if (mp3_data_offset > 0) {
                        if (mp3_data_size > 0) {
                            memmove(mp3_buffer.data(), mp3_buffer.data() + mp3_data_offset,
                                    mp3_data_size);
                        }
                        mp3_data_offset = 0;
                    }
                    if (mp3_data_size + next_chunk.size > mp3_buffer.size()) {
                        mp3_buffer.resize(mp3_data_size + next_chunk.size + BUFFER_SIZE);
                    }
                    memcpy(mp3_buffer.data() + mp3_data_size, next_chunk.data, next_chunk.size);
                    mp3_data_size += next_chunk.size;
                    delete[] next_chunk.data;
                    ESP_LOGD(TAG, "Prefetched %d bytes during decode, total buffered: %u",
                             (int)next_chunk.size, (unsigned int)mp3_data_size);
                }
            }
        }

        // 清理解码器
        MP3FreeDecoder(decoder);
        audio_codec_->EnableOutput(false);

        // 清空队列中剩余数据
        Mp3DataChunk chunk;
        while (xQueueReceive(mp3_queue_, &chunk, 0) == pdPASS) {
            if (chunk.data)
                delete[] chunk.data;
            if (chunk.is_eos)
                break;
        }

        // 更新曲目索引
        if (current_control_mode_ == MusicPlayer::PlayControlMode::kPrevious) {
            if (TrySetControlModeToHandled(1)) {
                current_track_index_ = std::max(0, current_track_index_--);
                display->SetChatMessage("music", "Previous track");
            }
        } else if (current_control_mode_ == MusicPlayer::PlayControlMode::kStop) {
            display->SetChatMessage("music", "Stop");
            break;
        } else {
            // 正常结束，进入下一首
            if (TrySetControlModeToHandled(1)) {
                current_track_index_++;
                display->SetChatMessage("music", "Next track");
            }
        }
    }

    is_playing_ = false;
    current_control_mode_ = MusicPlayer::PlayControlMode::kUnknown;
    display->SetChatMessage("music", "Stopped");
    ESP_LOGI(TAG, "Decode play task exiting");
}

bool Mp3MusicPlayer::IsPlaying() const { return is_playing_; }

bool Mp3MusicPlayer::PreparePlayback(const Music& music) {
    auto& board = Board::GetInstance();
    network_ = board.GetNetwork();

    if (!network_) {
        ESP_LOGE(TAG, "Network not available");
        return false;
    }

    RestfulClient restful_client(3);
    auto url = restful_client.NormalizeUrl(music.url);
    if (url.empty()) {
        ESP_LOGE(TAG, "URL is empty");
        return false;
    }

    std::string redirect_url;
    ESP_LOGI(TAG, "Checking for redirect URL for: %s", url.c_str());
    restful_client.TryGetRedirectUrl(url, redirect_url);
    if (!redirect_url.empty()) {
        url = redirect_url;
    }
    ESP_LOGI(TAG, "Final URL: %s", url.c_str());

    std::lock_guard<std::mutex> lock(http_mutex_);
    esp_wifi_set_ps(WIFI_PS_NONE);
    http_ = network_->CreateHttp(3);
    current_url_ = url;
    download_bytes_received_ = 0;
    if (!http_) {
        ESP_LOGE(TAG, "Failed to create HTTP client for: %s", url.c_str());
        return false;
    }

    ConfigureHttpHeaders(http_.get(), url);
    if (!http_->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection for: %s", url.c_str());
        http_->Close();
        http_.reset();
        return false;
    }

    int status_code = http_->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed, status code: %d", status_code);
        http_->Close();
        http_.reset();
        return false;
    }

    ESP_LOGI(TAG, "HTTP connection established successfully");
    // 记录部分响应头，便于排查服务器是否立即关闭连接或使用 chunked 编码
    {
        std::string cl = http_->GetResponseHeader("Content-Length");
        std::string te = http_->GetResponseHeader("Transfer-Encoding");
        ESP_LOGI(TAG, "Response headers: Content-Length=%s Transfer-Encoding=%s", cl.c_str(),
                 te.c_str());
    }
    current_control_mode_ = MusicPlayer::PlayControlMode::kControlHandled;
    return true;
}

void Mp3MusicPlayer::ConfigureHttpHeaders(Http* http, const std::string& url) {
    http->SetHeader("Accept", "audio/mpeg, audio/x-mpeg, audio/x-mpeg-3, audio/mpeg3");
    http->SetHeader("Connection", "keep-alive");
    http->SetHeader("user-agent",
                    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
                    "Chrome/148.0.0.0 Safari/537.36");
    http->SetHeader("Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8");
    http->SetHeader("Accept-Encoding", "identity");  // 不压缩，直接传输
    http->SetHeader("Referer", url.c_str());
}

void Mp3MusicPlayer::SkipId3Tag(std::vector<uint8_t>& mp3_buffer, size_t& mp3_data_size,
                                size_t& mp3_data_offset) {
    if (mp3_data_size < 10 || mp3_buffer[0] != 'I' || mp3_buffer[1] != 'D' ||
        mp3_buffer[2] != '3') {
        return;
    }

    uint32_t tag_size = ((uint32_t)(mp3_buffer[6] & 0x7F) << 21) |
                        ((uint32_t)(mp3_buffer[7] & 0x7F) << 14) |
                        ((uint32_t)(mp3_buffer[8] & 0x7F) << 7) | (uint32_t)(mp3_buffer[9] & 0x7F);
    tag_size += 10;
    ESP_LOGI(TAG, "Found ID3v2 tag, skipping %u bytes", tag_size);

    if (tag_size < mp3_data_size) {
        mp3_data_size -= tag_size;
        memmove(mp3_buffer.data(), mp3_buffer.data() + tag_size, mp3_data_size);
        mp3_data_offset = 0;
        ESP_LOGI(TAG, "After ID3 skip, first 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                 mp3_buffer[0], mp3_buffer[1], mp3_buffer[2], mp3_buffer[3], mp3_buffer[4],
                 mp3_buffer[5], mp3_buffer[6], mp3_buffer[7]);
    } else {
        ESP_LOGW(TAG, "ID3 tag larger than buffer, discarding buffer");
        mp3_data_offset = 0;
        mp3_data_size = 0;
    }
}

void Mp3MusicPlayer::CleanupResources() {
    is_playing_ = false;
    is_paused_ = false;
    pause_cv_.notify_all();

    // 删除任务
    if (download_task_handle_) {
        vTaskDelete(download_task_handle_);
        download_task_handle_ = nullptr;
    }
    if (decode_task_handle_) {
        vTaskDelete(decode_task_handle_);
        decode_task_handle_ = nullptr;
    }

    // 关闭HTTP连接
    {
        std::lock_guard<std::mutex> lock(http_mutex_);
        if (http_) {
            http_->Close();
            http_.reset();
        }
    }

    // 清空队列
    Mp3DataChunk chunk;
    while (xQueueReceive(mp3_queue_, &chunk, 0) == pdPASS) {
        if (chunk.data)
            delete[] chunk.data;
    }

    network_ = nullptr;
    current_track_index_ = 0;
    current_music_list_.clear();
}

void Mp3MusicPlayer::ConvertPcmIfNeeded(const MP3FrameInfo& frame_info,
                                        const std::vector<int16_t>& pcm_buffer,
                                        std::vector<int16_t>& output_pcm, int& output_samples,
                                        int& output_channels) {
    int codec_output_rate = audio_codec_->output_sample_rate();
    int codec_output_channels = audio_codec_->output_channels();
    int input_rate = frame_info.samprate;
    int input_channels = frame_info.nChans;
    int input_frames = frame_info.outputSamps / input_channels;

    bool need_rate_conversion = (input_rate != codec_output_rate && codec_output_rate > 0);
    bool need_channel_conversion =
        (input_channels != codec_output_channels && codec_output_channels > 0);

    output_samples = frame_info.outputSamps;
    output_channels = input_channels;

    if (!need_rate_conversion && !need_channel_conversion) {
        return;
    }

    output_channels = codec_output_channels;
    int output_frames = input_frames;
    if (need_rate_conversion) {
        output_frames = (int)((int64_t)input_frames * codec_output_rate / input_rate + 0.5);
        if (output_frames < 1) {
            output_frames = 1;
        }
    }

    output_samples = output_frames * output_channels;
    output_pcm.resize(output_samples);
    ESP_LOGD(TAG, "Resampling/downmixing %dHz %d-ch -> %dHz %d-ch", input_rate, input_channels,
             codec_output_rate, codec_output_channels);

    uint64_t step =
        need_rate_conversion ? (((uint64_t)input_rate << 32) / codec_output_rate) : (1ull << 32);
    uint64_t pos = 0;

    for (int f = 0; f < output_frames; ++f) {
        uint32_t idx = pos >> 32;
        uint32_t frac = pos & 0xFFFFFFFFu;
        if (idx >= (uint32_t)input_frames - 1) {
            idx = input_frames - 1;
        }

        if (input_channels == 2 && output_channels == 1) {
            int32_t s0 = ((int32_t)pcm_buffer[idx * 2] + (int32_t)pcm_buffer[idx * 2 + 1]) / 2;
            int32_t s1 = s0;
            if (idx + 1 < (size_t)input_frames) {
                s1 = ((int32_t)pcm_buffer[(idx + 1) * 2] + (int32_t)pcm_buffer[(idx + 1) * 2 + 1]) /
                     2;
            }
            output_pcm[f] = (int16_t)(s0 + ((int64_t)(s1 - s0) * frac >> 32));
        } else if (input_channels == 1 && output_channels == 2) {
            int16_t sample = pcm_buffer[idx];
            output_pcm[f * 2] = sample;
            output_pcm[f * 2 + 1] = sample;
        } else {
            for (int c = 0; c < output_channels; ++c) {
                int ch = c < input_channels ? c : (input_channels - 1);
                int32_t s0 = pcm_buffer[idx * input_channels + ch];
                int32_t s1 = s0;
                if (idx + 1 < (size_t)input_frames) {
                    s1 = pcm_buffer[(idx + 1) * input_channels + ch];
                }
                output_pcm[f * output_channels + c] =
                    (int16_t)(s0 + ((int64_t)(s1 - s0) * frac >> 32));
            }
        }

        pos += step;
    }
}

bool Mp3MusicPlayer::DecodeAndPlayFrame(HMP3Decoder decoder, std::vector<uint8_t>& mp3_buffer,
                                        size_t& mp3_data_offset, size_t& mp3_data_size,
                                        std::vector<int16_t>& pcm_buffer,
                                        int& consecutive_skip_count) {
    // 在每次从头开始解码时跳过ID3标签
    if (mp3_data_offset == 0) {
        SkipId3Tag(mp3_buffer, mp3_data_size, mp3_data_offset);
    }

    // 搜索有效帧头 - 每次失败后都重新搜索，避免卡在无效位置
    int sync_offset = MP3FindSyncWord(mp3_buffer.data() + mp3_data_offset, (int)mp3_data_size);
    if (sync_offset > 0) {
        mp3_data_offset += (size_t)sync_offset;
        mp3_data_size -= (size_t)sync_offset;
        ESP_LOGI(TAG, "Found valid MP3 frame at offset %d, remaining: %u", sync_offset,
                 (unsigned int)mp3_data_size);
    } else if (sync_offset < 0 && mp3_data_size > 0) {
        // 没有找到帧头，跳过一个字节
        mp3_data_offset++;
        mp3_data_size--;
        consecutive_skip_count++;
        if (consecutive_skip_count % 50 == 0) {
            ESP_LOGW(TAG, "No valid frame found, skipping bytes (total skipped: %d)",
                     consecutive_skip_count);
        }
        if (consecutive_skip_count >= MAX_CONSECUTIVE_SKIPS) {
            ESP_LOGE(TAG, "Too many consecutive frame skips (%d), stopping playback of this track",
                     consecutive_skip_count);
            return false;
        }
        return true;
    }

    // MP3帧最小约为24字节（帧头4字节 + 最小数据），要求至少有128字节确保有完整帧
    const int MIN_FRAME_SIZE = 128;
    if (mp3_data_size < MIN_FRAME_SIZE) {
        ESP_LOGD(TAG, "Not enough data to decode (size: %u, need: %d), waiting for more",
                 (unsigned int)mp3_data_size, MIN_FRAME_SIZE);
        return true;
    }

    unsigned char* pInData = const_cast<unsigned char*>(mp3_buffer.data() + mp3_data_offset);
    int nBytesLeft = mp3_data_size;
    int samples_decoded = MP3Decode(decoder, &pInData, &nBytesLeft, pcm_buffer.data(), 0);

    if (samples_decoded < 0) {
        if (samples_decoded == ERR_MP3_INDATA_UNDERFLOW) {
            ESP_LOGD(TAG, "Need more data for MP3 decoding, current buffered: %u",
                     (unsigned int)mp3_data_size);
            consecutive_skip_count = 0;
            return true;
        }

        ESP_LOGW(TAG, "MP3Decode failed with error code: %d, skipping 1 byte", samples_decoded);

        // 解码失败，跳过一个字节并重新搜索
        if (mp3_data_size > 0) {
            mp3_data_offset++;
            mp3_data_size--;
        }
        consecutive_skip_count++;

        if (consecutive_skip_count % 20 == 0) {
            ESP_LOGW(TAG, "Decode failures accumulating (total: %d/%d)", consecutive_skip_count,
                     MAX_CONSECUTIVE_SKIPS);
        }
        if (consecutive_skip_count >= MAX_CONSECUTIVE_SKIPS) {
            ESP_LOGE(TAG,
                     "Too many consecutive decode failures (%d), stopping playback of this track",
                     consecutive_skip_count);
            return false;
        }
        return true;
    }

    consecutive_skip_count = 0;

    MP3FrameInfo frame_info;
    MP3GetNextFrameInfo(decoder, &frame_info, pInData);
    int bytes_consumed = mp3_data_size - nBytesLeft;
    mp3_data_offset += bytes_consumed;
    mp3_data_size = nBytesLeft;
    ESP_LOGD(TAG, "Decoded %d samples, consumed %d bytes, remaining: %u", frame_info.outputSamps,
             bytes_consumed, (unsigned int)mp3_data_size);

    if (frame_info.outputSamps <= 0) {
        return true;
    }

    int codec_output_rate = audio_codec_->output_sample_rate();
    int output_samples = frame_info.outputSamps;
    int output_channels = frame_info.nChans;
    std::vector<int16_t> output_pcm;

    ConvertPcmIfNeeded(frame_info, pcm_buffer, output_pcm, output_samples, output_channels);

    // 更新播放进度current_position_ms_和total_duration_ms_
    UpdateTimeInfo(codec_output_rate, output_samples, output_channels, frame_info);

    int32_t current_ms = current_position_ms_.load();
    int32_t total_ms = total_duration_ms_.load();
    if (total_ms > 0 && current_ms > total_ms) {
        current_position_ms_ = total_ms;
    }

    static time_t time_flag = time(nullptr);

    if (time(nullptr) - time_flag > 10) {
        time_flag = time(nullptr);
        ESP_LOGI(TAG, "Current position: %1f s, total duration: %1f s",
                 current_position_ms_.load() / 1000.0f, total_duration_ms_.load() / 1000.0f);
    }

    if (output_samples > 0) {
        if (output_pcm.empty()) {
            audio_codec_->OutputData(pcm_buffer.data(), output_samples);
        } else {
            audio_codec_->OutputData(output_pcm.data(), output_samples);
        }
    }

    ESP_LOGD(TAG,
             "Decoded %d samples, %d channels, bitrate: %d kbps, samplerate: %d -> output "
             "%d samples, %d channels, %d Hz",
             frame_info.outputSamps, frame_info.nChans, frame_info.bitrate / 1000,
             frame_info.samprate, output_samples, output_channels, codec_output_rate);

    return true;
}
void Mp3MusicPlayer::UpdateTimeInfo(int codec_output_rate, int output_samples, int output_channels,
                                    const MP3FrameInfo& frame_info) {
    if (output_samples > 0 && output_channels > 0) {
        int output_frames = output_samples / output_channels;
        int sample_rate = codec_output_rate > 0 ? codec_output_rate : frame_info.samprate;
        if (output_frames > 0 && sample_rate > 0) {
            int32_t delta_ms = (int32_t)((int64_t)output_frames * 1000 / sample_rate);
            current_position_ms_.fetch_add(delta_ms);
        }
    }

    if (total_duration_ms_.load() == 0 && frame_info.bitrate > 0) {
        if (http_) {
            size_t content_length = http_->GetBodyLength();
            if (content_length > 0) {
                int32_t total_ms = (int32_t)((uint64_t)content_length * 8000 / frame_info.bitrate);
                if (total_ms > 0) {
                    total_duration_ms_ = total_ms;
                }
            }
        }
    }
}

bool Mp3MusicPlayer::CanChangePlayControlMode(const PlayControlMode& mode) {
    if (mode == current_control_mode_) {
        return false;
    }
    if (current_control_mode_ == PlayControlMode::kUnknown) {
        return false;
    }
    if (current_control_mode_ == PlayControlMode::kResume && mode == PlayControlMode::kPause) {
        return false;
    }
    return true;
}

bool Mp3MusicPlayer::ChangePlayControlMode(const PlayControlMode& mode) {
    if (!CanChangePlayControlMode(mode)) {
        ESP_LOGI(TAG, "Rejecting mode %d, previous control mode %d still pending", (int)mode,
                 (int)current_control_mode_.load());
        return false;
    }
    ESP_LOGI(TAG, "ChangePlayControlMode current_control_mode_ %d mode %d",
             (int)current_control_mode_.load(), (int)mode);

    ResetHandledTaskListFlag();
    auto ret = true;
    switch (mode) {
        case PlayControlMode::kPause:
            is_paused_ = true;
            break;

        case PlayControlMode::kResume:
        case PlayControlMode::kStop:
        case PlayControlMode::kNext:
        case PlayControlMode::kPrevious:
            is_paused_ = false;
            pause_cv_.notify_all();
            break;
        default:
            break;
    }
    current_control_mode_ = mode;
    return ret;
}

void Mp3MusicPlayer::OnStateMachineCallback(DeviceState old_state, DeviceState new_state) {
    if (!IsPlaying()) {
        return;
    }
    
    // ESP_LOGI(TAG, "OnStateMachineCallback old_state %d new_state %d", (int)old_state, (int)new_state);
    if (old_state == kDeviceStateIdle) {
        audio_codec_->EnableOutput(false);
        ChangePlayControlMode(PlayControlMode::kPause);
        audio_codec_->EnableOutput(true);
    }
}

bool Mp3MusicPlayer::TrySetControlModeToHandled(int task_flag) {
    if (current_control_mode_ == PlayControlMode::kControlHandled) {
        return false;
    }
    handled_task_list_flag_ |= task_flag;
    while (handled_task_list_flag_.load() != TASK_LIST_FLAG) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    current_control_mode_ = PlayControlMode::kControlHandled;
    ESP_LOGI(TAG, "TrySetControlModeToHandled current_control_mode to  kControlHandled");
    return true;
}
void Mp3MusicPlayer::ResetHandledTaskListFlag() { handled_task_list_flag_ = 0; }

void Mp3MusicPlayer::WaitPalySattus() {
    while (current_control_mode_ != MusicPlayer::PlayControlMode::kControlHandled &&
           current_control_mode_ != MusicPlayer::PlayControlMode::kUnknown) {
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}