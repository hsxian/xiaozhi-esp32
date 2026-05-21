#pragma once

#include <string>
#include "music.h"
#include "../common/query_base.h"
#include <vector>
#include "lyrics.h"

class MusicResource {
public:
    virtual ~MusicResource() = default;
    virtual bool Search(const QueryBase& query, std::vector<Music>& music_list) = 0;
    virtual std::string Search(const std::string & params) = 0;
    virtual void ParseMusicFromJson(cJSON* item, Music& music) = 0;
    virtual void ParseLyricsFromJson(const std::string& json, Lyrics& lyrics) = 0;
    static MusicResource* NewMusicResource();

private:
};