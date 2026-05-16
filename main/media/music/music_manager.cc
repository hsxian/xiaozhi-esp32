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
                                          Property("page", kPropertyTypeInteger),
                                          Property("pageSize", kPropertyTypeInteger)}),
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
            if (mp3_player_ == nullptr) {
                mp3_player_ = new Mp3MusicPlayer();
            }
            if (is_search_result_or_music_list) {
                if (!music_search_list_.empty()) {
                    mp3_player_->Play(music_search_list_);
                    return true;
                }
            } else {
                if (!music_list_.empty()) {
                    mp3_player_->Play(music_list_);
                    return true;
                }
            }
            if (!mp3_player_->IsPlaying()) {
                delete mp3_player_;
                mp3_player_ = nullptr;
            }
            return false;
        });
    tools.push_back(tool);
}
