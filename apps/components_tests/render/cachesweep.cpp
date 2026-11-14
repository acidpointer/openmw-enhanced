#include <components/render/cachesweep.hpp>
#include <gtest/gtest.h>
#include <map>

namespace
{
    TEST(RenderCacheSweep, ContinuesAfterBudgetWithoutInvalidatingItsCursor)
    {
        std::map<int, int> cache{ { 1, 0 }, { 2, 0 }, { 3, 0 }, { 4, 0 }, { 5, 0 } };
        Render::CacheSweep<decltype(cache)> sweep;
        int visits = 0;
        EXPECT_EQ(sweep.sweep(cache, [](const auto&) { return true; }, [&] { return visits++ == 2; }), 2u);
        cache.erase(3);
        cache.emplace(0, 0);
        EXPECT_EQ(sweep.sweep(cache, [](const auto&) { return true; }, [] { return false; }), 2u);
        ASSERT_EQ(cache.size(), 1u);
        EXPECT_EQ(cache.begin()->first, 0);
        EXPECT_EQ(sweep.sweep(cache, [](const auto&) { return true; }, [] { return false; }), 1u);
        EXPECT_TRUE(cache.empty());
    }
    TEST(RenderCacheSweep, DoesNotExpireActiveEntriesOrRestartWhenStopped)
    {
        std::map<int, int> cache{ { 1, 1 }, { 2, 0 }, { 3, 1 } };
        Render::CacheSweep<decltype(cache)> sweep;
        EXPECT_EQ(sweep.sweep(cache, [](const auto& x) { return x.second == 0; }, [] { return true; }), 0u);
        EXPECT_EQ(sweep.sweep(cache, [](const auto& x) { return x.second == 0; }, [] { return false; }), 1u);
        EXPECT_EQ(cache.size(), 2u);
    }
}
