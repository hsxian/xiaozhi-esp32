#include "music_resource.h"
#include "cJSON.h"
#include "media/common/restful_client.h"
#include "media/common/string_helper.h"
#include "esp_log.h"

#ifdef CONFIG_ENABLE_CENGUIGUI_RESOURCE
#include "cenguigui_resource.h"
#endif

#ifdef CONFIG_ENABLE_SHANHAI_RESOURCE
#include "shanhai_resource.h"
#endif

#define TAG "MusicResource"

std::unique_ptr<MusicResource> MusicResource::NewMusicResource() {
    #ifdef CONFIG_ENABLE_CENGUIGUI_RESOURCE
        return std::make_unique<CenguiguiResource>();
    #elif defined(CONFIG_ENABLE_SHANHAI_RESOURCE)
        return std::make_unique<ShanhaiResource>();
    #else
        #error "Please enable at least one music resource"
    #endif
 }

 bool MusicResource::Search(const std::string& url, std::vector<Music*>& music_list) {
     RestfulClient restful_client;
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