#include <components/render/environment.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace FogShader
{
    using std::abs;
    using std::clamp;
    using std::exp2;
    using std::log2;
    using std::min;

    // Exercise the actual scalar GLSL, not a second implementation of it.
#include <files/shaders/vulkan/heightfog.glsl>
}

namespace
{
    constexpr std::array<double, 4> EyeHeight{ 0, 0, 1, 4096 };
    constexpr std::array<float, 3> Sky{ .25f, .5f, 1.f };

    TEST(SceneEnvironment, HemispherePreservesMeanAuthoredAmbientAndNormalizesTheViewAxis)
    {
        const auto environment = Render::makeOutdoorEnvironment(EyeHeight, Sky, .4f, .5f, 8192, 0);
        EXPECT_EQ(environment.mHemisphere, (std::array<float, 4>{ .1f, .2f, .4f, 1.f }));
        for (unsigned i = 0; i < 3; ++i)
        {
            const float authoredAmbient = .15f;
            const float sky = authoredAmbient * (1 + environment.mHemisphere[i]);
            const float ground = authoredAmbient * (1 - environment.mHemisphere[i]);
            EXPECT_FLOAT_EQ((sky + ground) * .5f, authoredAmbient);
            EXPECT_GE(ground, 0);
        }
        const auto rotated = Render::makeOutdoorEnvironment({ 0, 2, 0, 4096 }, Sky, .4f, .5f, 8192, 0);
        EXPECT_FLOAT_EQ(rotated.mHemisphere[3], .5f);
        const auto mirrored = Render::makeOutdoorEnvironment({ 0, 0, -1, -4096 }, Sky, .4f, .5f, 8192, 0, 0);
        EXPECT_FLOAT_EQ(mirrored.mWorldUp[2], -1);
        EXPECT_EQ(mirrored.mHeightFog, (std::array<float, 4>{ .5f, 1.f / 8192, 0, 1 }));
    }

    TEST(SceneEnvironment, DisabledViewsAndZeroStrengthKeepLegacyLightingAndFog)
    {
        const Render::SceneEnvironment disabled;
        EXPECT_EQ(disabled.mHemisphere, (std::array<float, 4>{}));
        EXPECT_EQ(disabled.mHeightFog, (std::array<float, 4>{}));
        const auto environment = Render::makeOutdoorEnvironment(EyeHeight, Sky, 0, 0, 8192, 1000);
        EXPECT_EQ(environment.mHemisphere, (std::array<float, 4>{ 0, 0, 0, 1 }));
        EXPECT_FLOAT_EQ(environment.mHeightFog[0], 0);
        EXPECT_FLOAT_EQ(environment.mWorldUp[3], 3096);
        for (float factor : { 0.f, .2f, .5f, .999f, 1.f })
            EXPECT_EQ(FogShader::nativeHeightFogAmount(factor, -8, 12, 0, false, 0), factor);
    }

    TEST(SceneEnvironment, InvalidCameraOrFogParametersDisableTheEnvironmentSafely)
    {
        const auto invalid = Render::makeOutdoorEnvironment(EyeHeight, Sky, .4f, .5f, 0, 0);
        EXPECT_EQ(invalid.mHeightFog[0], 0);
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        const auto invalidEye = Render::makeOutdoorEnvironment({ 0, 0, 1, nan }, Sky, .4f, .5f, 8192, 0);
        EXPECT_EQ(invalidEye.mHemisphere[0], 0);
        const auto darkSky = Render::makeOutdoorEnvironment(EyeHeight, {}, 2, 2, 8192, 0);
        EXPECT_EQ(darkSky.mHemisphere, (std::array<float, 4>{ 1, 1, 1, 1 }));
        EXPECT_FLOAT_EQ(darkSky.mHeightFog[0], 1);
    }

    double integratedDensity(double start, double end, bool reflection = false, double plane = 0)
    {
        constexpr unsigned Samples = 16384;
        double sum = 0;
        for (unsigned i = 0; i < Samples; ++i)
        {
            double height = start + (end - start) * ((i + .5) / Samples);
            if (reflection)
                height = plane + std::abs(height - plane);
            sum += std::exp2(-height);
        }
        return sum / Samples;
    }

    TEST(HeightFogShader, AnalyticDensityMatchesNumericalIntegrationIncludingTheHorizon)
    {
        for (const auto& [start, end] :
            std::array{ std::pair{ 0.f, 0.f }, std::pair{ 2.f, 2.000001f }, std::pair{ -2.f, 6.f },
                std::pair{ 6.f, -2.f }, std::pair{ 0.f, .0015f }, std::pair{ 10.f, 12.f }, std::pair{ -4.f, -4.f } })
        {
            const double reference = integratedDensity(start, end);
            EXPECT_NEAR(FogShader::nativeAverageFogDensity(start, end), reference, reference * 2e-4)
                << start << ", " << end;
        }
    }

    TEST(HeightFogShader, ReflectionsIntegrateTheFoldedPhysicalRay)
    {
        for (const auto& [start, end] : std::array{ std::pair{ -1.f, 2.f }, std::pair{ 2.f, -1.f },
                 std::pair{ -3.f, -2.f }, std::pair{ 0.f, 2.f }, std::pair{ 2.f, 3.f } })
        {
            constexpr float Plane = .25f, Strength = .5f, DistanceFog = .6f;
            const double density = std::clamp(integratedDensity(start, end, true, Plane), .125, 8.);
            const double clearing = density < 1 ? 1 - std::pow(double(DistanceFog), 4) : 1;
            const double expected = 1 - std::pow(1 - double(DistanceFog), 1 + Strength * (density - 1) * clearing);
            EXPECT_NEAR(
                FogShader::nativeHeightFogAmount(DistanceFog, start, end, Plane, true, Strength), expected, 2e-4);
        }
    }

    TEST(HeightFogShader, ExtremeHeightsStayFiniteAndFarEndpointRemainsFullyFogged)
    {
        for (float height : { -1000000.f, -80.f, 0.f, 80.f, 1000000.f })
        {
            EXPECT_EQ(FogShader::nativeHeightFogAmount(0, height, height, 0, false, 1), 0);
            EXPECT_EQ(FogShader::nativeHeightFogAmount(1, height, height, 0, false, 1), 1);
            float previous = 0;
            for (float factor : { .001f, .2f, .5f, .9f, .999f })
            {
                const float fog = FogShader::nativeHeightFogAmount(factor, height, height, 0, false, 1);
                EXPECT_TRUE(std::isfinite(fog));
                EXPECT_GE(fog, previous);
                EXPECT_LE(fog, 1);
                previous = fog;
            }
            EXPECT_GT(FogShader::nativeHeightFogAmount(.999f, height, height, 0, false, 1), .998f);
        }
    }
}
