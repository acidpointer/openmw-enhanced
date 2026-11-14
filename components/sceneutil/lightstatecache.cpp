#include "lightstatecache.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace SceneUtil
{
    void LightStateCache::clear()
    {
        mLastView = nullptr;
        mCameras.clear();
        mTransientViews.clear();
        mFrame.reset();
        mEpoch = 0;
        mParameterRevision = 0;
        mStats.mRetainedStates = mStats.mUniformBytes = mStats.mRetainedLights = 0;
        mSlotStats = {};
    }

    void LightStateCache::retire(View& view)
    {
        for (const auto& [key, state] : view.mStates)
            if (state.mRetained)
            {
                --mStats.mRetainedStates;
                mStats.mUniformBytes -= std::size_t(mMaxLights) * sizeof(osg::Matrixf);
                --mSlotStats[view.mSlot].mRetainedStates;
                mSlotStats[view.mSlot].mUniformBytes -= std::size_t(mMaxLights) * sizeof(osg::Matrixf);
            }
        for (const auto& [key, light] : view.mLights)
            if (light.mRetained)
            {
                --mStats.mRetainedLights;
                --mSlotStats[view.mSlot].mRetainedLights;
            }
    }

    void LightStateCache::prune(View& view, std::size_t frame, std::size_t keepFrames)
    {
        const auto expired = [frame, keepFrames](const auto& entry) {
            return !entry.mRetained || (frame >= entry.mFrame && frame - entry.mFrame > keepFrames);
        };
        for (auto it = view.mStates.begin(); it != view.mStates.end();)
            if (expired(it->second))
            {
                if (it->second.mRetained)
                {
                    --mStats.mRetainedStates;
                    mStats.mUniformBytes -= std::size_t(mMaxLights) * sizeof(osg::Matrixf);
                    --mSlotStats[view.mSlot].mRetainedStates;
                    mSlotStats[view.mSlot].mUniformBytes -= std::size_t(mMaxLights) * sizeof(osg::Matrixf);
                }
                it = view.mStates.erase(it);
            }
            else
                ++it;
        for (auto it = view.mLights.begin(); it != view.mLights.end();)
            if (expired(it->second))
            {
                if (it->second.mRetained)
                {
                    --mStats.mRetainedLights;
                    --mSlotStats[view.mSlot].mRetainedLights;
                }
                it = view.mLights.erase(it);
            }
            else
                ++it;
    }

    void LightStateCache::beginFrame(std::size_t frame)
    {
        mLastView = nullptr;
        // A reset/wrap must allocate fresh state rather than mutating objects
        // from a traversal whose position in the pipeline is now unknown.
        if ((mFrame && frame < *mFrame) || mEpoch == std::numeric_limits<std::uint64_t>::max())
            clear();
        mFrame = frame;
        ++mEpoch;
        mTransientViews.clear();
        const bool pressure = std::any_of(mSlotStats.begin(), mSlotStats.end(), [](const Stats& slot) {
            return slot.mRetainedStates >= MaxStatesPerSlot * 3 / 4
                || slot.mUniformBytes >= MaxUniformBytesPerSlot * 3 / 4
                || slot.mRetainedLights >= MaxPackedLightsPerSlot * 3 / 4;
        });
        const std::size_t keepFrames = pressure ? 2 : 120;
        for (auto it = mCameras.begin(); it != mCameras.end();)
            if (!it->first.valid())
            {
                for (auto& pair : it->second.mViews)
                    for (auto& view : pair)
                        retire(view);
                it = mCameras.erase(it);
            }
            else
            {
                bool empty = true;
                for (auto& pair : it->second.mViews)
                    for (auto& view : pair)
                    {
                        prune(view, frame, keepFrames);
                        empty = empty && view.mStates.empty() && view.mLights.empty();
                    }
                if (empty)
                    it = mCameras.erase(it);
                else
                    ++it;
            }
    }

    LightStateCache::View& LightStateCache::getView(osg::Camera* camera, const osg::Matrixd& matrix, std::size_t frame)
    {
        // Most objects use the same view. Avoid creating an observer handle
        // and searching camera metadata for every object on this hot path.
        if (camera && mLastView && mLastView->mCamera.get() == camera
            && std::memcmp(mLastView->mMatrix.ptr(), matrix.ptr(), 16 * sizeof(double)) == 0)
            return *mLastView;
        auto found = mCameras.find(osg::observer_ptr<osg::Camera>(camera));
        if (camera && found == mCameras.end() && mCameras.size() < MaxCameras)
            found = mCameras.try_emplace(osg::observer_ptr<osg::Camera>(camera)).first;
        if (camera && found != mCameras.end())
        {
            View* available = nullptr;
            for (auto& pair : found->second.mViews)
            {
                auto& view = pair[frame % 2];
                if (view.mEpoch == mEpoch && std::memcmp(view.mMatrix.ptr(), matrix.ptr(), 16 * sizeof(double)) == 0)
                {
                    mLastView = &view;
                    return view;
                }
                if (!available && view.mEpoch != mEpoch)
                    available = &view;
            }
            if (available)
            {
                available->mMatrix = matrix;
                available->mCamera = camera;
                available->mEpoch = mEpoch;
                available->mSlot = frame % 2;
                available->mRetainable = true;
                mLastView = available;
                return *available;
            }
        }
        // Missing identity or excess views retain frame-local deduplication.
        // Fresh objects on the next frame cannot overwrite an in-flight view.
        for (auto& view : mTransientViews)
            if (view.mCamera.get() == camera && std::memcmp(view.mMatrix.ptr(), matrix.ptr(), 16 * sizeof(double)) == 0)
                return view;
        auto& view = mTransientViews.emplace_back();
        view.mCamera = camera;
        view.mMatrix = matrix;
        view.mEpoch = mEpoch;
        return view;
    }

    void LightStateCache::create(State& state, std::size_t count)
    {
        state.mStateSet = new osg::StateSet;
        state.mBuffer = new osg::Uniform(osg::Uniform::FLOAT_MAT4, "LightBuffer", mMaxLights);
        state.mCount = new osg::Uniform("PointLightCount", static_cast<int>(count));
        state.mRevisions.assign(count, 0);
        state.mStateSet->addUniform(state.mBuffer);
        state.mStateSet->addUniform(state.mCount);
        ++mStats.mCreatedStates;
    }

    osg::ref_ptr<osg::StateSet> LightStateCache::getImpl(osg::Camera* camera, const osg::Matrixd& matrix,
        std::size_t frame, const std::vector<int>& ids, unsigned int maxLights, const void* context,
        osg::Matrixf (*pack)(const void*, std::size_t))
    {
        if (ids.size() > maxLights)
            throw std::invalid_argument("light list exceeds shader uniform capacity");
        if (mMaxLights != maxLights)
        {
            clear();
            mMaxLights = maxLights;
        }
        if (!mFrame || *mFrame != frame)
            beginFrame(frame);
        auto& view = getView(camera, matrix, frame);
        auto& slotStats = mSlotStats[frame % 2];
        auto [entry, inserted] = view.mStates.try_emplace(ids);
        auto& state = entry->second;
        if (inserted)
        {
            const std::size_t bytes = std::size_t(maxLights) * sizeof(osg::Matrixf);
            state.mRetained = view.mRetainable && slotStats.mRetainedStates < MaxStatesPerSlot
                && bytes <= MaxUniformBytesPerSlot - slotStats.mUniformBytes;
            if (state.mRetained)
            {
                ++mStats.mRetainedStates;
                mStats.mUniformBytes += bytes;
                ++slotStats.mRetainedStates;
                slotStats.mUniformBytes += bytes;
            }
            create(state, ids.size());
        }
        if (state.mEpoch == mEpoch)
            return state.mStateSet;
        if (state.mFrame == frame)
            create(state, ids.size()); // repeated update in one frame: keep the old snapshot immutable
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            auto [packed, newLight] = view.mLights.try_emplace(ids[i]);
            auto& light = packed->second;
            if (newLight)
            {
                light.mRetained = state.mRetained && slotStats.mRetainedLights < MaxPackedLightsPerSlot;
                if (light.mRetained)
                {
                    ++mStats.mRetainedLights;
                    ++slotStats.mRetainedLights;
                }
            }
            if (light.mEpoch != mEpoch)
            {
                const auto packedMatrix = pack(context, i);
                if (!light.mRevision || std::memcmp(light.mMatrix.ptr(), packedMatrix.ptr(), 16 * sizeof(float)) != 0)
                {
                    light.mMatrix = packedMatrix;
                    // Tokens must survive record eviction/recreation: a per-
                    // record counter could falsely match a retained uniform.
                    // Saturation uses zero to force writes until cache reset.
                    light.mRevision
                        = mParameterRevision == std::numeric_limits<std::uint64_t>::max() ? 0 : ++mParameterRevision;
                }
                light.mEpoch = mEpoch;
                ++mStats.mPackedLights;
            }
            light.mFrame = frame;
            if (!light.mRevision || state.mRevisions[i] != light.mRevision)
            {
                state.mBuffer->setElement(static_cast<unsigned int>(i), light.mMatrix);
                state.mRevisions[i] = light.mRevision;
            }
        }
        state.mEpoch = mEpoch;
        state.mFrame = frame;
        return state.mStateSet;
    }
}
