#ifndef OPENMW_COMPONENTS_RENDER_RESIDENCY_H
#define OPENMW_COMPONENTS_RENDER_RESIDENCY_H

#include <algorithm>
#include <cstdint>

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
        void setLimit(std::uint64_t limit) { mLimit = limit; }
        std::uint64_t bytes() const { return mBytes; }
        bool evict(bool sourceAlive, std::uint64_t frame, std::uint64_t seen) const
        {
            return !sourceAlive || (mBytes > mLimit && frame > seen && frame - seen > 32);
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
                    mOwner->mBytes = mOwner->mBytes - mBytes + bytes;
                mBytes = bytes;
            }

        private:
            Residency* mOwner;
            std::uint64_t mBytes = 0;
        };

    private:
        std::uint64_t mLimit;
        std::uint64_t mBytes = 0;
    };
}
#endif
