#pragma once

#include <string>
#include <vector>

struct cJSON;

struct Music {
private:
public:
    std::string rid;     // 音乐ID
    std::string pic;     // 封面图片URL
    std::string vid;     // 视频ID
    std::string name;    // 音乐名称
    std::string artist;  // 歌手
    std::string album;   // 专辑名称
    std::string lrc;     // 歌词URL
    std::string url;     // 音乐URL

    // 转换为JSON字符串
    std::string ToJson() const;
    void ToJson(cJSON* root) const;
    void ToJsonSimple(cJSON* root) const;
    // 从JSON字符串解析
    bool FromJson(const std::string& json);
    void FromJson(const cJSON* item);
    std::string ToString() const;
};

class MusicHelper {
public:
    std::string ToJsonArray(std::vector<Music*>& musics);
    void FromJsonArray(cJSON* array, std::vector<Music*>& musics);
    std::vector<Music*> Search(std::vector<Music*>& musics, const std::string& keyword,
                               int page = 1, int page_size = 10);
    bool Contains(std::vector<Music*>& musics, Music* music);
    void TryAdd(std::vector<Music*>& musics, std::vector<Music*>& new_musics,
                std::vector<Music*>& added_musics, std::vector<Music*>& failed_musics);
    void Remove(std::vector<Music*>& musics, std::vector<Music*>& removed_musics);
    void Release(std::vector<Music*>& musics);
};