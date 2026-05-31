#include "music_manager.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <algorithm>
#include <format>
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"
#include "media/common/restful_client.h"
#include "mp3_music_player.h"
#include "music_resource.h"

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
        PropertyList({Property("controlMode", kPropertyTypeInteger,
                               (int)MusicPlayer::PlayControlMode::kPause,
                               (int)MusicPlayer::PlayControlMode::kPause,
                               (int)MusicPlayer::PlayControlMode::kPrevious)}),
        [this](const PropertyList& properties) -> ReturnValue {
            // Implement play logic here
            auto controlMode = properties["controlMode"].value<int>();
            if (music_player_ == nullptr || music_player_->IsPlaying() == false) {
                return false;
            }
            music_player_->ChangePlayControlMode(
                static_cast<MusicPlayer::PlayControlMode>(controlMode));
            return true;
        });
    tools.push_back(tool);

    // 搜索音乐，结果存入歌单
    tool = new McpTool(
        "self.music.search",
        "a tool to search music from the internet and store results in the "
        "playlist. Use self.music.playlist to view the playlist, then use "
        "self.music.play to play a song by index. You must provide a keyword "
        "to search, and you can also provide page number and page size for "
        "pagination.",
        PropertyList({Property("keyword", kPropertyTypeString),
                      Property("page", kPropertyTypeInteger, 1),
                      Property("pageSize", kPropertyTypeInteger, 10)}),
        [this](const PropertyList& properties) -> ReturnValue {
            QueryBase query;
            query.keyword = properties["keyword"].value<std::string>();
            query.page = properties["page"].value<int>();
            query.page_size = properties["pageSize"].value<int>();

            auto resource = MusicResource::NewMusicResource();
            std::vector<Music*> ms;
            resource->Search(query, ms);
            delete resource;
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage(
                "music", std::format("Search music result count: %d", ms.size()).c_str());
            if (ms.empty()) {
                return "No music found";
            }
            MusicHelper music_helper;
            std::vector<Music*> added_musics;
            std::vector<Music*> failed_musics;

            music_helper.TryAdd(music_list_, ms, added_musics, failed_musics);
            music_helper.Release(failed_musics);
            std::string msg = std::format("Added {} new song(s) to playlist, total {} song(s) : \n",
                                          added_musics.size(), music_list_.size());

            for (auto* music : added_musics) {
                msg += music->ToString() + "\n";
            }
            return msg;
        });
    tools.push_back(tool);

    // 播放歌单中的音乐
    tool = new McpTool(
        "self.music.play",
        "a tool to play music from the playlist. "
        "Use self.music.playlist to view the playlist first. "
        "loopMode: 0=play once, 1=loop, 2=shuffle (default 0).",
        PropertyList({Property("loopMode", kPropertyTypeInteger, 0, 0, 2)}),
        [this](const PropertyList& properties) -> ReturnValue {
            if (music_list_.empty()) {
                return "Playlist is empty, please search music first";
            }
            if (music_player_ && music_player_->IsPlaying()) {
                return "Music is already playing, please use self.music.control to resume it";
            }
            if (music_player_ == nullptr) {
                music_player_ = new Mp3MusicPlayer();
            }

            auto loop_mode =
                static_cast<MusicPlayer::LoopMode>(properties["loopMode"].value<int>());
            std::vector<const Music*> music_list_const(music_list_.begin(), music_list_.end());
            music_player_->Play(music_list_const, loop_mode);
            const char* mode_names[] = {"play once", "loop", "shuffle"};
            return std::format("Music playback started ({})",
                               mode_names[properties["loopMode"].value<int>()]);
        });
    tools.push_back(tool);

    // 查看歌单（支持关键字过滤和分页）
    tool = new McpTool("self.music.playlist",
                       "a tool to query the current playlist with optional keyword filter and "
                       "pagination. Returns matching music in JSON array. "
                       "Leave keyword empty to return all songs.",
                       PropertyList({Property("keyword", kPropertyTypeString, ""),
                                     Property("page", kPropertyTypeInteger, 1),
                                     Property("pageSize", kPropertyTypeInteger, 10)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           if (music_list_.empty()) {
                               return "Playlist is empty";
                           }
                           auto keyword = properties["keyword"].value<std::string>();
                           auto page = properties["page"].value<int>();
                           auto page_size = properties["pageSize"].value<int>();

                           MusicHelper music_helper;
                           auto result = music_helper.Search(music_list_, keyword, page, page_size);
                           if (result.empty()) {
                               return std::format("No music matching '{}'", keyword);
                           }
                           return music_helper.ToJsonArray(result);
                       });
    tools.push_back(tool);

    // 查看歌单数量
    tool = new McpTool("self.music.playlist.count",
                       "a tool to query the current playlist count. "
                       "Leave keyword empty to return all songs.",
                       PropertyList({Property("keyword", kPropertyTypeString, "")}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           if (music_list_.empty()) {
                               return "Playlist is empty";
                           }
                           auto keyword = properties["keyword"].value<std::string>();

                           if (keyword.empty()) {
                               return std::format("Playlist all count: {}", music_list_.size());
                           }

                           MusicHelper music_helper;
                           auto result =
                               music_helper.Search(music_list_, keyword, 1, music_list_.size());
                           // 只返回名称和歌手
                           if (result.empty()) {
                               return std::format("No music matching '{}'", keyword);
                           }
                           return std::format("Playlist matching count: {}", result.size());
                       });
    tools.push_back(tool);

    // 从歌单中删除音乐（按关键字匹配，关键字为空则清空全部）
    tool = new McpTool(
        "self.music.playlist.remove",
        "a tool to remove music from the playlist by keyword. "
        "Leave keyword empty to clear the entire playlist. "
        "Use self.music.playlist to query the playlist first.",
        PropertyList({Property("keyword", kPropertyTypeString, "")}),
        [this](const PropertyList& properties) -> ReturnValue {
            auto keyword = properties["keyword"].value<std::string>();
            if (music_list_.empty()) {
                return "Playlist is empty";
            }
            auto display = Board::GetInstance().GetDisplay();
            TryResleaseMusicPlayer();
            MusicHelper music_helper;
            // 关键字为空则清空全部歌单
            if (keyword.empty()) {
                int total = (int)music_list_.size();
                music_helper.Release(music_list_);
                auto msg = std::format("Cleared entire playlist, removed {} song(s)", total);
                display->SetChatMessage("music", msg.c_str());
                return msg;
            }

            // 使用MusicHelper::Search找到匹配的歌曲
            auto matches = music_helper.Search(music_list_, keyword, 1, music_list_.size());
            if (matches.empty()) {
                auto msg = std::format("No music matching '{}'", keyword);
                display->SetChatMessage("music", msg.c_str());
                return msg;
            }

            // 从music_list_中删除匹配的歌曲
            music_helper.Remove(music_list_, matches);
            auto msg = std::format("Removed {} song(s), remaining: {}", matches.size(),
                                   music_list_.size());
            display->SetChatMessage("music", msg.c_str());
            return msg;
        });
    tools.push_back(tool);

    // 播放状态查询
    tool = new McpTool("self.music.status", "a tool to get music status.", PropertyList(),
                       [this](const PropertyList& properties) -> ReturnValue {
                           if (!music_player_) {
                               return "No music player initialized,user use self.music.search and "
                                      "self.music.play to play";
                           }
                           switch (music_player_->GetPlayState()) {
                               case MusicPlayer::PlayState::kPlaying:
                                   return "playing";
                               case MusicPlayer::PlayState::kPaused:
                                   return "paused, use self.music.control with controlMode=3 to resume";
                               case MusicPlayer::PlayState::kResuming:
                                   return "resuming, please wait";
                               case MusicPlayer::PlayState::kIdle:
                                   return "idle, use self.music.play to play";
                               default:
                                   return "unknown";
                           }
                       });
    tools.push_back(tool);
}

void MusicManager::TryResleaseMusicPlayer() {
    if (music_player_ && !music_player_->IsPlaying()) {
        delete music_player_;
        music_player_ = nullptr;
    }
}
