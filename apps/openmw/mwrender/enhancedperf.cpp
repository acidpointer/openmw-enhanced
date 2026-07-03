#include "enhancedperf.hpp"

#include <sstream>
#include <utility>

#include <osg/Camera>
#include <osg/Drawable>
#include <osg/Geode>
#include <osg/Node>
#include <osg/NodeVisitor>
#include <osg/State>
#include <osgUtil/RenderBin>

#include <components/debug/debuglog.hpp>
#include <components/sceneutil/enhancedsettings.hpp>

namespace MWRender::Enhanced
{
    namespace
    {
        bool sLoggedConfig = false;
        bool sRenderBinProfilerInstalled = false;

        std::string cameraExtra(osg::RenderInfo& renderInfo, std::string_view extra)
        {
            std::ostringstream stream;
            if (!extra.empty())
                stream << extra << ';';
            if (const osg::Camera* camera = renderInfo.getCurrentCamera())
                stream << "camera=" << camera->getName();
            return stream.str();
        }

        std::string renderBinName(int binNum)
        {
            switch (binNum)
            {
                case -1:
                    return "sky";
                case 0:
                    return "opaque";
                case 9:
                    return "water";
                case 11:
                    return "occlusion-query";
                case 12:
                    return "first-person";
                case 13:
                    return "sun-glare";
                default:
                    return "bin-" + std::to_string(binNum);
            }
        }

        class RenderBinProfilerCallback : public osgUtil::RenderBin::DrawCallback
        {
        public:
            void drawImplementation(
                osgUtil::RenderBin* bin, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous) override
            {
                if (!sceneGpuProfileEnabled())
                {
                    bin->drawImplementation(renderInfo, previous);
                    return;
                }

                const int binNum = bin->getBinNum();
                const std::string binName = renderBinName(binNum);
                std::ostringstream extra;
                extra << cameraExtra(renderInfo, {}) << ";bin=" << binNum
                      << ";leaves=" << bin->getRenderLeafList().size()
                      << ";stategraphs=" << bin->getStateGraphList().size()
                      << ";children=" << bin->getRenderBinList().size();

                GpuScope scope(*renderInfo.getState(), "renderbin:" + binName, extra.str());
                bin->drawImplementation(renderInfo, previous);
            }
        };

        class DrawableProfilerCallback : public osg::Drawable::DrawCallback
        {
        public:
            DrawableProfilerCallback(osg::Drawable::DrawCallback* previous, std::string name, std::string extra)
                : mPrevious(previous)
                , mName(std::move(name))
                , mExtra(std::move(extra))
            {
            }

            void drawImplementation(osg::RenderInfo& renderInfo, const osg::Drawable* drawable) const override
            {
                if (!sceneGpuProfileEnabled())
                {
                    drawPrevious(renderInfo, drawable);
                    return;
                }

                GpuScope scope(*renderInfo.getState(), mName, cameraExtra(renderInfo, mExtra));
                drawPrevious(renderInfo, drawable);
            }

        private:
            void drawPrevious(osg::RenderInfo& renderInfo, const osg::Drawable* drawable) const
            {
                if (mPrevious)
                    mPrevious->drawImplementation(renderInfo, drawable);
                else
                    drawable->drawImplementation(renderInfo);
            }

            osg::ref_ptr<osg::Drawable::DrawCallback> mPrevious;
            std::string mName;
            std::string mExtra;
        };

        class InstallDrawableProfilerVisitor : public osg::NodeVisitor
        {
        public:
            InstallDrawableProfilerVisitor(std::string name, std::string extra)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mName(std::move(name))
                , mExtra(std::move(extra))
            {
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    install(geode.getDrawable(i));
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { install(&drawable); }

            unsigned int getInstalled() const { return mInstalled; }

        private:
            void install(osg::Drawable* drawable)
            {
                if (!drawable)
                    return;

                drawable->setDrawCallback(new DrawableProfilerCallback(drawable->getDrawCallback(), mName, mExtra));
                ++mInstalled;
            }

            std::string mName;
            std::string mExtra;
            unsigned int mInstalled = 0;
        };
    }

    bool csvGpuProfileEnabled()
    {
        return ::SceneUtil::Enhanced::csvGpuProfileEnabled();
    }

    bool sceneGpuProfileEnabled()
    {
        return csvGpuProfileEnabled() && ::SceneUtil::Enhanced::settingBool("Performance", "scene profile");
    }

    bool cameraGpuProfileEnabled()
    {
        return sceneGpuProfileEnabled() && ::SceneUtil::Enhanced::settingBool("Performance", "camera profile");
    }

    bool drawableGpuProfileEnabled()
    {
        return sceneGpuProfileEnabled() && ::SceneUtil::Enhanced::settingBool("Performance", "drawable profile");
    }

    bool waterSurfaceGpuProfileEnabled()
    {
        return false;
    }

    bool waterSurfaceEnabled()
    {
        return ::SceneUtil::Enhanced::settingBool("Water", "surface", true);
    }

    bool actorsEnabled()
    {
        return !::SceneUtil::Enhanced::settingBool("Renderer", "disable actors");
    }

    bool objectsEnabled()
    {
        return !::SceneUtil::Enhanced::settingBool("Renderer", "disable objects");
    }

    unsigned int applySceneCategoryMask(unsigned int mask, unsigned int actorMask, unsigned int objectMask)
    {
        if (!actorsEnabled())
            mask &= ~actorMask;
        if (!objectsEnabled())
            mask &= ~objectMask;
        return mask;
    }

    bool waterOcclusionEnabled()
    {
        const std::string value = ::SceneUtil::Enhanced::settingString("Water", "occlusion cameras", "main");
        return value.find("water") != std::string::npos || value.find("all") != std::string::npos;
    }

    bool waterReflectionEnabled()
    {
        return ::SceneUtil::Enhanced::settingBool("Water", "reflection", true);
    }

    bool waterRefractionEnabled()
    {
        return ::SceneUtil::Enhanced::settingBool("Water", "refraction", true);
    }

    TransparentDepthMode transparentDepthMode()
    {
        const std::string value = ::SceneUtil::Enhanced::settingString("Renderer", "transparent depth mode", "legacy");
        if (value == "alpha-test-only" || value == "alphatest")
            return TransparentDepthMode::AlphaTestOnly;
        if (value == "off" || value == "0" || value == "false")
            return TransparentDepthMode::Off;
        if (value == "profile-only")
            return TransparentDepthMode::ProfileOnly;
        return TransparentDepthMode::Legacy;
    }

    GpuScope::GpuScope(osg::State& state, std::string name, std::string extra)
        : mScope(std::make_unique<::SceneUtil::Enhanced::GpuScope>(state, std::move(name), std::move(extra)))
    {
    }

    GpuScope::~GpuScope() = default;

    void flushGpuProfile(osg::State& state)
    {
        ::SceneUtil::Enhanced::flushGpuProfile(state);
    }

    void logPerfConfigurationOnce()
    {
        if (sLoggedConfig)
            return;
        sLoggedConfig = true;

        if (csvGpuProfileEnabled())
            Log(Debug::Info) << "OpenMW Enhanced GPU profiler enabled.";

        switch (transparentDepthMode())
        {
            case TransparentDepthMode::Legacy:
                Log(Debug::Info) << "OpenMW Enhanced transparent depth mode: legacy";
                break;
            case TransparentDepthMode::AlphaTestOnly:
                Log(Debug::Info) << "OpenMW Enhanced transparent depth mode: alpha-test-only";
                break;
            case TransparentDepthMode::Off:
                Log(Debug::Info) << "OpenMW Enhanced transparent depth mode: off";
                break;
            case TransparentDepthMode::ProfileOnly:
                Log(Debug::Info) << "OpenMW Enhanced transparent depth mode: profile-only";
                break;
        }

        if (waterOcclusionEnabled())
            Log(Debug::Info) << "OpenMW Enhanced occlusion camera mode includes water RTT cameras.";
        if (sceneGpuProfileEnabled())
            Log(Debug::Info) << "OpenMW Enhanced scene GPU profiler enabled.";
        if (cameraGpuProfileEnabled())
            Log(Debug::Info) << "OpenMW Enhanced camera GPU profiler enabled.";
        if (drawableGpuProfileEnabled())
            Log(Debug::Info) << "OpenMW Enhanced drawable GPU profiler enabled.";
        if (!waterSurfaceEnabled())
            Log(Debug::Info) << "OpenMW Enhanced water surface disabled.";
        if (!actorsEnabled())
            Log(Debug::Info) << "OpenMW Enhanced actors disabled.";
        if (!objectsEnabled())
            Log(Debug::Info) << "OpenMW Enhanced objects disabled.";
        if (!waterReflectionEnabled())
            Log(Debug::Info) << "OpenMW Enhanced water reflection RTT disabled.";
        if (!waterRefractionEnabled())
            Log(Debug::Info) << "OpenMW Enhanced water refraction RTT disabled.";
    }

    void installCameraProfiler(osg::Camera& camera, std::string name, std::string extra)
    {
        if (!cameraGpuProfileEnabled())
            return;

        ::SceneUtil::Enhanced::installCameraProfiler(camera, std::move(name), std::move(extra));
    }

    void installRenderBinProfiler()
    {
        if (!sceneGpuProfileEnabled() || !::SceneUtil::Enhanced::settingBool("Performance", "renderbin profile")
            || sRenderBinProfilerInstalled)
            return;

        osgUtil::RenderBin* renderBin = osgUtil::RenderBin::getRenderBinPrototype("RenderBin");
        if (!renderBin)
            return;

        if (renderBin->getDrawCallback())
        {
            Log(Debug::Warning) << "OpenMW Enhanced render bin profiler skipped: RenderBin already has draw callback.";
            return;
        }

        renderBin->setDrawCallback(new RenderBinProfilerCallback);
        sRenderBinProfilerInstalled = true;
    }

    unsigned int installDrawableProfiler(osg::Node& node, std::string name, std::string extra)
    {
        if (!drawableGpuProfileEnabled())
            return 0;

        InstallDrawableProfilerVisitor visitor(std::move(name), std::move(extra));
        node.accept(visitor);
        return visitor.getInstalled();
    }
}
