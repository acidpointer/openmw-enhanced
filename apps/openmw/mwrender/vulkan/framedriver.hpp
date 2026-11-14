#ifndef OPENMW_MWRENDER_VULKAN_FRAMEDRIVER_H
#define OPENMW_MWRENDER_VULKAN_FRAMEDRIVER_H

#include <components/render/backend.hpp>

#include <memory>

namespace osgViewer
{
    class Viewer;
}

namespace MWRender
{
    // Transitional CPU scene-services adapter. Geometry, texture uploads and
    // the visible swapchain are native Vulkan; OSG still owns animation/culling.
    class VulkanFrameDriver final : public Render::FrameDriver
    {
    public:
        VulkanFrameDriver(osgViewer::Viewer& viewer, const Render::BackendOptions& options);
        ~VulkanFrameDriver() override;
        Render::Api api() const override { return Render::Api::Vulkan; }
        bool isDone() const override;
        void advance(double simulationTime) override;
        unsigned frameNumber() const override;
        void processEvents() override;
        void updateScene() override;
        void renderFrame() override;
        void stop() override;

    private:
        class Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
