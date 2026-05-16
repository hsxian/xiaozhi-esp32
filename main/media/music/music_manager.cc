#include "music_manager.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include "audio_codec.h"
#include "board.h"
#include "kw_music_resource.h"
#include "mcp_server.h"
#include "mp3_music_player.h"

#define TAG "MusicManager"

MusicManager::MusicManager() {}

MusicManager& MusicManager::GetInstance() {
    static MusicManager instance;
    return instance;
}

void MusicManager::GenerateMcpServerTools(std::vector<McpTool*>& tools) {
    // 搜索音乐
    auto tool = new McpTool("self.music.search",
                            "a tool to search music from the internet. You must provide a keyword "
                            "to search, and you can also provide page number and page size for "
                            "pagination. The result will be a list of music in JSON format.",
                            PropertyList({Property("keyword", kPropertyTypeString),
                                          Property("page", kPropertyTypeInteger, 1),
                                          Property("pageSize", kPropertyTypeInteger, 10)}),
                            [this](const PropertyList& properties) -> ReturnValue {
                                // Implement search logic here
                                QueryBase query;
                                query.keyword = properties["keyword"].value<std::string>();
                                query.page = properties["page"].value<int>();
                                query.page_size = properties["pageSize"].value<int>();

                                KwMusicResource resource;
                                music_search_list_.clear();
                                if (!resource.Search(query, music_search_list_)) {
                                    return "";
                                }

                                auto json = Music::ToJsonArray(music_search_list_);

                                return json;
                            });
    tools.push_back(tool);

    // 播放音乐
    tool = new McpTool(
        "self.music.play", "a tool to play music. You must provide the URL of the music to play.",
        PropertyList({Property("searchResultOrMusicList", kPropertyTypeBoolean, true)}),
        [this](const PropertyList& properties) -> ReturnValue {
            // Implement play logic here
            bool is_search_result_or_music_list =
                properties["searchResultOrMusicList"].value<bool>();
            if (music_player_ == nullptr) {
                music_player_ = new Mp3MusicPlayer();
            }
            auto musics = is_search_result_or_music_list ? music_search_list_ : music_list_;
            if (!musics.empty()) {
                music_player_->Play(musics);
                return true;
            }
            TryResleaseMusicPlayer();
            return false;
        });
    tools.push_back(tool);

    // 播放音乐控制
    tool = new McpTool(
        "self.music.playControl",
        "a tool to control music playback. You must provide the control mode to use. The control "
        "mode can be one of the following values: 2 for pause, 3 for resume, 4 for stop, 5 for "
        "next track, 6 for previous track.",
        PropertyList({Property(
            "controlMode", kPropertyTypeInteger, MusicPlayer::PlayControlMode::kPause,
            MusicPlayer::PlayControlMode::kPause, MusicPlayer::PlayControlMode::kPrevious)}),
        [this](const PropertyList& properties) -> ReturnValue {
            // Implement play logic here
            int controlMode = properties["controlMode"].value<int>();
            if (music_player_ == nullptr || music_player_->IsPlaying() == false) {
                return false;
            }
            music_player_->ChangePlayControlMode(
                static_cast<MusicPlayer::PlayControlMode>(controlMode));
            return true;
        });
    tools.push_back(tool);
}

void MusicManager::TryResleaseMusicPlayer() {
    if (music_player_ && !music_player_->IsPlaying()) {
        delete music_player_;
        music_player_ = nullptr;
    }
}
void MusicManager::HandleDeviceStateChange(const DeviceState& state) {
    if (state != kDeviceStateIdle && music_player_ && music_player_->IsPlaying()) {
        ESP_LOGI(TAG, "Device state changed to %d, pausing music playback", state);
        music_player_->ChangePlayControlMode(MusicPlayer::PlayControlMode::kPause);
    }
}