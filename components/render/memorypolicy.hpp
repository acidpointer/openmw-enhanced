#ifndef OPENMW_COMPONENTS_RENDER_MEMORYPOLICY_H
#define OPENMW_COMPONENTS_RENDER_MEMORYPOLICY_H

#include <algorithm>
#include <cstdint>

namespace Render
{
    // Bound pool overhead on small devices while packing several mod textures
    // into each allocation on larger devices. No image detail is discarded.
    constexpr std::uint64_t imageMemoryBlockSize(std::uint64_t localBytes)
    {
        constexpr std::uint64_t MiB = 1024 * 1024;
        return std::clamp(localBytes / 64, 16 * MiB, 64 * MiB);
    }
}

#endif
