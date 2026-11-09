#include <components/sceneutil/occlusionculling.hpp>

#include <MaskedOcclusionCulling.h>
#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Group>
#include <osgUtil/CullVisitor>
#include <osgUtil/RenderStage>

#include <gtest/gtest.h>

namespace
{
    struct TestCullVisitor : osgUtil::CullVisitor
    {
        ~TestCullVisitor() override = default;
    };
    const std::vector<osg::Vec3f> Wall{ { -8, -6, -10 }, { 8, -6, -10 }, { 8, 6, -10 }, { -8, 6, -10 } };
    const std::vector<unsigned int> Indices{ 0, 1, 2, 0, 2, 3 };

    TEST(OcclusionQueryTest, solidWallCullsBehindButNeverInFrontOrAcrossNearPlane)
    {
        SceneUtil::OcclusionCuller culler(256, 128);
        culler.beginFrame(osg::Matrixd::identity(), osg::Matrixd::perspective(90, 1, 1, 100), 1);
        culler.rasterizeTerrainOccluder(Wall, Indices);
        EXPECT_FALSE(culler.testVisibleAABB(osg::BoundingBox(-0.5, -0.5, -21, 0.5, 0.5, -20)));
        EXPECT_FALSE(culler.testVisibleAABBTerrainOnly(osg::BoundingBox(-0.5, -0.5, -21, 0.5, 0.5, -20)));
        EXPECT_TRUE(culler.testVisibleAABB(osg::BoundingBox(-0.5, -0.5, -6, 0.5, 0.5, -5)));
        EXPECT_TRUE(culler.testVisibleAABB(osg::BoundingBox(-0.5, -0.5, -2, 0.5, 0.5, 1)));
        culler.endFrame();
        EXPECT_TRUE(culler.testVisibleAABB(osg::BoundingBox(-0.5, -0.5, -21, 0.5, 0.5, -20)));
    }

    TEST(OcclusionQueryTest, localMeshUsesSimdTransformAndBackfaceRejection)
    {
        SceneUtil::OcclusionCuller culler(256, 128);
        culler.beginFrame(osg::Matrixd::identity(), osg::Matrixd::perspective(90, 1, 1, 100), 1);
        auto local = Wall;
        for (auto& vertex : local)
            vertex.z() = 0;
        const osg::Matrixf placement = osg::Matrixf::translate(0, 0, -10);
        culler.rasterizeOccluder(local, Indices, &placement);
        EXPECT_FALSE(culler.testVisibleAABB(osg::BoundingBox(-0.5, -0.5, -21, 0.5, 0.5, -20)));
        EXPECT_TRUE(culler.testVisibleAABBTerrainOnly(osg::BoundingBox(-0.5, -0.5, -21, 0.5, 0.5, -20)));
        culler.beginFrame(osg::Matrixd::identity(), osg::Matrixd::perspective(90, 1, 1, 100), 1);
        culler.rasterizeOccluder(Wall, { 2, 1, 0, 3, 2, 0 });
        EXPECT_TRUE(culler.testVisibleAABB(osg::BoundingBox(-0.5, -0.5, -21, 0.5, 0.5, -20)));
    }

    TEST(OcclusionQueryTest, AnOccluderCannotHideItsOwnConservativeBounds)
    {
        SceneUtil::OcclusionCuller culler(256, 128);
        culler.beginFrame(osg::Matrixd::identity(), osg::Matrixd::perspective(90, 1, 1, 100), 1);
        culler.rasterizeOccluder(Wall, Indices);
        osg::BoundingBox bounds;
        for (const auto& vertex : Wall)
            bounds.expandBy(vertex);
        EXPECT_TRUE(culler.testVisibleAABB(bounds));
        EXPECT_FALSE(culler.testVisibleAABB(osg::BoundingBox(-0.5, -0.5, -21, 0.5, 0.5, -20)));
    }

    TEST(OcclusionQueryTest, CameraInsideModelBoundsDoesNotDiscardItsOpaqueWallTriangles)
    {
        SceneUtil::OcclusionCuller culler(256, 128);
        culler.beginFrame(osg::Matrixd::identity(), osg::Matrixd::perspective(90, 1, 1, 100), 1);
        const osg::BoundingBox enclosing(-8, -6, -10, 8, 6, 2);
        SceneUtil::OcclusionCuller::ProjectedBounds projected;
        EXPECT_FALSE(culler.projectBounds(enclosing, projected));
        ASSERT_TRUE(culler.projectOccluderBounds(enclosing, projected));
        EXPECT_DOUBLE_EQ(projected.screenRatio(), 1);
        EXPECT_TRUE(culler.testVisibleAABB(enclosing));

        auto vertices = Wall;
        // A finite vertex on the camera plane must not discard the entire
        // model in debug builds. MOC clips triangles before perspective divide.
        vertices.push_back({ 0, 0, 0 });
        culler.rasterizeOccluder(vertices, Indices);
        EXPECT_FALSE(culler.testVisibleAABB(osg::BoundingBox(-0.5, -0.5, -21, 0.5, 0.5, -20)));
        EXPECT_TRUE(culler.testVisibleAABB(enclosing));
        EXPECT_TRUE(culler.testVisibleAABB(osg::BoundingBox(-0.5, -0.5, -6, 0.5, 0.5, -5)));
    }

    TEST(OcclusionQueryTest, ClippedCandidateRankingRejectsBehindCameraAndOffscreenBounds)
    {
        SceneUtil::OcclusionCuller culler(256, 128);
        culler.beginFrame(osg::Matrixd::identity(), osg::Matrixd::perspective(90, 1, 1, 100), 1);
        SceneUtil::OcclusionCuller::ProjectedBounds projected;
        EXPECT_FALSE(culler.projectOccluderBounds(osg::BoundingBox(-1, -1, 1, 1, 1, 2), projected));
        EXPECT_FALSE(culler.projectOccluderBounds(osg::BoundingBox(100, -1, -10, 110, 1, 2), projected));
        EXPECT_TRUE(culler.projectOccluderBounds(osg::BoundingBox(-1, -1, -10, 1, 1, -5), projected));
        EXPECT_LT(projected.screenRatio(), 1);
    }

    TEST(OcclusionQueryTest, simdProjectionMatchesKnownPerspectiveBounds)
    {
        SceneUtil::OcclusionCuller culler(256, 128);
        culler.beginFrame(osg::Matrixd::identity(), osg::Matrixd::perspective(90, 1, 1, 100), 1);
        SceneUtil::OcclusionCuller::ProjectedBounds bounds;
        ASSERT_TRUE(culler.projectBounds(osg::BoundingBox(-1, -2, -20, 1, 2, -10), bounds));
        EXPECT_NEAR(bounds.minX, -0.1, 1e-6);
        EXPECT_NEAR(bounds.maxY, 0.2, 1e-6);
        EXPECT_NEAR(bounds.minW, 10, 1e-6);
        EXPECT_NEAR(bounds.screenRatio(), 0.02, 1e-6);
    }

    TEST(OcclusionQueryTest, bufferBelongsToOneVisitorCameraProjectionAndFrame)
    {
        const auto projection = osg::Matrixd::perspective(90, 1, 1, 100);
        osg::ref_ptr<osg::Camera> camera = new osg::Camera;
        osg::ref_ptr<osg::Camera> otherCamera = new osg::Camera;
        osg::ref_ptr<osgUtil::RenderStage> stage = new osgUtil::RenderStage;
        stage->setCamera(camera);
        osg::ref_ptr<osg::FrameStamp> stamp = new osg::FrameStamp;
        stamp->setFrameNumber(1);
        TestCullVisitor visitor, otherVisitor;
        visitor.setRenderStage(stage);
        visitor.setFrameStamp(stamp);
        visitor.pushViewport(new osg::Viewport(0, 0, 64, 32));
        visitor.pushProjectionMatrix(new osg::RefMatrix(projection));
        visitor.pushModelViewMatrix(new osg::RefMatrix(osg::Matrixd::identity()), osg::Transform::ABSOLUTE_RF);
        SceneUtil::OcclusionCuller culler(8, 4);
        culler.beginFrame(osg::Matrixd::identity(), projection);
        osg::ref_ptr<osg::Group> worldRoot = new osg::Group;
        visitor.pushOntoNodePath(worldRoot);
        culler.bindVisitor(visitor);
        EXPECT_TRUE(culler.isFrameActiveFor(visitor));
        visitor.pushOntoNodePath(worldRoot);
        EXPECT_FALSE(culler.isFrameActiveFor(visitor));
        visitor.popFromNodePath();
        EXPECT_TRUE(culler.isFrameActiveFor(visitor));
        EXPECT_FALSE(culler.isFrameActiveFor(otherVisitor));
        stage->setCamera(otherCamera);
        EXPECT_FALSE(culler.isFrameActiveFor(visitor));
        stage->setCamera(camera);
        visitor.pushProjectionMatrix(new osg::RefMatrix(osg::Matrixd::perspective(60, 1, 1, 100)));
        EXPECT_FALSE(culler.isFrameActiveFor(visitor));
        visitor.popProjectionMatrix();
        stamp->setFrameNumber(2);
        EXPECT_FALSE(culler.isFrameActiveFor(visitor));
        stamp->setFrameNumber(1);
        EXPECT_TRUE(culler.isFrameActiveFor(visitor));
        culler.endFrame();
        EXPECT_FALSE(culler.isFrameActiveFor(visitor));
        visitor.popFromNodePath();
        visitor.popModelViewMatrix();
        visitor.popProjectionMatrix();
        visitor.popViewport();
    }

    TEST(OcclusionQueryTest, everyRequestedSimdBackendHasSafeCpuFallback)
    {
        for (const auto implementation : { MaskedOcclusionCulling::SSE2, MaskedOcclusionCulling::SSE41,
                 MaskedOcclusionCulling::AVX2, MaskedOcclusionCulling::AVX512 })
        {
            auto* rasterizer = MaskedOcclusionCulling::Create(implementation);
            ASSERT_NE(rasterizer, nullptr);
            EXPECT_LE(rasterizer->GetImplementation(), implementation);
            rasterizer->SetResolution(64, 32);
            rasterizer->ClearBuffer();
            EXPECT_EQ(rasterizer->TestRect(-0.1f, -0.1f, 0.1f, 0.1f, 10), MaskedOcclusionCulling::VISIBLE);
            MaskedOcclusionCulling::Destroy(rasterizer);
        }
    }
}
