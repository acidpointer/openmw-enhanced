#ifndef OPENMW_COMPONENTS_RENDER_DRAWRANGES_H
#define OPENMW_COMPONENTS_RENDER_DRAWRANGES_H

#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Render
{
    // Map request ranges onto emitted draws. Rejected/empty geometry emits no
    // draws, so a first-person depth clear belongs to the first surviving draw.
    // Range nesting and original request order are preserved.
    class DrawRanges
    {
    public:
        constexpr void clear() { mDepthClears.clear(); }
        constexpr void clearDepth(std::size_t begin, std::size_t end) { mDepthClears.emplace_back(begin, end); }
        constexpr void begin(std::size_t requests, std::size_t firstDraw = 0)
        {
            if (requests >= mOffsets.max_size())
                throw std::length_error("Draw request ranges exceed host capacity");
            mOffsets.resize(requests + 1);
            mOffsets[0] = firstDraw;
        }
        constexpr void emitted(std::size_t request, std::size_t drawCount) { mOffsets[request + 1] = drawCount; }
        template <class Mark>
        constexpr void apply(Mark&& mark) const
        {
            for (const auto& [begin, end] : mDepthClears)
                if (mOffsets[begin] < mOffsets[end])
                    mark(mOffsets[begin]);
        }

    private:
        std::vector<std::pair<std::size_t, std::size_t>> mDepthClears;
        std::vector<std::size_t> mOffsets;
    };
}

#endif
