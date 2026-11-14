#ifndef OPENMW_COMPONENTS_RENDER_ALLOCATIONRANGES_H
#define OPENMW_COMPONENTS_RENDER_ALLOCATIONRANGES_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "reserve.hpp"

namespace Render
{
    // Suballocation bookkeeping only. Splitting may allocate CPU storage;
    // releasing a completed GPU resource and coalescing its range never does.
    class AllocationRanges
    {
    public:
        explicit AllocationRanges(std::uint64_t capacity)
            : mRanges{ { 0, capacity, true } }
        {
            if (!capacity)
                throw std::invalid_argument("Rendering allocation capacity must be positive");
        }

        std::optional<std::uint64_t> allocate(std::uint64_t size, std::uint64_t alignment)
        {
            if (!size || !alignment)
                throw std::invalid_argument("Rendering allocation size and alignment must be positive");
            for (std::size_t i = mFirstFree; i < mRanges.size(); ++i)
            {
                const Range original = mRanges[i];
                if (!original.mFree)
                    continue;
                const auto padding = (alignment - original.mOffset % alignment) % alignment;
                if (padding > original.mSize || size > original.mSize - padding)
                    continue;
                reserveFor(mRanges, mRanges.size() + 2);
                const auto offset = original.mOffset + padding;
                const auto remaining = original.mSize - padding - size;
                const bool first = i == mFirstFree;
                mRanges.erase(mRanges.begin() + i);
                if (padding)
                    mRanges.insert(mRanges.begin() + i++, { original.mOffset, padding, true });
                mRanges.insert(mRanges.begin() + i++, { offset, size, false });
                if (remaining)
                    mRanges.insert(mRanges.begin() + i, { offset + size, remaining, true });
                if (first && !padding)
                {
                    mFirstFree = i;
                    while (mFirstFree < mRanges.size() && !mRanges[mFirstFree].mFree)
                        ++mFirstFree;
                }
                return offset;
            }
            return std::nullopt;
        }

        void release(std::uint64_t offset) noexcept
        {
            const auto found = std::lower_bound(mRanges.begin(), mRanges.end(), offset,
                [](const Range& range, std::uint64_t value) { return range.mOffset < value; });
            if (found == mRanges.end() || found->mOffset != offset || found->mFree)
                return;
            std::size_t i = static_cast<std::size_t>(found - mRanges.begin());
            {
                mRanges[i].mFree = true;
                if (i + 1 < mRanges.size() && mRanges[i + 1].mFree)
                {
                    mRanges[i].mSize += mRanges[i + 1].mSize;
                    mRanges.erase(mRanges.begin() + i + 1);
                }
                if (i > 0 && mRanges[i - 1].mFree)
                {
                    mRanges[i - 1].mSize += mRanges[i].mSize;
                    mRanges.erase(mRanges.begin() + i);
                    --i;
                }
                mFirstFree = std::min(mFirstFree, i);
                return;
            }
        }

        bool empty() const { return mRanges.size() == 1 && mRanges.front().mFree; }

    private:
        struct Range
        {
            std::uint64_t mOffset;
            std::uint64_t mSize;
            bool mFree;
        };
        std::vector<Range> mRanges;
        std::size_t mFirstFree = 0;
    };
}

#endif
