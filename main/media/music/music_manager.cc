#include "music_manager.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include "audio_codec.h"
#include "board.h"
#include "kw_music_resource.h"
#include "mcp_server.h"
#include "media/restful_client.h"
#include "mp3_music_player.h"

#define TAG "MusicManager"

MusicManager::MusicManager() {}

MusicManager& MusicManager::GetInstance() {
    static MusicManager instance;
    return instance;
}

void MusicManager::GenerateMcpServerTools(std::vector<McpTool*>& tools) {
    // 播放音乐控制
    auto tool = new McpTool(
        "self.music.control",
        "a tool to control music playback can pause, resume, stop, next track, previous track. You "
        "must provide the control mode to use. The control "
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
    // 搜索音乐
    tool = new McpTool("self.music.search",
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
                           RestfulClient restful_client;
                           std::string keyword = restful_client.UrlEncode(query.keyword);
                           auto parms = std::format("name={}&page={}&limit={}", keyword, query.page,
                                                    query.page_size);
                           auto search_result = resource.Search(parms);
                           return search_result;
                       });
    tools.push_back(tool);

    // 播放音乐
    // std::string pic;     // 封面图片URL
    // std::string vid;     // 视频ID
    // std::string name;    // 音乐名称
    // std::string artist;  // 歌手
    // std::string album;   // 专辑名称
    // std::string lrc;     // 歌词URL
    // std::string url;     // 音乐URL
    tool = new McpTool(
        "self.music.play",
        "a tool to play music by provide the json array(.eg "
        "[{\"name\":\"music_name\",\"artist\":\"artist_name\",\"url\":\"music_url\",\"album\":\"album_name\"}]) of the "
        "music to play.",
        PropertyList({Property("musicList", kPropertyTypeString)}),
        [this](const PropertyList& properties) -> ReturnValue {
            // Implement play logic here
            auto music_list = properties["musicList"].value<std::string>();
            music_search_list_.clear();
            auto json = cJSON_Parse(music_list.c_str());
            if (json == nullptr) {
                auto error_ptr = cJSON_GetErrorPtr();
                ESP_LOGE(TAG, "Failed to parse JSON: %s", error_ptr ? error_ptr : "Unknown error");
                return "Failed to parse JSON";
            }

            Music::FromJsonArray(json, music_search_list_);
            cJSON_Delete(json);
            if (music_player_ == nullptr) {
                music_player_ = new Mp3MusicPlayer();
            }
            auto musics = music_search_list_.empty() ? music_list_ : music_search_list_;
            if (!musics.empty()) {
                music_player_->Play(musics);
                return "Music playback started";
            }
            TryResleaseMusicPlayer();
            return "Failed to play music";
        });
    tools.push_back(tool);
}

void MusicManager::TryResleaseMusicPlayer() {
    if (music_player_ && !music_player_->IsPlaying()) {
        delete music_player_;
        music_player_ = nullptr;
    }
}