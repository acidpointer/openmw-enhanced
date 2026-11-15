#ifndef OPENMW_COMPONENTS_RENDER_BUFFERARENA_H
#define OPENMW_COMPONENTS_RENDER_BUFFERARENA_H

#include "allocationranges.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace Render
{
    // Suballocate buffer objects as well as device memory. Slices own their
    // range through the last GPU frame reference, including after pool clear.
    // All allocation/retirement operations belong to the rendering thread.
    template <class Page>
    class BufferArena
    {
        struct Block
        {
            std::shared_ptr<Page> mPage;
            AllocationRanges mRanges;
            std::uint64_t mCapacity;
            unsigned mIdleTrims = 0;
            bool mShuttingDown = false;
            Block(std::shared_ptr<Page> page, std::uint64_t capacity)
                : mPage(std::move(page))
                , mRanges(capacity)
                , mCapacity(capacity)
            {
            }
        };

    public:
        class Slice
        {
        public:
            Slice() = default;
            Slice(const Slice&) = delete;
            Slice& operator=(const Slice&) = delete;
            Slice(Slice&& other) noexcept
                : mBlock(std::move(other.mBlock))
                , mOffset(other.mOffset)
                , mSize(other.mSize)
            {
            }
            Slice& operator=(Slice&& other) noexcept
            {
                if (this != &other)
                {
                    release();
                    mBlock = std::move(other.mBlock);
                    mOffset = other.mOffset;
                    mSize = other.mSize;
                }
                return *this;
            }
            ~Slice() { release(); }
            explicit operator bool() const { return static_cast<bool>(mBlock); }
            const Page& page() const { return *mBlock->mPage; }
            std::uint64_t offset() const { return mOffset; }
            std::uint64_t size() const { return mSize; }

        private:
            friend class BufferArena;
            Slice(std::shared_ptr<Block> block, std::uint64_t offset, std::uint64_t size)
                : mBlock(std::move(block))
                , mOffset(offset)
                , mSize(size)
            {
            }
            void release() noexcept
            {
                if (mBlock && !mBlock->mShuttingDown)
                    mBlock->mRanges.release(mOffset);
            }
            std::shared_ptr<Block> mBlock;
            std::uint64_t mOffset = 0, mSize = 0;
        };

        explicit BufferArena(std::uint64_t pageSize)
            : mPageSize(pageSize)
        {
            if (!pageSize)
                throw std::invalid_argument("Buffer arena capacity must be positive");
        }

        template <class Create>
        Slice allocate(std::uint64_t size, std::uint64_t alignment, Create&& create)
        {
            if (!size || !alignment)
                throw std::invalid_argument("Buffer arena size and alignment must be positive");
            for (const auto& block : mBlocks)
                if (const auto offset = block->mRanges.allocate(size, alignment, true))
                {
                    block->mIdleTrims = 0;
                    return Slice(block, *offset, size);
                }
            const auto capacity = std::max(size, mPageSize);
            auto page = create(capacity);
            if (!page)
                throw std::runtime_error("Cannot create an empty buffer arena page");
            auto block = std::make_shared<Block>(std::move(page), capacity);
            const auto offset = block->mRanges.allocate(size, alignment);
            if (!offset)
                throw std::logic_error("A new buffer arena page cannot fit its first allocation");
            reserveFor(mBlocks, mBlocks.size() + 1);
            mBlocks.push_back(block);
            return Slice(std::move(block), *offset, size);
        }

        void trim(std::uint64_t retainBytes = 0, unsigned idleTrims = 0)
        {
            bool retained = false;
            std::uint64_t idleBytes = 0;
            std::erase_if(mBlocks, [&](const auto& block) {
                if (block.use_count() != 1)
                {
                    block->mIdleTrims = 0;
                    return false;
                }
                if (block->mCapacity == mPageSize && !retained)
                {
                    retained = true;
                    idleBytes += block->mCapacity;
                    return false;
                }
                if (block->mIdleTrims < idleTrims && idleBytes <= retainBytes
                    && block->mCapacity <= retainBytes - idleBytes)
                {
                    ++block->mIdleTrims;
                    idleBytes += block->mCapacity;
                    return false;
                }
                return true;
            });
        }
        void beginShutdown()
        {
            for (const auto& block : mBlocks)
                block->mShuttingDown = true;
        }
        void clear() { mBlocks.clear(); }

    private:
        std::uint64_t mPageSize;
        std::vector<std::shared_ptr<Block>> mBlocks;
    };
}

#endif
