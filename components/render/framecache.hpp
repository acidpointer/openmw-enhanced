#ifndef OPENMW_COMPONENTS_RENDER_FRAMECACHE_H
#define OPENMW_COMPONENTS_RENDER_FRAMECACHE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Render
{
    // Frame-local memoization with retained flat storage. Reset changes an
    // epoch, not keys/values or capacity. A new entry's value may contain data
    // from a previous frame; callers initialize it when inserted is true.
    // References remain valid until an insertion grows the backing table.
    template <class Key, class Value, class Hash = std::hash<Key>>
    class FrameCache
    {
    public:
        constexpr void beginFrame()
        {
            mSize = 0;
            if (++mEpoch == 0)
            {
                for (auto& entry : mEntries)
                    entry.mEpoch = 0;
                mEpoch = 1;
            }
        }
        constexpr std::pair<Value&, bool> getOrCreate(const Key& key)
        {
            if (mEntries.empty())
                grow();
            for (;;)
            {
                auto slot = mHash(key) & (mEntries.size() - 1);
                while (mEntries[slot].mEpoch == mEpoch)
                {
                    if (mEntries[slot].mKey == key)
                        return { mEntries[slot].mValue, false };
                    slot = (slot + 1) & (mEntries.size() - 1);
                }
                if (mSize >= mEntries.size() / 2)
                {
                    grow();
                    continue;
                }
                auto& entry = mEntries[slot];
                entry.mKey = key;
                entry.mEpoch = mEpoch;
                ++mSize;
                return { entry.mValue, true };
            }
        }
        constexpr const Value* find(const Key& key) const
        {
            if (mEntries.empty())
                return nullptr;
            auto slot = mHash(key) & (mEntries.size() - 1);
            while (mEntries[slot].mEpoch == mEpoch)
            {
                if (mEntries[slot].mKey == key)
                    return &mEntries[slot].mValue;
                slot = (slot + 1) & (mEntries.size() - 1);
            }
            return nullptr;
        }
        constexpr std::size_t size() const { return mSize; }
        constexpr std::size_t capacity() const { return mEntries.size() / 2; }

    private:
        struct Entry
        {
            std::uint64_t mEpoch = 0;
            Key mKey{};
            Value mValue{};
        };
        constexpr void grow()
        {
            if (mEntries.size() > mEntries.max_size() / 2)
                throw std::length_error("Frame cache exceeds host capacity");
            std::vector<Entry> replacement(mEntries.empty() ? 64 : mEntries.size() * 2);
            for (auto& entry : mEntries)
            {
                if (entry.mEpoch != mEpoch)
                    continue;
                auto slot = mHash(entry.mKey) & (replacement.size() - 1);
                while (replacement[slot].mEpoch == mEpoch)
                    slot = (slot + 1) & (replacement.size() - 1);
                replacement[slot] = std::move(entry);
            }
            mEntries.swap(replacement);
        }
        Hash mHash{};
        std::vector<Entry> mEntries;
        std::size_t mSize = 0;
        std::uint64_t mEpoch = 1;
    };
}

#endif
