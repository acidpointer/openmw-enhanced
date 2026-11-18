struct PbrEnvironmentBrdf
{
    float scale;
    float bias;
};

float pbrDefaultDielectricWeight(float roughness, float metalness, bool hasPropertiesMap)
{
    return roughness >= 0.0f || metalness > 0.0f || hasPropertiesMap ? 1.0f : 0.0f;
}

float pbrTerrainDielectricWeight(float defaultWeight, float bakedWeight)
{
    // A negative weight means the composite is absent. Zero is baked matte terrain.
    return bakedWeight >= 0.0f ? bakedWeight : defaultWeight;
}

// Karis's analytic fit to the integrated GGX environment BRDF. The input
// roughness is perceptual (sqrt of the microfacet alpha). Scale weights F0,
// while bias weights F90, so legacy masks affect the whole reflected lobe.
PbrEnvironmentBrdf pbrEnvironmentBrdf(float roughness, float ndotv)
{
    float smoothness = 1.0f - roughness;
    float grazing = min(smoothness * smoothness, exp2(-9.28f * ndotv));
    float fit = 1.04f * (grazing * smoothness + 0.0425f - 0.0275f * roughness);
    return PbrEnvironmentBrdf(1.04f - 0.572f * roughness - fit, 0.022f * roughness - 0.04f + fit);
}
