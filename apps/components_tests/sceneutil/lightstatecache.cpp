#include <components/sceneutil/lightstatecache.hpp>

#include <gtest/gtest.h>
#include <stdexcept>

namespace
{
    osg::Matrixf read(const osg::StateSet& state, unsigned int index = 0)
    {
        osg::Matrixf result;
        EXPECT_TRUE(state.getUniform("LightBuffer")->getElement(index, result));
        return result;
    }

    struct LightStateCacheTest : testing::Test
    {
        SceneUtil::LightStateCache cache;
        osg::ref_ptr<osg::Camera> camera = new osg::Camera;
        int offset = 0;
        osg::ref_ptr<osg::StateSet> get(std::size_t frame, const std::vector<int>& ids, const osg::Matrixd& matrix = {},
            osg::Camera* selectedCamera = nullptr)
        {
            return cache.get(selectedCamera ? selectedCamera : camera.get(), matrix, frame, ids, 16,
                [&](std::size_t i) { return osg::Matrixf::translate(ids[i] + offset, matrix(3, 0), 0); });
        }
    };

    TEST_F(LightStateCacheTest, FrameSlotsReuseObjectsWithoutMutatingThePreviousFrame)
    {
        const auto first = get(1, { 7 });
        const auto firstBuffer = first->getUniform("LightBuffer");
        offset = 10;
        const auto second = get(2, { 7 });
        EXPECT_NE(first, second);
        EXPECT_EQ(read(*first).getTrans().x(), 7);
        EXPECT_EQ(read(*second).getTrans().x(), 17);
        offset = 20;
        const auto third = get(3, { 7 });
        EXPECT_EQ(first, third);
        EXPECT_EQ(firstBuffer, third->getUniform("LightBuffer"));
        EXPECT_EQ(read(*third).getTrans().x(), 27);
        EXPECT_EQ(read(*second).getTrans().x(), 17);
        for (std::size_t frame = 4; frame < 30; ++frame)
            get(frame, { 7 });
        EXPECT_EQ(cache.stats().mCreatedStates, 2);
    }

    TEST_F(LightStateCacheTest, OverlappingCombinationsPackEachLightOnceInOriginalOrder)
    {
        const auto a = get(1, { 4, 2 });
        const auto b = get(1, { 2, 9 });
        const auto c = get(1, { 9, 4 });
        EXPECT_EQ(cache.stats().mPackedLights, 3);
        EXPECT_EQ(read(*a).getTrans().x(), 4);
        EXPECT_EQ(read(*a, 1).getTrans().x(), 2);
        EXPECT_EQ(read(*b).getTrans().x(), 2);
        EXPECT_EQ(read(*c, 1).getTrans().x(), 4);
        EXPECT_EQ(a, get(1, { 4, 2 }));
        EXPECT_EQ(cache.stats().mCreatedStates, 3);
    }

    TEST_F(LightStateCacheTest, MatrixValuesAndCameraIdentitySeparateViews)
    {
        osg::Matrixd matrix;
        const auto first = get(1, { 7 }, matrix);
        matrix.makeTranslate(40, 0, 0);
        const auto second = get(1, { 7 }, matrix);
        EXPECT_NE(first, second);
        EXPECT_EQ(read(*first).getTrans().y(), 0);
        EXPECT_EQ(read(*second).getTrans().y(), 40);
        const osg::Matrixd identical(matrix);
        EXPECT_EQ(second, get(1, { 7 }, identical));
        osg::ref_ptr<osg::Camera> other = new osg::Camera;
        EXPECT_NE(second, get(1, { 7 }, matrix, other));
        EXPECT_EQ(cache.stats().mPackedLights, 3);
        // Reversing eye/view visitation next frame must refresh both slots.
        const auto left = get(2, { 7 }, matrix);
        const auto right = get(2, { 7 });
        EXPECT_EQ(read(*left).getTrans().y(), 40);
        EXPECT_EQ(read(*right).getTrans().y(), 0);
        EXPECT_EQ(read(*first).getTrans().y(), 0);
        EXPECT_EQ(read(*second).getTrans().y(), 40);
    }

    TEST_F(LightStateCacheTest, RepeatedUpdateInOneFrameAllocatesANewSnapshot)
    {
        const auto first = get(1, { 7 });
        cache.beginFrame(1);
        offset = 10;
        const auto updated = get(1, { 7 });
        EXPECT_NE(first, updated);
        EXPECT_EQ(read(*first).getTrans().x(), 7);
        EXPECT_EQ(read(*updated).getTrans().x(), 17);
    }

    TEST_F(LightStateCacheTest, UnknownIdentityAndExcessViewsUseFrameLocalState)
    {
        const auto pack = [](std::size_t) { return osg::Matrixf::translate(7, 0, 0); };
        const auto anonymous = cache.get(nullptr, {}, 1, { 7 }, 16, pack);
        EXPECT_EQ(anonymous, cache.get(nullptr, {}, 1, { 7 }, 16, pack));
        EXPECT_EQ(cache.stats().mRetainedStates, 0);
        EXPECT_NE(anonymous, cache.get(nullptr, {}, 2, { 7 }, 16, pack));
        for (unsigned int i = 0; i < SceneUtil::LightStateCache::MaxViews; ++i)
            get(2, { 7 }, osg::Matrixd::translate(i, 0, 0));
        const auto count = cache.stats().mRetainedStates;
        const auto fifth = get(2, { 7 }, osg::Matrixd::translate(100, 0, 0));
        get(2, { 7 }, osg::Matrixd::translate(101, 0, 0));
        EXPECT_EQ(fifth, get(2, { 7 }, osg::Matrixd::translate(100, 0, 0)));
        EXPECT_EQ(cache.stats().mRetainedStates, count);
        EXPECT_EQ(read(*fifth).getTrans().y(), 100);
    }

    TEST_F(LightStateCacheTest, RetentionLimitDoesNotDropLightsAndOverflowStillDeduplicates)
    {
        for (unsigned int i = 0; i < SceneUtil::LightStateCache::MaxStatesPerSlot; ++i)
            get(1, { int(i) });
        EXPECT_EQ(cache.stats().mRetainedStates, SceneUtil::LightStateCache::MaxStatesPerSlot);
        const auto overflow = get(1, { 99999 });
        EXPECT_EQ(overflow, get(1, { 99999 }));
        EXPECT_EQ(read(*overflow).getTrans().x(), 99999);
        EXPECT_EQ(cache.stats().mRetainedStates, SceneUtil::LightStateCache::MaxStatesPerSlot);
        EXPECT_LE(cache.stats().mUniformBytes, SceneUtil::LightStateCache::MaxUniformBytes);
        // The second slot still has its full share of the retention budget.
        get(2, { 7 });
        EXPECT_EQ(cache.stats().mRetainedStates, SceneUtil::LightStateCache::MaxStatesPerSlot + 1);
        cache.beginFrame(5);
        EXPECT_EQ(cache.stats().mRetainedStates, 0);
        EXPECT_EQ(cache.stats().mRetainedLights, 0);
    }

    TEST_F(LightStateCacheTest, UnchangedParametersDoNotDirtyRetainedUniforms)
    {
        const auto first = get(1, { 7 });
        const auto* buffer = first->getUniform("LightBuffer");
        const auto* count = first->getUniform("PointLightCount");
        const auto bufferRevision = buffer->getModifiedCount();
        const auto countRevision = count->getModifiedCount();
        EXPECT_EQ(first, get(3, { 7 }));
        EXPECT_EQ(bufferRevision, buffer->getModifiedCount());
        EXPECT_EQ(countRevision, count->getModifiedCount());
        offset = 10;
        EXPECT_EQ(first, get(5, { 7 }));
        EXPECT_GT(buffer->getModifiedCount(), bufferRevision);
        EXPECT_EQ(countRevision, count->getModifiedCount());
        EXPECT_EQ(read(*first).getTrans().x(), 17);
    }

    TEST_F(LightStateCacheTest, RecreatedPackedRecordsCannotReuseStaleParameterRevisions)
    {
        std::vector<int> ids(16);
        osg::ref_ptr<osg::StateSet> last;
        for (std::size_t first = 0; first <= SceneUtil::LightStateCache::MaxPackedLightsPerSlot; first += ids.size())
        {
            for (std::size_t i = 0; i < ids.size(); ++i)
                ids[i] = static_cast<int>(first + i);
            last = get(1, ids);
        }
        EXPECT_EQ(cache.stats().mRetainedLights, SceneUtil::LightStateCache::MaxPackedLightsPerSlot);
        offset = 100;
        // The last StateSet was retained, but its packed records were not.
        // They are recreated at the same frame slot with different parameters.
        EXPECT_EQ(last, get(3, ids));
        for (unsigned int i = 0; i < ids.size(); ++i)
            EXPECT_EQ(read(*last, i).getTrans().x(), ids[i] + offset);
    }

    TEST_F(LightStateCacheTest, UniformPayloadLimitAndIdleExpiryReleaseRetainedEntries)
    {
        const auto count = SceneUtil::LightStateCache::MaxUniformBytesPerSlot / (64 * sizeof(osg::Matrixf));
        const auto pack = [](std::size_t) { return osg::Matrixf::translate(7, 0, 0); };
        for (std::size_t i = 0; i <= count; ++i)
            cache.get(camera, {}, 1, { static_cast<int>(i) }, 64, pack);
        EXPECT_EQ(cache.stats().mRetainedStates, count);
        EXPECT_EQ(cache.stats().mUniformBytes, SceneUtil::LightStateCache::MaxUniformBytesPerSlot);
        cache.beginFrame(122);
        EXPECT_EQ(cache.stats().mRetainedStates, 0);
        EXPECT_EQ(cache.stats().mUniformBytes, 0);
        EXPECT_EQ(cache.stats().mRetainedLights, 0);
    }

    TEST_F(LightStateCacheTest, ExcessCamerasKeepIndependentFrameLocalFallbacks)
    {
        std::vector<osg::ref_ptr<osg::Camera>> cameras;
        for (std::size_t i = 0; i <= SceneUtil::LightStateCache::MaxCameras; ++i)
        {
            cameras.emplace_back(new osg::Camera);
            get(1, { 7 }, {}, cameras.back());
        }
        EXPECT_EQ(cache.stats().mRetainedStates, SceneUtil::LightStateCache::MaxCameras);
        const auto first = get(1, { 7 }, {}, cameras.back());
        EXPECT_EQ(first, get(1, { 7 }, {}, cameras.back()));
        const auto next = get(3, { 7 }, {}, cameras.back());
        EXPECT_NE(first, next);
        EXPECT_EQ(cache.stats().mRetainedStates, SceneUtil::LightStateCache::MaxCameras);
    }

    TEST_F(LightStateCacheTest, ExpiredCameraDoesNotKeepSceneAliveAndRetiresItsEntries)
    {
        const auto state = get(1, { 7 });
        osg::observer_ptr<osg::Camera> weak = camera;
        camera = nullptr;
        EXPECT_FALSE(weak.valid());
        cache.beginFrame(2);
        EXPECT_EQ(cache.stats().mRetainedStates, 0);
        EXPECT_EQ(cache.stats().mRetainedLights, 0);
        EXPECT_EQ(read(*state).getTrans().x(), 7);
    }

    TEST_F(LightStateCacheTest, FrameResetAndShaderCapacityChangeUseFreshState)
    {
        const auto first = get(10, { 7 });
        offset = 10;
        const auto reset = get(1, { 7 });
        EXPECT_NE(first, reset);
        EXPECT_EQ(read(*first).getTrans().x(), 7);
        const auto resized
            = cache.get(camera, {}, 1, { 7 }, 2, [](std::size_t) { return osg::Matrixf::translate(27, 0, 0); });
        EXPECT_NE(reset, resized);
        EXPECT_EQ(resized->getUniform("LightBuffer")->getNumElements(), 2);
        EXPECT_EQ(read(*reset).getTrans().x(), 17);
        EXPECT_THROW(cache.get(camera, {}, 1, { 1, 2, 3 }, 2, [](std::size_t) { return osg::Matrixf{}; }),
            std::invalid_argument);
    }
}
