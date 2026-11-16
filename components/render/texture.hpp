#ifndef OPENMW_COMPONENTS_RENDER_TEXTURE_H
#define OPENMW_COMPONENTS_RENDER_TEXTURE_H

#include "backend.hpp"

#include <limits>
#include <stdexcept>

namespace Render
{
    constexpr bool compressedTexture(TextureFormat format)
    {
        return (format >= TextureFormat::BC1RGB && format <= TextureFormat::BC5)
            || (format >= TextureFormat::BC6Unsigned && format <= TextureFormat::BC7RGBA);
    }

    enum class TextureComponent : unsigned char
    {
        Identity,
        Zero,
        One,
        R,
        G,
        B,
        A
    };
    constexpr std::array<TextureComponent, 4> textureSwizzle(TextureFormat format)
    {
        using C = TextureComponent;
        switch (format)
        {
            case TextureFormat::Luminance8:
                return { C::R, C::R, C::R, C::One };
            case TextureFormat::LuminanceAlpha8:
                return { C::R, C::R, C::R, C::G };
            case TextureFormat::Alpha8:
                return { C::One, C::One, C::One, C::R };
            default:
                return { C::Identity, C::Identity, C::Identity, C::Identity };
        }
    }

    // Align every mip's buffer offset to both a texel block and four bytes.
    // Three-byte RGB texels need twelve-byte offsets, not power-of-two rounding.
    constexpr std::size_t textureCopyAlignment(TextureFormat format)
    {
        switch (format)
        {
            case TextureFormat::RGB8:
            case TextureFormat::BGR8:
                return 12;
            case TextureFormat::BC1RGB:
            case TextureFormat::BC1RGBA:
            case TextureFormat::BC4:
                return 8;
            case TextureFormat::BC2:
            case TextureFormat::BC3:
            case TextureFormat::BC5:
            case TextureFormat::BC6Unsigned:
            case TextureFormat::BC6Signed:
            case TextureFormat::BC7RGBA:
            case TextureFormat::RGBA32F:
                return 16;
            default:
                return 4;
        }
    }
    constexpr std::size_t textureUploadOffset(std::size_t offset, TextureFormat format)
    {
        const auto alignment = textureCopyAlignment(format);
        const auto padding = (alignment - offset % alignment) % alignment;
        if (offset > std::numeric_limits<std::size_t>::max() - padding)
            throw std::length_error("Texture upload alignment exceeds host capacity");
        return offset + padding;
    }

    constexpr std::size_t textureLevelSize(TextureFormat format, std::uint32_t width, std::uint32_t height)
    {
        if (!width || !height)
            throw std::invalid_argument("Texture dimensions must be positive");
        std::uint64_t units = 0;
        std::uint64_t stride = 0;
        switch (format)
        {
            case TextureFormat::RGBA32F:
                units = std::uint64_t{ width } * height;
                stride = 16;
                break;
            case TextureFormat::RGBA8:
            case TextureFormat::BGRA8:
                units = std::uint64_t{ width } * height;
                stride = 4;
                break;
            case TextureFormat::RGB8:
            case TextureFormat::BGR8:
                units = std::uint64_t{ width } * height;
                stride = 3;
                break;
            case TextureFormat::R8:
            case TextureFormat::Luminance8:
            case TextureFormat::Alpha8:
                units = std::uint64_t{ width } * height;
                stride = 1;
                break;
            case TextureFormat::RG8:
            case TextureFormat::LuminanceAlpha8:
                units = std::uint64_t{ width } * height;
                stride = 2;
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
            case TextureFormat::BC6Unsigned:
            case TextureFormat::BC6Signed:
            case TextureFormat::BC7RGBA:
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
