#include <components/sceneutil/paginganalysis.hpp>
#include <gtest/gtest.h>

namespace
{
    osg::ref_ptr<osg::Geometry> geometry(unsigned int vertices = 3)
    {
        osg::ref_ptr<osg::Geometry> result = new osg::Geometry;
        result->setVertexArray(new osg::Vec3Array(vertices));
        return result;
    }
    TEST(PagingAnalysis, DirectGeometryUnderSelectorsIsDispatchedAndMasked)
    {
        osg::ref_ptr<osg::Group> root = new osg::Group;
        osg::ref_ptr<osg::Switch> sw = new osg::Switch;
        sw->addChild(geometry(3), true);
        sw->addChild(geometry(100), false);
        osg::ref_ptr<osg::LOD> lod = new osg::LOD;
        lod->addChild(geometry(4), 0, 10);
        lod->addChild(geometry(200), 20, 30);
        osg::ref_ptr<osg::Sequence> sequence = new osg::Sequence;
        sequence->addChild(geometry(5));
        auto masked = geometry(300);
        masked->setNodeMask(1);
        sw->addChild(masked, true);
        root->addChild(sw);
        root->addChild(lod);
        root->addChild(sequence);
        root->addChild(new osg::Sequence);
        SceneUtil::PagingAnalysis analysis(~1u);
        analysis.mDistances = { 0, 10 };
        root->accept(analysis);
        const auto result = analysis.retrieveResult();
        EXPECT_EQ(result.mNumVerts, 12);
        ASSERT_EQ(result.mStateSetCounter.size(), 1);
        EXPECT_EQ(result.mStateSetCounter.begin()->second, 3);
    }
    TEST(PagingAnalysis, SiblingInheritanceAndGeometryLocalMaterialsRemainDistinct)
    {
        osg::ref_ptr<osg::Group> root = new osg::Group;
        osg::ref_ptr<osg::Group> branch = new osg::Group;
        auto first = geometry(), second = geometry(), third = geometry();
        branch->getOrCreateStateSet();
        branch->addChild(first);
        root->addChild(branch);
        root->addChild(second);
        third->getOrCreateStateSet();
        root->addChild(third);
        SceneUtil::PagingAnalysis analysis(~0u);
        root->accept(analysis);
        const auto result = analysis.retrieveResult();
        EXPECT_EQ(result.mNumVerts, 9);
        EXPECT_EQ(result.mStateSetCounter.size(), 3);
        EXPECT_EQ(analysis.getMergeBenefit(result), 1);
        analysis.addInstance(result);
        EXPECT_EQ(analysis.getMergeBenefit(result), 2);
    }
}
