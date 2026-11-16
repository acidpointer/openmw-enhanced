#ifndef OPENMW_COMPONENTS_RENDER_BUFFERVERSIONS_H
#define OPENMW_COMPONENTS_RENDER_BUFFERVERSIONS_H

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Render
{
    // Single rendering-thread owner of reusable buffer versions. Submitted frames
    // lease a version until their fence signals. Even a pose reused for several
    // frames must stay immutable until the last of those leases retires.
    template <class Buffer>
    class BufferVersions
    {
    public:
        BufferVersions() = default;
        BufferVersions(const BufferVersions&) = delete;
        BufferVersions& operator=(const BufferVersions&) = delete;
        BufferVersions(BufferVersions&&) = default;
        BufferVersions& operator=(BufferVersions&&) = default;

        const std::shared_ptr<Buffer>& current() const { return mVersions.at(mCurrent); }

        template <class Create>
        const std::shared_ptr<Buffer>& writable(Create&& create)
        {
            if (!mVersions.empty() && current().use_count() == 1)
                return current();
            for (std::size_t i = 0; i < mVersions.size(); ++i)
                if (mVersions[i].use_count() == 1)
                {
                    mCurrent = i;
                    return current();
                }
            auto buffer = std::forward<Create>(create)();
            if (!buffer)
                throw std::invalid_argument("Buffer version factory returned null");
            mVersions.push_back(std::move(buffer));
            mCurrent = mVersions.size() - 1;
            return current();
        }

        std::size_t size() const { return mVersions.size(); }

    private:
        std::vector<std::shared_ptr<Buffer>> mVersions;
        std::size_t mCurrent = 0;
    };
}

#endif
