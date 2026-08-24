#include "music_manager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <format>
#include "board.h"
#include "display.h"
#include "mcp_server.h"
#include "media/common/restful_client.h"
#include "player/music_player.h"
#include "provider/music_resource.h"

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
        "next track, 6 for previous track. "
        "IMPORTANT: Only one command is needed at a time. For example, calling 5 (next track) "
        "will automatically resume playback, so do NOT call 3 (resume) right after 5. "
        "Similarly, calling 6 (previous track) will also auto-play.",
        PropertyList({Property("controlMode", kPropertyTypeInteger,
                               (int)MusicPlayer::PlayControlMode::kPause,
                               (int)MusicPlayer::PlayControlMode::kPause,
                               (int)MusicPlayer::PlayControlMode::kPrevious)}),
        [this](const PropertyList& properties) -> ReturnValue {
            auto controlMode = properties["controlMode"].value<int>();
            if (music_player_ == nullptr) {
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
        "self.music.play to play a song by index. You must provide a keyword to "
        "search, and you can also provide page number and page size for pagination.",
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
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage(
                "music", std::format("Search music result count: {}", ms.size()).c_str());
            std::vector<Music*> added_musics;
            EnsureMusicPlayer();
            auto added_count = music_player_->AddToPlaylist(ms, &added_musics);
            if (added_count == 0) {
                return "No music found";
            }
            cJSON* root = cJSON_CreateObject();
            cJSON_AddNumberToObject(root, "added_count", added_count);
            cJSON_AddNumberToObject(root, "total_count", music_player_->GetPlaylist().size());

            cJSON* songs_array = cJSON_CreateArray();
            for (auto* music : added_musics) {
                cJSON* song_obj = cJSON_CreateObject();
                music->ToJsonSimple(song_obj);
                cJSON_AddItemToArray(songs_array, song_obj);
            }
            cJSON_AddItemToObject(root, "songs", songs_array);

            auto json_str = cJSON_Print(root);
            cJSON_Delete(root);
            return json_str;
        });
    tools.push_back(tool);

    // 获取收藏音乐
    tool = new McpTool(
        "self.music.favorite",
        "a tool to get favorite songs from the music resource. "
        "You can specify the number of favorite songs to retrieve, "
        "and it will store the songs in the playlist, then return count of the favorite songs. "
        "IMPORTANT: Only call this tool once at a time, do NOT call it again before the previous "
        "call returns.",
        PropertyList({Property("count", kPropertyTypeInteger, 100)}),
        [this](const PropertyList& properties) -> ReturnValue {
            auto count = properties["count"].value<int>();
            auto resource = MusicResource::NewMusicResource();
            std::vector<Music*> ms;
            resource->GetFavoriteSongs(count, ms);
            EnsureMusicPlayer();
            auto added_count = music_player_->AddToPlaylist(ms);
            if (added_count == 0) {
                return "No favorite songs found";
            }
            return std::format("Add new songs count: {}, total playlist count: {}", added_count,
                               music_player_->GetPlaylist().size());
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
            if (music_player_ == nullptr || music_player_->GetPlaylist().empty()) {
                return "Playlist is empty, please search music first";
            }
            if (music_player_->IsPlaying()) {
                return "Music is already playing, please use self.music.control to resume it";
            }

            auto loop_mode =
                static_cast<MusicPlayer::LoopMode>(properties["loopMode"].value<int>());
            music_player_->Play(loop_mode);
            const char* mode_names[] = {"play once", "loop", "shuffle"};
            return std::format("Music playback started ({})", mode_names[(int)loop_mode]);
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
                           if (music_player_ == nullptr || music_player_->GetPlaylist().empty()) {
                               return "Playlist is empty";
                           }
                           auto keyword = properties["keyword"].value<std::string>();
                           auto page = properties["page"].value<int>();
                           auto page_size = properties["pageSize"].value<int>();

                           MusicHelper music_helper;
                           auto& playlist = music_player_->GetPlaylist();
                           auto result = music_helper.Search(playlist, keyword, page, page_size);
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
                           if (music_player_ == nullptr || music_player_->GetPlaylist().empty()) {
                               return "Playlist is empty";
                           }
                           auto keyword = properties["keyword"].value<std::string>();

                           if (keyword.empty()) {
                               return std::format("Playlist all count: {}",
                                                  music_player_->GetPlaylist().size());
                           }

                           MusicHelper music_helper;
                           auto& playlist = music_player_->GetPlaylist();
                           auto result = music_helper.Search(playlist, keyword, 1, playlist.size());
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
            if (music_player_ == nullptr) {
                return "Playlist is empty";
            }
            MusicHelper music_helper;
            auto& playlist = music_player_->GetPlaylist();
            std::string msg;
            // 关键字为空则清空全部歌单
            if (keyword.empty()) {
                int total = (int)playlist.size();
                msg = std::format("Cleared entire playlist, removed {} song(s)", total);
                music_helper.Release(playlist);
            } else {
                // 使用MusicHelper::Search找到匹配的歌曲
                auto matches = music_helper.Search(playlist, keyword, 1, playlist.size());
                if (matches.empty()) {
                    msg = std::format("No music matching '{}'", keyword);
                } else {
                    // 从歌单中删除匹配的歌曲
                    music_helper.Remove(playlist, matches);
                    msg = std::format("Removed {} song(s), remaining: {}", matches.size(),
                                      playlist.size());
                }
            }
            ShowMusicMessage(msg);
            if (playlist.empty()) {
                TryReleaseMusicPlayer();
            }
            return msg;
        });
    tools.push_back(tool);

    // 播放状态查询
    tool = new McpTool(
        "self.music.status", "a tool to get music status.", PropertyList(),
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

void MusicManager::TryReleaseMusicPlayer() {
    music_player_.reset();
    vTaskDelay(pdMS_TO_TICKS(500));
}

void MusicManager::EnsureMusicPlayer() {
    if (music_player_ == nullptr) {
        music_player_ = MusicPlayer::NewMusicPlayer();
    }
}

void MusicManager::ShowMusicMessage(const std::string& msg) {
    auto display = Board::GetInstance().GetDisplay();
    display->SetChatMessage("music", msg.c_str());
}