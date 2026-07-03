#ifndef OPENMW_COMPONENTS_SCENEUTIL_ENHANCEDPERF_H
#define OPENMW_COMPONENTS_SCENEUTIL_ENHANCEDPERF_H

#include <string>

namespace osg
{
    class Camera;
    class State;
}

namespace SceneUtil::Enhanced
{
    bool csvGpuProfileEnabled();

    class GpuScope
    {
    public:
        GpuScope(osg::State& state, std::string name, std::string extra = {});
        ~GpuScope();

        GpuScope(const GpuScope&) = delete;
        GpuScope& operator=(const GpuScope&) = delete;

    private:
        osg::State* mState = nullptr;
        unsigned int mStartQuery = 0;
        unsigned int mEndQuery = 0;
        std::string mName;
        std::string mExtra;
    };

    void beginNamedSample(osg::State& state, const std::string& key, std::string name, std::string extra = {});
    void endNamedSample(osg::State& state, const std::string& key);
    void flushGpuProfile(osg::State& state);
    void installCameraProfiler(osg::Camera& camera, std::string name, std::string extra = {});
    void writeCsvSample(unsigned int frame, std::string name, double ms, std::string extra = {});
}

#endif
