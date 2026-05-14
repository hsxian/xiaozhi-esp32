#pragma once

#include <string>
#include <vector>

struct cJSON;

class Music {
private:
    int rid;             // 音乐ID
    std::string pic;     // 封面图片URL
    std::string vid;     // 视频ID
    std::string name;    // 音乐名称
    std::string artist;  // 歌手
    std::string album;   // 专辑名称
    std::string lrc;     // 歌词URL
    std::string url;     // 音乐URL
public:
    Music();
    // 转换为JSON字符串
    std::string toJson() const;
    void toJson(cJSON* root) const;
    static std::string toJsonArray(std::vector<Music>& musics);
    // 从JSON字符串解析
    bool fromJson(const std::string& json);
};
