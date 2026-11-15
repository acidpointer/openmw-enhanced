#ifndef OPENMW_COMPONENTS_MISC_MEMORYALLOCATOR_H
#define OPENMW_COMPONENTS_MISC_MEMORYALLOCATOR_H

namespace Misc
{
    // Logs the allocator actually linked into this process. No allocator
    // initialization or collection is performed on frame/worker hot paths.
    void logMemoryAllocator();
}

#endif
