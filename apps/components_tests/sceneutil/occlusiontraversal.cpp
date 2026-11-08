#include <components/sceneutil/occlusiontraversal.hpp>

#include <gtest/gtest.h>

#include <osg/Camera>
#include <osg/MatrixTransform>

namespace
{
    struct OcclusionTraversalTest : testing::Test
    {
        osg::NodeVisitor mVisitor;
        osg::CullingSet mCulling;
        osg::ref_ptr<osg::Node> mNode = new osg::Node;

        OcclusionTraversalTest()
        {
            mVisitor.setTraversalMask(1);
            mNode->setNodeMask(1);
            mNode->setInitialBound(osg::BoundingSphere({ 0, 0, 0 }, 0.1));
            osg::Polytope frustum;
            frustum.setToUnitFrustum();
            mCulling.setFrustum(frustum);
            mCulling.setCullingMask(osg::CullingSet::VIEW_FRUSTUM_CULLING);
        }

        bool visible() { return SceneUtil::mayTraverseForOcclusion(*mNode, mVisitor, mCulling); }
    };

    TEST_F(OcclusionTraversalTest, node_masks_and_overrides_match_normal_traversal)
    {
        EXPECT_TRUE(visible());
        mNode->setNodeMask(2);
        EXPECT_FALSE(visible());
        mNode->setCullingActive(false);
        EXPECT_FALSE(visible());
        mVisitor.setNodeMaskOverride(1);
        EXPECT_TRUE(visible());
    }

    TEST_F(OcclusionTraversalTest, only_fully_outside_bounds_are_rejected)
    {
        mNode->setInitialBound(osg::BoundingSphere({ 10, 0, 0 }, 0.1));
        EXPECT_FALSE(visible());
        mNode->setInitialBound(osg::BoundingSphere({ 1, 0, 0 }, 0.1));
        EXPECT_TRUE(visible());
    }

    TEST_F(OcclusionTraversalTest, does_not_change_sibling_plane_masks)
    {
        auto& frustum = mCulling.getFrustum();
        const auto current = frustum.getCurrentMask();
        frustum.setResultMask(5);
        EXPECT_TRUE(visible());
        EXPECT_EQ(frustum.getResultMask(), 5u);
        EXPECT_EQ(frustum.getCurrentMask(), current);
        mNode->setInitialBound(osg::BoundingSphere({ 10, 0, 0 }, 0.1));
        EXPECT_FALSE(visible());
        EXPECT_EQ(frustum.getResultMask(), 5u);
        EXPECT_EQ(frustum.getCurrentMask(), current);
    }

    TEST_F(OcclusionTraversalTest, disabled_culling_and_invalid_bounds_are_conservative)
    {
        mNode->setInitialBound(osg::BoundingSphere({ 10, 0, 0 }, 0.1));
        mNode->setCullingActive(false);
        EXPECT_TRUE(visible());
        mNode->setCullingActive(true);
        mCulling.setCullingMask(osg::CullingSet::NO_CULLING);
        EXPECT_TRUE(visible());
        mCulling.setCullingMask(osg::CullingSet::VIEW_FRUSTUM_CULLING);
        mNode->setInitialBound(osg::BoundingSphere());
        EXPECT_TRUE(visible());
    }

    TEST_F(OcclusionTraversalTest, absolute_transforms_and_cameras_keep_their_special_traversal)
    {
        osg::ref_ptr<osg::MatrixTransform> transform = new osg::MatrixTransform;
        transform->setInitialBound(osg::BoundingSphere({ 10, 0, 0 }, 0.1));
        transform->setNodeMask(1);
        transform->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        EXPECT_TRUE(SceneUtil::mayTraverseForOcclusion(*transform, mVisitor, mCulling));
        transform->setReferenceFrame(osg::Transform::RELATIVE_RF);
        transform->setCullingActive(true);
        EXPECT_FALSE(SceneUtil::mayTraverseForOcclusion(*transform, mVisitor, mCulling));
        osg::ref_ptr<osg::Camera> camera = new osg::Camera;
        camera->setNodeMask(1);
        camera->setInitialBound(osg::BoundingSphere({ 10, 0, 0 }, 0.1));
        EXPECT_TRUE(SceneUtil::mayTraverseForOcclusion(*camera, mVisitor, mCulling));
    }
}
