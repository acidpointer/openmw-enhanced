#include <gtest/gtest.h>

#include <limits>

#include <components/fx/executionplan.hpp>
#include <components/fx/scratchrouting.hpp>
#include <components/fx/shadercompatibility.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/terrain/material.hpp>
#include <components/testing/util.hpp>

#include <apps/openmw/mwrender/postprocessscratch.hpp>

namespace
{
    Fx::DispatchNode node(Fx::FlagsType flags, bool normals = false, bool hdr = false)
    {
        Fx::DispatchNode result;
        result.mFlags = flags;
        result.mNormals = normals;
        result.mHDR = hdr;
        result.mPasses.emplace_back();
        return result;
    }

    TEST(FxRenderPlan, EnvironmentFiltersBothDispatchAndRequirements)
    {
        Fx::DispatchArray nodes;
        nodes.push_back(node(Fx::Technique::Flag_Disable_Interiors, true));
        nodes.push_back(node(Fx::Technique::Flag_Disable_Abovewater, false, true));
        nodes.push_back(node(Fx::Technique::Flag_Hidden));
        const auto interior = Fx::makeExecutionPlan(nodes, Fx::Technique::getEnvironmentMask(false, false), true);
        EXPECT_EQ(interior.mNodes, (std::vector<std::size_t>{ 2 }));
        EXPECT_FALSE(interior.mNormals);
        EXPECT_FALSE(interior.mHDR);
        const auto exterior = Fx::makeExecutionPlan(nodes, Fx::Technique::getEnvironmentMask(false, true), true);
        EXPECT_EQ(exterior.mNodes, (std::vector<std::size_t>{ 0, 2 }));
        EXPECT_TRUE(exterior.mNormals);
        EXPECT_FALSE(exterior.mHDR);
        const auto underwater = Fx::makeExecutionPlan(nodes, Fx::Technique::getEnvironmentMask(true, true), true);
        EXPECT_TRUE(underwater.mHDR);
    }

    TEST(FxRenderPlan, DisabledAndEmptyNodesRequireNoResources)
    {
        Fx::DispatchArray nodes;
        nodes.push_back(node(0, true, true));
        nodes[0].mLights = true;
        const auto disabled = Fx::makeExecutionPlan(nodes, 0, false);
        EXPECT_TRUE(disabled.mNodes.empty());
        EXPECT_FALSE(disabled.mNormals || disabled.mHDR || disabled.mLights);
        nodes[0].mPasses.clear();
        const auto empty = Fx::makeExecutionPlan(nodes, 0, true);
        EXPECT_TRUE(empty.mNodes.empty());
        EXPECT_FALSE(empty.mNormals || empty.mHDR || empty.mLights);
    }

    TEST(FxRenderPlan, CopiedDispatchRetainsRequirementsAndOutputRouting)
    {
        auto original = node(0, true, true);
        original.mLights = true;
        original.mInternalDistortion = true;
        original.mPasses.emplace_back();
        original.mPasses.back().mRenderTarget = new osg::FrameBufferObject;
        original.compile();
        Fx::DispatchNode copied(original);
        EXPECT_TRUE(copied.mNormals && copied.mHDR && copied.mLights && copied.mInternalDistortion);
        EXPECT_TRUE(copied.mPasses[0].mResolve);
        EXPECT_FALSE(copied.mPasses[1].mResolve);
    }

    TEST(FxRenderTargets, ScratchWritesNeverAliasEitherLiveInput)
    {
        // Mixed technique lengths exercise retaining a previous shader output
        // while intermediate passes ping-pong between the other two images.
        Fx::ScratchRouting routing;
        EXPECT_EQ(routing.lastShader(), -1);
        EXPECT_EQ(routing.lastPass(), -1);
        for (const int length : { 1, 5, 2, 7, 1, 3, 4 })
            for (int pass = 0; pass < length; ++pass)
            {
                const int shaderInput = routing.lastShader();
                const int passInput = routing.lastPass();
                const bool final = pass + 1 == length;
                const int output = routing.selectOutput(final);
                EXPECT_GE(output, 0);
                EXPECT_LT(output, 3);
                EXPECT_NE(output, shaderInput);
                EXPECT_NE(output, passInput);
                EXPECT_EQ(routing.lastPass(), output);
                EXPECT_EQ(routing.lastShader(), final ? output : shaderInput);
            }
    }

    TEST(FxRenderTargets, RatioRoundingAndTinySurfacesStayValid)
    {
        Fx::Types::SizeProxy size;
        size.mWidthRatio = 0.55f;
        size.mHeightRatio = 0.5f;
        EXPECT_EQ(size.get(2880, 1800), std::make_tuple(1584, 900));
        EXPECT_EQ(size.get(3, 3), std::make_tuple(1, 1));
        EXPECT_EQ(size.get(0, 0), std::make_tuple(1, 1));
        size.mWidthRatio = std::numeric_limits<float>::infinity();
        EXPECT_THROW(size.get(100, 100), std::runtime_error);
        size.mWidthRatio = -1.f;
        EXPECT_THROW(size.get(100, 100), std::runtime_error);
        size.mWidthRatio = 1e30f;
        EXPECT_THROW(size.get(100, 100), std::runtime_error);
        size.mWidthRatio.reset();
        size.mWidth = 0;
        EXPECT_THROW(size.get(100, 100), std::runtime_error);
    }

    osg::ref_ptr<osg::Texture2D> sceneTexture(int width = 640)
    {
        osg::ref_ptr<osg::Texture2D> result = new osg::Texture2D;
        result->setTextureSize(width, 480);
        result->setInternalFormat(GL_RGBA);
        result->setSourceFormat(GL_RGBA);
        result->setSourceType(GL_UNSIGNED_BYTE);
        return result;
    }

    TEST(FxRenderTargets, AlternatingInputsShareScratchButContextsDoNot)
    {
        osg::ref_ptr<MWRender::PostprocessScratch> pool = new MWRender::PostprocessScratch;
        osg::ref_ptr<osg::State> state = new osg::State;
        state->setContextID(0);
        auto inputA = sceneTexture();
        auto inputB = sceneTexture();
        osg::ref_ptr<osg::FrameBufferObject> scratch = new osg::FrameBufferObject;
        pool->get(*state, *inputA)[1] = scratch;
        EXPECT_EQ(pool->get(*state, *inputB)[1], scratch);
        state->setContextID(1);
        EXPECT_FALSE(pool->get(*state, *inputB)[1]);
        pool->get(*state, *inputB)[1] = new osg::FrameBufferObject;
        state->setContextID(0);
        EXPECT_EQ(pool->get(*state, *inputA)[1], scratch);
        inputA->setTextureSize(800, 480);
        EXPECT_FALSE(pool->get(*state, *inputA)[1]);
        state->setContextID(1);
        EXPECT_TRUE(pool->get(*state, *inputB)[1]);
    }

    TEST(FxCompatibility, EditedOrUnrecognizedSourcesAreNeverPatchedByName)
    {
        const std::string source = "fragment rays {} technique { passes = rays; version = \"1.2\"; }";
        EXPECT_FALSE(Fx::optimizeLegacyShader("godrays", source, true));
        EXPECT_FALSE(Fx::optimizeLegacyShader("depth_of_field", source, false));
        EXPECT_FALSE(Fx::optimizeLegacyShader("user_custom_effect", source, true));
        EXPECT_FALSE(Fx::optimizeLegacyShader("godrays", "\xef\xbb\xbf" + source + "\r\n", true));
    }

    TEST(FxRenderTargets, CompactR8MaskCompilesThroughActualFxParser)
    {
        TestingOpenMW::VFSTestFile file(R"(
            render_target mask {
                width_ratio = 0.55;
                height_ratio = 0.55;
                internal_format = r8;
                source_format = red;
                source_type = unsigned_byte;
                mipmaps = false;
            }
            fragment maskpass(target=mask) { void main() { omw_FragColor = vec4(1.0); } }
            fragment output(rt1=mask) { void main() { omw_FragColor = vec4(1.0); } }
            technique { passes = maskpass, output; }
        )");
        const VFS::Path::NormalizedView path("shaders/test.omwfx");
        auto vfs = TestingOpenMW::createTestVFS({ { path, &file } });
        Resource::ImageManager images(vfs.get(), 0);
        Fx::Technique technique(*vfs, images, path, "test", 2880, 1800, true, true);
        ASSERT_TRUE(technique.compile()) << technique.getLastError();
        ASSERT_EQ(technique.getRenderTargetsMap().size(), 1);
        EXPECT_EQ(technique.getRenderTargetsMap().at("mask").mTarget->getInternalFormat(), GL_R8);
        EXPECT_EQ(technique.getRenderTargetsMap().at("mask").mSize.get(2880, 1800), std::make_tuple(1584, 990));
    }

    TEST(TerrainLayerBatch, SpecializedLayersRetainIndividualPasses)
    {
        Terrain::TextureLayer first;
        Terrain::TextureLayer second;
        EXPECT_FALSE(Terrain::canBatchLayers(first, second));
        first.mDiffuseMap = sceneTexture();
        second.mDiffuseMap = sceneTexture();
        EXPECT_TRUE(Terrain::canBatchLayers(first, second));
        second.mNormalMap = sceneTexture();
        EXPECT_FALSE(Terrain::canBatchLayers(first, second));
        second.mNormalMap = nullptr;
        second.mSpecular = true;
        EXPECT_FALSE(Terrain::canBatchLayers(first, second));
        second.mSpecular = false;
        first.mParallax = true;
        EXPECT_FALSE(Terrain::canBatchLayers(first, second));
    }
}
