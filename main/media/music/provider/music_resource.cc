#include "music_resource.h"
#include "cJSON.h"
#include "media/common/restful_client.h"
#include "media/common/string_helper.h"
#include "media/common/json_helper.h"
#include "esp_log.h"

#ifdef CONFIG_ENABLE_CENGUIGUI_RESOURCE
#include "cenguigui_resource.h"
#endif

#ifdef CONFIG_ENABLE_SHANHAI_RESOURCE
#include "shanhai_resource.h"
#endif

#ifdef CONFIG_ENABLE_FENGYE_RESOURCE
#include "fengye_resource.h"
#endif

#ifdef CONFIG_ENABLE_LUOYUE_RESOURCE
#include "luoyue_resource.h"
#endif

#define TAG "MusicResource"

std::unique_ptr<MusicResource> MusicResource::NewMusicResource() {
    #ifdef CONFIG_ENABLE_CENGUIGUI_RESOURCE
        return std::make_unique<CenguiguiResource>();
    #elif defined(CONFIG_ENABLE_SHANHAI_RESOURCE)
        return std::make_unique<ShanhaiResource>();
    #elif defined(CONFIG_ENABLE_FENGYE_RESOURCE)
        return std::make_unique<FengyeResource>();
    #elif defined(CONFIG_ENABLE_LUOYUE_RESOURCE)
        return std::make_unique<LuoyueResource>();
    #else
        #error "Please enable at least one music resource"
    #endif
 }

 void MusicResource::ParseJsonArray(const cJSON* array, std::vector<Music*>& music_list) {
    MusicHelper music_helper;
    music_helper.FromJsonArray(const_cast<cJSON*>(array), music_list);
}

bool MusicResource::ParseResponse(const cJSON* json, std::vector<Music*>& music_list) {
    cJSON* code = cJSON_GetObjectItem(json, "code");
    if (code && cJSON_IsNumber(code) && code->valueint == 200) {
        cJSON* data = cJSON_GetObjectItem(json, "data");
        ParseJsonArray(data, music_list);
        return music_list.size() > 0;
    }
    ESP_LOGE(TAG, "Failed to parse JSON, code: %d", code ? code->valueint : -1);
    return false;
}

bool MusicResource::Search(const std::string& url, std::vector<Music*>& music_list) {
    return Search(url, {}, music_list);
}

bool MusicResource::Search(const std::string& url, const std::map<std::string, std::string>& headers, std::vector<Music*>& music_list) {
    RestfulClient restful_client;
    bool success = false;
    std::string response = restful_client.Get(url, headers);
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

    success = this->ParseResponse(json, music_list);
    cJSON_Delete(json);
    return success;
}

 void MusicResource::ParseLyricsFromJson(const std::string& json, const std::vector<const char*>& keys,
                                      Lyrics& lyrics) {
    auto json_obj = cJSON_Parse(json.c_str());
    if (!json_obj) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }
    JsonHelper json_helper;
    auto lrc_item = json_helper.GetObject(json_obj, keys);
    if (lrc_item && cJSON_IsString(lrc_item)) {
        lyrics.Parse(cJSON_GetStringValue(lrc_item));
    } else {
        lyrics.Clear();
    }
    cJSON_Delete(json_obj);
}