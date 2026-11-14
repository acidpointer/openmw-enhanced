#ifndef OPENMW_MWRENDER_VULKAN_EFFECTS_H
#define OPENMW_MWRENDER_VULKAN_EFFECTS_H

#include <components/render/backend.hpp>
#include <components/renderassets/osgstate.hpp>

#include <functional>
#include <memory>

namespace osg
{
    class Texture2D;
}

namespace MWRender
{
    class PingPongCanvas;
    namespace Vulkan
    {
        class Effects
        {
        public:
            using TextureResolver = std::function<Render::TextureHandle(const osg::Texture2D*)>;
            Effects(Render::Backend& backend, TextureResolver textures);
            ~Effects();
            // Redirect the scene and append the active OMWFX passes. Resources
            // and CPU uniform spans remain alive until the next append call.
            void append(std::vector<Render::Pass>& passes, PingPongCanvas& canvas, const RenderAssets::OsgState& state,
                const osg::Matrixd& projection, std::span<const Render::Draw> distortion = {});
            void submitted();

        private:
            struct Impl;
            std::unique_ptr<Impl> mImpl;
        };
    }
}
#endif
