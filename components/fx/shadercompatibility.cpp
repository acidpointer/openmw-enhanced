#include "shadercompatibility.hpp"

#include <cstdint>
#include <utility>

namespace
{
    std::uint64_t fingerprint(std::string_view source)
    {
        std::uint64_t result = 14695981039346656037ull;
        for (const unsigned char c : source)
            result = (result ^ c) * 1099511628211ull;
        return result;
    }

    bool replaceOnce(std::string& source, std::string_view from, std::string_view to)
    {
        const auto pos = source.find(from);
        if (pos == std::string::npos || source.find(from, pos + from.size()) != std::string::npos)
            return false;
        source.replace(pos, from.size(), to);
        return true;
    }

    constexpr std::string_view maskTargets = R"FX(
render_target Enhanced_GodrayDepth {
    width_ratio = 0.55;
    height_ratio = 0.55;
    internal_format = r8;
    source_format = red;
    source_type = unsigned_byte;
    mipmaps = false;
    min_filter = linear;
    mag_filter = linear;
}
render_target Enhanced_GodrayBlur {
    width_ratio = 0.55;
    height_ratio = 0.55;
    internal_format = r8;
    source_format = red;
    source_type = unsigned_byte;
    mipmaps = false;
    min_filter = linear;
    mag_filter = linear;
}
)FX";

    constexpr std::string_view maskMapping = R"GLSL(
    vec2 enhancedMaskExtent() {
        return max(floor(omw.resolution * 0.55), vec2(1.0)) / omw.resolution;
    }
    vec2 enhancedMaskToScene(vec2 uv) {
        vec2 size = enhancedMaskExtent();
        return uv * size + vec2(0.0, 1.0 - size.y);
    }
    vec2 enhancedSceneToMask(vec2 uv) {
        vec2 size = enhancedMaskExtent();
        return (uv - vec2(0.0, 1.0 - size.y)) / size;
    }
)GLSL";
}

namespace Fx
{
    std::optional<CompatibleShader> optimizeLegacyShader(
        std::string_view name, std::string_view source, bool compactMasks)
    {
        if (name != "godrays" && name != "depth_of_field")
            return std::nullopt;
        std::string normalized;
        normalized.reserve(source.size());
        if (source.starts_with("\xef\xbb\xbf"))
            source.remove_prefix(3);
        for (std::size_t i = 0; i < source.size(); ++i)
            if (source[i] != '\r' || i + 1 == source.size() || source[i + 1] != '\n')
                normalized += source[i];

        const auto hash = fingerprint(normalized);
        if (name == "depth_of_field" && normalized.size() == 4328 && hash == 0x57eef2c877769e03ull)
        {
            if (!replaceOnce(normalized, "float c, fog = 0.0;", "float c = 0.0, fog = 0.0;")
                || !replaceOnce(normalized, "float c = color.a * Rfixed;", R"GLSL(float c = color.a * Rfixed;
        if (c == 0.0) {
            omw_FragColor = vec4(color.rgb, 1.0);
            return;
        })GLSL"))
                return std::nullopt;
            return CompatibleShader{ std::move(normalized), "XE DOF 1.0 / zero-radius" };
        }
        if (name != "godrays" || normalized.size() != 11472 || hash != 0x682586990b4b6c5cull)
            return std::nullopt;

        if (!replaceOnce(normalized, "        vec2 screendir = (Tex - sunview);", R"GLSL(
        if (strength == 0.0) {
            omw_FragColor = vec4(omw.sunColor.r, 0.8 * omw.sunColor.g, 0.8 * omw.sunColor.b, 0.0);
            return;
        }
        vec2 screendir = (Tex - sunview);)GLSL"))
            return std::nullopt;
        if (!compactMasks)
            return CompatibleShader{ std::move(normalized), "godrays 1.2 / zero-strength" };

        // Keep the packed region's pixel grid and guard band. Only its storage
        // and channel change; rays and the sun-disc combine remain full size.
        if (!replaceOnce(normalized, "shared {", "shared {\n" + std::string(maskMapping))
            || !replaceOnce(normalized, "fragment stretch {",
                std::string(maskTargets) + "\nfragment stretch(target=Enhanced_GodrayDepth) {")
            || !replaceOnce(normalized, "fragment blurRHalf {",
                "fragment blurRHalf(target=Enhanced_GodrayBlur, rt1=Enhanced_GodrayDepth) {")
            || !replaceOnce(normalized, "fragment rays {", "fragment rays(rt1=Enhanced_GodrayBlur) {")
            || !replaceOnce(normalized, "omw_FragColor = vec4(0, 0, 0, depth);", "omw_FragColor = vec4(depth);")
            || !replaceOnce(normalized, "omw_FragColor = vec4(0.0,0.0,0.0,alpha);", "omw_FragColor = vec4(alpha);")
            || !replaceOnce(normalized, "omw_GetLastPass(coords).a",
                "omw_Texture2D(Enhanced_GodrayBlur, enhancedSceneToMask(coords)).r"))
            return std::nullopt;

        const auto first = normalized.find("fragment stretch(");
        const auto end = normalized.find("fragment rays(");
        auto masks = normalized.substr(first, end - first);
        // Exactly two coordinate initializations belong to the two mask stages.
        std::size_t pos = 0;
        unsigned int count = 0;
        constexpr std::string_view oldCoord = "vec2 Tex = omw_TexCoord;";
        while ((pos = masks.find(oldCoord, pos)) != std::string::npos)
        {
            const std::string replacement = "vec2 Tex = enhancedMaskToScene(omw_TexCoord);";
            masks.replace(pos, oldCoord.size(), replacement);
            pos += replacement.size();
            ++count;
        }
        if (count != 2)
            return std::nullopt;
        for (const std::string_view coord :
            { "Tex", "Tex + radial", "Tex - radial", "Tex + 2.0*radial", "Tex - 2.0*radial" })
            if (!replaceOnce(masks, "omw_GetLastPass(" + std::string(coord) + ").a",
                    "omw_Texture2D(Enhanced_GodrayDepth, enhancedSceneToMask(" + std::string(coord) + ")).r"))
                return std::nullopt;
        normalized.replace(first, end - first, masks);
        return CompatibleShader{ std::move(normalized), "godrays 1.2 / compact masks + zero-strength" };
    }
}
