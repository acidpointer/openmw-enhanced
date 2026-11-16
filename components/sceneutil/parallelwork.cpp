#include "parallelwork.hpp"

#include <stdexcept>
#include <system_error>

namespace SceneUtil
{
    ParallelWork::ParallelWork(std::size_t workers)
    {
        mWorkers.reserve(workers);
        try
        {
            for (std::size_t i = 0; i < workers; ++i)
                mWorkers.emplace_back([this] { worker(); });
        }
        catch (const std::system_error&)
        {
            // Thread quotas or a busy low-end system must not prevent the
            // renderer from starting. The caller can execute every batch.
            stop();
            mStop = false;
        }
        catch (...)
        {
            stop();
            throw;
        }
    }

    ParallelWork::~ParallelWork()
    {
        stop();
    }

    void ParallelWork::stop()
    {
        {
            const std::lock_guard lock(mMutex);
            mStop = true;
        }
        mWake.notify_all();
        for (auto& thread : mWorkers)
            thread.join();
        mWorkers.clear();
    }

    void ParallelWork::run(std::size_t count, Task task, void* context)
    {
        if (!count)
            return;
        if (!task)
            throw std::invalid_argument("Parallel work requires a task");
        const bool parallel = count >= 8 && !mWorkers.empty();
        {
            const std::lock_guard lock(mMutex);
            if (mBusy)
                throw std::logic_error("Parallel work batches cannot overlap");
            mBusy = true;
            mTask = task;
            mContext = context;
            mCount = count;
            mNext.store(0, std::memory_order_relaxed);
            mError = nullptr;
            mRunning = parallel;
            ++mGeneration;
        }
        if (parallel)
            mWake.notify_all();
        execute();
        std::unique_lock lock(mMutex);
        // Late workers skip this generation; wait only for workers that
        // actually borrowed its context, not for idle threads to acknowledge.
        mRunning = false;
        mDone.wait(lock, [this] { return mActive == 0; });
        auto error = mError;
        mError = nullptr;
        mTask = nullptr;
        mContext = nullptr;
        mBusy = false;
        lock.unlock();
        if (error)
            std::rethrow_exception(error);
    }

    void ParallelWork::execute()
    {
        for (auto index = mNext.fetch_add(1, std::memory_order_relaxed); index < mCount;
            index = mNext.fetch_add(1, std::memory_order_relaxed))
        {
            try
            {
                mTask(mContext, index);
            }
            catch (...)
            {
                const std::lock_guard lock(mMutex);
                if (!mError)
                    mError = std::current_exception();
            }
        }
    }

    void ParallelWork::worker()
    {
        std::uint64_t generation = 0;
        std::unique_lock lock(mMutex);
        for (;;)
        {
            mWake.wait(lock, [&] { return mStop || generation != mGeneration; });
            if (mStop)
                return;
            generation = mGeneration;
            if (!mRunning)
                continue;
            ++mActive;
            lock.unlock();
            execute();
            lock.lock();
            --mActive;
            if (!mActive)
                mDone.notify_one();
        }
    }
}
