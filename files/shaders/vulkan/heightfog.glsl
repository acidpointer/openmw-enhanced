// Scalar shader math is also included by the CPU regression fixture, which
// compares this exact implementation against numerical integration.
// Heights are relative to the fog base and measured in density half-heights.
float nativeAverageFogDensity(float startHeight, float endHeight)
{
    float difference = abs(endHeight - startHeight);
    float slope = 0.6931471805599453f * difference;
    // Stable at the horizon. The lower endpoint avoids multiplying an
    // underflowing exponential by an overflowing exponential uphill.
    float integral = slope < 0.001f
        ? 1.0f - slope * 0.5f + slope * slope / 6.0f
        : (1.0f - exp2(-difference)) / slope;
    return exp2(clamp(-min(startHeight, endHeight), -80.0f, 80.0f)) * integral;
}

float nativeHeightFogAmount(float distanceFog, float startHeight, float endHeight,
    float mirrorHeight, bool reflection, float strength)
{
    if (strength <= 0.0f || distanceFog <= 0.0f || distanceFog >= 1.0f)
        return distanceFog;
    float density;
    if (reflection)
    {
        // Fold the virtual ray around the water plane to integrate along the
        // physical path, rather than through fictitious underground fog.
        float a = startHeight - mirrorHeight;
        float b = endHeight - mirrorHeight;
        if ((a < 0.0f) != (b < 0.0f))
        {
            float split = abs(a) / (abs(a) + abs(b));
            density = split * nativeAverageFogDensity(mirrorHeight + abs(a), mirrorHeight)
                + (1.0f - split) * nativeAverageFogDensity(mirrorHeight, mirrorHeight + abs(b));
        }
        else
            density = nativeAverageFogDensity(mirrorHeight + abs(a), mirrorHeight + abs(b));
    }
    else
        density = nativeAverageFogDensity(startHeight, endHeight);
    // Bound artistic redistribution, retaining weather's start/end and a
    // strictly positive density so the far clipping plane stays hidden.
    float redistribution = clamp(density, 0.125f, 8.0f) - 1.0f;
    if (redistribution < 0.0f)
    {
        // Fade clearing back to weather's fog near the far endpoint. Even a
        // high-altitude view must not expose a sharp geometric cutoff.
        float squared = distanceFog * distanceFog;
        redistribution *= 1.0f - squared * squared;
    }
    float scale = 1.0f + strength * redistribution;
    return 1.0f - exp2(log2(1.0f - distanceFog) * scale);
}
