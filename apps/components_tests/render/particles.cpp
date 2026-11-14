#include <components/renderassets/particles.hpp>

#include <gtest/gtest.h>

namespace
{
    struct ParticleOutput
    {
        std::vector<Render::Vertex> mVertices;
        std::vector<std::uint32_t> mIndices;
        std::vector<int> mOrder;
        std::uint32_t import(
            const osgParticle::ParticleSystem& source, const osg::Matrixd& view = osg::Matrixd::identity())
        {
            return RenderAssets::importParticles(source, view, mVertices, mIndices, mOrder);
        }
    };

    osgParticle::Particle* particle(osgParticle::ParticleSystem& source)
    {
        auto* result = source.createParticle(nullptr);
        result->setLifeTime(0);
        result->setSizeRange(osgParticle::rangef(2, 2));
        result->setAlphaRange(osgParticle::rangef(.5f, .5f));
        result->setColorRange(osgParticle::rangev4(osg::Vec4(1, .5f, 0, .8f), osg::Vec4(1, .5f, 0, .8f)));
        result->setPosition(osg::Vec3(3, 4, -10));
        result->update(0, false);
        return result;
    }

    TEST(RenderParticles, PreservesBillboardCenterAtlasAndIndependentAlpha)
    {
        osg::ref_ptr<osgParticle::ParticleSystem> source = new osgParticle::ParticleSystem;
        auto* sprite = particle(*source);
        sprite->setTextureTileRange(2, 2, 0, 3);
        sprite->update(0, false);
        ParticleOutput result;
        ASSERT_EQ(result.import(*source, osg::Matrixd::translate(10, 0, 0)), 6u);
        const auto& a = result.mVertices[0];
        const auto& c = result.mVertices[2];
        EXPECT_FLOAT_EQ((a.mPosition[0] + c.mPosition[0]) * .5f, 13);
        EXPECT_FLOAT_EQ((a.mPosition[1] + c.mPosition[1]) * .5f, 4);
        EXPECT_FLOAT_EQ(a.mPosition[2], -10);
        EXPECT_FLOAT_EQ(c.mPosition[0] - a.mPosition[0], 4);
        EXPECT_FLOAT_EQ(a.mAlpha, .4f);
        EXPECT_FLOAT_EQ(c.mUV[0] - a.mUV[0], .5f);
        EXPECT_FLOAT_EQ(c.mUV[1] - a.mUV[1], .5f);
        EXPECT_EQ(a.mColor[1], 128);
    }

    TEST(RenderParticles, ReusesTopologyAndOmitsDeadParticles)
    {
        osg::ref_ptr<osgParticle::ParticleSystem> source = new osgParticle::ParticleSystem;
        particle(*source);
        ParticleOutput result;
        ASSERT_EQ(result.import(*source), 6u);
        const auto* indices = result.mIndices.data();
        particle(*source);
        ASSERT_EQ(result.import(*source), 12u);
        EXPECT_EQ(result.mIndices.data(), indices);
        source->destroyParticle(0);
        source->getParticle(0)->update(0, false);
        EXPECT_EQ(result.import(*source), 6u);
        EXPECT_EQ(result.mIndices.data(), indices);
    }

    TEST(RenderParticles, SortsTranslucentSpritesInEachViewsDepthOrder)
    {
        osg::ref_ptr<osgParticle::ParticleSystem> source = new osgParticle::ParticleSystem;
        source->setSortMode(osgParticle::ParticleSystem::SORT_BACK_TO_FRONT);
        particle(*source)->setPosition(osg::Vec3(0, 0, -10));
        particle(*source)->setPosition(osg::Vec3(0, 0, -20));
        ParticleOutput result;
        ASSERT_EQ(result.import(*source), 12u);
        EXPECT_FLOAT_EQ(result.mVertices[0].mPosition[2], -20);
        EXPECT_FLOAT_EQ(result.mVertices[4].mPosition[2], -10);
    }

    TEST(RenderParticles, HonorsConfiguredAlignmentAndParticleScaleFrame)
    {
        osg::ref_ptr<osgParticle::ParticleSystem> source = new osgParticle::ParticleSystem;
        particle(*source);
        source->setAlignVectors(osg::Vec3(0, 1, 0), osg::Vec3(-1, 0, 0));
        ParticleOutput result;
        const auto scaled = osg::Matrixd::scale(2, 2, 2);
        ASSERT_EQ(result.import(*source, scaled), 6u);
        EXPECT_FLOAT_EQ(result.mVertices[1].mPosition[1] - result.mVertices[0].mPosition[1], 4);
        EXPECT_FLOAT_EQ(result.mVertices[1].mPosition[0] - result.mVertices[0].mPosition[0], 0);
        source->setParticleScaleReferenceFrame(osgParticle::ParticleSystem::LOCAL_COORDINATES);
        ASSERT_EQ(result.import(*source, scaled), 6u);
        EXPECT_FLOAT_EQ(result.mVertices[1].mPosition[1] - result.mVertices[0].mPosition[1], 8);
    }

    TEST(RenderParticles, AppliesVisibilityDistanceInTheCurrentCamera)
    {
        osg::ref_ptr<osgParticle::ParticleSystem> source = new osgParticle::ParticleSystem;
        source->setSortMode(osgParticle::ParticleSystem::SORT_BACK_TO_FRONT);
        source->setVisibilityDistance(15);
        particle(*source)->setPosition(osg::Vec3(0, 0, -10));
        particle(*source)->setPosition(osg::Vec3(0, 0, -20));
        ParticleOutput result;
        ASSERT_EQ(result.import(*source), 6u);
        EXPECT_FLOAT_EQ(result.mVertices[0].mPosition[2], -10);
        ASSERT_EQ(result.import(*source, osg::Matrixd::translate(0, 0, 10)), 12u);
    }
}
