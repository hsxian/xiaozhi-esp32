#include "sdkconfig.h"

#ifdef CONFIG_ENABLE_FENGYE_RESOURCE
#include "fengye_resource.h"

#include <esp_log.h>
#include <algorithm>
#include <format>
#include "cJSON.h"
#include "media/common/restful_client.h"
#include "media/common/string_helper.h"

#define TAG "FengyeResource"
#ifdef CONFIG_FENGYE_RESOURCE_ADDRESS
_Static_assert(sizeof(CONFIG_FENGYE_RESOURCE_ADDRESS) > 1,
               "CONFIG_FENGYE_RESOURCE_ADDRESS不能为空");
#else
#error "CONFIG_FENGYE_RESOURCE_ADDRESS is not defined"
#endif

void FengyeResource::ParseJsonArray(const cJSON* array, std::vector<Music*>& music_list) {
    cJSON* item = nullptr;
    cJSON_ArrayForEach (item, array) {
        auto music = new Music();
        music->FromJson(item);

        cJSON* id = cJSON_GetObjectItem(item, "id");
        if (id && cJSON_IsString(id)) {
            music->rid = id->valuestring;
        }

        music_list.push_back(music);
    }
}

bool FengyeResource::Search(const QueryBase& query, std::vector<Music*>& music_list) {
    RestfulClient restful_client;

    std::string keyword = restful_client.UrlEncode(query.keyword);
    auto url = std::format("{}/netease/search/song/?keywords={}&pn={}&limit={}",
                           CONFIG_FENGYE_RESOURCE_ADDRESS, keyword, query.page, query.page_size);
    ESP_LOGI(TAG, "url: %s, keyword: %s", url.c_str(), query.keyword.c_str());

    std::map<std::string, std::string> headers;
    headers["Referer"] = CONFIG_FENGYE_RESOURCE_REFERER;
    return MusicResource::Search(url, headers, {}, music_list);
}

bool FengyeResource::GetFavoriteSongs(const int& count, std::vector<Music*>& music_list) {
    return false;
}

void FengyeResource::ParseLyricsFromJson(const std::string& json, Lyrics& lyrics) {
    lyrics.Parse(json);
}

std::string FengyeResource::GetUrl(Music& music) { return music.url; }
std::string FengyeResource::GetLyricsUrl(Music& music) { return music.lrc; }

#endif