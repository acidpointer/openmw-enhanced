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

    // Two preferred texture-upload pages plus the normal 4 MiB page. A
    // smaller idle budget discards large pages before their grace expires.
    constexpr std::uint64_t uploadMemoryRetentionBytes(std::uint64_t localBytes)
    {
        return 2 * imageMemoryBlockSize(localBytes) + 4 * 1024 * 1024;
    }
}

#endif
