#include <components/render/allocationranges.hpp>
#include <components/render/pipelinecache.hpp>
#include <components/render/raster.hpp>
#include <components/render/residency.hpp>
#include <components/render/resourcepool.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <set>
#include <type_traits>

namespace
{
    TEST(RenderRaster, DefaultDrawPreservesFullScreenClipCoordinates)
    {
        const Render::Draw draw;
        const Render::Draw valueInitialized{};
        EXPECT_EQ(draw.mClipFromModel, Render::IdentityTransform);
        EXPECT_EQ(valueInitialized.mClipFromModel, Render::IdentityTransform);
        // The final postprocess resolve uses a full-screen triangle. Its clip
        // W must remain one, including when the Draw is default-initialized.
        for (const auto point : { std::array<float, 4>{ -1, -1, 0, 1 }, std::array<float, 4>{ 3, -1, 0, 1 },
                 std::array<float, 4>{ -1, 3, 0, 1 } })
        {
            std::array<float, 4> clip{};
            for (unsigned row = 0; row < 4; ++row)
                for (unsigned column = 0; column < 4; ++column)
                    clip[row] += draw.mClipFromModel[column * 4 + row] * point[column];
            EXPECT_EQ(clip, point);
            EXPECT_EQ(clip[3], 1.f);
        }
    }

    TEST(RenderPipelineCache, RejectsForeignTruncatedAndFutureVersionData)
    {
        std::array<std::byte, 32> bytes{};
        const auto word = [&](std::size_t offset, std::uint32_t value) {
            for (unsigned i = 0; i < 4; ++i)
                bytes[offset + i] = std::byte((value >> (i * 8)) & 255);
        };
        word(0, 32);
        word(4, 1);
        word(8, 0x10de);
        word(12, 0x25b9);
        std::array<std::uint8_t, 16> uuid{};
        uuid[0] = 123;
        bytes[16] = std::byte{ 123 };
        EXPECT_TRUE(Render::compatiblePipelineCache(bytes, 0x10de, 0x25b9, uuid));
        EXPECT_FALSE(Render::compatiblePipelineCache(std::span(bytes).first(31), 0x10de, 0x25b9, uuid));
        EXPECT_FALSE(Render::compatiblePipelineCache(bytes, 0x8086, 0x25b9, uuid));
        EXPECT_FALSE(Render::compatiblePipelineCache(bytes, 0x10de, 0, uuid));
        uuid[1] = 1;
        EXPECT_FALSE(Render::compatiblePipelineCache(bytes, 0x10de, 0x25b9, uuid));
        uuid[1] = 0;
        word(0, 64);
        EXPECT_FALSE(Render::compatiblePipelineCache(bytes, 0x10de, 0x25b9, uuid));
        word(0, 32);
        word(4, 2);
        EXPECT_FALSE(Render::compatiblePipelineCache(bytes, 0x10de, 0x25b9, uuid));
    }

    TEST(RenderResidency, RetainsLoadedAssetsAcrossCameraTurnsAndEvictsOnlyUnderPressure)
    {
        Render::Residency cache(100);
        Render::Residency::Charge first(&cache);
        first.set(60);
        EXPECT_EQ(cache.bytes(), 60u);
        EXPECT_FALSE(cache.evict(true, 1000, 1));
        EXPECT_TRUE(cache.evict(false, 2, 1));
        {
            Render::Residency::Charge second(&cache);
            second.set(60);
            EXPECT_EQ(cache.bytes(), 120u);
            EXPECT_FALSE(cache.evict(true, 20, 1));
            EXPECT_TRUE(cache.evict(true, 100, 1));
            EXPECT_FALSE(cache.evict(true, 100, 100));
            second.set(20);
            EXPECT_EQ(cache.bytes(), 80u);
            EXPECT_FALSE(cache.evict(true, 100, 1));
        }
        EXPECT_EQ(cache.bytes(), 60u);
        first.set(0);
        EXPECT_EQ(cache.bytes(), 0u);
    }
    TEST(RenderResidency, ScalesBudgetsToDeviceMemoryWithoutConsumingTheHeap)
    {
        constexpr std::uint64_t MiB = 1024 * 1024;
        EXPECT_EQ(Render::Residency::limit(0, 8, 64 * MiB, 512 * MiB, 256 * MiB), 256 * MiB);
        EXPECT_EQ(Render::Residency::limit(512 * MiB, 8, 64 * MiB, 512 * MiB, 256 * MiB), 64 * MiB);
        EXPECT_EQ(Render::Residency::limit(4096 * MiB, 8, 64 * MiB, 512 * MiB, 256 * MiB), 512 * MiB);
        EXPECT_EQ(Render::Residency::limit(16384 * MiB, 8, 64 * MiB, 512 * MiB, 256 * MiB), 512 * MiB);
    }

    TEST(RenderRaster, DynamicRasterVariantsPreserveBlendAndKeepDepthOutOfThePipeline)
    {
        std::set<std::size_t> baked;
        for (std::size_t key = 0; key < 480; ++key)
        {
            EXPECT_EQ(Render::bakedRasterKey(key, false), key);
            const auto reduced = Render::bakedRasterKey(key, true);
            baked.insert(reduced);
            EXPECT_EQ((reduced / 4) % 5, (key / 4) % 5);
            EXPECT_EQ(reduced % 4, 0u);
            EXPECT_LT(reduced, 20u);
        }
        EXPECT_EQ(baked.size(), 5u);
    }

    TEST(RenderRaster, NativeViewportPreservesOutwardTriangleFacing)
    {
        // Independent implementation of the Vulkan specification's polygon
        // area formula, applied after the viewport and a reflected instance.
        for (bool negativeHeight : { false, true })
            for (bool reflected : { false, true })
            {
                const double x0 = 0, y0 = 0;
                const double x1 = reflected ? -1 : 1, y1 = 0;
                const double x2 = 0, y2 = negativeHeight ? -1 : 1;
                const double area = -.5 * (x0 * y1 - x1 * y0 + x1 * y2 - x2 * y1 + x2 * y0 - x0 * y2);
                const bool ccw = Render::counterClockwiseFrontFace(negativeHeight, reflected);
                EXPECT_TRUE(ccw ? area > 0 : area < 0);
            }
    }

    static_assert(!std::is_convertible_v<Render::MeshHandle, Render::TextureHandle>);

    TEST(RenderResourcePool, StaleUpdatesCannotReachAReusedSlot)
    {
        Render::ResourcePool<int, Render::MeshTag> pool;
        const auto first = pool.insert(std::make_shared<int>(7));
        pool.erase(first);
        const auto second = pool.insert(std::make_shared<int>(19));
        EXPECT_EQ(first.mIndex, second.mIndex);
        EXPECT_NE(first.mGeneration, second.mGeneration);
        EXPECT_THROW(pool.get(first), std::invalid_argument);
        EXPECT_THROW(pool.erase(first), std::invalid_argument);
        EXPECT_EQ(*pool.get(second), 19);
    }

    TEST(RenderResourcePool, SubmittedWorkCanKeepAReleasedResourceAlive)
    {
        Render::ResourcePool<int, Render::MeshTag> pool;
        auto resource = std::make_shared<int>(7);
        const std::weak_ptr<int> observer = resource;
        const auto handle = pool.insert(resource);
        auto submitted = pool.get(handle);
        resource.reset();
        pool.erase(handle);
        EXPECT_FALSE(observer.expired());
        EXPECT_EQ(*submitted, 7);
        submitted.reset();
        EXPECT_TRUE(observer.expired());
    }

    TEST(RenderResourcePool, RejectsAbsentResourcesAndUnknownHandles)
    {
        Render::ResourcePool<int, Render::MeshTag> pool;
        EXPECT_THROW(pool.insert({}), std::invalid_argument);
        EXPECT_THROW(pool.get({}), std::invalid_argument);
        EXPECT_THROW(pool.erase({ 19, 1 }), std::invalid_argument);
        const auto handle = pool.insert(std::make_shared<int>(7));
        pool.erase(handle);
        EXPECT_THROW(pool.erase(handle), std::invalid_argument);
    }

    TEST(RenderAllocationRanges, AlignmentPaddingRemainsAvailable)
    {
        Render::AllocationRanges ranges(64);
        EXPECT_EQ(ranges.allocate(3, 1), 0);
        EXPECT_EQ(ranges.allocate(16, 16), 16);
        EXPECT_EQ(ranges.allocate(13, 1), 3);
        EXPECT_EQ(ranges.allocate(32, 16), 32);
        EXPECT_FALSE(ranges.allocate(1, 1));
    }

    TEST(RenderAllocationRanges, ReleasingAdjacentAllocationsRestoresTheWholeBlock)
    {
        Render::AllocationRanges ranges(64);
        const auto first = ranges.allocate(16, 16);
        const auto second = ranges.allocate(16, 16);
        const auto third = ranges.allocate(32, 16);
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        ASSERT_TRUE(third);
        ranges.release(*second);
        EXPECT_FALSE(ranges.allocate(32, 1));
        ranges.release(*first);
        EXPECT_EQ(ranges.allocate(32, 16), 0);
        ranges.release(*third);
        ranges.release(0);
        EXPECT_TRUE(ranges.empty());
        EXPECT_EQ(ranges.allocate(64, 32), 0);
    }

    TEST(RenderAllocationRanges, ImpossibleAllocationsDoNotOverflowOrConsumeCapacity)
    {
        Render::AllocationRanges ranges(std::numeric_limits<std::uint64_t>::max());
        EXPECT_THROW(ranges.allocate(0, 1), std::invalid_argument);
        EXPECT_THROW(ranges.allocate(1, 0), std::invalid_argument);
        EXPECT_EQ(ranges.allocate(std::numeric_limits<std::uint64_t>::max() - 1, 1), 0);
        EXPECT_FALSE(ranges.allocate(1, 4));
        EXPECT_EQ(ranges.allocate(1, 1), std::numeric_limits<std::uint64_t>::max() - 1);
        EXPECT_FALSE(ranges.allocate(1, 1));
    }
}
