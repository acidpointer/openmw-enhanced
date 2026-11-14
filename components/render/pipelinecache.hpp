#ifndef OPENMW_COMPONENTS_RENDER_PIPELINECACHE_H
#define OPENMW_COMPONENTS_RENDER_PIPELINECACHE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Render
{
    // Vulkan cache header version one is little-endian and contains the
    // device identity and pipeline-cache UUID. Never feed a foreign/truncated
    // file to the driver. Cache data remains optional and disposable.
    inline bool compatiblePipelineCache(std::span<const std::byte> bytes, std::uint32_t vendor, std::uint32_t device,
        std::span<const std::uint8_t, 16> uuid)
    {
        if (bytes.size() < 32)
            return false;
        const auto word = [&](std::size_t offset) {
            std::uint32_t value = 0;
            for (unsigned i = 0; i < 4; ++i)
                value |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (8 * i);
            return value;
        };
        return word(0) >= 32 && word(0) <= bytes.size() && word(4) == 1 && word(8) == vendor && word(12) == device
            && std::equal(uuid.begin(), uuid.end(), bytes.begin() + 16,
                [](auto a, auto b) { return a == std::to_integer<std::uint8_t>(b); });
    }
}
#endif
