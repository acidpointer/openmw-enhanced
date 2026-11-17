#ifndef OPENMW_COMPONENTS_RENDER_EFFECTLIFETIME_H
#define OPENMW_COMPONENTS_RENDER_EFFECTLIFETIME_H

#include <algorithm>
#include <cstddef>
#include <limits>

namespace Render
{
    // Only an unconditional first write can make prior-frame contents dead.
    // A read in that same pass is feedback, not a transient replacement.
    class EffectLifetime
    {
    public:
        constexpr void read(std::size_t pass)
        {
            mFirstRead = std::min(mFirstRead, pass);
            mLast = std::max(mLast, pass);
        }
        constexpr void write(std::size_t pass, bool replaces)
        {
            if (pass < mFirstWrite)
            {
                mFirstWrite = pass;
                mReplaces = replaces;
            }
            mLast = std::max(mLast, pass);
        }
        constexpr bool transient() const { return mReplaces && mFirstWrite < mFirstRead; }
        constexpr std::size_t first() const { return mFirstWrite; }
        constexpr std::size_t last() const { return mLast; }

    private:
        std::size_t mFirstWrite = std::numeric_limits<std::size_t>::max();
        std::size_t mFirstRead = std::numeric_limits<std::size_t>::max();
        std::size_t mLast = 0;
        bool mReplaces = false;
    };
    constexpr bool disjointEffectLifetimes(
        std::size_t first, std::size_t last, std::size_t otherFirst, std::size_t otherLast)
    {
        return last < otherFirst || otherLast < first;
    }
}

#endif
