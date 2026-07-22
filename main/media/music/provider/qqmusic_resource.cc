#include "sdkconfig.h"

#ifdef CONFIG_ENABLE_QQ_MUSIC_RESOURCE
#include "qqmusic_resource.h"

#include <esp_log.h>
#include <algorithm>
#include <format>
#include "cJSON.h"
#include "media/common/json_helper.h"
#include "media/common/restful_client.h"
#include "media/common/string_helper.h"

#define TAG "QQMusicResource"
#ifdef CONFIG_QQ_MUSIC_RESOURCE_ADDRESS
_Static_assert(sizeof(CONFIG_QQ_MUSIC_RESOURCE_ADDRESS) > 1,
               "CONFIG_QQ_MUSIC_RESOURCE_ADDRESS不能为空");
#else
#error "CONFIG_QQ_MUSIC_RESOURCE_ADDRESS is not defined"
#endif

void QQMusicResource::ParseJsonArray(const cJSON* array, std::vector<Music*>& music_list) {
    cJSON* item = nullptr;
    JsonHelper json_helper;
    cJSON_ArrayForEach (item, array) {
        auto music = new Music();

        int value = 0;
        json_helper.GetNumber(item, "songid", value);
        music->rid = std::to_string(value);
        json_helper.GetString(item, "songmid", music->vid);
        auto jn = json_helper.GetArrayItem(item, {"singer"}, 0);
        if (jn) {
            json_helper.GetString(jn, "name", music->artist);
        }
        json_helper.GetString(item, "songname", music->name);
        json_helper.GetString(item, "albumname", music->album);

        music_list.push_back(music);
    }
}

bool QQMusicResource::Search(const QueryBase& query, std::vector<Music*>& music_list) {
    RestfulClient restful_client;

    std::string keyword = restful_client.UrlEncode(query.keyword);

    auto url = std::format("{}/soso/fcgi-bin/search_for_qq_cp?format=json&ie=utf-8&w={}&p={}&n={}",
                           CONFIG_QQ_MUSIC_RESOURCE_ADDRESS, keyword, query.page, query.page_size);

    ESP_LOGI(TAG, "url: %s, keyword: %s", url.c_str(), query.keyword.c_str());

    std::map<std::string, std::string> headers;
    headers["Referer"] = CONFIG_QQ_MUSIC_RESOURCE_ADDRESS;
#ifdef CONFIG_QQ_MUSIC_COOKIE
    if (sizeof(CONFIG_QQ_MUSIC_COOKIE) > 1) {
        headers["Cookie"] = CONFIG_QQ_MUSIC_COOKIE;
    }
#endif

    return MusicResource::Search(url, headers, {"data", "song", "list"}, music_list);
}

bool QQMusicResource::GetFavoriteSongs(const int& count, std::vector<Music*>& music_list) {
    constexpr int kMaxNumPerPage = 50;
    int remaining = count;
    int page = 0;
    bool success = false;

#ifdef CONFIG_QQ_MUSIC_FAVORITE_ID
    while (remaining > 0) {
        int num = std::min(remaining, kMaxNumPerPage);
        auto url = std::format(
            "{}/qzone/fcg-bin/"
            "fcg_ucc_getcdinfo_byids_cp.fcg?type=1&utf8=1&onlysong=1&disstid={}&format=json&song_"
            "begin={}&song_num={}",
            CONFIG_QQ_MUSIC_RESOURCE_ADDRESS, CONFIG_QQ_MUSIC_FAVORITE_ID, page * num, num);

        std::map<std::string, std::string> headers;
        headers["Referer"] = CONFIG_QQ_MUSIC_RESOURCE_ADDRESS;
        headers["Referer"] += "/";
#ifdef CONFIG_QQ_MUSIC_COOKIE
        if (sizeof(CONFIG_QQ_MUSIC_COOKIE) > 1) {
            headers["Cookie"] = CONFIG_QQ_MUSIC_COOKIE;
        }
#endif

        std::vector<Music*> page_list;
        if (MusicResource::Search(url, headers, {"songlist"}, page_list)) {
            music_list.insert(music_list.end(), page_list.begin(), page_list.end());
            success = true;
        } else {
            break;
        }

        remaining -= num;
        page++;
    }
#endif

    return success;
}

void QQMusicResource::ParseLyricsFromJson(const std::string& json, Lyrics& lyrics) {
    // lyrics.Parse(json);
    MusicResource::ParseLyricsFromJson(json, {"song_lyric"}, lyrics);
}

std::string QQMusicResource::GetUrl(Music& music) {
    if (music.url.empty()) {
        RestfulClient restful_client;

        auto url = std::format("{}/music_open_api.php?type=json&mid={}", CONFIG_QQ_MUSIC_COOKIE,
                               music.vid);
        auto response = restful_client.Get(url);
        if (!response.empty()) {
            auto json = cJSON_Parse(response.c_str());
            if (json) {
                JsonHelper json_helper;
                std::string url_str;
                if (json_helper.GetString(json, "song_play_url_hq", url_str)) {
                    music.url = url_str;
                }
                if (url_str.empty() && json_helper.GetString(json, "song_play_url", url_str)) {
                    music.url = url_str;
                }
                cJSON_Delete(json);
            }
        }
    }
    return music.url;
}

std::string QQMusicResource::GetLyricsUrl(Music& music) {
    if (music.lrc.empty()) {
        auto url = std::format("{}/music_open_api.php?type=json&mid={}", CONFIG_QQ_MUSIC_COOKIE,
                               music.vid);
        music.lrc = url;
    }
    return music.lrc;
}

#endif