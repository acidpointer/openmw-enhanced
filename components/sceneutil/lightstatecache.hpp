#ifndef OPENMW_COMPONENTS_SCENEUTIL_LIGHTSTATECACHE_H
#define OPENMW_COMPONENTS_SCENEUTIL_LIGHTSTATECACHE_H

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include <osg/Camera>
#include <osg/Matrixf>
#include <osg/StateSet>
#include <osg/Uniform>
#include <osg/observer_ptr>

namespace SceneUtil
{
    // Same two-frame lifetime contract as LightSource's parameter buffers.
    // CullThreadPerCamera is not supported, as with LightListCallback itself.
    class LightStateCache
    {
    public:
        LightStateCache() = default;
        LightStateCache(const LightStateCache&) = delete;
        LightStateCache& operator=(const LightStateCache&) = delete;
        static constexpr std::size_t MaxStates = 8192;
        static constexpr std::size_t MaxUniformBytes = 16 * 1024 * 1024;
        static constexpr std::size_t MaxPackedLights = 32768;
        // Reserve equal capacity for both in-flight frame slots, so a busy
        // first frame cannot leave the other slot allocating forever.
        static constexpr std::size_t MaxStatesPerSlot = MaxStates / 2;
        static constexpr std::size_t MaxUniformBytesPerSlot = MaxUniformBytes / 2;
        static constexpr std::size_t MaxPackedLightsPerSlot = MaxPackedLights / 2;
        static constexpr std::size_t MaxCameras = 16;
        static constexpr std::size_t MaxViews = 4;

        struct Stats
        {
            std::size_t mRetainedStates = 0, mUniformBytes = 0, mRetainedLights = 0;
            std::size_t mCreatedStates = 0, mPackedLights = 0;
        };

        void clear();
        void beginFrame(std::size_t frame);
        const Stats& stats() const { return mStats; }
        template <class Pack>
        osg::ref_ptr<osg::StateSet> get(osg::Camera* camera, const osg::Matrixd& view, std::size_t frame,
            const std::vector<int>& ids, unsigned int maxLights, const Pack& pack)
        {
            // A synchronous non-owning callback avoids std::function allocation
            // in the per-object cull path. Neither pointer escapes getImpl.
            return getImpl(camera, view, frame, ids, maxLights, &pack,
                [](const void* context, std::size_t index) { return (*static_cast<const Pack*>(context))(index); });
        }

    private:
        osg::ref_ptr<osg::StateSet> getImpl(osg::Camera* camera, const osg::Matrixd& view, std::size_t frame,
            const std::vector<int>& ids, unsigned int maxLights, const void* context,
            osg::Matrixf (*pack)(const void*, std::size_t));
        static constexpr auto InvalidFrame = std::numeric_limits<std::size_t>::max();
        struct State
        {
            osg::ref_ptr<osg::StateSet> mStateSet;
            osg::ref_ptr<osg::Uniform> mBuffer, mCount;
            std::vector<std::uint64_t> mRevisions;
            std::uint64_t mEpoch = 0;
            std::size_t mFrame = InvalidFrame;
            bool mRetained = false;
        };
        struct PackedLight
        {
            osg::Matrixf mMatrix;
            std::uint64_t mEpoch = 0;
            std::uint64_t mRevision = 0;
            std::size_t mFrame = InvalidFrame;
            bool mRetained = false;
        };
        struct View
        {
            osg::observer_ptr<osg::Camera> mCamera;
            osg::Matrixd mMatrix;
            std::uint64_t mEpoch = 0;
            std::size_t mSlot = 0;
            bool mRetainable = false;
            std::map<std::vector<int>, State> mStates;
            std::unordered_map<int, PackedLight> mLights;
        };
        struct Camera
        {
            std::array<std::array<View, 2>, MaxViews> mViews;
        };

        View& getView(osg::Camera* camera, const osg::Matrixd& matrix, std::size_t frame);
        void prune(View& view, std::size_t frame, std::size_t keepFrames);
        void retire(View& view);
        void create(State& state, std::size_t count);
        std::map<osg::observer_ptr<osg::Camera>, Camera> mCameras;
        std::vector<View> mTransientViews;
        View* mLastView = nullptr; // retained map storage; reset before pruning
        std::optional<std::size_t> mFrame;
        std::uint64_t mEpoch = 0;
        std::uint64_t mParameterRevision = 0;
        unsigned int mMaxLights = 0;
        Stats mStats;
        std::array<Stats, 2> mSlotStats;
    };
}
#endif
