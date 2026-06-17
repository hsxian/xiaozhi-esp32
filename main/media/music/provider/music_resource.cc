#include "music_resource.h"

#if CONFIG_ENABLE_CENGUIGUI_RESOURCE
#include "cenguigui_resource.h"
#endif

MusicResource* MusicResource::NewMusicResource() { 
    #if CONFIG_ENABLE_CENGUIGUI_RESOURCE
        return new CenguiguiResource();
    #else
        #error "Please enable at least one music resource"
    #endif
 }