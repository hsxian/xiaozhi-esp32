#pragma once

#include <string>
#include "../music.h"
#include "../../common/query_base.h"
#include <vector>
#include <memory>
#include "../lyrics.h"

class MusicResource {
public:
    virtual ~MusicResource() = default;
    virtual bool Search(const QueryBase& query, std::vector<Music*>& music_list) = 0;
    bool Search(const std::string& url, std::vector<Music*>& music_list);
    virtual std::string GetUrl(Music& music) = 0;
    virtual std::string GetLyricsUrl(Music& music) = 0;
    virtual void ParseLyricsFromJson(const std::string& json, Lyrics& lyrics) = 0;
    void ParseLyricsFromJson(const std::string& json, const std::vector<const char*>& keys, Lyrics& lyrics);
    static std::unique_ptr<MusicResource> NewMusicResource();

private:
};