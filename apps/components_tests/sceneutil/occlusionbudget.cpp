#include <components/sceneutil/occlusionculling.hpp>

#include <gtest/gtest.h>

namespace
{
    TEST(OcclusionBudgetTest, candidatePreparationConsumesTheStaticBudget)
    {
        SceneUtil::OcclusionCuller culler(8, 4);
        culler.setStaticRasterBudgetMs(0.5);
        EXPECT_TRUE(culler.staticRasterBudgetAvailable());
        culler.recordStaticCandidate(0.25);
        EXPECT_TRUE(culler.staticRasterBudgetAvailable());
        EXPECT_FALSE(culler.staticCandidateBudgetAvailable());
        culler.recordStaticCandidate(0.25);
        EXPECT_FALSE(culler.staticRasterBudgetAvailable());
        culler.beginFrame(osg::Matrixd::identity(), osg::Matrixd::identity());
        EXPECT_TRUE(culler.staticRasterBudgetAvailable());
    }

    TEST(OcclusionBudgetTest, zeroRetainsUnlimitedBudgetBehavior)
    {
        SceneUtil::OcclusionCuller culler(8, 4);
        culler.setStaticRasterBudgetMs(0);
        culler.recordStaticCandidate(100);
        EXPECT_TRUE(culler.staticRasterBudgetAvailable());
    }
}
