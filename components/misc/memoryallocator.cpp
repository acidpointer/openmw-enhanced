#include "memoryallocator.hpp"

#include <components/debug/debuglog.hpp>

#ifdef OPENMW_USE_MIMALLOC
#include <mimalloc.h>
#endif

namespace Misc
{
    void logMemoryAllocator()
    {
#ifdef OPENMW_USE_MIMALLOC
        const int version = mi_version();
        Log(Debug::Info) << "CPU allocator: mimalloc " << version / 10000 << '.' << (version / 100) % 100 << '.'
                         << version % 100 << " (static process override; purge delay "
                         << mi_option_get(mi_option_purge_delay) << " ms, arena "
                         << mi_option_get_size(mi_option_arena_reserve) / (1024 * 1024) << " MiB, "
                         << (mi_option_is_enabled(mi_option_purge_decommits) ? "decommit" : "lazy reclamation") << ')';
#else
        Log(Debug::Info) << "CPU allocator: system";
#endif
    }
}
