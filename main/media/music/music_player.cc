
#include "music_player.h"
#include "application.h"
#include "board.h"
#include "esp_log.h"

#define TAG "MusicPlayer"


void MusicPlayer::ResetPlaybackProgress() {
    current_position_ms_ = 0;
    total_duration_ms_ = 0;
    ESP_LOGD(TAG, "Playback progress reset");
}