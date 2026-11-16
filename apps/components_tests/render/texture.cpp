#include <components/render/texture.hpp>

#include <gtest/gtest.h>

#include <array>
#include <limits>

namespace
{
    // Evaluate the upload policy used by the real Vulkan path: odd-sized
    // levels cannot overlap and every absolute offset must align to a texel.
    constexpr bool packedMipOffsets()
    {
        using F = Render::TextureFormat;
        std::size_t offset = 0;
        for (const auto width : { 5u, 2u, 1u })
        {
            offset = Render::textureUploadOffset(offset, F::RGB8);
            if (offset % 3 || offset % 4)
                return false;
            offset += Render::textureLevelSize(F::RGB8, width, width);
        }
        if (offset != 99 || Render::textureUploadOffset(15, F::R8) != 16)
            return false;
        for (const auto format : { F::BC1RGB, F::BC1RGBA, F::BC2, F::BC3, F::BC4, F::BC5 })
            if (Render::textureUploadOffset(17, format) % Render::textureCopyAlignment(format))
                return false;
        return true;
    }
    static_assert(packedMipOffsets());
    static_assert(Render::textureLevelSize(Render::TextureFormat::LuminanceAlpha8, 5, 3) == 30);
    static_assert(!Render::compressedTexture(Render::TextureFormat::BGRA8));
    static_assert(Render::textureSwizzle(Render::TextureFormat::LuminanceAlpha8)
        == std::array{ Render::TextureComponent::R, Render::TextureComponent::R, Render::TextureComponent::R,
            Render::TextureComponent::G });
    static_assert(Render::textureSwizzle(Render::TextureFormat::Alpha8)
        == std::array{ Render::TextureComponent::One, Render::TextureComponent::One, Render::TextureComponent::One,
            Render::TextureComponent::R });

    TEST(RenderTexture, RejectsUploadOffsetOverflow)
    {
        EXPECT_THROW(Render::textureUploadOffset(std::numeric_limits<std::size_t>::max(), Render::TextureFormat::RGB8),
            std::length_error);
    }

    TEST(RenderTexture, NativeChannelsAcceptMipGenerationAndExactByteCounts)
    {
        const std::array<std::byte, 30> bytes{};
        const Render::TextureLevel level{ 5, 3, bytes };
        const Render::TextureData data{ Render::TextureFormat::LuminanceAlpha8, false, true, { &level, 1 } };
        EXPECT_NO_THROW(Render::validateTexture(data));
        const Render::TextureLevel shortLevel{ 5, 3, std::span(bytes).first(29) };
        EXPECT_THROW(Render::validateTexture({ data.mFormat, false, true, { &shortLevel, 1 } }), std::invalid_argument);
    }

    TEST(RenderTexture, CompressedMipSizesRoundUpToWholeBlocks)
    {
        EXPECT_EQ(Render::textureLevelSize(Render::TextureFormat::BC1RGB, 1, 1), 8u);
        EXPECT_EQ(Render::textureLevelSize(Render::TextureFormat::BC3, 5, 7), 64u);
        EXPECT_EQ(Render::textureLevelSize(Render::TextureFormat::BC5, 2, 3), 16u);
        EXPECT_EQ(Render::textureLevelSize(Render::TextureFormat::RGBA8, 5, 7), 140u);
        EXPECT_THROW(Render::textureLevelSize(Render::TextureFormat::RGBA8, 0, 1), std::invalid_argument);
        EXPECT_THROW(Render::textureLevelSize(Render::TextureFormat::RGBA8, std::numeric_limits<std::uint32_t>::max(),
                         std::numeric_limits<std::uint32_t>::max()),
            std::length_error);
    }

    TEST(RenderTexture, AuthoredMipsMustHaveCorrectDimensionsAndByteCounts)
    {
        const std::array<std::byte, 8> bytes{};
        std::array<Render::TextureLevel, 3> levels{ { { 4, 4, bytes }, { 2, 2, bytes }, { 1, 1, bytes } } };
        Render::TextureData data{ Render::TextureFormat::BC1RGBA, false, false, levels };
        EXPECT_NO_THROW(Render::validateTexture(data));
        levels[1].mWidth = 3;
        EXPECT_THROW(Render::validateTexture(data), std::invalid_argument);
        levels[1].mWidth = 2;
        levels[2].mBytes = {};
        EXPECT_THROW(Render::validateTexture(data), std::invalid_argument);
    }

    TEST(RenderTexture, RejectsIncompatibleEncodingsAndUnsupportedMipGeneration)
    {
        const std::array<std::byte, 8> bytes{};
        const Render::TextureLevel level{ 1, 1, bytes };
        Render::TextureData data{ Render::TextureFormat::BC4, true, false, { &level, 1 } };
        EXPECT_THROW(Render::validateTexture(data), std::invalid_argument);
        data.mSrgb = false;
        data.mGenerateMipmaps = true;
        EXPECT_THROW(Render::validateTexture(data), std::invalid_argument);
    }
}
