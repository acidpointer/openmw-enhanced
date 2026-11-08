#include "pingpongcanvas.hpp"

#include <components/debug/frametrace.hpp>
#include <cstdlib>
#include <string_view>

#include <cassert>
#include <osg/FrameStamp>
#include <osg/GLExtensions>

#include <components/shader/shadermanager.hpp>
#include <components/stereo/multiview.hpp>
#include <components/stereo/stereomanager.hpp>

#include <osg/Texture2DArray>

#include "postprocessor.hpp"

namespace MWRender
{
    static void clearColorTarget(osg::State& state, const osg::Vec4f& color)
    {
        glClearColor(color.r(), color.g(), color.b(), color.a());
        glColorMask(true, true, true, true);
        state.haveAppliedAttribute(osg::StateAttribute::COLORMASK);
        const bool scissor = state.getLastAppliedMode(GL_SCISSOR_TEST);
        state.applyMode(GL_SCISSOR_TEST, false);
        glClear(GL_COLOR_BUFFER_BIT);
        state.applyMode(GL_SCISSOR_TEST, scissor);
    }

    PingPongCanvas::PingPongCanvas(Shader::ShaderManager& shaderManager,
        const std::shared_ptr<LuminanceCalculator>& luminanceCalculator, DistortionCallback* distortionCallback)
        : mFallbackStateSet(new osg::StateSet)
        , mMultiviewResolveStateSet(new osg::StateSet)
        , mDistortionCallback(distortionCallback)
        , mLuminanceCalculator(luminanceCalculator)
    {
        setUseDisplayList(false);
        setUseVertexBufferObjects(true);

        osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array;
        verts->push_back(osg::Vec3f(-1, -1, 0));
        verts->push_back(osg::Vec3f(-1, 3, 0));
        verts->push_back(osg::Vec3f(3, -1, 0));

        setVertexArray(verts);

        addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::TRIANGLES, 0, 3));

        mLuminanceCalculator->disable();

        Shader::ShaderManager::DefineMap defines;
        Stereo::shaderStereoDefines(defines);

        mFallbackProgram = shaderManager.getProgram("fullscreen_tri");

        mFallbackStateSet->setAttributeAndModes(mFallbackProgram);
        mFallbackStateSet->addUniform(new osg::Uniform("lastShader", 0));
        mFallbackStateSet->addUniform(new osg::Uniform("scaling", osg::Vec2f(1, 1)));

        mMultiviewResolveProgram = shaderManager.getProgram("multiview_resolve");
        mMultiviewResolveStateSet->setAttributeAndModes(mMultiviewResolveProgram);
        mMultiviewResolveStateSet->addUniform(new osg::Uniform("lastShader", 0));
    }

    void PingPongCanvas::setPasses(Fx::DispatchArray&& passes)
    {
        mPasses = std::move(passes);
        mDirty = true;
    }

    void PingPongCanvas::setMask(bool underwater, bool exterior)
    {
        mMask = Fx::Technique::getEnvironmentMask(underwater, exterior);
    }

    void PingPongCanvas::drawGeometry(osg::RenderInfo& renderInfo) const
    {
        osg::Geometry::drawImplementation(renderInfo);
    }

    void PingPongCanvas::resizeGLObjectBuffers(unsigned int maxSize)
    {
        osg::Geometry::resizeGLObjectBuffers(maxSize);
        mEmptyUniformStacks.resize(maxSize);
        mGpuTimings.resize(maxSize);
    }

    static void attachCloneOfTemplate(
        osg::FrameBufferObject* fbo, osg::Camera::BufferComponent component, osg::Texture* tex)
    {
        osg::ref_ptr<osg::Texture> clone = static_cast<osg::Texture*>(tex->clone(osg::CopyOp::SHALLOW_COPY));
        fbo->setAttachment(component, Stereo::createMultiviewCompatibleAttachment(clone));
    }

    static void cacheEmptyUniformStacks(osg::State::UniformMap& uniformMap, osg::State::UniformMap& cache)
    {
        for (auto it = uniformMap.begin(); it != uniformMap.end();)
        {
            if (!it->second.uniformVec.empty())
            {
                ++it;
                continue;
            }

            cache.insert(uniformMap.extract(it++));
        }
    }

    void PingPongCanvas::drawImplementation(osg::RenderInfo& renderInfo) const
    {
        osg::State& state = *renderInfo.getState();
        auto& uniformMap = const_cast<osg::State::UniformMap&>(state.getUniformMap());
        auto& emptyUniformStacks = mEmptyUniformStacks[state.getContextID()];
        uniformMap.merge(emptyUniformStacks);
        emptyUniformStacks.clear();
        osg::GLExtensions* ext = state.get<osg::GLExtensions>();

        const unsigned frameNumber = state.getFrameStamp()->getFrameNumber();
        size_t frameId = frameNumber % 2;
        static const bool collectGpuTimings = [] {
            const char* value = std::getenv("OPENMW_GPU_TIMINGS");
            return value && std::string_view(value) == "1";
        }();
        const bool gpuTimings = collectGpuTimings && ext->isARBTimerQuerySupported && ext->glQueryCounter
            && ext->glGetQueryObjectui64v && ext->glGetQueryObjectiv && ext->glGenQueries && ext->glDeleteQueries;
        auto& pendingTimings = mGpuTimings[state.getContextID()];
        if (gpuTimings)
        {
            for (auto it = pendingTimings.begin(); it != pendingTimings.end();)
            {
                GLint available = GL_FALSE;
                ext->glGetQueryObjectiv(it->mQueries[1], GL_QUERY_RESULT_AVAILABLE, &available);
                if (!available)
                {
                    ++it;
                    continue; // Never synchronously wait for a GPU result.
                }
                GLuint64 begin = 0, end = 0;
                ext->glGetQueryObjectui64v(it->mQueries[0], GL_QUERY_RESULT, &begin);
                ext->glGetQueryObjectui64v(it->mQueries[1], GL_QUERY_RESULT, &end);
                if (end >= begin && it->mStats)
                    it->mStats->setAttribute(it->mFrame, it->mName, static_cast<double>(end - begin) * 1e-9);
                ext->glDeleteQueries(2, it->mQueries.data());
                it = pendingTimings.erase(it);
            }
        }

        auto beginGpuTiming = [&](std::string_view effect, std::string_view pass = {}) -> std::optional<GpuTiming> {
            if (!gpuTimings || pendingTimings.size() >= 256)
                return std::nullopt;

            GpuTiming timing;
            timing.mFrame = frameNumber;
            timing.mName = "GPU postprocess " + std::string(effect);
            if (!pass.empty())
                timing.mName += "/" + std::string(pass);
            timing.mName += " time taken";
            timing.mStats = mGpuTimingStats;
            ext->glGenQueries(2, timing.mQueries.data());
            ext->glQueryCounter(timing.mQueries[0], GL_TIMESTAMP);
            return timing;
        };

        auto endGpuTiming = [&](std::optional<GpuTiming>& timing) {
            if (!timing)
                return;
            ext->glQueryCounter(timing->mQueries[1], GL_TIMESTAMP);
            pendingTimings.push_back(std::move(*timing));
        };

        std::vector<size_t> filtered;

        filtered.reserve(mPasses.size());

        for (size_t i = 0; i < mPasses.size(); ++i)
        {
            const auto& node = mPasses[i];

            if (mMask & node.mFlags)
                continue;

            if (node.mHandle->getInternal() && node.mHandle->getName() == "internal_distortion" && mDistortionCallback
                && !mDistortionCallback->hasDistortion(state.getContextID(), frameNumber))
                continue;

            filtered.push_back(i);
        }

        auto* resolveViewport = state.getCurrentViewport();

        if (mDirty)
        {
            mFbos.fill(nullptr);
            mMultiviewResolveFramebuffer = nullptr;
            mLuminanceCalculator->dirty(mTextureScene->getTextureWidth(), mTextureScene->getTextureHeight());

            mRenderViewport
                = new osg::Viewport(0, 0, mTextureScene->getTextureWidth(), mTextureScene->getTextureHeight());

            mDirty = false;
        }

        auto reportIntermediates = [&]() {
            if (!mGpuTimingStats || !mGpuTimingStats->collectStats("resource"))
                return;

            unsigned int allocated = 0;
            for (const auto& fbo : mFbos)
                if (fbo && fbo->getHandle(state.getContextID()) != 0)
                    ++allocated;
            mGpuTimingStats->setAttribute(frameNumber, "Postprocess intermediates", allocated);
        };

        if (filtered.empty() || !mPostprocessing)
        {
            state.pushStateSet(mFallbackStateSet);
            state.apply();

            if (Stereo::getMultiview())
            {
                state.pushStateSet(mMultiviewResolveStateSet);
                state.apply();
            }

            state.applyTextureAttribute(0, mTextureScene);
            state.applyAttribute(resolveViewport);

            auto timing = beginGpuTiming("presentation");
            drawGeometry(renderInfo);
            endGpuTiming(timing);
            state.popStateSet();

            if (Stereo::getMultiview())
                state.popStateSet();

            cacheEmptyUniformStacks(uniformMap, emptyUniformStacks);
            reportIntermediates();
            return;
        }

        // A single resolving pass needs no intermediate scene textures. Allocate a ping-pong
        // buffer only when a pass actually writes to it, rather than clearing three full-size
        // buffers on each canvas just because post-processing is enabled.
        auto bindIntermediateFbo = [&](int attachment) {
            auto& fbo = mFbos[attachment - GL_COLOR_ATTACHMENT0_EXT];
            if (!fbo)
            {
                fbo = new osg::FrameBufferObject;
                attachCloneOfTemplate(fbo, osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0, mTextureScene);
            }

            const bool initialize = fbo->getHandle(state.getContextID()) == 0;
            fbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
            if (initialize)
            {
                clearColorTarget(state, osg::Vec4f(0.5, 0.5, 0.5, 1));
            }
            return fbo->getHandle(state.getContextID());
        };

        if (Stereo::getMultiview() && !mMultiviewResolveFramebuffer)
        {
            mMultiviewResolveFramebuffer = new osg::FrameBufferObject();
            attachCloneOfTemplate(
                mMultiviewResolveFramebuffer, osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0, mTextureScene);
            mMultiviewResolveFramebuffer->apply(state);
            clearColorTarget(state, osg::Vec4f(0.5, 0.5, 0.5, 1));

            mMultiviewResolveStateSet->setTextureAttribute(PostProcessor::Unit_LastShader,
                (osg::Texture*)mMultiviewResolveFramebuffer->getAttachment(osg::Camera::COLOR_BUFFER0).getTexture());
        }

        constexpr std::array<std::array<int, 2>, 3> buffers
            = { { { GL_COLOR_ATTACHMENT1_EXT, GL_COLOR_ATTACHMENT2_EXT },
                { GL_COLOR_ATTACHMENT0_EXT, GL_COLOR_ATTACHMENT2_EXT },
                { GL_COLOR_ATTACHMENT0_EXT, GL_COLOR_ATTACHMENT1_EXT } } };

        (mAvgLum) ? mLuminanceCalculator->enable() : mLuminanceCalculator->disable();

        // A histogram based approach is superior way to calculate scene luminance. Using mipmaps is more broadly
        // supported, so that's what we use for now.
        if (mAvgLum)
        {
            auto timing = beginGpuTiming("luminance");
            mLuminanceCalculator->draw(*this, renderInfo, state, ext, frameId);
            endGpuTiming(timing);
        }

        auto buffer = buffers[0];

        int lastDraw = 0;
        int lastShader = 0;

        unsigned int lastApplied = 0;

        const unsigned int cid = state.getContextID();

        const osg::ref_ptr<osg::FrameBufferObject>& destinationFbo = mDestinationFBO ? mDestinationFBO : nullptr;
        unsigned int destinationHandle = destinationFbo ? destinationFbo->getHandle(cid) : 0;

        auto bindDestinationFbo = [&]() {
            if (destinationFbo)
            {
                destinationFbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
                lastApplied = destinationHandle;
            }
            else if (Stereo::getMultiview())
            {
                mMultiviewResolveFramebuffer->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
                lastApplied = mMultiviewResolveFramebuffer->getHandle(cid);
            }
            else
            {
                ext->glBindFramebuffer(GL_DRAW_FRAMEBUFFER_EXT, 0);

                lastApplied = 0;
            }
        };

        // When textures are created (or resized) we need to either dirty them and/or clear them.
        // Otherwise, there will be undefined behavior when reading from a texture that has yet to be written to in a
        // later pass.
        for (const auto& attachment : mDirtyAttachments)
        {
            const auto [w, h]
                = attachment.mSize.get(mTextureScene->getTextureWidth(), mTextureScene->getTextureHeight());

            attachment.mTarget->setTextureSize(w, h);
            if (attachment.mMipMap)
                attachment.mTarget->setNumMipmapLevels(osg::Image::computeNumberOfMipmapLevels(w, h));
            attachment.mTarget->dirtyTextureObject();

            osg::ref_ptr<osg::FrameBufferObject> fbo = new osg::FrameBufferObject;

            fbo->setAttachment(
                osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0, osg::FrameBufferAttachment(attachment.mTarget));
            fbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);

            glViewport(0, 0, attachment.mTarget->getTextureWidth(), attachment.mTarget->getTextureHeight());
            state.haveAppliedAttribute(osg::StateAttribute::VIEWPORT);
            clearColorTarget(state, attachment.mClearColor);

            if (attachment.mMipMap)
            {
                state.setActiveTextureUnit(0);
                state.applyTextureAttribute(0, attachment.mTarget);
                ext->glGenerateMipmap(GL_TEXTURE_2D);
            }
        }

        for (const size_t& index : filtered)
        {
            const auto& node = mPasses[index];

            node.mRootStateSet->setTextureAttribute(PostProcessor::Unit_Depth, mTextureDepth);
            node.mRootStateSet->setTextureAttribute(PostProcessor::Unit_WorldDepth, mTextureWorldDepth);

            if (mAvgLum)
                node.mRootStateSet->setTextureAttribute(PostProcessor::TextureUnits::Unit_EyeAdaptation,
                    mLuminanceCalculator->getLuminanceTexture(frameId));

            if (mTextureNormals)
                node.mRootStateSet->setTextureAttribute(PostProcessor::TextureUnits::Unit_Normals, mTextureNormals);

            if (mTextureDistortion)
                node.mRootStateSet->setTextureAttribute(
                    PostProcessor::TextureUnits::Unit_Distortion, mTextureDistortion);

            state.pushStateSet(node.mRootStateSet);

            for (size_t passIndex = 0; passIndex < node.mPasses.size(); ++passIndex)
            {
                const auto& pass = node.mPasses[passIndex];

                bool lastPass = passIndex == node.mPasses.size() - 1;
                const bool resolveToOutput = !pass.mRenderTarget && pass.mResolve && index == filtered.back();

                // VR-TODO: This won't actually work for tex2darrays
                if (lastShader == 0)
                    pass.mStateSet->setTextureAttribute(PostProcessor::Unit_LastShader, mTextureScene);
                else
                    pass.mStateSet->setTextureAttribute(PostProcessor::Unit_LastShader,
                        (osg::Texture*)mFbos[lastShader - GL_COLOR_ATTACHMENT0_EXT]
                            ->getAttachment(osg::Camera::COLOR_BUFFER0)
                            .getTexture());

                if (lastDraw == 0)
                    pass.mStateSet->setTextureAttribute(PostProcessor::Unit_LastPass, mTextureScene);
                else
                    pass.mStateSet->setTextureAttribute(PostProcessor::Unit_LastPass,
                        (osg::Texture*)mFbos[lastDraw - GL_COLOR_ATTACHMENT0_EXT]
                            ->getAttachment(osg::Camera::COLOR_BUFFER0)
                            .getTexture());

                if (pass.mRenderTarget)
                {
                    pass.mRenderTarget->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);

                    lastApplied = pass.mRenderTarget->getHandle(state.getContextID());
                }
                else if (resolveToOutput)
                {
                    bindDestinationFbo();
                }
                else if (lastPass)
                {
                    lastDraw = buffer[0];
                    lastShader = buffer[0];
                    lastApplied = bindIntermediateFbo(buffer[0]);
                    buffer = buffers[lastShader - GL_COLOR_ATTACHMENT0_EXT];
                }
                else
                {
                    lastApplied = bindIntermediateFbo(buffer[0]);
                    lastDraw = buffer[0];
                    std::swap(buffer[0], buffer[1]);
                }

                state.pushStateSet(pass.mStateSet);
                // Apply the complete pass once. Applying a temporary parent
                // program between draws only adds driver work and uniform scans.
                state.apply();
                if (!pass.mRenderTarget)
                    state.applyAttribute(resolveToOutput && !destinationFbo && !Stereo::getMultiview()
                            ? resolveViewport
                            : mRenderViewport.get());

                if (!state.getLastAppliedProgramObject())
                    mFallbackProgram->apply(state);

                auto timing = beginGpuTiming(node.mHandle->getName(), pass.mName);
                {
                    Debug::Trace::Scope trace("Postprocess draw dispatch", frameNumber);
                    drawGeometry(renderInfo);
                }

                // The effect declaration controls generation, not the allocated level count
                // (which can also include just the base level).
                if (pass.mRenderTarget && pass.mMipMap)
                {
                    state.setActiveTextureUnit(0);
                    state.applyTextureAttribute(0,
                        pass.mRenderTarget->getAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0)
                            .getTexture());
                    ext->glGenerateMipmap(GL_TEXTURE_2D);
                }

                endGpuTiming(timing);
                state.popStateSet();
            }

            state.popStateSet();
        }

        if (Stereo::getMultiview())
        {
            ext->glBindFramebuffer(GL_DRAW_FRAMEBUFFER_EXT, 0);
            lastApplied = 0;

            state.pushStateSet(mMultiviewResolveStateSet);
            state.apply();
            state.applyAttribute(resolveViewport);

            drawGeometry(renderInfo);

            state.popStateSet();
        }

        if (lastApplied != destinationHandle)
        {
            bindDestinationFbo();
        }

        cacheEmptyUniformStacks(uniformMap, emptyUniformStacks);
        mDirtyAttachments.clear();
        reportIntermediates();
    }
}
