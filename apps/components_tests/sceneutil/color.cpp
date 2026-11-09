#include <components/sceneutil/color.hpp>

#include <gtest/gtest.h>
#include <osg/GraphicsContext>
#include <osg/ValueObject>

namespace
{
    class MetadataContext : public osg::GraphicsContext
    {
    public:
        int mGLCalls = 0;
        bool valid() const override { return false; }
        bool realizeImplementation() override
        {
            ++mGLCalls;
            return false;
        }
        bool isRealizedImplementation() const override { return false; }
        void closeImplementation() override {}
        bool makeCurrentImplementation() override
        {
            ++mGLCalls;
            return false;
        }
        bool makeContextCurrentImplementation(osg::GraphicsContext*) override
        {
            ++mGLCalls;
            return false;
        }
        bool releaseContextImplementation() override
        {
            ++mGLCalls;
            return false;
        }
        void bindPBufferToTextureImplementation(GLenum) override { ++mGLCalls; }
        void swapBuffersImplementation() override { ++mGLCalls; }
    };

    TEST(NormalTargetFormat, PreserveRgbWhenBothFormatsSupportTheRequestedSamples)
    {
        const SceneUtil::Color::NormalFormatSupport support{ true, true, { 2, 4, 8 } };
        EXPECT_EQ(SceneUtil::Color::selectNormalInternalFormat(support, support, 4), GL_RGB16F);
    }

    TEST(NormalTargetFormat, RgbaFallbackRequiresRenderingBlendingAndMatchingSamples)
    {
        const SceneUtil::Color::NormalFormatSupport rgb{ true, true, { 2 } };
        SceneUtil::Color::NormalFormatSupport rgba{ true, true, { 2, 4 } };
        EXPECT_EQ(SceneUtil::Color::selectNormalInternalFormat(rgb, rgba, 4), GL_RGBA16F);
        rgba.mBlendable = false;
        EXPECT_EQ(SceneUtil::Color::selectNormalInternalFormat(rgb, rgba, 4), GL_RGB16F);
        rgba.mBlendable = true;
        rgba.mRenderable = false;
        EXPECT_EQ(SceneUtil::Color::selectNormalInternalFormat(rgb, rgba, 4), GL_RGB16F);
        rgba.mRenderable = true;
        EXPECT_EQ(SceneUtil::Color::selectNormalInternalFormat(rgb, rgba, 8), GL_RGB16F);
    }

    TEST(NormalTargetFormat, UnknownCapabilitiesKeepLegacyFormatAndSingleSampleNeedsNoMsaaList)
    {
        const SceneUtil::Color::NormalFormatSupport unknown, supported{ true, true, {} };
        EXPECT_EQ(SceneUtil::Color::selectNormalInternalFormat(unknown, unknown, 4), GL_RGB16F);
        EXPECT_EQ(SceneUtil::Color::selectNormalInternalFormat(unknown, supported, 0), GL_RGBA16F);
        EXPECT_EQ(SceneUtil::Color::selectNormalInternalFormat(unknown, supported, 1), GL_RGBA16F);
    }

    TEST(NormalTargetFormat, ConstructorReadsOnlyTheMatchingContextsCachedMetadata)
    {
        osg::ref_ptr<MetadataContext> context = new MetadataContext, other = new MetadataContext;
        EXPECT_EQ(SceneUtil::Color::normalInternalFormat(context, 4), GL_RGB16F);
        context->setUserValue("OpenMW Normals Internal Format", unsigned(GL_RGBA16F));
        context->setUserValue("OpenMW Normals Samples", 4);
        EXPECT_EQ(SceneUtil::Color::normalInternalFormat(context, 4), GL_RGBA16F);
        EXPECT_EQ(SceneUtil::Color::normalInternalFormat(context, 8), GL_RGB16F);
        EXPECT_EQ(SceneUtil::Color::normalInternalFormat(other, 4), GL_RGB16F);
        context->setUserValue("OpenMW Normals Internal Format", unsigned(GL_RGB8));
        EXPECT_EQ(SceneUtil::Color::normalInternalFormat(context, 4), GL_RGB16F);
        EXPECT_EQ(context->mGLCalls, 0);
        EXPECT_EQ(other->mGLCalls, 0);
    }

    TEST(NormalTargetFormat, ProbeCannotUseAnUnrealizedContextOrAcquireItFromAnotherThread)
    {
        osg::ref_ptr<MetadataContext> context = new MetadataContext;
        SceneUtil::Color::SelectNormalFormatOperation operation(4);
        operation(context);
        EXPECT_EQ(SceneUtil::Color::normalInternalFormat(context, 4), GL_RGB16F);
        EXPECT_EQ(context->mGLCalls, 0);
        operation(nullptr);
    }
}
