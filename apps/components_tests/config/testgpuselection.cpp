#include <components/config/gpuselection.hpp>

#include <gtest/gtest.h>

namespace
{
    QProcessEnvironment inheritedEnvironment()
    {
        QProcessEnvironment environment;
        environment.insert(QStringLiteral("LD_LIBRARY_PATH"), QStringLiteral("/AppDir/usr/lib"));
        environment.insert(QStringLiteral("OSG_LIBRARY_PATH"), QStringLiteral("/AppDir/usr/lib"));
        environment.insert(QStringLiteral("DRI_PRIME"), QStringLiteral("1"));
        environment.insert(QStringLiteral("__NV_PRIME_RENDER_OFFLOAD"), QStringLiteral("1"));
        environment.insert(QStringLiteral("__NV_PRIME_RENDER_OFFLOAD_PROVIDER"), QStringLiteral("NVIDIA-G0"));
        environment.insert(QStringLiteral("__GLX_VENDOR_LIBRARY_NAME"), QStringLiteral("nvidia"));
        return environment;
    }

    TEST(RenderGpuTest, automaticAndUnavailablePreserveInheritedEnvironment)
    {
        const auto environment = inheritedEnvironment();
        EXPECT_EQ(Config::renderGpuEnvironment({}, environment), environment);
        EXPECT_EQ(Config::renderGpuEnvironment({ "0000:03:00.0", "GPU", "nvidia", false }, environment), environment);
    }

    TEST(RenderGpuTest, mesaUsesStablePciAddressAndRemovesNvidiaOverrides)
    {
        const auto environment
            = Config::renderGpuEnvironment({ "0000:00:02.0", "Intel GPU", "i915" }, inheritedEnvironment());
        EXPECT_EQ(environment.value("DRI_PRIME"), QStringLiteral("pci-0000_00_02_0"));
        EXPECT_EQ(environment.value("OPENMW_RENDER_GPU"), QStringLiteral("0000:00:02.0"));
        EXPECT_FALSE(environment.contains("__NV_PRIME_RENDER_OFFLOAD"));
        EXPECT_FALSE(environment.contains("__NV_PRIME_RENDER_OFFLOAD_PROVIDER"));
        EXPECT_EQ(environment.value("__GLX_VENDOR_LIBRARY_NAME"), QStringLiteral("mesa"));
        EXPECT_EQ(environment.value("LD_LIBRARY_PATH"), QStringLiteral("/AppDir/usr/lib"));
        EXPECT_EQ(environment.value("OSG_LIBRARY_PATH"), QStringLiteral("/AppDir/usr/lib"));
    }

    TEST(RenderGpuTest, nvidiaOffloadRemovesMesaAndProviderOverrides)
    {
        const auto environment
            = Config::renderGpuEnvironment({ "0000:03:00.0", "NVIDIA GPU", "nvidia" }, inheritedEnvironment());
        EXPECT_EQ(environment.value("__NV_PRIME_RENDER_OFFLOAD"), QStringLiteral("1"));
        EXPECT_EQ(environment.value("__GLX_VENDOR_LIBRARY_NAME"), QStringLiteral("nvidia"));
        EXPECT_FALSE(environment.contains("DRI_PRIME"));
        EXPECT_FALSE(environment.contains("__NV_PRIME_RENDER_OFFLOAD_PROVIDER"));
        EXPECT_EQ(environment.value("LD_LIBRARY_PATH"), QStringLiteral("/AppDir/usr/lib"));
        EXPECT_EQ(environment.value("OSG_LIBRARY_PATH"), QStringLiteral("/AppDir/usr/lib"));
    }
}
