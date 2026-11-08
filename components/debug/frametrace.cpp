#include "frametrace.hpp"

#include "debuglog.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <thread>

namespace Debug::Trace
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        struct Event
        {
            const char* mName;
            double mBegin;
            double mDuration;
            std::uint64_t mFrame;
            unsigned mThread;
        };
        constexpr std::size_t Capacity = 8192;

        bool enabled()
        {
            static const bool value = [] {
                const char* path = std::getenv("OPENMW_FRAME_TRACE");
                return path && *path;
            }();
            return value;
        }
    }

    class Recorder
    {
    public:
        static Recorder& instance()
        {
            static Recorder recorder;
            return recorder;
        }

        void record(const char* name, Clock::time_point begin, Clock::time_point end, std::uint64_t frame)
        {
            if (!mOutput.is_open())
                return;
            static std::atomic<unsigned> nextThread{ 1 };
            thread_local const unsigned thread = nextThread.fetch_add(1, std::memory_order_relaxed);
            std::unique_lock lock(mMutex, std::try_to_lock);
            if (!lock.owns_lock() || mCount == Capacity)
            {
                mDropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const std::size_t index = (mBegin + mCount) % Capacity;
            mQueue[index] = { name, std::chrono::duration<double, std::micro>(begin - mEpoch).count(),
                std::chrono::duration<double, std::micro>(end - begin).count(), frame, thread };
            ++mCount;
            lock.unlock();
            mReady.notify_one();
        }

    private:
        Recorder()
            : mEpoch(Clock::now())
            , mOutput(std::getenv("OPENMW_FRAME_TRACE"))
        {
            if (mOutput)
            {
                mOutput.imbue(std::locale::classic());
                mOutput << "{\"traceEvents\":[\n" << std::fixed << std::setprecision(3);
                try
                {
                    mWriter = std::thread([this] { write(); });
                }
                catch (const std::exception& error)
                {
                    mOutput.close();
                    Log(Debug::Warning) << "Cannot start frame trace writer: " << error.what();
                }
            }
            else
                Log(Debug::Warning) << "Cannot open frame trace: " << std::getenv("OPENMW_FRAME_TRACE");
        }

        ~Recorder()
        {
            {
                std::lock_guard lock(mMutex);
                mStopping = true;
            }
            mReady.notify_one();
            if (mWriter.joinable())
                mWriter.join();
        }

        void write()
        {
            std::array<Event, 256> batch;
            bool first = true;
            while (true)
            {
                std::size_t count = 0;
                {
                    std::unique_lock lock(mMutex);
                    mReady.wait(lock, [this] { return mStopping || mCount != 0; });
                    if (!mCount && mStopping)
                        break;
                    while (count < batch.size() && mCount)
                    {
                        batch[count++] = mQueue[mBegin];
                        mBegin = (mBegin + 1) % Capacity;
                        --mCount;
                    }
                }
                for (std::size_t i = 0; i < count; ++i)
                {
                    const Event& event = batch[i];
                    if (!first)
                        mOutput << ",\n";
                    first = false;
                    mOutput << "{\"name\":\"" << event.mName
                            << "\",\"cat\":\"OpenMW\",\"ph\":\"X\",\"pid\":1,\"tid\":" << event.mThread
                            << ",\"ts\":" << event.mBegin << ",\"dur\":" << event.mDuration
                            << ",\"args\":{\"frame\":" << event.mFrame << "}}";
                }
            }
            mOutput << "\n],\"droppedEvents\":" << mDropped.load() << "}\n";
        }

        Clock::time_point mEpoch;
        std::ofstream mOutput;
        std::array<Event, Capacity> mQueue;
        std::mutex mMutex;
        std::condition_variable mReady;
        std::thread mWriter;
        std::atomic<std::uint64_t> mDropped{ 0 };
        std::size_t mBegin = 0;
        std::size_t mCount = 0;
        bool mStopping = false;
    };

    Scope::Scope(const char* name, std::uint64_t frame)
        : mName(name)
        , mFrame(frame)
    {
        if (enabled())
        {
            mRecorder = &Recorder::instance();
            mStart = Clock::now();
        }
    }

    Scope::~Scope()
    {
        if (mRecorder)
            mRecorder->record(mName, mStart, Clock::now(), mFrame);
    }
}
