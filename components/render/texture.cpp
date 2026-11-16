#include "texture.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Render
{
    bool validSampler(const SamplerState& state) noexcept
    {
        return state.mWrapS <= TextureWrap::Border && state.mWrapT <= TextureWrap::Border
            && state.mMinFilter <= TextureFilter::Linear && state.mMagFilter <= TextureFilter::Linear
            && state.mMipFilter <= TextureFilter::Linear && state.mBorderColor <= BorderColor::White
            && std::isfinite(state.mAnisotropy) && state.mAnisotropy >= 1;
    }

    void validateSampler(const SamplerState& state)
    {
        if (!validSampler(state))
            throw std::invalid_argument("Texture sampler state is invalid");
    }

    void validateTexture(const TextureData& data)
    {
        if (data.mLevels.empty())
            throw std::invalid_argument("Texture has no image levels");
        if (data.mSrgb && (data.mFormat == TextureFormat::BC4 || data.mFormat == TextureFormat::BC5))
            throw std::invalid_argument("BC4 and BC5 textures do not have an sRGB encoding");
        if (data.mGenerateMipmaps && compressedTexture(data.mFormat))
            throw std::invalid_argument("Automatic mip generation requires uncompressed data");
        auto width = data.mLevels.front().mWidth;
        auto height = data.mLevels.front().mHeight;
        for (std::size_t i = 0; i < data.mLevels.size(); ++i)
        {
            const auto& level = data.mLevels[i];
            if (level.mWidth != width || level.mHeight != height
                || level.mBytes.size() != textureLevelSize(data.mFormat, width, height))
                throw std::invalid_argument("Texture mip dimensions or data length are invalid");
            if (i + 1 < data.mLevels.size() && width == 1 && height == 1)
                throw std::invalid_argument("Texture contains levels beyond its 1 by 1 mip");
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
    }
}
