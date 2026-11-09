#ifndef OPENMW_COMPONENTS_FX_SHADERCOMPATIBILITY_H
#define OPENMW_COMPONENTS_FX_SHADERCOMPATIBILITY_H

#include <optional>
#include <string>
#include <string_view>

namespace Fx
{
    struct CompatibleShader
    {
        std::string mSource;
        std::string_view mRevision;
    };

    // Exact reviewed source revisions only, after normal VFS resolution. Never
    // modify a user's files or interpret an unknown shader by its filename.
    std::optional<CompatibleShader> optimizeLegacyShader(
        std::string_view name, std::string_view source, bool compactMasks);
}

#endif
