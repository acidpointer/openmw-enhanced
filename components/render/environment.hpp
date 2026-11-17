#ifndef OPENMW_COMPONENTS_RENDER_ENVIRONMENT_H
#define OPENMW_COMPONENTS_RENDER_ENVIRONMENT_H

#include "backend.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace Render
{
    // Called once per world view, never per material or mesh. No allocation or
    // image sampling: weather supplies the tint, authored ambient the energy.
    inline SceneEnvironment makeOutdoorEnvironment(const std::array<double, 4>& worldHeightRow,
        const std::array<float, 3>& skyColor, float hemisphereStrength, float heightFogStrength,
        float heightFogHalfHeight, float heightFogBase, std::optional<double> reflectionHeight = {})
    {
        SceneEnvironment result;
        if (!std::all_of(worldHeightRow.begin(), worldHeightRow.end(), [](double v) { return std::isfinite(v); })
            || !std::isfinite(hemisphereStrength) || !std::isfinite(heightFogStrength)
            || !std::isfinite(heightFogHalfHeight) || heightFogHalfHeight < 1 || !std::isfinite(heightFogBase)
            || (reflectionHeight && !std::isfinite(*reflectionHeight)))
            return result;
        const double length = std::hypot(worldHeightRow[0], worldHeightRow[1], worldHeightRow[2]);
        if (length < 1e-12)
            return result;
        for (unsigned i = 0; i < 3; ++i)
            result.mWorldUp[i] = static_cast<float>(worldHeightRow[i]);
        result.mWorldUp[3] = static_cast<float>(worldHeightRow[3] - heightFogBase);
        result.mHemisphere[3] = static_cast<float>(1 / length);
        const float brightest = std::max({ skyColor[0], skyColor[1], skyColor[2] });
        for (unsigned i = 0; i < 3; ++i)
        {
            const float tint = std::isfinite(brightest) && brightest > 1e-6f && std::isfinite(skyColor[i])
                ? std::clamp(skyColor[i] / brightest, 0.f, 1.f)
                : 1.f;
            result.mHemisphere[i] = std::clamp(hemisphereStrength, 0.f, 1.f) * tint;
        }
        result.mHeightFog = { std::clamp(heightFogStrength, 0.f, 1.f), 1 / heightFogHalfHeight,
            reflectionHeight ? static_cast<float>(*reflectionHeight - heightFogBase) : 0.f,
            reflectionHeight ? 1.f : 0.f };
        const auto finite = [](const auto& values) {
            return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
        };
        return finite(result.mWorldUp) && finite(result.mHemisphere) && finite(result.mHeightFog) ? result
                                                                                                  : SceneEnvironment{};
    }
}

#endif
