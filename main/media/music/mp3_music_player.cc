#include "mp3_music_player.h"
#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "esp_wifi.h"
#include "lyrics.h"
#include "media/common/http_stream.h"
#include "media/common/restful_client.h"
#include "media/common/string_helper.h"
#include "media/common/xiaozhi_helper.h"
#include "music_resource.h"

extern "C" {
#include <mp3dec.h>
}

#define TAG "Mp3MusicPlayer"

Mp3MusicPlayer::Mp3MusicPlayer()
    : audio_codec_(Board::GetInstance().GetAudioCodec()),
      display_(Board::GetInstance().GetDisplay()),
      lyrics_(new Lyrics()),
      http_stream_(new HttpStream()),
      output_pcm_buffer_(MAX_PCM_OUTPUT_SAMPLES, 0) {  // 预分配PCM输出缓冲区
    pause_ack_semaphore_ = xSemaphoreCreateBinary();

    wake_word_listener_id_ =
        Application::GetInstance().BeforeHandleWakeWordEventListener().AddEventListener(
            [this](void* data) { return this->OnWakeWordDetected(data); });
    state_machine_listener_id_ =
        Application::GetInstance().GetStateMachine().AddStateChangeListener(
            [this](DeviceState old_state, DeviceState new_state) {
                if (new_state == kDeviceStateAlarmClock) {
                    this->ChangePlayControlMode(PlayControlMode::kPause);
                }
            });
}

Mp3MusicPlayer::~Mp3MusicPlayer() {
    if (pause_ack_semaphore_) {
        vSemaphoreDelete(pause_ack_semaphore_);
        pause_ack_semaphore_ = nullptr;
    }
    Application::GetInstance().BeforeHandleWakeWordEventListener().RemoveEventListener(
        wake_word_listener_id_);
    Application::GetInstance().GetStateMachine().RemoveStateChangeListener(
        state_machine_listener_id_);
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

    lyrics_->Clear();

    RestfulClient client;
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

bool Mp3MusicPlayer::Play(const Music* music, LoopMode mode) {
    std::vector<const Music*> music_list = {music};

    Play(music_list, mode);
    return true;
}

void Mp3MusicPlayer::Play(const std::vector<const Music*>& music_list, LoopMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (IsPlaying()) {
        ESP_LOGW(TAG, "Already playing, stop current first");
        return;
    }
    current_music_list_ = music_list;
    current_control_mode_ = MusicPlayer::PlayControlMode::kUnknown;
    play_state_ = PlayState::kPlaying;
    current_track_index_ = 0;
    loop_mode_ = mode;
    // 创建解码播放线程
    xTaskCreate(PlayMusicTask, "mp3_decode_task", 8192, this, 5, &decode_task_handle_);

    ESP_LOGI(TAG, "Started  playing MP3 %d tracks, loop mode: %d", current_music_list_.size(),
             mode);
}

// 解码播放线程
void Mp3MusicPlayer::PlayMusicTask(void* arg) {
    auto* player = static_cast<Mp3MusicPlayer*>(arg);
    player->PlayMusicLoop();
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

    // ESP_LOGD(TAG, "%s %d bytes, total buffered: %u", log_tag, (int)chunk.size,
    //          (unsigned int)mp3_data_size);

    return true;
}
void Mp3MusicPlayer::PreparePlayState() {
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateIdle) {
        return;
    }
    XiaozhiHelper helper;
    auto& audio_service = app.GetAudioService();
    // 等待播放状态
    while (helper.IsNeedWaitDeviceIdleState()) {
        audio_service.UpdateLastOutputTime();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
}

void Mp3MusicPlayer::PlayMusicLoop() {
    current_control_mode_ = PlayControlMode::kControlHandled;

    auto loop_mode = loop_mode_;
    int playlist_size = static_cast<int>(current_music_list_.size());

    // 随机播放：预先生成随机顺序
    std::vector<int> shuffle_order;
    if (loop_mode == MusicPlayer::LoopMode::kShuffle && playlist_size > 0) {
        shuffle_order.resize(playlist_size);
        for (int i = 0; i < playlist_size; i++)
            shuffle_order[i] = i;
        // Fisher-Yates 洗牌
        for (int i = playlist_size - 1; i > 0; i--) {
            int j = esp_random() % (i + 1);
            std::swap(shuffle_order[i], shuffle_order[j]);
        }
    }

    while (play_state_ != PlayState::kIdle) {
        // 检查曲目索引越界
        if (current_track_index_ < 0 || current_track_index_ >= playlist_size) {
            if (loop_mode == MusicPlayer::LoopMode::kPlayOnce) {
                ESP_LOGI(TAG, "No more tracks to play (play once mode)");
                break;
            }
            // 循环或随机：回到开头
            current_track_index_ = 0;
            if (loop_mode == MusicPlayer::LoopMode::kShuffle) {
                // 重新洗牌，生成新的随机顺序
                for (int i = playlist_size - 1; i > 0; i--) {
                    int j = esp_random() % (i + 1);
                    std::swap(shuffle_order[i], shuffle_order[j]);
                }
                ESP_LOGI(TAG, "Reshuffled playlist");
            }
            ESP_LOGI(TAG, "Looping back to first track");
        }

        // 通过随机映射获取实际曲目索引
        int actual_index = (loop_mode == MusicPlayer::LoopMode::kShuffle)
                               ? shuffle_order[current_track_index_]
                               : current_track_index_.load();
        auto* music = current_music_list_[actual_index];
        auto msg = std::format("Playing track {}/{} [actual={}]: {}",
                              1 + current_track_index_, playlist_size, actual_index, music->ToString()).c_str();
        ESP_LOGI(TAG, "%s", msg);
        display_->SetChatMessage("music", msg);


        // 初始播放准备
        PreparePlayState();

        DownloadLyrics(*music);

        DecodePlayLoop(*music);

        HandleControlSignal();  // 处理停止/下一曲/上一曲命令
    }

    play_state_ = PlayState::kIdle;
    current_control_mode_ = MusicPlayer::PlayControlMode::kUnknown;
    display_->SetChatMessage("music", "music Stopped");
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    ESP_LOGI(TAG, "Decode play task exiting");
}
void Mp3MusicPlayer::DecodePlayLoop(const Music& music) {
    auto& app = Application::GetInstance();
    auto& audio_service = app.GetAudioService();
    auto& mp3_queue = http_stream_->GetDataQueue();

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
        http_stream_->StopRequest();
        http_stream_->CleanDataQueue();
        return;
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
    while (play_state_ != PlayState::kIdle && !track_complete && !track_error) {
        decode_loop_count++;

        // 检查控制命令
        if (BreakDecodePlayLoop()) {
            break;
        }

        // 检查暂停
        auto state = play_state_.load();
        if (state == PlayState::kPausing || state == PlayState::kPaused) {
            if (state == PlayState::kPausing) {
                // 转换 Pausing→Paused，通知解码线程已真正暂停
                PlayState expected = PlayState::kPausing;
                if (play_state_.compare_exchange_strong(expected, PlayState::kPaused)) {
                    xSemaphoreGive(pause_ack_semaphore_);
                }
            }
            audio_service.UpdateLastOutputTime();
            std::unique_lock<std::mutex> lock(mutex_);
            pause_cv_.wait_for(lock, std::chrono::milliseconds(1000), [this]() {
                auto s = play_state_.load();
                return s == PlayState::kPlaying || s == PlayState::kResuming ||
                       s == PlayState::kIdle;
            });
            continue;
        }
        if (play_state_ == PlayState::kResuming) {
            play_state_ = PlayState::kPlaying;
            current_control_mode_ = PlayControlMode::kControlHandled;
            // 暂停恢复后准备播放状态
            PreparePlayState();
            // 恢复 HTTP 下载，从断点继续
            http_stream_->Open(music.url, http_stream_->GetDownloadBytesReceived());
        }

        // 从队列获取数据 - 缓冲区充足时用长超时批处理，不足时用短超时防止DMA饥饿
        DataChunk chunk;
        // 仅在真正危急(mp3_data_size < 512)时用短超时，避免频繁轮询导致AFE任务饿死
        TickType_t recv_timeout = (mp3_data_size < 512) ? pdMS_TO_TICKS(60) : pdMS_TO_TICKS(500);
        BaseType_t recv_result = xQueueReceive(mp3_queue, &chunk, recv_timeout);
        if (recv_result == pdPASS) {
            was_outputting_silence_ = false;  // 收到新数据，退出静音状态
            if (!ProcessReceivedChunk(chunk, mp3_buffer, mp3_data_offset, mp3_data_size,
                                      track_complete, track_error, "Received")) {
                break;
            }
        } else {
            // 队列超时，检查是否真的没有数据
            static int wait_count = 0;
            wait_count++;
            UBaseType_t queue_count = uxQueueMessagesWaiting(mp3_queue);
            if (queue_count > 0) {
                ESP_LOGW(TAG, "[DECODE] loop#%d queue has %d items but receive timed out! wait#%d",
                         decode_loop_count, queue_count, wait_count);
            }

            // 保持AudioPowerCheck活跃，即使队列为空也定期更新
            audio_service.UpdateLastOutputTime();
            // 缓冲区数据不足时输出静音（带淡出过渡）防止 DMA 饥饿导致炸音
            if (mp3_data_size < 512) {
                // 输出80ms静音，在60ms超时周期内持续填充DMA防止下溢
                int silence_samples = audio_codec_->output_sample_rate() *
                                      audio_codec_->output_channels() * 80 / 1000;
                if (silence_samples > 0 && silence_samples <= (int)output_pcm_buffer_.size()) {
                    if (!was_outputting_silence_ && output_pcm_buffer_.size() > 0) {
                        // 从音频切换到静音：利用output_pcm_buffer_中上次的PCM数据做淡出
                        int valid = std::min((int)output_pcm_buffer_.size(), silence_samples);
                        // 5ms线性淡出，从1.0渐变到0.0
                        int fade_samples = audio_codec_->output_sample_rate() *
                                           audio_codec_->output_channels() * 5 / 1000;
                        if (fade_samples > valid) fade_samples = valid;
                        for (int i = 0; i < fade_samples; i++) {
                            int32_t gain = ((fade_samples - i) << 8) / fade_samples;
                            output_pcm_buffer_[i] = (int16_t)((int32_t)output_pcm_buffer_[i] * gain >> 8);
                        }
                        // 淡出后剩余部分填零
                        int remaining = silence_samples - fade_samples;
                        if (remaining > 0) {
                            memset(output_pcm_buffer_.data() + fade_samples, 0,
                                   remaining * sizeof(int16_t));
                        }
                    } else {
                        // 已在静音状态，直接输出静音
                        memset(output_pcm_buffer_.data(), 0, silence_samples * sizeof(int16_t));
                    }
                    audio_codec_->OutputData(output_pcm_buffer_.data(), silence_samples);
                    was_outputting_silence_ = true;
                }
                continue;
            }
        }

        audio_service.UpdateLastOutputTime();
        if (false == audio_codec_->output_enabled()) {
            audio_codec_->EnableOutput(true);
        }

        // 解码播放循环：持续解码直到缓冲区数据不足
        while (mp3_data_size >= 512 && play_state_ == PlayState::kPlaying) {
            if (!DecodeAndPlayFrame(decoder, mp3_buffer, mp3_data_offset, mp3_data_size, pcm_buffer,
                                    consecutive_skip_count)) {
                ESP_LOGW(TAG, "Decode failed, skipping");
                break;
            }

            // 解码过程中尝试预取下一批数据，提高效率
            DataChunk next_chunk;
            if (xQueueReceive(mp3_queue, &next_chunk, 0) == pdPASS) {
                if (!ProcessReceivedChunk(next_chunk, mp3_buffer, mp3_data_offset, mp3_data_size,
                                          track_complete, track_error, "Prefetched")) {
                    break;
                }
            }
        }
    }

    // 清理解码器前先淡出，防止直接关闭编解码器产生炸音
    if (!was_outputting_silence_ && output_pcm_buffer_.size() > 0) {
        int fade_ms = 10;  // 10ms淡出
        int fade_samples = audio_codec_->output_sample_rate() *
                           audio_codec_->output_channels() * fade_ms / 1000;
        if (fade_samples > (int)output_pcm_buffer_.size()) {
            fade_samples = (int)output_pcm_buffer_.size();
        }
        if (fade_samples > 0) {
            for (int i = 0; i < fade_samples; i++) {
                int32_t gain = ((fade_samples - i) << 8) / fade_samples;
                output_pcm_buffer_[i] = (int16_t)((int32_t)output_pcm_buffer_[i] * gain >> 8);
            }
            audio_codec_->OutputData(output_pcm_buffer_.data(), fade_samples);
            vTaskDelay(pdMS_TO_TICKS(fade_ms + 5));  // 等待淡出播放完成
        }
    }
    MP3FreeDecoder(decoder);
    audio_codec_->EnableOutput(false);
    // 清空队列中剩余数据
    http_stream_->CleanDataQueue();
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
    play_state_ = PlayState::kIdle;
    pause_cv_.notify_all();
    // 释放信号量，防止 OnStateMachineCallback 死等
    if (pause_ack_semaphore_) {
        xSemaphoreGive(pause_ack_semaphore_);
    }

    http_stream_->CleanDataQueue();

    if (decode_task_handle_) {
        vTaskDelete(decode_task_handle_);
        decode_task_handle_ = nullptr;
    }

    current_track_index_ = 0;
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

    // 防御性检查：防止非法 frame_info 导致除零或越界崩溃
    if (input_channels <= 0 || input_rate <= 0 || input_frames <= 0) {
        ESP_LOGE(TAG, "Invalid frame info: nChans=%d, samprate=%d, outputSamps=%d",
                 frame_info.nChans, frame_info.samprate, frame_info.outputSamps);
        return;
    }

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
    // ESP_LOGD(TAG, "Resampling/downmixing %dHz %d-ch -> %dHz %d-ch", input_rate, input_channels,
    //          codec_output_rate, codec_output_channels);

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
        // ESP_LOGD(TAG, "Not enough data to decode (size: %u, need: %d), waiting for more",
        //          (unsigned int)mp3_data_size, MIN_FRAME_SIZE);
        return true;
    }

    unsigned char* pInData = const_cast<unsigned char*>(mp3_buffer.data() + mp3_data_offset);
    int nBytesLeft = mp3_data_size;
    int samples_decoded = MP3Decode(decoder, &pInData, &nBytesLeft, pcm_buffer.data(), 0);

    if (samples_decoded < 0) {
        if (samples_decoded == ERR_MP3_INDATA_UNDERFLOW) {
            // ESP_LOGD(TAG, "Need more data for MP3 decoding, current buffered: %u",
            //          (unsigned int)mp3_data_size);
            consecutive_skip_count = 0;
            return true;
        }

        ESP_LOGW(TAG, "MP3Decode failed with error code: %d, skipping 1 byte", samples_decoded);

        // 对于非 INVALID_FRAMEHEADER 的错误，MP3ClearBadFrame 已将 pcm_buffer 清零
        // 输出这些静音数据以保持音频时间线连续，防止炸音
        if (samples_decoded != ERR_MP3_INVALID_FRAMEHEADER) {
            MP3FrameInfo error_fi;
            MP3GetLastFrameInfo(decoder, &error_fi);
            if (error_fi.outputSamps > 0 && error_fi.nChans > 0 && error_fi.outputSamps <= (int)(PCM_BUFFER_SIZE / 2)) {
                audio_codec_->OutputData(pcm_buffer.data(), error_fi.outputSamps);
            }
        }

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
    MP3GetLastFrameInfo(decoder, &frame_info);
    int bytes_consumed = mp3_data_size - nBytesLeft;
    mp3_data_offset += bytes_consumed;
    mp3_data_size = nBytesLeft;
    // ESP_LOGD(TAG, "Decoded %d samples, consumed %d bytes, remaining: %u", frame_info.outputSamps,
    //          bytes_consumed, (unsigned int)mp3_data_size);

    if (frame_info.outputSamps <= 0) {
        return true;
    }

    int codec_output_rate = audio_codec_->output_sample_rate();
    int output_samples = frame_info.outputSamps;
    int output_channels = frame_info.nChans;

    ConvertPcmIfNeeded(frame_info, pcm_buffer, output_pcm_buffer_, output_samples, output_channels);

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

    if (output_samples > 0 && play_state_ == PlayState::kPlaying) {
        // 从静音恢复时做淡入，防止DC跳变炸音
        if (was_outputting_silence_) {
            int fade_samples = audio_codec_->output_sample_rate() *
                               audio_codec_->output_channels() * 5 / 1000;  // 5ms淡入
            if (fade_samples > output_samples) fade_samples = output_samples;
            // 确定实际输出的PCM数据在哪个缓冲区
            bool use_converted = !(output_samples == frame_info.outputSamps &&
                                   output_channels == frame_info.nChans);
            int16_t* out_data = use_converted ? output_pcm_buffer_.data() : pcm_buffer.data();
            for (int i = 0; i < fade_samples; i++) {
                int32_t gain = (i << 8) / fade_samples;
                out_data[i] = (int16_t)((int32_t)out_data[i] * gain >> 8);
            }
        }

        if (output_samples == frame_info.outputSamps && output_channels == frame_info.nChans) {
            // 不需要转换，直接输出，同时更新output_pcm_buffer_供后续淡出使用
            output_pcm_buffer_.resize(output_samples);
            memcpy(output_pcm_buffer_.data(), pcm_buffer.data(), output_samples * sizeof(int16_t));
            audio_codec_->OutputData(pcm_buffer.data(), output_samples);
        } else {
            // 使用转换后的PCM数据（已在output_pcm_buffer_中）
            audio_codec_->OutputData(output_pcm_buffer_.data(), output_samples);
        }
        was_outputting_silence_ = false;
        // 保持AudioPowerCheck活跃
        Application::GetInstance().GetAudioService().UpdateLastOutputTime();
    }

    // ESP_LOGD(TAG,
    //          "Decoded %d samples, %d channels, bitrate: %d kbps, samplerate: %d -> output "
    //          "%d samples, %d channels, %d Hz",
    //          frame_info.outputSamps, frame_info.nChans, frame_info.bitrate / 1000,
    //          frame_info.samprate, output_samples, output_channels, codec_output_rate);

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

    auto ret = true;
    switch (mode) {
        case PlayControlMode::kPause:
            // 暂停 HTTP 下载，释放网络带宽给 MQTT/TTS
            http_stream_->StopRequest();
            http_stream_->CleanDataQueue();
            play_state_ = PlayState::kPausing;
            break;

        case PlayControlMode::kResume:
            play_state_ = PlayState::kResuming;
            pause_cv_.notify_all();
            break;
            
        case PlayControlMode::kUnknown:
        case PlayControlMode::kControlHandled:
        case PlayControlMode::kNext:
        case PlayControlMode::kPrevious:
            play_state_ = PlayState::kPlaying;
            pause_cv_.notify_all();
            break;

        case PlayControlMode::kStop:
            play_state_ = PlayState::kIdle;
            pause_cv_.notify_all();
            break;
        default:
            break;
    }
    current_control_mode_ = mode;
    return ret;
}

bool Mp3MusicPlayer::HandleControlSignal() {
    if (current_control_mode_ == PlayControlMode::kStop) {
        current_control_mode_ = PlayControlMode::kControlHandled;
        ESP_LOGI(TAG, "Decode play stopped by user");
        display_->SetChatMessage("music", "Stopped");
        http_stream_->StopRequest();
        http_stream_->CleanDataQueue();
        play_state_ = PlayState::kIdle;
        return true;
    }
    if (current_control_mode_ == PlayControlMode::kNext ||
        current_control_mode_ == PlayControlMode::kControlHandled) {
        current_control_mode_ = PlayControlMode::kControlHandled;
        current_track_index_++;
        display_->SetChatMessage("music", "Next music");
        http_stream_->StopRequest();
        http_stream_->CleanDataQueue();
        return true;
    }
    if (current_control_mode_ == PlayControlMode::kPrevious) {
        current_control_mode_ = PlayControlMode::kControlHandled;
        current_track_index_ = std::max(0, --current_track_index_);
        display_->SetChatMessage("music", "Previous music");
        http_stream_->StopRequest();
        http_stream_->CleanDataQueue();
        return true;
    }
    return false;
}
bool Mp3MusicPlayer::BreakDecodePlayLoop() {
    return current_control_mode_ == PlayControlMode::kStop ||
           current_control_mode_ == PlayControlMode::kNext ||
           current_control_mode_ == PlayControlMode::kPrevious;
}

bool Mp3MusicPlayer::OnWakeWordDetected(void* data) {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state != kDeviceStateIdle) {
        return false;
    }
    if (play_state_ != PlayState::kPlaying) {
        return false;
    }
    ChangePlayControlMode(PlayControlMode::kPause);

    // 等待解码线程进入暂停状态（Pausing→Paused）
    if (pause_ack_semaphore_) {
        xSemaphoreTake(pause_ack_semaphore_, pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Wake word detected, pausing music");
    xTaskCreate(
        [](void* pvParameters) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            auto& app = Application::GetInstance();
            app.ClearEventFromGroup(MAIN_EVENT_WAKE_WORD_DETECTED);
            vTaskDelay(pdMS_TO_TICKS(1000));
            app.AppendEventToGroup(MAIN_EVENT_WAKE_WORD_DETECTED);
            vTaskDelete(nullptr);
        },
        "OnWakeWordDetectedTask", 2048, this, 5, nullptr);
    return true;
}