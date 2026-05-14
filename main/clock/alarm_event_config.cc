#include "alarm_event_config.h"
#include "alarm_manager.h"
#include "assets/lang_config.h"
#include "device_state.h"
#include "esp_log.h"
#include "freertos/event_groups.h"
#include "board.h"

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
    static time_t to_alarm_event_time = time(NULL);
    ESP_LOGI(TAG, "handle alarm ring event, device state %d", device_state);
    if (alram_manager.IsRinging()) {
        ESP_LOGI(TAG, "Alarm ring, ringing");
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
            to_alarm_event_time = time(NULL);
            // auto display = Board::GetInstance().GetDisplay();
            // display->SetChatMessage("system", general_timer_->GetAlarmMessage().c_str());
            // display->SetEmotion("neutral");
        }else{
            ESP_LOGI(TAG, "Alarm ring, already in alarm clock state");
        }
        auto & audio_service = app.GetAudioService();
        app.AppendEventToGroup(MAIN_EVENT_ARARM_CLOCK_RINGING);
        if (audio_service.IsIdle()) {
            app.PlaySound(Lang::Sounds::OGG_ALARM_RING);
        }
        to_clear_alarm_event = true;
        auto now = time(NULL);
        if (difftime(now, to_alarm_event_time) > 60) {
            to_alarm_event_time = time(NULL);
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
    auto & audio_service = app.GetAudioService();
    auto & board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    audio_service.ResetDecoder();
    codec->EnableOutput(true);
    audio_service.EnableVoiceProcessing(false);
    audio_service.EnableWakeWordDetection(true);
    // display->SetStatus(Lang::Strings::ALARM);
}

bool AlarmEventConfig::HandleWakeWordDetected(const std::string& wake_word, std::unique_ptr<Protocol>& protocol) {
    auto& alram_manager = AlarmManager::GetInstance();
    
    alram_manager.StopRinging();
    ESP_LOGI(TAG, "Alarm detected, start listening");
    
    auto& app = Application::GetInstance();
    auto & audio_service = app.GetAudioService();
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

    return true;
}
