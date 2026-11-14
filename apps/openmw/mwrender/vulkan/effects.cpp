#include "effects.hpp"
#include "../pingpongcanvas.hpp"

#include <components/debug/debuglog.hpp>
#include <components/fx/scratchrouting.hpp>
#include <components/render/math.hpp>
#include <components/renderassets/effectshader.hpp>
#include <components/renderassets/osgimporter.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/settings/values.hpp>

#include <SDL.h>
#include <chrono>
#include <osg/BufferIndexBinding>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Texture3D>
#include <osg/observer_ptr>

#include <algorithm>
#include <cstring>
#include <deque>
#include <future>
#include <map>
#include <regex>
#include <set>

namespace MWRender::Vulkan
{
    namespace
    {
        struct ShaderPending
        {
        };
        Render::TargetFormat targetFormat(GLint format)
        {
            switch (format)
            {
                case GL_RGBA:
                case GL_RGBA8:
                case GL_RGB:
                case GL_RGB8:
                case 0:
                    return Render::TargetFormat::RGBA8;
                case GL_RGBA16F:
                case GL_RGB16F:
                    return Render::TargetFormat::RGBA16F;
                case GL_R8:
                case GL_RED:
                    return Render::TargetFormat::R8;
                case GL_R16F:
                    return Render::TargetFormat::R16F;
                case GL_RG16F:
                    return Render::TargetFormat::RG16F;
                case GL_RGBA32F:
                case GL_RGB32F:
                    return Render::TargetFormat::RGBA32F;
                case GL_RG32F:
                    return Render::TargetFormat::RG32F;
                case GL_R32F:
                    return Render::TargetFormat::R32F;
                case GL_RG8:
                case GL_RG:
                    return Render::TargetFormat::RG8;
                default:
                    throw std::invalid_argument("Unsupported OMWFX render-target format " + std::to_string(format));
            }
        }
        void packMember(std::vector<std::byte>& bytes, const RenderAssets::EffectMember& member,
            const RenderAssets::OsgState& state, const osg::Matrixf& projection, const osg::Matrixf& inverse)
        {
            const float* matrix = member.mName == "omw.projectionMatrix" ? projection.ptr()
                : member.mName == "omw.invProjectionMatrix"              ? inverse.ptr()
                                                                         : nullptr;
            const auto* uniform = state.getUniform(member.mName);
            const void* source = matrix;
            std::size_t count = matrix ? 16 : 0;
            if (!matrix && uniform)
            {
                if (member.mScalar == RenderAssets::EffectMember::Scalar::Float && uniform->getFloatArray())
                {
                    source = uniform->getFloatArray()->getDataPointer();
                    count = uniform->getFloatArray()->size();
                }
                else if (member.mScalar != RenderAssets::EffectMember::Scalar::Float && uniform->getIntArray())
                {
                    source = uniform->getIntArray()->getDataPointer();
                    count = uniform->getIntArray()->size();
                }
                else if (member.mScalar != RenderAssets::EffectMember::Scalar::Float && uniform->getUIntArray())
                {
                    source = uniform->getUIntArray()->getDataPointer();
                    count = uniform->getUIntArray()->size();
                }
            }
            if (!source)
                return;
            const unsigned components = member.mRows * member.mColumns;
            const unsigned elements = std::min<std::size_t>(member.mCount, count / components);
            for (unsigned element = 0; element < elements; ++element)
                for (unsigned column = 0; column < member.mColumns; ++column)
                {
                    const auto offset = member.mOffset + element * member.mArrayStride + column * member.mMatrixStride;
                    const auto size = member.mRows * 4;
                    if (offset > bytes.size() || size > bytes.size() - offset)
                        throw std::invalid_argument("Invalid reflected OMWFX uniform offset");
                    const auto input = (element * components + column * member.mRows) * 4;
                    std::memcpy(bytes.data() + offset, static_cast<const std::byte*>(source) + input, size);
                }
        }
    }

    struct Effects::Impl
    {
        Render::Backend& mBackend;
        TextureResolver mTextures;
        std::filesystem::path mCompiler;
        struct Target
        {
            Render::Backend& mBackend;
            Render::TextureHandle mColor{}, mDepth{}, mNormals{}, mWorldDepth{};
            unsigned mWidth = 0, mHeight = 0;
            Render::TargetFormat mFormat = Render::TargetFormat::RGBA8;
            Render::SamplerState mSampler;
            bool mInitialized = false;
            explicit Target(Render::Backend& backend)
                : mBackend(backend)
            {
            }
            ~Target()
            {
                if (mNormals)
                    mBackend.releaseTexture(mNormals);
                if (mWorldDepth)
                    mBackend.releaseTexture(mWorldDepth);
                if (mDepth)
                    mBackend.releaseTexture(mDepth);
                if (mColor)
                    mBackend.releaseTexture(mColor);
            }
        };
        std::unique_ptr<Target> mScene;
        std::array<std::unique_ptr<Target>, 3> mScratch;
        struct NamedTarget
        {
            std::weak_ptr<Fx::Technique> mOwner;
            osg::observer_ptr<const osg::Texture2D> mSource;
            std::unique_ptr<Target> mTarget;
        };
        std::map<std::pair<const Fx::Technique*, std::string>, NamedTarget> mNamed;
        struct Program
        {
            Render::Backend& mBackend;
            Render::EffectHandle mHandle{};
            RenderAssets::EffectShader mShader;
            std::future<RenderAssets::EffectShader> mCompilation;
            std::string mError;
            explicit Program(Render::Backend& backend)
                : mBackend(backend)
            {
            }
            ~Program()
            {
                if (mHandle)
                    mBackend.releaseEffect(mHandle);
            }
        };
        std::shared_ptr<Program> mLuminanceProgram, mExposureProgram, mDepthProgram;
        std::unique_ptr<Target> mCombinedDepth, mDistortion;
        std::vector<Render::Draw> mDistortionDraws;
        std::unique_ptr<Target> mLuminance;
        std::array<std::unique_ptr<Target>, 2> mExposure;
        osg::ref_ptr<osg::StateSet> mExposureState;
        osg::ref_ptr<osg::StateSet> mDepthState;
        unsigned mExposureIndex = 0;
        bool mExposureQueued = false;
        std::chrono::steady_clock::time_point mExposureTime{};
        struct VolumeRecord
        {
            osg::observer_ptr<const osg::Texture3D> mSource;
            osg::observer_ptr<const osg::Image> mImage;
            unsigned mRevision = 0;
            std::unique_ptr<Target> mTexture;
        };
        std::map<const osg::Texture3D*, VolumeRecord> mVolumes;
        struct ProgramRecord
        {
            osg::observer_ptr<const osg::Program> mSource;
            std::shared_ptr<Program> mProgram;
            std::string mError;
        };
        std::map<const osg::Program*, ProgramRecord> mPrograms;
        std::map<std::pair<std::string, std::string>, std::shared_ptr<Program>> mCode;
        struct Work
        {
            std::vector<std::vector<std::byte>> mData;
            std::vector<Render::EffectInput> mInputs;
        };
        std::deque<Work> mWork;
        std::vector<Target*> mInitialized;
        std::vector<Render::Draw> mResolve;
        Render::MeshHandle mQuad{};
        std::set<std::string> mWarnings;

        Impl(Render::Backend& backend, TextureResolver textures)
            : mBackend(backend)
            , mTextures(std::move(textures))
        {
            char* base = SDL_GetBasePath();
            if (!base)
                throw std::runtime_error("Cannot locate native effect compiler");
            mCompiler = std::filesystem::path(base) / "glslangValidator";
            SDL_free(base);
            // Installed development builds may use the system tool. AppImages
            // ship this executable beside openmw, together with its libraries.
            if (!std::filesystem::exists(mCompiler))
                mCompiler = "/usr/bin/glslangValidator";
            const std::array<Render::Vertex, 3> vertices{ { { { -1, -1, 0 }, { 0, 0 } }, { { 3, -1, 0 }, { 2, 0 } },
                { { -1, 3, 0 }, { 0, 2 } } } };
            constexpr std::array<std::uint32_t, 3> indices{ 0, 1, 2 };
            mQuad = mBackend.createMesh(vertices, indices);
        }
        ~Impl()
        {
            if (mQuad)
                mBackend.releaseMesh(mQuad);
        }
        Target& target(std::unique_ptr<Target>& holder, unsigned width, unsigned height, Render::TargetFormat format,
            const Render::SamplerState& sampling = {}, bool screen = true)
        {
            auto sampler = sampling;
            if (screen)
                sampler.mWrapS = sampler.mWrapT = Render::TextureWrap::Clamp;
            sampler.mUseMipmaps = false;
            if (!holder || holder->mWidth != width || holder->mHeight != height || holder->mFormat != format
                || holder->mSampler != sampler)
            {
                auto result = std::make_unique<Target>(mBackend);
                result->mSampler = sampler;
                result->mColor = mBackend.createRenderTarget(width, height, sampler, format);
                result->mWidth = width;
                result->mHeight = height;
                result->mFormat = format;
                holder = std::move(result);
            }
            return *holder;
        }
        std::shared_ptr<Program> complete(const std::shared_ptr<Program>& result)
        {
            if (!result->mError.empty())
                throw std::invalid_argument(result->mError);
            if (!result->mHandle)
            {
                if (result->mCompilation.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                    throw ShaderPending{};
                try
                {
                    result->mShader = result->mCompilation.get();
                    std::vector<Render::EffectBinding> bindings;
                    for (const auto& resource : result->mShader.mResources)
                        bindings.push_back(resource.mBinding);
                    result->mHandle
                        = mBackend.createEffect({ result->mShader.mVertex, result->mShader.mFragment, bindings });
                }
                catch (const std::exception& error)
                {
                    result->mError = error.what();
                    throw;
                }
            }
            return result;
        }
        std::shared_ptr<Program> queue(
            const std::string& vertex, const std::string& fragment, const std::vector<std::string>& samplers)
        {
            auto result = std::make_shared<Program>(mBackend);
            result->mCompilation = std::async(std::launch::async, [compiler = mCompiler, vertex, fragment, samplers] {
                return RenderAssets::compileEffectShader(compiler, vertex, fragment, samplers);
            });
            return result;
        }

        std::shared_ptr<Program> program(const RenderAssets::OsgState& state)
        {
            const auto* source = dynamic_cast<const osg::Program*>(state.getAttribute(osg::StateAttribute::PROGRAM));
            if (!source)
                throw std::invalid_argument("Effect pass has no shader program");
            auto& record = mPrograms[source];
            if (record.mSource.get() == source)
            {
                if (!record.mError.empty())
                    throw std::invalid_argument(record.mError);
                return complete(record.mProgram);
            }
            record = {};
            record.mSource = source;
            try
            {
                std::pair<std::string, std::string> code;
                for (unsigned i = 0; i < source->getNumShaders(); ++i)
                {
                    const auto* shader = source->getShader(i);
                    if (shader->getType() == osg::Shader::VERTEX)
                        code.first = shader->getShaderSource();
                    else if (shader->getType() == osg::Shader::FRAGMENT)
                        code.second = shader->getShaderSource();
                    else
                        throw std::invalid_argument("Compute and geometry OMWFX passes are not yet supported");
                }
                if (code.first.empty() || code.second.empty())
                    throw std::invalid_argument("Incomplete effect shader pair");
                if (const auto found = mCode.find(code); found != mCode.end())
                    return complete(record.mProgram = found->second);
                std::vector<std::string> samplers;
                const std::regex declaration(R"(uniform\s+[iu]?sampler\w*\s+(\w+))");
                for (const auto* stage : { &code.first, &code.second })
                    for (std::sregex_iterator match(stage->begin(), stage->end(), declaration), end; match != end;
                        ++match)
                        samplers.push_back((*match)[1].str());
                std::sort(samplers.begin(), samplers.end());
                samplers.erase(std::unique(samplers.begin(), samplers.end()), samplers.end());
                auto result = queue(code.first, code.second, samplers);
                mCode.emplace(std::move(code), result);
                record.mProgram = result;
                Log(Debug::Info) << "Vulkan OMWFX queued: " << source->getName();
                return complete(result);
            }
            catch (const ShaderPending&)
            {
                throw;
            }
            catch (const std::exception& error)
            {
                record.mError = error.what();
                throw;
            }
        }
        void uniforms(Work& work, const Program& program, const RenderAssets::OsgState& state,
            const osg::Matrixf& projection, const std::function<Render::TextureHandle(const std::string&)>& texture)
        {
            work.mData.reserve(program.mShader.mResources.size());
            work.mInputs.reserve(program.mShader.mResources.size());
            const auto inverse = osg::Matrixf::inverse(projection);
            for (const auto& resource : program.mShader.mResources)
            {
                if (resource.mBinding.mKind == Render::EffectBindingKind::Texture)
                    work.mInputs.push_back({ resource.mBinding.mBinding, texture(resource.mName), {} });
                else
                {
                    auto& bytes = work.mData.emplace_back(resource.mBinding.mSize);
                    if (resource.mName == "_data")
                    {
                        const auto* binding = dynamic_cast<const osg::UniformBufferBinding*>(
                            state.getAttribute(osg::StateAttribute::UNIFORMBUFFERBINDING,
                                static_cast<unsigned>(Resource::SceneManager::UBOBinding::PostProcessor)));
                        if (binding && binding->getBufferData() && binding->getOffset() >= 0)
                        {
                            const auto offset = static_cast<std::size_t>(binding->getOffset());
                            const auto available = binding->getBufferData()->getTotalDataSize();
                            if (offset <= available && bytes.size() <= available - offset
                                && bytes.size() <= static_cast<std::size_t>(binding->getSize()))
                                std::memcpy(bytes.data(),
                                    static_cast<const std::byte*>(binding->getBufferData()->getDataPointer()) + offset,
                                    bytes.size());
                            else
                                throw std::invalid_argument("OMWFX engine buffer does not match reflected layout");
                        }
                    }
                    for (const auto& member : resource.mMembers)
                        packMember(bytes, member, state, projection, inverse);
                    work.mInputs.push_back({ resource.mBinding.mBinding, {}, bytes });
                }
            }
        }

        std::shared_ptr<Program> builtin(
            std::shared_ptr<Program>& holder, const std::string& fragment, const std::vector<std::string>& samplers)
        {
            if (!holder)
            {
                const std::string vertex
                    = "#version 450\nlayout(location=0) in vec2 position;\nlayout(location=0) out vec2 uv;\nvoid "
                      "main(){ gl_Position=vec4(position,0,1); uv=position*.5+.5; }\n";
                holder = queue(vertex, fragment, samplers);
            }
            return complete(holder);
        }
        Render::TextureHandle exposure(
            std::vector<Render::Pass>& effects, Render::TextureHandle scene, const osg::Matrixf& projection)
        {
            const auto luminance = builtin(mLuminanceProgram, R"GLSL(#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outputColor;
uniform sampler2D inputScene;
void main() {
    float sum=0;
    for (int y=0;y<4;++y) for(int x=0;x<4;++x) {
        vec2 offset=(vec2(x,y)-1.5)/64.0;
        float value=max(dot(texture(inputScene,uv+offset).rgb,vec3(.2126,.7152,.0722)),.004);
        sum+=clamp((log2(value)+9.0)/13.0,0,1);
    }
    outputColor=vec4(sum/16.0,0,0,1);
})GLSL",
                { "inputScene" });
            const auto adapt = builtin(mExposureProgram, R"GLSL(#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outputColor;
uniform sampler2D currentLum;
uniform sampler2D previousLum;
uniform float delta;
uniform float speed;
uniform int initialized;
void main() {
    float sum=0;
    for (int y=0;y<16;++y) for(int x=0;x<16;++x) sum+=texelFetch(currentLum,ivec2(x,y),0).r;
    float current=exp2(sum/256.0*13.0-9.0);
    float previous=texture(previousLum,vec2(.5)).r;
    outputColor=vec4(initialized==0 ? current : mix(previous,current,1.0-exp(-delta*speed)),0,0,1);
})GLSL",
                { "currentLum", "previousLum" });
            auto& lum = target(mLuminance, 16, 16, Render::TargetFormat::R16F);
            auto& previous = target(mExposure[mExposureIndex], 1, 1, Render::TargetFormat::R16F);
            auto& current = target(mExposure[1 - mExposureIndex], 1, 1, Render::TargetFormat::R16F);
            auto& first = mWork.emplace_back();
            uniforms(first, *luminance, {}, projection, [&](const std::string&) { return scene; });
            effects.push_back({ {}, lum.mColor, {}, false, {}, luminance->mHandle, first.mInputs });
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = mExposureTime.time_since_epoch().count()
                ? std::chrono::duration<float>(now - mExposureTime).count()
                : 0.f;
            // Reuse these three uniforms; no per-frame shader/state allocation.
            if (!mExposureState)
            {
                mExposureState = new osg::StateSet;
                mExposureState->addUniform(new osg::Uniform("delta", 0.f));
                mExposureState->addUniform(new osg::Uniform("speed", 1.f));
                mExposureState->addUniform(new osg::Uniform("initialized", 0));
            }
            mExposureState->getUniform("delta")->set(std::clamp(elapsed, 0.f, 1.f));
            mExposureState->getUniform("speed")->set(Settings::postProcessing().mAutoExposureSpeed.get());
            mExposureState->getUniform("initialized")->set(previous.mInitialized ? 1 : 0);
            auto& second = mWork.emplace_back();
            uniforms(second, *adapt, RenderAssets::OsgState(*mExposureState), projection,
                [&](const std::string& name) { return name == "currentLum" ? lum.mColor : previous.mColor; });
            effects.push_back({ {}, current.mColor, {}, false, {}, adapt->mHandle, second.mInputs });
            mInitialized.push_back(&current);
            mExposureQueued = true;
            return current.mColor;
        }

        Render::TextureHandle volume(const osg::Texture3D& texture)
        {
            const auto* image = texture.getImage();
            if (!image || image->s() <= 0 || image->t() <= 0 || image->r() < 2 || !image->data())
                throw std::invalid_argument("OMWFX volume texture has no 3D image");
            auto& entry = mVolumes[&texture];
            if (entry.mSource.get() == &texture && entry.mImage.get() == image
                && entry.mRevision == image->getModifiedCount())
                return entry.mTexture->mColor;
            const auto pixels = std::uint64_t(image->s()) * image->t() * image->r();
            if (pixels > 128 * 1024 * 1024)
                throw std::invalid_argument("OMWFX volume image is too large");
            std::vector<std::byte> rgba(pixels * 4);
            for (int z = 0; z < image->r(); ++z)
                for (int y = 0; y < image->t(); ++y)
                    for (int x = 0; x < image->s(); ++x)
                    {
                        const auto color = image->getColor(x, y, z);
                        const auto offset = ((std::size_t(z) * image->t() + y) * image->s() + x) * 4;
                        for (unsigned c = 0; c < 4; ++c)
                            rgba[offset + c]
                                = std::byte(static_cast<unsigned char>(std::clamp(color[c], 0.f, 1.f) * 255.f + .5f));
                    }
            auto sampling = RenderAssets::OsgImporter::importSampler(texture);
            switch (texture.getWrap(osg::Texture::WRAP_R))
            {
                case osg::Texture::CLAMP_TO_EDGE:
                case osg::Texture::CLAMP:
                    sampling.mWrapR = Render::TextureWrap::Clamp;
                    break;
                case osg::Texture::CLAMP_TO_BORDER:
                    sampling.mWrapR = Render::TextureWrap::Border;
                    break;
                case osg::Texture::MIRROR:
                    sampling.mWrapR = Render::TextureWrap::Mirror;
                    break;
                default:
                    sampling.mWrapR = Render::TextureWrap::Repeat;
                    break;
            }
            auto uploaded = std::make_unique<Target>(mBackend);
            uploaded->mColor = mBackend.createVolume(image->s(), image->t(), image->r(), rgba, sampling);
            entry.mTexture = std::move(uploaded);
            entry.mSource = &texture;
            entry.mImage = image;
            entry.mRevision = image->getModifiedCount();
            return entry.mTexture->mColor;
        }

        void append(std::vector<Render::Pass>& passes, PingPongCanvas& canvas,
            const RenderAssets::OsgState& canvasState, const osg::Matrixd& projection,
            std::span<const Render::Draw> distortion)
        {
            mExposureQueued = false;
            mWork.clear();
            mInitialized.clear();
            mResolve.clear();
            const auto plan = Fx::makeExecutionPlan(canvas.getPasses(), canvas.getMask(), canvas.getPostProcessing());
            if (plan.mNodes.empty())
                return;
            const auto scenePosition
                = std::find_if(passes.begin(), passes.end(), [](const auto& pass) { return !pass.mTarget; });
            if (scenePosition == passes.end())
                return;
            const auto sceneIndex = static_cast<std::size_t>(scenePosition - passes.begin());
            unsigned width = scenePosition->mViewport[2], height = scenePosition->mViewport[3];
            if (!width || !height)
                return;
            const auto format = plan.mHDR ? Render::TargetFormat::RGBA16F : Render::TargetFormat::RGBA8;
            struct Prepared
            {
                const Fx::DispatchNode* node;
                std::vector<std::pair<std::shared_ptr<Program>, RenderAssets::OsgState>> passes;
            };
            std::vector<Prepared> prepared;
            for (auto index : plan.mNodes)
            {
                const auto& node = canvas.getPasses()[index];
                try
                {
                    if (node.mInternalDistortion && distortion.empty())
                        continue;
                    if (node.mInternalDistortion)
                        node.mRootStateSet->getOrCreateUniform("enhancedDistortionCoverage", osg::Uniform::FLOAT_VEC4)
                            ->set(osg::Vec4f(0, 0, 1, 1));
                    Prepared result{ &node, {} };
                    for (const auto& pass : node.mPasses)
                    {
                        if (pass.mMipMap)
                            throw std::invalid_argument("Mipmapped OMWFX targets are not yet available in Vulkan");
                        RenderAssets::OsgState state = canvasState;
                        state.merge(*node.mRootStateSet);
                        state.merge(*pass.mStateSet);
                        if (state.getMode(GL_BLEND) & osg::StateAttribute::ON)
                            throw std::invalid_argument("OMWFX attachment blending is not yet available in Vulkan");
                        auto shader = program(state);
                        result.passes.emplace_back(std::move(shader), std::move(state));
                    }
                    prepared.push_back(std::move(result));
                }
                catch (const ShaderPending&)
                {
                    continue;
                }
                catch (const std::exception& error)
                {
                    const auto message = node.mHandle->getName() + ": " + error.what();
                    if (mWarnings.insert(message).second)
                        Log(Debug::Warning) << "Vulkan OMWFX skipped " << message;
                }
            }
            if (prepared.empty())
                return;
            bool needsNormals = false, needsDepth = false, needsDistortion = false;
            for (const auto& item : prepared)
                for (const auto& [shader, state] : item.passes)
                    for (const auto& resource : shader->mShader.mResources)
                    {
                        needsNormals |= resource.mName == "omw_SamplerNormals";
                        needsDistortion |= resource.mName == "omw_SamplerDistortion";
                        needsDepth |= resource.mName == "omw_SamplerDepth" || resource.mName == "omw_SamplerWorldDepth";
                    }
            const auto sceneFormat = needsNormals
                ? (plan.mHDR ? Render::TargetFormat::SceneRGBA16F : Render::TargetFormat::SceneRGBA8)
                : format;
            auto& scene = target(mScene, width, height, sceneFormat);
            if (!scene.mDepth)
                scene.mDepth = mBackend.depthTexture(scene.mColor);
            if (needsNormals && !scene.mNormals)
                scene.mNormals = mBackend.normalTexture(scene.mColor);
            const bool firstPerson = (needsDepth || needsDistortion)
                && std::any_of(passes[sceneIndex].mDraws.begin(), passes[sceneIndex].mDraws.end(),
                    [](const auto& draw) { return draw.mClearDepthBefore; });
            if (firstPerson && !scene.mWorldDepth)
                scene.mWorldDepth = mBackend.worldDepthTexture(scene.mColor);
            std::vector<Render::Pass> effects;
            auto depth = scene.mDepth;
            const auto worldDepth = firstPerson ? scene.mWorldDepth : scene.mDepth;
            if (firstPerson)
            {
                auto shader = builtin(mDepthProgram, R"GLSL(#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outputColor;
uniform sampler2D firstPersonDepth;
uniform sampler2D worldDepth;
uniform vec4 fpProjection;
uniform vec4 worldProjection;
void main(){
    float fp=texture(firstPersonDepth,uv).r;
    float world=texture(worldDepth,uv).r;
    float result=world;
    if(fp<1.0){
        float denominator=fp*fpProjection.z-fpProjection.x;
        if(abs(denominator)>1e-8){
            float viewZ=(fpProjection.y-fp*fpProjection.w)/denominator;
            float clipW=worldProjection.z*viewZ+worldProjection.w;
            result=abs(clipW)>1e-8?clamp((worldProjection.x*viewZ+worldProjection.y)/clipW,0.0,1.0):world;
        }
    }
    outputColor=vec4(result,0,0,1);
}
)GLSL",
                    { "firstPersonDepth", "worldDepth" });
                const auto first = std::find_if(passes[sceneIndex].mDraws.begin(), passes[sceneIndex].mDraws.end(),
                    [](const auto& draw) { return draw.mClearDepthBefore; });
                const auto inverseView = osg::Matrixf::inverse(osg::Matrixf(first->mViewFromModel.data()));
                Render::Matrix inverse{};
                std::copy_n(inverseView.ptr(), 16, inverse.begin());
                const auto fp = Render::multiply(first->mClipFromModel, inverse);
                if (!mDepthState)
                {
                    mDepthState = new osg::StateSet;
                    mDepthState->addUniform(new osg::Uniform("fpProjection", osg::Vec4f()));
                    mDepthState->addUniform(new osg::Uniform("worldProjection", osg::Vec4f()));
                }
                const auto fpCoefficients = RenderAssets::depthCoefficients(osg::Matrixd(fp.data()), true);
                const auto worldCoefficients = RenderAssets::depthCoefficients(projection, false);
                mDepthState->getUniform("fpProjection")
                    ->set(osg::Vec4f(fpCoefficients[0], fpCoefficients[1], fpCoefficients[2], fpCoefficients[3]));
                mDepthState->getUniform("worldProjection")
                    ->set(osg::Vec4f(
                        worldCoefficients[0], worldCoefficients[1], worldCoefficients[2], worldCoefficients[3]));
                Render::SamplerState nearest;
                nearest.mMinFilter = nearest.mMagFilter = Render::TextureFilter::Nearest;
                auto& merged = target(mCombinedDepth, width, height, Render::TargetFormat::R32F, nearest);
                auto& work = mWork.emplace_back();
                uniforms(work, *shader, RenderAssets::OsgState(*mDepthState), osg::Matrixf(projection),
                    [&](const std::string& name) { return name == "worldDepth" ? worldDepth : scene.mDepth; });
                effects.push_back({ {}, merged.mColor, {}, false, {}, shader->mHandle, work.mInputs });
                depth = merged.mColor;
            }
            Render::TextureHandle distortionTexture;
            if (needsDistortion)
            {
                auto& field = target(mDistortion, width, height, Render::TargetFormat::RGBA16F);
                mDistortionDraws.assign(distortion.begin(), distortion.end());
                for (auto& draw : mDistortionDraws)
                    draw.mMaskTexture = depth;
                effects.push_back({ mDistortionDraws, field.mColor, { 0, 0, 0, 0 }, true });
                distortionTexture = field.mColor;
            }
            bool needsExposure = false;
            for (const auto& item : prepared)
                for (const auto& [shader, state] : item.passes)
                    for (const auto& resource : shader->mShader.mResources)
                        needsExposure |= resource.mName == "omw_EyeAdaptation";
            const auto eye
                = needsExposure ? exposure(effects, scene.mColor, osg::Matrixf(projection)) : Render::TextureHandle{};
            Fx::ScratchRouting routing;
            const auto scratch = [&](int slot) {
                return slot < 0 ? scene.mColor : target(mScratch[slot], width, height, format).mColor;
            };
            for (const auto& item : prepared)
            {
                const auto& node = *item.node;
                for (const auto& [name, source] : node.mHandle->getRenderTargetsMap())
                {
                    auto& entry = mNamed[{ node.mHandle.get(), std::string(name) }];
                    if (entry.mOwner.lock() != node.mHandle || entry.mSource.get() != source.mTarget.get())
                    {
                        entry.mTarget.reset();
                        entry.mOwner = node.mHandle;
                        entry.mSource = source.mTarget.get();
                    }
                    const auto [w, h] = source.mSize.get(width, height);
                    auto& destination = target(entry.mTarget, w, h, targetFormat(source.mTarget->getInternalFormat()),
                        RenderAssets::OsgImporter::importSampler(*source.mTarget), false);
                    if (!destination.mInitialized)
                    {
                        effects.push_back({ {}, destination.mColor,
                            { source.mClearColor.r(), source.mClearColor.g(), source.mClearColor.b(),
                                source.mClearColor.a() } });
                        mInitialized.push_back(&destination);
                    }
                }
                for (std::size_t index = 0; index < item.passes.size(); ++index)
                {
                    const auto& [shader, state] = item.passes[index];
                    const auto lastShader = scratch(routing.lastShader());
                    const auto lastPass = scratch(routing.lastPass());
                    const auto& source = node.mHandle->getPasses()[index];
                    const auto name = source->getTarget();
                    const auto output = name.empty() ? scratch(routing.selectOutput(index + 1 == item.passes.size()))
                                                     : mNamed.at({ node.mHandle.get(), name }).mTarget->mColor;
                    auto& work = mWork.emplace_back();
                    uniforms(work, *shader, state, osg::Matrixf(projection), [&](const std::string& sampler) {
                        if (sampler == "omw_SamplerDistortion")
                            return distortionTexture;
                        if (sampler == "omw_EyeAdaptation")
                            return eye;
                        if (sampler == "omw_SamplerLastShader")
                            return lastShader;
                        if (sampler == "omw_SamplerLastPass")
                            return lastPass;
                        if (sampler == "omw_SamplerNormals")
                            return scene.mNormals;
                        if (sampler == "omw_SamplerWorldDepth")
                            return worldDepth;
                        if (sampler == "omw_SamplerDepth")
                            return depth;
                        if (const auto named = mNamed.find({ node.mHandle.get(), sampler }); named != mNamed.end())
                            return named->second.mTarget->mColor;
                        const auto unit = RenderAssets::textureUnit(state, sampler);
                        const auto* attribute
                            = unit ? state.getTextureAttribute(*unit, osg::StateAttribute::TEXTURE) : nullptr;
                        if (const auto* image = dynamic_cast<const osg::Texture2D*>(attribute))
                            return mTextures(image);
                        if (const auto* image = dynamic_cast<const osg::Texture3D*>(attribute))
                            return volume(*image);
                        throw std::invalid_argument("Missing or unsupported OMWFX texture: " + sampler);
                    });
                    for (const auto& input : work.mInputs)
                        if (input.mTexture == output)
                            throw std::invalid_argument("OMWFX pass samples its own render target");
                    effects.push_back({ {}, output, {}, false, {}, shader->mHandle, work.mInputs });
                }
            }
            Render::Draw resolve;
            resolve.mClipFromModel = Render::IdentityTransform;
            resolve.mMesh = mQuad;
            resolve.mTexture = scratch(routing.lastShader());
            resolve.mDepthTest = resolve.mDepthWrite = false;
            resolve.mDoubleSided = true;
            resolve.mUseVertexColor = false;
            mResolve.push_back(resolve);
            // Publish scene redirection and its resolve together. An allocation
            // failure while assembling passes must leave the original window
            // scene intact, rather than stranding it in an offscreen target.
            auto composed = passes;
            composed.reserve(passes.size() + effects.size() + 1);
            const auto windowViewport = composed[sceneIndex].mViewport;
            composed[sceneIndex].mViewport = { 0, 0, width, height };
            composed[sceneIndex].mTarget = scene.mColor;
            composed[sceneIndex].mPreserveWorldDepth = firstPerson;
            composed.insert(composed.begin() + sceneIndex + 1, effects.begin(), effects.end());
            composed.insert(
                composed.begin() + sceneIndex + effects.size() + 1, { mResolve, {}, {}, true, windowViewport });
            passes.swap(composed);
            std::erase_if(mNamed, [](const auto& pair) { return pair.second.mOwner.expired(); });
            std::erase_if(mVolumes, [](const auto& pair) { return !pair.second.mSource.valid(); });
            std::erase_if(mPrograms, [](const auto& pair) { return !pair.second.mSource.valid(); });
            std::erase_if(mCode, [](const auto& pair) {
                const auto& program = pair.second;
                // Destroying an unfinished async future would join its worker.
                return program.use_count() == 1
                    && (!program->mCompilation.valid()
                        || program->mCompilation.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
            });
        }
    };

    Effects::Effects(Render::Backend& backend, TextureResolver textures)
        : mImpl(std::make_unique<Impl>(backend, std::move(textures)))
    {
    }
    Effects::~Effects() = default;
    void Effects::append(std::vector<Render::Pass>& passes, PingPongCanvas& canvas, const RenderAssets::OsgState& state,
        const osg::Matrixd& projection, std::span<const Render::Draw> distortion)
    {
        try
        {
            mImpl->append(passes, canvas, state, projection, distortion);
        }
        catch (const ShaderPending&)
        {
            mImpl->mInitialized.clear();
            mImpl->mExposureQueued = false;
        }
        catch (const std::exception& error)
        {
            mImpl->mInitialized.clear();
            mImpl->mExposureQueued = false;
            if (mImpl->mWarnings.insert(error.what()).second)
                Log(Debug::Warning) << "Vulkan OMWFX: " << error.what();
        }
    }
    void Effects::submitted()
    {
        if (mImpl->mExposureQueued)
        {
            mImpl->mExposureIndex = 1 - mImpl->mExposureIndex;
            mImpl->mExposureTime = std::chrono::steady_clock::now();
            mImpl->mExposureQueued = false;
        }
        for (auto* target : mImpl->mInitialized)
            target->mInitialized = true;
        mImpl->mInitialized.clear();
    }
}
