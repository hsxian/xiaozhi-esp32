#include "music.h"
#include <string>
#include <vector>
#include "cJSON.h"
#include "esp_log.h"

#define TAG "Music"

void Music::ToJson(cJSON* root) const {
    cJSON_AddNumberToObject(root, "rid", rid);
    cJSON_AddStringToObject(root, "pic", pic.c_str());
    cJSON_AddStringToObject(root, "vid", vid.c_str());
    cJSON_AddStringToObject(root, "name", name.c_str());
    cJSON_AddStringToObject(root, "artist", artist.c_str());
    cJSON_AddStringToObject(root, "album", album.c_str());
    cJSON_AddStringToObject(root, "lrc", lrc.c_str());
    cJSON_AddStringToObject(root, "url", url.c_str());
}
std::string Music::ToJson() const {
    cJSON* root = cJSON_CreateObject();
    ToJson(root);
    auto json_str = cJSON_Print(root);
    cJSON_Delete(root);
    return json_str;
}
bool Music::FromJson(const std::string& json) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return false;
    }
    FromJson(root);

    cJSON_Delete(root);
    return true;
}
void Music::FromJson(const cJSON* item) {
    rid = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "rid"));
    pic = cJSON_GetStringValue(cJSON_GetObjectItem(item, "pic"));
    vid = cJSON_GetStringValue(cJSON_GetObjectItem(item, "vid"));
    name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
    artist = cJSON_GetStringValue(cJSON_GetObjectItem(item, "artist"));
    album = cJSON_GetStringValue(cJSON_GetObjectItem(item, "album"));
    lrc = cJSON_GetStringValue(cJSON_GetObjectItem(item, "lrc"));
    url = cJSON_GetStringValue(cJSON_GetObjectItem(item, "url"));
}

void Music::FromJsonArray(cJSON* array, std::vector<Music>& musics) {
    cJSON* item = nullptr;
    cJSON_ArrayForEach (item, array) {
        Music music;
        music.FromJson(item);
        musics.push_back(std::move(music));
    }
}

std::string Music::ToJsonArray(std::vector<Music>& musics) {
    cJSON* array = cJSON_CreateArray();
    for (auto& music : musics) {
        cJSON* root = cJSON_CreateObject();
        music.ToJson(root);
        cJSON_AddItemToArray(array, root);
    }
    auto json_str = cJSON_Print(array);
    cJSON_Delete(array);
    return json_str;
}
