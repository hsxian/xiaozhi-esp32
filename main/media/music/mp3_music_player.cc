#include "mp3_music_player.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "esp_wifi.h"
#include "lyrics.h"
#include "media/common/restful_client.h"
#include "media/common/http_stream.h"
#include "media/common/string_helper.h"
#include "music_resource.h"

extern "C" {
#include <mp3dec.h>
}

#define TAG "Mp3MusicPlayer"

Mp3MusicPlayer::Mp3MusicPlayer()
    : audio_codec_(Board::GetInstance().GetAudioCodec()),
      display_(Board::GetInstance().GetDisplay()),
      lyrics_(new Lyrics()),
      http_stream_(new HttpStream()) {

    auto& state_machine = Application::GetInstance().GetStateMachine();
    listener_id_ =
        state_machine.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
            this->OnStateMachineCallback(old_state, new_state);
        });
}

Mp3MusicPlayer::~Mp3MusicPlayer() {
    auto& state_machine = Application::GetInstance().GetStateMachine();
    state_machine.RemoveStateChangeListener(listener_id_);
    CleanupResources();
    delete lyrics_;
    lyrics_ = nullptr;
    delete http_stream_;
    http_stream_ = nullptr;
}

void Mp3MusicPlayer::DownloadLyrics(const Music& music) {
    if (music.lrc.empty()) {
        ESP_LOGW(TAG, "No lyrics for music: %s", music.ToString().c_str());
        return;
    }
    static bool is_downloading_lyrics_ = false;
    if (is_downloading_lyrics_) {
        ESP_LOGW(TAG, "Already downloading lyrics for music: %s", music.ToString().c_str());
        return;
    }
    is_downloading_lyrics_ = true;

    RestfulClient client(3);
    // std::string lyrics_url = client.NormalizeUrl(music.lrc);
    // client.TryGetRedirectUrl(lyrics_url, lyrics_url);
    // if (lyrics_url.empty()) {
    //     ESP_LOGE(TAG, "Failed to download lyrics for music: %s", music.ToString().c_str());
    //     is_downloading_lyrics_ = false;
    //     return;
    // }
    auto res = client.Get(music.lrc);
    if (res.empty()) {
        ESP_LOGE(TAG, "Failed to download lyrics for music: %s", music.ToString().c_str());
        is_downloading_lyrics_ = false;
        return;
    }
    auto resource = MusicResource::NewMusicResource();
    resource->ParseLyricsFromJson(res, *lyrics_);
    delete resource;
    is_downloading_lyrics_ = false;
}

void Mp3MusicPlayer::ShowLyrics() {
    if (!lyrics_->HasLyrics()) {
        return;
    }
    std::string line;
    auto line_index = lyrics_->GetCurrentLineIndex();
    if (!lyrics_->GetLyricAtTime(current_position_ms_, line)) {
        return;
    }
    if (line_index == lyrics_->GetCurrentLineIndex()) {
        return;
    }
    auto str = line.c_str();
    // ESP_LOGI(TAG, "Show lyrics: %s", str);
    display_->SetChatMessage("music", str);
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

    // 创建解码播放线程
    xTaskCreate(DecodePlayTask, "mp3_decode_task", 8192, this, 5, &decode_task_handle_);

    ESP_LOGI(TAG, "Started playing MP3 %d tracks", current_music_list_.size());
}

// 解码播放线程
void Mp3MusicPlayer::DecodePlayTask(void* arg) {
    auto* player = static_cast<Mp3MusicPlayer*>(arg);
    player->DecodePlayLoop();
    vTaskDelete(nullptr);
}

// 处理接收到的MP3数据块
bool Mp3MusicPlayer::ProcessReceivedChunk(DataChunk& chunk, std::vector<uint8_t>& mp3_buffer,
                                          size_t& mp3_data_offset, size_t& mp3_data_size,
                                          bool& track_complete, bool& track_error,
                                          const char* log_tag) {
    // 检查状态
    if (chunk.status == DataStatus::kEos) {
        ESP_LOGI(TAG, "%s EOS signal", log_tag);
        track_complete = true;
        return false;
    }
    if (chunk.status == DataStatus::kError) {
        ESP_LOGE(TAG, "%s error signal", log_tag);
        track_error = true;
        return false;
    }

    // 将数据添加到缓冲区。若数据已消费，需要先压缩剩余数据到缓冲区前端。
    if (mp3_data_offset > 0) {
        if (mp3_data_size > 0) {
            memmove(mp3_buffer.data(), mp3_buffer.data() + mp3_data_offset, mp3_data_size);
        }
        mp3_data_offset = 0;
    }

    if (mp3_data_size + chunk.size > mp3_buffer.size()) {
        mp3_buffer.resize(mp3_data_size + chunk.size + BUFFER_SIZE);
    }
    memcpy(mp3_buffer.data() + mp3_data_size, chunk.data, chunk.size);
    mp3_data_size += chunk.size;
    delete[] chunk.data;

    ESP_LOGD(TAG, "%s %d bytes, total buffered: %u", log_tag, (int)chunk.size,
             (unsigned int)mp3_data_size);

    return true;
}

void Mp3MusicPlayer::DecodePlayLoop() {
    auto& app = Application::GetInstance();
    auto& audio_service = app.GetAudioService();
    auto& mp3_queue = http_stream_->GetDataQueue();

    current_control_mode_ = MusicPlayer::PlayControlMode::kControlHandled;
    
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

        DownloadLyrics(music);
        
        http_stream_->Open(music.url);

        display_->SetChatMessage("music", ("Playing: " + music.ToString()).c_str());

        audio_codec_->SetOutputVolume(std::max(10, audio_codec_->output_volume()));
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
            DataChunk chunk;
            while (xQueueReceive(mp3_queue, &chunk, portMAX_DELAY) == pdPASS && is_playing_) {
                if (chunk.data)
                    delete[] chunk.data;
                if (chunk.status == DataStatus::kEos || chunk.status == DataStatus::kError)
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
        int decode_loop_count = 0;
        while (is_playing_ && !track_complete && !track_error) {
            decode_loop_count++;
            // 检查暂停
            if (is_paused_) {
                ESP_LOGI(TAG, "Paused, waiting for resume");
                audio_service.UpdateLastOutputTime();
                std::unique_lock<std::mutex> lock(mutex_);
                auto waited = pause_cv_.wait_for(lock, std::chrono::milliseconds(1000), [this]() {
                    return !is_paused_ ||
                           current_control_mode_ == MusicPlayer::PlayControlMode::kStop;
                });
                audio_service.UpdateLastOutputTime();
                ESP_LOGI(TAG, "Resumed or stopped (waited=%d)", (int)waited);
            }

            // 检查控制命令
            if (current_control_mode_ == MusicPlayer::PlayControlMode::kStop) {
                ESP_LOGI(TAG, "Decode play stopped by user");
                http_stream_->StopRequest();
                break;
            }
            if (current_control_mode_ == MusicPlayer::PlayControlMode::kNext ||
                current_control_mode_ == MusicPlayer::PlayControlMode::kPrevious) {
                ESP_LOGI(TAG, "Track change requested");
                http_stream_->StopRequest();
                break;
            }

            // 等待播放状态
            while (IsNeedWaitDeviceSattus()) {
                audio_service.UpdateLastOutputTime();
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            // 从队列获取数据 - 使用较长超时时间确保能获取到数据
            DataChunk chunk;
            BaseType_t recv_result = xQueueReceive(mp3_queue, &chunk, pdMS_TO_TICKS(500));
            if (recv_result == pdPASS) {
                size_t prev_offset = mp3_data_offset;
                size_t prev_size = mp3_data_size;
                if (!ProcessReceivedChunk(chunk, mp3_buffer, mp3_data_offset, mp3_data_size,
                                          track_complete, track_error, "Received")) {
                    break;
                }
                ESP_LOGD(TAG, "[DECODE] loop#%d recv sz=%d buf_sz=%d->%d",
                         decode_loop_count, (int)chunk.size,
                         (int)(prev_offset + prev_size),
                         (int)(mp3_data_offset + mp3_data_size));
            } else {
                // 队列超时，检查是否真的没有数据
                static int wait_count = 0;
                wait_count++;
                UBaseType_t queue_count = uxQueueMessagesWaiting(mp3_queue);
                if (queue_count > 0) {
                    ESP_LOGW(TAG, "[DECODE] loop#%d queue has %d items but receive timed out! wait#%d",
                             decode_loop_count, queue_count, wait_count);
                } else {
                    ESP_LOGD(TAG, "[DECODE] loop#%d queue empty, waiting... wait#%d",
                             decode_loop_count, wait_count);
                }
                // 保持AudioPowerCheck活跃，即使队列为空也定期更新
                audio_service.UpdateLastOutputTime();
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
                DataChunk next_chunk;
                if (xQueueReceive(mp3_queue, &next_chunk, 0) == pdPASS) {
                    if (!ProcessReceivedChunk(next_chunk, mp3_buffer, mp3_data_offset,
                                              mp3_data_size, track_complete, track_error,
                                              "Prefetched")) {
                        break;
                    }
                }
            }
        }

        // 清理解码器
        MP3FreeDecoder(decoder);
        audio_codec_->EnableOutput(false);

        // 清空队列中剩余数据
        http_stream_->ClearDataQueue();

        // 更新曲目索引
        if (current_control_mode_ == MusicPlayer::PlayControlMode::kStop) {
            display_->SetChatMessage("music", "Stop");
            break;
        } else if (current_control_mode_ == MusicPlayer::PlayControlMode::kControlHandled) {
            current_track_index_++;
            display_->SetChatMessage("music", "Next track");
            TrySetControlModeToHandled(1);
        } else {
            TrySetControlModeToHandled(1);
        }
    }

    is_playing_ = false;
    current_control_mode_ = MusicPlayer::PlayControlMode::kUnknown;
    display_->SetChatMessage("music", "Stopped");
    ESP_LOGI(TAG, "Decode play task exiting");
}

bool Mp3MusicPlayer::IsPlaying() const { return is_playing_; }


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

    http_stream_->ClearDataQueue();

    if (decode_task_handle_) {
        vTaskDelete(decode_task_handle_);
        decode_task_handle_ = nullptr;
    }

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
            ESP_LOGE(TAG,
                     "Too many consecutive frame skips (%d), stopping playback of this track. "
                     "Buffer: offset=%u size=%u",
                     consecutive_skip_count, (unsigned int)mp3_data_offset,
                     (unsigned int)mp3_data_size);
            // 打印缓冲区最后32字节帮助调试
            if (mp3_data_size > 0) {
                ESP_LOGE(TAG, "Buffer tail: %02X %02X %02X %02X %02X %02X %02X %02X",
                         mp3_buffer[mp3_data_offset + mp3_data_size - 8],
                         mp3_buffer[mp3_data_offset + mp3_data_size - 7],
                         mp3_buffer[mp3_data_offset + mp3_data_size - 6],
                         mp3_buffer[mp3_data_offset + mp3_data_size - 5],
                         mp3_buffer[mp3_data_offset + mp3_data_size - 4],
                         mp3_buffer[mp3_data_offset + mp3_data_size - 3],
                         mp3_buffer[mp3_data_offset + mp3_data_size - 2],
                         mp3_buffer[mp3_data_offset + mp3_data_size - 1]);
            }
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
    ShowLyrics();

    static time_t time_flag = time(nullptr);

    if (time(nullptr) - time_flag > 10) {
        StringHelper sh;
        time_flag = time(nullptr);
        ESP_LOGI(TAG, "Current position: %s, total duration: %s",
                 sh.MillisecondToString(current_position_ms_.load()).c_str(),
                 sh.MillisecondToString(total_duration_ms_.load()).c_str());
    }

    if (output_samples > 0) {
        if (output_pcm.empty()) {
            audio_codec_->OutputData(pcm_buffer.data(), output_samples);
        } else {
            audio_codec_->OutputData(output_pcm.data(), output_samples);
        }
        // 保持AudioPowerCheck活跃
        Application::GetInstance().GetAudioService().UpdateLastOutputTime();
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
        size_t content_length = http_stream_->GetContentLength();
        if (content_length > 0) {
            int32_t total_ms = (int32_t)((uint64_t)content_length * 8000 / frame_info.bitrate);
            if (total_ms > 0) {
                total_duration_ms_ = total_ms;
            }
        }
    }
}

bool Mp3MusicPlayer::CanChangePlayControlMode(const PlayControlMode& mode) {
    if (mode == current_control_mode_) {
        return false;
    }
    if (mode == PlayControlMode::kStop) {
        return true;
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

        case PlayControlMode::kUnknown:
        case PlayControlMode::kControlHandled:
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

    // ESP_LOGI(TAG, "OnStateMachineCallback old_state %d new_state %d", (int)old_state,
    // (int)new_state);
    if (old_state == kDeviceStateIdle) {
        ESP_LOGI(TAG, "State changed from Idle during playback, pausing");
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
    int count = 0;
    const int MAX_WAIT_COUNT = 100;  // 5秒超时
    while (handled_task_list_flag_.load() != TASK_LIST_FLAG) {
        vTaskDelay(pdMS_TO_TICKS(50));
        count++;
        if (count >= MAX_WAIT_COUNT) {
            ESP_LOGE(TAG, "TrySetControlModeToHandled timeout!");
            return true;
        }
        if (count % 20 == 19) {
            ESP_LOGW(TAG,
                     "TrySetControlModeToHandled wait count %d , flag %d current_control_mode_ %d",
                     count, task_flag, (int)current_control_mode_.load());
        }
    }
    auto expected = PlayControlMode::kControlHandled;
    if (!current_control_mode_.compare_exchange_strong(expected,
                                                       PlayControlMode::kControlHandled)) {
        return false;  // 已被其他任务设置
    }
    current_control_mode_ = PlayControlMode::kControlHandled;
    if (expected == PlayControlMode::kNext) {
        current_track_index_++;
        display_->SetChatMessage("music", "Next track");
    } else if (expected == PlayControlMode::kPrevious) {
        current_track_index_ = std::max(0, --current_track_index_);
        display_->SetChatMessage("music", "Previous track");
    }
    ESP_LOGI(TAG, "TrySetControlModeToHandled current_control_mode to  kControlHandled");
    return true;
}
void Mp3MusicPlayer::ResetHandledTaskListFlag() { handled_task_list_flag_ = 0; }

void Mp3MusicPlayer::WaitPalySattus() {
    while (current_control_mode_ != PlayControlMode::kControlHandled &&
           current_control_mode_ != PlayControlMode::kUnknown) {
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}