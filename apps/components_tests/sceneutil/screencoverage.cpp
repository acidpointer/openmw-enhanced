#include <gtest/gtest.h>

#include <components/sceneutil/screencoverage.hpp>

namespace
{
    TEST(ScreenCoverage, ProjectsBoundsInNormalizedScreenCoordinates)
    {
        const auto result = SceneUtil::screenCoverage(
            osg::BoundingBox(-0.5, -0.25, -1, 0.5, 0.25, 1), osg::Matrix::identity(), osg::Matrix::identity());
        ASSERT_TRUE(result);
        EXPECT_EQ(*result, osg::Vec4f(0.25, 0.375, 0.75, 0.625));
    }

    TEST(ScreenCoverage, InvalidAndEyePlaneCrossingBoundsRemainUnknown)
    {
        EXPECT_FALSE(SceneUtil::screenCoverage(osg::BoundingBox(), osg::Matrix::identity(), osg::Matrix::identity()));
        EXPECT_FALSE(SceneUtil::screenCoverage(
            osg::BoundingBox(-1, -1, -2, 1, 1, 2), osg::Matrix::identity(), osg::Matrix::perspective(60, 1, 0.1, 100)));
    }

    TEST(ScreenCoverage, BoundsOutsideScreenClampConservatively)
    {
        const auto result = SceneUtil::screenCoverage(
            osg::BoundingBox(-4, -4, -1, 0, 0, 1), osg::Matrix::identity(), osg::Matrix::identity());
        ASSERT_TRUE(result);
        EXPECT_EQ(*result, osg::Vec4f(0, 0, 0.5, 0.5));
    }
}
