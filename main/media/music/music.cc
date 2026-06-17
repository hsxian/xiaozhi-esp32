#include "music.h"
#include <format>
#include <string>
#include <vector>
#include "../common/json_helper.h"
#include "cJSON.h"
#include "esp_log.h"

#define TAG "Music"

void Music::ToJsonSimple(cJSON* root) const {
    cJSON_AddStringToObject(root, "name", name.c_str());
    cJSON_AddStringToObject(root, "artist", artist.c_str());
    cJSON_AddStringToObject(root, "album", album.c_str());
}

void Music::ToJson(cJSON* root) const {
    cJSON_AddNumberToObject(root, "rid", rid);
    cJSON_AddStringToObject(root, "pic", pic.c_str());
    cJSON_AddStringToObject(root, "vid", vid.c_str());
    cJSON_AddStringToObject(root, "lrc", lrc.c_str());
    cJSON_AddStringToObject(root, "url", url.c_str());
    ToJsonSimple(root);
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
    JsonHelper json_helper;
    json_helper.GetNumber(item, "rid", rid);
    json_helper.GetString(item, "pic", pic);
    json_helper.GetString(item, "vid", vid);
    json_helper.GetString(item, "name", name);
    json_helper.GetString(item, "artist", artist);
    json_helper.GetString(item, "album", album);
    json_helper.GetString(item, "lrc", lrc);
    json_helper.GetString(item, "url", url);
}

std::string Music::ToString() const {
    // 美化输出音乐信息，太长了不方便查看，暂时只输出部分字段
    return std::format("Music[name={}, artist={}, album={}]", name, artist, album);
}

void MusicHelper::FromJsonArray(cJSON* array, std::vector<Music*>& musics) {
    cJSON* item = nullptr;
    cJSON_ArrayForEach (item, array) {
        auto music = new Music();
        music->FromJson(item);
        musics.push_back(music);
    }
}

std::string MusicHelper::ToJsonArray(std::vector<Music*>& musics) {
    cJSON* array = cJSON_CreateArray();
    for (auto* music : musics) {
        cJSON* root = cJSON_CreateObject();
        music->ToJson(root);
        cJSON_AddItemToArray(array, root);
    }
    auto json_str = cJSON_Print(array);
    cJSON_Delete(array);
    return json_str;
}


std::vector<Music*> MusicHelper::Search(std::vector<Music*>& musics, const std::string& keyword, int page,
                                  int page_size) {
    // 简单的搜索实现，根据关键词和分页参数返回匹配的音乐
    std::vector<Music*> result;
    if (keyword.empty()) {
        result.assign(musics.begin(), musics.end());
    } else {
        for (auto* music : musics) {
            if (music->name.find(keyword) != std::string::npos ||
                music->artist.find(keyword) != std::string::npos ||
                music->album.find(keyword) != std::string::npos) {
                result.push_back(music);
            }
        }
    }
    // 分页处理
    int start = (page - 1) * page_size;
    if (start >= static_cast<int>(result.size())) {
        result.clear();
        return result;
    }
    int end = std::min(start + page_size, (int)result.size());
    result.assign(result.begin() + start, result.begin() + end);
    return result;
}
bool MusicHelper::Contains(std::vector<Music*>& musics, Music* music) {
    for (auto* m : musics) {
        if (m->rid == music->rid||m->name == music->name) {
            return true;
        }
    }
    return false;
}
void MusicHelper::TryAdd(std::vector<Music*>& musics, std::vector<Music*>& new_musics, std::vector<Music*>& added_musics, std::vector<Music*>& failed_musics) {
    for (auto* music : new_musics) {
        if (Contains(musics, music)) {
            failed_musics.push_back(music);
        } else {
            musics.push_back(music);
            added_musics.push_back(music);
        }
    }
}
void MusicHelper::Release(std::vector<Music*>& musics) {
    for (auto* music : musics) {
        delete music;
    }
    musics.clear();
}

void MusicHelper::Remove(std::vector<Music*>& musics, std::vector<Music*>& removed_musics) {
    for (auto* music : removed_musics) {
        auto it = std::find(musics.begin(), musics.end(), music);
        if (it != musics.end()) {
            delete *it;
            musics.erase(it);
        }
    }
}