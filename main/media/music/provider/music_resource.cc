#include "music_resource.h"

#ifdef CONFIG_ENABLE_CENGUIGUI_RESOURCE
#include "cenguigui_resource.h"
#endif

#ifdef CONFIG_ENABLE_SHANHAI_RESOURCE
#include "shanhai_resource.h"
#endif

std::unique_ptr<MusicResource> MusicResource::NewMusicResource() {
    #ifdef CONFIG_ENABLE_CENGUIGUI_RESOURCE
        return std::make_unique<CenguiguiResource>();
    #elif defined(CONFIG_ENABLE_SHANHAI_RESOURCE)
        return std::make_unique<ShanhaiResource>();
    #else
        #error "Please enable at least one music resource"
    #endif
 }