#ifndef OPENMW_COMPONENTS_SCENEUTIL_FADE_H
#define OPENMW_COMPONENTS_SCENEUTIL_FADE_H

#include <algorithm>

namespace SceneUtil
{
    constexpr float smoothFade(float from, float to, float progress)
    {
        const float t = std::clamp(progress, 0.f, 1.f);
        return from + (to - from) * t * t * (3.f - 2.f * t);
    }
}

#endif
