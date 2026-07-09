#pragma once

#include <map>
#include <string>
#include "../music.h"
#include "../../common/query_base.h"
#include <vector>
#include <memory>
#include "../lyrics.h"

struct cJSON;

class MusicResource {
public:
    virtual ~MusicResource() = default;
    virtual bool Search(const QueryBase& query, std::vector<Music*>& music_list) = 0;
    virtual bool GetFavoriteSongs(const int& count, std::vector<Music*>& music_list) = 0;
    bool Search(const std::string& url, std::vector<Music*>& music_list);
    bool Search(const std::string& url, const std::map<std::string, std::string>& headers,
                std::vector<Music*>& music_list);
    bool Search(const std::string& url, const std::map<std::string, std::string>& headers,
                const std::vector<const char*>& keysOfMusicJsonArray, std::vector<Music*>& music_list);
    virtual std::string GetUrl(Music& music) = 0;
    virtual std::string GetLyricsUrl(Music& music) = 0;
    virtual void ParseLyricsFromJson(const std::string& json, Lyrics& lyrics) = 0;
    virtual void ParseJsonArray(const cJSON* array, std::vector<Music*>& music_list);
    void ParseLyricsFromJson(const std::string& json, const std::vector<const char*>& keys, Lyrics& lyrics);
    static std::unique_ptr<MusicResource> NewMusicResource();

private:
};