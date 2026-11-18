#ifndef OPENMW_COMPONENTS_RENDER_COLORSPACE_HPP
#define OPENMW_COMPONENTS_RENDER_COLORSPACE_HPP

#include <array>
#include <cmath>
#include <cstddef>

namespace Render
{
    // Legacy content can provide subtractive lights. Extend the transfer
    // function symmetrically while leaving the alpha channel as authored.
    inline float linearFromSrgb(float value)
    {
        const float magnitude = std::abs(value);
        if (magnitude <= 0.04045f)
            return value / 12.92f;
        if (magnitude == 1.f)
            return value;
        return std::copysign(std::pow((magnitude + 0.055f) / 1.055f, 2.4f), value);
    }

    inline std::array<float, 4> linearRgbFromSrgb(std::array<float, 4> color)
    {
        for (std::size_t channel = 0; channel < 3; ++channel)
            color[channel] = linearFromSrgb(color[channel]);
        return color;
    }
}

#endif
