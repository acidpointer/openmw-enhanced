#include "transparentpass.hpp"

#include "water.hpp"

#include <algorithm>

#include <osg/AlphaFunc>
#include <osg/BlendFunc>
#include <osg/ColorMask>
#include <osg/Stats>
#include <osg/Texture2D>
#include <osg/Texture2DArray>

#include <string>

#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>

#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/renderstageinventory.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/stereo/multiview.hpp>
#include <components/stereo/stereomanager.hpp>

#include "renderbin.hpp"
#include "vismask.hpp"

namespace MWRender
{
    namespace
    {
        bool materialAllowsDepthPostPass(const osg::StateSet* stateSet)
        {
            if (!stateSet || !stateSet->getAttribute(osg::StateAttribute::MATERIAL))
                return true;

            const auto* mat
                = static_cast<const SceneUtil::Material*>(stateSet->getAttribute(osg::StateAttribute::MATERIAL));
            return mat->getDiffuse().a() >= 0.5f;
        }

        bool shouldReplayTransparentLeaf(osgUtil::RenderLeaf* leaf)
        {
            if (!leaf || !leaf->_drawable || !leaf->_parent)
                return false;

            if (leaf->_drawable->getNodeMask() == Mask_ParticleSystem)
                return false;

            const osg::StateSet* stateSet = leaf->_parent->getStateSet();
            if (!materialAllowsDepthPostPass(stateSet))
                return false;

            return true;
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

        mStateSet->setTextureAttribute(0, dummyTexture);

        Shader::ShaderManager::DefineMap defines;
        Stereo::shaderStereoDefines(defines);

        mStateSet->setAttributeAndModes(new osg::BlendFunc, modeOff);
        // Omitting a fragment color output does not disable color writes. This
        // FBO also contains the opaque scene color used for water refraction.
        mStateSet->setAttributeAndModes(new osg::ColorMask(false, false, false, false), modeOn);
        mStateSet->setAttributeAndModes(shaderManager.getProgram("depthclipped", defines), modeOn);
        mStateSet->setAttributeAndModes(new SceneUtil::AutoDepth, modeOn);
    }

    void TransparentDepthBinCallback::drawImplementation(
        osgUtil::RenderBin* bin, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous)
    {
        osg::State& state = *renderInfo.getState();

        bool validFbo = false;
        unsigned int frameId = state.getFrameStamp()->getFrameNumber() % 2;
        auto* stats = bin->getStage()->getCamera() ? bin->getStage()->getCamera()->getStats() : nullptr;
        const bool inventory = stats && stats->collectStats("rendering");
        if (inventory)
            stats->setAttribute(state.getFrameStamp()->getFrameNumber(), "Transparent depth replay leaves", 0);

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
            bin->drawImplementation(renderInfo, previous);
            return;
        }

        msaaFbo ? msaaFbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER)
                : fbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);

        // draws scene into primary attachments
        bin->drawImplementation(renderInfo, previous);

        if (!mPostPass)
            return;

        // Water already consumed opaque depth during the color pass above.
        // Keep replay for effects and any later scene geometry (hands, distortion
        // or custom bins), which may sample it. Never gate just on the PP switch.
        if (!mEffectConsumer[frameId] && bin->getParent() == bin->getStage()
            && bin->getBinNum() == RenderBin_DepthSorted
            && !SceneUtil::hasLaterSubmissions(
                *bin->getStage(), RenderBin_DepthSorted, { RenderBin_OcclusionQuery, RenderBin_SunGlare })
            && !SceneUtil::hasUnknownCameraConsumers(*bin->getStage(), mColorOnlyCompositor.get()))
            return;

        const osg::Drawable* waterDrawable = mWater ? mWater->getDrawable() : nullptr;
        const auto shouldReplay = [waterDrawable](osgUtil::RenderLeaf* leaf) {
            return shouldReplayTransparentLeaf(leaf) && leaf->_drawable.get() != waterDrawable;
        };
        const auto& leaves = bin->getRenderLeafList();
        const auto first = std::find_if(leaves.begin(), leaves.end(), shouldReplay);
        if (first == leaves.end())
            return;

        opaqueFbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);

        // draw transparent post-pass to populate a postprocess friendly depth texture with alpha-clipped geometry

        unsigned int numToPop = previous ? osgUtil::StateGraph::numToPop(previous->_parent) : 0;
        if (numToPop > 1)
            numToPop--;
        unsigned int insertStateSetPosition = state.getStateSetStackSize() - numToPop;

        state.insertStateSet(insertStateSetPosition, mStateSet);
        std::size_t replayed = 0;
        for (auto rit = first; rit != leaves.end(); ++rit)
        {
            osgUtil::RenderLeaf* rl = *rit;

            if (!shouldReplay(rl))
                continue;

            rl->render(renderInfo, previous);
            ++replayed;
            previous = rl;
        }
        state.removeStateSet(insertStateSetPosition);
        if (inventory)
            stats->setAttribute(state.getFrameStamp()->getFrameNumber(), "Transparent depth replay leaves", replayed);

        msaaFbo ? msaaFbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER)
                : fbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
        state.checkGLErrors("after TransparentDepthBinCallback::drawImplementation");
    }
}
