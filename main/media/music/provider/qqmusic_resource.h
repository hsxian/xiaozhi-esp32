#pragma once

#ifdef CONFIG_ENABLE_QQ_MUSIC_RESOURCE

#include <string>
#include <vector>
#include "../../common/query_base.h"
#include "../lyrics.h"
#include "music_resource.h"

class QQMusicResource : public MusicResource {
public:
    virtual ~QQMusicResource() = default;
    virtual bool Search(const QueryBase& query, std::vector<Music*>& music_list) override;
    virtual bool GetFavoriteSongs(const int& count, std::vector<Music*>& music_list) override;
    virtual std::string GetUrl(Music& music) override;
    virtual std::string GetLyricsUrl(Music& music) override;
    virtual void ParseLyricsFromJson(const std::string& json, Lyrics& lyrics) override;
    virtual void ParseJsonArray(const cJSON* array, std::vector<Music*>& music_list) override;
};

#endif