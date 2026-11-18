#ifndef OPENMW_COMPONENTS_RENDER_RESOURCEPOOL_H
#define OPENMW_COMPONENTS_RENDER_RESOURCEPOOL_H

#include "backend.hpp"

#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Render
{
    // Constant-time lookup without hashing on the draw path. Reusing a slot
    // changes its generation, so an old scene update cannot name a new asset.
    template <class T, class Tag>
    class ResourcePool
    {
    public:
        using Id = Handle<Tag>;

        Id insert(std::shared_ptr<T> value)
        {
            if (!value)
                throw std::invalid_argument("Cannot register a null rendering resource");
            std::uint32_t index = mFree;
            if (index == Invalid)
            {
                if (mSlots.size() >= Invalid)
                    throw std::length_error("Rendering resource handle space exhausted");
                index = static_cast<std::uint32_t>(mSlots.size());
                mSlots.emplace_back();
            }
            else
                mFree = mSlots[index].mNext;
            Slot& slot = mSlots[index];
            slot.mValue = std::move(value);
            return { index, slot.mGeneration };
        }

        const std::shared_ptr<T>& get(Id id, const char* error = "Invalid or expired rendering resource handle") const
        {
            if (id.mIndex >= mSlots.size() || id.mGeneration != mSlots[id.mIndex].mGeneration
                || !mSlots[id.mIndex].mValue)
                throw std::invalid_argument(error);
            return mSlots[id.mIndex].mValue;
        }

        void erase(Id id)
        {
            get(id);
            Slot& slot = mSlots[id.mIndex];
            slot.mValue.reset();
            // Retire an exhausted slot permanently rather than reviving an
            // ancient handle by wrapping the generation counter.
            if (slot.mGeneration != std::numeric_limits<std::uint32_t>::max())
            {
                ++slot.mGeneration;
                slot.mNext = mFree;
                mFree = id.mIndex;
            }
        }

    private:
        static constexpr std::uint32_t Invalid = ~std::uint32_t{ 0 };
        struct Slot
        {
            std::shared_ptr<T> mValue;
            std::uint32_t mGeneration = 1;
            std::uint32_t mNext = Invalid;
        };
        std::vector<Slot> mSlots;
        std::uint32_t mFree = Invalid;
    };
}

#endif
