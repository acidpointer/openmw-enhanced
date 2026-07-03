#include "transparentpass.hpp"

#include <osg/AlphaFunc>
#include <osg/BlendFunc>
#include <osg/Material>
#include <osg/Texture2D>
#include <osg/Texture2DArray>

#include <string>

#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>

#include <components/sceneutil/depth.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/stereo/multiview.hpp>
#include <components/stereo/stereomanager.hpp>

#include "enhancedperf.hpp"
#include "vismask.hpp"

namespace MWRender
{
    namespace
    {
        const char* transparentDepthModeName(Enhanced::TransparentDepthMode mode)
        {
            switch (mode)
            {
                case Enhanced::TransparentDepthMode::Legacy:
                    return "legacy";
                case Enhanced::TransparentDepthMode::AlphaTestOnly:
                    return "alpha-test-only";
                case Enhanced::TransparentDepthMode::Off:
                    return "off";
                case Enhanced::TransparentDepthMode::ProfileOnly:
                    return "profile-only";
            }
            return "legacy";
        }

        bool hasAlphaTestState(osgUtil::StateGraph* stateGraph)
        {
            for (osgUtil::StateGraph* current = stateGraph; current; current = current->_parent)
            {
                const osg::StateSet* stateSet = current->getStateSet();
                if (!stateSet)
                    continue;

                const auto* alphaFunc
                    = static_cast<const osg::AlphaFunc*>(stateSet->getAttribute(osg::StateAttribute::ALPHAFUNC));
                if (alphaFunc && alphaFunc->getFunction() != osg::AlphaFunc::ALWAYS)
                    return true;
            }

            return false;
        }

        bool materialAllowsDepthPostPass(const osg::StateSet* stateSet)
        {
            if (!stateSet || !stateSet->getAttribute(osg::StateAttribute::MATERIAL))
                return true;

            const auto* mat = static_cast<const osg::Material*>(stateSet->getAttribute(osg::StateAttribute::MATERIAL));
            return mat->getDiffuse(osg::Material::FRONT).a() >= 0.5f;
        }

        bool shouldReplayTransparentLeaf(osgUtil::RenderLeaf* leaf, Enhanced::TransparentDepthMode mode)
        {
            if (!leaf || !leaf->_drawable || !leaf->_parent)
                return false;

            if (leaf->_drawable->getNodeMask() == Mask_ParticleSystem)
                return false;

            const osg::StateSet* stateSet = leaf->_parent->getStateSet();
            if (!materialAllowsDepthPostPass(stateSet))
                return false;

            if (mode == Enhanced::TransparentDepthMode::AlphaTestOnly)
                return hasAlphaTestState(leaf->_parent);

            return mode == Enhanced::TransparentDepthMode::Legacy || mode == Enhanced::TransparentDepthMode::ProfileOnly;
        }

        struct TransparentDepthStats
        {
            unsigned int mLeaves = 0;
            unsigned int mReplay = 0;
        };

        TransparentDepthStats collectTransparentDepthStats(
            osgUtil::RenderBin* bin, Enhanced::TransparentDepthMode mode)
        {
            TransparentDepthStats stats;
            for (osgUtil::RenderLeaf* leaf : bin->getRenderLeafList())
            {
                ++stats.mLeaves;
                if (shouldReplayTransparentLeaf(leaf, mode))
                    ++stats.mReplay;
            }
            return stats;
        }
    }

    TransparentDepthBinCallback::TransparentDepthBinCallback(Shader::ShaderManager& shaderManager, bool postPass)
        : mStateSet(new osg::StateSet)
        , mPostPass(postPass)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        image->setColor(osg::Vec4(1, 1, 1, 1), 0, 0);

        osg::ref_ptr<osg::Texture2D> dummyTexture = new osg::Texture2D(image);
        dummyTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        dummyTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);

        constexpr osg::StateAttribute::OverrideValue modeOff = osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE;
        constexpr osg::StateAttribute::OverrideValue modeOn = osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE;

        mStateSet->setTextureAttributeAndModes(0, dummyTexture);

        Shader::ShaderManager::DefineMap defines;
        Stereo::shaderStereoDefines(defines);

        mStateSet->setAttributeAndModes(new osg::BlendFunc, modeOff);
        mStateSet->setAttributeAndModes(shaderManager.getProgram("depthclipped", defines), modeOn);
        mStateSet->setAttributeAndModes(new SceneUtil::AutoDepth, modeOn);

        for (unsigned int unit = 1; unit < 8; ++unit)
            mStateSet->setTextureMode(unit, GL_TEXTURE_2D, modeOff);
    }

    void TransparentDepthBinCallback::drawImplementation(
        osgUtil::RenderBin* bin, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous)
    {
        osg::State& state = *renderInfo.getState();
        osg::GLExtensions* ext = state.get<osg::GLExtensions>();

        bool validFbo = false;
        unsigned int frameId = state.getFrameStamp()->getFrameNumber() % 2;

        const auto& fbo = mFbo[frameId];
        const auto& msaaFbo = mMsaaFbo[frameId];
        const auto& opaqueFbo = mOpaqueFbo[frameId];

        if (bin->getStage()->getMultisampleResolveFramebufferObject()
            && bin->getStage()->getMultisampleResolveFramebufferObject() == fbo)
            validFbo = true;
        else if (bin->getStage()->getFrameBufferObject()
            && (bin->getStage()->getFrameBufferObject() == fbo || bin->getStage()->getFrameBufferObject() == msaaFbo))
            validFbo = true;

        if (!validFbo)
        {
            {
                Enhanced::GpuScope mainScope(state, "transparent:main-untracked");
                bin->drawImplementation(renderInfo, previous);
            }
            Enhanced::flushGpuProfile(state);
            return;
        }

        const Enhanced::TransparentDepthMode mode = Enhanced::transparentDepthMode();

        const osg::Texture* tex
            = opaqueFbo->getAttachment(osg::FrameBufferObject::BufferComponent::PACKED_DEPTH_STENCIL_BUFFER)
                  .getTexture();

        {
            Enhanced::GpuScope blitScope(state, "transparent:depth-blit");
            if (Stereo::getMultiview())
            {
                if (!mMultiviewResolve[frameId])
                {
                    mMultiviewResolve[frameId] = std::make_unique<Stereo::MultiviewFramebufferResolve>(
                        msaaFbo ? msaaFbo : fbo, opaqueFbo, GL_DEPTH_BUFFER_BIT);
                }
                else
                {
                    mMultiviewResolve[frameId]->setResolveFbo(opaqueFbo);
                    mMultiviewResolve[frameId]->setMsaaFbo(msaaFbo ? msaaFbo : fbo);
                }
                mMultiviewResolve[frameId]->resolveImplementation(state);
            }
            else
            {
                opaqueFbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
                ext->glBlitFramebuffer(0, 0, tex->getTextureWidth(), tex->getTextureHeight(), 0, 0,
                    tex->getTextureWidth(), tex->getTextureHeight(), GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            }
        }

        msaaFbo ? msaaFbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER)
                : fbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);

        // draws scene into primary attachments
        {
            Enhanced::GpuScope mainScope(state, "transparent:main");
            bin->drawImplementation(renderInfo, previous);
        }

        if (!mPostPass || mode == Enhanced::TransparentDepthMode::Off)
        {
            Enhanced::flushGpuProfile(state);
            return;
        }

        const TransparentDepthStats stats = collectTransparentDepthStats(bin, mode);
        if (mode == Enhanced::TransparentDepthMode::ProfileOnly || stats.mReplay == 0)
        {
            Enhanced::flushGpuProfile(state);
            return;
        }

        opaqueFbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);

        // draw transparent post-pass to populate a postprocess friendly depth texture with alpha-clipped geometry

        unsigned int numToPop = previous ? osgUtil::StateGraph::numToPop(previous->_parent) : 0;
        if (numToPop > 1)
            numToPop--;
        unsigned int insertStateSetPosition = state.getStateSetStackSize() - numToPop;

        state.insertStateSet(insertStateSetPosition, mStateSet);
        {
            Enhanced::GpuScope postScope(state, "transparent:depth-postpass",
                std::string("mode=") + transparentDepthModeName(mode) + ";leaves=" + std::to_string(stats.mLeaves)
                    + ";replay=" + std::to_string(stats.mReplay));
            for (auto rit = bin->getRenderLeafList().begin(); rit != bin->getRenderLeafList().end(); rit++)
            {
                osgUtil::RenderLeaf* rl = *rit;

                if (!shouldReplayTransparentLeaf(rl, mode))
                    continue;

                rl->render(renderInfo, previous);
                previous = rl;
            }
        }
        state.removeStateSet(insertStateSetPosition);

        msaaFbo ? msaaFbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER)
                : fbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
        state.checkGLErrors("after TransparentDepthBinCallback::drawImplementation");
        Enhanced::flushGpuProfile(state);
    }
}
