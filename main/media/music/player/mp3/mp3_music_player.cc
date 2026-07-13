#include "mp3_music_player.h"
#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>
#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "esp_wifi.h"
#include "media/common/http_stream.h"
#include "../../provider/music_resource.h"
#include "media/common/string_helper.h"
#include "media/common/ring_buffer.h"

extern "C" {
#include <mp3dec.h>
}

#define TAG "Mp3MusicPlayer"

Mp3MusicPlayer::Mp3MusicPlayer() = default;
Mp3MusicPlayer::~Mp3MusicPlayer() {
    CloseDecoder();
}

bool Mp3MusicPlayer::OpenDecoder() {
    decoder_ = MP3InitDecoder();
    return decoder_ != nullptr;
}

void Mp3MusicPlayer::CloseDecoder() {
    if (decoder_) {
        MP3FreeDecoder(decoder_);
        decoder_ = nullptr;
    }
}


void Mp3MusicPlayer::SkipId3Tag(RingBuffer& data_buffer) {
    if (data_buffer.size() < 10 || data_buffer[0] != 'I' || data_buffer[1] != 'D' ||
        data_buffer[2] != '3') {
        return;
    }

    uint32_t tag_size = ((uint32_t)(data_buffer[6] & 0x7F) << 21) |
                        ((uint32_t)(data_buffer[7] & 0x7F) << 14) |
                        ((uint32_t)(data_buffer[8] & 0x7F) << 7) | (uint32_t)(data_buffer[9] & 0x7F);
    tag_size += 10;
    ESP_LOGI(TAG, "Found ID3v2 tag, skipping %u bytes", tag_size);

    if (tag_size < data_buffer.size()) {
        data_buffer.skip_front(tag_size);
        ESP_LOGI(TAG, "After ID3 skip, first 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                 data_buffer[0], data_buffer[1], data_buffer[2], data_buffer[3],
                 data_buffer[4], data_buffer[5], data_buffer[6], data_buffer[7]);
    } else {
        ESP_LOGW(TAG, "ID3 tag larger than buffer, discarding buffer");
        data_buffer.clear();
    }
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

bool Mp3MusicPlayer::DecodeAndPlayFrame(RingBuffer& data_buffer) {
    if (data_buffer.head() == 0) {
        SkipId3Tag(data_buffer);
    }

    int sync_offset = MP3FindSyncWord(data_buffer.read_ptr(), (int)data_buffer.size());
    if (sync_offset > 0) {
        data_buffer.consume((size_t)sync_offset);
        ESP_LOGI(TAG, "Found valid MP3 frame at offset %d, remaining: %u", sync_offset,
                 (unsigned int)data_buffer.size());
    } else if (sync_offset < 0 && data_buffer.size() > 0) {
        data_buffer.consume(1);
        consecutive_skip_count_++;
        if (consecutive_skip_count_ % 50 == 0) {
            ESP_LOGW(TAG, "No valid frame found, skipping bytes (total skipped: %d)",
                     consecutive_skip_count_);
        }
        if (consecutive_skip_count_ >= MAX_CONSECUTIVE_SKIPS) {
            ESP_LOGE(TAG,
                     "Too many consecutive frame skips (%d), stopping playback of this track. "
                     "Buffer: head=%u size=%u",
                     consecutive_skip_count_, (unsigned int)data_buffer.head(),
                     (unsigned int)data_buffer.size());
            if (data_buffer.size() > 0) {
                ESP_LOGE(TAG, "Buffer tail: %02X %02X %02X %02X %02X %02X %02X %02X",
                         data_buffer[data_buffer.size() - 8],
                         data_buffer[data_buffer.size() - 7],
                         data_buffer[data_buffer.size() - 6],
                         data_buffer[data_buffer.size() - 5],
                         data_buffer[data_buffer.size() - 4],
                         data_buffer[data_buffer.size() - 3],
                         data_buffer[data_buffer.size() - 2],
                         data_buffer[data_buffer.size() - 1]);
            }
            return false;
        }
        return true;
    }

    const int MIN_FRAME_SIZE = 128;
    if (data_buffer.size() < MIN_FRAME_SIZE) {
        return true;
    }

    unsigned char* pInData = const_cast<unsigned char*>(data_buffer.read_ptr());
    int nBytesLeft = data_buffer.size();
    int samples_decoded = MP3Decode(decoder_, &pInData, &nBytesLeft, pcm_buffer_.data(), 0);

    if (samples_decoded < 0) {
        if (samples_decoded == ERR_MP3_INDATA_UNDERFLOW) {
            consecutive_skip_count_ = 0;
            return true;
        }

        ESP_LOGW(TAG, "MP3Decode failed with error code: %d, skipping 1 byte", samples_decoded);

        if (samples_decoded != ERR_MP3_INVALID_FRAMEHEADER) {
            MP3FrameInfo error_fi;
            MP3GetLastFrameInfo(decoder_, &error_fi);
            if (error_fi.outputSamps > 0 && error_fi.outputSamps <= (int)(PCM_BUFFER_SIZE / 2)) {
                audio_codec_->OutputData(pcm_buffer_.data(), error_fi.outputSamps);
            }
        }

        if (data_buffer.size() > 0) {
            data_buffer.consume(1);
        }
        consecutive_skip_count_++;

        if (consecutive_skip_count_ % 20 == 0) {
            ESP_LOGW(TAG, "Decode failures accumulating (total: %d/%d)", consecutive_skip_count_,
                     MAX_CONSECUTIVE_SKIPS);
        }
        if (consecutive_skip_count_ >= MAX_CONSECUTIVE_SKIPS) {
            ESP_LOGE(TAG,
                     "Too many consecutive decode failures (%d), stopping playback of this track",
                     consecutive_skip_count_);
            return false;
        }
        return true;
    }

    consecutive_skip_count_ = 0;

    MP3FrameInfo frame_info;
    MP3GetLastFrameInfo(decoder_, &frame_info);
    int bytes_consumed = data_buffer.size() - nBytesLeft;
    data_buffer.consume(bytes_consumed);

    if (frame_info.outputSamps <= 0) {
        return true;
    }

    int codec_output_rate = audio_codec_->output_sample_rate();
    int output_samples = frame_info.outputSamps;
    int output_channels = frame_info.nChans;

    ConvertPcmIfNeeded(frame_info, pcm_buffer_, output_pcm_buffer_, output_samples, output_channels);

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
        if (was_outputting_silence_) {
            int fade_samples = audio_codec_->output_sample_rate() *
                               audio_codec_->output_channels() * 5 / 1000;
            if (fade_samples > output_samples) fade_samples = output_samples;
            bool use_converted = !(output_samples == frame_info.outputSamps &&
                                   output_channels == frame_info.nChans);
            int16_t* out_data = use_converted ? output_pcm_buffer_.data() : pcm_buffer_.data();
            for (int i = 0; i < fade_samples; i++) {
                int32_t gain = (i << 8) / fade_samples;
                out_data[i] = (int16_t)((int32_t)out_data[i] * gain >> 8);
            }
        }

        if (output_samples == frame_info.outputSamps && output_channels == frame_info.nChans) {
            output_pcm_buffer_.resize(output_samples);
            memcpy(output_pcm_buffer_.data(), pcm_buffer_.data(), output_samples * sizeof(int16_t));
            audio_codec_->OutputData(pcm_buffer_.data(), output_samples);
        } else {
            audio_codec_->OutputData(output_pcm_buffer_.data(), output_samples);
        }
        was_outputting_silence_ = false;
        Application::GetInstance().GetAudioService().UpdateLastOutputTime();
    }

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