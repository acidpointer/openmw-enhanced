#include <components/render/allocationranges.hpp>
#include <components/render/bufferarena.hpp>
#include <components/render/bufferversions.hpp>
#include <components/render/drawranges.hpp>
#include <components/render/framecache.hpp>
#include <components/render/meshindices.hpp>
#include <components/render/pipelinecache.hpp>
#include <components/render/raster.hpp>
#include <components/render/residency.hpp>
#include <components/render/resourcepool.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <span>
#include <type_traits>

namespace
{
    static_assert([] {
        Render::DrawRanges ranges;
        ranges.clearDepth(1, 4);
        ranges.clearDepth(2, 3); // Nested weapon range, after rejected geometry.
        ranges.clearDepth(0, 1); // Empty range must not clear unrelated depth.
        ranges.begin(4);
        ranges.emitted(0, 0);
        ranges.emitted(1, 0);
        ranges.emitted(2, 1);
        ranges.emitted(3, 2);
        std::array<unsigned, 2> marks{};
        ranges.apply([&](auto index) { ++marks[index]; });
        if (marks[0] != 2 || marks[1] != 0)
            return false;
        ranges.clear();
        ranges.begin(1, 2);
        ranges.clearDepth(0, 1);
        ranges.emitted(0, 3);
        unsigned first = 0;
        ranges.apply([&](auto index) { first = index; });
        return first == 2;
    }());

    // Check the actual cache policy during compilation as well as in the
    // executable suite: revisiting a recent view must not force reuploads.
    static_assert([] {
        Render::Residency cache(20);
        cache.setLimit(20, 250);
        Render::Residency::Charge north(&cache), south(&cache), unused(&cache);
        north.set(100);
        south.set(100);
        unused.set(10);
        cache.beginFrame();
        north.touch();
        cache.beginFrame();
        south.touch();
        if (cache.recentBytes() != 200 || cache.evict(true, 100, 1))
            return false;
        cache.setLimit(20, 200);
        if (!cache.evict(true, 100, 1)) // Recent views cannot exceed the device ceiling.
            return false;
        cache.setLimit(20, 250);
        for (unsigned i = 0; i < 255; ++i)
        {
            cache.beginFrame();
            south.touch();
        }
        return cache.recentBytes() == 100 && cache.evict(true, 400, 1);
    }());

    static_assert([] {
        Render::Residency cache(20);
        Render::Residency::Charge first(&cache), middle(&cache), last(&cache);
        cache.beginFrame();
        for (auto* charge : { &first, &middle, &last })
        {
            charge->set(10);
            charge->touch();
        }
        middle.set(0);
        last.set(20);
        middle.set(15);
        middle.touch();
        if (cache.activeBytes() != 45 || cache.recentBytes() != 45)
            return false;
        cache.beginFrame();
        first.touch();
        last.set(0);
        if (cache.recentBytes() != 25)
            return false;
        for (unsigned i = 0; i < 256; ++i)
            cache.beginFrame();
        if (cache.recentBytes() != 0 || cache.bytes() != 25)
            return false;
        first.touch();
        return cache.recentBytes() == 10;
    }());

    static_assert([] {
        Render::Residency cache(20);
        cache.beginFrame();
        {
            Render::Residency::Charge temporary(&cache);
            temporary.set(10);
            temporary.touch();
        }
        if (cache.bytes() || cache.activeBytes() || cache.recentBytes())
            return false;
        for (unsigned i = 0; i < 256; ++i)
            cache.beginFrame(); // Destroyed charges must leave no bucket links.
        return cache.recentBytes() == 0;
    }());

    TEST(RenderBufferVersions, ReusesIdleRangesWithoutAllocatingAndNeverOverwritesFrameLeases)
    {
        Render::BufferVersions<int> versions;
        int allocations = 0;
        const auto create = [&] { return std::make_shared<int>(++allocations); };
        const auto* first = versions.writable(create).get();
        EXPECT_EQ(versions.writable(create).get(), first);
        auto firstFrame = versions.current();
        const auto* second = versions.writable(create).get();
        EXPECT_NE(first, second);
        EXPECT_EQ(*firstFrame, 1);
        auto secondFrame = versions.current();
        firstFrame.reset();
        EXPECT_EQ(versions.writable(create).get(), first);
        EXPECT_EQ(allocations, 2);
        EXPECT_EQ(*secondFrame, 2);
    }

    TEST(RenderBufferVersions, FrozenPoseRemainsLeasedUntilEveryReadingFrameRetires)
    {
        Render::BufferVersions<int> versions;
        int allocations = 0;
        const auto create = [&] { return std::make_shared<int>(++allocations); };
        auto firstFrame = versions.writable(create);
        auto secondFrame = versions.current();
        const auto* frozen = firstFrame.get();
        firstFrame.reset();
        EXPECT_NE(versions.writable(create).get(), frozen);
        auto updatedFrame = versions.current();
        secondFrame.reset();
        EXPECT_EQ(versions.writable(create).get(), frozen);
        EXPECT_EQ(versions.size(), 2u);
        EXPECT_EQ(*updatedFrame, 2);
    }

    TEST(RenderBufferVersions, FrameLeaseOutlivesMeshAndFailedAllocationPreservesCurrent)
    {
        std::shared_ptr<int> frame;
        {
            Render::BufferVersions<int> versions;
            frame = versions.writable([] { return std::make_shared<int>(42); });
            EXPECT_THROW(versions.writable([] { return std::shared_ptr<int>{}; }), std::invalid_argument);
            EXPECT_EQ(versions.current(), frame);
            EXPECT_EQ(versions.size(), 1u);
        }
        EXPECT_EQ(*frame, 42);
    }

    TEST(RenderRaster, ReverseDepthFlipsOnlyOrderedComparisons)
    {
        using C = Render::AlphaCompare;
        EXPECT_EQ(Render::reverseDepthComparison(C::Less), C::Greater);
        EXPECT_EQ(Render::reverseDepthComparison(C::LessEqual), C::GreaterEqual);
        EXPECT_EQ(Render::reverseDepthComparison(C::Greater), C::Less);
        EXPECT_EQ(Render::reverseDepthComparison(C::GreaterEqual), C::LessEqual);
        for (auto comparison : { C::Always, C::Never, C::Equal, C::NotEqual })
            EXPECT_EQ(Render::reverseDepthComparison(comparison), comparison);
        EXPECT_TRUE(Render::targetHasDepth(Render::TargetFormat::ShadowMap));
    }

    TEST(RenderMeshIndices, WidthUsesFullVertexCapacityAndCopiesAreFourByteAligned)
    {
        EXPECT_TRUE(Render::narrowMeshIndices(65536));
        EXPECT_FALSE(Render::narrowMeshIndices(65537));
        EXPECT_EQ(Render::meshIndexBytes(3, true), 8u);
        EXPECT_EQ(Render::meshIndexBytes(6, true), 12u);
        EXPECT_EQ(Render::meshIndexBytes(3, false), 12u);
        EXPECT_THROW(Render::meshIndexBytes(std::numeric_limits<std::size_t>::max(), true), std::length_error);
    }

    TEST(RenderMeshIndices, WritesUnalignedStagingAndInitializesOddIndexPadding)
    {
        const std::array<std::uint32_t, 3> indices{ 0, 65535, 42 };
        std::array<std::byte, 10> storage;
        storage.fill(std::byte{ 0xcd });
        const auto staging = std::span(storage).subspan(1, 8);
        Render::writeMeshIndices(staging, indices, true);
        for (std::size_t i = 0; i < indices.size(); ++i)
        {
            std::uint16_t value;
            std::memcpy(&value, staging.data() + i * sizeof(value), sizeof(value));
            EXPECT_EQ(value, indices[i]);
        }
        EXPECT_EQ(storage[0], std::byte{ 0xcd });
        EXPECT_EQ(storage[9], std::byte{ 0xcd });
        EXPECT_EQ(staging[6], std::byte{});
        EXPECT_EQ(staging[7], std::byte{});
    }

    TEST(RenderMeshIndices, WideIndicesStayLosslessAndInvalidNarrowWritesAreRejected)
    {
        const std::array<std::uint32_t, 3> indices{ 0, 65536, 70000 };
        std::array<std::byte, 12> staging{};
        Render::writeMeshIndices(staging, indices, false);
        EXPECT_EQ(std::memcmp(staging.data(), indices.data(), indices.size() * sizeof(indices[0])), 0);
        EXPECT_THROW(Render::writeMeshIndices(staging, indices, true), std::invalid_argument);
        EXPECT_THROW(Render::writeMeshIndices(std::span(staging).first(4), indices, false), std::invalid_argument);
    }

    struct FrameHash
    {
        std::size_t operator()(int) const { return 0; }
    };
    TEST(RenderFrameCache, GrowthPreservesFrameValuesThroughCollisions)
    {
        Render::FrameCache<int, int, FrameHash> cache;
        cache.beginFrame();
        for (int i = 0; i < 100; ++i)
        {
            auto [value, inserted] = cache.getOrCreate(i);
            ASSERT_TRUE(inserted);
            value = i * 7;
        }
        EXPECT_EQ(cache.size(), 100u);
        for (int i = 0; i < 100; ++i)
        {
            auto [value, inserted] = cache.getOrCreate(i);
            EXPECT_FALSE(inserted);
            EXPECT_EQ(value, i * 7);
        }
        const auto capacity = cache.capacity();
        cache.beginFrame();
        EXPECT_EQ(cache.size(), 0u);
        EXPECT_EQ(cache.capacity(), capacity);
        auto [fresh, inserted] = cache.getOrCreate(7);
        ASSERT_TRUE(inserted);
        fresh = 42;
        auto [same, duplicate] = cache.getOrCreate(7);
        EXPECT_FALSE(duplicate);
        EXPECT_EQ(same, 42);
        EXPECT_EQ(cache.size(), 1u);
    }

    TEST(RenderBufferArena, PacksDistinctVertexAndIndexRangesInOneBuffer)
    {
        Render::BufferArena<int> arena(64);
        int created = 0;
        const auto create = [&](std::uint64_t) { return std::make_shared<int>(++created); };
        auto vertices = arena.allocate(16, 16, create);
        auto indices = arena.allocate(12, 4, create);
        auto nextMesh = arena.allocate(16, 16, create);
        EXPECT_EQ(created, 1);
        EXPECT_EQ(&vertices.page(), &indices.page());
        EXPECT_EQ(&vertices.page(), &nextMesh.page());
        EXPECT_EQ(vertices.offset(), 0u);
        EXPECT_EQ(indices.offset(), 16u);
        EXPECT_EQ(nextMesh.offset(), 32u);
        EXPECT_EQ(indices.size(), 12u);
    }

    TEST(RenderBufferArena, VertexStrideAlignmentSurvivesIndexPaddingAndRangeReuse)
    {
        Render::BufferArena<int> arena(384);
        const auto create = [](std::uint64_t) { return std::make_shared<int>(0); };
        auto firstVertices = arena.allocate(3 * 48, 48, create);
        auto firstIndices = arena.allocate(Render::meshIndexBytes(3, true), 4, create);
        auto nextVertices = arena.allocate(3 * 48, 48, create);
        EXPECT_EQ(&firstVertices.page(), &nextVertices.page());
        EXPECT_EQ(nextVertices.offset(), 192u);
        EXPECT_EQ(nextVertices.offset() % 48, 0u);
        firstVertices = {};
        auto replacement = arena.allocate(2 * 48, 48, create);
        EXPECT_EQ(&replacement.page(), &nextVertices.page());
        EXPECT_EQ(replacement.offset(), 0u);
        EXPECT_EQ(replacement.offset() % 48, 0u);
        EXPECT_NE(replacement.offset(), firstIndices.offset());
    }

    TEST(RenderBufferArena, SubmittedOwnerKeepsItsRangeUntilCompletion)
    {
        using Slice = Render::BufferArena<int>::Slice;
        static_assert(!std::is_copy_constructible_v<Slice>);
        static_assert(std::is_nothrow_move_constructible_v<Slice>);
        Render::BufferArena<int> arena(64);
        int created = 0;
        const auto create = [&](std::uint64_t) { return std::make_shared<int>(++created); };
        auto resident = std::make_shared<Slice>(arena.allocate(64, 16, create));
        auto submitted = resident;
        const auto* firstPage = &submitted->page();
        resident.reset();
        arena.trim();
        auto next = arena.allocate(64, 16, create);
        EXPECT_NE(&next.page(), firstPage);
        submitted.reset();
        auto reused = arena.allocate(64, 16, create);
        EXPECT_EQ(&reused.page(), firstPage);
        EXPECT_EQ(created, 2);
    }

    TEST(RenderBufferArena, MovingAndClearingKeepOutstandingRangesValid)
    {
        Render::BufferArena<int> arena(64);
        std::weak_ptr<int> page;
        const auto create = [&](std::uint64_t) {
            auto result = std::make_shared<int>(7);
            page = result;
            return result;
        };
        auto first = arena.allocate(16, 16, create);
        auto second = arena.allocate(16, 16, create);
        auto tail = arena.allocate(32, 16, create);
        second = std::move(first);
        EXPECT_FALSE(first);
        auto replacement = arena.allocate(16, 16, create);
        EXPECT_EQ(replacement.offset(), 16u);
        EXPECT_EQ(second.offset(), 0u);
        arena.clear();
        EXPECT_FALSE(page.expired());
        EXPECT_EQ(second.page(), 7);
        second = {};
        EXPECT_FALSE(page.expired());
        replacement = {};
        EXPECT_FALSE(page.expired());
        tail = {};
        EXPECT_TRUE(page.expired());
        EXPECT_THROW(arena.allocate(0, 16, create), std::invalid_argument);
        EXPECT_THROW(arena.allocate(16, 0, create), std::invalid_argument);
    }

    TEST(RenderTargets, OnlySceneTargetsAllocateDepth)
    {
        EXPECT_TRUE(Render::targetHasDepth(Render::TargetFormat::RGBA8));
        EXPECT_TRUE(Render::targetHasDepth(Render::TargetFormat::SceneRGBA16F));
        EXPECT_FALSE(Render::targetHasDepth(Render::TargetFormat::ColorRGBA8));
        EXPECT_FALSE(Render::targetHasDepth(Render::TargetFormat::ColorRGBA16F));
        EXPECT_FALSE(Render::targetHasDepth(Render::TargetFormat::R32F));
        EXPECT_FALSE(Render::targetHasDepth(Render::TargetFormat::RG16F));
    }

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

    TEST(RenderResidency, RecentWorkingSetIncludesTheUnionOfCameraViews)
    {
        Render::Residency cache(20);
        cache.setLimit(20, 250);
        Render::Residency::Charge north(&cache), south(&cache), old(&cache);
        north.set(100);
        south.set(100);
        old.set(10);
        cache.beginFrame();
        north.touch();
        cache.beginFrame();
        south.touch();
        south.touch();
        EXPECT_EQ(cache.activeBytes(), 100u);
        EXPECT_EQ(cache.recentBytes(), 200u);
        EXPECT_FALSE(cache.evict(true, 100, 1)); // Both views fit, plus warm data.
        for (unsigned i = 0; i < 255; ++i)
        {
            cache.beginFrame();
            south.touch();
        }
        EXPECT_EQ(cache.recentBytes(), 100u);
        EXPECT_TRUE(cache.evict(true, 400, 1));
    }

    TEST(RenderResidency, ResizingAndRemovingBucketNodesKeepsAccountingExact)
    {
        Render::Residency cache(20);
        Render::Residency::Charge first(&cache), middle(&cache), last(&cache);
        cache.beginFrame();
        for (auto* charge : { &first, &middle, &last })
        {
            charge->set(10);
            charge->touch();
        }
        middle.set(0); // Remove the middle of the bucket list.
        EXPECT_EQ(cache.recentBytes(), 20u);
        last.set(20);
        EXPECT_EQ(cache.recentBytes(), 30u);
        EXPECT_EQ(cache.activeBytes(), 30u);
        middle.set(15);
        middle.touch(); // Reinsert in the same frame after setting zero.
        EXPECT_EQ(cache.recentBytes(), 45u);
        cache.beginFrame();
        first.touch(); // Move an old tail to another bucket.
        last.set(0);
        EXPECT_EQ(cache.recentBytes(), 25u);
        for (unsigned i = 0; i < 256; ++i)
            cache.beginFrame();
        EXPECT_EQ(cache.recentBytes(), 0u);
        EXPECT_EQ(cache.bytes(), 25u);
        first.touch();
        EXPECT_EQ(cache.recentBytes(), 10u);
    }

    TEST(RenderResidency, VisibleWorkingSetDoesNotConsumeTheWarmCacheAllowance)
    {
        Render::Residency cache(100);
        cache.setLimit(100, 250);
        Render::Residency::Charge visible(&cache), warm(&cache);
        visible.set(160);
        warm.set(80);
        cache.beginFrame();
        visible.touch();
        visible.touch(); // Shared meshes and multiple views count once.
        EXPECT_EQ(cache.activeBytes(), 160u);
        EXPECT_FALSE(cache.evict(true, 100, 1)); // 80 bytes fit the warm allowance.
        warm.set(100);
        EXPECT_TRUE(cache.evict(true, 100, 1)); // Device ceiling wins over 160+100.
        EXPECT_FALSE(cache.evict(true, 100, 100)); // Visible data is never evicted.
        visible.set(170);
        EXPECT_EQ(cache.activeBytes(), 170u);
        warm.set(80);
        EXPECT_FALSE(cache.evict(true, 100, 1));
        cache.beginFrame();
        warm.touch();
        EXPECT_EQ(cache.activeBytes(), 80u);
        EXPECT_FALSE(cache.evict(true, 101, 1)); // A brief turn keeps the previous working set warm.
        for (unsigned i = 0; i < 260; ++i)
        {
            cache.beginFrame();
            warm.touch();
        }
        EXPECT_TRUE(cache.evict(true, 400, 1)); // The smaller working set eventually replaces its high watermark.
        warm.set(0);
        EXPECT_EQ(cache.activeBytes(), 0u);
        EXPECT_TRUE(cache.evict(false, 101, 101));
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

    TEST(RenderAllocationRanges, AppendPreferenceStillReusesHolesAndInvalidatesFailedFitCacheOnRelease)
    {
        Render::AllocationRanges ranges(64);
        EXPECT_EQ(ranges.allocate(3, 1, true), 0);
        EXPECT_EQ(ranges.allocate(16, 16, true), 16);
        EXPECT_EQ(ranges.allocate(13, 1, true), 32);
        EXPECT_EQ(ranges.allocate(19, 1, true), 45);
        EXPECT_FALSE(ranges.allocate(14, 1, true));
        EXPECT_EQ(ranges.allocate(13, 1, true), 3);
        EXPECT_FALSE(ranges.allocate(1, 1, true));
        ranges.release(16);
        EXPECT_EQ(ranges.allocate(16, 16, true), 16);
        for (auto offset : { 0u, 3u, 16u, 32u, 45u })
            ranges.release(offset);
        EXPECT_TRUE(ranges.empty());
        EXPECT_EQ(ranges.allocate(64, 1, true), 0);
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
