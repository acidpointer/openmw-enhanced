#include "color.hpp"

#include <algorithm>
#include <array>

#include <osg/GLExtensions>
#include <osg/GraphicsContext>
#include <osg/ValueObject>

#ifndef GL_FRAMEBUFFER_RENDERABLE
#define GL_FRAMEBUFFER_RENDERABLE 0x8289
#endif
#ifndef GL_FRAMEBUFFER_BLEND
#define GL_FRAMEBUFFER_BLEND 0x828B
#endif
#ifndef GL_NUM_SAMPLE_COUNTS
#define GL_NUM_SAMPLE_COUNTS 0x9380
#endif

namespace SceneUtil
{

    bool isColorFormat(GLenum format)
    {
        static constexpr std::array<GLenum, 42> formats = {
            GL_RGB,
            GL_RGB4,
            GL_RGB5,
            GL_RGB8,
            GL_RGB8_SNORM,
            GL_RGB10,
            GL_RGB12,
            GL_RGB16,
            GL_RGB16_SNORM,
            GL_SRGB,
            GL_SRGB8,
            GL_RGB16F,
            GL_RGB32F,
            GL_R11F_G11F_B10F,
            GL_RGB9_E5,
            GL_RGB8I,
            GL_RGB8UI,
            GL_RGB16I,
            GL_RGB16UI,
            GL_RGB32I,
            GL_RGB32UI,
            GL_RGBA,
            GL_RGBA2,
            GL_RGBA4,
            GL_RGB5_A1,
            GL_RGBA8,
            GL_RGBA8_SNORM,
            GL_RGB10_A2,
            GL_RGB10_A2UI,
            GL_RGBA12,
            GL_RGBA16,
            GL_RGBA16_SNORM,
            GL_SRGB_ALPHA8,
            GL_SRGB8_ALPHA8,
            GL_RGBA16F,
            GL_RGBA32F,
            GL_RGBA8I,
            GL_RGBA8UI,
            GL_RGBA16I,
            GL_RGBA16UI,
            GL_RGBA32I,
            GL_RGBA32UI,
        };

        return std::find(formats.cbegin(), formats.cend(), format) != formats.cend();
    }

    bool isFloatingPointColorFormat(GLenum format)
    {
        static constexpr std::array<GLenum, 5> formats = {
            GL_RGB16F,
            GL_RGB32F,
            GL_R11F_G11F_B10F,
            GL_RGBA16F,
            GL_RGBA32F,
        };

        return std::find(formats.cbegin(), formats.cend(), format) != formats.cend();
    }

    int getColorFormatChannelCount(GLenum format)
    {
        static constexpr std::array<GLenum, 21> formats = {
            GL_RGBA,
            GL_RGBA2,
            GL_RGBA4,
            GL_RGB5_A1,
            GL_RGBA8,
            GL_RGBA8_SNORM,
            GL_RGB10_A2,
            GL_RGB10_A2UI,
            GL_RGBA12,
            GL_RGBA16,
            GL_RGBA16_SNORM,
            GL_SRGB_ALPHA8,
            GL_SRGB8_ALPHA8,
            GL_RGBA16F,
            GL_RGBA32F,
            GL_RGBA8I,
            GL_RGBA8UI,
            GL_RGBA16I,
            GL_RGBA16UI,
            GL_RGBA32I,
            GL_RGBA32UI,
        };
        if (std::find(formats.cbegin(), formats.cend(), format) != formats.cend())
            return 4;
        return 3;
    }

    void getColorFormatSourceFormatAndType(GLenum internalFormat, GLenum& sourceFormat, GLenum& sourceType)
    {
        if (getColorFormatChannelCount(internalFormat == 4))
            sourceFormat = GL_RGBA;
        else
            sourceFormat = GL_RGB;

        if (isFloatingPointColorFormat(internalFormat))
            sourceType = GL_FLOAT;
        else
            sourceType = GL_UNSIGNED_BYTE;
    }

    namespace Color
    {
        namespace
        {
            constexpr auto NormalFormatKey = "OpenMW Normals Internal Format";
            constexpr auto NormalSamplesKey = "OpenMW Normals Samples";
        }

        bool NormalFormatSupport::supports(int samples) const
        {
            return mRenderable && mBlendable
                && (samples <= 1 || std::find(mSamples.begin(), mSamples.end(), samples) != mSamples.end());
        }

        GLenum selectNormalInternalFormat(const NormalFormatSupport& rgb, const NormalFormatSupport& rgba, int samples)
        {
            return !rgb.supports(samples) && rgba.supports(samples) ? GL_RGBA16F : GL_RGB16F;
        }

        GLenum normalInternalFormat(const osg::GraphicsContext* context, int samples)
        {
            unsigned int format = GL_RGB16F;
            int cachedSamples = 0;
            if (!context || !context->getUserValue(NormalSamplesKey, cachedSamples) || cachedSamples != samples
                || !context->getUserValue(NormalFormatKey, format) || (format != GL_RGB16F && format != GL_RGBA16F))
                return GL_RGB16F;
            return format;
        }

        void SelectNormalFormatOperation::operator()(osg::GraphicsContext* context)
        {
            if (!context)
                return;
            context->setUserValue(NormalFormatKey, unsigned(GL_RGB16F));
            context->setUserValue(NormalSamplesKey, mSamples);
            if (!context->valid() || !context->isCurrent() || !context->getState())
                return;
            // Viewer::realize makes this context current before running its
            // realization operations. PostProcessor construction does not.
            const auto* extensions = context->getState()->get<osg::GLExtensions>();
            using Query = void(GL_APIENTRY*)(GLenum, GLenum, GLenum, GLsizei, GLint*);
            Query query = nullptr;
            const unsigned int id = context->getState()->getContextID();
            if ((extensions->glDisablei || extensions->glDisableIndexedEXT)
                && (extensions->glslLanguageVersion >= 4.3f
                    || osg::isGLExtensionSupported(id, "GL_ARB_internalformat_query2")))
                osg::setGLExtensionFuncPtr(query, "glGetInternalformativ");

            unsigned int format = GL_RGB16F;
            if (query)
            {
                const auto support = [&](GLenum internalFormat) {
                    NormalFormatSupport result;
                    GLint renderable = GL_NONE, blendable = GL_NONE;
                    query(GL_TEXTURE_2D, internalFormat, GL_FRAMEBUFFER_RENDERABLE, 1, &renderable);
                    query(GL_TEXTURE_2D, internalFormat, GL_FRAMEBUFFER_BLEND, 1, &blendable);
                    result.mRenderable = renderable != GL_NONE;
                    result.mBlendable = blendable != GL_NONE;
                    if (mSamples > 1 && result.mRenderable && result.mBlendable)
                    {
                        GLint count = 0;
                        query(GL_RENDERBUFFER, internalFormat, GL_NUM_SAMPLE_COUNTS, 1, &count);
                        std::array<GLint, 64> samples{};
                        if (count > 0 && count <= static_cast<GLint>(samples.size()))
                        {
                            query(GL_RENDERBUFFER, internalFormat, GL_SAMPLES, count, samples.data());
                            result.mSamples.assign(samples.begin(), samples.begin() + count);
                        }
                    }
                    return result;
                };
                const auto rgb = support(GL_RGB16F);
                const auto rgba = support(GL_RGBA16F);
                format = selectNormalInternalFormat(rgb, rgba, mSamples);
            }
            context->setUserValue(NormalFormatKey, format);
        }

        GLenum sColorInternalFormat;
        GLenum sColorSourceFormat;
        GLenum sColorSourceType;

        GLenum colorInternalFormat()
        {
            return sColorInternalFormat;
        }

        GLenum colorSourceFormat()
        {
            return sColorSourceFormat;
        }

        GLenum colorSourceType()
        {
            return sColorSourceType;
        }

        void SelectColorFormatOperation::operator()([[maybe_unused]] osg::GraphicsContext* graphicsContext)
        {
            sColorInternalFormat = GL_RGB;

            for (auto supportedFormat : mSupportedFormats)
            {
                if (isColorFormat(supportedFormat))
                {
                    sColorInternalFormat = supportedFormat;
                    break;
                }
            }

            getColorFormatSourceFormatAndType(sColorInternalFormat, sColorSourceFormat, sColorSourceType);
        }
    }
}
