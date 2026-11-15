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
    struct PrepareStats
    {
        double mScene = 0, mEffects = 0, mOverlay = 0;
        // Nested inside scene/effects preparation, not additional phases.
        double mMeshes = 0, mTextures = 0, mTargets = 0;
        double mParticles = 0;
        double mParticleWait = 0;
        std::size_t mMeshBuilds = 0, mMeshUpdates = 0;
        std::size_t mStateBindings = 0, mTransforms = 0;
    };

    class StatsOverlay
    {
    public:
        explicit StatsOverlay(Render::Backend& backend);
        ~StatsOverlay();
        void setLevel(int level);
        void update(osg::Stats& engine, std::size_t meshes, std::size_t textures, std::uint64_t meshBytes,
            std::uint64_t textureBytes);
        void prepared(double milliseconds, const PrepareStats& prepare);
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
