#include "hostenvironment.hpp"

#include <cstdlib>
#include <string>
#include <string_view>

namespace Misc
{
    void restoreHostEnvironment()
    {
#ifndef _WIN32
        const char* saved = std::getenv("OPENMW_ENHANCED_HOST_ENV_SAVED");
        if (!saved || std::string_view(saved) != "1")
            return;
        for (const char* name :
            { "LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "OSG_LIBRARY_PATH", "XDG_DATA_DIRS" })
        {
            const std::string key = std::string("OPENMW_ENHANCED_HOST_") + name;
            const char* state = std::getenv((key + "_SET").c_str());
            if (!state)
                continue;
            if (std::string_view(state).empty())
                unsetenv(name);
            else if (std::string_view(state) == "x")
            {
                if (const char* value = std::getenv(key.c_str()))
                    setenv(name, value, 1);
            }
        }
#endif
    }
}
