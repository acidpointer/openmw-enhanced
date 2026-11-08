#include <components/misc/hostenvironment.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <map>
#include <optional>
#include <string>

#ifndef _WIN32
namespace
{
    class HostEnvironmentTest : public ::testing::Test
    {
    protected:
        std::map<std::string, std::optional<std::string>> mOriginal;

        void remember(const std::string& name)
        {
            if (!mOriginal.contains(name))
            {
                const char* original = std::getenv(name.c_str());
                mOriginal.emplace(name, original ? std::optional<std::string>(original) : std::nullopt);
            }
        }

        void SetUp() override
        {
            remember("OPENMW_ENHANCED_HOST_ENV_SAVED");
            for (const char* variable : { "LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                     "OSG_LIBRARY_PATH", "XDG_DATA_DIRS" })
            {
                remember(variable);
                const std::string saved = std::string("OPENMW_ENHANCED_HOST_") + variable;
                remember(saved);
                remember(saved + "_SET");
            }
        }

        void set(const std::string& name, const char* value)
        {
            remember(name);
            if (value)
                ASSERT_EQ(setenv(name.c_str(), value, 1), 0);
            else
                ASSERT_EQ(unsetenv(name.c_str()), 0);
        }

        void TearDown() override
        {
            for (const auto& [name, value] : mOriginal)
                if (value)
                    setenv(name.c_str(), value->c_str(), 1);
                else
                    unsetenv(name.c_str());
        }
    };

    TEST_F(HostEnvironmentTest, leavesNativeInstallEnvironmentUnchanged)
    {
        set("OPENMW_ENHANCED_HOST_ENV_SAVED", nullptr);
        set("LD_LIBRARY_PATH", "/native/libraries");
        Misc::restoreHostEnvironment();
        EXPECT_STREQ(std::getenv("LD_LIBRARY_PATH"), "/native/libraries");
    }

    TEST_F(HostEnvironmentTest, restoresExactHostValuesAndDistinguishesUnsetFromEmpty)
    {
        set("OPENMW_ENHANCED_HOST_ENV_SAVED", "1");
        set("LD_LIBRARY_PATH", "/AppDir/usr/lib:/host/lib");
        set("OPENMW_ENHANCED_HOST_LD_LIBRARY_PATH_SET", "x");
        set("OPENMW_ENHANCED_HOST_LD_LIBRARY_PATH", "/host/lib::/path with spaces");
        set("QT_PLUGIN_PATH", "/AppDir/plugins");
        set("OPENMW_ENHANCED_HOST_QT_PLUGIN_PATH_SET", "x");
        set("OPENMW_ENHANCED_HOST_QT_PLUGIN_PATH", "");
        set("OSG_LIBRARY_PATH", "/AppDir/usr/lib");
        set("OPENMW_ENHANCED_HOST_OSG_LIBRARY_PATH_SET", "");
        set("XDG_DATA_DIRS", "/AppDir/usr/share:/usr/share");
        set("OPENMW_ENHANCED_HOST_XDG_DATA_DIRS_SET", "x");
        set("OPENMW_ENHANCED_HOST_XDG_DATA_DIRS", "/usr/local/share:/usr/share");
        Misc::restoreHostEnvironment();
        EXPECT_STREQ(std::getenv("LD_LIBRARY_PATH"), "/host/lib::/path with spaces");
        EXPECT_STREQ(std::getenv("QT_PLUGIN_PATH"), "");
        EXPECT_EQ(std::getenv("OSG_LIBRARY_PATH"), nullptr);
        EXPECT_STREQ(std::getenv("XDG_DATA_DIRS"), "/usr/local/share:/usr/share");
    }

    TEST_F(HostEnvironmentTest, doesNotClearPathsForIncompleteSnapshots)
    {
        set("OPENMW_ENHANCED_HOST_ENV_SAVED", "1");
        set("LD_LIBRARY_PATH", "/keep/this/path");
        set("OPENMW_ENHANCED_HOST_LD_LIBRARY_PATH_SET", "x");
        set("OPENMW_ENHANCED_HOST_LD_LIBRARY_PATH", nullptr);
        Misc::restoreHostEnvironment();
        EXPECT_STREQ(std::getenv("LD_LIBRARY_PATH"), "/keep/this/path");
    }

    TEST_F(HostEnvironmentTest, restoresHostQtPlatformPluginOverrideRemovedByAppRun)
    {
        set("OPENMW_ENHANCED_HOST_ENV_SAVED", "1");
        set("QT_QPA_PLATFORM_PLUGIN_PATH", nullptr);
        set("OPENMW_ENHANCED_HOST_QT_QPA_PLATFORM_PLUGIN_PATH_SET", "x");
        set("OPENMW_ENHANCED_HOST_QT_QPA_PLATFORM_PLUGIN_PATH", "/host/qt/platforms");

        Misc::restoreHostEnvironment();

        EXPECT_STREQ(std::getenv("QT_QPA_PLATFORM_PLUGIN_PATH"), "/host/qt/platforms");
    }
}
#endif
