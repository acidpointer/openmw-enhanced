#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>

#include <gtest/gtest.h>

#include <osg/MatrixTransform>
#include <osgUtil/CullVisitor>

namespace
{
    class CaptureRig : public osgUtil::CullVisitor
    {
    public:
        osg::Geometry* mGeometry = nullptr;
        unsigned mVisits = 0;
        void apply(osg::Geometry& geometry) override
        {
            mGeometry = &geometry;
            ++mVisits;
        }
    };

    struct RigGeometryTest : testing::Test
    {
        osg::ref_ptr<SceneUtil::Skeleton> mSkeleton = new SceneUtil::Skeleton;
        osg::ref_ptr<osg::MatrixTransform> mBone = new osg::MatrixTransform;
        osg::ref_ptr<osg::MatrixTransform> mPart = new osg::MatrixTransform;
        osg::ref_ptr<SceneUtil::RigGeometry> mRig = new SceneUtil::RigGeometry;
        osg::ref_ptr<osg::Vec3Array> mPositions = new osg::Vec3Array;
        osg::ref_ptr<CaptureRig> mCull = new CaptureRig;

        RigGeometryTest()
        {
            mBone->setName("bone");
            mPart->setName("part");
            mRig->setName("part");
            mSkeleton->addChild(mBone);
            mSkeleton->addChild(mPart);
            mPart->addChild(mRig);
            mPositions->push_back({ 1, 0, 0 });
            osg::ref_ptr<osg::Geometry> source = new osg::Geometry;
            source->setVertexArray(mPositions);
            mRig->setSourceGeometry(source);
            mRig->setBoneInfo({ { "bone", osg::BoundingSpheref({ 0, 0, 0 }, 2), osg::Matrixf::identity() } });
            mRig->setInfluences({ { { 0, 1.f } } });
            mRig->setCullingActive(false);
            mCull->pushViewport(new osg::Viewport(0, 0, 100, 100));
            mCull->pushProjectionMatrix(new osg::RefMatrix(osg::Matrix::identity()));
            mCull->pushModelViewMatrix(new osg::RefMatrix(osg::Matrix::identity()), osg::Transform::RELATIVE_RF);
        }

        ~RigGeometryTest() override
        {
            mCull->popModelViewMatrix();
            mCull->popProjectionMatrix();
            mCull->popViewport();
        }

        void update(unsigned frame)
        {
            osg::NodeVisitor visitor(osg::NodeVisitor::UPDATE_VISITOR, osg::NodeVisitor::TRAVERSE_ALL_CHILDREN);
            visitor.setTraversalNumber(frame);
            mSkeleton->accept(visitor);
        }

        osg::Geometry* cull(unsigned frame)
        {
            mCull->setTraversalNumber(frame);
            mCull->pushOntoNodePath(mSkeleton);
            mCull->pushOntoNodePath(mPart);
            mRig->accept(*mCull);
            mCull->popFromNodePath();
            mCull->popFromNodePath();
            return mCull->mGeometry;
        }
    };

    TEST_F(RigGeometryTest, FrameZeroSkinsAndUnchangedPoseKeepsArraysAndRevisions)
    {
        update(0);
        auto* first = cull(0);
        ASSERT_NE(first, nullptr);
        const auto revision = first->getVertexArray()->getModifiedCount();
        EXPECT_GT(revision, 0u);
        update(1);
        EXPECT_EQ(cull(1), first);
        EXPECT_EQ(first->getVertexArray()->getModifiedCount(), revision);
        EXPECT_EQ(cull(1), first); // Reflection/shadow view in the same traversal.
    }

    TEST_F(RigGeometryTest, ChangedPoseAlternatesBuffersAfterSkippedTraversals)
    {
        update(1);
        auto* first = cull(1);
        ASSERT_NE(first, nullptr);
        update(2);
        EXPECT_EQ(cull(2), first);
        mBone->setMatrix(osg::Matrix::translate(2, 0, 0));
        update(3);
        auto* changed = cull(3);
        ASSERT_NE(changed, nullptr);
        ASSERT_NE(changed, first);
        EXPECT_EQ((*static_cast<osg::Vec3Array*>(changed->getVertexArray()))[0], osg::Vec3(3, 0, 0));
        EXPECT_EQ((*static_cast<osg::Vec3Array*>(first->getVertexArray()))[0], osg::Vec3(1, 0, 0));
        update(4);
        EXPECT_EQ(cull(4), changed);
        (*mPositions)[0].x() = 4;
        mPositions->dirty();
        update(5);
        EXPECT_EQ(cull(5), first);
        EXPECT_EQ((*static_cast<osg::Vec3Array*>(first->getVertexArray()))[0], osg::Vec3(6, 0, 0));
    }

    TEST_F(RigGeometryTest, AnimatedBoundsRejectOffscreenPartsBeforeSkinning)
    {
        mRig->setCullingActive(true);
        mBone->setMatrix(osg::Matrix::translate(100, 0, 0));
        update(1);
        EXPECT_EQ(cull(1), nullptr);
        EXPECT_EQ(mCull->mVisits, 0u);
        mBone->setMatrix(osg::Matrix::identity());
        update(2);
        EXPECT_NE(cull(2), nullptr);
        EXPECT_EQ(mCull->mVisits, 1u);
    }

    TEST_F(RigGeometryTest, UnusedPaletteBoneDoesNotInvalidateBodyPart)
    {
        osg::ref_ptr<osg::MatrixTransform> unused = new osg::MatrixTransform;
        unused->setName("unused");
        mSkeleton->addChild(unused);
        mRig->setBoneInfo({
            { "bone", osg::BoundingSpheref({ 0, 0, 0 }, 2), osg::Matrixf::identity() },
            { "unused", osg::BoundingSpheref({ 0, 0, 0 }, 2), osg::Matrixf::identity() },
        });
        update(1);
        auto* first = cull(1);
        ASSERT_NE(first, nullptr);
        const auto revision = first->getVertexArray()->getModifiedCount();
        unused->setMatrix(osg::Matrix::translate(10, 0, 0));
        update(2);
        EXPECT_EQ(cull(2), first);
        EXPECT_EQ(first->getVertexArray()->getModifiedCount(), revision);
    }
}
