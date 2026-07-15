#include "sdkconfig.h"

#ifdef CONFIG_ENABLE_SHANHAI_RESOURCE
#include "shanhai_resource.h"

#include <esp_log.h>
#include <algorithm>
#include <format>
#include "cJSON.h"
#include "media/common/json_helper.h"
#include "media/common/restful_client.h"
#include "media/common/string_helper.h"

#define TAG "ShanhaiResource"
#ifdef CONFIG_SHANHAI_RESOURCE_ADDRESS
_Static_assert(sizeof(CONFIG_SHANHAI_RESOURCE_ADDRESS) > 1,
               "CONFIG_SHANHAI_RESOURCE_ADDRESS不能为空");
#else
#error "CONFIG_SHANHAI_RESOURCE_ADDRESS is not defined"
#endif

bool ShanhaiResource::Search(const QueryBase& query, std::vector<Music*>& music_list) {
    RestfulClient restful_client;
    
    std::string keyword = restful_client.UrlEncode(query.keyword);
    auto url = std::format("{}/?action=search&keyword={}&page={}&size={}&key={}",
                           CONFIG_SHANHAI_RESOURCE_ADDRESS, keyword, query.page, query.page_size,
                           CONFIG_SHANHAI_RESOURCE_API_KEY);

    ESP_LOGI(TAG, "url: %s, keyword: %s", url.c_str(), query.keyword.c_str());

    return MusicResource::Search(url, music_list);
}

bool ShanhaiResource::GetFavoriteSongs(const int& count, std::vector<Music*>& music_list) {
    return false;
}

void ShanhaiResource::ParseLyricsFromJson(const std::string& json, Lyrics& lyrics) {
    MusicResource::ParseLyricsFromJson(json, {"data", "lyric_text"}, lyrics);
}

std::string ShanhaiResource::GetUrl(Music& music) {
    if (music.url.empty()) {
        auto url =
            std::format("{}/?action=music_url&music_id={}&key={}", CONFIG_SHANHAI_RESOURCE_ADDRESS,
                        music.rid, CONFIG_SHANHAI_RESOURCE_API_KEY);

        RestfulClient restful_client;
        std::string response = restful_client.Get(url);
        if (!response.empty()) {
            auto json = cJSON_Parse(response.c_str());
            if (json) {
                JsonHelper json_helper;
                auto url_item = json_helper.GetObject(json, {"data", "url"});
                if (url_item && cJSON_IsString(url_item)) {
                    music.url = cJSON_GetStringValue(url_item);
                }
                cJSON_Delete(json);
            }
        }
    }
    return music.url;
}
std::string ShanhaiResource::GetLyricsUrl(Music& music) {
    if (music.lrc.empty()) {
        music.lrc =
            std::format("{}/?action=lyric&music_id={}&key={}", CONFIG_SHANHAI_RESOURCE_ADDRESS,
                        music.rid, CONFIG_SHANHAI_RESOURCE_API_KEY);
    }
    return music.lrc;
}

#endif