#include "music.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string>
#include <vector>

#define TAG "Music"

Music::Music()
{
}

void Music::toJson(cJSON* root) const
{
    cJSON_AddNumberToObject(root, "rid", rid);
    cJSON_AddStringToObject(root, "pic", pic.c_str());
    cJSON_AddStringToObject(root, "vid", vid.c_str());
    cJSON_AddStringToObject(root, "name", name.c_str());
    cJSON_AddStringToObject(root, "artist", artist.c_str());
    cJSON_AddStringToObject(root, "album", album.c_str());
    cJSON_AddStringToObject(root, "lrc", lrc.c_str());
    cJSON_AddStringToObject(root, "url", url.c_str());
}
std::string Music::toJson() const
{
    cJSON* root = cJSON_CreateObject();
    toJson(root);
    auto json_str = cJSON_Print(root);
    cJSON_Delete(root);
    return json_str;
}
bool Music::fromJson(const std::string& json)
{
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return false;
    }
    rid = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "rid"));
    pic = cJSON_GetStringValue(cJSON_GetObjectItem(root, "pic"));
    vid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "vid"));
    name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    artist = cJSON_GetStringValue(cJSON_GetObjectItem(root, "artist"));
    album = cJSON_GetStringValue(cJSON_GetObjectItem(root, "album"));
    lrc = cJSON_GetStringValue(cJSON_GetObjectItem(root, "lrc"));
    url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));

    cJSON_Delete(root);
    return true;
}

std::string Music::toJsonArray(std::vector<Music>& musics)
{
    cJSON* array = cJSON_CreateArray();
    for (const auto& music : musics) {
        cJSON* item = cJSON_CreateObject();
        music.toJson(item);
        cJSON_AddItemToArray(array, item);
    }
    auto json_str = cJSON_Print(array);
    cJSON_Delete(array);
    return json_str;
}