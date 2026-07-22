#include "sdkconfig.h"

#ifdef CONFIG_ENABLE_CENGUIGUI_RESOURCE
#include "cenguigui_resource.h"

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
    RestfulClient restful_client;

    std::string keyword = restful_client.UrlEncode(query.keyword);
    auto url = std::format("{}/?name={}&page={}&limit={}", CONFIG_CENGUIGUI_RESOURCE_ADDRESS,
                           keyword, query.page, query.page_size);
    ESP_LOGI(TAG, "url: %s, keyword: %s", url.c_str(), query.keyword.c_str());

    return MusicResource::Search(url, music_list);
}

bool CenguiguiResource::GetFavoriteSongs(const int& count, std::vector<Music*>& music_list) {
    return false;
}

void CenguiguiResource::ParseLyricsFromJson(const std::string& json, Lyrics& lyrics) {
    MusicResource::ParseLyricsFromJson(json, {"data", "lrclist"}, lyrics);
}

std::string CenguiguiResource::GetUrl(Music& music) { return music.url; }
std::string CenguiguiResource::GetLyricsUrl(Music& music) { return music.lrc; }

#endif