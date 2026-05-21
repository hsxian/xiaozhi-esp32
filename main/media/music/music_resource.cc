#include "music_resource.h"
#include "kw_music_resource.h"

MusicResource* MusicResource::NewMusicResource() { return new KwMusicResource(); }
