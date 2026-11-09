#ifndef OPENMW_COMPONENTS_FX_EXECUTIONPLAN_H
#define OPENMW_COMPONENTS_FX_EXECUTIONPLAN_H

#include "technique.hpp"

namespace Fx
{
    struct ExecutionPlan
    {
        FlagsType mMask = 0;
        bool mEnabled = false;
        bool mHDR = false;
        bool mNormals = false;
        bool mLights = false;
        std::vector<std::size_t> mNodes;
    };

    inline ExecutionPlan makeExecutionPlan(const DispatchArray& nodes, FlagsType mask, bool enabled)
    {
        ExecutionPlan result;
        result.mMask = mask;
        result.mEnabled = enabled;
        if (!enabled)
            return result;
        result.mNodes.reserve(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            const auto& node = nodes[i];
            if ((node.mFlags & mask) || node.mPasses.empty())
                continue;
            result.mNodes.push_back(i);
            result.mHDR |= node.mHDR;
            result.mNormals |= node.mNormals;
            result.mLights |= node.mLights;
        }
        return result;
    }
}

#endif
