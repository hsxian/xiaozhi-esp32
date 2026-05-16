#pragma once

#include <string>
#include <vector>

struct cJSON;

struct Music {
private:
public:
    int rid;             // 音乐ID
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
    // 从JSON字符串解析
    bool FromJson(const std::string& json);
    void FromJson(const cJSON* item);
    static std::string ToJsonArray(std::vector<Music>& musics);
    static void FromJsonArray(cJSON* array, std::vector<Music>& musics);
};
