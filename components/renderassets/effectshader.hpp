#ifndef OPENMW_COMPONENTS_RENDERASSETS_EFFECTSHADER_H
#define OPENMW_COMPONENTS_RENDERASSETS_EFFECTSHADER_H

#include <components/render/backend.hpp>

#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace RenderAssets
{
    struct EffectMember
    {
        enum class Scalar
        {
            Float,
            Int,
            UInt
        };
        std::string mName;
        std::uint32_t mOffset = 0;
        std::uint32_t mRows = 1;
        std::uint32_t mColumns = 1;
        std::uint32_t mCount = 1;
        std::uint32_t mArrayStride = 0;
        std::uint32_t mMatrixStride = 0;
        Scalar mScalar = Scalar::Float;
        bool operator==(const EffectMember&) const = default;
    };
    struct EffectResource
    {
        Render::EffectBinding mBinding;
        std::string mName;
        std::vector<EffectMember> mMembers;
    };
    struct EffectShader
    {
        std::vector<std::uint32_t> mVertex, mFragment;
        std::vector<EffectResource> mResources;
        bool mMayDiscard = true;
        bool mRequiresOffscreen = true;
        bool mVertexTextureReads = true;
    };
    // Conservative SPIR-V coverage check. A false result only permits an
    // overwrite when the caller also guarantees fullscreen vertex coverage.
    bool effectMayDiscard(std::span<const std::uint32_t> words);
    // Window presentation flips the viewport. Preserve offscreen execution
    // for shaders observing framebuffer coordinates, facing or signed dFdy.
    bool effectRequiresOffscreen(std::span<const std::uint32_t> words);
    // Translate the generated API and legacy GLSL spellings without changing
    // the effect files/settings used by the OMWFX parser and editor.
    std::string translateEffectShader(
        std::string_view source, bool fragment, std::span<const std::string> samplers = {});
    std::vector<EffectResource> reflectEffectShader(std::span<const std::uint32_t> words);
    std::vector<std::string> effectCompilerArguments(
        const std::filesystem::path& compiler, const std::vector<std::string>& samplers);
    EffectShader compileEffectShader(const std::filesystem::path& compiler, std::string_view vertex,
        std::string_view fragment, const std::vector<std::string>& samplers, std::stop_token stop = {});
}
#endif
