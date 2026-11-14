#ifndef OPENMW_COMPONENTS_RENDER_RASTER_H
#define OPENMW_COMPONENTS_RENDER_RASTER_H

#include <cstddef>

namespace Render
{
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
