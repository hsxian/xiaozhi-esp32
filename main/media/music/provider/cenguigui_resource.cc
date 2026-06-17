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
_Static_assert(sizeof(CONFIG_CENGUIGUI_RESOURCE_ADDRESS) > 1, "CONFIG_CENGUIGUI_RESOURCE_ADDRESS不能为空");
#else
#error "CONFIG_CENGUIGUI_RESOURCE_ADDRESS is not defined"
#endif
// 辅助函数：替换字符串中的子串

bool CenguiguiResource::Search(const QueryBase& query, std::vector<Music*>& music_list) {
    RestfulClient restful_client;
    std::string url = std::format("{}?name={}&page={}&limit={}", CONFIG_CENGUIGUI_RESOURCE_ADDRESS,
                                  query.keyword, query.page, query.page_size);
    ESP_LOGI(TAG, "url: %s", url.c_str());
    std::string keyword = restful_client.UrlEncode(query.keyword);
    url = std::format("{}?name={}&page={}&limit={}", CONFIG_CENGUIGUI_RESOURCE_ADDRESS, keyword, query.page,
                      query.page_size);
    bool success = false;
    std::string response = restful_client.Get(url);
    if (response.empty()) {
        return success;
    }
    StringHelper string_helper;
    response = string_helper.ReplaceStringAll(response, "http://", "https://");

    auto json = cJSON_Parse(response.c_str());
    if (!json) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return success;
    }

    cJSON* code = cJSON_GetObjectItem(json, "code");
    if (code && cJSON_IsNumber(code) && code->valueint == 200) {
        cJSON* data = cJSON_GetObjectItem(json, "data");

        MusicHelper music_helper;
        music_helper.FromJsonArray(data, music_list);
        success = music_list.size() > 0;
    } else {
        ESP_LOGE(TAG, "Failed to parse JSON, code: %d", code->valueint);
    }
    cJSON_Delete(json);
    return success;
}

std::string CenguiguiResource::Search(const std::string& params) {
    RestfulClient restful_client;
    std::string url = std::format("{}?{}", CONFIG_CENGUIGUI_RESOURCE_ADDRESS, params);
    ESP_LOGI(TAG, "url: %s", url.c_str());

    std::string response = restful_client.Get(url);
    if (!response.empty()) {
        StringHelper string_helper;
        response = string_helper.ReplaceStringAll(response, "http://", "https://");
    }

    return response;
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

std::string CenguiguiResource::GetUrl(Music& music) {
    return music.url;
}
std::string CenguiguiResource::GetLyricsUrl(Music& music) {
    return music.lrc;
}

#endif