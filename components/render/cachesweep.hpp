#ifndef OPENMW_COMPONENTS_RENDER_CACHESWEEP_H
#define OPENMW_COMPONENTS_RENDER_CACHESWEEP_H

#include <cstddef>
#include <optional>

namespace Render
{
    // A key cursor survives insertions, replacements and erasures between
    // sweeps. The caller supplies a time budget and the expiration policy.
    template <class Map>
    class CacheSweep
    {
    public:
        template <class Expired, class Stop>
        std::size_t sweep(Map& cache, Expired&& expired, Stop&& stop)
        {
            auto next = mAfter ? cache.upper_bound(*mAfter) : cache.begin();
            std::size_t removed = 0;
            while (next != cache.end() && !stop())
            {
                mAfter = next->first;
                if (expired(*next))
                {
                    next = cache.erase(next);
                    ++removed;
                }
                else
                    ++next;
            }
            if (next == cache.end())
                mAfter.reset();
            return removed;
        }

    private:
        std::optional<typename Map::key_type> mAfter;
    };
}

#endif
