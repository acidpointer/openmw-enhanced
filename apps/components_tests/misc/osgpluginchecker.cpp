#include <components/misc/osgpluginchecker.hpp>
#include <components/testing/util.hpp>

#include <osgDB/FileUtils>
#include <osgDB/Registry>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>

#ifdef __linux__
namespace
{
    class OSGPluginPathTest : public ::testing::Test
    {
    protected:
        osgDB::FilePathList mOriginalPaths;
        std::map<std::string, std::optional<std::string>> mEnvironment;
        std::filesystem::path mRoot;

        void SetUp() override
        {
            mOriginalPaths = osgDB::Registry::instance()->getLibraryFilePathList();
            for (const char* key : { "OPENMW_ENHANCED_APPIMAGE", "OPENMW_ENHANCED_OSG_LIBRARY_PATH" })
            {
                const char* value = std::getenv(key);
                mEnvironment.emplace(key, value ? std::optional<std::string>(value) : std::nullopt);
            }
            mRoot = TestingOpenMW::currentTestDirPath();
        }

        void TearDown() override
        {
            osgDB::Registry::instance()->setLibraryFilePathList(mOriginalPaths);
            for (const auto& [key, value] : mEnvironment)
                if (value)
                    setenv(key.c_str(), value->c_str(), 1);
                else
                    unsetenv(key.c_str());
            std::error_code ignored;
            std::filesystem::remove_all(mRoot, ignored);
        }

        void touch(const std::filesystem::path& path)
        {
            std::filesystem::create_directories(path.parent_path());
            ASSERT_TRUE(std::ofstream(path).good());
        }

        void configure(const std::string& paths)
        {
            ASSERT_EQ(setenv("OPENMW_ENHANCED_APPIMAGE", "1", 1), 0);
            ASSERT_EQ(setenv("OPENMW_ENHANCED_OSG_LIBRARY_PATH", paths.c_str(), 1), 0);
            Misc::configureOSGPluginPaths();
        }
    };

    TEST_F(OSGPluginPathTest, missingBundledPluginDoesNotFallBackToHostSearchDirectories)
    {
        const std::string library = "osgPlugins-test/osgdb_openmw_missing.so";
        const auto host = mRoot / "host";
        const auto bundle = mRoot / "bundle";
        std::filesystem::create_directories(bundle);
        touch(host / library);
        osgDB::Registry::instance()->setLibraryFilePathList(osgDB::FilePathList{ bundle.string(), host.string() });
        ASSERT_FALSE(osgDB::findLibraryFile(library).empty());

        configure(bundle.string());

        EXPECT_TRUE(osgDB::findLibraryFile(library).empty());
        EXPECT_TRUE(std::filesystem::exists(host / library));
    }

    TEST_F(OSGPluginPathTest, bundledVersionedAndBareNamesResolveToTheSamePlugin)
    {
        const auto bundle = mRoot / "bundle";
        const auto directory = bundle / "osgPlugins-test";
        const auto alias = bundle / "openmw-osg-plugins";
        const std::string plugin = "osgdb_openmw_test.so";
        touch(directory / plugin);
        std::filesystem::create_directory_symlink(directory.filename(), alias);

        configure(alias.string() + ":" + bundle.string());

        const auto versioned = osgDB::findLibraryFile("osgPlugins-test/" + plugin);
        const auto bare = osgDB::findLibraryFile(plugin);
        ASSERT_FALSE(versioned.empty());
        ASSERT_FALSE(bare.empty());
        EXPECT_TRUE(std::filesystem::equivalent(versioned, bare));
    }

    TEST_F(OSGPluginPathTest, nativeInstallRetainsItsPluginSearchPaths)
    {
        ASSERT_EQ(unsetenv("OPENMW_ENHANCED_APPIMAGE"), 0);
        ASSERT_EQ(setenv("OPENMW_ENHANCED_OSG_LIBRARY_PATH", "/unused/bundle", 1), 0);

        Misc::configureOSGPluginPaths();

        EXPECT_EQ(osgDB::Registry::instance()->getLibraryFilePathList(), mOriginalPaths);
    }

    TEST_F(OSGPluginPathTest, incompleteAppImageEnvironmentRetainsItsPluginSearchPaths)
    {
        ASSERT_EQ(setenv("OPENMW_ENHANCED_APPIMAGE", "1", 1), 0);
        ASSERT_EQ(unsetenv("OPENMW_ENHANCED_OSG_LIBRARY_PATH"), 0);

        Misc::configureOSGPluginPaths();

        EXPECT_EQ(osgDB::Registry::instance()->getLibraryFilePathList(), mOriginalPaths);
    }
}
#endif
