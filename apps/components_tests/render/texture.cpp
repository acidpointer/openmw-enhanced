#include <components/render/texture.hpp>

#include <gtest/gtest.h>

#include <array>
#include <limits>

namespace
{
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
