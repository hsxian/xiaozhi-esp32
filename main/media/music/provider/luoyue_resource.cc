#include "luoyue_resource.h"

#ifdef CONFIG_ENABLE_LUOYUE_RESOURCE

#include <esp_log.h>
#include <algorithm>
#include <format>
#include "cJSON.h"
#include "media/common/json_helper.h"
#include "media/common/restful_client.h"
#include "media/common/string_helper.h"

#define TAG "LuoyueResource"
#ifdef CONFIG_LUOYUE_RESOURCE_ADDRESS
_Static_assert(sizeof(CONFIG_LUOYUE_RESOURCE_ADDRESS) > 1,
               "CONFIG_LUOYUE_RESOURCE_ADDRESS不能为空");
#else
#error "CONFIG_LUOYUE_RESOURCE_ADDRESS is not defined"
#endif

void LuoyueResource::ParseJsonArray(const cJSON* array, std::vector<Music*>& music_list) {
    cJSON* item = nullptr;
    JsonHelper json_helper;
    cJSON_ArrayForEach (item, array) {
        auto music = new Music();

        int value = 0;
        json_helper.GetNumber(item, "id", value);
        music->rid = std::to_string(value);
        json_helper.GetString(item, "mid", music->vid);
        json_helper.GetString(item, "song", music->name);
        json_helper.GetString(item, "singer", music->artist);
        json_helper.GetString(item, "album", music->album);

        music_list.push_back(music);
    }
}

bool LuoyueResource::Search(const QueryBase& query, std::vector<Music*>& music_list) {
    RestfulClient restful_client;

    std::string keyword = restful_client.UrlEncode(query.keyword);

    auto url = std::format("{}/v2/music/tencent/search/song?word={}&page={}&num={}",
                           CONFIG_LUOYUE_RESOURCE_ADDRESS, keyword, query.page, query.page_size);

    ESP_LOGI(TAG, "url: %s, keyword: %s", url.c_str(), query.keyword.c_str());

    return MusicResource::Search(url, {}, {"data"}, music_list);
}

bool LuoyueResource::GetFavoriteSongs(const int& count, std::vector<Music*>& music_list) {
    constexpr int kMaxNumPerPage = 50;
    int remaining = count;
    int page = 1;
    bool success = false;

    while (remaining > 0) {
        int num = std::min(remaining, kMaxNumPerPage);
        auto url =
            std::format("{}/v2/music/tencent/dissinfo?id={}&page={}&num={}",
                        CONFIG_LUOYUE_RESOURCE_ADDRESS, CONFIG_QQ_MUSIC_PLAYLIST_ID, page, num);

        std::vector<Music*> page_list;
        if (MusicResource::Search(url, {}, {"data", "list"}, page_list)) {
            music_list.insert(music_list.end(), page_list.begin(), page_list.end());
            success = true;
        } else {
            break;
        }

        remaining -= num;
        page++;
    }

    return success;
}

void LuoyueResource::ParseLyricsFromJson(const std::string& json, Lyrics& lyrics) {
    MusicResource::ParseLyricsFromJson(json, {"data", "lrc"}, lyrics);
}

std::string LuoyueResource::GetUrl(Music& music) {
    if (music.url.empty()) {
        RestfulClient restful_client;
        // 先获取歌曲信息（包含音质列表）

        auto url = std::format("{}/music/tencent/song/link?mid={}&quality=8", CONFIG_LUOYUE_RESOURCE_ADDRESS, music.vid);

        ESP_LOGI(TAG, "url: %s", url.c_str());
        auto response = restful_client.Get(url);
        if (!response.empty()) {
            auto json = cJSON_Parse(response.c_str());
            if (json) {
                JsonHelper json_helper;
                auto url_item = json_helper.GetObject(json, {"data", "url"});
                if (url_item && cJSON_IsString(url_item)) {
                    std::string url_str = cJSON_GetStringValue(url_item);
                    if (url_str.find(".mp3") != std::string::npos) {
                        music.url = url_str;
                    }
                }
                cJSON_Delete(json);
            }
        }
    }
    return music.url;
}
std::string LuoyueResource::GetLyricsUrl(Music& music) {
    if (music.lrc.empty()) {
        music.lrc = std::format("{}/v2/music/tencent/lyric?id={}", CONFIG_LUOYUE_RESOURCE_ADDRESS,
                                music.rid);
    }
    return music.lrc;
}

#endif