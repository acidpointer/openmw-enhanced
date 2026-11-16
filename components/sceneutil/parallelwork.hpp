#ifndef OPENMW_COMPONENTS_SCENEUTIL_PARALLELWORK_H
#define OPENMW_COMPONENTS_SCENEUTIL_PARALLELWORK_H

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace SceneUtil
{
    // Synchronous batches of independent CPU work. The caller participates;
    // workers borrow the context only until run() returns. All tasks drain
    // before an exception is rethrown on the caller. No per-task allocation.
    class ParallelWork
    {
    public:
        using Task = void (*)(void*, std::size_t);
        explicit ParallelWork(std::size_t workers);
        ~ParallelWork();
        ParallelWork(const ParallelWork&) = delete;
        ParallelWork& operator=(const ParallelWork&) = delete;

        static constexpr unsigned workersFor(unsigned threads)
        {
            return threads > 2 ? std::min(3u, (threads - 1) / 2) : 0;
        }
        void run(std::size_t count, Task task, void* context);
        std::size_t workerCount() const { return mWorkers.size(); }

    private:
        void execute();
        void worker();
        void stop();

        std::mutex mMutex;
        std::condition_variable mWake;
        std::condition_variable mDone;
        std::atomic<std::size_t> mNext{ 0 };
        std::size_t mCount = 0;
        Task mTask = nullptr;
        void* mContext = nullptr;
        std::size_t mActive = 0;
        std::uint64_t mGeneration = 0;
        std::exception_ptr mError;
        bool mRunning = false;
        bool mBusy = false;
        bool mStop = false;
        std::vector<std::thread> mWorkers;
    };
}

#endif
