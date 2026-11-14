#ifndef OPENMW_MWRENDER_VULKAN_STATSOVERLAY_H
#define OPENMW_MWRENDER_VULKAN_STATSOVERLAY_H

#include <components/render/backend.hpp>

#include <array>
#include <memory>

namespace osg
{
    class Stats;
}

namespace MWRender::Vulkan
{
    class StatsOverlay
    {
    public:
        explicit StatsOverlay(Render::Backend& backend);
        ~StatsOverlay();
        void setLevel(int level);
        void update(osg::Stats& engine, std::size_t meshes, std::size_t textures, std::uint64_t meshBytes,
            std::uint64_t textureBytes);
        void submitted(double intervalMs, const std::array<double, 5>& phases, std::size_t draws,
            std::size_t meshUploads, std::size_t imageUploads);
        std::span<const Render::Draw> graph();
        void shutdown();

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
