#include <components/sceneutil/occlusionplan.hpp>
#include <gtest/gtest.h>

namespace
{
    using namespace SceneUtil;
    std::shared_ptr<const OcclusionCulling::OccluderMesh> wall(float z)
    {
        auto mesh = std::make_shared<OcclusionCulling::OccluderMesh>();
        mesh->vertices = { { -8, -6, z }, { 8, -6, z }, { 8, 6, z }, { -8, 6, z } };
        mesh->indices = { 0, 1, 2, 0, 2, 3 };
        for (const auto& p : mesh->vertices)
            mesh->aabb.expandBy(p);
        return mesh;
    }
    struct OcclusionPlanTest : testing::Test
    {
        OcclusionCuller culler{ 256, 128 };
        OcclusionPlan plan;
        OcclusionViewKey key;
        OcclusionPlanTest()
        {
            key.mProjection = osg::Matrixd::perspective(90, 1, 1, 100);
            key.mNear = 1;
            key.mViewport.set(0, 0, 256, 128);
        }
        bool prepare(
            const std::vector<std::shared_ptr<const OcclusionCulling::OccluderMesh>>& meshes, double preparationMs = 0)
        {
            culler.beginFrame(key.mView, key.mProjection, key.mNear, true);
            plan.start();
            for (const auto& mesh : meshes)
                plan.add(culler, { mesh, {}, mesh->aabb, false });
            const bool reused = plan.prepare(key);
            if (!reused)
                culler.clearDepth();
            culler.resetStaticRasterBudgetBaseline();
            culler.recordStaticCandidate(preparationMs);
            plan.rasterize(culler);
            return reused;
        }
        bool hidden() const { return !culler.testVisibleAABB(osg::BoundingBox(-.5, -.5, -21, .5, .5, -20)); }
    };
    TEST_F(OcclusionPlanTest, SameViewRetainsDepthWithoutRasterizingAgain)
    {
        auto mesh = wall(-10);
        EXPECT_FALSE(prepare({ mesh }));
        EXPECT_TRUE(hidden());
        EXPECT_TRUE(plan.complete());
        EXPECT_TRUE(prepare({ mesh }));
        EXPECT_EQ(culler.getRasterizeCalls(), 0);
        EXPECT_EQ(culler.getNumBuildingTris(), 2);
        EXPECT_TRUE(hidden());
    }
    TEST_F(OcclusionPlanTest, RemovalAndReplacementClearOldWallsBeforeQueries)
    {
        auto mesh = wall(-10);
        prepare({ mesh });
        ASSERT_TRUE(hidden());
        EXPECT_FALSE(prepare({}));
        EXPECT_FALSE(hidden());
        EXPECT_FALSE(prepare({ wall(-10) }));
        EXPECT_TRUE(hidden());
    }
    TEST_F(OcclusionPlanTest, CandidateInsertionOrderDoesNotChangeThePlan)
    {
        auto near = wall(-8), far = wall(-10);
        EXPECT_FALSE(prepare({ far, near }));
        EXPECT_TRUE(prepare({ near, far }));
        EXPECT_TRUE(hidden());
        EXPECT_EQ(culler.getRasterizeCalls(), 0);
    }
    TEST_F(OcclusionPlanTest, WorkContinuesOverBudgetWithoutDiscardingCompletedCoverage)
    {
        auto a = wall(-8), b = wall(-9), c = wall(-10);
        culler.setStaticRasterBudgetMs(0.5);
        for (unsigned int i = 1; i <= 3; ++i)
        {
            EXPECT_EQ(prepare({ a, b, c }, 1), i != 1);
            EXPECT_EQ(culler.getRasterizeCalls(), 1);
            EXPECT_EQ(culler.getNumBuildingTris(), i * 2);
            EXPECT_TRUE(hidden());
        }
        EXPECT_TRUE(plan.complete());
        EXPECT_TRUE(prepare({ a, b, c }));
        EXPECT_EQ(culler.getRasterizeCalls(), 0);
    }
    TEST_F(OcclusionPlanTest, TriangleLimitBoundsBothFreshAndResumedPlans)
    {
        auto a = wall(-8), b = wall(-10);
        key.mMaxTriangles = 2;
        prepare({ a, b });
        EXPECT_EQ(culler.getNumBuildingTris(), 2);
        EXPECT_TRUE(plan.complete());
        EXPECT_TRUE(prepare({ a, b }));
        EXPECT_EQ(culler.getNumBuildingTris(), 2);
        key.mMaxTriangles = 4;
        EXPECT_FALSE(prepare({ a, b }));
        EXPECT_EQ(culler.getNumBuildingTris(), 4);
    }
    TEST_F(OcclusionPlanTest, ViewportClipPlaneAndTerrainRevisionInvalidateDepth)
    {
        auto mesh = wall(-10);
        prepare({ mesh });
        key.mViewport.z() = 512;
        EXPECT_FALSE(prepare({ mesh }));
        key.mPlanes.emplace_back(0, 0, 1, 20);
        EXPECT_FALSE(prepare({ mesh }));
        ++key.mTerrainRevision;
        EXPECT_FALSE(prepare({ mesh }));
        key.mCamera = &plan;
        EXPECT_FALSE(prepare({ mesh }));
        key.mView = osg::Matrixd::translate(-1, 0, 0);
        EXPECT_FALSE(prepare({ mesh }));
        key.mSmallFeature = 4;
        EXPECT_FALSE(prepare({ mesh }));
        key.mLodScale = 2;
        EXPECT_FALSE(prepare({ mesh }));
        key.mCullingMode = 0x10;
        EXPECT_FALSE(prepare({ mesh }));
    }
    TEST_F(OcclusionPlanTest, MovingPlacementCannotReuseItsOldSilhouette)
    {
        auto mesh = wall(-10);
        prepare({ mesh });
        ASSERT_TRUE(hidden());
        culler.beginFrame(key.mView, key.mProjection, 1, true);
        plan.start();
        const auto matrix = osg::Matrixf::translate(3, 0, 0);
        osg::BoundingBox bounds(mesh->aabb);
        bounds._min.x() += 3;
        bounds._max.x() += 3;
        plan.add(culler, { mesh, matrix, bounds, true });
        EXPECT_FALSE(plan.prepare(key));
        culler.clearDepth();
        plan.rasterize(culler);
        EXPECT_GT(culler.getRasterizeCalls(), 0);
    }
    TEST_F(OcclusionPlanTest, LargeWallsAreSelectedByTheirNearestBoundsRatherThanCentre)
    {
        EXPECT_DOUBLE_EQ(occlusionDistanceSquared(osg::BoundingBox(1, -2, -2, 10000, 2, 2), osg::Vec3d()), 1);
    }
}
