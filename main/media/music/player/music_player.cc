#include "music_player.h"
#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "device_state.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../lyrics.h"
#include "media/common/http_stream.h"
#include "../provider/music_resource.h"
#include "media/common/restful_client.h"
#include "media/common/xiaozhi_helper.h"
#include "media/common/ring_buffer.h"

#ifdef CONFIG_ENABLE_MP3_DECODER
#include "mp3/mp3_music_player.h"
#endif

#define TAG "MusicPlayer"

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
#ifdef CONFIG_ENABLE_MP3_DECODER
    return std::make_unique<Mp3MusicPlayer>();
#else
#error "Please enable at least one music player decoder"
#endif
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

    xTaskCreate(PlayMusicTask, "music_decode_task", 8192, this, 5, &decode_task_handle_);

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
        track_complete = true;
        return false;
    }
    if (chunk.status == DataStatus::kError) {
        ESP_LOGE(TAG, "%s error signal", log_tag);
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

    if (!OpenDecoder()) {
        ESP_LOGE(TAG, "Failed to initialize decoder");
        audio_codec_->EnableOutput(false);
        http_stream_->StopRequest();
        http_stream_->CleanDataQueue();
        return;
    }

    track_complete_ = false;
    track_error_ = false;
    size_t min_decode_size = GetMinDecodeSize();

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

        DataChunk chunk;
        TickType_t recv_timeout;

            // 输入数据环形缓冲区
        RingBuffer data_buffer;

        if (CanFitNextChunk(data_buffer, data_queue)) {
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
            if (!ProcessReceivedChunk(chunk, data_buffer, track_complete_,
                                      track_error_, "Received")) {
                break;
            }
        } else {
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