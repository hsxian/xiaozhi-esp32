#include "cenguigui_resource.h"

#ifdef CONFIG_ENABLE_CENGUIGUI_RESOURCE

#include <esp_log.h>
#include <algorithm>
#include <format>
#include "cJSON.h"
#include "media/common/restful_client.h"
#include "media/common/string_helper.h"

#define TAG "CenguiguiResource"
#ifdef CONFIG_CENGUIGUI_RESOURCE_ADDRESS
_Static_assert(sizeof(CONFIG_CENGUIGUI_RESOURCE_ADDRESS) > 1,
               "CONFIG_CENGUIGUI_RESOURCE_ADDRESS不能为空");
#else
#error "CONFIG_CENGUIGUI_RESOURCE_ADDRESS is not defined"
#endif

bool CenguiguiResource::Search(const QueryBase& query, std::vector<Music*>& music_list) {
    std::string keyword = restful_client.UrlEncode(query.keyword);
    auto url = std::format("{}?name={}&page={}&limit={}", CONFIG_CENGUIGUI_RESOURCE_ADDRESS,
                           keyword, query.page, query.page_size);
    ESP_LOGI(TAG, "url: %s, keyword: %s", url.c_str(), query.keyword.c_str());
   
    return MusicResource::Search(url, music_list);
}

void CenguiguiResource::ParseLyricsFromJson(const std::string& json, Lyrics& lyrics) {
    auto json_obj = cJSON_Parse(json.c_str());
    if (!json_obj) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }
    cJSON* data = cJSON_GetObjectItem(json_obj, "data");
    if (data && cJSON_IsObject(data)) {
        cJSON* lrclist = cJSON_GetObjectItem(data, "lrclist");
        if (lrclist && cJSON_IsString(lrclist)) {
            lyrics.Parse(lrclist->valuestring);
        }
    } else {
        lyrics.Clear();
    }
    cJSON_Delete(json_obj);
}
void CenguiguiResource::ParseMusicFromJson(cJSON* item, Music& music) { music.FromJson(item); }

std::string CenguiguiResource::GetUrl(Music& music) { return music.url; }
std::string CenguiguiResource::GetLyricsUrl(Music& music) { return music.lrc; }

#endif