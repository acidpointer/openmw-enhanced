#ifndef OPENMW_COMPONENTS_RENDER_RESIDENCY_H
#define OPENMW_COMPONENTS_RENDER_RESIDENCY_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Render
{
    // Limits apply to cached assets, not the currently visible working set.
    // A view change must not immediately evict the assets behind the camera.
    class Residency
    {
    public:
        class Charge;
        Residency(const Residency&) = delete;
        Residency& operator=(const Residency&) = delete;

        explicit constexpr Residency(std::uint64_t limit)
            : mLimit(limit)
        {
        }
        constexpr void setLimit(std::uint64_t limit, std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max())
        {
            mLimit = limit;
            mMaximum = maximum;
        }
        constexpr void beginFrame()
        {
            ++mEpoch;
            mActiveBytes = 0;
            // Expire only this bucket, rather than walking every resident asset.
            // Touching a charge moves it to the current bucket in constant time.
            auto& expired = mRecentBuckets[mEpoch % mRecentBuckets.size()];
            while (expired)
                unlink(*expired);
        }
        constexpr std::uint64_t bytes() const { return mBytes; }
        constexpr std::uint64_t activeBytes() const { return mActiveBytes; }
        constexpr std::uint64_t recentBytes() const { return mRecentBytes; }
        constexpr bool evict(bool sourceAlive, std::uint64_t frame, std::uint64_t seen) const
        {
            // The warm allowance belongs above the visible working set, but
            // must never raise the combined allowance past its device ceiling.
            const auto active = std::min(std::max(mActiveBytes, mRecentBytes), mMaximum);
            const auto allowed = active + std::min(mLimit, mMaximum - active);
            return !sourceAlive || (mBytes > allowed && frame > seen && frame - seen > 32);
        }
        static constexpr std::uint64_t limit(std::uint64_t localMemory, unsigned divisor, std::uint64_t minimum,
            std::uint64_t maximum, std::uint64_t fallback)
        {
            return localMemory ? std::clamp(localMemory / divisor, minimum, maximum) : fallback;
        }
        class Charge
        {
        public:
            explicit constexpr Charge(Residency* owner = nullptr)
                : mOwner(owner)
            {
            }
            Charge(const Charge&) = delete;
            Charge& operator=(const Charge&) = delete;
            constexpr ~Charge() { set(0); }
            constexpr void set(std::uint64_t bytes)
            {
                if (mOwner)
                {
                    mOwner->mBytes = mOwner->mBytes - mBytes + bytes;
                    if (mSeenEpoch == mOwner->mEpoch)
                        mOwner->mActiveBytes = mOwner->mActiveBytes - mBytes + bytes;
                    if (mLinked)
                    {
                        if (bytes == 0)
                            mOwner->unlink(*this);
                        else
                            mOwner->mRecentBytes = mOwner->mRecentBytes - mBytes + bytes;
                    }
                }
                mBytes = bytes;
            }
            constexpr void touch()
            {
                if (!mOwner || (mSeenEpoch == mOwner->mEpoch && mLinked))
                    return;
                if (mSeenEpoch != mOwner->mEpoch)
                    mOwner->mActiveBytes += mBytes;
                if (mLinked)
                    mOwner->unlink(*this);
                mSeenEpoch = mOwner->mEpoch;
                if (mBytes)
                    mOwner->link(*this);
            }

        private:
            friend class Residency;
            Residency* mOwner;
            Charge* mPrevious = nullptr;
            Charge* mNext = nullptr;
            bool mLinked = false;
            std::uint64_t mBytes = 0;
            std::uint64_t mSeenEpoch = std::numeric_limits<std::uint64_t>::max();
        };

    private:
        constexpr void unlink(Charge& charge)
        {
            if (charge.mPrevious)
                charge.mPrevious->mNext = charge.mNext;
            else
                mRecentBuckets[charge.mSeenEpoch % mRecentBuckets.size()] = charge.mNext;
            if (charge.mNext)
                charge.mNext->mPrevious = charge.mPrevious;
            mRecentBytes -= charge.mBytes;
            charge.mPrevious = charge.mNext = nullptr;
            charge.mLinked = false;
        }
        constexpr void link(Charge& charge)
        {
            auto& head = mRecentBuckets[mEpoch % mRecentBuckets.size()];
            charge.mNext = head;
            if (head)
                head->mPrevious = &charge;
            head = &charge;
            charge.mLinked = true;
            mRecentBytes += charge.mBytes;
        }

        // Budget for the union of recent views, not just the largest single
        // frame. Each asset occupies one intrusive bucket node, with no per-frame
        // allocations or repeated counting when several views share it.
        std::array<Charge*, 256> mRecentBuckets{};
        std::uint64_t mRecentBytes = 0;
        std::uint64_t mLimit;
        std::uint64_t mBytes = 0;
        std::uint64_t mMaximum = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t mActiveBytes = 0;
        std::uint64_t mEpoch = 0;
    };
}
#endif
