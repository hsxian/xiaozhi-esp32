
#include "music_player.h"
#include "application.h"
#include "board.h"
#include "esp_log.h"

#define TAG "MusicPlayer"

bool MusicPlayer::IsNeedWaitPalySattus() const {
    auto& app = Application::GetInstance();
    DeviceState current_state = app.GetDeviceState();
    // 状态转换：说话中-》聆听中-》待机状态-》播放音乐
    if (current_state == kDeviceStateListening || current_state == kDeviceStateSpeaking) {
        if (current_state == kDeviceStateSpeaking) {
            ESP_LOGI(
                TAG,
                "Device is in speaking state, switching to listening state for music playback");
            // app.Schedule([this, &app]() { app.AbortSpeaking(kAbortReasonNone); });
            // app.SetDeviceState(kDeviceStateListening);
        }
        if (current_state == kDeviceStateListening) {
            ESP_LOGI(TAG,
                     "Device is in listening state, switching to idle state for music playback");
            // app.Schedule([this, &app]() { app.StopListening(); });

        }
        // 切换状态
        app.ToggleChatState();  // 变成待机状态
        return true;
    } else if (current_state != kDeviceStateIdle) {  // 不是待机状态，就一直卡在这里，不让播放音乐
        ESP_LOGD(TAG, "Device state is %d, pausing music playback", current_state);
        return true;
    }
    return false;
}