#pragma once

#include <string>
#include <vector>
#include "../../common/query_base.h"
#include "music_resource.h"
#include "../lyrics.h"

class CenguiguiResource : public MusicResource {
public:
    virtual ~CenguiguiResource() = default;
    virtual bool Search(const QueryBase& query, std::vector<Music*>& music_list) override;
    virtual std::string Search(const std::string & params) override;
    virtual std::string GetUrl(Music& music) override;
    virtual std::string GetLyricsUrl(Music& music) override;
    virtual void ParseMusicFromJson(cJSON* item, Music& music) override;
    virtual void ParseLyricsFromJson(const std::string& json, Lyrics& lyrics) override;
    
private:
};