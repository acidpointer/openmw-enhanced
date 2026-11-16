#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_H

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;

namespace Render
{
    inline constexpr std::array<float, 16> IdentityTransform{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    enum class Api
    {
        OpenGL,
        Vulkan,
    };

    constexpr std::string_view getApiName(Api api)
    {
        return api == Api::OpenGL ? "opengl" : "vulkan";
    }

    // These operations preserve the engine's ordering of scene updates, focus
    // queries, Lua work and submission. Scene services migrate separately.
    class FrameDriver
    {
    public:
        virtual ~FrameDriver() = default;
        virtual Api api() const = 0;
        virtual bool isDone() const = 0;
        virtual void advance(double simulationTime) = 0;
        virtual unsigned int frameNumber() const = 0;
        virtual void processEvents() = 0;
        virtual void updateScene() = 0;
        virtual void renderFrame() = 0;
        // Join rendering work before destroying the world or asset services.
        virtual void stop() = 0;
    };

    template <class Tag>
    struct Handle
    {
        std::uint32_t mIndex = ~std::uint32_t{ 0 };
        std::uint32_t mGeneration = 0;
        explicit operator bool() const { return mGeneration != 0; }
        bool operator==(const Handle&) const = default;
    };

    using MeshHandle = Handle<struct MeshTag>;
    using TextureHandle = Handle<struct TextureTag>;
    using EffectHandle = Handle<struct EffectTag>;

    enum class TargetFormat : std::uint8_t
    {
        RGBA8,
        RGBA16F,
        R8,
        R16F,
        RG16F,
        RGBA32F,
        RG32F,
        R32F,
        RG8,
        SceneRGBA8,
        SceneRGBA16F,
        // Fullscreen effects do not need depth storage or depth clears.
        ColorRGBA8,
        ColorRGBA16F,
        ShadowMap,
        Count
    };

    constexpr bool targetHasDepth(TargetFormat format)
    {
        return format == TargetFormat::RGBA8 || format == TargetFormat::RGBA16F || format == TargetFormat::SceneRGBA8
            || format == TargetFormat::SceneRGBA16F || format == TargetFormat::ShadowMap;
    }

    enum class EffectBindingKind
    {
        Texture,
        UniformBuffer
    };
    struct EffectBinding
    {
        std::uint32_t mBinding = 0;
        EffectBindingKind mKind = EffectBindingKind::Texture;
        std::uint32_t mSize = 0;
    };
    struct EffectProgram
    {
        std::span<const std::uint32_t> mVertex;
        std::span<const std::uint32_t> mFragment;
        std::span<const EffectBinding> mBindings;
        // Conservative default for callers without shader reflection.
        bool mVertexTextureReads = true;
    };
    struct EffectInput
    {
        std::uint32_t mBinding = 0;
        TextureHandle mTexture;
        std::span<const std::byte> mData{};
    };

    enum class TextureFormat : std::uint8_t
    {
        RGBA8,
        BC1RGB,
        BC1RGBA,
        BC2,
        BC3,
        BC4,
        BC5,
    };

    enum class TextureWrap : std::uint8_t
    {
        Repeat,
        Mirror,
        Clamp,
        Border,
    };

    enum class TextureFilter : std::uint8_t
    {
        Nearest,
        Linear,
    };

    enum class BorderColor : std::uint8_t
    {
        TransparentBlack,
        Black,
        White,
    };

    struct SamplerState
    {
        TextureWrap mWrapS = TextureWrap::Repeat;
        TextureWrap mWrapT = TextureWrap::Repeat;
        TextureWrap mWrapR = TextureWrap::Repeat;
        TextureFilter mMinFilter = TextureFilter::Linear;
        TextureFilter mMagFilter = TextureFilter::Linear;
        TextureFilter mMipFilter = TextureFilter::Linear;
        BorderColor mBorderColor = BorderColor::TransparentBlack;
        float mAnisotropy = 1;
        bool mUseMipmaps = true;
        auto operator<=>(const SamplerState&) const = default;
    };

    struct TextureLevel
    {
        std::uint32_t mWidth;
        std::uint32_t mHeight;
        std::span<const std::byte> mBytes;
    };

    struct TextureData
    {
        TextureFormat mFormat = TextureFormat::RGBA8;
        bool mSrgb = false;
        // Only RGBA8 data can request generated mips. Explicit authored levels
        // are retained exactly and take precedence over generation.
        bool mGenerateMipmaps = false;
        std::span<const TextureLevel> mLevels;
    };

    enum class AlphaCompare : std::uint8_t
    {
        Always,
        Never,
        Less,
        LessEqual,
        Equal,
        NotEqual,
        GreaterEqual,
        Greater,
    };

    // The initial raster pipeline consumes packed vertex color and RGBA8
    // textures. Additional material pipelines can use separate vertex layouts.
    struct Vertex
    {
        std::array<float, 3> mPosition;
        std::array<float, 2> mUV;
        std::array<std::uint8_t, 4> mColor{ 255, 255, 255, 255 };
        // Keep authored floating-point alpha exact for EQUAL/NOTEQUAL tests.
        // A negative value selects the packed alpha for ordinary callers.
        float mAlpha = -1;
        std::array<float, 2> mMaskUV{};
        std::array<float, 3> mNormal{ 0, 0, 1 };
    };
    static_assert(sizeof(Vertex) == 48);

    enum class BlendMode : std::uint8_t
    {
        Opaque,
        Alpha,
        Additive,
        WeightedReplace,
        PremultipliedAlpha,
    };

    struct PointLight
    {
        std::array<float, 4> mPosition{};
        std::array<float, 4> mAmbient{};
        std::array<float, 4> mDiffuse{};
        std::array<float, 4> mSpecular{};
        std::array<float, 4> mAttenuation{ 1, 0, 0, 0 };
    };
    static_assert(sizeof(PointLight) == 80);
    struct LightSet
    {
        std::vector<PointLight> mPoints;
    };

    struct ShadowSet
    {
        static constexpr unsigned MaxMaps = 8;
        std::array<TextureHandle, MaxMaps> mTextures{};
        std::array<std::array<float, 16>, MaxMaps> mViewToShadow{};
        std::array<std::array<float, 16>, MaxMaps> mValidRegions{};
        unsigned mCount = 0;
        unsigned mRegionMask = 0;
        float mMaximumDistance = 0;
        float mFadeStart = 0;
        float mNormalOffset = 0;
        bool mSoft = false;
    };

    struct Draw
    {
        MeshHandle mMesh;
        // Zero uses all indices; dynamic streams may retain spare mesh capacity.
        std::uint32_t mIndexCount = 0;
        // An empty texture selects the renderer's resident white texture.
        TextureHandle mTexture;
        // Column-major, zero-to-one clip depth, following Pass::mReversedDepth.
        // View orientation is handled by
        // the backend viewport; the world remains right handed and Y-up in clip.
        std::array<float, 16> mClipFromModel = IdentityTransform;
        std::array<float, 4> mTint{ 1, 1, 1, 1 };
        float mAlphaCutoff = 0;
        AlphaCompare mAlphaCompare = AlphaCompare::Always;
        BlendMode mBlend = BlendMode::Opaque;
        bool mDoubleSided = false;
        bool mFlipWinding = false;
        bool mUseVertexColor = true;
        bool mDepthTest = true;
        bool mDepthWrite = true;
        // Two affine rows (u and v), applied to the mesh's primary UVs.
        std::array<float, 6> mUVTransform{ 1, 0, 0, 0, 1, 0 };
        bool mClearDepthBefore = false;
        TextureHandle mMaskTexture;
        std::array<float, 6> mMaskUVTransform{ 1, 0, 0, 0, 1, 0 };
        AlphaCompare mDepthCompare = AlphaCompare::LessEqual;
        bool mDistortion = false;
        float mDistortionStrength = 1;
        bool mWater = false;
        // Rational depth coefficients for the water draw's zero-to-one projection.
        std::array<float, 4> mWaterProjection{};
        bool mSky = false;
        int mSkyPass = -1;
        // Water interprets opacity at an optical depth of 128 world units;
        // it fades continuously to clear at the shoreline and opaque at depth.
        float mOpacity = 1;
        bool mReflection = false;
        std::array<float, 4> mClipPlane{};
        bool mClip = false;
        std::array<float, 4> mFogColor{};
        float mFogStart = 0;
        float mFogEnd = 0;
        std::array<float, 16> mViewFromModel = IdentityTransform;
        std::array<float, 16> mNormalFromModel = IdentityTransform;
        std::array<float, 4> mDiffuse{ 1, 1, 1, 1 };
        std::array<float, 4> mAmbient{ 1, 1, 1, 1 };
        std::array<float, 4> mEmission{ 0, 0, 0, 1 };
        std::array<float, 4> mSpecular{};
        std::array<float, 4> mSunPosition{ 0, 0, 1, 0 };
        std::array<float, 4> mSunAmbient{};
        std::array<float, 4> mSunDiffuse{};
        std::array<float, 4> mSunSpecular{};
        std::shared_ptr<const LightSet> mLights;
        float mShininess = 0;
        float mEmissiveMult = 1;
        float mSpecStrength = 1;
        std::uint32_t mVertexColorMode = 4;
        bool mLighting = false;
        bool mClassicFalloff = true;
        bool mShadowCasting = false;
        bool mShadowAlphaTest = false;
        float mDepthBiasConstant = 0;
        float mDepthBiasSlope = 0;
        std::shared_ptr<const ShadowSet> mShadows;
        // Preserve projection coefficients before model/view multiplication;
        // post-processing must not recover them from rounded world matrices.
        std::array<float, 4> mDepthProjection{};
    };

    struct Pass
    {
        std::span<const Draw> mDraws;
        // An empty target selects the game swapchain.
        TextureHandle mTarget;
        std::array<float, 4> mClearColor{ 0, 0, 0, 0 };
        bool mClear = true;
        // Bottom-left viewport coordinates; zero extent selects the full target.
        std::array<std::uint32_t, 4> mViewport{};
        EffectHandle mEffect{};
        std::span<const EffectInput> mEffectInputs{};
        bool mPreserveWorldDepth = false;
        // The caller guarantees every pixel in the viewport is replaced,
        // without blending, discard, sample masking, or clipped geometry.
        // Contents outside the viewport remain untouched.
        bool mOverwrite = false;
        // Draw clip matrices and sampled depth follow this pass convention.
        // Comparison functions retain their logical forward-depth meaning.
        bool mReversedDepth = false;
    };

    struct DeviceInfo
    {
        std::string mName;
        std::uint32_t mApiVersion = 0;
        std::uint32_t mDriverVersion = 0;
        std::uint32_t mVendorId = 0;
        std::uint32_t mDeviceId = 0;
        std::uint64_t mLocalMemoryBytes = 0;
        std::uint64_t mLocalMemoryBudgetBytes = 0;
        bool mFloatNormals = false;
    };

    enum class PresentMode
    {
        Immediate,
        Fifo,
        Adaptive,
    };

    struct BackendOptions
    {
        SDL_Window* mWindow = nullptr;
        std::filesystem::path mShaderPath;
        std::filesystem::path mPipelineCachePath;
        // Optional stable device UUID, encoded as 32 hexadecimal digits.
        std::string mDeviceUuid;
        // Optional launcher-selected PCI address (dddd:bb:dd.f).
        std::string mDevicePciAddress;
        PresentMode mPresentMode = PresentMode::Fifo;
    };

    // Single producer contract: resource changes and rendering are called on
    // the rendering owner thread. Decoding workers publish immutable input.
    // Handles belong to this backend instance. GPU work retains its resources
    // independently of handle removal; clients need not wait before release.
    struct CapturedImage
    {
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;
        std::vector<std::byte> mRgba;
    };

    class Backend
    {
    public:
        virtual ~Backend() = default;
        virtual const DeviceInfo& deviceInfo() const = 0;
        virtual bool ready() { return true; }
        virtual void requestCapture() {}
        virtual CapturedImage takeCapture() { return {}; }
        virtual void setPresentMode(PresentMode) {}
        virtual MeshHandle createMesh(std::span<const Vertex> vertices, std::span<const std::uint32_t> indices) = 0;
        // Update resident geometry in submission order. Earlier frames retain
        // their data until the queue reaches this upload. Empty indices retain
        // the existing topology (the usual skinning and UI update case).
        // With retained topology, a shorter nonempty vertex span replaces its
        // prefix only; the caller limits draws to initialized vertices.
        virtual void updateMesh(MeshHandle, std::span<const Vertex>, std::span<const std::uint32_t>)
        {
            throw std::runtime_error("This renderer does not support mesh updates");
        }
        virtual bool supportsTextureFormat(TextureFormat format, bool srgb) const = 0;
        virtual TextureHandle createTexture(const TextureData& data, const SamplerState& sampler = {}) = 0;
        // Share immutable 2D image storage with another sampler. Releasing the
        // original handle does not invalidate a view or cancel its upload.
        virtual TextureHandle createTextureSamplerView(TextureHandle, const SamplerState&)
        {
            throw std::runtime_error("This renderer does not support shared texture samplers");
        }
        virtual TextureHandle createVolume(
            std::uint32_t, std::uint32_t, std::uint32_t, std::span<const std::byte>, const SamplerState&)
        {
            throw std::runtime_error("Volume textures are unavailable");
        }
        virtual TextureHandle createRenderTarget(
            std::uint32_t, std::uint32_t, const SamplerState& = {}, TargetFormat = TargetFormat::RGBA8)
        {
            throw std::runtime_error("This renderer does not support offscreen targets");
        }
        virtual TextureHandle depthTexture(TextureHandle) { throw std::runtime_error("Depth targets are unavailable"); }
        virtual TextureHandle normalTexture(TextureHandle)
        {
            throw std::runtime_error("Normal targets are unavailable");
        }
        virtual TextureHandle worldDepthTexture(TextureHandle)
        {
            throw std::runtime_error("World depth targets are unavailable");
        }
        virtual EffectHandle createEffect(const EffectProgram&)
        {
            throw std::runtime_error("Native effects are unavailable");
        }
        virtual void releaseEffect(EffectHandle) { throw std::runtime_error("Native effects are unavailable"); }
        TextureHandle createTexture(
            std::uint32_t width, std::uint32_t height, std::span<const std::byte> rgba, bool srgb, bool mipmaps = true)
        {
            const TextureLevel level{ width, height, rgba };
            return createTexture({ TextureFormat::RGBA8, srgb, mipmaps, { &level, 1 } });
        }
        virtual void releaseMesh(MeshHandle mesh) = 0;
        virtual void releaseTexture(TextureHandle texture) = 0;
        // Preserve the caller's draw ordering, including transparent objects.
        // False means the surface is temporarily unavailable (for example,
        // minimized). No normal frame performs a device-wide idle wait.
        virtual bool render(std::span<const Draw> draws, const std::array<float, 4>& clearColor) = 0;
        virtual bool renderPasses(std::span<const Pass> passes)
        {
            if (passes.size() != 1 || passes.front().mTarget)
                throw std::runtime_error("This renderer does not support multiple render views");
            return render(passes.front().mDraws, passes.front().mClearColor);
        }
    };

    // Private, same-build module ABI. Creation and destruction stay paired in
    // the module; errors are copied into caller-owned memory, never thrown
    // through the exported entry point. No graphics API types cross it.
    inline constexpr std::uint32_t BackendAbi = 14;
    using CreateBackend = Backend* (*)(std::uint32_t abi, const BackendOptions*, char* error, std::size_t errorSize);
    using DestroyBackend = void (*)(Backend*);
}

#endif
