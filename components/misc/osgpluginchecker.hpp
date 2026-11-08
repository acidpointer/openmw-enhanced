#ifndef OPENMW_COMPONENTS_MISC_OSGPLUGINCHECKER_HPP
#define OPENMW_COMPONENTS_MISC_OSGPLUGINCHECKER_HPP

namespace Misc
{
    // Apply AppRun's bundled-only OSG search list before any plugin is loaded.
    // Native installs retain OSG's normal environment and platform search paths.
    void configureOSGPluginPaths();

    bool checkRequiredOSGPluginsArePresent();
}

#endif
