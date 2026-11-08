#ifndef OPENMW_COMPONENTS_MISC_HOSTENVIRONMENT_H
#define OPENMW_COMPONENTS_MISC_HOSTENVIRONMENT_H

namespace Misc
{
    // Restore AppRun's snapshot before invoking host tools. Call only in the
    // separate crash helper, never in the running engine/launcher: their future
    // plugin/library loads still require the bundled environment.
    void restoreHostEnvironment();
}

#endif
