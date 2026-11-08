#include <components/sceneutil/lightmanager.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

#include <osg/Camera>
#include <osgUtil/CullVisitor>
#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>

namespace
{
    void expectIndexedMatches(
        const std::vector<osg::BoundingSphere>& spheres, const std::vector<osg::BoundingSphere>& queries)
    {
        SceneUtil::LightBoundsIndex index;
        index.build(spheres);
        std::vector<std::size_t> candidates;
        for (const auto& query : queries)
        {
            ASSERT_TRUE(index.queryCandidates(query, candidates));
            std::sort(candidates.begin(), candidates.end());
            ASSERT_EQ(std::adjacent_find(candidates.begin(), candidates.end()), candidates.end());
            std::vector<std::size_t> expected, actual;
            for (std::size_t i = 0; i < spheres.size(); ++i)
                if (spheres[i].intersects(query))
                    expected.push_back(i);
            for (std::size_t candidate : candidates)
            {
                ASSERT_LT(candidate, spheres.size());
                if (spheres[candidate].intersects(query))
                    actual.push_back(candidate);
            }
            EXPECT_EQ(actual, expected);
        }
    }

    TEST(LightBoundsIndexTest, random_queries_match_the_legacy_sphere_predicate)
    {
        std::mt19937 generator(0x6c696768);
        std::uniform_real_distribution<float> coordinate(-50000, 50000);
        std::uniform_real_distribution<float> radius(0, 5000);
        std::vector<osg::BoundingSphere> spheres, queries;
        for (unsigned int i = 0; i < 512; ++i)
            spheres.emplace_back(
                osg::Vec3f(coordinate(generator), coordinate(generator), coordinate(generator)), radius(generator));
        for (unsigned int i = 0; i < 128; ++i)
            queries.emplace_back(
                osg::Vec3f(coordinate(generator), coordinate(generator), coordinate(generator)), radius(generator) * 2);
        expectIndexedMatches(spheres, queries);
    }

    TEST(LightBoundsIndexTest, tangent_and_malformed_bounds_match_legacy_behavior)
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        const std::vector<osg::BoundingSphere> spheres{
            { { 0, 0, 0 }, 1 },
            { { 2, 0, 0 }, 1 },
            { { 1e7f, 0, 0 }, 1 },
            { { 1e30f, 0, 0 }, 1e30f },
            { { 0, 0, 0 }, inf },
            { { nan, 0, 0 }, 1 },
            { { 0, 0, 0 }, -1 },
            { { 0, 0, 0 }, 0 },
        };
        expectIndexedMatches(spheres,
            {
                { { 2, 0, 0 }, 1 },
                { { std::nextafter(2.f, 0.f), 0, 0 }, 1 },
                { { std::nextafter(2.f, inf), 0, 0 }, 1 },
                { { 1e7f + 2, 0, 0 }, 1 },
                { { 0, 0, 0 }, 0 },
                { { 0, 0, 0 }, -1 },
                { { nan, 0, 0 }, 1 },
                { { 0, 0, 0 }, inf },
                { { -1e30f, 0, 0 }, 1e30f },
            });
    }

    TEST(LightBoundsIndexTest, sparse_queries_exclude_distant_lights_and_rebuild_drops_old_bounds)
    {
        std::vector<osg::BoundingSphere> spheres;
        for (unsigned int i = 0; i < 128; ++i)
            spheres.emplace_back(osg::Vec3f(i * 100.f, 0, 0), 1);
        SceneUtil::LightBoundsIndex index;
        index.build(spheres);
        std::vector<std::size_t> candidates;
        ASSERT_TRUE(index.queryCandidates(osg::BoundingSphere({ 5000, 0, 0 }, 1), candidates));
        ASSERT_EQ(candidates, std::vector<std::size_t>{ 50 });
        spheres.clear();
        index.build(spheres);
        ASSERT_TRUE(index.queryCandidates(osg::BoundingSphere({ 5000, 0, 0 }, 1), candidates));
        EXPECT_TRUE(candidates.empty());
    }

    TEST(LightBoundsIndexTest, dense_query_reports_budget_exhaustion)
    {
        const std::vector<osg::BoundingSphere> spheres(64, osg::BoundingSphere({ 0, 0, 0 }, 10));
        SceneUtil::LightBoundsIndex index;
        index.build(spheres);
        std::vector<std::size_t> candidates;
        EXPECT_FALSE(index.queryCandidates(osg::BoundingSphere({ 0, 0, 0 }, 1), candidates, 8));
        EXPECT_LE(candidates.size(), 8u);
        expectIndexedMatches(spheres, { osg::BoundingSphere({ 0, 0, 0 }, 1) });
    }

    struct TestCullVisitor : osgUtil::CullVisitor
    {
        ~TestCullVisitor() override = default;
    };

    struct CountBounds : osg::Node::ComputeBoundingSphereCallback
    {
        mutable unsigned int mCalls = 0;

        osg::BoundingSphere computeBound(const osg::Node&) const override
        {
            ++mCalls;
            return osg::BoundingSphere(osg::Vec3f(0, 0, -10), 1);
        }
    };

    struct LightListTest : testing::Test
    {
        osg::ref_ptr<SceneUtil::LightManager> mManager;
        osg::ref_ptr<osg::Node> mNode = new osg::Node;
        osg::ref_ptr<CountBounds> mBounds = new CountBounds;
        osg::ref_ptr<osg::Camera> mCamera = new osg::Camera;
        osg::ref_ptr<osgUtil::RenderStage> mStage = new osgUtil::RenderStage;
        osg::ref_ptr<osgUtil::StateGraph> mStateGraph = new osgUtil::StateGraph;
        TestCullVisitor mVisitor;
        SceneUtil::LightListCallback mCallback;
        std::vector<osg::ref_ptr<SceneUtil::LightSource>> mLights;

        LightListTest()
        {
            SceneUtil::LightSettings settings;
            settings.mMaxLights = 2;
            settings.mMaximumLightDistance = 0;
            mManager = new SceneUtil::LightManager(settings);
            mNode->setComputeBoundingSphereCallback(mBounds);

            mStage->setCamera(mCamera);
            mStage->setInitialViewMatrix(new osg::RefMatrix(osg::Matrixd::identity()));
            mVisitor.setRenderStage(mStage);
            mVisitor.setStateGraph(mStateGraph);
            mVisitor.setTraversalNumber(1);
            mVisitor.pushViewport(new osg::Viewport(0, 0, 640, 480));
            mVisitor.pushProjectionMatrix(new osg::RefMatrix(osg::Matrixd::perspective(60, 4.0 / 3, 1, 1000)));
            mVisitor.pushModelViewMatrix(new osg::RefMatrix(osg::Matrixd::identity()), osg::Transform::ABSOLUTE_RF);
            mVisitor.pushOntoNodePath(mManager);
        }

        ~LightListTest() override
        {
            mVisitor.popFromNodePath();
            mVisitor.popModelViewMatrix();
            mVisitor.popProjectionMatrix();
            mVisitor.popViewport();
        }

        SceneUtil::LightSource* addLight(float x, float radius)
        {
            auto& light = mLights.emplace_back(new SceneUtil::LightSource);
            light->setRadius(radius);
            mManager->addLight(light, osg::Matrixf::translate(x, 0, -10), mVisitor.getTraversalNumber());
            return light;
        }
    };

    TEST_F(LightListTest, empty_light_list_does_not_compute_object_bounds)
    {
        EXPECT_FALSE(mCallback.pushLightState(mNode, &mVisitor));
        EXPECT_EQ(mBounds->mCalls, 0u);
    }

    TEST_F(LightListTest, capped_selection_preserves_radius_weighting_and_ignored_lights)
    {
        addLight(4, 10);
        addLight(8, 80);
        addLight(50, 100);
        addLight(2, 10);
        mCallback.getIgnoredLightSources().insert(addLight(1, 10));

        ASSERT_TRUE(mCallback.pushLightState(mNode, &mVisitor));
        const osg::StateSet* stateset = mVisitor.getCurrentStateGraph()->getStateSet();
        ASSERT_NE(stateset, nullptr);
        const osg::Uniform* count = stateset->getUniform("PointLightCount");
        const osg::Uniform* buffer = stateset->getUniform("LightBuffer");
        ASSERT_NE(count, nullptr);
        ASSERT_NE(buffer, nullptr);
        int numLights = 0;
        ASSERT_TRUE(count->get(numLights));
        EXPECT_EQ(numLights, 2);
        osg::Matrixf light;
        ASSERT_TRUE(buffer->getElement(0, light));
        EXPECT_FLOAT_EQ(light(0, 0), 2);
        ASSERT_TRUE(buffer->getElement(1, light));
        EXPECT_FLOAT_EQ(light(0, 0), 8);
        mVisitor.popStateSet();
    }

    TEST_F(LightListTest, removing_all_lights_drops_cached_selection_without_recomputing_bounds)
    {
        addLight(2, 10);
        ASSERT_TRUE(mCallback.pushLightState(mNode, &mVisitor));
        mVisitor.popStateSet();
        EXPECT_EQ(mBounds->mCalls, 1u);

        mNode->dirtyBound();
        mManager->update(2);
        mVisitor.setTraversalNumber(2);
        EXPECT_FALSE(mCallback.pushLightState(mNode, &mVisitor));
        EXPECT_EQ(mBounds->mCalls, 1u);
    }

    TEST_F(LightListTest, moving_lights_invalidate_the_previous_frames_spatial_index)
    {
        for (unsigned int i = 0; i < 64; ++i)
            addLight(5000 + i * 100.f, 10);
        EXPECT_FALSE(mCallback.pushLightState(mNode, &mVisitor));

        mManager->update(2);
        mVisitor.setTraversalNumber(2);
        for (std::size_t i = 0; i < mLights.size(); ++i)
            mManager->addLight(mLights[i], osg::Matrixf::translate(i == 37 ? 2.f : 5000 + i * 100.f, 0, -10), 2);
        ASSERT_TRUE(mCallback.pushLightState(mNode, &mVisitor));
        const auto* count = mVisitor.getCurrentStateGraph()->getStateSet()->getUniform("PointLightCount");
        ASSERT_NE(count, nullptr);
        int value = 0;
        ASSERT_TRUE(count->get(value));
        EXPECT_EQ(value, 1);
        mVisitor.popStateSet();
    }

    TEST_F(LightListTest, dense_index_fallback_preserves_light_ranking)
    {
        for (unsigned int i = 1; i <= 64; ++i)
        {
            auto* light = addLight(i, 1000);
            if (i == 1)
                mCallback.getIgnoredLightSources().insert(light);
        }
        ASSERT_TRUE(mCallback.pushLightState(mNode, &mVisitor));
        const auto* buffer = mVisitor.getCurrentStateGraph()->getStateSet()->getUniform("LightBuffer");
        ASSERT_NE(buffer, nullptr);
        osg::Matrixf light;
        ASSERT_TRUE(buffer->getElement(0, light));
        EXPECT_FLOAT_EQ(light(0, 0), 2);
        ASSERT_TRUE(buffer->getElement(1, light));
        EXPECT_FLOAT_EQ(light(0, 0), 3);
        mVisitor.popStateSet();
    }

    TEST_F(LightListTest, sparse_index_preserves_input_order_below_the_light_limit)
    {
        addLight(8, 10);
        for (unsigned int i = 0; i < 64; ++i)
            addLight(5000 + i * 100.f, 10);
        addLight(2, 10);
        ASSERT_TRUE(mCallback.pushLightState(mNode, &mVisitor));
        const auto* buffer = mVisitor.getCurrentStateGraph()->getStateSet()->getUniform("LightBuffer");
        ASSERT_NE(buffer, nullptr);
        osg::Matrixf light;
        ASSERT_TRUE(buffer->getElement(0, light));
        EXPECT_FLOAT_EQ(light(0, 0), 8);
        ASSERT_TRUE(buffer->getElement(1, light));
        EXPECT_FLOAT_EQ(light(0, 0), 2);
        mVisitor.popStateSet();
    }

    TEST_F(LightListTest, spatial_indices_are_separate_for_different_camera_views)
    {
        for (unsigned int i = 0; i < 64; ++i)
            addLight(5000 + i * 100.f, 10);
        EXPECT_FALSE(mCallback.pushLightState(mNode, &mVisitor));

        osg::ref_ptr<osg::Camera> otherCamera = new osg::Camera;
        osg::ref_ptr<osgUtil::RenderStage> otherStage = new osgUtil::RenderStage;
        otherStage->setCamera(otherCamera);
        otherStage->setInitialViewMatrix(new osg::RefMatrix(osg::Matrixd::translate(-5000, 0, 0)));
        mVisitor.setRenderStage(otherStage);
        SceneUtil::LightListCallback otherCallback;
        ASSERT_TRUE(otherCallback.pushLightState(mNode, &mVisitor));
        const auto* count = mVisitor.getCurrentStateGraph()->getStateSet()->getUniform("PointLightCount");
        ASSERT_NE(count, nullptr);
        int value = 0;
        ASSERT_TRUE(count->get(value));
        EXPECT_EQ(value, 1);
        mVisitor.popStateSet();
        mVisitor.setRenderStage(mStage);
    }

    TEST_F(LightListTest, reusable_cache_key_does_not_overwrite_other_light_sets)
    {
        addLight(2, 10);
        addLight(8, 10);
        const auto* view = mStage->getInitialViewMatrix();
        const auto& lights = mManager->getLightsInViewSpace(&mVisitor, view, 1);
        ASSERT_EQ(lights.size(), 2u);
        const SceneUtil::LightManager::LightList first{ &lights[0] };
        const SceneUtil::LightManager::LightList second{ &lights[1] };
        const auto firstState = mManager->getLightListStateSet(first, 1, view);
        const auto secondState = mManager->getLightListStateSet(second, 1, view);
        EXPECT_NE(firstState, secondState);
        EXPECT_EQ(firstState, mManager->getLightListStateSet(first, 1, view));
        osg::Matrixf light;
        ASSERT_TRUE(firstState->getUniform("LightBuffer")->getElement(0, light));
        EXPECT_FLOAT_EQ(light(0, 0), 2);
        ASSERT_TRUE(secondState->getUniform("LightBuffer")->getElement(0, light));
        EXPECT_FLOAT_EQ(light(0, 0), 8);
    }
}
