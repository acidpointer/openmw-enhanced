#ifndef OPENMW_MWRENDER_ENHANCEDPERF_H
#define OPENMW_MWRENDER_ENHANCEDPERF_H

#include <memory>
#include <string>
#include <string_view>

#include <components/sceneutil/enhancedperf.hpp>

#include <osg/RenderInfo>

namespace osg
{
    class Camera;
    class Node;
    class State;
}

namespace MWRender
{
    namespace Enhanced
    {
        bool csvGpuProfileEnabled();
        bool sceneGpuProfileEnabled();
        bool cameraGpuProfileEnabled();
        bool drawableGpuProfileEnabled();
        bool waterSurfaceGpuProfileEnabled();
        bool waterSurfaceEnabled();
        bool actorsEnabled();
        bool objectsEnabled();
        bool waterOcclusionEnabled();
        bool waterReflectionEnabled();
        bool waterRefractionEnabled();
        unsigned int applySceneCategoryMask(unsigned int mask, unsigned int actorMask, unsigned int objectMask);

        enum class TransparentDepthMode
        {
            Legacy,
            AlphaTestOnly,
            Off,
            ProfileOnly,
        };

        TransparentDepthMode transparentDepthMode();

        class GpuScope
        {
        public:
            GpuScope(osg::State& state, std::string name, std::string extra = {});
            ~GpuScope();

            GpuScope(const GpuScope&) = delete;
            GpuScope& operator=(const GpuScope&) = delete;

        private:
            std::unique_ptr<::SceneUtil::Enhanced::GpuScope> mScope;
        };

        void flushGpuProfile(osg::State& state);
        void logPerfConfigurationOnce();
        void installCameraProfiler(osg::Camera& camera, std::string name, std::string extra = {});
        void installRenderBinProfiler();
        unsigned int installDrawableProfiler(osg::Node& node, std::string name, std::string extra = {});
    }
}

#endif
