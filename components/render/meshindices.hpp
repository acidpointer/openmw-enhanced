#ifndef OPENMW_COMPONENTS_RENDER_MESHINDICES_H
#define OPENMW_COMPONENTS_RENDER_MESHINDICES_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>

namespace Render
{
    // Choose by vertex capacity, not the current maximum index: later topology
    // updates must be able to address every vertex without replacing the buffer.
    constexpr bool narrowMeshIndices(std::size_t vertices)
    {
        return vertices <= 65536;
    }

    inline std::size_t meshIndexBytes(std::size_t count, bool narrow)
    {
        const std::size_t width = narrow ? sizeof(std::uint16_t) : sizeof(std::uint32_t);
        if (count > (std::numeric_limits<std::size_t>::max() - 3) / width)
            throw std::length_error("Mesh index storage exceeds addressable memory");
        // Vulkan buffer-copy ranges are multiples of four bytes, including an
        // odd number of 16-bit indices (a single triangle occupies six bytes).
        return (count * width + 3) & ~std::size_t{ 3 };
    }

    inline void writeMeshIndices(std::span<std::byte> destination, std::span<const std::uint32_t> indices, bool narrow)
    {
        const auto bytes = meshIndexBytes(indices.size(), narrow);
        if (destination.size() < bytes)
            throw std::invalid_argument("Mesh index staging range is too small");
        if (indices.empty())
            return;
        if (!narrow)
        {
            std::memcpy(destination.data(), indices.data(), indices.size_bytes());
            return;
        }
        // Write straight into mapped staging memory, without a temporary vector.
        // memcpy also permits unaligned destinations and avoids aliasing casts.
        for (std::size_t i = 0; i < indices.size(); ++i)
        {
            if (indices[i] > std::numeric_limits<std::uint16_t>::max())
                throw std::invalid_argument("Mesh index exceeds 16-bit storage");
            const auto value = static_cast<std::uint16_t>(indices[i]);
            std::memcpy(destination.data() + i * sizeof(value), &value, sizeof(value));
        }
        if (indices.size() % 2)
            std::memset(destination.data() + indices.size() * sizeof(std::uint16_t), 0, sizeof(std::uint16_t));
    }
}

#endif
