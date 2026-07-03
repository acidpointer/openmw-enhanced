#include "enhancedsettings.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <exception>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

#include <components/debug/debuglog.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/settings/parser.hpp>

namespace SceneUtil::Enhanced
{
    namespace
    {
        std::mutex sMutex;
        Settings::CategorySettingValueMap sSettings;
        constexpr char sSettingsFileName[] = "openmw-enhanced.cfg";

        std::string normalizeBoolText(std::string value)
        {
            value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                           [](unsigned char ch) { return !std::isspace(ch); }));
            value.erase(std::find_if(value.rbegin(), value.rend(),
                            [](unsigned char ch) { return !std::isspace(ch); })
                            .base(),
                value.end());

            for (char& ch : value)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

            return value;
        }

        const std::string* findSetting(std::string_view category, std::string_view setting)
        {
            const auto it = sSettings.find(std::make_pair(category, setting));
            if (it == sSettings.end())
                return nullptr;
            return &it->second;
        }

        void loadSettingsFile(Settings::SettingsFileParser& parser, const std::filesystem::path& file)
        {
            if (!std::filesystem::exists(file))
                return;

            parser.loadSettingsFile(file, sSettings, false, true);
        }

        std::filesystem::path findDefaultSettingsFile(const Files::ConfigurationManager& cfgMgr)
        {
            const std::filesystem::path userFile = cfgMgr.getUserConfigPath() / sSettingsFileName;
            for (const std::filesystem::path& path : cfgMgr.getActiveConfigPaths())
            {
                std::filesystem::path file = path / sSettingsFileName;
                if (file == userFile)
                    continue;
                if (std::filesystem::is_regular_file(file))
                    return file;
            }
            return {};
        }

        void writeCommentedTemplate(std::ostream& stream, std::istream& defaults)
        {
            stream << "# This is the OpenMW Enhanced user '" << sSettingsFileName << "' file.\n";
            stream << "# It only needs settings you explicitly override.\n";
            stream << "# Uncomment a setting below to override the bundled fork default.\n\n";

            std::string line;
            while (std::getline(defaults, line))
            {
                std::string trimmed = line;
                trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(),
                                                 [](unsigned char ch) { return !std::isspace(ch); }));

                if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == '[')
                    stream << line << '\n';
                else
                    stream << "# " << line << '\n';
            }
        }
    }

    void ensureUserSettingsFile(const Files::ConfigurationManager& cfgMgr)
    {
        const std::filesystem::path userFile = cfgMgr.getUserConfigPath() / sSettingsFileName;
        if (std::filesystem::exists(userFile))
            return;

        std::error_code ec;
        std::filesystem::create_directories(userFile.parent_path(), ec);
        if (ec)
        {
            Log(Debug::Warning) << "Could not create OpenMW Enhanced user config directory: " << userFile.parent_path()
                                << " (" << ec.message() << ")";
            return;
        }

        const std::filesystem::path defaultFile = findDefaultSettingsFile(cfgMgr);
        std::ifstream input(defaultFile);
        std::ofstream output(userFile);
        if (!output.is_open())
        {
            Log(Debug::Warning) << "Could not create OpenMW Enhanced user config file: " << userFile;
            return;
        }

        if (input.is_open())
            writeCommentedTemplate(output, input);
        else
        {
            output << "# This is the OpenMW Enhanced user '" << sSettingsFileName << "' file.\n";
            output << "# Add fork-specific setting overrides here.\n";
        }

        Log(Debug::Info) << "Created OpenMW Enhanced user config file: " << userFile;
    }

    void loadSettings(const Files::ConfigurationManager& cfgMgr)
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sSettings.clear();

        Settings::SettingsFileParser parser;
        for (const std::filesystem::path& path : cfgMgr.getActiveConfigPaths())
        {
            loadSettingsFile(parser, path / sSettingsFileName);
        }

        if (const char* file = std::getenv("OPENMW_ENHANCED_CONFIG_FILE"))
        {
            if (*file != '\0')
                loadSettingsFile(parser, std::filesystem::path(file));
        }

        if (const char* dir = std::getenv("OPENMW_ENHANCED_CONFIG_DIR"))
        {
            if (*dir != '\0')
                loadSettingsFile(parser, std::filesystem::path(dir) / sSettingsFileName);
        }
    }

    bool settingBool(std::string_view category, std::string_view setting, bool defaultValue)
    {
        std::lock_guard<std::mutex> lock(sMutex);
        const std::string* value = findSetting(category, setting);
        if (!value)
            return defaultValue;

        const std::string text = normalizeBoolText(*value);
        if (text == "1" || text == "true" || text == "yes" || text == "on")
            return true;
        if (text == "0" || text == "false" || text == "no" || text == "off")
            return false;

        Log(Debug::Warning) << "Invalid OpenMW Enhanced boolean setting [" << category << "] " << setting << " = "
                            << *value;
        return defaultValue;
    }

    int settingInt(std::string_view category, std::string_view setting, int defaultValue)
    {
        std::lock_guard<std::mutex> lock(sMutex);
        const std::string* value = findSetting(category, setting);
        if (!value || value->empty())
            return defaultValue;

        int parsed = 0;
        const char* begin = value->data();
        const char* end = begin + value->size();
        const auto result = std::from_chars(begin, end, parsed);
        if (result.ec != std::errc() || result.ptr != end)
        {
            Log(Debug::Warning) << "Invalid OpenMW Enhanced integer setting [" << category << "] " << setting << " = "
                                << *value;
            return defaultValue;
        }

        return parsed;
    }

    double settingDouble(std::string_view category, std::string_view setting, double defaultValue)
    {
        std::lock_guard<std::mutex> lock(sMutex);
        const std::string* value = findSetting(category, setting);
        if (!value || value->empty())
            return defaultValue;

        try
        {
            std::size_t consumed = 0;
            const double parsed = std::stod(*value, &consumed);
            if (consumed == value->size())
                return parsed;
        }
        catch (const std::exception&)
        {
        }

        Log(Debug::Warning) << "Invalid OpenMW Enhanced numeric setting [" << category << "] " << setting << " = "
                            << *value;
        return defaultValue;
    }

    std::string settingString(std::string_view category, std::string_view setting, std::string_view defaultValue)
    {
        std::lock_guard<std::mutex> lock(sMutex);
        const std::string* value = findSetting(category, setting);
        if (!value)
            return std::string(defaultValue);
        return *value;
    }
}
