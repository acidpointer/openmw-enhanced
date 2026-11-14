#ifndef OPENMW_COMPONENTS_RENDER_RESERVE_H
#define OPENMW_COMPONENTS_RENDER_RESERVE_H

#include <algorithm>
#include <vector>

namespace Render
{
    template <class T>
    void reserveFor(std::vector<T>& values, std::size_t required)
    {
        if (required <= values.capacity())
            return;
        const auto maximum = values.max_size();
        const auto doubled = values.capacity() > maximum / 2 ? maximum : values.capacity() * 2;
        values.reserve(std::max(required, std::min(maximum, std::max(std::size_t{ 64 }, doubled))));
    }
}

#endif
