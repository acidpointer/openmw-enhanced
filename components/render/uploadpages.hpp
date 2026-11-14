#ifndef OPENMW_COMPONENTS_RENDER_UPLOADPAGES_H
#define OPENMW_COMPONENTS_RENDER_UPLOADPAGES_H

#include "reserve.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace Render
{
    // Upload slices retain their page through pending work and GPU completion.
    // The pool can reset a cursor only when no outstanding slice owns the page.
    template <class Page>
    class UploadPages
    {
    public:
        struct Slice
        {
            std::shared_ptr<Page> mPage;
            std::uint64_t mOffset = 0;
            explicit operator bool() const { return static_cast<bool>(mPage); }
        };

        explicit UploadPages(std::uint64_t pageSize)
            : mPageSize(pageSize)
        {
            if (!pageSize)
                throw std::invalid_argument("Upload page capacity must be positive");
        }

        template <class Create>
        Slice allocate(std::uint64_t size, std::uint64_t alignment, Create&& create)
        {
            if (!size || !alignment)
                throw std::invalid_argument("Upload size and alignment must be positive");
            for (auto& page : mPages)
            {
                if (page.mValue.use_count() == 1)
                    page.mUsed = 0;
                const auto padding = (alignment - page.mUsed % alignment) % alignment;
                const auto available = page.mCapacity - page.mUsed;
                if (padding > available || size > available - padding)
                    continue;
                const auto offset = page.mUsed + padding;
                page.mUsed = offset + size;
                return { page.mValue, offset };
            }
            const auto capacity = std::max(size, mPageSize);
            auto value = create(capacity);
            if (!value)
                throw std::runtime_error("Cannot create an empty upload page");
            reserveFor(mPages, mPages.size() + 1);
            mPages.push_back({ value, capacity, size });
            return { std::move(value), 0 };
        }

        void trim()
        {
            bool retained = false;
            std::erase_if(mPages, [&](const auto& page) {
                if (page.mValue.use_count() != 1)
                    return false;
                if (page.mCapacity == mPageSize && !retained)
                {
                    retained = true;
                    return false;
                }
                return true;
            });
        }

        void clear() { mPages.clear(); }

    private:
        struct Entry
        {
            std::shared_ptr<Page> mValue;
            std::uint64_t mCapacity;
            std::uint64_t mUsed;
        };
        std::uint64_t mPageSize;
        std::vector<Entry> mPages;
    };
}

#endif
