#ifndef OPENMW_COMPONENTS_RENDER_TEXTURE_H
#define OPENMW_COMPONENTS_RENDER_TEXTURE_H

#include "backend.hpp"

#include <limits>
#include <stdexcept>

namespace Render
{
    inline std::size_t textureLevelSize(TextureFormat format, std::uint32_t width, std::uint32_t height)
    {
        if (!width || !height)
            throw std::invalid_argument("Texture dimensions must be positive");
        std::uint64_t units = 0;
        std::uint64_t stride = 0;
        switch (format)
        {
            case TextureFormat::RGBA8:
                units = std::uint64_t{ width } * height;
                stride = 4;
                break;
            case TextureFormat::BC1RGB:
            case TextureFormat::BC1RGBA:
            case TextureFormat::BC4:
                units = ((std::uint64_t{ width } + 3) / 4) * ((std::uint64_t{ height } + 3) / 4);
                stride = 8;
                break;
            case TextureFormat::BC2:
            case TextureFormat::BC3:
            case TextureFormat::BC5:
                units = ((std::uint64_t{ width } + 3) / 4) * ((std::uint64_t{ height } + 3) / 4);
                stride = 16;
                break;
            default:
                throw std::invalid_argument("Unknown texture format");
        }
        if (units > std::numeric_limits<std::size_t>::max() / stride)
            throw std::length_error("Texture data exceeds the host address space");
        return static_cast<std::size_t>(units * stride);
    }

    void validateTexture(const TextureData& data);
    bool validSampler(const SamplerState& state) noexcept;
    void validateSampler(const SamplerState& state);
}

#endif
