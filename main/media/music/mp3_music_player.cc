#include "mp3_music_player.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "../restful_client.h"
#include "application.h"
#include "audio_codec.h"
#include "board.h"

#define TAG "Mp3MusicPlayer"
#define BUFFER_SIZE (4 * 1024)      // 4KB缓冲区
#define PCM_BUFFER_SIZE (8 * 1024)  // PCM输出缓冲区（增大以适应解码输出）

extern "C" {
#include <mp3dec.h>
}

Mp3MusicPlayer::Mp3MusicPlayer() {}
Mp3MusicPlayer::~Mp3MusicPlayer() {}

bool Mp3MusicPlayer::Play(const Music& music) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (IsPlaying()) {
        ESP_LOGW(TAG, "Already playing, stop current first");
        return false;
    }
    current_music_list_.push_back(music);
    current_control_mode_ = MusicPlayer::PlayControlMode::kUnknown;
    is_playing_ = true;

    // 创建播放任务
    xTaskCreate(PlayTask, "mp3_play_task", 8192, this, 15, nullptr);

    ESP_LOGI(TAG, "Started playing MP3 %d tracks", current_music_list_.size());
    return true;
}

void Mp3MusicPlayer::PlayTask(void* arg) {
    auto* player = static_cast<Mp3MusicPlayer*>(arg);

    while (player->IsNeedWaitPalySattus()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    int total_tracks = player->current_music_list_.size();
    for (int track_index = 0; track_index >= 0 && track_index < total_tracks; track_index++) {
        const auto& music = player->current_music_list_[track_index];
        if (player->current_control_mode_ == MusicPlayer::PlayControlMode::kStop) {
            ESP_LOGI(TAG, "Stop requested, exiting play loop");
            player->current_control_mode_ = MusicPlayer::PlayControlMode::kControlHandled;
            break;
        } else if (player->current_control_mode_ == MusicPlayer::PlayControlMode::kNext) {
            player->current_control_mode_ = MusicPlayer::PlayControlMode::kControlHandled;
            ESP_LOGI(TAG, "Next requested, skipping to next track");
            continue;
        } else if (player->current_control_mode_ == MusicPlayer::PlayControlMode::kPrevious) {
            player->current_control_mode_ = MusicPlayer::PlayControlMode::kControlHandled;
            ESP_LOGI(TAG, "Previous requested, skipping to previous track");
            track_index = std::max(-1, track_index - 2);  // -2因为循环会加1
            continue;
        }
        player->current_control_mode_ = MusicPlayer::PlayControlMode::kControlHandled;
        ESP_LOGI(TAG, "Playing track %d/%d: %s %s from %s", track_index, total_tracks,
                 music.artist.c_str(), music.name.c_str(), music.url.c_str());
        player->PlayInternal(music);
    }
    player->current_control_mode_ = MusicPlayer::PlayControlMode::kUnknown;
    player->is_playing_ = false;
    vTaskDelete(nullptr);
}

bool Mp3MusicPlayer::IsPlaying() const { return is_playing_; }

bool Mp3MusicPlayer::PreparePlayback(const Music& music, AudioCodec*& audio_codec,
                                     NetworkInterface*& network, std::string& final_url,
                                     std::unique_ptr<Http>& http) {
    auto& board = Board::GetInstance();
    audio_codec = board.GetAudioCodec();
    network = board.GetNetwork();

    if (!audio_codec || !network) {
        ESP_LOGE(TAG, "Audio codec or network not available");
        return false;
    }

    RestfulClient restful_client;
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
    ESP_LOGI(TAG, "Final URL: %s (redirected from: %s)", url.c_str(), redirect_url.c_str());

    http = network->CreateHttp();
    if (!http || !http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection for: %s", url.c_str());
        if (http) {
            http->Close();
        }
        return false;
    }

    ConfigureHttpHeaders(http.get(), url);

    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed, status code: %d", status_code);
        http->Close();
        return false;
    }

    final_url = std::move(url);
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

bool Mp3MusicPlayer::ReadInitialBuffer(Http* http, std::vector<uint8_t>& mp3_buffer,
                                       size_t& mp3_data_size) {
    int initial_read = http->Read(reinterpret_cast<char*>(mp3_buffer.data()), BUFFER_SIZE);
    if (initial_read <= 0) {
        ESP_LOGE(TAG, "Failed to read initial data");
        return false;
    }
    mp3_data_size = initial_read;
    ESP_LOGI(TAG, "Initial read: %d bytes, buffer size: %d", initial_read, BUFFER_SIZE);
    ESP_LOGI(TAG, "Initial data first 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
             mp3_buffer[0], mp3_buffer[1], mp3_buffer[2], mp3_buffer[3], mp3_buffer[4],
             mp3_buffer[5], mp3_buffer[6], mp3_buffer[7]);
    return true;
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

bool Mp3MusicPlayer::RefillBuffer(Http* http, std::vector<uint8_t>& mp3_buffer,
                                  size_t& mp3_data_offset, size_t& mp3_data_size) {
    if (mp3_data_size >= BUFFER_SIZE / 2) {
        return true;
    }

    if (mp3_data_offset > 0 && mp3_data_size > 0) {
        memmove(mp3_buffer.data(), mp3_buffer.data() + mp3_data_offset, mp3_data_size);
    }
    mp3_data_offset = 0;

    int bytes_read = http->Read(reinterpret_cast<char*>(mp3_buffer.data() + mp3_data_size),
                                BUFFER_SIZE - mp3_data_size);
    if (bytes_read <= 0) {
        ESP_LOGI(TAG, "End of stream or connection closed");
        return false;
    }

    mp3_data_size += bytes_read;
    ESP_LOGD(TAG, "Read %d bytes, total buffered: %u", bytes_read, (unsigned int)mp3_data_size);

    int zero_count = 0;
    for (int i = 0; i < bytes_read && i < 16; i++) {
        if (mp3_buffer[mp3_data_size - bytes_read + i] == 0) {
            zero_count++;
        }
    }
    if (zero_count == 16 && bytes_read >= 16) {
        ESP_LOGW(TAG, "Warning: Last read contains all zeros!");
    }

    return true;
}

void Mp3MusicPlayer::ConvertPcmIfNeeded(AudioCodec* audio_codec, const MP3FrameInfo& frame_info,
                                        const std::vector<int16_t>& pcm_buffer,
                                        std::vector<int16_t>& output_pcm, int& output_samples,
                                        int& output_channels) {
    int codec_output_rate = audio_codec->output_sample_rate();
    int codec_output_channels = audio_codec->output_channels();
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

bool Mp3MusicPlayer::DecodeAndPlayFrame(AudioCodec* audio_codec, HMP3Decoder decoder,
                                        std::vector<uint8_t>& mp3_buffer, size_t& mp3_data_offset,
                                        size_t& mp3_data_size, std::vector<int16_t>& pcm_buffer,
                                        int& consecutive_skip_count) {
    unsigned char* pInData = const_cast<unsigned char*>(mp3_buffer.data() + mp3_data_offset);
    int nBytesLeft = mp3_data_size;
    int samples_decoded = MP3Decode(decoder, &pInData, &nBytesLeft, pcm_buffer.data(), 0);

    if (samples_decoded < 0) {
        if (samples_decoded == ERR_MP3_INDATA_UNDERFLOW) {
            ESP_LOGI(TAG, "Need more data for MP3 decoding, current buffered: %u",
                     (unsigned int)mp3_data_size);
            consecutive_skip_count = 0;  // 重置跳过计数
            return true;
        }

        ESP_LOGW(TAG, "MP3Decode failed with error code: %d", samples_decoded);

        /* Use library sync finder instead of ad-hoc checks to resync */
        int sync = MP3FindSyncWord(mp3_buffer.data() + mp3_data_offset, (int)mp3_data_size);
        if (sync >= 0) {
            mp3_data_offset += (size_t)sync;
            mp3_data_size -= (size_t)sync;
            consecutive_skip_count = 0;  // 找到有效帧头，重置计数
            ESP_LOGW(TAG, "Resynced, skipped %d bytes to find frame at offset %u", sync,
                     (unsigned int)mp3_data_offset);
        } else {
            consecutive_skip_count++;
            if (mp3_data_size > 0) {
                mp3_data_offset++;
                mp3_data_size--;
            }

            // 检查是否超过阈值
            if (consecutive_skip_count % 20 == 0) {
                // 每20次跳过打印一次
                ESP_LOGW(TAG, "No valid frame found, skipping 1 byte (total: %d/%d)",
                         consecutive_skip_count, MAX_CONSECUTIVE_SKIPS);
            }
            if (consecutive_skip_count >= MAX_CONSECUTIVE_SKIPS) {
                ESP_LOGE(TAG,
                         "Too many consecutive frame skips (%d), stopping playback of this track",
                         consecutive_skip_count);
                return false;  // 返回false停止播放
            }
        }
        return true;
    }

    // 成功解码，重置跳过计数
    consecutive_skip_count = 0;

    MP3FrameInfo frame_info;
    MP3GetNextFrameInfo(decoder, &frame_info, pInData);
    int bytes_consumed = mp3_data_size - nBytesLeft;
    mp3_data_offset += bytes_consumed;
    mp3_data_size = nBytesLeft;
    ESP_LOGD(TAG, "Decoded %d samples, consumed %d bytes, remaining: %u", frame_info.outputSamps,
             bytes_consumed, (unsigned int)mp3_data_size);

    if (frame_info.outputSamps <= 0 ||
        current_control_mode_ == MusicPlayer::PlayControlMode::kStop) {
        return true;
    }

    int codec_output_rate = audio_codec->output_sample_rate();
    int output_samples = frame_info.outputSamps;
    int output_channels = frame_info.nChans;
    std::vector<int16_t> output_pcm;

    ConvertPcmIfNeeded(audio_codec, frame_info, pcm_buffer, output_pcm, output_samples,
                       output_channels);

    if (false == audio_codec->output_enabled()) {
        audio_codec->EnableOutput(true);
    }

    if (output_samples > 0) {
        if (output_pcm.empty()) {
            audio_codec->OutputData(pcm_buffer.data(), output_samples);
        } else {
            audio_codec->OutputData(output_pcm.data(), output_samples);
        }
    }

    ESP_LOGD(TAG,
             "Decoded %d samples, %d channels, bitrate: %d kbps, samplerate: %d -> output "
             "%d samples, %d channels, %d Hz",
             frame_info.outputSamps, frame_info.nChans, frame_info.bitrate / 1000,
             frame_info.samprate, output_samples, output_channels, codec_output_rate);

    return true;
}

void Mp3MusicPlayer::PlayInternal(const Music& music) {
    AudioCodec* audio_codec = nullptr;
    NetworkInterface* network = nullptr;
    std::string final_url;
    std::unique_ptr<Http> http;

    if (!PreparePlayback(music, audio_codec, network, final_url, http)) {
        return;
    }

    auto& app = Application::GetInstance();
    auto& audio_service = app.GetAudioService();
    ESP_LOGI(TAG, "Starting MP3(%s) playback...", music.name.c_str());
    audio_codec->SetOutputVolume(std::max(30, audio_codec->output_volume()));
    audio_codec->EnableOutput(true);

    std::vector<uint8_t> mp3_buffer(BUFFER_SIZE);
    std::vector<int16_t> pcm_buffer(PCM_BUFFER_SIZE / 2);
    auto decoder = MP3InitDecoder();
    if (!decoder) {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        audio_codec->EnableOutput(false);
        http->Close();
        return;
    }

    ESP_LOGI(TAG, "MP3 decoder initialized successfully");

    size_t mp3_data_offset = 0;
    size_t mp3_data_size = 0;

    if (!ReadInitialBuffer(http.get(), mp3_buffer, mp3_data_size)) {
        MP3FreeDecoder(decoder);
        audio_codec->EnableOutput(false);
        http->Close();
        return;
    }

    SkipId3Tag(mp3_buffer, mp3_data_size, mp3_data_offset);

    /* Search for first valid MP3 frame header after potential ID3 or other metadata */
    int sync_offset = MP3FindSyncWord(mp3_buffer.data(), (int)mp3_data_size);
    if (sync_offset >= 0) {
        mp3_data_offset = (size_t)sync_offset;
        mp3_data_size -= (size_t)sync_offset;
        ESP_LOGI(TAG, "Found first valid MP3 frame at offset %d", sync_offset);
    } else {
        ESP_LOGW(TAG,
                 "Warning: No valid MP3 frame found in initial buffer, first 8 bytes: %02X %02X "
                 "%02X %02X %02X %02X %02X %02X",
                 mp3_buffer[0], mp3_buffer[1], mp3_buffer[2], mp3_buffer[3], mp3_buffer[4],
                 mp3_buffer[5], mp3_buffer[6], mp3_buffer[7]);
    }

    int consecutive_skip_count = 0;
    while (true) {
        // 检查暂停请求
        if (current_control_mode_ == MusicPlayer::PlayControlMode::kPause) {
            ESP_LOGI(TAG, "Pause requested, pausing playback");
            current_control_mode_ = MusicPlayer::PlayControlMode::kControlHandled;

            // 暂停循环：等待恢复或停止命令
            while (true) {
                if (current_control_mode_ == MusicPlayer::PlayControlMode::kResume) {
                    ESP_LOGI(TAG, "Resume requested, resuming playback");
                    current_control_mode_ = MusicPlayer::PlayControlMode::kControlHandled;
                    break;  // 恢复播放
                }
                if (current_control_mode_ == MusicPlayer::PlayControlMode::kStop ||
                    current_control_mode_ == MusicPlayer::PlayControlMode::kNext ||
                    current_control_mode_ == MusicPlayer::PlayControlMode::kPrevious) {
                    break;  // 退出暂停循环，继续处理其他命令
                }
                vTaskDelay(pdMS_TO_TICKS(100));  // 等待控制命令
            }
        }

        if (current_control_mode_ == MusicPlayer::PlayControlMode::kStop ||
            current_control_mode_ == MusicPlayer::PlayControlMode::kNext ||
            current_control_mode_ == MusicPlayer::PlayControlMode::kPrevious) {
            break;
        }

        if (!RefillBuffer(http.get(), mp3_buffer, mp3_data_offset, mp3_data_size)) {
            break;
        }

        if (mp3_data_size < 512) {
            continue;
        }
        audio_service.UpdateLastOutputTime();
        if (!DecodeAndPlayFrame(audio_codec, decoder, mp3_buffer, mp3_data_offset, mp3_data_size,
                                pcm_buffer, consecutive_skip_count)) {
            break;
        }
    }
    MP3FreeDecoder(decoder);
    audio_codec->EnableOutput(false);
    http->Close();
    ESP_LOGI(TAG, "Finished playing MP3(%s)", music.name.c_str());
}

void Mp3MusicPlayer::Play(const std::vector<Music>& music_list) {
    if (IsPlaying()) {
        ESP_LOGW(TAG, "Already playing, stop current first");
        return;
    }
    current_music_list_ = music_list;
    current_control_mode_ = MusicPlayer::PlayControlMode::kUnknown;
    is_playing_ = true;

    // 创建播放任务
    xTaskCreate(PlayTask, "mp3_play_task", 8192, this, 15, nullptr);

    ESP_LOGI(TAG, "Started playing MP3 %d tracks", current_music_list_.size());
}

bool Mp3MusicPlayer::ChangePlayControlMode(const PlayControlMode& mode) {
    if (current_control_mode_ == mode ||
        (current_control_mode_ != PlayControlMode::kControlHandled &&
         current_control_mode_ != PlayControlMode::kUnknown)) {
        return false;
    }
    ESP_LOGI(TAG, "ChangePlayControlMode current_control_mode_ %d mode %d",
             (int)current_control_mode_, (int)mode);
    current_control_mode_ = mode;
    return true;
}
