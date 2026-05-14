#include "alarm_event_config.h"
#include "alarm_manager.h"
#include "assets/lang_config.h"
#include "board.h"
#include "device_state.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/event_groups.h"

#define TAG "AlarmEventConfig"

// 单例实例
AlarmEventConfig& AlarmEventConfig::GetInstance() {
    static AlarmEventConfig instance;
    return instance;
}

AlarmEventConfig::AlarmEventConfig(/* args */) {}

AlarmEventConfig::~AlarmEventConfig() {}

bool AlarmEventConfig::HandleAlarmRingingEvent(bool& aborted, std::unique_ptr<Protocol>& protocol) {
    auto& alram_manager = AlarmManager::GetInstance();
    auto& app = Application::GetInstance();
    auto device_state = app.GetDeviceState();
    static bool to_clear_alarm_event = false;
    ESP_LOGD(TAG, "handle alarm ring event, device state %d", device_state);
    if (alram_manager.IsRinging()) {
        Alarm current_alarm;
        alram_manager.GetCurrentRingingAlarm(current_alarm);
        if (device_state != kDeviceStateAlarmClock) {
            if (device_state == kDeviceStateActivating) {
                ESP_LOGI(TAG, "Alarm ring, reboot");
                app.Reboot();
                return false;
            } else if (device_state == kDeviceStateSpeaking) {
                app.AbortSpeaking(kAbortReasonNone);
                protocol->CloseAudioChannel();
                aborted = false;
                ESP_LOGI(TAG, "Alarm ring, abort speaking");
            } else if (device_state == kDeviceStateListening) {
                ESP_LOGI(TAG, "Alarm ring, close audio channel");
                // protocol->CloseAudioChannel();
            } else if (device_state == kDeviceStateStarting) {
                app.SetDeviceState(kDeviceStateActivating);
                ESP_LOGI(TAG, "Alarm ring, exit starting state, enter activating state");
                return false;
            }
            ESP_LOGI(TAG, "Alarm ring, begging status %d", device_state);
            app.SetDeviceState(kDeviceStateAlarmClock);
            
            auto display = Board::GetInstance().GetDisplay();

            display->SetChatMessage("system", current_alarm.name.c_str());
            display->SetEmotion("neutral");
        }

        auto now = time(NULL);
        int ringing_seconds = 120;
        int time_diff = difftime(now, current_alarm.start_ring_time);
        int start_volume = 30;
        auto& board = Board::GetInstance();
        auto audio_codec = board.GetAudioCodec();
        int vol =
            start_volume + time_diff * (current_alarm.volume - start_volume) / ringing_seconds;
        vol = std::min(vol, current_alarm.volume);
        if (time_diff % 3 == 0 && vol != audio_codec->output_volume()) {
            audio_codec->SetOutputVolume(vol);
        }

        auto& audio_service = app.GetAudioService();
        if (audio_service.IsIdle()) {
            app.PlaySound(Lang::Sounds::OGG_ALARM_RING);
        }

        app.AppendEventToGroup(MAIN_EVENT_ARARM_CLOCK_RINGING);
        to_clear_alarm_event = true;


        if (time_diff > ringing_seconds) {
            alram_manager.Snooze();
        }

    } else {
        if (to_clear_alarm_event) {
            app.ClearEventFromGroup(MAIN_EVENT_ARARM_CLOCK_RINGING);
            to_clear_alarm_event = false;
        }
    }
    return true;
}

void AlarmEventConfig::SetDeviceState() {
    auto& app = Application::GetInstance();
    auto& audio_service = app.GetAudioService();
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    audio_service.ResetDecoder();
    codec->EnableOutput(true);
    audio_service.EnableVoiceProcessing(false);
    audio_service.EnableWakeWordDetection(true);
    // display->SetStatus(Lang::Strings::ALARM);
    auto display = board.GetDisplay();
    display->SetStatus("alarm clock ringing");
}

bool AlarmEventConfig::HandleWakeWordDetected(const std::string& wake_word,
                                              std::unique_ptr<Protocol>& protocol) {
    auto& alram_manager = AlarmManager::GetInstance();

    alram_manager.StopRinging();
    ESP_LOGI(TAG, "Alarm detected, start listening");

    auto& app = Application::GetInstance();
    auto& audio_service = app.GetAudioService();
    audio_service.EncodeWakeWord();

    if (!protocol->IsAudioChannelOpened()) {
        app.SetDeviceState(kDeviceStateConnecting);
        ESP_LOGI(TAG, "Alarm detected, start connecting");
        if (!protocol->OpenAudioChannel()) {
            audio_service.EnableWakeWordDetection(true);
            ESP_LOGI(TAG, "Alarm detected, connect failed");
            return false;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service.PopWakeWordPacket()) {
        protocol->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol->SendWakeWordDetected(wake_word);
    // SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
    // SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
    // Play the pop up sound to indicate the wake word is detected
    audio_service.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    return true;
}
