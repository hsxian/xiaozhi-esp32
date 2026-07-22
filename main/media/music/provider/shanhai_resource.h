#pragma once

#ifdef CONFIG_ENABLE_SHANHAI_RESOURCE

#include <string>
#include <vector>
#include "../../common/query_base.h"
#include "../lyrics.h"
#include "music_resource.h"

class ShanhaiResource : public MusicResource {
public:
    virtual ~ShanhaiResource() = default;
    virtual bool Search(const QueryBase& query, std::vector<Music*>& music_list) override;
    virtual bool GetFavoriteSongs(const int& count, std::vector<Music*>& music_list) override;
    virtual std::string GetUrl(Music& music) override;
    virtual std::string GetLyricsUrl(Music& music) override;
    virtual void ParseLyricsFromJson(const std::string& json, Lyrics& lyrics) override;

private:
};

#endif