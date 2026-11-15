#include <components/render/cachesweep.hpp>
#include <gtest/gtest.h>
#include <map>
#include <set>
#include <unordered_map>

namespace
{
    struct CollidingHash
    {
        std::size_t operator()(int) const { return 0; }
    };

    TEST(RenderCacheSweep, HashCursorContinuesThroughCollisionsAndSurvivesRehash)
    {
        std::unordered_map<int, int, CollidingHash> cache{ { 1, 0 }, { 2, 0 }, { 3, 0 }, { 4, 0 } };
        Render::CacheSweep<decltype(cache)> sweep;
        std::set<int> visited;
        for (unsigned i = 0; i < 4; ++i)
        {
            int checks = 0;
            EXPECT_EQ(sweep.sweep(
                          cache,
                          [&](const auto& item) {
                              visited.insert(item.first);
                              return false;
                          },
                          [&] { return checks++ == 1; }),
                0u);
        }
        EXPECT_EQ(visited.size(), 4u);
        cache.rehash(cache.bucket_count() * 4);
        EXPECT_EQ(sweep.sweep(cache, [](const auto&) { return true; }, [] { return false; }), 4u);
        EXPECT_TRUE(cache.empty());
    }

    TEST(RenderCacheSweep, HashCursorRecoversWhenItsNextEntryIsRemoved)
    {
        std::unordered_map<int, int> cache{ { 1, 0 }, { 2, 0 }, { 3, 0 } };
        Render::CacheSweep<decltype(cache)> sweep;
        int retained = 0, checks = 0;
        sweep.sweep(
            cache,
            [&](const auto& item) {
                retained = item.first;
                return false;
            },
            [&] { return checks++ == 1; });
        std::erase_if(cache, [&](const auto& item) { return item.first != retained; });
        EXPECT_EQ(sweep.sweep(cache, [](const auto&) { return true; }, [] { return false; }), 1u);
        EXPECT_TRUE(cache.empty());
    }

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
