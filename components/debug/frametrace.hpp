#ifndef OPENMW_COMPONENTS_DEBUG_FRAMETRACE_H
#define OPENMW_COMPONENTS_DEBUG_FRAMETRACE_H

#include <chrono>
#include <cstdint>

namespace Debug::Trace
{
    class Recorder;

    // Enabled only with OPENMW_FRAME_TRACE=/path/to/trace.json. Names must be
    // string literals. Producers never wait for the writer or for a full queue.
    class Scope
    {
    public:
        explicit Scope(const char* name, std::uint64_t frame = 0);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Recorder* mRecorder = nullptr;
        const char* mName;
        std::uint64_t mFrame;
        std::chrono::steady_clock::time_point mStart;
    };
}

#endif
