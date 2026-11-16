#ifndef OPENMW_COMPONENTS_RENDER_RASTER_H
#define OPENMW_COMPONENTS_RENDER_RASTER_H

#include "backend.hpp"
#include <cstddef>

namespace Render
{
    constexpr AlphaCompare reverseDepthComparison(AlphaCompare compare)
    {
        switch (compare)
        {
            case AlphaCompare::Less:
                return AlphaCompare::Greater;
            case AlphaCompare::LessEqual:
                return AlphaCompare::GreaterEqual;
            case AlphaCompare::Greater:
                return AlphaCompare::Less;
            case AlphaCompare::GreaterEqual:
                return AlphaCompare::LessEqual;
            default:
                return compare;
        }
    }

    // Four face/culling combinations, then five blend modes, followed by
    // depth test/write and comparison variants. Dynamic depth/raster state
    // leaves only the blend mode baked into a material pipeline.
    constexpr std::size_t bakedRasterKey(std::size_t key, bool dynamicRaster)
    {
        return dynamicRaster ? 4 * ((key / 4) % 5) : key;
    }

    // Vulkan uses minus the usual shoelace area in framebuffer coordinates.
    // A negative-height viewport therefore preserves source NDC CCW facing.
    constexpr bool counterClockwiseFrontFace(bool negativeViewportHeight, bool reversedSourceFace)
    {
        return negativeViewportHeight != reversedSourceFace;
    }
}

#endif
