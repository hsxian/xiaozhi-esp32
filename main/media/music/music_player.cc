
#include "music_player.h"
#include "application.h"
#include "board.h"
#include "esp_log.h"

#define TAG "MusicPlayer"

bool MusicPlayer::IsNeedWaitDeviceSattus() const {
    auto& app = Application::GetInstance();
    auto current_state = app.GetDeviceState();
    // 状态转换：说话中-》聆听中-》待机状态-》播放音乐
    if (current_state == kDeviceStateListening || current_state == kDeviceStateSpeaking) {
        if (current_state == kDeviceStateSpeaking) {
            ESP_LOGI(
                TAG,
                "Device is in speaking state, switching to listening state for music playback");
        }
        if (current_state == kDeviceStateListening) {
            ESP_LOGI(TAG,
                     "Device is in listening state, switching to idle state for music playback");
        }
        // 切换状态
        app.ToggleChatState();  // 变成待机状态
        // 不要立即返回true，而是等待下一个调用周期来验证状态是否已更改
    }
    return app.GetDeviceState() != kDeviceStateIdle;  // 检查更新后的状态
}

void MusicPlayer::ResetPlaybackProgress() {
    current_position_ms_ = 0;
    total_duration_ms_ = 0;
    ESP_LOGD(TAG, "Playback progress reset");
}