#include "kw_music_resource.h"

#include <esp_log.h>
#include <format>
#include <algorithm>
#include "../restful_client.h"
#include "cJSON.h"

#define TAG "KwMusicResource"

// 辅助函数：替换字符串中的子串
static std::string ReplaceString(const std::string& str, const std::string& from, const std::string& to) {
    std::string result = str;
    size_t start_pos = result.find(from);
    if (start_pos != std::string::npos) {
        result.replace(start_pos, from.length(), to);
    }
    return result;
}

bool KwMusicResource::Search(const QueryBase& query, std::vector<Music>& music_list) {

    RestfulClient restful_client;
    std::string keyword = restful_client.UrlEncode(query.keyword);
    std::string url = std::format("https://kw-api.cenguigui.cn/?name={}&page={}&limit={}", keyword,
                                  query.page, query.page_size);
    ESP_LOGI(TAG, "url: %s", url.c_str());

    bool success = false;
    std::string response = restful_client.Get(url);
    if (response.empty()) {
        return success;
    }

    auto json = cJSON_Parse(response.c_str());
    if (!json) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return success;
    }

    cJSON* code = cJSON_GetObjectItem(json, "code");
    if (code && cJSON_IsNumber(code) && code->valueint == 200) {
        cJSON* data = cJSON_GetObjectItem(json, "data");
        Music::FromJsonArray(data, music_list);
        for (auto& music : music_list) {
            // 替换url中level=exhigh为level=standard，获取标准品质的音乐URL
            music.url = ReplaceString(music.url, "level=exhigh", "level=standard");
        }
        success = music_list.size() > 0;
    } else {
        ESP_LOGE(TAG, "Failed to parse JSON, code: %d", code->valueint);
    }
    cJSON_Delete(json);
    return success;
}