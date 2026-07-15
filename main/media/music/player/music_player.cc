#include "music_player.h"
#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "device_state.h"
#include "display.h"
// #include "esp_audio_dec_default.h"
// #include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_reg.h"
#include "esp_mp3_dec.h"
#include "esp_flac_dec.h"
#include "esp_aac_dec.h"
#include "simple_dec/impl/esp_m4a_dec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../lyrics.h"
#include "../music.h"
#include "../provider/music_resource.h"
#include "media/common/http_stream.h"
#include "media/common/restful_client.h"
#include "media/common/xiaozhi_helper.h"
#include "media/common/ring_buffer.h"
#include <cctype>

#define TAG "MusicPlayer"

esp_audio_simple_dec_type_t MusicPlayer::DetectAudioType(const uint8_t* data, size_t size) {
    if (size < 4) return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;

    // Skip ID3v2 tag
    if (data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        if (size < 10) return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
        uint32_t tag_size = ((uint32_t)(data[6] & 0x7F) << 21) |
                            ((uint32_t)(data[7] & 0x7F) << 14) |
                            ((uint32_t)(data[8] & 0x7F) << 7) |
                            (uint32_t)(data[9] & 0x7F);
        tag_size += 10;
        if (tag_size + 4 > size) return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
        data += tag_size;
        size -= tag_size;
        if (size < 4) return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }

    // FLAC: "fLaC" magic
    if (data[0] == 'f' && data[1] == 'L' && data[2] == 'a' && data[3] == 'C') {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    }

    // M4A/MP4 container: "ftyp" at offset 4
    if (size >= 12 && data[4] == 'f' && data[5] == 't' && data[6] == 'y' && data[7] == 'p') {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    }

    // MP3 / AAC ADTS: sync word 0xFFF
    if (data[0] == 0xFF && (data[1] & 0xE0) == 0xE0) {
        uint8_t layer = (data[1] >> 1) & 0x03;

        // AAC ADTS: layer bits == 00
        if (layer == 0) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        }

        // MP3: layer bits != 00 (01=Layer3, 10=Layer2, 11=Layer1)
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }

    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

MusicPlayer::MusicPlayer()
    : audio_codec_(Board::GetInstance().GetAudioCodec()),
      display_(Board::GetInstance().GetDisplay()),
      lyrics_(new Lyrics()),
      http_stream_(new HttpStream()),
      output_pcm_buffer_(MAX_PCM_OUTPUT_SAMPLES, 0) {
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
    toggle_chat_listener_id_ =
        Application::GetInstance().BeforeHandleToggleChatEventListener().AddEventListener(
            [this](void* data) { return this->OnWakeWordDetected(data); });
}

std::unique_ptr<MusicPlayer> MusicPlayer::NewMusicPlayer() {
    return std::make_unique<MusicPlayer>();
}

MusicPlayer::~MusicPlayer() {
    if (pause_ack_semaphore_) {
        vSemaphoreDelete(pause_ack_semaphore_);
        pause_ack_semaphore_ = nullptr;
    }
    Application::GetInstance().BeforeHandleWakeWordEventListener().RemoveEventListener(
        wake_word_listener_id_);
    Application::GetInstance().GetStateMachine().RemoveStateChangeListener(
        state_machine_listener_id_);
    Application::GetInstance().BeforeHandleToggleChatEventListener().RemoveEventListener(
        toggle_chat_listener_id_);
    CleanupResources();
    delete lyrics_;
    lyrics_ = nullptr;
    delete http_stream_;
    http_stream_ = nullptr;
}

void MusicPlayer::ResetPlaybackProgress() {
    current_position_ms_ = 0;
    total_duration_ms_ = 0;
    ESP_LOGD(TAG, "Playback progress reset");
}

bool MusicPlayer::Play(Music* music, LoopMode mode) {
    std::vector<Music*> music_list = {music};
    Play(music_list, mode);
    return true;
}

void MusicPlayer::Play(const std::vector<Music*>& music_list, LoopMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (IsPlaying()) {
        ESP_LOGW(TAG, "Already playing, stop current first");
        return;
    }
    current_music_list_ = music_list;
    current_control_mode_ = PlayControlMode::kUnknown;
    play_state_ = PlayState::kPlaying;
    current_track_index_ = 0;
    loop_mode_ = mode;

    xTaskCreate(PlayMusicTask, "music_decode_task", 8192, this, 2, &decode_task_handle_);

    ESP_LOGI(TAG, "Started playing %d tracks, loop mode: %d", current_music_list_.size(), mode);
}

void MusicPlayer::PlayMusicTask(void* arg) {
    auto* player = static_cast<MusicPlayer*>(arg);
    player->PlayMusicLoop();
    vTaskDelete(nullptr);
}

bool MusicPlayer::ProcessReceivedChunk(DataChunk& chunk, RingBuffer& buffer,
                                        bool& track_complete, bool& track_error,
                                        const char* log_tag) {
    if (chunk.status == DataStatus::kEos) {
        ESP_LOGI(TAG, "%s EOS signal", log_tag);
        if (chunk.data) {
            delete[] chunk.data;
        }
        track_complete = true;
        return false;
    }
    if (chunk.status == DataStatus::kError) {
        ESP_LOGE(TAG, "%s error signal", log_tag);
        if (chunk.data) {
            delete[] chunk.data;
        }
        track_error = true;
        return false;
    }

    buffer.write(chunk.data, chunk.size);
    delete[] chunk.data;

    return true;
}

// Peek 队列检查缓冲区是否有足够空间容纳下一个 chunk
static bool CanFitNextChunk(RingBuffer& buffer, QueueHandle_t queue) {
    DataChunk chunk;
    if (xQueuePeek(queue, &chunk, 0) != pdPASS) return false;
    return (buffer.capacity() - buffer.size()) >= chunk.size;
}

void MusicPlayer::DecodePlayLoop(Music& music) {
    auto& app = Application::GetInstance();
    auto& audio_service = app.GetAudioService();
    auto& data_queue = http_stream_->GetDataQueue();

    auto mr = MusicResource::NewMusicResource();
    auto url = mr->GetUrl(music);
    if (url.empty()) {
        ESP_LOGW(TAG, "Failed to get music URL");
        return;
    }
    http_stream_->Open(url);

    display_->SetChatMessage("music", ("Playing: " + music.ToString()).c_str());

    audio_codec_->SetOutputVolume(std::max(10, audio_codec_->output_volume()));
    audio_codec_->EnableOutput(true);

    ResetPlaybackProgress();

    track_complete_ = false;
    track_error_ = false;
    const size_t min_decode_size = 256;
    bool decoder_opened = false;

    // 输入数据环形缓冲区 - 声明在循环外，保持数据持久性
    RingBuffer data_buffer;

    ESP_LOGI(TAG, "Decode: Entering main decode loop");
    while (play_state_ != PlayState::kIdle && !track_complete_ && !track_error_) {
        if (BreakDecodePlayLoop()) {
            break;
        }

        if (play_state_ == PlayState::kPausing || play_state_ == PlayState::kPaused) {
            HandlePauseState();
            continue;
        }
        HandleResumeState(url);

        // 定期让出任务以允许其他任务（如WiFi）执行，防止watchdog超时
        vTaskDelay(1);

        DataChunk chunk;
        TickType_t recv_timeout;

        // 解码器未打开时，用较短的超时等待首块数据
        if (!decoder_opened) {
            recv_timeout = pdMS_TO_TICKS(5000);
        } else if (CanFitNextChunk(data_buffer, data_queue)) {
            recv_timeout = pdMS_TO_TICKS(0);
        } else if (uxQueueMessagesWaiting(data_queue) > 0) {
            recv_timeout = pdMS_TO_TICKS(0);
        } else {
            recv_timeout = (data_buffer.size() < min_decode_size)
                               ? pdMS_TO_TICKS(60)
                               : pdMS_TO_TICKS(500);
        }
        BaseType_t recv_result = xQueueReceive(data_queue, &chunk, recv_timeout);
        if (recv_result == pdPASS) {
            was_outputting_silence_ = false;

            // 如果解码器还没打开，先用收到的数据检测格式
            if (!decoder_opened) {
                auto type = DetectAudioType((const uint8_t*)chunk.data, chunk.size);
                if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
                    type = DecideDecoderTypeByUrl(url);
                }
                if (!OpenDecoder(type)) {
                    ESP_LOGE(TAG, "Failed to initialize decoder");
                    delete[] chunk.data;
                    audio_codec_->EnableOutput(false);
                    http_stream_->StopRequest();
                    http_stream_->CleanDataQueue();
                    return;
                }
                decoder_opened = true;
            }

            if (!ProcessReceivedChunk(chunk, data_buffer, track_complete_,
                                      track_error_, "Received")) {
                break;
            }
        } else {
            if (!decoder_opened) {
                ESP_LOGE(TAG, "Timeout waiting for first data chunk");
                break;
            }
            audio_service.UpdateLastOutputTime();
            if (data_buffer.size() < min_decode_size) {
                HandleBufferUnderrun(80, 5);
                continue;
            }
        }

        audio_service.UpdateLastOutputTime();
        if (false == audio_codec_->output_enabled()) {
            audio_codec_->EnableOutput(true);
        }

        while (data_buffer.size() >= min_decode_size && play_state_ == PlayState::kPlaying) {
            if (!DecodeAndPlayFrame(data_buffer)) {
                ESP_LOGW(TAG, "Decode failed, skipping");
                track_error_ = true;
                break;
            }

            // 在长时间解码后让出任务以允许其他任务运行
            vTaskDelay(1);

            DataChunk next_chunk;
            if (CanFitNextChunk(data_buffer, data_queue)) {
                xQueueReceive(data_queue, &next_chunk, 0);
                if (!ProcessReceivedChunk(next_chunk, data_buffer, track_complete_,
                                          track_error_, "Prefetched")) {
                    break;
                }
            }
        }
    }

    // Flush remaining data in decoder on track complete
    if (track_complete_ && !track_error_) {
        while (data_buffer.size() > 0) {
            if (!DecodeAndPlayFrame(data_buffer)) break;
        }
    }

    FadeOutAndStop(10);
    CloseDecoder();
    audio_codec_->EnableOutput(false);
    http_stream_->CleanDataQueue();
}

void MusicPlayer::PreparePlayState() {
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateIdle) {
        return;
    }
    XiaozhiHelper helper;
    auto& audio_service = app.GetAudioService();
    while (helper.IsNeedWaitDeviceIdleState()) {
        audio_service.UpdateLastOutputTime();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
}

void MusicPlayer::PlayMusicLoop() {
    current_control_mode_ = PlayControlMode::kControlHandled;

    auto loop_mode = loop_mode_;
    int playlist_size = static_cast<int>(current_music_list_.size());

    std::vector<int> shuffle_order;
    if (loop_mode == LoopMode::kShuffle && playlist_size > 0) {
        shuffle_order.resize(playlist_size);
        for (int i = 0; i < playlist_size; i++)
            shuffle_order[i] = i;
        for (int i = playlist_size - 1; i > 0; i--) {
            int j = esp_random() % (i + 1);
            std::swap(shuffle_order[i], shuffle_order[j]);
        }
    }

    while (play_state_ != PlayState::kIdle) {
        if (current_track_index_ < 0 || current_track_index_ >= playlist_size) {
            if (loop_mode == LoopMode::kPlayOnce) {
                ESP_LOGI(TAG, "No more tracks to play (play once mode)");
                break;
            }
            current_track_index_ = 0;
            if (loop_mode == LoopMode::kShuffle) {
                for (int i = playlist_size - 1; i > 0; i--) {
                    int j = esp_random() % (i + 1);
                    std::swap(shuffle_order[i], shuffle_order[j]);
                }
                ESP_LOGI(TAG, "Reshuffled playlist");
            }
            ESP_LOGI(TAG, "Looping back to first track");
        }

        int actual_index = (loop_mode == LoopMode::kShuffle)
                               ? shuffle_order[current_track_index_.load()]
                               : current_track_index_.load();
        auto* music = current_music_list_[actual_index];
        auto msg = std::format("Playing track {}/{} [actual={}]: {}",
                              1 + current_track_index_.load(), playlist_size, actual_index, music->ToString());
        ESP_LOGI(TAG, "%s", msg.c_str());
        display_->SetChatMessage("music", msg.c_str());

        PreparePlayState();

        DownloadLyrics(*music);

        DecodePlayLoop(*music);

        HandleControlSignal();

        if(current_control_mode_ == PlayControlMode::kPause || current_control_mode_ == PlayControlMode::kResume) {
            break;
        }
    }

    play_state_ = PlayState::kIdle;
    current_control_mode_ = PlayControlMode::kUnknown;
    display_->SetChatMessage("music", "Music Stopped");
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    ESP_LOGI(TAG, "Decode play task exiting");
}

void MusicPlayer::DownloadLyrics(Music& music) {
    auto resource = MusicResource::NewMusicResource();
    auto lyrics_url = resource->GetLyricsUrl(music);
    if (lyrics_url.empty()) {
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
    auto res = client.Get(lyrics_url);
    if (res.empty()) {
        ESP_LOGE(TAG, "Failed to download lyrics for music: %s", music.ToString().c_str());
        is_downloading_lyrics_ = false;
        return;
    }
    resource->ParseLyricsFromJson(res, *lyrics_);
    is_downloading_lyrics_ = false;
}

void MusicPlayer::ShowLyrics() {
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
    display_->SetChatMessage("music", line.c_str());
}

bool MusicPlayer::HandleControlSignal() {
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

bool MusicPlayer::BreakDecodePlayLoop() {
    return current_control_mode_ == PlayControlMode::kStop ||
           current_control_mode_ == PlayControlMode::kNext ||
           current_control_mode_ == PlayControlMode::kPrevious;
}

bool MusicPlayer::HandlePauseState() {
    auto state = play_state_.load();
    if (state == PlayState::kPausing) {
        PlayState expected = PlayState::kPausing;
        if (play_state_.compare_exchange_strong(expected, PlayState::kPaused)) {
            xSemaphoreGive(pause_ack_semaphore_);
        }
    }
    Application::GetInstance().GetAudioService().UpdateLastOutputTime();
    std::unique_lock<std::mutex> lock(mutex_);
    pause_cv_.wait_for(lock, std::chrono::milliseconds(1000), [this]() {
        auto s = play_state_.load();
        return s == PlayState::kPlaying || s == PlayState::kResuming || s == PlayState::kIdle;
    });
    return true;
}

void MusicPlayer::HandleResumeState(const std::string& url) {
    if (play_state_ != PlayState::kResuming) {
        return;
    }
    play_state_ = PlayState::kPlaying;
    current_control_mode_ = PlayControlMode::kControlHandled;
    PreparePlayState();
    http_stream_->Open(url, http_stream_->GetDownloadBytesReceived());
}

void MusicPlayer::HandleBufferUnderrun(int silence_duration_ms, int fade_duration_ms) {
    int silence_samples = audio_codec_->output_sample_rate() *
                          audio_codec_->output_channels() * silence_duration_ms / 1000;
    if (silence_samples <= 0 || silence_samples > (int)output_pcm_buffer_.size()) {
        return;
    }

    if (!was_outputting_silence_ && output_pcm_buffer_.size() > 0) {
        int valid = std::min((int)output_pcm_buffer_.size(), silence_samples);
        int fade_samples = audio_codec_->output_sample_rate() *
                           audio_codec_->output_channels() * fade_duration_ms / 1000;
        if (fade_samples > valid) fade_samples = valid;
        for (int i = 0; i < fade_samples; i++) {
            int32_t gain = ((fade_samples - i) << 8) / fade_samples;
            output_pcm_buffer_[i] = (int16_t)((int32_t)output_pcm_buffer_[i] * gain >> 8);
        }
        int remaining = silence_samples - fade_samples;
        if (remaining > 0) {
            memset(output_pcm_buffer_.data() + fade_samples, 0,
                   remaining * sizeof(int16_t));
        }
    } else {
        memset(output_pcm_buffer_.data(), 0, silence_samples * sizeof(int16_t));
    }
    audio_codec_->OutputData(output_pcm_buffer_.data(), silence_samples);
    was_outputting_silence_ = true;
}

void MusicPlayer::FadeOutAndStop(int fade_duration_ms) {
    if (was_outputting_silence_ || output_pcm_buffer_.size() == 0) {
        return;
    }

    int fade_samples = audio_codec_->output_sample_rate() *
                       audio_codec_->output_channels() * fade_duration_ms / 1000;
    if (fade_samples > (int)output_pcm_buffer_.size()) {
        fade_samples = (int)output_pcm_buffer_.size();
    }
    if (fade_samples <= 0) {
        return;
    }

    for (int i = 0; i < fade_samples; i++) {
        int32_t gain = ((fade_samples - i) << 8) / fade_samples;
        output_pcm_buffer_[i] = (int16_t)((int32_t)output_pcm_buffer_[i] * gain >> 8);
    }
    audio_codec_->OutputData(output_pcm_buffer_.data(), fade_samples);
    vTaskDelay(pdMS_TO_TICKS(fade_duration_ms + 5));
}

bool MusicPlayer::CanChangePlayControlMode(const PlayControlMode& mode) {
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

bool MusicPlayer::ChangePlayControlMode(const PlayControlMode& mode) {
    if (!CanChangePlayControlMode(mode)) {
        ESP_LOGI(TAG, "Rejecting mode %d, previous control mode %d still pending", (int)mode,
                 (int)current_control_mode_.load());
        return false;
    }
    ESP_LOGI(TAG, "ChangePlayControlMode current_control_mode_ %d mode %d",
             (int)current_control_mode_.load(), (int)mode);

    switch (mode) {
        case PlayControlMode::kPause:
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
    return true;
}

void MusicPlayer::CleanupResources() {
    play_state_ = PlayState::kIdle;
    pause_cv_.notify_all();
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

bool MusicPlayer::OnWakeWordDetected(void* data) {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state != kDeviceStateIdle) {
        return false;
    }
    if (play_state_ != PlayState::kPlaying) {
        return false;
    }
    ChangePlayControlMode(PlayControlMode::kPause);

    if (pause_ack_semaphore_) {
        xSemaphoreTake(pause_ack_semaphore_, pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Wake word detected, pausing music");
    auto helper = new XiaozhiHelper();
    helper->ReRaiseWakeWordDetectedInTask([helper]() { delete helper; });
    return true;
}

esp_audio_simple_dec_type_t MusicPlayer::DecideDecoderTypeByUrl(const std::string& url) {
    auto dot = url.rfind('.');
    if (dot == std::string::npos) return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    std::string ext = url.substr(dot);
    for (auto& c : ext) c = tolower(c);

    if (ext == ".mp3") return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (ext == ".m4a" || ext == ".mp4") return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    if (ext == ".aac") return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    if (ext == ".flac") return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    if (ext == ".wav") return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    if (ext == ".opus") return ESP_AUDIO_SIMPLE_DEC_TYPE_RAW_OPUS;
    if (ext == ".ogg") return ESP_AUDIO_SIMPLE_DEC_TYPE_VORBIS;

    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

bool MusicPlayer::OpenDecoder(esp_audio_simple_dec_type_t type) {
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        ESP_LOGE(TAG, "Unknown audio type");
        return false;
    }

    // 注册解码器（仅首次调用时执行）
    static bool decoders_registered = false;
    if (!decoders_registered) {
        esp_mp3_dec_register();
        esp_flac_dec_register();
        esp_aac_dec_register();
        esp_m4a_dec_register();
        decoders_registered = true;
    }

    esp_audio_err_t ret = esp_audio_simple_check_audio_type(type);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Audio type %d not supported", (int)type);
        return false;
    }

    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = type,
        .dec_cfg = nullptr,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    ret = esp_audio_simple_dec_open(&cfg, &decoder_);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open decoder: %d", ret);
        return false;
    }

    consecutive_skip_count_ = 0;
    ESP_LOGI(TAG, "Decoder opened for type %d", (int)type);
    return true;
}

void MusicPlayer::CloseDecoder() {
    if (decoder_) {
        esp_audio_simple_dec_close(decoder_);
        decoder_ = nullptr;
    }
}

bool MusicPlayer::DecodeAndPlayFrame(RingBuffer& data_buffer) {
    if (data_buffer.size() == 0) return true;

    esp_audio_simple_dec_raw_t raw = {
        .buffer = data_buffer.read_ptr(),
        .len = (uint32_t)data_buffer.size(),
        .eos = false,
        .consumed = 0,
        .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
    };
    esp_audio_simple_dec_out_t out_frame = {
        .buffer = (uint8_t*)output_pcm_buffer_.data(),
        .len = (uint32_t)(output_pcm_buffer_.size() * sizeof(int16_t)),
        .needed_size = 0,
        .decoded_size = 0,
    };

    esp_audio_err_t ret = esp_audio_simple_dec_process(decoder_, &raw, &out_frame);
    data_buffer.consume(raw.consumed);

    if (ret == ESP_AUDIO_ERR_OK) {
        if (out_frame.decoded_size > 0) {
            consecutive_skip_count_ = 0;
            int samples = out_frame.decoded_size / sizeof(int16_t);

            esp_audio_simple_dec_info_t info;
            if (esp_audio_simple_dec_get_info(decoder_, &info) == ESP_AUDIO_ERR_OK) {
                int codec_output_rate = audio_codec_->output_sample_rate();
                int output_samples = samples;
                int output_channels = info.channel;

                ConvertPcmIfNeeded(info.sample_rate, info.channel, samples, output_samples,
                                   output_channels);

                UpdateTimeInfo(codec_output_rate, output_samples, output_channels, info.bitrate);
                ShowLyrics();

                if (output_samples > 0 && play_state_ == PlayState::kPlaying) {
                    if (was_outputting_silence_) {
                        int fade_samples =
                            audio_codec_->output_sample_rate() * audio_codec_->output_channels() *
                            5 / 1000;
                        if (fade_samples > output_samples) fade_samples = output_samples;
                        for (int i = 0; i < fade_samples; i++) {
                            int32_t gain = (i << 8) / fade_samples;
                            output_pcm_buffer_[i] =
                                (int16_t)((int32_t)output_pcm_buffer_[i] * gain >> 8);
                        }
                    }

                    audio_codec_->OutputData(output_pcm_buffer_.data(), output_samples);
                    was_outputting_silence_ = false;
                    Application::GetInstance().GetAudioService().UpdateLastOutputTime();
                }
            }
        }
        return true;
    } else if (ret == ESP_AUDIO_ERR_CONTINUE || ret == ESP_AUDIO_ERR_DATA_LACK) {
        return true;
    } else if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
        size_t new_size = out_frame.needed_size / sizeof(int16_t);
        if (new_size > output_pcm_buffer_.size()) {
            output_pcm_buffer_.resize(new_size);
            ESP_LOGW(TAG, "Resized output PCM buffer to %u samples", (unsigned)new_size);
        }
        return true;
    } else {
        ESP_LOGW(TAG, "Decode error: %d, skipping 1 byte", (int)ret);
        if (data_buffer.size() > 0) {
            data_buffer.consume(1);
        }
        consecutive_skip_count_++;
        if (consecutive_skip_count_ % 20 == 0) {
            ESP_LOGW(TAG, "Decode failures accumulating (total: %d/%d)", consecutive_skip_count_,
                     MAX_CONSECUTIVE_SKIPS);
        }
        if (consecutive_skip_count_ >= MAX_CONSECUTIVE_SKIPS) {
            ESP_LOGE(TAG, "Too many consecutive decode failures (%d), stopping",
                     consecutive_skip_count_);
            return false;
        }
        return true;
    }
}

void MusicPlayer::ConvertPcmIfNeeded(int input_rate, int input_channels, int input_samples,
                                     int& output_samples, int& output_channels) {
    int codec_output_rate = audio_codec_->output_sample_rate();
    int codec_output_channels = audio_codec_->output_channels();
    int input_frames = input_samples / input_channels;

    if (input_channels <= 0 || input_rate <= 0 || input_frames <= 0) return;

    bool need_rate_conversion = (input_rate != codec_output_rate && codec_output_rate > 0);
    bool need_channel_conversion =
        (input_channels != codec_output_channels && codec_output_channels > 0);

    output_samples = input_samples;
    output_channels = input_channels;

    if (!need_rate_conversion && !need_channel_conversion) return;

    output_channels = codec_output_channels;
    int output_frames = input_frames;
    if (need_rate_conversion) {
        output_frames = (int)((int64_t)input_frames * codec_output_rate / input_rate + 0.5);
        if (output_frames < 1) output_frames = 1;
    }

    output_samples = output_frames * output_channels;

    std::vector<int16_t> input_pcm(output_pcm_buffer_.begin(),
                                   output_pcm_buffer_.begin() + input_samples);
    if ((size_t)output_samples > output_pcm_buffer_.size()) {
        output_pcm_buffer_.resize(output_samples);
    }

    uint64_t step =
        need_rate_conversion ? (((uint64_t)input_rate << 32) / codec_output_rate) : (1ull << 32);
    uint64_t pos = 0;

    for (int f = 0; f < output_frames; ++f) {
        uint32_t idx = pos >> 32;
        uint32_t frac = pos & 0xFFFFFFFFu;
        if (idx >= (uint32_t)input_frames - 1) idx = input_frames - 1;

        if (input_channels == 2 && output_channels == 1) {
            int32_t s0 = ((int32_t)input_pcm[idx * 2] + (int32_t)input_pcm[idx * 2 + 1]) / 2;
            int32_t s1 = s0;
            if (idx + 1 < (size_t)input_frames)
                s1 = ((int32_t)input_pcm[(idx + 1) * 2] + (int32_t)input_pcm[(idx + 1) * 2 + 1]) /
                     2;
            output_pcm_buffer_[f] = (int16_t)(s0 + ((int64_t)(s1 - s0) * frac >> 32));
        } else if (input_channels == 1 && output_channels == 2) {
            int16_t sample = input_pcm[idx];
            output_pcm_buffer_[f * 2] = sample;
            output_pcm_buffer_[f * 2 + 1] = sample;
        } else {
            for (int c = 0; c < output_channels; ++c) {
                int ch = c < input_channels ? c : (input_channels - 1);
                int32_t s0 = input_pcm[idx * input_channels + ch];
                int32_t s1 = s0;
                if (idx + 1 < (size_t)input_frames)
                    s1 = input_pcm[(idx + 1) * input_channels + ch];
                output_pcm_buffer_[f * output_channels + c] =
                    (int16_t)(s0 + ((int64_t)(s1 - s0) * frac >> 32));
            }
        }
        pos += step;
    }
}

void MusicPlayer::UpdateTimeInfo(int codec_output_rate, int output_samples, int output_channels,
                                 int bitrate) {
    if (output_samples > 0 && output_channels > 0) {
        int output_frames = output_samples / output_channels;
        int sample_rate = codec_output_rate > 0 ? codec_output_rate : 44100;
        if (output_frames > 0 && sample_rate > 0) {
            int32_t delta_ms = (int32_t)((int64_t)output_frames * 1000 / sample_rate);
            current_position_ms_.fetch_add(delta_ms);
        }
    }

    if (total_duration_ms_.load() == 0 && bitrate > 0) {
        size_t content_length = http_stream_->GetContentLength();
        if (content_length > 0) {
            int32_t total_ms = (int32_t)((uint64_t)content_length * 8000 / bitrate);
            if (total_ms > 0) {
                total_duration_ms_ = total_ms;
            }
        }
    }
}