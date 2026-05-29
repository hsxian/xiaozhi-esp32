
#include "music_player.h"
#include "application.h"
#include "board.h"
#include "esp_log.h"

#define TAG "MusicPlayer"

bool MusicPlayer::IsNeedWaitDeviceState() const {
    auto& app = Application::GetInstance();
    auto current_state = app.GetDeviceState();
    // 等待说话中状态结束
    if (current_state == kDeviceStateSpeaking)
        return true;
    // 状态转换：聆听中-》待机状态-》播放音乐
    if (current_state == kDeviceStateListening)
        app.ToggleChatState();
    return app.GetDeviceState() != kDeviceStateIdle;  // 检查更新后的状态
}

void MusicPlayer::ResetPlaybackProgress() {
    current_position_ms_ = 0;
    total_duration_ms_ = 0;
    ESP_LOGD(TAG, "Playback progress reset");
}