#include "distortion.hpp"

#include <osg/FrameBufferObject>

#include "postprocessor.hpp"

namespace MWRender
{
    void DistortionCallback::drawImplementation(
        osgUtil::RenderBin* bin, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous)
    {
        osg::State* state = renderInfo.getState();
        unsigned frameId = state->getFrameStamp()->getFrameNumber() % 2;

        PostProcessor* postProcessor = dynamic_cast<PostProcessor*>(renderInfo.getCurrentCamera()->getUserData());

        if (!postProcessor || bin->getStage()->getFrameBufferObject() != postProcessor->getPrimaryFbo(frameId))
            return;

        auto& content = mContent[state->getContextID()];
        const unsigned int frameNumber = state->getFrameStamp()->getFrameNumber();
        if (content.mFrameNumber != frameNumber)
        {
            content.mFrameNumber = frameNumber;
            content.mHasDistortion = false;
        }
        // Child bins and unsorted state graphs are conservatively treated as having content.
        // Accumulate across eyes so an empty eye cannot hide the other eye's distortion.
        const bool hasDrawables = !bin->getRenderLeafList().empty() || !bin->getStateGraphList().empty()
            || !bin->getRenderBinList().empty();
        content.mHasDistortion |= hasDrawables;

        auto& clearedFbo = content.mClearedFbos[frameId];
        if (!hasDrawables && clearedFbo == mFBO[frameId] && mFBO[frameId]->getHandle(state->getContextID()) != 0)
            return;

        // An empty distortion buffer stays empty until this callback draws into
        // it again. Track each frame buffer and context separately; replacements
        // and released GL objects must be initialized with a clear first.
        clearedFbo = nullptr;

        mFBO[frameId]->apply(*state);

        const osg::Texture* tex
            = mFBO[frameId]->getAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0).getTexture();

        glViewport(0, 0, tex->getTextureWidth(), tex->getTextureHeight());
        glClearColor(0.0, 0.0, 0.0, 1.0);
        glColorMask(true, true, true, true);
        state->haveAppliedAttribute(osg::StateAttribute::Type::COLORMASK);
        const bool scissorEnabled = state->getLastAppliedMode(GL_SCISSOR_TEST);
        state->applyMode(GL_SCISSOR_TEST, false);
        glClear(GL_COLOR_BUFFER_BIT);
        state->applyMode(GL_SCISSOR_TEST, scissorEnabled);

        bin->drawImplementation(renderInfo, previous);

        if (!hasDrawables)
            clearedFbo = mFBO[frameId];

        tex = mOriginalFBO[frameId]->getAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0).getTexture();
        glViewport(0, 0, tex->getTextureWidth(), tex->getTextureHeight());
        mOriginalFBO[frameId]->apply(*state);
    }
}
