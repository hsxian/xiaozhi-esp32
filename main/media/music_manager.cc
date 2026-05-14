#include "music_manager.h"
#include "board.h"
#include "audio_codec.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

extern "C" {
#include <mp3dec.h>
}

#define TAG "MusicManager"
#define BUFFER_SIZE (4 * 1024)  // 4KB缓冲区
#define PCM_BUFFER_SIZE (8 * 1024)  // PCM输出缓冲区（增大以适应解码输出）

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
    xTaskCreate(PlayTask, "mp3_play_task", 8192, this, 15, nullptr);

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
    const int volume = 30;  // 可以改为参数

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

    // 设置必要的请求头（支持流式传输，模拟浏览器请求）
    http->SetHeader("Accept", "audio/mpeg, audio/x-mpeg, audio/x-mpeg-3, audio/mpeg3");
    http->SetHeader("Connection", "keep-alive");
    http->SetHeader("user-agent",
                    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
                    "Chrome/148.0.0.0 Safari/537.36");
    http->SetHeader("Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8");
    http->SetHeader("Accept-Encoding", "identity");  // 不压缩，直接传输
    http->SetHeader("Referer", url.c_str());

    // 获取响应状态码并处理重定向
    int status_code = http->GetStatusCode();
    int redirect_count = 0;
    const int max_redirects = 5;
    std::string redirect_url = "";
    while (
        (status_code == 301 || status_code == 302 || status_code == 307 || status_code == 308) &&
        redirect_count < max_redirects) {
        redirect_url = http->GetResponseHeader("Location");
        if (redirect_url.empty()) {
            ESP_LOGE(TAG, "Redirect response has no Location header");
            http->Close();
            return;
        }

        ESP_LOGI(TAG, "Redirecting to: %s (status: %d)", redirect_url.c_str(), status_code);
        http->Close();
        
        // 使用重定向URL重新建立连接
        http = network->CreateHttp(30);
        if (!http->Open("GET", redirect_url)) {
            ESP_LOGE(TAG, "Failed to open HTTP connection for redirect: %s", redirect_url.c_str());
            http->Close();
            return;
        }
        
        status_code = http->GetStatusCode();
        redirect_count++;
    }

    if (redirect_count >= max_redirects) {
        ESP_LOGE(TAG, "Too many redirects (max: %d)", max_redirects);
        http->Close();
        return;
    }
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed, status code: %d", status_code);
        http->Close();
        return;
    }
    
    ESP_LOGI(TAG, "HTTP request succeeded with status 200, redirects: %d", redirect_count);
    
    // 检查HTTP响应头，确认传输编码
    std::string content_type = http->GetResponseHeader("Content-Type");
    std::string transfer_encoding = http->GetResponseHeader("Transfer-Encoding");
    std::string content_length = http->GetResponseHeader("Content-Length");
    ESP_LOGI(TAG, "Response headers - Content-Type: %s, Transfer-Encoding: %s, Content-Length: %s", 
             content_type.c_str(), transfer_encoding.c_str(), content_length.c_str());
    
    ESP_LOGI(TAG, "Starting MP3 playback with volume: %d", volume);

    // 设置音量
    audio_codec->SetOutputVolume(volume);
    audio_codec->EnableOutput(true);

    // 分配缓冲区
    std::vector<uint8_t> mp3_buffer(BUFFER_SIZE);
    std::vector<int16_t> pcm_buffer(PCM_BUFFER_SIZE / 2);  // 16-bit PCM

    // 初始化Helix MP3解码器
    HMP3Decoder decoder = MP3InitDecoder();
    if (!decoder) {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        audio_codec->EnableOutput(false);
        http->Close();
        return;
    }

    ESP_LOGI(TAG, "MP3 decoder initialized successfully");

    size_t mp3_data_offset = 0;
    size_t mp3_data_size = 0;
    MP3FrameInfo frame_info;
    
    // 读取初始数据用于检查ID3标签
    int initial_read = http->Read(reinterpret_cast<char*>(mp3_buffer.data()), BUFFER_SIZE);
    if (initial_read <= 0) {
        ESP_LOGE(TAG, "Failed to read initial data");
        MP3FreeDecoder(decoder);
        audio_codec->EnableOutput(false);
        http->Close();
        return;
    }
    mp3_data_size = initial_read;
    ESP_LOGI(TAG, "Initial read: %d bytes, buffer size: %d", initial_read, BUFFER_SIZE);
    
    // 检查数据是否以有效MP3帧头或ID3标签开头
    ESP_LOGI(TAG, "Initial data first 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
             mp3_buffer[0], mp3_buffer[1], mp3_buffer[2], mp3_buffer[3],
             mp3_buffer[4], mp3_buffer[5], mp3_buffer[6], mp3_buffer[7]);
    
    // 检查并跳过ID3v2标签
    if (mp3_data_size >= 10 && mp3_buffer[0] == 'I' && mp3_buffer[1] == 'D' && mp3_buffer[2] == '3') {
        // ID3v2标签格式：3字节标识 + 2字节版本 + 1字节标志 + 4字节大小
        // 大小是同步安全整数（最高位为0）
        uint32_t tag_size = ((uint32_t)(mp3_buffer[6] & 0x7F) << 21) | 
                           ((uint32_t)(mp3_buffer[7] & 0x7F) << 14) | 
                           ((uint32_t)(mp3_buffer[8] & 0x7F) << 7) | 
                           (uint32_t)(mp3_buffer[9] & 0x7F);
        tag_size += 10;  // 加上标签头本身的10字节
        ESP_LOGI(TAG, "Found ID3v2 tag, skipping %u bytes", tag_size);
        
        if (tag_size < mp3_data_size) {
            mp3_data_size -= tag_size;
            memmove(mp3_buffer.data(), mp3_buffer.data() + tag_size, mp3_data_size);
            mp3_data_offset = 0;  // memmove后数据在缓冲区开头，offset设为0
            ESP_LOGI(TAG, "After ID3 skip, first 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                     mp3_buffer[0], mp3_buffer[1], mp3_buffer[2], mp3_buffer[3],
                     mp3_buffer[4], mp3_buffer[5], mp3_buffer[6], mp3_buffer[7]);
        } else {
            ESP_LOGW(TAG, "ID3 tag larger than buffer, discarding buffer");
            mp3_data_offset = 0;
            mp3_data_size = 0;
        }
    }
    
    // 检查是否为有效MP3数据（帧头以FF开头）
    if (mp3_data_size >= 2 && (mp3_buffer[0] & 0xFF) != 0xFF) {
        ESP_LOGW(TAG, "Warning: Data does not start with MP3 frame header (0x%02X%02X)", 
                 mp3_buffer[0], mp3_buffer[1]);
    }

    // 读取并播放MP3数据流
    while (!stop_requested_) {
        // 如果缓冲区中剩余数据不足，从HTTP读取更多数据
        if (mp3_data_size < BUFFER_SIZE / 2) {
            // 将剩余数据移到缓冲区开头
            if (mp3_data_offset > 0 && mp3_data_size > 0) {
                memmove(mp3_buffer.data(), mp3_buffer.data() + mp3_data_offset, mp3_data_size);
            }
            mp3_data_offset = 0;

            // 读取新数据
            int bytes_read = http->Read(reinterpret_cast<char*>(mp3_buffer.data() + mp3_data_size), 
                                        BUFFER_SIZE - mp3_data_size);
            
            if (bytes_read <= 0) {
                ESP_LOGI(TAG, "End of stream or connection closed");
                break;
            }

            mp3_data_size += bytes_read;
            ESP_LOGD(TAG, "Read %d bytes, total buffered: %u", bytes_read,
                     (unsigned int)mp3_data_size);

            // 检查新读取的数据是否有效（非零）
            int zero_count = 0;
            for (int i = 0; i < bytes_read && i < 16; i++) {
                if (mp3_buffer[mp3_data_size - bytes_read + i] == 0) zero_count++;
            }
            if (zero_count == 16 && bytes_read >= 16) {
                ESP_LOGW(TAG, "Warning: Last read contains all zeros!");
            }
            
            // 调试：打印当前解码位置的前16字节十六进制
            // if (mp3_data_size >= 16) {
            //     size_t pos = mp3_data_offset;
            //     ESP_LOGD(TAG, "Buffer at offset %u: %02X %02X %02X %02X %02X %02X %02X %02X "
            //                  "%02X %02X %02X %02X %02X %02X %02X %02X",
            //              (unsigned int)pos,
            //              mp3_buffer[pos], mp3_buffer[pos+1], mp3_buffer[pos+2], mp3_buffer[pos+3],
            //              mp3_buffer[pos+4], mp3_buffer[pos+5], mp3_buffer[pos+6], mp3_buffer[pos+7],
            //              mp3_buffer[pos+8], mp3_buffer[pos+9], mp3_buffer[pos+10], mp3_buffer[pos+11],
            //              mp3_buffer[pos+12], mp3_buffer[pos+13], mp3_buffer[pos+14], mp3_buffer[pos+15]);
            // }
        }

        // 确保有足够的数据解码
        if (mp3_data_size < 512) {
            continue;
        }

        // 使用Helix解码器解码MP3帧（解码器会自动寻找帧同步）
        unsigned char* pInData = const_cast<unsigned char*>(mp3_buffer.data() + mp3_data_offset);
        int nBytesLeft = mp3_data_size;
        int samples_decoded = MP3Decode(decoder, 
                                        &pInData, 
                                        &nBytesLeft,
                                        pcm_buffer.data(),
                                        0);

        if (samples_decoded < 0) {
            if (samples_decoded == -4) {  // ERR_MP3_NEED_MORE
                ESP_LOGI(TAG, "Need more data for MP3 decoding, current buffered: %u", (unsigned int)mp3_data_size);
                continue;  // Wait for more data
            } else {
                ESP_LOGW(TAG, "MP3Decode failed with error code: %d", samples_decoded);
                // 解码失败，寻找下一个可能的帧头（以0xFF开头，且第二字节高3位为111）
                size_t original_offset = mp3_data_offset;
                bool found = false;
                while (mp3_data_offset < mp3_data_size - 1) {
                    if (mp3_buffer[mp3_data_offset] == 0xFF && 
                        (mp3_buffer[mp3_data_offset + 1] & 0xE0) == 0xE0) {
                        found = true;
                        break;
                    }
                    mp3_data_offset++;
                    mp3_data_size--;
                }
                if (found) {
                    ESP_LOGW(TAG, "Resynced, skipped %d bytes to find frame at offset %u", 
                             (int)(mp3_data_offset - original_offset), (unsigned int)mp3_data_offset);
                } else {
                    ESP_LOGW(TAG, "No valid frame found, skipping 1 byte");
                    mp3_data_offset++;
                    mp3_data_size--;
                }
                continue;
            }
        }
        
        // 获取帧信息用于输出
        MP3GetNextFrameInfo(decoder, &frame_info, pInData);

        // 更新缓冲区偏移（MP3Decode会更新pInData和nBytesLeft）
        int bytes_consumed = mp3_data_size - nBytesLeft;
        mp3_data_offset += bytes_consumed;
        mp3_data_size = nBytesLeft;
        ESP_LOGD(TAG, "Decoded %d samples, consumed %d bytes, remaining: %u", 
                 frame_info.outputSamps, bytes_consumed, (unsigned int)mp3_data_size);

        if (frame_info.outputSamps > 0 && !stop_requested_) {
            int codec_output_rate = audio_codec->output_sample_rate();
            int codec_output_channels = audio_codec->output_channels();
            int input_rate = frame_info.samprate;
            int input_channels = frame_info.nChans;
            int input_frames = frame_info.outputSamps / input_channels;
            int output_frames = input_frames;
            bool need_rate_conversion = (input_rate != codec_output_rate && codec_output_rate > 0);
            bool need_channel_conversion = (input_channels != codec_output_channels && codec_output_channels > 0);

            std::vector<int16_t> output_pcm;
            int output_samples = frame_info.outputSamps;
            int output_channels = input_channels;

            if (need_rate_conversion || need_channel_conversion) {
                output_channels = codec_output_channels;
                if (need_rate_conversion) {
                    output_frames = (int)((int64_t)input_frames * codec_output_rate / input_rate + 0.5);
                    if (output_frames < 1) {
                        output_frames = 1;
                    }
                }
                output_samples = output_frames * output_channels;
                output_pcm.resize(output_samples);

                ESP_LOGD(TAG, "Resampling/downmixing %dHz %d-ch -> %dHz %d-ch", input_rate,
                         input_channels, codec_output_rate, codec_output_channels);

                uint64_t step = need_rate_conversion ? (((uint64_t)input_rate << 32) / codec_output_rate) : (1ull << 32);
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
                            s1 = ((int32_t)pcm_buffer[(idx + 1) * 2] + (int32_t)pcm_buffer[(idx + 1) * 2 + 1]) / 2;
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
                            output_pcm[f * output_channels + c] = (int16_t)(s0 + ((int64_t)(s1 - s0) * frac >> 32));
                        }
                    }
                    pos += step;
                }
            }

            if (need_rate_conversion || need_channel_conversion) {
                audio_codec->OutputData(output_pcm.data(), output_samples);
            } else {
                audio_codec->OutputData(pcm_buffer.data(), output_samples);
            }
            ESP_LOGD(TAG, "Decoded %d samples, %d channels, bitrate: %d kbps, samplerate: %d -> output %d samples, %d channels, %d Hz", 
                     frame_info.outputSamps, frame_info.nChans, frame_info.bitrate / 1000, frame_info.samprate,
                     output_samples, output_channels, codec_output_rate);
        }

        // 如果缓冲区数据太少，继续读取
        if (mp3_data_size < 1024) {
            continue;
        }
    }

    // 清理
    MP3FreeDecoder(decoder);
    audio_codec->EnableOutput(false);
    http->Close();
    
    ESP_LOGI(TAG, "Finished playing MP3");
}