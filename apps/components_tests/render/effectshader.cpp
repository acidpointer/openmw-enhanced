#include <components/renderassets/effectshader.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

namespace
{
    TEST(NativeEffectShader, CancelledJobsDoNotLaunchACompiler)
    {
        std::stop_source stop;
        stop.request_stop();
        try
        {
            RenderAssets::compileEffectShader("/nonexistent/compiler", "", "", {}, stop.get_token());
            FAIL() << "A cancelled job must not run";
        }
        catch (const std::runtime_error& error)
        {
            EXPECT_STREQ(error.what(), "Effect shader compilation cancelled");
        }
    }

    struct Module
    {
        std::vector<std::uint32_t> words{ 0x07230203, 0x00010300, 0, 128, 0 };
        void instruction(unsigned op, std::initializer_list<std::uint32_t> args)
        {
            words.push_back(static_cast<std::uint32_t>((args.size() + 1) << 16) | op);
            words.insert(words.end(), args);
        }
        void name(unsigned op, std::initializer_list<std::uint32_t> args, const char* name)
        {
            const auto size = std::strlen(name) + 1;
            const auto count = (size + 3) / 4;
            words.push_back(static_cast<std::uint32_t>((args.size() + count + 1) << 16) | op);
            words.insert(words.end(), args);
            const auto offset = words.size();
            words.resize(offset + count);
            std::memcpy(words.data() + offset, name, size);
        }
    };

    TEST(NativeEffectShader, CompilerArgumentsReserveASeparateBlockForGlobals)
    {
        for (const auto& samplers :
            { std::vector<std::string>{}, std::vector<std::string>{ "omw_SamplerLastShader", "noise" } })
        {
            const auto args = RenderAssets::effectCompilerArguments("/compiler", samplers);
            ASSERT_GE(args.size(), 3u);
            EXPECT_EQ(args[1], "effect.vert");
            EXPECT_EQ(args[2], "effect.frag");
            EXPECT_EQ(std::find(args.begin(), args.end(), "--resource-set-binding"), args.end());
            auto block = std::find(args.begin(), args.end(), "--set-default-uniform-block");
            ASSERT_NE(block, args.end());
            ASSERT_GE(args.end() - block, 4);
            EXPECT_EQ(block[1], "_NativeGlobals");
            EXPECT_EQ(block[2], "0");
            EXPECT_EQ(block[3], std::to_string(samplers.size()));
        }
    }

    TEST(NativeEffectShader, AssignsIdenticalDescriptorLayoutsBeforeLinkingStages)
    {
        const std::vector<std::string> samplers{ "noise", "omw_SamplerLastShader" };
        const std::string header = R"(#version 120
#define omw_FragColor gl_FragColor
uniform sampler2D omw_SamplerLastShader;
uniform sampler2D noise;
layout(std140) uniform _data { mat4 projectionMatrix; };
)";
        for (bool fragment : { false, true })
        {
            const auto output = RenderAssets::translateEffectShader(header, fragment, samplers);
            EXPECT_NE(
                output.find("layout(set=0,binding=1) uniform sampler2D omw_SamplerLastShader;"), std::string::npos);
            EXPECT_NE(output.find("layout(set=0,binding=0) uniform sampler2D noise;"), std::string::npos);
            EXPECT_NE(output.find("layout(std140,set=0,binding=3) uniform _data"), std::string::npos);
            EXPECT_EQ(output.find("layout(std140) uniform _data"), std::string::npos);
        }
        EXPECT_THROW(RenderAssets::translateEffectShader(header, true), std::invalid_argument);
    }

    TEST(NativeEffectShader, TranslatesLegacyHdrVaryingsWithoutChangingUserFiles)
    {
        const auto output = RenderAssets::translateEffectShader(R"(#version 120
#define omw_FragColor gl_FragColor
varying vec2 omw_TexCoord;
void main() {gl_FragColor=vec4(omw_TexCoord,0,1);}
)",
            true);
        EXPECT_NE(output.find("#define varying in\n"), std::string::npos);
        EXPECT_NE(output.find("void main() {_omw_FragColor=vec4(omw_TexCoord,0,1);}"), std::string::npos);
        EXPECT_EQ(output.find("gl_FragColor"), std::string::npos);
    }

    TEST(NativeEffectShader, TranslatesGeneratedApiAndPreservesEffectBody)
    {
        const std::string source = R"(#version 120
#define OMW_REVERSE_Z 1
#define OMW_USE_BINDINGS 0
#define omw_In varying
#define omw_Out varying
#define omw_FragColor gl_FragColor
out vec4 omw_FragColor;
// user shader
void main() { omw_FragColor = texture2D(omw_SamplerLastShader, omw_TexCoord); }
)";
        const auto output = RenderAssets::translateEffectShader(source, true);
        EXPECT_TRUE(output.starts_with("#version 450 core\n"));
        EXPECT_NE(output.find("#define OMW_REVERSE_Z 0\n"), std::string::npos);
        EXPECT_NE(output.find("#define OMW_USE_BINDINGS 1\n"), std::string::npos);
        EXPECT_NE(output.find("layout(location=0) out vec4 _omw_FragColor;"), std::string::npos);
        EXPECT_EQ(output.find("out vec4 omw_FragColor;"), std::string::npos);
        EXPECT_NE(output.find("void main() { omw_FragColor = texture2D(omw_SamplerLastShader, omw_TexCoord); }"),
            std::string::npos);
        EXPECT_THROW(RenderAssets::translateEffectShader("void main(){}", true), std::invalid_argument);
        EXPECT_THROW(RenderAssets::translateEffectShader("#version 120\n#version 450", true), std::invalid_argument);
    }

    TEST(NativeEffectShader, ReflectsEngineStructAndColumnMajorProjection)
    {
        Module m;
        m.instruction(22, { 1, 32 }); // float
        m.instruction(23, { 2, 1, 4 }); // vec4
        m.instruction(24, { 3, 2, 4 }); // mat4
        m.instruction(21, { 4, 32, 0 }); // uint (UBO bool)
        m.instruction(30, { 10, 3, 2, 4 }); // _omw_data
        m.name(6, { 10, 0 }, "projectionMatrix");
        m.name(6, { 10, 1 }, "fogColor");
        m.name(6, { 10, 2 }, "isUnderwater");
        m.instruction(72, { 10, 0, 35, 0 });
        m.instruction(72, { 10, 0, 7, 16 });
        m.instruction(72, { 10, 1, 35, 64 });
        m.instruction(72, { 10, 2, 35, 80 });
        m.instruction(30, { 20, 10 });
        m.name(5, { 20 }, "_data");
        m.name(6, { 20, 0 }, "omw");
        m.instruction(72, { 20, 0, 35, 0 });
        m.instruction(71, { 20, 2 });
        m.instruction(32, { 21, 2, 20 });
        m.instruction(59, { 21, 22, 2 });
        m.instruction(71, { 22, 33, 8 });
        m.instruction(71, { 22, 34, 0 });
        const auto resources = RenderAssets::reflectEffectShader(m.words);
        ASSERT_EQ(resources.size(), 1u);
        const auto& block = resources[0];
        EXPECT_EQ(block.mName, "_data");
        EXPECT_EQ(block.mBinding.mBinding, 8u);
        EXPECT_EQ(block.mBinding.mKind, Render::EffectBindingKind::UniformBuffer);
        EXPECT_EQ(block.mBinding.mSize, 96u);
        ASSERT_EQ(block.mMembers.size(), 3u);
        EXPECT_EQ(block.mMembers[0].mName, "omw.projectionMatrix");
        EXPECT_EQ(block.mMembers[0].mColumns, 4u);
        EXPECT_EQ(block.mMembers[0].mRows, 4u);
        EXPECT_EQ(block.mMembers[0].mMatrixStride, 16u);
        EXPECT_EQ(block.mMembers[1].mOffset, 64u);
        EXPECT_EQ(block.mMembers[2].mScalar, RenderAssets::EffectMember::Scalar::UInt);
    }

    TEST(NativeEffectShader, PreservesArrayStrideAndVolumeSampler)
    {
        Module m;
        m.instruction(22, { 1, 32 });
        m.instruction(23, { 2, 1, 3 });
        m.instruction(21, { 3, 32, 0 });
        m.instruction(43, { 3, 4, 3 });
        m.instruction(28, { 5, 2, 4 });
        m.instruction(71, { 5, 6, 16 }); // vec3[3]
        m.instruction(30, { 6, 5 });
        m.instruction(71, { 6, 2 });
        m.name(5, { 6 }, "_NativeGlobals");
        m.name(6, { 6, 0 }, "colors");
        m.instruction(72, { 6, 0, 35, 0 });
        m.instruction(32, { 7, 2, 6 });
        m.instruction(59, { 7, 8, 2 });
        m.instruction(71, { 8, 33, 1 });
        m.instruction(71, { 8, 34, 0 });
        m.instruction(25, { 10, 1, 2, 0, 0, 0, 1, 0 }); // sampled 3D float image
        m.instruction(27, { 11, 10 });
        m.instruction(32, { 12, 0, 11 });
        m.instruction(59, { 12, 13, 0 });
        m.instruction(71, { 13, 33, 0 });
        m.instruction(71, { 13, 34, 0 });
        m.name(5, { 13 }, "water_nrm");
        const auto resources = RenderAssets::reflectEffectShader(m.words);
        ASSERT_EQ(resources.size(), 2u);
        EXPECT_EQ(resources[0].mBinding.mSize, 48u);
        ASSERT_EQ(resources[0].mMembers.size(), 1u);
        EXPECT_EQ(resources[0].mMembers[0].mArrayStride, 16u);
        EXPECT_EQ(resources[0].mMembers[0].mCount, 3u);
        EXPECT_EQ(resources[1].mName, "water_nrm");
        EXPECT_EQ(resources[1].mBinding.mKind, Render::EffectBindingKind::Texture);
    }

    TEST(NativeEffectShader, RejectsTruncatedAndUnsupportedResources)
    {
        Module m;
        m.instruction(22, { 1, 32 });
        m.instruction(25, { 2, 1, 3, 0, 0, 0, 1, 0 }); // cube
        m.instruction(27, { 3, 2 });
        m.instruction(32, { 4, 0, 3 });
        m.instruction(59, { 4, 5, 0 });
        m.instruction(71, { 5, 33, 0 });
        m.instruction(71, { 5, 34, 0 });
        EXPECT_THROW(RenderAssets::reflectEffectShader(m.words), std::invalid_argument);
        m.words.pop_back();
        EXPECT_THROW(RenderAssets::reflectEffectShader(m.words), std::invalid_argument);
        m.words.resize(5);
        m.words.push_back(0);
        EXPECT_THROW(RenderAssets::reflectEffectShader(m.words), std::invalid_argument);
    }
}
