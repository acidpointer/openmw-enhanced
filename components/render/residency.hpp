#ifndef OPENMW_COMPONENTS_RENDER_RESIDENCY_H
#define OPENMW_COMPONENTS_RENDER_RESIDENCY_H

#include <algorithm>
#include <cstdint>
#include <limits>

namespace Render
{
    // Limits apply to cached assets, not the currently visible working set.
    // A view change must not immediately evict the assets behind the camera.
    class Residency
    {
    public:
        explicit Residency(std::uint64_t limit)
            : mLimit(limit)
        {
        }
        void setLimit(std::uint64_t limit, std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max())
        {
            mLimit = limit;
            mMaximum = maximum;
        }
        void beginFrame()
        {
            // Briefly looking at the sky or a wall must not collapse the
            // allowance and evict the view we are about to turn back to.
            if (mActiveBytes >= mWorkingSetBytes || mEpoch - mWorkingSetEpoch > 256)
            {
                mWorkingSetBytes = mActiveBytes;
                mWorkingSetEpoch = mEpoch;
            }
            ++mEpoch;
            mActiveBytes = 0;
        }
        std::uint64_t bytes() const { return mBytes; }
        std::uint64_t activeBytes() const { return mActiveBytes; }
        bool evict(bool sourceAlive, std::uint64_t frame, std::uint64_t seen) const
        {
            // The warm allowance belongs above the visible working set, but
            // must never raise the combined allowance past its device ceiling.
            const auto active = std::min(std::max(mActiveBytes, mWorkingSetBytes), mMaximum);
            const auto allowed = active + std::min(mLimit, mMaximum - active);
            return !sourceAlive || (mBytes > allowed && frame > seen && frame - seen > 32);
        }
        static std::uint64_t limit(std::uint64_t localMemory, unsigned divisor, std::uint64_t minimum,
            std::uint64_t maximum, std::uint64_t fallback)
        {
            return localMemory ? std::clamp(localMemory / divisor, minimum, maximum) : fallback;
        }
        class Charge
        {
        public:
            explicit Charge(Residency* owner = nullptr)
                : mOwner(owner)
            {
            }
            Charge(const Charge&) = delete;
            Charge& operator=(const Charge&) = delete;
            ~Charge() { set(0); }
            void set(std::uint64_t bytes)
            {
                if (mOwner)
                {
                    mOwner->mBytes = mOwner->mBytes - mBytes + bytes;
                    if (mSeenEpoch == mOwner->mEpoch)
                        mOwner->mActiveBytes = mOwner->mActiveBytes - mBytes + bytes;
                }
                mBytes = bytes;
            }
            void touch()
            {
                if (mOwner && mSeenEpoch != mOwner->mEpoch)
                {
                    mOwner->mActiveBytes += mBytes;
                    mSeenEpoch = mOwner->mEpoch;
                }
            }

        private:
            Residency* mOwner;
            std::uint64_t mBytes = 0;
            std::uint64_t mSeenEpoch = std::numeric_limits<std::uint64_t>::max();
        };

    private:
        std::uint64_t mLimit;
        std::uint64_t mBytes = 0;
        std::uint64_t mMaximum = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t mActiveBytes = 0;
        std::uint64_t mEpoch = 0;
        std::uint64_t mWorkingSetBytes = 0;
        std::uint64_t mWorkingSetEpoch = 0;
    };
}
#endif
