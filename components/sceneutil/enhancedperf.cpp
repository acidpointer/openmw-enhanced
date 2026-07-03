#include "enhancedperf.hpp"

#include "enhancedsettings.hpp"

#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <osg/Camera>
#include <osg/GL>
#include <osg/GLExtensions>
#include <osg/RenderInfo>
#include <osg/State>

#include <components/debug/debuglog.hpp>

#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28
#endif

#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif

#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif

namespace SceneUtil::Enhanced
{
    namespace
    {
        using GLGenQueries = void(GL_APIENTRY*)(GLsizei, GLuint*);
        using GLDeleteQueries = void(GL_APIENTRY*)(GLsizei, const GLuint*);
        using GLQueryCounter = void(GL_APIENTRY*)(GLuint, GLenum);
        using GLGetQueryObjectiv = void(GL_APIENTRY*)(GLuint, GLenum, GLint*);
        using GLGetQueryObjectui64v = void(GL_APIENTRY*)(GLuint, GLenum, GLuint64*);

        struct GlTimerApi
        {
            bool mAttempted = false;
            bool mReady = false;
            GLGenQueries glGenQueries = nullptr;
            GLDeleteQueries glDeleteQueries = nullptr;
            GLQueryCounter glQueryCounter = nullptr;
            GLGetQueryObjectiv glGetQueryObjectiv = nullptr;
            GLGetQueryObjectui64v glGetQueryObjectui64v = nullptr;
        };

        struct Sample
        {
            unsigned int mFrame = 0;
            GLuint mStartQuery = 0;
            GLuint mEndQuery = 0;
            std::string mName;
            std::string mExtra;
        };

        struct OpenSample
        {
            unsigned int mFrame = 0;
            GLuint mStartQuery = 0;
            std::string mName;
            std::string mExtra;
        };

        std::mutex sMutex;
        std::unordered_map<unsigned int, GlTimerApi> sApis;
        std::deque<Sample> sPending;
        std::unordered_map<std::string, OpenSample> sOpenSamples;
        std::ofstream sCsv;
        bool sCsvHeaderWritten = false;

        std::string csvEscape(std::string_view value)
        {
            bool quote = false;
            for (char ch : value)
                quote = quote || ch == ',' || ch == '"' || ch == '\n' || ch == '\r';

            if (!quote)
                return std::string(value);

            std::string out = "\"";
            for (char ch : value)
            {
                if (ch == '"')
                    out += "\"\"";
                else
                    out += ch;
            }
            out += '"';
            return out;
        }

        GlTimerApi& getApi(osg::State& state)
        {
            const unsigned int contextId = state.getContextID();
            GlTimerApi& api = sApis[contextId];
            if (api.mAttempted)
                return api;

            api.mAttempted = true;
            const bool supported = osg::isGLExtensionOrVersionSupported(contextId, "GL_ARB_timer_query", 3.3f);
            if (!supported)
            {
                Log(Debug::Warning) << "OpenMW Enhanced GPU profiler disabled: GL_ARB_timer_query unsupported.";
                return api;
            }

            osg::setGLExtensionFuncPtr(api.glGenQueries, "glGenQueries");
            osg::setGLExtensionFuncPtr(api.glDeleteQueries, "glDeleteQueries");
            osg::setGLExtensionFuncPtr(api.glQueryCounter, "glQueryCounter");
            osg::setGLExtensionFuncPtr(api.glGetQueryObjectiv, "glGetQueryObjectiv");
            osg::setGLExtensionFuncPtr(api.glGetQueryObjectui64v, "glGetQueryObjectui64v");

            if (!api.glQueryCounter)
                osg::setGLExtensionFuncPtr(api.glQueryCounter, "glQueryCounterARB");
            if (!api.glGetQueryObjectui64v)
                osg::setGLExtensionFuncPtr(api.glGetQueryObjectui64v, "glGetQueryObjectui64vEXT");

            api.mReady = api.glGenQueries && api.glDeleteQueries && api.glQueryCounter && api.glGetQueryObjectiv
                && api.glGetQueryObjectui64v;

            if (!api.mReady)
                Log(Debug::Warning) << "OpenMW Enhanced GPU profiler disabled: timer query entry points missing.";

            return api;
        }

        std::ofstream& csv()
        {
            if (sCsv.is_open())
                return sCsv;

            std::string path = settingString("Performance", "gpu profile csv");
            if (path.empty())
                path = "/tmp/openmw-enhanced-gpu.csv";
            sCsv.open(path, std::ios::out | std::ios::trunc);
            if (sCsv.is_open())
                Log(Debug::Info) << "OpenMW Enhanced GPU profile CSV: " << path;
            else
                Log(Debug::Warning) << "OpenMW Enhanced GPU profile CSV could not be opened: " << path;
            return sCsv;
        }

        void writeCsv(unsigned int frame, std::string_view name, double ms, std::string_view extra)
        {
            auto& out = csv();
            if (!out.is_open())
                return;

            if (!sCsvHeaderWritten)
            {
                out << "frame,name,gpu_ms,extra\n";
                sCsvHeaderWritten = true;
            }

            out << frame << ',' << csvEscape(name) << ',' << ms << ',' << csvEscape(extra) << '\n';
        }

        void pushPending(osg::State& state, GLuint startQuery, GLuint endQuery, std::string name, std::string extra)
        {
            const osg::FrameStamp* stamp = state.getFrameStamp();
            sPending.push_back(
                { stamp ? stamp->getFrameNumber() : 0, startQuery, endQuery, std::move(name), std::move(extra) });
        }

        std::string currentCameraExtra(osg::RenderInfo& renderInfo, std::string_view extra)
        {
            std::ostringstream stream;
            if (!extra.empty())
                stream << extra << ';';
            if (const osg::Camera* camera = renderInfo.getCurrentCamera())
                stream << "camera=" << camera->getName();
            return stream.str();
        }

        class CameraProfilerCallback : public osg::Camera::DrawCallback
        {
        public:
            CameraProfilerCallback(std::string key, std::string name, std::string extra, bool begin)
                : mKey(std::move(key))
                , mName(std::move(name))
                , mExtra(std::move(extra))
                , mBegin(begin)
            {
            }

            void operator()(osg::RenderInfo& renderInfo) const override
            {
                osg::State* state = renderInfo.getState();
                if (!state)
                    return;

                if (mBegin)
                    beginNamedSample(*state, mKey, mName, currentCameraExtra(renderInfo, mExtra));
                else
                    endNamedSample(*state, mKey);
            }

        private:
            std::string mKey;
            std::string mName;
            std::string mExtra;
            bool mBegin;
        };
    }

    bool csvGpuProfileEnabled()
    {
        return settingBool("Performance", "gpu profile");
    }

    GpuScope::GpuScope(osg::State& state, std::string name, std::string extra)
        : mState(&state)
        , mName(std::move(name))
        , mExtra(std::move(extra))
    {
        if (!csvGpuProfileEnabled())
        {
            mState = nullptr;
            return;
        }

        std::lock_guard<std::mutex> lock(sMutex);
        GlTimerApi& api = getApi(state);
        if (!api.mReady)
        {
            mState = nullptr;
            return;
        }

        GLuint queries[2] = { 0, 0 };
        api.glGenQueries(2, queries);
        mStartQuery = queries[0];
        mEndQuery = queries[1];
        api.glQueryCounter(mStartQuery, GL_TIMESTAMP);
    }

    GpuScope::~GpuScope()
    {
        if (!mState || !mEndQuery)
            return;

        std::lock_guard<std::mutex> lock(sMutex);
        GlTimerApi& api = getApi(*mState);
        if (!api.mReady)
            return;

        api.glQueryCounter(mEndQuery, GL_TIMESTAMP);
        pushPending(*mState, mStartQuery, mEndQuery, std::move(mName), std::move(mExtra));
    }

    void beginNamedSample(osg::State& state, const std::string& key, std::string name, std::string extra)
    {
        if (!csvGpuProfileEnabled())
            return;

        std::lock_guard<std::mutex> lock(sMutex);
        GlTimerApi& api = getApi(state);
        if (!api.mReady)
            return;

        GLuint query = 0;
        api.glGenQueries(1, &query);
        api.glQueryCounter(query, GL_TIMESTAMP);

        const osg::FrameStamp* stamp = state.getFrameStamp();
        sOpenSamples[key] = { stamp ? stamp->getFrameNumber() : 0, query, std::move(name), std::move(extra) };
    }

    void endNamedSample(osg::State& state, const std::string& key)
    {
        if (!csvGpuProfileEnabled())
            return;

        std::lock_guard<std::mutex> lock(sMutex);
        auto it = sOpenSamples.find(key);
        if (it == sOpenSamples.end())
            return;

        GlTimerApi& api = getApi(state);
        if (!api.mReady)
        {
            sOpenSamples.erase(it);
            return;
        }

        GLuint endQuery = 0;
        api.glGenQueries(1, &endQuery);
        api.glQueryCounter(endQuery, GL_TIMESTAMP);

        sPending.push_back(
            { it->second.mFrame, it->second.mStartQuery, endQuery, std::move(it->second.mName),
                std::move(it->second.mExtra) });
        sOpenSamples.erase(it);
    }

    void flushGpuProfile(osg::State& state)
    {
        if (!csvGpuProfileEnabled())
            return;

        std::lock_guard<std::mutex> lock(sMutex);
        GlTimerApi& api = getApi(state);
        if (!api.mReady)
            return;

        while (!sPending.empty())
        {
            Sample& sample = sPending.front();

            GLint available = 0;
            api.glGetQueryObjectiv(sample.mEndQuery, GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available)
                break;

            GLuint64 start = 0;
            GLuint64 end = 0;
            api.glGetQueryObjectui64v(sample.mStartQuery, GL_QUERY_RESULT, &start);
            api.glGetQueryObjectui64v(sample.mEndQuery, GL_QUERY_RESULT, &end);

            const GLuint queries[2] = { sample.mStartQuery, sample.mEndQuery };
            api.glDeleteQueries(2, queries);

            const double ms = end >= start ? static_cast<double>(end - start) / 1000000.0 : 0.0;
            writeCsv(sample.mFrame, sample.mName, ms, sample.mExtra);
            sPending.pop_front();
        }
    }

    void writeCsvSample(unsigned int frame, std::string name, double ms, std::string extra)
    {
        if (!csvGpuProfileEnabled())
            return;

        std::lock_guard<std::mutex> lock(sMutex);
        writeCsv(frame, name, ms, extra);
    }

    void installCameraProfiler(osg::Camera& camera, std::string name, std::string extra)
    {
        const std::string key = name + ":" + std::to_string(reinterpret_cast<std::uintptr_t>(&camera));
        camera.addInitialDrawCallback(new CameraProfilerCallback(key, name, extra, true));
        camera.addFinalDrawCallback(new CameraProfilerCallback(key, name, extra, false));
    }
}
