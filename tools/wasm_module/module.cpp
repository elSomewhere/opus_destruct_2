// Entry translation unit of the browser engine module (svx_web): the C ABI lives in svx_core
// (core/src/api/svx_api.cpp); this file only references it so the archive members are linked.
#include "svx/game/api/svx_api.h"

extern "C" void* svx_module_keepalive[] = {
    reinterpret_cast<void*>(&svx_create),    reinterpret_cast<void*>(&svx_tick),
    reinterpret_cast<void*>(&svx_poll_meshes), reinterpret_cast<void*>(&svx_load_wad),
};
