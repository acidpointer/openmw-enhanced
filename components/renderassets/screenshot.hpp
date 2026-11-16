#ifndef OPENMW_COMPONENTS_RENDERASSETS_SCREENSHOT_H
#define OPENMW_COMPONENTS_RENDERASSETS_SCREENSHOT_H

#include <components/render/backend.hpp>

#include <osg/Image>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace RenderAssets
{
    // Native presentation captures are top-down RGBA. Saved games use the OSG
    // JPEG writer, which requires packed RGB and flips bottom-up rows itself.
    inline void saveScreenshot(const Render::CapturedImage& pixels, int width, int height, osg::Image& image)
    {
        if (width <= 0 || height <= 0 || !pixels.mWidth || !pixels.mHeight
            || std::uint64_t(pixels.mWidth) * pixels.mHeight > pixels.mRgba.size() / 4)
            throw std::invalid_argument("Invalid native save screenshot dimensions or pixel buffer");
        const auto cropWidth
            = std::clamp<std::uint64_t>(std::uint64_t(pixels.mHeight) * width / height, 1, pixels.mWidth);
        const auto cropHeight
            = std::clamp<std::uint64_t>(std::uint64_t(pixels.mWidth) * height / width, 1, pixels.mHeight);
        if (cropWidth > std::numeric_limits<int>::max() || cropHeight > std::numeric_limits<int>::max())
            throw std::length_error("Native save screenshot exceeds OSG image dimensions");
        const auto left = (pixels.mWidth - cropWidth) / 2;
        const auto top = (pixels.mHeight - cropHeight) / 2;
        image.allocateImage(static_cast<int>(cropWidth), static_cast<int>(cropHeight), 1, GL_RGB, GL_UNSIGNED_BYTE, 1);
        image.setOrigin(osg::Image::BOTTOM_LEFT);
        for (std::uint32_t row = 0; row < cropHeight; ++row)
        {
            const auto* source
                = pixels.mRgba.data() + (std::size_t(top + cropHeight - row - 1) * pixels.mWidth + left) * 4;
            auto* destination = image.data(0, row);
            for (std::uint32_t column = 0; column < cropWidth; ++column)
                std::memcpy(destination + column * 3, source + column * 4, 3);
        }
        image.scaleImage(width, height, 1);
    }
}

#endif
