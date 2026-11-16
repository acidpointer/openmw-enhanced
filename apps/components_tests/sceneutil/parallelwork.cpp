#include <components/sceneutil/parallelwork.hpp>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <stdexcept>

namespace
{
    static_assert(SceneUtil::ParallelWork::workersFor(0) == 0);
    static_assert(SceneUtil::ParallelWork::workersFor(2) == 0);
    static_assert(SceneUtil::ParallelWork::workersFor(4) == 1);
    static_assert(SceneUtil::ParallelWork::workersFor(64) == 3);

    struct Tasks
    {
        std::array<std::atomic<unsigned>, 128> mVisits{};
        bool mFail = false;
        static void run(void* context, std::size_t index)
        {
            auto& self = *static_cast<Tasks*>(context);
            ++self.mVisits[index];
            if (self.mFail && index == 3)
                throw std::runtime_error("test task failure");
        }
    };

    TEST(ParallelWork, VisitsEachIndexExactlyOnceAndReusesThePool)
    {
        SceneUtil::ParallelWork pool(2);
        Tasks tasks;
        for (unsigned iteration = 1; iteration <= 20; ++iteration)
        {
            pool.run(tasks.mVisits.size(), Tasks::run, &tasks);
            for (const auto& visits : tasks.mVisits)
                EXPECT_EQ(visits.load(), iteration);
        }
    }

    TEST(ParallelWork, CallerHandlesEmptySmallAndZeroWorkerBatches)
    {
        for (unsigned workers : { 0u, 2u })
        {
            SceneUtil::ParallelWork pool(workers);
            Tasks tasks;
            pool.run(0, nullptr, nullptr);
            pool.run(3, Tasks::run, &tasks);
            for (unsigned i = 0; i < tasks.mVisits.size(); ++i)
                EXPECT_EQ(tasks.mVisits[i].load(), i < 3 ? 1u : 0u);
            EXPECT_THROW(pool.run(1, nullptr, &tasks), std::invalid_argument);
        }
    }

    TEST(ParallelWork, DrainsFailuresBeforeReturningAndRemainsUsable)
    {
        for (unsigned workers : { 0u, 2u })
        {
            SceneUtil::ParallelWork pool(workers);
            Tasks tasks;
            tasks.mFail = true;
            EXPECT_THROW(pool.run(tasks.mVisits.size(), Tasks::run, &tasks), std::runtime_error);
            for (const auto& visits : tasks.mVisits)
                EXPECT_EQ(visits.load(), 1u);
            tasks.mFail = false;
            pool.run(tasks.mVisits.size(), Tasks::run, &tasks);
            for (const auto& visits : tasks.mVisits)
                EXPECT_EQ(visits.load(), 2u);
        }
    }
}
