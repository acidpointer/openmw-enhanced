#ifndef OPENMW_COMPONENTS_FX_SCRATCHROUTING_H
#define OPENMW_COMPONENTS_FX_SCRATCHROUTING_H

#include <algorithm>
#include <array>

namespace Fx
{
    class ScratchRouting
    {
    public:
        // Negative means the original scene, which must never be overwritten.
        int lastShader() const { return mLastShader; }
        int lastPass() const { return mLastPass; }

        int selectOutput(bool completesTechnique)
        {
            const int result = mAvailable[0];
            mLastPass = result;
            if (completesTechnique)
            {
                mLastShader = result;
                unsigned int i = 0;
                for (int slot = 0; slot < 3; ++slot)
                    if (slot != result)
                        mAvailable[i++] = slot;
            }
            else
                std::swap(mAvailable[0], mAvailable[1]);
            return result;
        }

    private:
        int mLastShader = -1;
        int mLastPass = -1;
        std::array<int, 2> mAvailable{ 1, 2 };
    };
}

#endif
