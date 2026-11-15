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
            , mLargestFree(capacity)
        {
            if (!capacity)
                throw std::invalid_argument("Rendering allocation capacity must be positive");
        }

        std::optional<std::uint64_t> allocate(std::uint64_t size, std::uint64_t alignment, bool preferEnd = false)
        {
            if (!size || !alignment)
                throw std::invalid_argument("Rendering allocation size and alignment must be positive");
            if (size > mLargestFree)
                return {};
            // Streaming many small meshes should append to unused capacity
            // without scanning alignment gaps or shifting live range records.
            // Fragmented holes remain available when the tail cannot fit.
            if (preferEnd && !mRanges.empty() && mRanges.back().mFree)
                if (auto offset = take(mRanges.size() - 1, size, alignment))
                    return offset;
            std::uint64_t largest = 0;
            for (std::size_t i = mFirstFree; i < mRanges.size(); ++i)
            {
                if (!mRanges[i].mFree)
                    continue;
                largest = std::max(largest, mRanges[i].mSize);
                if (auto offset = take(i, size, alignment))
                    return offset;
            }
            // Cache failed-fit information. Already-full blocks with tiny
            // padding holes must not be rescanned for every subsequent mesh.
            mLargestFree = largest;
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
                mLargestFree = std::max(mLargestFree, mRanges[i].mSize);
                return;
            }
        }

        bool empty() const { return mRanges.size() == 1 && mRanges.front().mFree; }

    private:
        std::optional<std::uint64_t> take(std::size_t i, std::uint64_t size, std::uint64_t alignment)
        {
            const Range original = mRanges[i];
            const auto padding = (alignment - original.mOffset % alignment) % alignment;
            if (padding > original.mSize || size > original.mSize - padding)
                return {};
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
        struct Range
        {
            std::uint64_t mOffset;
            std::uint64_t mSize;
            bool mFree;
        };
        std::vector<Range> mRanges;
        std::size_t mFirstFree = 0;
        std::uint64_t mLargestFree;
    };
}

#endif
