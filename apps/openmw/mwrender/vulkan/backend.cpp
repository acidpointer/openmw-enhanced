#include "backend.hpp"

#include "check.hpp"
#include "memory.hpp"
#include "pipelineworker.hpp"

#include <components/render/bufferarena.hpp>
#include <components/render/bufferversions.hpp>
#include <components/render/cachesweep.hpp>
#include <components/render/capture.hpp>
#include <components/render/colorspace.hpp>
#include <components/render/framecache.hpp>
#include <components/render/memorypolicy.hpp>
#include <components/render/meshindices.hpp>
#include <components/render/pipelinecache.hpp>
#include <components/render/raster.hpp>
#include <components/render/reserve.hpp>
#include <components/render/resourcepool.hpp>
#include <components/render/texture.hpp>
#include <components/render/uploadpages.hpp>

#include <SDL_vulkan.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MWRender::Vulkan
{
    namespace
    {
        constexpr std::size_t FramesInFlight = 2;

        constexpr VkFormat textureFormat(Render::TextureFormat format, bool srgb)
        {
            switch (format)
            {
                case Render::TextureFormat::RGBA8:
                    return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
                case Render::TextureFormat::BGRA8:
                    return srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
                case Render::TextureFormat::RGB8:
                    return srgb ? VK_FORMAT_R8G8B8_SRGB : VK_FORMAT_R8G8B8_UNORM;
                case Render::TextureFormat::BGR8:
                    return srgb ? VK_FORMAT_B8G8R8_SRGB : VK_FORMAT_B8G8R8_UNORM;
                case Render::TextureFormat::R8:
                case Render::TextureFormat::Luminance8:
                    return srgb ? VK_FORMAT_R8_SRGB : VK_FORMAT_R8_UNORM;
                case Render::TextureFormat::RG8:
                    return srgb ? VK_FORMAT_R8G8_SRGB : VK_FORMAT_R8G8_UNORM;
                case Render::TextureFormat::LuminanceAlpha8:
                    // Alpha must stay linear; decoding both channels as sRGB is incorrect.
                    return srgb ? VK_FORMAT_UNDEFINED : VK_FORMAT_R8G8_UNORM;
                case Render::TextureFormat::Alpha8:
                    return VK_FORMAT_R8_UNORM;
                case Render::TextureFormat::RGBA32F:
                    return srgb ? VK_FORMAT_UNDEFINED : VK_FORMAT_R32G32B32A32_SFLOAT;
                case Render::TextureFormat::BC6Unsigned:
                    return srgb ? VK_FORMAT_UNDEFINED : VK_FORMAT_BC6H_UFLOAT_BLOCK;
                case Render::TextureFormat::BC6Signed:
                    return srgb ? VK_FORMAT_UNDEFINED : VK_FORMAT_BC6H_SFLOAT_BLOCK;
                case Render::TextureFormat::BC7RGBA:
                    return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
                case Render::TextureFormat::Count:
                    return VK_FORMAT_UNDEFINED;
                case Render::TextureFormat::BC1RGB:
                    return srgb ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK;
                case Render::TextureFormat::BC1RGBA:
                    return srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
                case Render::TextureFormat::BC2:
                    return srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
                case Render::TextureFormat::BC3:
                    return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
                case Render::TextureFormat::BC4:
                    return srgb ? VK_FORMAT_UNDEFINED : VK_FORMAT_BC4_UNORM_BLOCK;
                case Render::TextureFormat::BC5:
                    return srgb ? VK_FORMAT_UNDEFINED : VK_FORMAT_BC5_UNORM_BLOCK;
            }
            return VK_FORMAT_UNDEFINED;
        }

        constexpr VkComponentMapping textureComponents(Render::TextureFormat format)
        {
            constexpr std::array components{ VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ZERO,
                VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                VK_COMPONENT_SWIZZLE_A };
            const auto channels = Render::textureSwizzle(format);
            return { components[static_cast<unsigned>(channels[0])], components[static_cast<unsigned>(channels[1])],
                components[static_cast<unsigned>(channels[2])], components[static_cast<unsigned>(channels[3])] };
        }
        static_assert(textureFormat(Render::TextureFormat::LuminanceAlpha8, true) == VK_FORMAT_UNDEFINED);
        static_assert(textureFormat(Render::TextureFormat::Alpha8, true) == VK_FORMAT_R8_UNORM);
        static_assert(textureComponents(Render::TextureFormat::Alpha8).a == VK_COMPONENT_SWIZZLE_R);

        VkSamplerAddressMode textureWrap(Render::TextureWrap wrap)
        {
            switch (wrap)
            {
                case Render::TextureWrap::Repeat:
                    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                case Render::TextureWrap::Mirror:
                    return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                case Render::TextureWrap::Clamp:
                    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                case Render::TextureWrap::Border:
                    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            }
            throw std::invalid_argument("Unknown Vulkan texture wrapping mode");
        }

        constexpr std::size_t TargetFormats = static_cast<std::size_t>(Render::TargetFormat::Count);
        constexpr std::size_t TextureFormats = static_cast<std::size_t>(Render::TextureFormat::Count);
        constexpr std::array TargetVkFormats{ VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_R8_UNORM, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
            VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_R8_UNORM };
        static_assert(TargetVkFormats.size() == TargetFormats);
        bool hasNormals(Render::TargetFormat format)
        {
            return format == Render::TargetFormat::SceneRGBA8 || format == Render::TargetFormat::SceneRGBA16F;
        }
        struct TargetPasses
        {
            VkRenderPass mClear = VK_NULL_HANDLE;
            VkRenderPass mLoad = VK_NULL_HANDLE;
            VkRenderPass mOverwrite = VK_NULL_HANDLE;
            VkRenderPass mResume = VK_NULL_HANDLE;
            VkRenderPass mDiscardDepthClear = VK_NULL_HANDLE;
            VkRenderPass mDiscardDepthLoad = VK_NULL_HANDLE;
        };

        struct Sampler
        {
            VkDevice mDevice;
            VkSampler mSampler = VK_NULL_HANDLE;
            Render::SamplerState mState;
            explicit Sampler(VkDevice device, const Render::SamplerState& state)
                : mDevice(device)
                , mState(state)
            {
            }
            ~Sampler() { vkDestroySampler(mDevice, mSampler, nullptr); }
        };

        struct Buffer
        {
            VkDevice mDevice;
            VkBuffer mBuffer = VK_NULL_HANDLE;
            Allocation mMemory;
            VkDeviceSize mSize = 0;
            std::uint64_t mLastUploadSubmission = 0;
            explicit Buffer(VkDevice device)
                : mDevice(device)
            {
            }
            ~Buffer() { vkDestroyBuffer(mDevice, mBuffer, nullptr); }
        };

        struct GeometryBuffer
        {
            Render::BufferArena<Buffer>::Slice mRange;
            std::uint64_t mLastSubmission = 0;
        };

        struct Mesh
        {
            Render::BufferVersions<GeometryBuffer> mVertices;
            Render::BufferVersions<GeometryBuffer> mIndices;
            std::uint32_t mIndexCount = 0;
            VkIndexType mIndexType = VK_INDEX_TYPE_UINT32;
            std::size_t mVertexBytes = 0;
            std::size_t mIndexBytes = 0;
            std::optional<std::size_t> mVertexUpload;
            std::optional<std::size_t> mIndexUpload;
            std::uint64_t mLastSubmission = 0;
        };

        struct DescriptorPage
        {
            VkDevice mDevice;
            VkDescriptorPool mPool = VK_NULL_HANDLE;
            std::uint32_t mLive = 0;
            bool mShuttingDown = false;
            explicit DescriptorPage(VkDevice device)
                : mDevice(device)
            {
            }
            ~DescriptorPage() { vkDestroyDescriptorPool(mDevice, mPool, nullptr); }
        };

        struct Image
        {
            VkDevice mDevice;
            VkImage mImage = VK_NULL_HANDLE;
            VkImageView mView = VK_NULL_HANDLE;
            VkImageView mAttachmentView = VK_NULL_HANDLE;
            std::shared_ptr<DescriptorPage> mPool;
            VkDescriptorSet mDescriptor = VK_NULL_HANDLE;
            Allocation mMemory;
            std::uint32_t mWidth = 0;
            std::uint32_t mHeight = 0;
            std::uint32_t mMipLevels = 1;
            // Only immutable uploaded 2D textures may share image/sampler views.
            std::optional<Render::TextureFormat> mTextureFormat;
            bool mSrgb = false;
            std::shared_ptr<Image> mStorage;
            std::size_t mSamplerViews = 0;
            VkSampler mSampler = VK_NULL_HANDLE;
            bool mTarget = false;
            bool mUsesMipmaps = false, mMipsDirty = false, mDepthReadOnly = false;
            VkImageAspectFlags mAspect = VK_IMAGE_ASPECT_COLOR_BIT;
            VkPipelineStageFlags mReaders = 0, mWriter = 0;
            VkPipelineStageFlags mVisible = 0;
            Render::TargetFormat mTargetFormat = Render::TargetFormat::RGBA8;
            VkFramebuffer mFramebuffer = VK_NULL_HANDLE;
            std::shared_ptr<Image> mDepth;
            std::shared_ptr<Image> mNormals;
            std::shared_ptr<Image> mWorldDepth;
            bool mWorldDepthInitialized = false;
            std::shared_ptr<Image> mWaterDepth;
            bool mWaterDepthInitialized = false;
            std::optional<std::size_t> mUpload;
            std::uint64_t mLastSubmission = 0;
            explicit Image(VkDevice device)
                : mDevice(device)
            {
            }
            ~Image()
            {
                if (mDescriptor)
                {
                    if (!mPool->mShuttingDown)
                        vkFreeDescriptorSets(mDevice, mPool->mPool, 1, &mDescriptor);
                    --mPool->mLive;
                }
                vkDestroyFramebuffer(mDevice, mFramebuffer, nullptr);
                if (mStorage)
                    --mStorage->mSamplerViews;
                else
                {
                    vkDestroyImageView(mDevice, mAttachmentView, nullptr);
                    vkDestroyImageView(mDevice, mView, nullptr);
                    vkDestroyImage(mDevice, mImage, nullptr);
                }
            }
        };

        struct EffectObjects
        {
            VkDevice mDevice;
            VkDescriptorSetLayout mDescriptorLayout = VK_NULL_HANDLE;
            VkPipelineLayout mLayout = VK_NULL_HANDLE;
            std::array<VkShaderModule, 2> mModules{};
            explicit EffectObjects(VkDevice device)
                : mDevice(device)
            {
            }
            ~EffectObjects()
            {
                for (auto module : mModules)
                    vkDestroyShaderModule(mDevice, module, nullptr);
                vkDestroyPipelineLayout(mDevice, mLayout, nullptr);
                vkDestroyDescriptorSetLayout(mDevice, mDescriptorLayout, nullptr);
            }
        };
        struct Effect
        {
            struct PipelineEntry
            {
                PipelineWorker::Value mReady;
                std::future<PipelineWorker::Value> mPending;
                std::exception_ptr mError;
            };
            std::shared_ptr<EffectObjects> mObjects;
            std::future<PipelineWorker::Value> mObjectsPending;
            bool mObjectsReady = false;
            std::exception_ptr mObjectsError;
            std::map<std::tuple<std::size_t, VkFormat, Render::EffectBlend>, PipelineEntry> mPipelines;
            bool mVertexTextureReads = true;
            std::vector<Render::EffectBinding> mBindings;
            explicit Effect(VkDevice device)
                : mObjects(std::make_shared<EffectObjects>(device))
            {
            }
        };

        using Staging = Render::UploadPages<Buffer>::Slice;

        struct Upload
        {
            Staging mStaging;
            std::shared_ptr<Mesh> mMesh;
            std::shared_ptr<Image> mImage;
            VkDeviceSize mSize = 0;
            bool mIndices = false;
            std::vector<VkBufferImageCopy> mImageCopies;
            bool mGenerateMipmaps = false;
            std::shared_ptr<GeometryBuffer> mPreserve{};
        };

        struct Frame
        {
            VkCommandPool mPool = VK_NULL_HANDLE;
            VkCommandBuffer mCommands = VK_NULL_HANDLE;
            VkFence mFence = VK_NULL_HANDLE;
            VkSemaphore mAcquired = VK_NULL_HANDLE;
            std::vector<std::shared_ptr<Buffer>> mUploads;
            std::vector<std::shared_ptr<void>> mResources;
            std::unique_ptr<Buffer> mInstanceUpload;
            std::unique_ptr<Buffer> mInstances;
            std::size_t mInstanceCapacity = 0;
            std::unique_ptr<Buffer> mLightUpload;
            std::unique_ptr<Buffer> mLights;
            std::size_t mLightCapacity = 0;
            VkDescriptorSet mDataDescriptor = VK_NULL_HANDLE;
            std::vector<VkDescriptorPool> mEffectPools;
            std::size_t mEffectPoolIndex = 0;
            std::vector<VkDescriptorPool> mPersistentEffectPools;
            struct EffectDescriptor
            {
                VkDescriptorSet mSet = VK_NULL_HANDLE;
                VkDescriptorPool mPool = VK_NULL_HANDLE;
                std::shared_ptr<EffectObjects> mObjects;
                std::vector<VkDescriptorBufferInfo> mBuffers;
                std::vector<VkDescriptorImageInfo> mImages;
                std::vector<std::shared_ptr<Image>> mRetainedImages;
            };
            std::vector<EffectDescriptor> mEffectDescriptors;
            std::size_t mEffectDescriptorsUsed = 0;
            std::vector<std::shared_ptr<Buffer>> mUniformPages;
            std::size_t mUniformPage = 0;
            VkDeviceSize mUniformOffset = 0;
            struct UniformSlice
            {
                std::size_t mSize = 0, mInputSize = 0;
                VkDescriptorBufferInfo mBuffer{};
            };
            // Inputs are immutable for renderPasses(). Shared engine spans need
            // one staging copy, even when many effects bind the same block.
            Render::FrameCache<const std::byte*, UniformSlice> mEffectUniforms;
            std::optional<std::uint32_t> mAcquiredImage;
            std::shared_ptr<Image> mWaterDepth;
            bool mWaterDepthInitialized = false;
            VkQueryPool mQueries = VK_NULL_HANDLE;
            struct Region
            {
                std::string mName;
                unsigned mStart, mEnd;
            };
            std::vector<Region> mRegions;
            unsigned mQueryCount = 0;
        };

        constexpr std::size_t BlendCount = 5;
        constexpr std::size_t DepthStride = BlendCount * 4;
        constexpr std::size_t CompareStride = DepthStride * 3;
        constexpr std::size_t RasterKeys = CompareStride * 8;
        // Must match the specialization constant in basic.vert/basic.frag.
        enum class MaterialShader : std::uint32_t
        {
            Surface,
            Water,
            Sky,
            Distortion,
            Overlay,
            SolidSurface,
            Shadow,
            SurfaceShadowed,
            SolidSurfaceShadowed,
            PbrSurface,
            PbrSolidSurface,
            PbrSurfaceShadowed,
            PbrSolidSurfaceShadowed,
            PbrBake,
            Count
        };
        constexpr bool isPbr(MaterialShader shader)
        {
            return shader >= MaterialShader::PbrSurface;
        }
        static_assert(static_cast<unsigned>(MaterialShader::PbrSurface) == 9
            && static_cast<unsigned>(MaterialShader::PbrBake) == 13);
        constexpr std::size_t PipelinesPerTarget = RasterKeys * static_cast<std::size_t>(MaterialShader::Count);

        struct PreparedDraw
        {
            Mesh* mMesh;
            Image* mTexture;
            Image* mMask;
            std::size_t mPipeline;
            std::size_t mRasterKey;
            bool mClearDepthBefore;
            std::uint32_t mIndexCount;
            std::array<float, 8> mFog;
            bool mWater = false;
            VkDescriptorSet mDataDescriptor = VK_NULL_HANDLE;
            bool mReversedDepth = false;
            std::array<float, 2> mDepthBias{};
            VkDescriptorSet mPbrDescriptor = VK_NULL_HANDLE;
            std::uint32_t mPbrFlags = 0;
            std::array<Image*, 2> mPbrTargets{};
        };

        struct PbrDescriptor
        {
            std::shared_ptr<DescriptorPage> mPage;
            VkDescriptorSet mSet = VK_NULL_HANDLE;
            std::uint64_t mSubmission = 0;
            std::array<Image*, 2> mTargets{};
            std::uint32_t mMapFlags = 0;
            ~PbrDescriptor()
            {
                if (mSet)
                {
                    if (!mPage->mShuttingDown)
                        vkFreeDescriptorSets(mPage->mDevice, mPage->mPool, 1, &mSet);
                    --mPage->mLive;
                }
            }
        };
        using PbrDescriptorKey = std::array<std::uint64_t, 4>;
        struct PbrDescriptorHash
        {
            std::size_t operator()(const PbrDescriptorKey& key) const
            {
                std::uint64_t hash = 0;
                for (auto handle : key)
                    hash = std::rotl(hash, 17) ^ handle;
                return static_cast<std::size_t>(hash ^ (hash >> 32));
            }
        };

        struct PreparedPass
        {
            std::size_t mFirst = 0;
            std::size_t mCount = 0;
            std::shared_ptr<Image> mTarget;
            std::array<float, 4> mClearColor;
            bool mClear = true;
            std::array<std::uint32_t, 4> mViewport;
            std::shared_ptr<Effect> mEffect{};
            VkDescriptorSet mEffectDescriptor = VK_NULL_HANDLE;
            VkPipeline mEffectPipeline = VK_NULL_HANDLE;
            bool mReversedDepth = false;
            bool mPreserveWorldDepth = false;
            bool mGenerateMipmaps = false;
            Render::EffectBlend mEffectBlend{};
            bool mDiscardDepthAfter = false;
            bool mGui = false;
            bool mOverwrite = false;
            bool mWater = false;
            std::shared_ptr<Image> mWaterDepth{};
            struct Sample
            {
                Image* mImage;
                VkPipelineStageFlags mStages;
            };
            std::span<const Sample> mSamples{};
            std::string_view mLabel{};
            Render::SceneEnvironment mEnvironment{};
        };

        struct InstanceData
        {
            std::array<float, 16> mClipFromModel;
            std::array<float, 4> mTint;
            std::array<float, 4> mParameters;
            std::array<float, 4> mUVRow0;
            std::array<float, 4> mUVRow1;
            std::array<float, 4> mMaskRow0;
            std::array<float, 4> mMaskRow1;
            std::array<float, 16> mViewFromModel;
            std::array<float, 16> mNormalFromModel;
            std::array<float, 4> mDiffuse;
            std::array<float, 4> mAmbient;
            std::array<float, 4> mEmission;
            std::array<float, 4> mSpecular;
            std::array<float, 4> mSunPosition;
            std::array<float, 4> mSunAmbient;
            std::array<float, 4> mSunDiffuse;
            std::array<float, 4> mSunSpecular;
            std::array<std::uint32_t, 4> mLighting;
            std::array<float, 4> mClipPlane;
        };
        static_assert(sizeof(InstanceData) == 448);

        std::vector<std::uint32_t> readShader(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream)
                throw std::runtime_error("Cannot open Vulkan shader: " + path.string());
            const auto bytes = stream.tellg();
            if (bytes < 20 || bytes > 16 * 1024 * 1024 || bytes % 4 != 0)
                throw std::runtime_error("Invalid Vulkan shader size: " + path.string());
            std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes) / 4);
            stream.seekg(0);
            if (!stream.read(reinterpret_cast<char*>(words.data()), bytes) || words.front() != 0x07230203)
                throw std::runtime_error("Invalid SPIR-V shader: " + path.string());
            return words;
        }

        std::string deviceUuid(const VkPhysicalDeviceIDProperties& properties)
        {
            constexpr char hex[] = "0123456789abcdef";
            std::string result;
            result.reserve(2 * VK_UUID_SIZE);
            for (const auto byte : properties.deviceUUID)
            {
                result.push_back(hex[byte >> 4]);
                result.push_back(hex[byte & 15]);
            }
            return result;
        }

        class VulkanBackend final : public Render::Backend
        {
        public:
            explicit VulkanBackend(const Render::BackendOptions& options)
                : mOptions(options)
            {
            }
            ~VulkanBackend() override;
            void initialize();

            const Render::DeviceInfo& deviceInfo() const override { return mInfo; }
            void requestCapture(bool includeGui) override
            {
                if (mCaptureFrame)
                    takeCapture();
                mCaptureRequested = true;
                mCaptureIncludesGui = includeGui;
            }
            Render::CapturedImage takeCapture() override;
            bool ready() override
            {
                if (SDL_GetWindowFlags(mOptions.mWindow) & SDL_WINDOW_MINIMIZED)
                    return false;
                auto& frame = mFrames[mFrameIndex];
                const auto result = vkWaitForFences(mDevice, 1, &frame.mFence, VK_TRUE, 2'000'000);
                if (result == VK_TIMEOUT)
                    return false;
                check(result, "Check Vulkan frame completion");
                if (frame.mAcquiredImage && mImageFences[*frame.mAcquiredImage])
                {
                    const auto image
                        = vkWaitForFences(mDevice, 1, &mImageFences[*frame.mAcquiredImage], VK_TRUE, 2'000'000);
                    if (image == VK_TIMEOUT)
                        return false;
                    check(image, "Check acquired Vulkan image completion");
                }
                // Release completed leases before preparation requests writable
                // geometry and staging slices, rather than after those requests.
                frame.mResources.clear();
                frame.mUploads.clear();
                return true;
            }
            void setPresentMode(Render::PresentMode mode) override
            {
                if (mOptions.mPresentMode != mode)
                {
                    mOptions.mPresentMode = mode;
                    mResize = true;
                }
            }
            Render::MeshHandle createMesh(
                std::span<const Render::Vertex> vertices, std::span<const std::uint32_t> indices) override;
            void updateMesh(Render::MeshHandle handle, std::span<const Render::Vertex> vertices,
                std::span<const std::uint32_t> indices) override;
            using Render::Backend::createTexture;
            Render::TextureHandle createTextureSamplerView(
                Render::TextureHandle texture, const Render::SamplerState& state) override;
            bool supportsTextureFormat(Render::TextureFormat format, bool srgb) const override;
            bool supportsTexture(const Render::TextureData& data, const Render::SamplerState& state) const override;
            Render::TextureHandle createTexture(
                const Render::TextureData& data, const Render::SamplerState& sampler = {}) override;
            Render::TextureHandle createDeferredTexture(
                const Render::TextureData& data, const Render::SamplerState& sampler) override;
            bool stageTexture(Render::TextureHandle texture, const Render::TextureData& data, std::size_t& bytes) override;
            Render::TextureHandle createRenderTarget(std::uint32_t width, std::uint32_t height,
                const Render::SamplerState& sampler, Render::TargetFormat format, bool mipmaps) override;
            Render::TextureHandle createVolume(std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                std::span<const std::byte> rgba, const Render::SamplerState& sampling) override;
            Render::TextureHandle depthTexture(Render::TextureHandle target) override;
            Render::TextureHandle normalTexture(Render::TextureHandle target) override;
            Render::TextureHandle worldDepthTexture(Render::TextureHandle target) override;
            Render::EffectHandle createEffect(const Render::EffectProgram& program) override;
            void releaseEffect(Render::EffectHandle handle) override { mEffects.erase(handle); }
            void setEffectDiagnostics(bool enabled) override { mDiagnostics = enabled; }
            Render::EffectDiagnostics effectDiagnostics() const override
            {
                return { mGpuTimings, mPipelineMisses, mTargetCreations, mDescriptorUpdates, mGpuAvailable };
            }
            bool prepareEffect(
                Render::EffectHandle handle, Render::TextureHandle target, const Render::EffectBlend& blend) override
            {
                if (!target && !mRenderPass)
                    return false;
                return effectPipeline(*mEffects.get(handle), target ? mTextures.get(target).get() : nullptr, blend)
                    != VK_NULL_HANDLE;
            }
            void releaseMesh(Render::MeshHandle mesh) override;
            void releaseTexture(Render::TextureHandle texture) override;
            bool render(std::span<const Render::Draw> draws, const std::array<float, 4>& clearColor) override;
            bool renderPasses(std::span<const Render::Pass> passes) override;

        private:
            void selectDevice();
            void createDevice();
            void createLayouts();
            void createFrames();
            bool createSwapchain();
            void destroySwapchain();
            void destroyWindowPipelines();
            void refreshSurface();
            VkRenderPass createRenderPass(VkFormat format, bool offscreen, bool clear, bool normals = false,
                bool depth = true, bool overwrite = false, bool preserveDepth = false, bool storeDepth = false,
                bool discardDepth = false);
            void copyWorldDepth(VkCommandBuffer commands, Image& target);
            void generateTargetMips(VkCommandBuffer commands, const Image& target);
            void sampleImage(Frame& frame, Image& image, VkPipelineStageFlags stages);
            unsigned beginTiming(Frame& frame, std::string_view label);
            void endTiming(Frame& frame, unsigned index);
            void collectTimings(Frame& frame);
            std::shared_ptr<Image> createDepthSnapshot(std::uint32_t width, std::uint32_t height);
            void copyDepth(VkCommandBuffer commands, const Image& source, Image& destination, bool& initialized);
            void createPipeline(std::size_t index);
            void warmPipelines(std::size_t target);
            void loadPipelineCache();
            void savePipelineCache() noexcept;
            VkPipeline effectPipeline(Effect& effect, const Image* target, const Render::EffectBlend& blending);
            VkDescriptorSet allocateFrameDescriptor(
                Frame& frame, VkDescriptorSetLayout layout, VkDescriptorPool* persistentPool = nullptr);
            VkDescriptorSet dataDescriptor(Frame& frame, const Render::ShadowSet* shadows);
            VkDescriptorSet effectDescriptor(Frame& frame, Effect& effect, std::span<const Render::EffectInput> inputs);
            void retainGeometry(Frame& frame, const std::shared_ptr<GeometryBuffer>& buffer);
            void recordCapture(VkCommandBuffer commands, std::uint32_t imageIndex);
            void recordUploads(Frame& frame, VkDeviceSize instanceBytes, VkDeviceSize lightBytes);
            void cancelUpload(std::optional<std::size_t>& index);
            bool renderFrame(std::span<const Render::Pass> passes);
            std::unique_ptr<Buffer> createBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, MemoryKind kind);
            std::shared_ptr<Image> createImage(std::uint32_t width, std::uint32_t height, VkFormat format,
                VkImageUsageFlags usage, VkImageAspectFlags aspect, std::uint32_t mipLevels = 1,
                std::uint32_t depth = 1, VkComponentMapping components = {});
            Staging allocateStaging(VkDeviceSize size, VkDeviceSize alignment = 16);
            Staging stage(std::span<const std::byte> bytes);
            std::shared_ptr<GeometryBuffer> allocateGeometry(VkDeviceSize size, VkDeviceSize alignment);
            void allocateDescriptor(Image& image);
            Render::TextureHandle createTexture(const Render::TextureData&, const Render::SamplerState&, bool deferred);
            std::pair<std::shared_ptr<DescriptorPage>, VkDescriptorSet> allocateMaterialSet(
                VkDescriptorSetLayout layout, std::uint32_t bindings,
                std::vector<std::shared_ptr<DescriptorPage>>& pages);
            const PbrDescriptor& pbrDescriptor(Frame& frame, const Render::Draw& draw);
            VkSampler sampler(Render::SamplerState state);

            Render::BackendOptions mOptions;
            Render::DeviceInfo mInfo;
            VkInstance mInstance = VK_NULL_HANDLE;
            VkSurfaceKHR mSurface = VK_NULL_HANDLE;
            VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
            VkPhysicalDeviceProperties mProperties{};
            VkPhysicalDeviceFeatures mFeatures{};
            std::array<std::array<VkFormatProperties, 2>, TextureFormats> mTextureProperties{};
            std::array<std::array<std::array<VkImageFormatProperties, 2>, 2>, TextureFormats> mTextureLimits{};
            std::array<VkFormatProperties, TargetFormats> mTargetProperties{};
            VkDevice mDevice = VK_NULL_HANDLE;
            bool mDynamicRaster = false;
            PFN_vkCmdSetCullModeEXT mSetCullMode = nullptr;
            PFN_vkCmdSetFrontFaceEXT mSetFrontFace = nullptr;
            PFN_vkCmdSetDepthTestEnableEXT mSetDepthTest = nullptr;
            PFN_vkCmdSetDepthWriteEnableEXT mSetDepthWrite = nullptr;
            PFN_vkCmdSetDepthCompareOpEXT mSetDepthCompare = nullptr;
            std::uint32_t mGraphicsFamily = 0;
            std::uint32_t mPresentFamily = 0;
            VkQueue mGraphicsQueue = VK_NULL_HANDLE;
            VkQueue mPresentQueue = VK_NULL_HANDLE;
            std::unique_ptr<MemoryAllocator> mAllocator;
            VkDescriptorSetLayout mDescriptorLayout = VK_NULL_HANDLE;
            VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
            VkPipelineLayout mPbrPipelineLayout = VK_NULL_HANDLE;
            VkDescriptorSetLayout mPbrLayout = VK_NULL_HANDLE;
            VkDescriptorSetLayout mDataLayout = VK_NULL_HANDLE;
            VkDescriptorPool mDataPool = VK_NULL_HANDLE;
            std::vector<Render::PointLight> mLightData;
            struct LightOffset
            {
                const Render::LightSet* mSet = nullptr;
                std::uint32_t mOffset = 0;
                bool mLinear = false;
            };
            std::vector<LightOffset> mLightOffsets;
            std::vector<std::uint32_t> mDrawLightOffsets;
            VkPipelineCache mPipelineCache = VK_NULL_HANDLE;
            std::unique_ptr<PipelineWorker> mPipelineWorker;
            std::uint64_t mPipelineMisses = 0, mTargetCreations = 0, mDescriptorUpdates = 0;
            bool mDiagnostics = false, mGpuAvailable = false;
            unsigned mTimestampBits = 0;
            std::vector<Render::GpuTiming> mGpuTimings;
            std::array<VkShaderModule, 2> mShaderModules{};
            std::array<VkShaderModule, 2> mPbrShaderModules{};
            std::vector<std::unique_ptr<Sampler>> mSamplers;
            std::vector<std::shared_ptr<DescriptorPage>> mDescriptorPools;
            std::vector<std::shared_ptr<DescriptorPage>> mPbrPools;
            std::map<PbrDescriptorKey, std::shared_ptr<PbrDescriptor>> mPbrDescriptors;
            Render::CacheSweep<decltype(mPbrDescriptors)> mPbrDescriptorSweep;
            Render::FrameCache<PbrDescriptorKey, const PbrDescriptor*, PbrDescriptorHash> mFramePbrDescriptors;
            std::array<Frame, FramesInFlight> mFrames;
            std::size_t mFrameIndex = 0;
            std::uint64_t mSubmission = 0;
            VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;
            VkFormat mColorFormat = VK_FORMAT_UNDEFINED;
            VkFormat mDepthFormat = VK_FORMAT_UNDEFINED;
            VkFormat mNormalFormat = VK_FORMAT_UNDEFINED;
            VkExtent2D mExtent{};
            VkExtent2D mDrawableExtent{};
            VkRenderPass mRenderPass = VK_NULL_HANDLE;
            VkRenderPass mMainLoadPass = VK_NULL_HANDLE;
            VkRenderPass mMainResumePass = VK_NULL_HANDLE;
            VkRenderPass mMainWaterClearPass = VK_NULL_HANDLE;
            VkRenderPass mMainWaterLoadPass = VK_NULL_HANDLE;
            std::array<TargetPasses, TargetFormats> mTargetPasses{};
            std::array<VkPipeline, PipelinesPerTarget*(TargetFormats + 1)> mPipelines{};
            std::vector<const Render::Draw*> mFrameDraws;
            std::vector<PreparedPass> mPreparedPasses;
            std::vector<std::vector<PreparedPass::Sample>> mPassSamples;
            std::vector<VkImage> mSwapchainImages;
            std::vector<bool> mSwapchainInitialized;
            std::vector<VkImageView> mSwapchainViews;
            std::vector<std::shared_ptr<Image>> mDepthImages;
            std::vector<VkFramebuffer> mFramebuffers;
            // Presentation waits are associated with images, not CPU frame
            // slots. Reacquisition establishes that the old wait completed.
            std::vector<VkSemaphore> mPresented;
            std::vector<VkFence> mImageFences;
            Render::ResourcePool<Mesh, Render::MeshTag> mMeshes;
            Render::ResourcePool<Image, Render::TextureTag> mTextures;
            Render::TextureHandle mWhiteTexture;
            Render::MeshHandle mFullscreenMesh;
            Render::ResourcePool<Effect, Render::EffectTag> mEffects;
            std::vector<Upload> mPendingUploads;
            struct DeferredTexture
            {
                Render::TextureHandle mHandle;
                Upload mUpload;
                Render::TextureStagingProgress mProgress;
            };
            std::optional<DeferredTexture> mDeferredTexture;
            Render::UploadPages<Buffer> mStagingPages{ MemoryAllocator::blockSize(MemoryKind::Upload) };
            Render::BufferArena<Buffer> mGeometryPages{ MemoryAllocator::blockSize(MemoryKind::Geometry) };
            std::vector<PreparedDraw> mPrepared;
            Render::FrameCache<const Render::ShadowSet*, VkDescriptorSet> mShadowDescriptors;
            std::vector<VkWriteDescriptorSet> mEffectWrites;
            std::vector<VkDescriptorBufferInfo> mEffectBuffers;
            std::vector<VkDescriptorImageInfo> mEffectImages;
            bool mResize = true;
            bool mSurfaceRefresh = false;
            VkColorSpaceKHR mColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            VkSurfaceTransformFlagBitsKHR mSurfaceTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
            VkCompositeAlphaFlagBitsKHR mCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            std::chrono::steady_clock::time_point mNextSurfaceRefresh{};
            bool mFailed = false;
            bool mCaptureRequested = false;
            bool mCaptureIncludesGui = true;
            std::optional<std::size_t> mCaptureFrame;
            std::unique_ptr<Buffer> mReadback;
            VkExtent2D mCaptureExtent{};
            VkFormat mCaptureFormat = VK_FORMAT_UNDEFINED;
            bool mCaptureSupported = false;
        };

        VulkanBackend::~VulkanBackend()
        {
            if (mDevice)
            {
                // Destruction and swapchain recreation may drain the device;
                // ordinary rendering only waits for a reused frame/image.
                if (mPipelineWorker)
                {
                    try
                    {
                        mPipelineWorker->stopAndMerge(mPipelineCache);
                    }
                    catch (...)
                    {
                    }
                    mPipelineWorker.reset();
                }
                vkDeviceWaitIdle(mDevice);
                savePipelineCache();
                // All submissions are complete and these pools/blocks are
                // about to be destroyed in bulk. Avoid thousands of individual
                // descriptor frees and quadratic suballocation coalescing.
                if (mAllocator)
                    mAllocator->beginShutdown();
                mGeometryPages.beginShutdown();
                for (const auto& page : mDescriptorPools)
                    page->mShuttingDown = true;
                for (const auto& page : mPbrPools)
                    page->mShuttingDown = true;
                for (auto& frame : mFrames)
                {
                    frame.mResources.clear();
                    frame.mUploads.clear();
                    frame.mInstances.reset();
                    frame.mInstanceUpload.reset();
                    frame.mLightUpload.reset();
                    frame.mLights.reset();
                    frame.mWaterDepth.reset();
                    frame.mEffectDescriptors.clear();
                    frame.mUniformPages.clear();
                    for (auto pool : frame.mPersistentEffectPools)
                        vkDestroyDescriptorPool(mDevice, pool, nullptr);
                    for (auto pool : frame.mEffectPools)
                        vkDestroyDescriptorPool(mDevice, pool, nullptr);
                    vkDestroyQueryPool(mDevice, frame.mQueries, nullptr);
                    vkDestroySemaphore(mDevice, frame.mAcquired, nullptr);
                    vkDestroyFence(mDevice, frame.mFence, nullptr);
                    vkDestroyCommandPool(mDevice, frame.mPool, nullptr);
                }
                mPendingUploads.clear();
                mDeferredTexture.reset();
                mStagingPages.clear();
                mPreparedPasses.clear();
                mReadback.reset();
                mMeshes = {};
                mGeometryPages.clear();
                mTextures = {};
                mEffects = {};
                destroySwapchain();
                destroyWindowPipelines();
                for (std::size_t i = PipelinesPerTarget; i < mPipelines.size(); ++i)
                    vkDestroyPipeline(mDevice, mPipelines[i], nullptr);
                for (const auto& passes : mTargetPasses)
                {
                    vkDestroyRenderPass(mDevice, passes.mClear, nullptr);
                    vkDestroyRenderPass(mDevice, passes.mLoad, nullptr);
                    vkDestroyRenderPass(mDevice, passes.mOverwrite, nullptr);
                    vkDestroyRenderPass(mDevice, passes.mResume, nullptr);
                    vkDestroyRenderPass(mDevice, passes.mDiscardDepthClear, nullptr);
                    vkDestroyRenderPass(mDevice, passes.mDiscardDepthLoad, nullptr);
                }
                mDescriptorPools.clear();
                mPbrDescriptors.clear();
                mPbrPools.clear();
                mSamplers.clear();
                for (const auto shader : mShaderModules)
                    vkDestroyShaderModule(mDevice, shader, nullptr);
                for (const auto shader : mPbrShaderModules)
                    vkDestroyShaderModule(mDevice, shader, nullptr);
                vkDestroyPipelineCache(mDevice, mPipelineCache, nullptr);
                vkDestroyPipelineLayout(mDevice, mPipelineLayout, nullptr);
                vkDestroyPipelineLayout(mDevice, mPbrPipelineLayout, nullptr);
                vkDestroyDescriptorSetLayout(mDevice, mPbrLayout, nullptr);
                vkDestroyDescriptorSetLayout(mDevice, mDescriptorLayout, nullptr);
                vkDestroyDescriptorPool(mDevice, mDataPool, nullptr);
                vkDestroyDescriptorSetLayout(mDevice, mDataLayout, nullptr);
                mAllocator.reset();
                vkDestroyDevice(mDevice, nullptr);
            }
            if (mSurface)
                vkDestroySurfaceKHR(mInstance, mSurface, nullptr);
            if (mInstance)
                vkDestroyInstance(mInstance, nullptr);
        }

        void VulkanBackend::initialize()
        {
            if (!mOptions.mWindow || !(SDL_GetWindowFlags(mOptions.mWindow) & SDL_WINDOW_VULKAN))
                throw std::invalid_argument("Vulkan renderer requires an SDL Vulkan window");
            if (!mOptions.mDeviceUuid.empty()
                && (mOptions.mDeviceUuid.size() != 2 * VK_UUID_SIZE
                    || mOptions.mDeviceUuid.find_first_not_of("0123456789abcdef") != std::string::npos))
                throw std::invalid_argument("Vulkan device UUID must contain 32 lowercase hexadecimal digits");

            unsigned extensionCount = 0;
            if (!SDL_Vulkan_GetInstanceExtensions(mOptions.mWindow, &extensionCount, nullptr))
                throw std::runtime_error(SDL_GetError());
            std::vector<const char*> extensions(extensionCount);
            if (!SDL_Vulkan_GetInstanceExtensions(mOptions.mWindow, &extensionCount, extensions.data()))
                throw std::runtime_error(SDL_GetError());
            auto application = vkInfo<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
            application.pApplicationName = "OpenMW";
            application.pEngineName = "OpenMW";
            application.apiVersion = VK_API_VERSION_1_1;
            auto info = vkInfo<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
            info.pApplicationInfo = &application;
            info.enabledExtensionCount = extensionCount;
            info.ppEnabledExtensionNames = extensions.data();
            check(vkCreateInstance(&info, nullptr, &mInstance), "Create Vulkan instance");
            if (!SDL_Vulkan_CreateSurface(mOptions.mWindow, mInstance, &mSurface))
                throw std::runtime_error(SDL_GetError());
            selectDevice();
            createDevice();
            createLayouts();
            // Attachment formats are known at startup. First-use compilation
            // during an effect change would stall the preparation phase.
            for (std::size_t slot = 0; slot < TargetFormats; ++slot)
                if (mTargetPasses[slot].mClear
                    && (Render::targetHasDepth(static_cast<Render::TargetFormat>(slot))
                        || (mOptions.mPbr
                            && static_cast<Render::TargetFormat>(slot) == Render::TargetFormat::ColorRGBA8)))
                    warmPipelines(slot + 1);
            createFrames();
            const std::array<std::byte, 4> white{ std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 },
                std::byte{ 255 } };
            mWhiteTexture = createTexture(1, 1, white, false);
            std::array<Render::Vertex, 3> vertices{};
            vertices[0].mPosition = { -1, -1, 0 };
            vertices[1].mPosition = { 3, -1, 0 };
            vertices[2].mPosition = { -1, 3, 0 };
            const std::array<std::uint32_t, 3> indices{ 0, 1, 2 };
            mFullscreenMesh = createMesh(vertices, indices);
        }

        void VulkanBackend::selectDevice()
        {
            std::uint32_t count = 0;
            check(vkEnumeratePhysicalDevices(mInstance, &count, nullptr), "Enumerate Vulkan devices");
            std::vector<VkPhysicalDevice> devices(count);
            check(vkEnumeratePhysicalDevices(mInstance, &count, devices.data()), "Read Vulkan devices");
            for (const auto candidate : devices)
            {
                auto identity = vkInfo<VkPhysicalDeviceIDProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
                auto properties
                    = vkInfo<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &identity);
                vkGetPhysicalDeviceProperties2(candidate, &properties);
                if (properties.properties.apiVersion < VK_API_VERSION_1_1
                    || (properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && mOptions.mDeviceUuid.empty())
                    || (!mOptions.mDeviceUuid.empty() && deviceUuid(identity) != mOptions.mDeviceUuid))
                    continue;
                std::uint32_t extensionCount = 0;
                check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr),
                    "Enumerate Vulkan device extensions");
                std::vector<VkExtensionProperties> extensions(extensionCount);
                check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, extensions.data()),
                    "Read Vulkan device extensions");
                if (std::none_of(extensions.begin(), extensions.end(), [](const auto& extension) {
                        return std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
                    }))
                    continue;
                if (!mOptions.mDevicePciAddress.empty())
                {
                    const bool pciSupported
                        = std::any_of(extensions.begin(), extensions.end(), [](const auto& extension) {
                              return std::strcmp(extension.extensionName, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME) == 0;
                          });
                    if (!pciSupported)
                        continue;
                    auto pci = vkInfo<VkPhysicalDevicePCIBusInfoPropertiesEXT>(
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT);
                    auto busProperties
                        = vkInfo<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pci);
                    vkGetPhysicalDeviceProperties2(candidate, &busProperties);
                    std::array<char, 64> address{};
                    std::snprintf(address.data(), address.size(), "%04x:%02x:%02x.%x", pci.pciDomain, pci.pciBus,
                        pci.pciDevice, pci.pciFunction);
                    if (mOptions.mDevicePciAddress != address.data())
                        continue;
                }
                std::uint32_t familyCount = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
                std::vector<VkQueueFamilyProperties> families(familyCount);
                vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
                std::optional<std::uint32_t> graphics;
                std::optional<std::uint32_t> present;
                for (std::uint32_t i = 0; i < familyCount; ++i)
                {
                    VkBool32 supported = VK_FALSE;
                    check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, mSurface, &supported),
                        "Check presentation queue");
                    if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && !graphics)
                        graphics = i;
                    if (families[i].queueCount && supported && !present)
                        present = i;
                    if (families[i].queueCount && supported && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                    {
                        graphics = i;
                        present = i;
                        break;
                    }
                }
                if (!graphics || !present)
                    continue;
                std::uint32_t formats = 0;
                std::uint32_t modes = 0;
                check(vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, mSurface, &formats, nullptr),
                    "Check surface formats");
                check(vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, mSurface, &modes, nullptr),
                    "Check present modes");
                if (!formats || !modes)
                    continue;
                mPhysicalDevice = candidate;
                mProperties = properties.properties;
                mGraphicsFamily = *graphics;
                mPresentFamily = *present;
                mInfo = { mProperties.deviceName, mProperties.apiVersion, mProperties.driverVersion,
                    mProperties.vendorID, mProperties.deviceID };
                // Respect the loader/environment's device ordering instead of
                // silently preferring a discrete GPU over the selected one.
                return;
            }
            throw std::runtime_error("No selected hardware Vulkan 1.1 device can present to this SDL window");
        }

        void VulkanBackend::createDevice()
        {
            const float priority = 1;
            std::array<VkDeviceQueueCreateInfo, 2> queues{};
            queues[0] = vkInfo<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
            queues[0].queueFamilyIndex = mGraphicsFamily;
            queues[0].queueCount = 1;
            queues[0].pQueuePriorities = &priority;
            queues[1] = queues[0];
            queues[1].queueFamilyIndex = mPresentFamily;
            std::uint32_t extensionCount = 0;
            check(vkEnumerateDeviceExtensionProperties(mPhysicalDevice, nullptr, &extensionCount, nullptr),
                "Count device extensions");
            std::vector<VkExtensionProperties> available(extensionCount);
            check(vkEnumerateDeviceExtensionProperties(mPhysicalDevice, nullptr, &extensionCount, available.data()),
                "Read device extensions");
            std::vector<const char*> extensions{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
            const bool memoryBudget = std::any_of(available.begin(), available.end(), [](const auto& extension) {
                return std::strcmp(extension.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0;
            });
            if (memoryBudget)
                extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
            auto dynamicRaster = vkInfo<VkPhysicalDeviceExtendedDynamicStateFeaturesEXT>(
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT);
            if (std::any_of(available.begin(), available.end(), [](const auto& extension) {
                    return std::strcmp(extension.extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) == 0;
                }))
            {
                auto features
                    = vkInfo<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &dynamicRaster);
                vkGetPhysicalDeviceFeatures2(mPhysicalDevice, &features);
                if (dynamicRaster.extendedDynamicState)
                {
                    extensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
                    mDynamicRaster = true;
                }
            }
            auto info = vkInfo<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
            info.queueCreateInfoCount = mGraphicsFamily == mPresentFamily ? 1 : 2;
            info.pQueueCreateInfos = queues.data();
            info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
            info.ppEnabledExtensionNames = extensions.data();
            if (mDynamicRaster)
                info.pNext = &dynamicRaster;
            VkPhysicalDeviceFeatures supported{};
            vkGetPhysicalDeviceFeatures(mPhysicalDevice, &supported);
            mFeatures.textureCompressionBC = supported.textureCompressionBC;
            mFeatures.samplerAnisotropy = supported.samplerAnisotropy;
            mFeatures.depthClamp = supported.depthClamp;
            info.pEnabledFeatures = &mFeatures;
            check(vkCreateDevice(mPhysicalDevice, &info, nullptr, &mDevice), "Create Vulkan device");
            if (mDynamicRaster)
            {
                mSetCullMode
                    = reinterpret_cast<PFN_vkCmdSetCullModeEXT>(vkGetDeviceProcAddr(mDevice, "vkCmdSetCullModeEXT"));
                mSetFrontFace
                    = reinterpret_cast<PFN_vkCmdSetFrontFaceEXT>(vkGetDeviceProcAddr(mDevice, "vkCmdSetFrontFaceEXT"));
                mSetDepthTest = reinterpret_cast<PFN_vkCmdSetDepthTestEnableEXT>(
                    vkGetDeviceProcAddr(mDevice, "vkCmdSetDepthTestEnableEXT"));
                mSetDepthWrite = reinterpret_cast<PFN_vkCmdSetDepthWriteEnableEXT>(
                    vkGetDeviceProcAddr(mDevice, "vkCmdSetDepthWriteEnableEXT"));
                mSetDepthCompare = reinterpret_cast<PFN_vkCmdSetDepthCompareOpEXT>(
                    vkGetDeviceProcAddr(mDevice, "vkCmdSetDepthCompareOpEXT"));
                mDynamicRaster = mSetCullMode && mSetFrontFace && mSetDepthTest && mSetDepthWrite && mSetDepthCompare;
            }
            std::uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> queueProperties(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &familyCount, queueProperties.data());
            mTimestampBits = queueProperties.at(mGraphicsFamily).timestampValidBits;
            vkGetDeviceQueue(mDevice, mGraphicsFamily, 0, &mGraphicsQueue);
            vkGetDeviceQueue(mDevice, mPresentFamily, 0, &mPresentQueue);
            auto budget = vkInfo<VkPhysicalDeviceMemoryBudgetPropertiesEXT>(
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT);
            auto memoryProperties = vkInfo<VkPhysicalDeviceMemoryProperties2>(
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, memoryBudget ? &budget : nullptr);
            vkGetPhysicalDeviceMemoryProperties2(mPhysicalDevice, &memoryProperties);
            const auto& memory = memoryProperties.memoryProperties;
            for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i)
                if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                {
                    mInfo.mLocalMemoryBytes = std::max(mInfo.mLocalMemoryBytes, memory.memoryHeaps[i].size);
                    mInfo.mLocalMemoryBudgetBytes = std::max(
                        mInfo.mLocalMemoryBudgetBytes, std::min(memory.memoryHeaps[i].size, budget.heapBudget[i]));
                }
            mAllocator = std::make_unique<MemoryAllocator>(mDevice, memory);
            // Format capabilities are immutable for the selected device.
            // Texture preparation must not query the driver repeatedly for
            // every material, conversion decision and upload.
            for (std::size_t i = 0; i < TextureFormats; ++i)
                for (unsigned srgb = 0; srgb < 2; ++srgb)
                {
                    const auto format = textureFormat(static_cast<Render::TextureFormat>(i), srgb != 0);
                    if (format == VK_FORMAT_UNDEFINED)
                        continue;
                    vkGetPhysicalDeviceFormatProperties(mPhysicalDevice, format, &mTextureProperties[i][srgb]);
                    for (unsigned generate = 0; generate < 2; ++generate)
                    {
                        const auto status = vkGetPhysicalDeviceImageFormatProperties(mPhysicalDevice, format,
                            VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                | (generate ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0),
                            0, &mTextureLimits[i][srgb][generate]);
                        if (status == VK_ERROR_FORMAT_NOT_SUPPORTED)
                            mTextureLimits[i][srgb][generate] = {};
                        else
                            check(status, "Query Vulkan texture limits");
                    }
                }
            mDepthFormat = VK_FORMAT_UNDEFINED;
            for (const auto candidate : { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM, VK_FORMAT_D24_UNORM_S8_UINT })
            {
                VkFormatProperties properties;
                vkGetPhysicalDeviceFormatProperties(mPhysicalDevice, candidate, &properties);
                if ((properties.optimalTilingFeatures
                        & (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
                            | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
                    == (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
                        | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
                {
                    mDepthFormat = candidate;
                    break;
                }
            }
            if (mDepthFormat == VK_FORMAT_UNDEFINED)
                throw std::runtime_error("Vulkan device has no supported depth attachment format");
            // Match OpenMW's floating-point normal target. Additive terrain
            // passes may exceed [0,1] before their final weighted result; UNORM
            // clamping destroys that information and corrupts AO inputs.
            for (const auto candidate : { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT })
            {
                VkFormatProperties properties{};
                vkGetPhysicalDeviceFormatProperties(mPhysicalDevice, candidate, &properties);
                constexpr auto required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
                    | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
                    | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
                if ((properties.optimalTilingFeatures & required) == required)
                {
                    mNormalFormat = candidate;
                    break;
                }
            }
            mInfo.mFloatNormals = mNormalFormat != VK_FORMAT_UNDEFINED;
            mInfo.mMaxSampledImageDescriptors = std::min(mProperties.limits.maxPerStageDescriptorSamplers,
                mProperties.limits.maxPerStageDescriptorSampledImages);
            for (std::size_t i = 0; i < TargetFormats; ++i)
            {
                auto& properties = mTargetProperties[i];
                vkGetPhysicalDeviceFormatProperties(mPhysicalDevice, TargetVkFormats[i], &properties);
                constexpr auto required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
                    | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
                if ((properties.optimalTilingFeatures & required) != required)
                    continue;
                if (hasNormals(static_cast<Render::TargetFormat>(i)) && mNormalFormat == VK_FORMAT_UNDEFINED)
                    continue;
                const auto targetFormat = static_cast<Render::TargetFormat>(i);
                mTargetPasses[i] = { createRenderPass(TargetVkFormats[i], true, true, hasNormals(targetFormat),
                                         Render::targetHasDepth(targetFormat)),
                    createRenderPass(TargetVkFormats[i], true, false, hasNormals(targetFormat),
                        Render::targetHasDepth(targetFormat)),
                    Render::targetHasDepth(targetFormat)
                        ? VK_NULL_HANDLE
                        : createRenderPass(TargetVkFormats[i], true, false, false, false, true),
                    Render::targetHasDepth(targetFormat)
                        ? createRenderPass(TargetVkFormats[i], true, false, hasNormals(targetFormat), true, false, true)
                        : VK_NULL_HANDLE };
                if (Render::targetHasDepth(targetFormat))
                {
                    mTargetPasses[i].mDiscardDepthClear = createRenderPass(
                        TargetVkFormats[i], true, true, hasNormals(targetFormat), true, false, false, false, true);
                    mTargetPasses[i].mDiscardDepthLoad = createRenderPass(
                        TargetVkFormats[i], true, false, hasNormals(targetFormat), true, false, false, false, true);
                }
            }
        }

        void VulkanBackend::createLayouts()
        {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding = 0;
            binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binding.descriptorCount = 1;
            binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            auto descriptor
                = vkInfo<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
            descriptor.bindingCount = 1;
            descriptor.pBindings = &binding;
            check(vkCreateDescriptorSetLayout(mDevice, &descriptor, nullptr, &mDescriptorLayout),
                "Create texture layout");
            auto layout = vkInfo<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
            std::array<VkDescriptorSetLayoutBinding, 11> dataBindings{};
            for (unsigned i = 0; i < dataBindings.size(); ++i)
            {
                dataBindings[i].binding = i;
                dataBindings[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                    : i == 2                           ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                       : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                dataBindings[i].descriptorCount = 1;
                dataBindings[i].stageFlags = i < 2 && !mOptions.mPbr ? VK_SHADER_STAGE_VERTEX_BIT
                    : i <= 2 ? VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                             : VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            descriptor.bindingCount = dataBindings.size();
            descriptor.pBindings = dataBindings.data();
            check(vkCreateDescriptorSetLayout(mDevice, &descriptor, nullptr, &mDataLayout), "Create frame data layout");
            const std::array dataSizes{ VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * FramesInFlight },
                VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FramesInFlight },
                VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 * FramesInFlight } };
            auto pool = vkInfo<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
            pool.maxSets = FramesInFlight;
            pool.poolSizeCount = dataSizes.size();
            pool.pPoolSizes = dataSizes.data();
            check(vkCreateDescriptorPool(mDevice, &pool, nullptr, &mDataPool), "Create frame data descriptor pool");
            std::array materialLayouts{ mDescriptorLayout, mDescriptorLayout, mDataLayout, mDescriptorLayout };
            layout.setLayoutCount = materialLayouts.size();
            layout.pSetLayouts = materialLayouts.data();
            const VkPushConstantRange sceneConstants{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                32 + sizeof(Render::SceneEnvironment) };
            static_assert(32 + sizeof(Render::SceneEnvironment) <= 128);
            layout.pushConstantRangeCount = 1;
            layout.pPushConstantRanges = &sceneConstants;
            check(vkCreatePipelineLayout(mDevice, &layout, nullptr, &mPipelineLayout), "Create Vulkan pipeline layout");
            if (mOptions.mPbr)
            {
                std::array<VkDescriptorSetLayoutBinding, 4> maps{};
                for (std::uint32_t i = 0; i < maps.size(); ++i)
                {
                    maps[i] = binding;
                    maps[i].binding = i;
                }
                descriptor.bindingCount = maps.size();
                descriptor.pBindings = maps.data();
                check(vkCreateDescriptorSetLayout(mDevice, &descriptor, nullptr, &mPbrLayout),
                    "Create Vulkan PBR texture layout");
                materialLayouts[3] = mPbrLayout;
                check(vkCreatePipelineLayout(mDevice, &layout, nullptr, &mPbrPipelineLayout),
                    "Create Vulkan PBR pipeline layout");
            }
            loadPipelineCache();
            // Cache seeding and worker initialization belong to startup, not
            // the first in-game effect whose shader compilation completes.
            mPipelineWorker = std::make_unique<PipelineWorker>(mDevice, mPipelineCache);
        }

        void VulkanBackend::loadPipelineCache()
        {
            std::vector<std::byte> bytes;
            if (!mOptions.mPipelineCachePath.empty())
            {
                std::ifstream input(mOptions.mPipelineCachePath, std::ios::binary | std::ios::ate);
                const auto size = input.tellg();
                if (size >= 32 && size <= 64 * 1024 * 1024)
                {
                    bytes.resize(static_cast<std::size_t>(size));
                    input.seekg(0);
                    if (!input.read(reinterpret_cast<char*>(bytes.data()), size)
                        || !Render::compatiblePipelineCache(
                            bytes, mProperties.vendorID, mProperties.deviceID, mProperties.pipelineCacheUUID))
                        bytes.clear();
                }
            }
            auto info = vkInfo<VkPipelineCacheCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO);
            info.initialDataSize = bytes.size();
            info.pInitialData = bytes.empty() ? nullptr : bytes.data();
            auto result = vkCreatePipelineCache(mDevice, &info, nullptr, &mPipelineCache);
            if (result != VK_SUCCESS && !bytes.empty())
            {
                info.initialDataSize = 0;
                info.pInitialData = nullptr;
                result = vkCreatePipelineCache(mDevice, &info, nullptr, &mPipelineCache);
            }
            check(result, "Create Vulkan pipeline cache");
        }

        void VulkanBackend::savePipelineCache() noexcept
        {
            if (!mPipelineCache || mOptions.mPipelineCachePath.empty())
                return;
            std::filesystem::path temporary;
            try
            {
                std::size_t size = 0;
                if (vkGetPipelineCacheData(mDevice, mPipelineCache, &size, nullptr) != VK_SUCCESS || !size
                    || size > 64 * 1024 * 1024)
                    return;
                std::vector<std::byte> bytes(size);
                if (vkGetPipelineCacheData(mDevice, mPipelineCache, &size, bytes.data()) != VK_SUCCESS)
                    return;
                std::error_code error;
                std::filesystem::create_directories(mOptions.mPipelineCachePath.parent_path(), error);
                if (error)
                    return;
                temporary = mOptions.mPipelineCachePath;
                temporary += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(size));
                output.close();
                if (output)
                    std::filesystem::rename(temporary, mOptions.mPipelineCachePath, error);
                std::filesystem::remove(temporary, error);
            }
            catch (...)
            {
                std::error_code error;
                if (!temporary.empty())
                    std::filesystem::remove(temporary, error);
            }
        }

        VkSampler VulkanBackend::sampler(Render::SamplerState state)
        {
            Render::validateSampler(state);
            state.mAnisotropy = mFeatures.samplerAnisotropy
                ? std::min(state.mAnisotropy, mProperties.limits.maxSamplerAnisotropy)
                : 1;
            for (const auto& existing : mSamplers)
                if (existing->mState == state)
                    return existing->mSampler;
            auto result = std::make_unique<Sampler>(mDevice, state);
            auto info = vkInfo<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
            info.magFilter = state.mMagFilter == Render::TextureFilter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            info.minFilter = state.mMinFilter == Render::TextureFilter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            info.mipmapMode = state.mMipFilter == Render::TextureFilter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                                : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            info.addressModeU = textureWrap(state.mWrapS);
            info.addressModeV = textureWrap(state.mWrapT);
            info.addressModeW = textureWrap(state.mWrapR);
            info.maxLod = state.mUseMipmaps ? VK_LOD_CLAMP_NONE : 0;
            info.anisotropyEnable = state.mAnisotropy > 1;
            info.maxAnisotropy = state.mAnisotropy;
            switch (state.mBorderColor)
            {
                case Render::BorderColor::TransparentBlack:
                    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
                    break;
                case Render::BorderColor::Black:
                    info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
                    break;
                case Render::BorderColor::White:
                    info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
                    break;
                default:
                    throw std::invalid_argument("Unknown Vulkan texture border color");
            }
            check(vkCreateSampler(mDevice, &info, nullptr, &result->mSampler), "Create Vulkan texture sampler");
            const auto handle = result->mSampler;
            mSamplers.push_back(std::move(result));
            return handle;
        }

        void VulkanBackend::createFrames()
        {
            for (auto& frame : mFrames)
            {
                auto pool = vkInfo<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
                pool.queueFamilyIndex = mGraphicsFamily;
                check(vkCreateCommandPool(mDevice, &pool, nullptr, &frame.mPool), "Create frame command pool");
                auto commands = vkInfo<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
                commands.commandPool = frame.mPool;
                commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                commands.commandBufferCount = 1;
                check(vkAllocateCommandBuffers(mDevice, &commands, &frame.mCommands), "Allocate frame commands");
                auto fence = vkInfo<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
                fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
                check(vkCreateFence(mDevice, &fence, nullptr, &frame.mFence), "Create frame fence");
                auto semaphore = vkInfo<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
                check(vkCreateSemaphore(mDevice, &semaphore, nullptr, &frame.mAcquired), "Create acquire semaphore");
                auto dataSet = vkInfo<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
                dataSet.descriptorPool = mDataPool;
                dataSet.descriptorSetCount = 1;
                dataSet.pSetLayouts = &mDataLayout;
                check(vkAllocateDescriptorSets(mDevice, &dataSet, &frame.mDataDescriptor), "Allocate frame data set");
            }
        }

        std::unique_ptr<Buffer> VulkanBackend::createBuffer(
            VkDeviceSize bytes, VkBufferUsageFlags usage, MemoryKind kind)
        {
            auto buffer = std::make_unique<Buffer>(mDevice);
            auto info = vkInfo<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
            info.size = bytes;
            buffer->mSize = bytes;
            info.usage = usage;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(vkCreateBuffer(mDevice, &info, nullptr, &buffer->mBuffer), "Create Vulkan buffer");
            auto request = vkInfo<VkBufferMemoryRequirementsInfo2>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2);
            request.buffer = buffer->mBuffer;
            auto dedicated = vkInfo<VkMemoryDedicatedRequirements>(VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);
            auto requirements = vkInfo<VkMemoryRequirements2>(VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated);
            vkGetBufferMemoryRequirements2(mDevice, &request, &requirements);
            auto allocation = vkInfo<VkMemoryDedicatedAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
            allocation.buffer = buffer->mBuffer;
            buffer->mMemory = mAllocator->allocate(requirements.memoryRequirements, kind,
                dedicated.requiresDedicatedAllocation
                        || (dedicated.prefersDedicatedAllocation
                            && requirements.memoryRequirements.size >= MemoryAllocator::blockSize(kind))
                    ? &allocation
                    : nullptr);
            check(vkBindBufferMemory(mDevice, buffer->mBuffer, buffer->mMemory.memory(), buffer->mMemory.offset()),
                "Bind Vulkan buffer memory");
            return buffer;
        }

        std::shared_ptr<Image> VulkanBackend::createImage(std::uint32_t width, std::uint32_t height, VkFormat format,
            VkImageUsageFlags usage, VkImageAspectFlags aspect, std::uint32_t mipLevels, std::uint32_t depth,
            VkComponentMapping components)
        {
            auto image = std::make_shared<Image>(mDevice);
            image->mWidth = width;
            image->mHeight = height;
            image->mMipLevels = mipLevels;
            image->mAspect = aspect;
            auto info = vkInfo<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
            info.imageType = depth == 1 ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_3D;
            info.format = format;
            info.extent = { width, height, depth };
            info.mipLevels = mipLevels;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = usage;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(vkCreateImage(mDevice, &info, nullptr, &image->mImage), "Create Vulkan image");
            auto request = vkInfo<VkImageMemoryRequirementsInfo2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2);
            request.image = image->mImage;
            auto dedicated = vkInfo<VkMemoryDedicatedRequirements>(VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);
            auto requirements = vkInfo<VkMemoryRequirements2>(VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated);
            vkGetImageMemoryRequirements2(mDevice, &request, &requirements);
            auto allocation = vkInfo<VkMemoryDedicatedAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
            allocation.image = image->mImage;
            image->mMemory = mAllocator->allocate(requirements.memoryRequirements, MemoryKind::Image,
                dedicated.requiresDedicatedAllocation
                        || (dedicated.prefersDedicatedAllocation
                            && requirements.memoryRequirements.size
                                >= mAllocator->preferredBlockSize(MemoryKind::Image))
                    ? &allocation
                    : nullptr);
            check(vkBindImageMemory(mDevice, image->mImage, image->mMemory.memory(), image->mMemory.offset()),
                "Bind Vulkan image memory");
            auto view = vkInfo<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
            view.image = image->mImage;
            view.viewType = depth == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_3D;
            view.format = format;
            view.components = components;
            view.subresourceRange = { aspect, 0, mipLevels, 0, 1 };
            check(vkCreateImageView(mDevice, &view, nullptr, &image->mView), "Create Vulkan image view");
            return image;
        }

        Staging VulkanBackend::allocateStaging(VkDeviceSize size, VkDeviceSize alignment)
        {
            const auto minimum = std::max(size, MemoryAllocator::blockSize(MemoryKind::Upload));
            const auto preferred = size > MemoryAllocator::blockSize(MemoryKind::Upload)
                ? mAllocator->preferredBlockSize(MemoryKind::Image)
                : minimum;
            return mStagingPages.allocate(
                size, alignment,
                [&](VkDeviceSize capacity) -> std::shared_ptr<Buffer> {
                    try
                    {
                        return createBuffer(capacity,
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, MemoryKind::Upload);
                    }
                    catch (const AllocationFailure& error)
                    {
                        if (capacity > minimum && error.retryable())
                            return {}; // Retry the requested size, with accurate page metadata.
                        throw;
                    }
                },
                preferred);
        }

        Staging VulkanBackend::stage(std::span<const std::byte> bytes)
        {
            auto staging = allocateStaging(bytes.size());
            std::memcpy(staging.mPage->mMemory.mapped() + staging.mOffset, bytes.data(), bytes.size());
            return staging;
        }

        std::shared_ptr<GeometryBuffer> VulkanBackend::allocateGeometry(VkDeviceSize size, VkDeviceSize alignment)
        {
            auto buffer = std::make_shared<GeometryBuffer>();
            // Whole-vertex offsets allow shared page bindings with baseVertex.
            buffer->mRange = mGeometryPages.allocate(size, alignment, [&](VkDeviceSize capacity) {
                return createBuffer(capacity,
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    MemoryKind::Geometry);
            });
            return buffer;
        }

        Render::MeshHandle VulkanBackend::createMesh(
            std::span<const Render::Vertex> vertices, std::span<const std::uint32_t> indices)
        {
            if (vertices.empty() || indices.empty() || indices.size() % 3 != 0
                || indices.size() > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("Vulkan mesh requires nonempty triangle-list vertices and indices");
            if (std::any_of(indices.begin(), indices.end(), [&](auto index) { return index >= vertices.size(); }))
                throw std::invalid_argument("Vulkan mesh index exceeds its vertex count");
            auto mesh = std::make_shared<Mesh>();
            mesh->mIndexCount = static_cast<std::uint32_t>(indices.size());
            const bool narrow = Render::narrowMeshIndices(vertices.size());
            mesh->mIndexType = narrow ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            mesh->mVertexBytes = vertices.size_bytes();
            mesh->mIndexBytes = Render::meshIndexBytes(indices.size(), narrow);
            mesh->mVertices.writable([&] {
                return allocateGeometry(vertices.size_bytes(), std::lcm(std::size_t{ 16 }, sizeof(Render::Vertex)));
            });
            mesh->mIndices.writable([&] { return allocateGeometry(mesh->mIndexBytes, 4); });
            auto vertexStaging = stage(std::as_bytes(vertices));
            auto indexStaging = allocateStaging(mesh->mIndexBytes);
            Render::writeMeshIndices(
                { indexStaging.mPage->mMemory.mapped() + indexStaging.mOffset, mesh->mIndexBytes }, indices, narrow);
            Render::reserveFor(mPendingUploads, mPendingUploads.size() + 2);
            const auto handle = mMeshes.insert(mesh);
            mesh->mVertexUpload = mPendingUploads.size();
            mPendingUploads.push_back({ std::move(vertexStaging), mesh, {}, vertices.size_bytes(), false, {}, false });
            mesh->mIndexUpload = mPendingUploads.size();
            mPendingUploads.push_back({ std::move(indexStaging), mesh, {}, mesh->mIndexBytes, true, {}, false });
            return handle;
        }

        void VulkanBackend::updateMesh(
            Render::MeshHandle handle, std::span<const Render::Vertex> vertices, std::span<const std::uint32_t> indices)
        {
            const auto& mesh = mMeshes.get(handle);
            if (vertices.empty() || vertices.size_bytes() > mesh->mVertexBytes
                || (!indices.empty() && vertices.size_bytes() != mesh->mVertexBytes)
                || (!indices.empty() && indices.size() != mesh->mIndexCount)
                || std::any_of(indices.begin(), indices.end(), [&](auto index) { return index >= vertices.size(); }))
                throw std::invalid_argument("Resident mesh updates require valid dimensions and indices");
            Render::reserveFor(mPendingUploads, mPendingUploads.size() + 2);
            // Coalesce repeated CPU updates before the next submission.
            for (bool index : { false, true })
            {
                if (index && indices.empty())
                    continue;
                const auto size = index ? mesh->mIndexBytes : vertices.size_bytes();
                const auto write = [&](Staging& staging) {
                    auto* destination = staging.mPage->mMemory.mapped() + staging.mOffset;
                    if (index)
                        Render::writeMeshIndices(
                            { destination, size }, indices, mesh->mIndexType == VK_INDEX_TYPE_UINT16);
                    else
                        std::memcpy(destination, vertices.data(), size);
                };
                auto& pending = index ? mesh->mIndexUpload : mesh->mVertexUpload;
                if (pending)
                {
                    auto& upload = mPendingUploads[*pending];
                    if (size <= upload.mSize)
                    {
                        // This slice has not been recorded/submitted. Reuse
                        // its host bytes, preserving a longer pending tail
                        // when a newer update replaces only the live prefix.
                        write(upload.mStaging);
                    }
                    else
                    {
                        upload.mStaging = allocateStaging(size);
                        write(upload.mStaging);
                        upload.mSize = size;
                    }
                }
                else
                {
                    auto staging = allocateStaging(size);
                    write(staging);
                    auto& versions = index ? mesh->mIndices : mesh->mVertices;
                    const auto capacity = index ? mesh->mIndexBytes : mesh->mVertexBytes;
                    std::shared_ptr<GeometryBuffer> preserve;
                    // A prefix update must retain the previous tail when the
                    // current range is still leased by an unfinished GPU frame.
                    if (size < capacity && versions.current().use_count() > 1)
                        preserve = versions.current();
                    versions.writable([&] {
                        return allocateGeometry(
                            capacity, index ? 4 : std::lcm(std::size_t{ 16 }, sizeof(Render::Vertex)));
                    });
                    pending = mPendingUploads.size();
                    mPendingUploads.push_back(
                        { std::move(staging), mesh, {}, size, index, {}, false, std::move(preserve) });
                }
            }
        }

        std::pair<std::shared_ptr<DescriptorPage>, VkDescriptorSet> VulkanBackend::allocateMaterialSet(
            VkDescriptorSetLayout layout, std::uint32_t bindings, std::vector<std::shared_ptr<DescriptorPage>>& pages)
        {
            auto info = vkInfo<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
            info.descriptorSetCount = 1;
            info.pSetLayouts = &layout;
            VkResult result = VK_ERROR_OUT_OF_POOL_MEMORY;
            VkDescriptorSet descriptor = VK_NULL_HANDLE;
            std::shared_ptr<DescriptorPage> selected;
            for (const auto& page : pages)
            {
                if (page->mLive >= 1024)
                    continue;
                info.descriptorPool = page->mPool;
                result = vkAllocateDescriptorSets(mDevice, &info, &descriptor);
                if (result == VK_SUCCESS)
                {
                    selected = page;
                    break;
                }
                if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
                    check(result, "Allocate texture descriptor");
            }
            if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL)
            {
                VkDescriptorPoolSize size{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 * bindings };
                auto pool = vkInfo<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
                pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
                pool.maxSets = 1024;
                pool.poolSizeCount = 1;
                pool.pPoolSizes = &size;
                selected = std::make_shared<DescriptorPage>(mDevice);
                check(vkCreateDescriptorPool(mDevice, &pool, nullptr, &selected->mPool),
                    "Create material descriptor pool");
                // Register the page before allocating a set so exceptions do
                // not strand an allocated set outside its page owner.
                pages.push_back(selected);
                info.descriptorPool = selected->mPool;
                result = vkAllocateDescriptorSets(mDevice, &info, &descriptor);
            }
            check(result, "Allocate texture descriptor");
            ++selected->mLive;
            return { std::move(selected), descriptor };
        }

        void VulkanBackend::allocateDescriptor(Image& image)
        {
            auto [page, descriptor] = allocateMaterialSet(mDescriptorLayout, 1, mDescriptorPools);
            image.mPool = std::move(page);
            image.mDescriptor = descriptor;
            VkDescriptorImageInfo binding{ image.mSampler, image.mView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            auto write = vkInfo<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            write.dstSet = image.mDescriptor;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &binding;
            vkUpdateDescriptorSets(mDevice, 1, &write, 0, nullptr);
        }

        const PbrDescriptor& VulkanBackend::pbrDescriptor(Frame& frame, const Render::Draw& draw)
        {
            PbrDescriptorKey key{};
            for (std::size_t i = 0; i < key.size(); ++i)
                if (const auto handle = draw.mPbrMaps[i])
                    key[i] = (std::uint64_t{ handle.mGeneration } << 32) | handle.mIndex;
            auto [prepared, inserted] = mFramePbrDescriptors.getOrCreate(key);
            if (!inserted)
                return *prepared;
            auto& entry = mPbrDescriptors[key];
            std::array<VkDescriptorImageInfo, 4> images{};
            std::array<Image*, 2> targets{};
            std::uint32_t flags = 0;
            for (std::size_t i = 0; i < images.size(); ++i)
            {
                const auto& image = mTextures.get(draw.mPbrMaps[i] ? draw.mPbrMaps[i] : mWhiteTexture,
                    "Invalid or expired Vulkan PBR texture handle");
                if (image->mAspect != VK_IMAGE_ASPECT_COLOR_BIT || (image->mTarget && i != 0 && i != 2))
                    throw std::invalid_argument("Invalid PBR color texture binding");
                if (image->mTarget)
                    targets[i / 2] = image.get();
                if (key[i])
                {
                    flags |= 32u << i;
                    if (image->mSrgb)
                        flags |= i == 3 ? 8192u : 32768u << i;
                }
                if (!entry)
                    images[i] = { image->mSampler, image->mView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                if (image->mLastSubmission != mSubmission)
                {
                    frame.mResources.push_back(image);
                    image->mLastSubmission = mSubmission;
                }
            }
            if (!entry)
            {
                auto descriptor = std::make_shared<PbrDescriptor>();
                auto [page, set] = allocateMaterialSet(mPbrLayout, images.size(), mPbrPools);
                descriptor->mPage = std::move(page);
                descriptor->mSet = set;
                std::array<VkWriteDescriptorSet, 4> writes{};
                for (std::uint32_t i = 0; i < writes.size(); ++i)
                {
                    auto& write = writes[i];
                    write = vkInfo<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
                    write.dstSet = set;
                    write.dstBinding = i;
                    write.descriptorCount = 1;
                    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    write.pImageInfo = &images[i];
                }
                vkUpdateDescriptorSets(mDevice, writes.size(), writes.data(), 0, nullptr);
                entry = std::move(descriptor);
            }
            frame.mResources.push_back(entry);
            entry->mSubmission = mSubmission;
            entry->mMapFlags = flags;
            entry->mTargets = targets;
            prepared = entry.get();
            return *prepared;
        }

        bool VulkanBackend::supportsTextureFormat(Render::TextureFormat format, bool srgb) const
        {
            if (Render::compressedTexture(format) && !mFeatures.textureCompressionBC)
                return false;
            const auto slot = static_cast<std::size_t>(format);
            if (slot >= TextureFormats)
                return false;
            const auto& properties = mTextureProperties[slot][srgb];
            constexpr auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            return (properties.optimalTilingFeatures & required) == required;
        }

        bool VulkanBackend::supportsTexture(const Render::TextureData& data, const Render::SamplerState& state) const
        {
            if (data.mLevels.empty() || !supportsTextureFormat(data.mFormat, data.mSrgb))
                return false;
            const auto width = data.mLevels.front().mWidth;
            const auto height = data.mLevels.front().mHeight;
            const bool generate = data.mGenerateMipmaps && data.mLevels.size() == 1 && std::max(width, height) > 1;
            const auto levels = generate ? std::bit_width(std::max(width, height)) : data.mLevels.size();
            const auto slot = static_cast<std::size_t>(data.mFormat);
            const auto& limits = mTextureLimits[slot][data.mSrgb][generate];
            if (!width || !height || width > limits.maxExtent.width || height > limits.maxExtent.height
                || levels > limits.maxMipLevels || !(limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT))
                return false;
            VkFormatFeatureFlags required = 0;
            if (state.mMinFilter == Render::TextureFilter::Linear || state.mMagFilter == Render::TextureFilter::Linear
                || (state.mAnisotropy > 1 && mFeatures.samplerAnisotropy) || generate)
                required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            if (generate)
                required |= VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT
                    | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
            return (mTextureProperties[slot][data.mSrgb].optimalTilingFeatures & required) == required;
        }

        Render::TextureHandle VulkanBackend::createTexture(
            const Render::TextureData& data, const Render::SamplerState& state)
        {
            return createTexture(data, state, false);
        }

        Render::TextureHandle VulkanBackend::createDeferredTexture(
            const Render::TextureData& data, const Render::SamplerState& state)
        {
            if (mDeferredTexture)
                throw std::logic_error("A Vulkan texture upload is already being staged");
            return createTexture(data, state, true);
        }

        Render::TextureHandle VulkanBackend::createTexture(
            const Render::TextureData& data, const Render::SamplerState& state, bool deferred)
        {
            Render::validateTexture(data);
            const auto width = data.mLevels.front().mWidth;
            const auto height = data.mLevels.front().mHeight;
            if (!supportsTexture(data, state))
                throw std::runtime_error("Vulkan device does not support the texture layout, dimensions or filtering");
            const VkFormat format = textureFormat(data.mFormat, data.mSrgb);
            const bool generate = data.mGenerateMipmaps && data.mLevels.size() == 1 && std::max(width, height) > 1;
            const auto levels = generate ? static_cast<std::uint32_t>(std::bit_width(std::max(width, height)))
                                         : static_cast<std::uint32_t>(data.mLevels.size());
            auto image = createImage(width, height, format,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                    | (generate && levels > 1 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0),
                VK_IMAGE_ASPECT_COLOR_BIT, levels, 1, textureComponents(data.mFormat));
            image->mTextureFormat = data.mFormat;
            image->mSrgb = data.mSrgb;
            image->mSampler = sampler(state);
            allocateDescriptor(*image);
            std::size_t bytes = 0;
            for (const auto& level : data.mLevels)
            {
                bytes = Render::textureUploadOffset(bytes, data.mFormat);
                if (level.mBytes.size() > std::numeric_limits<std::size_t>::max() - bytes)
                    throw std::length_error("Texture upload size overflows the host address space");
                bytes += level.mBytes.size();
            }
            Upload upload;
            upload.mStaging
                = allocateStaging(bytes, std::lcm<std::size_t>(16, Render::textureCopyAlignment(data.mFormat)));
            upload.mImage = image;
            upload.mGenerateMipmaps = generate;
            upload.mImageCopies.reserve(data.mLevels.size());
            std::size_t offset = 0;
            for (std::size_t i = 0; i < data.mLevels.size(); ++i)
            {
                offset = Render::textureUploadOffset(offset, data.mFormat);
                const auto& level = data.mLevels[i];
                if (!deferred)
                    std::memcpy(upload.mStaging.mPage->mMemory.mapped() + upload.mStaging.mOffset + offset,
                        level.mBytes.data(), level.mBytes.size());
                VkBufferImageCopy copy{};
                copy.bufferOffset = upload.mStaging.mOffset + offset;
                copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, static_cast<std::uint32_t>(i), 0, 1 };
                copy.imageExtent = { level.mWidth, level.mHeight, 1 };
                upload.mImageCopies.push_back(copy);
                offset += level.mBytes.size();
            }
            if (!deferred)
                Render::reserveFor(mPendingUploads, mPendingUploads.size() + 1);
            const auto handle = mTextures.insert(image);
            if (deferred)
                mDeferredTexture.emplace(DeferredTexture{ handle, std::move(upload), {} });
            else
            {
                image->mUpload = mPendingUploads.size();
                mPendingUploads.push_back(std::move(upload));
            }
            return handle;
        }

        bool VulkanBackend::stageTexture(
            Render::TextureHandle texture, const Render::TextureData& data, std::size_t& bytes)
        {
            if (!mDeferredTexture || mDeferredTexture->mHandle != texture)
                throw std::invalid_argument("Vulkan texture does not have a deferred upload");
            auto& pending = *mDeferredTexture;
            const auto& copies = pending.mUpload.mImageCopies;
            const auto& image = *pending.mUpload.mImage;
            if (data.mFormat != image.mTextureFormat || data.mSrgb != image.mSrgb || data.mLevels.size() != copies.size())
                throw std::invalid_argument("Vulkan texture upload source changed");
            auto* mapped = pending.mUpload.mStaging.mPage->mMemory.mapped();
            const bool complete = pending.mProgress.copy(data.mLevels, bytes,
                [&](std::size_t index, std::size_t offset, std::span<const std::byte> chunk) {
                const auto& copy = copies[index];
                const auto& level = data.mLevels[index];
                if (level.mWidth != copy.imageExtent.width || level.mHeight != copy.imageExtent.height
                    || level.mBytes.size() != Render::textureLevelSize(data.mFormat, level.mWidth, level.mHeight))
                    throw std::invalid_argument("Vulkan texture mip changed during upload");
                std::memcpy(mapped + copy.bufferOffset + offset, chunk.data(), chunk.size());
            });
            if (!complete)
                return false;
            Render::reserveFor(mPendingUploads, mPendingUploads.size() + 1);
            pending.mUpload.mImage->mUpload = mPendingUploads.size();
            mPendingUploads.push_back(std::move(pending.mUpload));
            mDeferredTexture.reset();
            return true;
        }

        Render::TextureHandle VulkanBackend::createTextureSamplerView(
            Render::TextureHandle texture, const Render::SamplerState& state)
        {
            const auto& source = mTextures.get(texture);
            const auto& storage = source->mStorage ? source->mStorage : source;
            if (!storage->mTextureFormat)
                throw std::invalid_argument("Sampler views require an immutable uploaded 2D texture");
            const auto& properties
                = mTextureProperties[static_cast<std::size_t>(*storage->mTextureFormat)][storage->mSrgb];
            if ((state.mMinFilter == Render::TextureFilter::Linear || state.mMagFilter == Render::TextureFilter::Linear
                    || (state.mAnisotropy > 1 && mFeatures.samplerAnisotropy))
                && !(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
                throw std::invalid_argument("Texture format does not support linear filtering");
            auto view = std::make_shared<Image>(mDevice);
            view->mStorage = storage;
            ++storage->mSamplerViews;
            view->mImage = storage->mImage;
            view->mView = storage->mView;
            view->mWidth = storage->mWidth;
            view->mHeight = storage->mHeight;
            view->mMipLevels = storage->mMipLevels;
            view->mTextureFormat = storage->mTextureFormat;
            view->mSrgb = storage->mSrgb;
            view->mUsesMipmaps = state.mUseMipmaps;
            view->mSampler = sampler(state);
            allocateDescriptor(*view);
            return mTextures.insert(std::move(view));
        }

        Render::TextureHandle VulkanBackend::createRenderTarget(std::uint32_t width, std::uint32_t height,
            const Render::SamplerState& requestedSampling, Render::TargetFormat format, bool mipmaps)
        {
            auto sampling = requestedSampling;
            if (!width || !height || width > mProperties.limits.maxFramebufferWidth
                || height > mProperties.limits.maxFramebufferHeight || width > mProperties.limits.maxImageDimension2D
                || height > mProperties.limits.maxImageDimension2D)
                throw std::invalid_argument("Invalid Vulkan render target dimensions");
            const auto slot = static_cast<std::size_t>(format);
            if (slot >= TargetFormats || !mTargetPasses[slot].mClear)
                throw std::invalid_argument("Unsupported native target format");
            const auto& properties = mTargetProperties[slot];
            if ((sampling.mMinFilter == Render::TextureFilter::Linear
                    || sampling.mMagFilter == Render::TextureFilter::Linear)
                && !(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
                throw std::invalid_argument("Render-target format does not support linear filtering");
            const auto levels = mipmaps ? std::bit_width(std::max(width, height)) : 1u;
            constexpr auto mipFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT
                | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            if (mipmaps
                && (Render::targetHasDepth(format) || (properties.optimalTilingFeatures & mipFeatures) != mipFeatures))
                throw std::invalid_argument("Render-target format does not support color mip generation");
            auto image = createImage(width, height, TargetVkFormats[slot],
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, levels);
            image->mTarget = true;
            image->mUsesMipmaps = sampling.mUseMipmaps && mipmaps;
            ++mTargetCreations;
            image->mTargetFormat = format;
            sampling.mUseMipmaps = sampling.mUseMipmaps && mipmaps;
            if (levels > 1)
            {
                // Sampling sees the full chain; framebuffer attachments expose
                // only level zero as required by Vulkan.
                auto view = vkInfo<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
                view.image = image->mImage;
                view.viewType = VK_IMAGE_VIEW_TYPE_2D;
                view.format = TargetVkFormats[slot];
                view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                check(vkCreateImageView(mDevice, &view, nullptr, &image->mAttachmentView),
                    "Create mipmapped framebuffer attachment");
            }
            image->mSampler = sampler(sampling);
            allocateDescriptor(*image);
            if (Render::targetHasDepth(format))
                image->mDepth = createImage(width, height, mDepthFormat,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT);
            if (hasNormals(format))
            {
                image->mNormals = createImage(width, height, mNormalFormat,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
                image->mNormals->mSampler = image->mSampler;
                allocateDescriptor(*image->mNormals);
                image->mNormals->mUpload = mPendingUploads.size();
                mPendingUploads.push_back({ {}, {}, image->mNormals, 0, false, {}, false });
            }
            const std::array attachments{ image->mAttachmentView ? image->mAttachmentView : image->mView,
                image->mDepth ? image->mDepth->mView : VK_NULL_HANDLE,
                image->mNormals ? image->mNormals->mView : VK_NULL_HANDLE };
            auto info = vkInfo<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
            info.renderPass = mTargetPasses[slot].mClear;
            info.attachmentCount = image->mNormals ? 3 : image->mDepth ? 2 : 1;
            info.pAttachments = attachments.data();
            info.width = width;
            info.height = height;
            info.layers = 1;
            check(vkCreateFramebuffer(mDevice, &info, nullptr, &image->mFramebuffer), "Create Vulkan render target");
            if (Render::targetHasDepth(format))
                warmPipelines(slot + 1);
            Render::reserveFor(mPendingUploads, mPendingUploads.size() + 1);
            const auto handle = mTextures.insert(image);
            image->mUpload = mPendingUploads.size();
            // Initialize even targets sampled before their first camera render.
            mPendingUploads.push_back({ {}, {}, image, 0, false, {}, false });
            return handle;
        }

        Render::TextureHandle VulkanBackend::createVolume(std::uint32_t width, std::uint32_t height,
            std::uint32_t depth, std::span<const std::byte> rgba, const Render::SamplerState& requestedSampling)
        {
            const auto limit = mProperties.limits.maxImageDimension3D;
            if (!width || !height || depth < 2 || width > limit || height > limit || depth > limit
                || VkDeviceSize(width) * height * depth * 4 != rgba.size())
                throw std::invalid_argument("Invalid volume texture dimensions");
            auto image = createImage(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1, depth);
            auto sampling = requestedSampling;
            sampling.mUseMipmaps = false;
            image->mSampler = sampler(sampling);
            allocateDescriptor(*image);
            Upload upload;
            upload.mImage = image;
            upload.mStaging = stage(rgba);
            upload.mSize = rgba.size();
            VkBufferImageCopy copy{};
            copy.bufferOffset = upload.mStaging.mOffset;
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { width, height, depth };
            upload.mImageCopies.push_back(copy);
            Render::reserveFor(mPendingUploads, mPendingUploads.size() + 1);
            const auto handle = mTextures.insert(image);
            image->mUpload = mPendingUploads.size();
            mPendingUploads.push_back(std::move(upload));
            return handle;
        }

        Render::TextureHandle VulkanBackend::depthTexture(Render::TextureHandle target)
        {
            auto image = mTextures.get(target);
            if (!image->mTarget || !image->mDepth)
                throw std::invalid_argument("Depth sampling requires a render target");
            auto depth = image->mDepth;
            if (!depth->mDescriptor)
            {
                Render::SamplerState sampling;
                sampling.mWrapS = sampling.mWrapT = Render::TextureWrap::Clamp;
                sampling.mUseMipmaps = false;
                sampling.mMinFilter = sampling.mMagFilter = Render::TextureFilter::Nearest;
                depth->mSampler = sampler(sampling);
                allocateDescriptor(*depth);
            }
            return mTextures.insert(std::move(depth));
        }

        Render::TextureHandle VulkanBackend::normalTexture(Render::TextureHandle target)
        {
            const auto image = mTextures.get(target);
            if (!image->mNormals)
                throw std::invalid_argument("Render target has no normal attachment");
            return mTextures.insert(image->mNormals);
        }
        Render::TextureHandle VulkanBackend::worldDepthTexture(Render::TextureHandle target)
        {
            auto image = mTextures.get(target);
            if (!image->mTarget || !image->mDepth)
                throw std::invalid_argument("World depth requires a render target");
            if (!image->mWorldDepth)
                image->mWorldDepth = createDepthSnapshot(image->mWidth, image->mHeight);
            return mTextures.insert(image->mWorldDepth);
        }
        std::shared_ptr<Image> VulkanBackend::createDepthSnapshot(std::uint32_t width, std::uint32_t height)
        {
            auto depth = createImage(width, height, mDepthFormat,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            Render::SamplerState sampling;
            sampling.mWrapS = sampling.mWrapT = Render::TextureWrap::Clamp;
            sampling.mMinFilter = sampling.mMagFilter = Render::TextureFilter::Nearest;
            sampling.mUseMipmaps = false;
            depth->mSampler = sampler(sampling);
            allocateDescriptor(*depth);
            return depth;
        }

        unsigned VulkanBackend::beginTiming(Frame& frame, std::string_view label)
        {
            if (!mDiagnostics || !frame.mQueries || frame.mQueryCount + 2 > 512)
                return std::numeric_limits<unsigned>::max();
            const auto index = static_cast<unsigned>(frame.mRegions.size());
            frame.mRegions.push_back({ std::string(label), frame.mQueryCount, frame.mQueryCount + 1 });
            frame.mQueryCount += 2;
            vkCmdWriteTimestamp(
                frame.mCommands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.mQueries, frame.mRegions.back().mStart);
            return index;
        }
        void VulkanBackend::endTiming(Frame& frame, unsigned index)
        {
            if (index < frame.mRegions.size())
                vkCmdWriteTimestamp(
                    frame.mCommands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.mQueries, frame.mRegions[index].mEnd);
        }
        void VulkanBackend::collectTimings(Frame& frame)
        {
            if (!mDiagnostics)
            {
                mGpuTimings.clear();
                mGpuAvailable = false;
                return;
            }
            if (!frame.mQueryCount)
                return;
            struct Query
            {
                std::uint64_t value, available;
            };
            std::array<Query, 512> results{};
            const auto status = vkGetQueryPoolResults(mDevice, frame.mQueries, 0, frame.mQueryCount,
                frame.mQueryCount * sizeof(Query), results.data(), sizeof(Query),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (status == VK_NOT_READY)
                return;
            check(status, "Read completed GPU timings");
            for (unsigned i = 0; i < frame.mQueryCount; ++i)
                if (!results[i].available)
                    return;
            mGpuTimings.clear();
            const auto mask = mTimestampBits >= 64 ? ~std::uint64_t{ 0 } : (std::uint64_t{ 1 } << mTimestampBits) - 1;
            for (const auto& region : frame.mRegions)
            {
                const auto ticks = (results[region.mEnd].value - results[region.mStart].value) & mask;
                const auto milliseconds = ticks * double(mProperties.limits.timestampPeriod) / 1e6;
                auto entry = std::find_if(
                    mGpuTimings.begin(), mGpuTimings.end(), [&](const auto& t) { return t.mName == region.mName; });
                if (entry == mGpuTimings.end())
                    mGpuTimings.push_back({ region.mName, milliseconds });
                else
                    entry->mMilliseconds += milliseconds;
            }
            mGpuAvailable = true;
        }

        void VulkanBackend::sampleImage(Frame& frame, Image& image, VkPipelineStageFlags stages)
        {
            const auto commands = frame.mCommands;
            if (image.mStorage)
            {
                sampleImage(frame, *image.mStorage, stages);
                return;
            }
            if (image.mMipsDirty && image.mUsesMipmaps)
            {
                const auto timing = beginTiming(frame, "Mip generation");
                generateTargetMips(commands, image);
                endTiming(frame, timing);
                image.mMipsDirty = false;
                image.mWriter = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                image.mReaders = VK_PIPELINE_STAGE_TRANSFER_BIT;
                image.mVisible = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            }
            const auto missing = stages & ~image.mVisible;
            if (image.mWriter && missing)
            {
                auto barrier = vkInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
                barrier.srcAccessMask
                    = (image.mWriter & VK_PIPELINE_STAGE_TRANSFER_BIT ? VK_ACCESS_TRANSFER_WRITE_BIT : 0)
                    | (image.mWriter & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                            ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                            : 0)
                    | (image.mWriter
                                & (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                                    | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
                            ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                            : 0);
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barrier.oldLayout = barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image.mImage;
                barrier.subresourceRange = { image.mAspect, 0, image.mMipLevels, 0, 1 };
                if ((image.mAspect & VK_IMAGE_ASPECT_DEPTH_BIT) && mDepthFormat == VK_FORMAT_D24_UNORM_S8_UINT)
                    barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
                vkCmdPipelineBarrier(commands, image.mWriter, missing, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                image.mVisible |= stages;
            }
            image.mReaders |= stages;
        }

        void VulkanBackend::generateTargetMips(VkCommandBuffer commands, const Image& target)
        {
            if (target.mMipLevels < 2)
                return;
            constexpr auto shaders = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            std::array<VkImageMemoryBarrier, 2> barriers{};
            for (auto& barrier : barriers)
            {
                barrier = vkInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
                barrier.image = target.mImage;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            barriers[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barriers[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 1, target.mMipLevels - 1, 0, 1 };
            barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
                | (target.mReaders & (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
                        ? VK_ACCESS_SHADER_READ_BIT
                        : 0);
            barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(commands,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | target.mReaders | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers.data());
            auto width = static_cast<std::int32_t>(target.mWidth);
            auto height = static_cast<std::int32_t>(target.mHeight);
            for (std::uint32_t level = 1; level < target.mMipLevels; ++level)
            {
                VkImageBlit blit{};
                blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1 };
                blit.srcOffsets[1] = { width, height, 1 };
                width = std::max(1, width / 2);
                height = std::max(1, height / 2);
                blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
                blit.dstOffsets[1] = { width, height, 1 };
                vkCmdBlitImage(commands, target.mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.mImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                auto& finished = barriers[0];
                finished.subresourceRange.baseMipLevel = level - 1;
                finished.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                finished.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                finished.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                finished.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(
                    commands, VK_PIPELINE_STAGE_TRANSFER_BIT, shaders, 0, 0, nullptr, 0, nullptr, 1, &finished);
                auto& next = barriers[1];
                next.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1 };
                next.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                const bool last = level + 1 == target.mMipLevels;
                next.newLayout = last ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                next.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                next.dstAccessMask = last ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    last ? shaders : VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &next);
            }
        }

        void VulkanBackend::copyWorldDepth(VkCommandBuffer commands, Image& target)
        {
            copyDepth(commands, *target.mDepth, *target.mWorldDepth, target.mWorldDepthInitialized);
        }

        void VulkanBackend::copyDepth(
            VkCommandBuffer commands, const Image& source, Image& destination, bool& initialized)
        {
            std::array<VkImageMemoryBarrier, 2> barriers{};
            for (auto& barrier : barriers)
            {
                barrier = vkInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                if (mDepthFormat == VK_FORMAT_D24_UNORM_S8_UINT)
                    barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
            barriers[0].image = source.mImage;
            barriers[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barriers[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barriers[1].image = destination.mImage;
            barriers[1].oldLayout = initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barriers[1].srcAccessMask = initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
            barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(commands,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                    | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers.data());
            VkImageCopy copy{};
            copy.srcSubresource = copy.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
            copy.extent = { source.mWidth, source.mHeight, 1 };
            vkCmdCopyImage(commands, source.mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.mImage,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barriers[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barriers[0].dstAccessMask
                = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                    | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 2, barriers.data());
            destination.mDepthReadOnly = true;
            destination.mWriter = VK_PIPELINE_STAGE_TRANSFER_BIT;
            destination.mReaders = 0;
            destination.mVisible = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            initialized = true;
        }

        Render::EffectHandle VulkanBackend::createEffect(const Render::EffectProgram& program)
        {
            auto effect = std::make_shared<Effect>(mDevice);
            effect->mVertexTextureReads = program.mVertexTextureReads;
            effect->mBindings.assign(program.mBindings.begin(), program.mBindings.end());
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            std::uint32_t textures = 0, buffers = 0;
            for (const auto& binding : effect->mBindings)
            {
                if (std::any_of(
                        bindings.begin(), bindings.end(), [&](const auto& b) { return b.binding == binding.mBinding; }))
                    throw std::invalid_argument("Duplicate effect descriptor binding");
                if (!binding.mStages || (binding.mStages & ~3u))
                    throw std::invalid_argument("Invalid effect descriptor shader stages");
                const bool texture = binding.mKind == Render::EffectBindingKind::Texture;
                if (texture)
                    ++textures;
                else
                {
                    ++buffers;
                    if (!binding.mSize || binding.mSize > mProperties.limits.maxUniformBufferRange)
                        throw std::invalid_argument("Effect uniform buffer exceeds device limits");
                }
                bindings.push_back({ binding.mBinding,
                    texture ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                    static_cast<VkShaderStageFlags>((binding.mStages & 1 ? VK_SHADER_STAGE_VERTEX_BIT : 0)
                        | (binding.mStages & 2 ? VK_SHADER_STAGE_FRAGMENT_BIT : 0)),
                    nullptr });
            }
            const auto& limits = mProperties.limits;
            if (textures > std::min({ limits.maxPerStageDescriptorSamplers, limits.maxPerStageDescriptorSampledImages,
                    limits.maxDescriptorSetSamplers, limits.maxDescriptorSetSampledImages })
                || buffers > std::min(limits.maxPerStageDescriptorUniformBuffers, limits.maxDescriptorSetUniformBuffers)
                || textures + buffers > limits.maxPerStageResources)
                throw std::invalid_argument("Effect descriptor count exceeds device limits");
            std::array<std::vector<std::uint32_t>, 2> code{ std::vector<std::uint32_t>(
                                                                program.mVertex.begin(), program.mVertex.end()),
                std::vector<std::uint32_t>(program.mFragment.begin(), program.mFragment.end()) };
            if (!mPipelineWorker)
                mPipelineWorker = std::make_unique<PipelineWorker>(mDevice, mPipelineCache);
            effect->mObjectsPending = mPipelineWorker->submit(
                [device = mDevice, objects = effect->mObjects, bindings = std::move(bindings), code = std::move(code)](
                    VkPipelineCache) {
                    auto descriptor
                        = vkInfo<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
                    descriptor.bindingCount = static_cast<std::uint32_t>(bindings.size());
                    descriptor.pBindings = bindings.data();
                    check(vkCreateDescriptorSetLayout(device, &descriptor, nullptr, &objects->mDescriptorLayout),
                        "Create effect descriptors");
                    auto layout = vkInfo<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
                    layout.setLayoutCount = 1;
                    layout.pSetLayouts = &objects->mDescriptorLayout;
                    check(vkCreatePipelineLayout(device, &layout, nullptr, &objects->mLayout), "Create effect layout");
                    for (std::size_t i = 0; i < code.size(); ++i)
                    {
                        if (code[i].size() < 5 || code[i][0] != 0x07230203)
                            throw std::invalid_argument("Invalid effect SPIR-V");
                        auto module = vkInfo<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
                        module.codeSize = code[i].size() * sizeof(std::uint32_t);
                        module.pCode = code[i].data();
                        check(vkCreateShaderModule(device, &module, nullptr, &objects->mModules[i]),
                            "Create effect shader");
                    }
                    return PipelineWorker::Value{};
                });
            return mEffects.insert(std::move(effect));
        }

        PipelineWorker::Value compileEffectPipeline(VkDevice device, VkPipelineCache cache, const EffectObjects& effect,
            VkRenderPass renderPass, const Render::EffectBlend& blending)
        {
            auto pipeline = std::make_shared<CompiledEffectPipeline>(device);
            std::array<VkPipelineShaderStageCreateInfo, 2> shaders{};
            for (std::size_t i = 0; i < shaders.size(); ++i)
            {
                shaders[i]
                    = vkInfo<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
                shaders[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
                shaders[i].module = effect.mModules[i];
                shaders[i].pName = "main";
            }
            const VkVertexInputBindingDescription binding{ 0, sizeof(Render::Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
            const VkVertexInputAttributeDescription attribute{ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
            auto vertex = vkInfo<VkPipelineVertexInputStateCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
            vertex.vertexBindingDescriptionCount = vertex.vertexAttributeDescriptionCount = 1;
            vertex.pVertexBindingDescriptions = &binding;
            vertex.pVertexAttributeDescriptions = &attribute;
            auto assembly = vkInfo<VkPipelineInputAssemblyStateCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            auto viewport
                = vkInfo<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
            viewport.viewportCount = viewport.scissorCount = 1;
            auto raster = vkInfo<VkPipelineRasterizationStateCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.lineWidth = 1;
            auto samples = vkInfo<VkPipelineMultisampleStateCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
            samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            auto depth = vkInfo<VkPipelineDepthStencilStateCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
            VkPipelineColorBlendAttachmentState attachment{};
            attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
                | VK_COLOR_COMPONENT_A_BIT;
            constexpr std::array factors{ VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
                VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
                VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
                VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_CONSTANT_COLOR,
                VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA,
                VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE };
            constexpr std::array operations{ VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
                VK_BLEND_OP_MIN, VK_BLEND_OP_MAX };
            attachment.blendEnable = blending.mEnabled;
            attachment.srcColorBlendFactor = factors.at(static_cast<unsigned>(blending.mFactors[0]));
            attachment.dstColorBlendFactor = factors.at(static_cast<unsigned>(blending.mFactors[1]));
            attachment.srcAlphaBlendFactor = factors.at(static_cast<unsigned>(blending.mFactors[2]));
            attachment.dstAlphaBlendFactor = factors.at(static_cast<unsigned>(blending.mFactors[3]));
            attachment.colorBlendOp = operations.at(static_cast<unsigned>(blending.mOperations[0]));
            attachment.alphaBlendOp = operations.at(static_cast<unsigned>(blending.mOperations[1]));
            auto blend
                = vkInfo<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
            std::copy(blending.mConstant.begin(), blending.mConstant.end(), blend.blendConstants);
            blend.attachmentCount = 1;
            blend.pAttachments = &attachment;
            const std::array states{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            auto dynamic
                = vkInfo<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
            dynamic.dynamicStateCount = states.size();
            dynamic.pDynamicStates = states.data();
            auto info = vkInfo<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
            info.stageCount = shaders.size();
            info.pStages = shaders.data();
            info.pVertexInputState = &vertex;
            info.pInputAssemblyState = &assembly;
            info.pViewportState = &viewport;
            info.pRasterizationState = &raster;
            info.pMultisampleState = &samples;
            info.pDepthStencilState = &depth;
            info.pColorBlendState = &blend;
            info.pDynamicState = &dynamic;
            info.layout = effect.mLayout;
            info.renderPass = renderPass;
            check(vkCreateGraphicsPipelines(device, cache, 1, &info, nullptr, &pipeline->mPipeline),
                "Create native effect pipeline");
            return pipeline;
        }

        VkPipeline VulkanBackend::effectPipeline(
            Effect& effect, const Image* target, const Render::EffectBlend& blending)
        {
            if (effect.mObjectsError)
                std::rethrow_exception(effect.mObjectsError);
            if (!effect.mObjectsReady)
            {
                if (effect.mObjectsPending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                    return VK_NULL_HANDLE;
                try
                {
                    effect.mObjectsPending.get();
                }
                catch (...)
                {
                    effect.mObjectsError = std::current_exception();
                    throw;
                }
                effect.mObjectsReady = true;
            }
            const auto slot = target ? static_cast<std::size_t>(target->mTargetFormat) + 1 : 0;
            if (slot > TargetFormats || (target && !mTargetPasses[slot - 1].mClear))
                throw std::invalid_argument("Unsupported effect output format");
            auto& entry = effect.mPipelines[{ slot, target ? VK_FORMAT_UNDEFINED : mColorFormat, blending }];
            if (entry.mError)
                std::rethrow_exception(entry.mError);
            if (entry.mReady)
                return entry.mReady->mPipeline;
            if (entry.mPending.valid())
            {
                if (entry.mPending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                    return VK_NULL_HANDLE;
                try
                {
                    entry.mReady = entry.mPending.get();
                }
                catch (...)
                {
                    entry.mError = std::current_exception();
                    throw;
                }
                return entry.mReady->mPipeline;
            }
            if (blending.mEnabled && target
                && !(mTargetProperties[slot - 1].optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))
                throw std::invalid_argument("Effect target format does not support attachment blending");
            if (!mPipelineWorker)
                mPipelineWorker = std::make_unique<PipelineWorker>(mDevice, mPipelineCache);
            const auto renderPass = target ? mTargetPasses[slot - 1].mClear : mRenderPass;
            // Hold shader modules/layout alive until the worker finishes. The
            // result owns its pipeline even if its requesting handle disappears.
            auto retained = effect.mObjects;
            entry.mPending
                = mPipelineWorker->submit([device = mDevice, retained, renderPass, blending](VkPipelineCache cache) {
                      return compileEffectPipeline(device, cache, *retained, renderPass, blending);
                  });
            ++mPipelineMisses;
            return VK_NULL_HANDLE;
        }

        VkDescriptorSet VulkanBackend::allocateFrameDescriptor(
            Frame& frame, VkDescriptorSetLayout layout, VkDescriptorPool* persistentPool)
        {
            auto& pools = persistentPool ? frame.mPersistentEffectPools : frame.mEffectPools;
            std::size_t persistentIndex = 0;
            auto& index = persistentPool ? persistentIndex : frame.mEffectPoolIndex;
            VkDescriptorSet descriptor = VK_NULL_HANDLE;
            for (;;)
            {
                if (index == pools.size())
                {
                    const std::array sizes{ VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 },
                        VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256 },
                        VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 128 } };
                    auto pool = vkInfo<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
                    pool.flags = persistentPool ? VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT : 0;
                    pool.maxSets = 64;
                    pool.poolSizeCount = sizes.size();
                    pool.pPoolSizes = sizes.data();
                    VkDescriptorPool value = VK_NULL_HANDLE;
                    check(vkCreateDescriptorPool(mDevice, &pool, nullptr, &value), "Create effect descriptor page");
                    pools.push_back(value);
                }
                auto info = vkInfo<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
                info.descriptorPool = pools[index];
                info.descriptorSetCount = 1;
                info.pSetLayouts = &layout;
                const auto result = vkAllocateDescriptorSets(mDevice, &info, &descriptor);
                if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL)
                {
                    ++index;
                    continue;
                }
                check(result, "Allocate effect descriptor");
                break;
            }
            if (persistentPool)
                *persistentPool = pools[index];
            return descriptor;
        }

        VkDescriptorSet VulkanBackend::dataDescriptor(Frame& frame, const Render::ShadowSet* shadows)
        {
            auto [cached, inserted] = mShadowDescriptors.getOrCreate(shadows);
            if (!inserted)
                return cached;
            if (shadows && shadows->mCount > Render::ShadowSet::MaxMaps)
                throw std::invalid_argument("Invalid native shadow cascade count");
            const auto descriptor = shadows ? allocateFrameDescriptor(frame, mDataLayout) : frame.mDataDescriptor;
            struct ShadowData
            {
                std::array<std::array<float, 16>, 8> mMatrices{};
                std::array<std::array<float, 16>, 8> mRegions{};
                std::array<float, 4> mParameters{};
                std::array<unsigned, 4> mFlags{};
            } data;
            static_assert(sizeof(ShadowData) == 1056);
            if (shadows)
            {
                data.mMatrices = shadows->mViewToShadow;
                data.mRegions = shadows->mValidRegions;
                data.mParameters = { shadows->mMaximumDistance, shadows->mFadeStart, shadows->mNormalOffset, 0 };
                data.mFlags = { shadows->mCount, shadows->mSoft ? 1u : 0u, shadows->mRegionMask, 0 };
            }
            auto staging = allocateStaging(
                sizeof(data), std::max(VkDeviceSize{ 16 }, mProperties.limits.minUniformBufferOffsetAlignment));
            std::memcpy(staging.mPage->mMemory.mapped() + staging.mOffset, &data, sizeof(data));
            if (staging.mPage->mLastUploadSubmission != mSubmission)
            {
                frame.mUploads.push_back(staging.mPage);
                staging.mPage->mLastUploadSubmission = mSubmission;
            }
            const std::array buffers{ VkDescriptorBufferInfo{ frame.mInstances->mBuffer, 0, VK_WHOLE_SIZE },
                VkDescriptorBufferInfo{ frame.mLights->mBuffer, 0, VK_WHOLE_SIZE },
                VkDescriptorBufferInfo{ staging.mPage->mBuffer, staging.mOffset, sizeof(data) } };
            std::array<VkDescriptorImageInfo, 8> images{};
            std::array<VkWriteDescriptorSet, 11> writes{};
            for (unsigned i = 0; i < writes.size(); ++i)
            {
                auto& write = writes[i];
                write = vkInfo<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
                write.dstSet = descriptor;
                write.dstBinding = i;
                write.descriptorCount = 1;
                if (i < buffers.size())
                {
                    write.descriptorType
                        = i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                    write.pBufferInfo = &buffers[i];
                }
                else
                {
                    const auto map = i - 3;
                    const auto& image
                        = mTextures.get(shadows && map < shadows->mCount ? shadows->mTextures[map] : mWhiteTexture);
                    images[map] = { image->mSampler, image->mView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                    if (image->mLastSubmission != mSubmission)
                    {
                        frame.mResources.push_back(image);
                        image->mLastSubmission = mSubmission;
                    }
                    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    write.pImageInfo = &images[map];
                }
            }
            vkUpdateDescriptorSets(mDevice, writes.size(), writes.data(), 0, nullptr);
            cached = descriptor;
            return descriptor;
        }

        VkDescriptorSet VulkanBackend::effectDescriptor(
            Frame& frame, Effect& effect, std::span<const Render::EffectInput> inputs)
        {
            if (frame.mEffectDescriptorsUsed == frame.mEffectDescriptors.size())
                frame.mEffectDescriptors.emplace_back();
            auto& cachedSet = frame.mEffectDescriptors[frame.mEffectDescriptorsUsed++];
            if (cachedSet.mObjects != effect.mObjects)
            {
                if (cachedSet.mSet)
                    check(vkFreeDescriptorSets(mDevice, cachedSet.mPool, 1, &cachedSet.mSet),
                        "Release effect descriptor");
                cachedSet = {};
                cachedSet.mSet = allocateFrameDescriptor(frame, effect.mObjects->mDescriptorLayout, &cachedSet.mPool);
                cachedSet.mObjects = effect.mObjects;
            }
            const auto descriptor = cachedSet.mSet;
            auto& writes = mEffectWrites;
            auto& buffers = mEffectBuffers;
            auto& images = mEffectImages;
            writes.clear();
            buffers.clear();
            images.clear();
            writes.reserve(effect.mBindings.size());
            buffers.reserve(effect.mBindings.size());
            images.reserve(effect.mBindings.size());
            auto retained = std::move(cachedSet.mRetainedImages);
            retained.clear();
            retained.reserve(effect.mBindings.size());
            for (const auto& binding : effect.mBindings)
            {
                const auto input = std::find_if(
                    inputs.begin(), inputs.end(), [&](const auto& i) { return i.mBinding == binding.mBinding; });
                auto write = vkInfo<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
                write.dstSet = descriptor;
                write.dstBinding = binding.mBinding;
                write.descriptorCount = 1;
                if (binding.mKind == Render::EffectBindingKind::Texture)
                {
                    if (input == inputs.end() || !input->mTexture)
                        throw std::invalid_argument("Missing native effect texture");
                    auto image = mTextures.get(input->mTexture);
                    retained.push_back(image);
                    images.push_back({ image->mSampler, image->mView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
                    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    write.pImageInfo = &images.back();
                }
                else
                {
                    const auto data = input == inputs.end() ? std::span<const std::byte>{} : input->mData;
                    if (data.size() > binding.mSize)
                        throw std::invalid_argument("Effect uniform data exceeds reflected block");
                    auto [cached, inserted] = frame.mEffectUniforms.getOrCreate(data.data());
                    if (inserted || cached.mSize != binding.mSize || cached.mInputSize != data.size())
                    {
                        const auto alignment
                            = std::max(VkDeviceSize{ 16 }, mProperties.limits.minUniformBufferOffsetAlignment);
                        const auto offset = (frame.mUniformOffset + alignment - 1) / alignment * alignment;
                        auto at = offset;
                        while (frame.mUniformPage < frame.mUniformPages.size()
                            && (at > frame.mUniformPages[frame.mUniformPage]->mSize
                                || binding.mSize > frame.mUniformPages[frame.mUniformPage]->mSize - at))
                        {
                            ++frame.mUniformPage;
                            frame.mUniformOffset = at = 0;
                        }
                        if (frame.mUniformPage == frame.mUniformPages.size())
                            frame.mUniformPages.push_back(createBuffer(std::max<VkDeviceSize>(65536, binding.mSize),
                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, MemoryKind::Upload));
                        const auto& page = frame.mUniformPages[frame.mUniformPage];
                        auto* destination = page->mMemory.mapped() + at;
                        if (!data.empty())
                            std::memcpy(destination, data.data(), data.size());
                        std::memset(destination + data.size(), 0, binding.mSize - data.size());
                        cached = { binding.mSize, data.size(), { page->mBuffer, at, binding.mSize } };
                        frame.mUniformOffset = at + binding.mSize;
                    }
                    buffers.push_back(cached.mBuffer);
                    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                    write.pBufferInfo = &buffers.back();
                }
                writes.push_back(write);
            }
            const auto sameBuffer = [](const auto& a, const auto& b) {
                return a.buffer == b.buffer && a.offset == b.offset && a.range == b.range;
            };
            const auto sameImage = [](const auto& a, const auto& b) {
                return a.sampler == b.sampler && a.imageView == b.imageView && a.imageLayout == b.imageLayout;
            };
            if (!std::equal(
                    buffers.begin(), buffers.end(), cachedSet.mBuffers.begin(), cachedSet.mBuffers.end(), sameBuffer)
                || !std::equal(
                    images.begin(), images.end(), cachedSet.mImages.begin(), cachedSet.mImages.end(), sameImage))
            {
                vkUpdateDescriptorSets(mDevice, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
                cachedSet.mBuffers.assign(buffers.begin(), buffers.end());
                cachedSet.mImages.assign(images.begin(), images.end());
                ++mDescriptorUpdates;
            }
            cachedSet.mRetainedImages = std::move(retained);
            return descriptor;
        }

        void VulkanBackend::cancelUpload(std::optional<std::size_t>& index)
        {
            if (!index)
                return;
            const auto position = *index;
            index.reset();
            mPendingUploads[position] = {};
            // Reclaim cancelled tails without shifting indices of live uploads.
            while (!mPendingUploads.empty() && !mPendingUploads.back().mStaging && !mPendingUploads.back().mImage)
                mPendingUploads.pop_back();
        }

        void VulkanBackend::releaseMesh(Render::MeshHandle handle)
        {
            const auto resource = mMeshes.get(handle);
            mMeshes.erase(handle);
            cancelUpload(resource->mVertexUpload);
            cancelUpload(resource->mIndexUpload);
        }

        void VulkanBackend::releaseTexture(Render::TextureHandle texture)
        {
            if (texture == mWhiteTexture)
                throw std::invalid_argument("Cannot release the backend's default texture");
            const auto resource = mTextures.get(texture);
            mTextures.erase(texture);
            if (mDeferredTexture && mDeferredTexture->mHandle == texture)
                mDeferredTexture.reset();
            if (resource->mSamplerViews == 0)
                cancelUpload(resource->mUpload);
        }

        void VulkanBackend::destroySwapchain()
        {
            for (const auto framebuffer : mFramebuffers)
                vkDestroyFramebuffer(mDevice, framebuffer, nullptr);
            mFramebuffers.clear();
            mDepthImages.clear();
            for (const auto view : mSwapchainViews)
                vkDestroyImageView(mDevice, view, nullptr);
            mSwapchainViews.clear();
            for (const auto semaphore : mPresented)
                vkDestroySemaphore(mDevice, semaphore, nullptr);
            mPresented.clear();
            vkDestroySwapchainKHR(mDevice, mSwapchain, nullptr);
            mSwapchain = VK_NULL_HANDLE;
            mSwapchainImages.clear();
            mSwapchainInitialized.clear();
            mImageFences.clear();
        }

        void VulkanBackend::destroyWindowPipelines()
        {
            if (mPipelineWorker)
                mPipelineWorker->finish();
            for (auto& pipeline : std::span(mPipelines).first(PipelinesPerTarget))
            {
                vkDestroyPipeline(mDevice, pipeline, nullptr);
                pipeline = VK_NULL_HANDLE;
            }
            vkDestroyRenderPass(mDevice, mRenderPass, nullptr);
            vkDestroyRenderPass(mDevice, mMainLoadPass, nullptr);
            vkDestroyRenderPass(mDevice, mMainResumePass, nullptr);
            vkDestroyRenderPass(mDevice, mMainWaterClearPass, nullptr);
            vkDestroyRenderPass(mDevice, mMainWaterLoadPass, nullptr);
            mRenderPass = VK_NULL_HANDLE;
            mMainLoadPass = VK_NULL_HANDLE;
            mMainResumePass = VK_NULL_HANDLE;
            mMainWaterClearPass = VK_NULL_HANDLE;
            mMainWaterLoadPass = VK_NULL_HANDLE;
        }

        void VulkanBackend::refreshSurface()
        {
            const auto now = std::chrono::steady_clock::now();
            if (!mSurfaceRefresh || !mSwapchain || now < mNextSurfaceRefresh)
                return;
            mSurfaceRefresh = false;
            mNextSurfaceRefresh = now + std::chrono::seconds(1);
            VkSurfaceCapabilitiesKHR capabilities{};
            check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mPhysicalDevice, mSurface, &capabilities),
                "Refresh surface capabilities");
            auto expected = capabilities.currentExtent;
            if (expected.width == std::numeric_limits<std::uint32_t>::max())
                expected = { std::clamp(mDrawableExtent.width, capabilities.minImageExtent.width,
                                 capabilities.maxImageExtent.width),
                    std::clamp(mDrawableExtent.height, capabilities.minImageExtent.height,
                        capabilities.maxImageExtent.height) };
            if (expected.width != mExtent.width || expected.height != mExtent.height
                || capabilities.currentTransform != mSurfaceTransform
                || !(capabilities.supportedCompositeAlpha & mCompositeAlpha)
                || capabilities.minImageCount > mSwapchainImages.size()
                || (capabilities.maxImageCount && capabilities.maxImageCount < mSwapchainImages.size()))
                mResize = true;
            std::uint32_t count = 0;
            check(vkGetPhysicalDeviceSurfaceFormatsKHR(mPhysicalDevice, mSurface, &count, nullptr),
                "Refresh surface format count");
            std::vector<VkSurfaceFormatKHR> formats(count);
            check(vkGetPhysicalDeviceSurfaceFormatsKHR(mPhysicalDevice, mSurface, &count, formats.data()),
                "Refresh surface formats");
            if (std::none_of(formats.begin(), formats.end(), [&](const auto& format) {
                    return (format.format == mColorFormat || format.format == VK_FORMAT_UNDEFINED)
                        && format.colorSpace == mColorSpace;
                }))
                mResize = true;
        }

        bool VulkanBackend::createSwapchain()
        {
            int width = 0;
            int height = 0;
            SDL_Vulkan_GetDrawableSize(mOptions.mWindow, &width, &height);
            if (width <= 0 || height <= 0)
                return false;
            mDrawableExtent = { static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height) };
            if (mSwapchain)
            {
                for (const auto& frame : mFrames)
                {
                    const auto status = vkGetFenceStatus(mDevice, frame.mFence);
                    if (status == VK_NOT_READY)
                        return false;
                    check(status, "Check frames before swapchain recreation");
                }
                check(vkDeviceWaitIdle(mDevice), "Wait for swapchain recreation");
                destroySwapchain();
                for (auto& frame : mFrames)
                {
                    frame.mResources.clear();
                    frame.mUploads.clear();
                }
                mStagingPages.trim();
                mAllocator->trim();
            }
            VkSurfaceCapabilitiesKHR capabilities;
            check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mPhysicalDevice, mSurface, &capabilities),
                "Read surface capabilities");
            if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
                throw std::runtime_error("Vulkan surface does not support color attachment rendering");
            std::uint32_t count = 0;
            check(vkGetPhysicalDeviceSurfaceFormatsKHR(mPhysicalDevice, mSurface, &count, nullptr),
                "Count surface formats");
            std::vector<VkSurfaceFormatKHR> formats(count);
            check(vkGetPhysicalDeviceSurfaceFormatsKHR(mPhysicalDevice, mSurface, &count, formats.data()),
                "Read surface formats");
            if (formats.empty())
                throw std::runtime_error("Vulkan surface has no color format");
            auto format = formats.front();
            if (formats.size() == 1 && format.format == VK_FORMAT_UNDEFINED)
                format = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
            else
                for (const auto candidate : formats)
                    if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM
                        && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                    {
                        format = candidate;
                        break;
                    }
            if (mRenderPass && mColorFormat != format.format)
                destroyWindowPipelines();
            mColorFormat = format.format;
            mColorSpace = format.colorSpace;
            mExtent = capabilities.currentExtent;
            if (mExtent.width == std::numeric_limits<std::uint32_t>::max())
                mExtent = { std::clamp(static_cast<std::uint32_t>(width), capabilities.minImageExtent.width,
                                capabilities.maxImageExtent.width),
                    std::clamp(static_cast<std::uint32_t>(height), capabilities.minImageExtent.height,
                        capabilities.maxImageExtent.height) };
            if (!mExtent.width || !mExtent.height)
                return false;
            check(vkGetPhysicalDeviceSurfacePresentModesKHR(mPhysicalDevice, mSurface, &count, nullptr),
                "Count present modes");
            std::vector<VkPresentModeKHR> modes(count);
            check(vkGetPhysicalDeviceSurfacePresentModesKHR(mPhysicalDevice, mSurface, &count, modes.data()),
                "Read present modes");
            VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
            const auto supports
                = [&](auto value) { return std::find(modes.begin(), modes.end(), value) != modes.end(); };
            if (mOptions.mPresentMode == Render::PresentMode::Immediate)
            {
                if (supports(VK_PRESENT_MODE_IMMEDIATE_KHR))
                    mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
                else if (supports(VK_PRESENT_MODE_MAILBOX_KHR))
                    mode = VK_PRESENT_MODE_MAILBOX_KHR;
            }
            else if (mOptions.mPresentMode == Render::PresentMode::Adaptive
                && supports(VK_PRESENT_MODE_FIFO_RELAXED_KHR))
                mode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
            std::uint32_t imageCount = capabilities.minImageCount + 1;
            if (capabilities.maxImageCount)
                imageCount = std::min(imageCount, capabilities.maxImageCount);
            VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            for (const auto candidate : { VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                     VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR })
                if (capabilities.supportedCompositeAlpha & candidate)
                {
                    alpha = candidate;
                    break;
                }
            const std::array families{ mGraphicsFamily, mPresentFamily };
            auto info = vkInfo<VkSwapchainCreateInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
            info.surface = mSurface;
            info.minImageCount = imageCount;
            info.imageFormat = format.format;
            info.imageColorSpace = format.colorSpace;
            info.imageExtent = mExtent;
            info.imageArrayLayers = 1;
            info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            mCaptureSupported = (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                && (mColorFormat == VK_FORMAT_B8G8R8A8_UNORM || mColorFormat == VK_FORMAT_B8G8R8A8_SRGB
                    || mColorFormat == VK_FORMAT_R8G8B8A8_UNORM || mColorFormat == VK_FORMAT_R8G8B8A8_SRGB);
            if (mCaptureSupported)
                info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            info.imageSharingMode
                = mGraphicsFamily == mPresentFamily ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
            if (info.imageSharingMode == VK_SHARING_MODE_CONCURRENT)
            {
                info.queueFamilyIndexCount = 2;
                info.pQueueFamilyIndices = families.data();
            }
            mSurfaceTransform = capabilities.currentTransform;
            mCompositeAlpha = alpha;
            info.preTransform = mSurfaceTransform;
            info.compositeAlpha = alpha;
            info.presentMode = mode;
            info.clipped = VK_TRUE;
            check(vkCreateSwapchainKHR(mDevice, &info, nullptr, &mSwapchain), "Create Vulkan swapchain");
            check(vkGetSwapchainImagesKHR(mDevice, mSwapchain, &imageCount, nullptr), "Count swapchain images");
            mSwapchainImages.resize(imageCount);
            check(vkGetSwapchainImagesKHR(mDevice, mSwapchain, &imageCount, mSwapchainImages.data()),
                "Read swapchain images");
            mSwapchainImages.resize(imageCount);
            mSwapchainInitialized.assign(imageCount, false);
            mSwapchainViews.resize(imageCount);
            mPresented.resize(imageCount);
            mImageFences.assign(imageCount, VK_NULL_HANDLE);
            for (std::uint32_t i = 0; i < imageCount; ++i)
            {
                auto view = vkInfo<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
                view.image = mSwapchainImages[i];
                view.viewType = VK_IMAGE_VIEW_TYPE_2D;
                view.format = mColorFormat;
                view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                check(vkCreateImageView(mDevice, &view, nullptr, &mSwapchainViews[i]), "Create swapchain image view");
                auto semaphore = vkInfo<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
                check(vkCreateSemaphore(mDevice, &semaphore, nullptr, &mPresented[i]), "Create presentation semaphore");
            }
            // Viewport/scissor are dynamic. A resize does not invalidate
            // pipelines or compatible render passes, including offscreen ones.
            if (!mRenderPass)
            {
                mRenderPass = createRenderPass(mColorFormat, false, true);
                mMainLoadPass = createRenderPass(mColorFormat, false, false);
                mMainResumePass = createRenderPass(mColorFormat, false, false, false, true, false, true);
                mMainWaterClearPass = createRenderPass(mColorFormat, false, true, false, true, false, false, true);
                mMainWaterLoadPass = createRenderPass(mColorFormat, false, false, false, true, false, false, true);
                warmPipelines(0);
            }
            mDepthImages.reserve(FramesInFlight);
            for (std::size_t i = 0; i < FramesInFlight; ++i)
                mDepthImages.push_back(createImage(mExtent.width, mExtent.height, mDepthFormat,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT));
            mFramebuffers.resize(imageCount * FramesInFlight);
            for (std::size_t i = 0; i < mFramebuffers.size(); ++i)
            {
                const std::array attachments{ mSwapchainViews[i / FramesInFlight],
                    mDepthImages[i % FramesInFlight]->mView };
                auto framebuffer = vkInfo<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
                framebuffer.renderPass = mRenderPass;
                framebuffer.attachmentCount = static_cast<std::uint32_t>(attachments.size());
                framebuffer.pAttachments = attachments.data();
                framebuffer.width = mExtent.width;
                framebuffer.height = mExtent.height;
                framebuffer.layers = 1;
                check(vkCreateFramebuffer(mDevice, &framebuffer, nullptr, &mFramebuffers[i]),
                    "Create Vulkan framebuffer");
            }
            mResize = false;
            mSurfaceRefresh = false;
            mNextSurfaceRefresh = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            return true;
        }

        VkRenderPass VulkanBackend::createRenderPass(VkFormat format, bool offscreen, bool clear, bool normals,
            bool hasDepth, bool overwrite, bool preserveDepth, bool storeDepth, bool discardDepth)
        {
            std::array<VkAttachmentDescription, 3> attachments{};
            attachments[0].format = format;
            attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[0].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                : overwrite               ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                                          : VK_ATTACHMENT_LOAD_OP_LOAD;
            attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[0].initialLayout = offscreen ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                : clear                              ? VK_IMAGE_LAYOUT_UNDEFINED
                                                     : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            attachments[0].finalLayout
                = offscreen ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            attachments[1].format = mDepthFormat;
            attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[1].loadOp = preserveDepth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[1].storeOp = !discardDepth && (offscreen || preserveDepth || storeDepth)
                ? VK_ATTACHMENT_STORE_OP_STORE
                : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[1].initialLayout
                = preserveDepth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachments[2] = attachments[0];
            attachments[2].format = mNormalFormat;
            const std::array colors{ VkAttachmentReference{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
                VkAttachmentReference{ 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } };
            VkAttachmentReference depth{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = normals ? 2 : 1;
            subpass.pColorAttachments = colors.data();
            subpass.pDepthStencilAttachment = hasDepth ? &depth : nullptr;
            // Keep color and depth dependencies separate. Combining their
            // masks makes early depth tests wait for unrelated color stores.
            std::array<VkSubpassDependency, 3> dependencies{};
            for (auto& dependency : dependencies)
            {
                dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
                dependency.dstSubpass = 0;
            }
            auto& colorDependency = dependencies[0];
            colorDependency.srcStageMask = colorDependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            colorDependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            colorDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
            auto& depthDependency = dependencies[1];
            depthDependency.srcStageMask = depthDependency.dstStageMask
                = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            depthDependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            depthDependency.dstAccessMask
                = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            auto& outputDependency = dependencies[2];
            outputDependency.srcSubpass = 0;
            outputDependency.dstSubpass = VK_SUBPASS_EXTERNAL;
            outputDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            outputDependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            outputDependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            outputDependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            auto info = vkInfo<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
            info.attachmentCount = normals ? 3 : hasDepth ? 2 : 1;
            info.pAttachments = attachments.data();
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            // Scope output visibility to this producer subpass. Color-only
            // passes need no duplicate global incoming dependency or separate
            // post-pass barrier command; initial transitions remain explicit.
            info.dependencyCount = hasDepth ? 2 : 1;
            info.pDependencies = dependencies.data() + (offscreen ? (hasDepth ? 1 : 2) : 0);
            VkRenderPass result = VK_NULL_HANDLE;
            check(vkCreateRenderPass(mDevice, &info, nullptr, &result), "Create Vulkan render pass");
            return result;
        }

        void VulkanBackend::warmPipelines(std::size_t target)
        {
            if (!mDynamicRaster)
                return; // The Vulkan 1.1 fallback has many baked raster states.
            // Prepare common scene variants when their render target appears,
            // instead of compiling them on a later camera rotation. Persistent
            // driver pipeline-cache data is shared with ordinary creation.
            const auto warm = [&](MaterialShader shader, Render::BlendMode blend) {
                const auto index = target * PipelinesPerTarget + static_cast<std::size_t>(shader) * RasterKeys
                    + 4 * static_cast<std::size_t>(blend);
                if (!mPipelines[index])
                    createPipeline(index);
            };
            if (target && static_cast<Render::TargetFormat>(target - 1) == Render::TargetFormat::ShadowMap)
            {
                warm(MaterialShader::Shadow, Render::BlendMode::Opaque);
                return;
            }
            if (target && !Render::targetHasDepth(static_cast<Render::TargetFormat>(target - 1)))
            {
                warm(MaterialShader::Distortion, Render::BlendMode::Alpha);
                warm(MaterialShader::Distortion, Render::BlendMode::Additive);
                if (mOptions.mPbr && static_cast<Render::TargetFormat>(target - 1) == Render::TargetFormat::ColorRGBA8)
                    for (const auto blend : { Render::BlendMode::Opaque, Render::BlendMode::Alpha,
                             Render::BlendMode::Additive, Render::BlendMode::WeightedReplace })
                        warm(MaterialShader::PbrBake, blend);
                return;
            }
            for (const auto blend : { Render::BlendMode::Opaque, Render::BlendMode::Alpha, Render::BlendMode::Additive,
                     Render::BlendMode::WeightedReplace, Render::BlendMode::PremultipliedAlpha })
            {
                warm(MaterialShader::Surface, blend);
                warm(MaterialShader::SolidSurface, blend);
            }
            warm(MaterialShader::Water, Render::BlendMode::Alpha);
            for (const auto blend : { Render::BlendMode::Opaque, Render::BlendMode::Alpha })
            {
                warm(MaterialShader::SurfaceShadowed, blend);
                warm(MaterialShader::SolidSurfaceShadowed, blend);
            }
            if (mOptions.mPbr)
                for (const auto blend : { Render::BlendMode::Opaque, Render::BlendMode::Alpha,
                         Render::BlendMode::Additive, Render::BlendMode::WeightedReplace })
                {
                    warm(MaterialShader::PbrSurface, blend);
                    warm(MaterialShader::PbrSolidSurface, blend);
                    warm(MaterialShader::PbrSurfaceShadowed, blend);
                    warm(MaterialShader::PbrSolidSurfaceShadowed, blend);
                }
            warm(MaterialShader::Sky, Render::BlendMode::Opaque);
            warm(MaterialShader::Sky, Render::BlendMode::Alpha);
            warm(MaterialShader::Sky, Render::BlendMode::Additive);
            if (!target)
            {
                warm(MaterialShader::Overlay, Render::BlendMode::Opaque);
                warm(MaterialShader::Overlay, Render::BlendMode::Alpha);
                warm(MaterialShader::Overlay, Render::BlendMode::PremultipliedAlpha);
            }
        }

        void VulkanBackend::createPipeline(std::size_t pipelineIndex)
        {
            const auto rasterKey = pipelineIndex % RasterKeys;
            const auto material = static_cast<std::uint32_t>((pipelineIndex % PipelinesPerTarget) / RasterKeys);
            const auto targetGroup = pipelineIndex / PipelinesPerTarget;
            const std::array<std::uint32_t, 3> constants{ material,
                targetGroup && hasNormals(static_cast<Render::TargetFormat>(targetGroup - 1)) ? VK_TRUE : VK_FALSE,
                mFeatures.depthClamp };
            const std::array<VkSpecializationMapEntry, 3> entries{ {
                { 0, 0, sizeof(std::uint32_t) },
                { 1, sizeof(std::uint32_t), sizeof(std::uint32_t) },
                { 2, 2 * sizeof(std::uint32_t), sizeof(std::uint32_t) },
            } };
            const VkSpecializationInfo specialization{ static_cast<std::uint32_t>(entries.size()), entries.data(),
                sizeof(constants), constants.data() };
            const bool pbr = isPbr(static_cast<MaterialShader>(material));
            auto& modules = pbr ? mPbrShaderModules : mShaderModules;
            std::array<VkPipelineShaderStageCreateInfo, 2> shaders{};
            const auto names
                = pbr ? std::array{ "pbr.vert.spv", "pbr.frag.spv" } : std::array{ "basic.vert.spv", "basic.frag.spv" };
            for (std::size_t i = 0; i < modules.size(); ++i)
            {
                if (!modules[i])
                {
                    const auto words = readShader(mOptions.mShaderPath / names[i]);
                    auto module = vkInfo<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
                    module.codeSize = words.size() * sizeof(std::uint32_t);
                    module.pCode = words.data();
                    check(vkCreateShaderModule(mDevice, &module, nullptr, &modules[i]), "Create Vulkan shader module");
                }
                shaders[i]
                    = vkInfo<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
                shaders[i].stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
                shaders[i].module = modules[i];
                shaders[i].pName = "main";
                shaders[i].pSpecializationInfo = &specialization;
            }
            const VkVertexInputBindingDescription binding{ 0, sizeof(Render::Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
            const std::array<VkVertexInputAttributeDescription, 6> attributes{ {
                { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Render::Vertex, mPosition) },
                { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Render::Vertex, mUV) },
                { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Render::Vertex, mColor) },
                { 3, 0, VK_FORMAT_R32_SFLOAT, offsetof(Render::Vertex, mAlpha) },
                { 4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Render::Vertex, mMaskUV) },
                { 5, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Render::Vertex, mNormal) },
            } };
            auto vertex = vkInfo<VkPipelineVertexInputStateCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
            vertex.vertexBindingDescriptionCount = 1;
            vertex.pVertexBindingDescriptions = &binding;
            vertex.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
            vertex.pVertexAttributeDescriptions = attributes.data();
            auto assembly = vkInfo<VkPipelineInputAssemblyStateCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            auto viewport
                = vkInfo<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
            viewport.viewportCount = 1;
            viewport.scissorCount = 1;
            auto raster = vkInfo<VkPipelineRasterizationStateCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            raster.lineWidth = 1;
            raster.depthClampEnable = material == static_cast<unsigned>(MaterialShader::Shadow) && mFeatures.depthClamp;
            raster.depthBiasEnable = material == static_cast<unsigned>(MaterialShader::Shadow);
            auto samples = vkInfo<VkPipelineMultisampleStateCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
            samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            auto depth = vkInfo<VkPipelineDepthStencilStateCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
            depth.depthTestEnable = VK_TRUE;
            depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            VkPipelineColorBlendAttachmentState blend{};
            blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
                | VK_COLOR_COMPONENT_A_BIT;
            blend.colorBlendOp = VK_BLEND_OP_ADD;
            blend.alphaBlendOp = VK_BLEND_OP_ADD;
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            auto blending
                = vkInfo<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
            blending.attachmentCount = 1;
            blending.pAttachments = &blend;
            const std::array dynamicStates{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_CULL_MODE_EXT, VK_DYNAMIC_STATE_FRONT_FACE_EXT,
                VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT,
                VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT };
            auto dynamic
                = vkInfo<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
            dynamic.dynamicStateCount = mDynamicRaster ? static_cast<std::uint32_t>(dynamicStates.size()) : 3;
            dynamic.pDynamicStates = dynamicStates.data();
            auto pipeline = vkInfo<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
            pipeline.stageCount = static_cast<std::uint32_t>(shaders.size());
            pipeline.pStages = shaders.data();
            pipeline.pVertexInputState = &vertex;
            pipeline.pInputAssemblyState = &assembly;
            pipeline.pViewportState = &viewport;
            pipeline.pRasterizationState = &raster;
            pipeline.pMultisampleState = &samples;
            pipeline.pDepthStencilState = &depth;
            pipeline.pColorBlendState = &blending;
            pipeline.pDynamicState = &dynamic;
            pipeline.layout = pbr ? mPbrPipelineLayout : mPipelineLayout;
            pipeline.renderPass = pipelineIndex >= PipelinesPerTarget
                ? mTargetPasses[pipelineIndex / PipelinesPerTarget - 1].mClear
                : mRenderPass;
            const std::size_t mode = (rasterKey / 4) % BlendCount;
            raster.cullMode = (rasterKey / 2) % 2 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
            // Vulkan's signed-area definition already accounts for framebuffer
            // Y-down coordinates. With our negative-height viewport, the
            // source CCW convention remains CCW; flipping it culls the terrain
            // surface and renders the inside of closed meshes instead.
            raster.frontFace = Render::counterClockwiseFrontFace(pipelineIndex < PipelinesPerTarget, rasterKey % 2 != 0)
                ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                : VK_FRONT_FACE_CLOCKWISE;
            const auto depthMode = (rasterKey / DepthStride) % 3;
            const bool hasDepth = pipelineIndex < PipelinesPerTarget
                || Render::targetHasDepth(static_cast<Render::TargetFormat>(pipelineIndex / PipelinesPerTarget - 1));
            depth.depthTestEnable = hasDepth && depthMode != 0;
            depth.depthWriteEnable = hasDepth && depthMode == 2;
            constexpr std::array comparisons{ VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS,
                VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_NOT_EQUAL,
                VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_GREATER };
            depth.depthCompareOp = comparisons[rasterKey / CompareStride];
            if (material == static_cast<unsigned>(MaterialShader::Shadow))
                blend.colorWriteMask = 0;
            blend.blendEnable = mode != 0;
            blend.srcColorBlendFactor = mode == static_cast<std::size_t>(Render::BlendMode::PremultipliedAlpha)
                ? VK_BLEND_FACTOR_ONE
                : VK_BLEND_FACTOR_SRC_ALPHA;
            blend.dstColorBlendFactor = mode == 3 ? VK_BLEND_FACTOR_ZERO
                : mode == 2                       ? VK_BLEND_FACTOR_ONE
                                                  : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend.dstAlphaBlendFactor = blend.dstColorBlendFactor;
            const std::array blendAttachments{ blend, blend };
            if (pipelineIndex >= PipelinesPerTarget
                && hasNormals(static_cast<Render::TargetFormat>(pipelineIndex / PipelinesPerTarget - 1)))
            {
                blending.attachmentCount = 2;
                blending.pAttachments = blendAttachments.data();
            }
            check(vkCreateGraphicsPipelines(mDevice, mPipelineCache, 1, &pipeline, nullptr, &mPipelines[pipelineIndex]),
                "Create Vulkan material pipeline");
        }

        void VulkanBackend::retainGeometry(Frame& frame, const std::shared_ptr<GeometryBuffer>& buffer)
        {
            if (buffer->mLastSubmission != mSubmission)
            {
                frame.mResources.push_back(buffer);
                buffer->mLastSubmission = mSubmission;
            }
        }

        void VulkanBackend::recordUploads(Frame& frame, VkDeviceSize instanceBytes, VkDeviceSize lightBytes)
        {
            bool copiedBuffers = instanceBytes != 0 || lightBytes != 0;
            // Writable geometry versions have no outstanding GPU readers.
            // Only order transfer writes before copies (including preserved tails),
            // allowing uploads to overlap unrelated vertex/index reads.
            auto before = vkInfo<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
            before.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(frame.mCommands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                &before, 0, nullptr, 0, nullptr);
            if (instanceBytes)
            {
                const VkBufferCopy copy{ 0, 0, instanceBytes };
                vkCmdCopyBuffer(frame.mCommands, frame.mInstanceUpload->mBuffer, frame.mInstances->mBuffer, 1, &copy);
            }
            if (lightBytes)
            {
                const VkBufferCopy copy{ 0, 0, lightBytes };
                vkCmdCopyBuffer(frame.mCommands, frame.mLightUpload->mBuffer, frame.mLights->mBuffer, 1, &copy);
            }
            Render::reserveFor(frame.mUploads, frame.mUploads.size() + mPendingUploads.size());
            Render::reserveFor(frame.mResources, frame.mResources.size() + mPendingUploads.size() * 3);
            for (auto& upload : mPendingUploads)
            {
                if (!upload.mStaging && !upload.mImage)
                    continue;
                if (upload.mMesh)
                {
                    (upload.mIndices ? upload.mMesh->mIndexUpload : upload.mMesh->mVertexUpload).reset();
                    const auto& version
                        = (upload.mIndices ? upload.mMesh->mIndices : upload.mMesh->mVertices).current();
                    const auto& destination = version->mRange;
                    if (upload.mPreserve)
                    {
                        const auto& source = upload.mPreserve->mRange;
                        const auto capacity = upload.mIndices ? upload.mMesh->mIndexBytes : upload.mMesh->mVertexBytes;
                        if (upload.mSize < capacity)
                        {
                            const VkBufferCopy tail{ source.offset() + upload.mSize,
                                destination.offset() + upload.mSize, capacity - upload.mSize };
                            vkCmdCopyBuffer(
                                frame.mCommands, source.page().mBuffer, destination.page().mBuffer, 1, &tail);
                        }
                        retainGeometry(frame, upload.mPreserve);
                    }
                    retainGeometry(frame, version);
                    const VkBufferCopy copy{ upload.mStaging.mOffset, destination.offset(), upload.mSize };
                    vkCmdCopyBuffer(
                        frame.mCommands, upload.mStaging.mPage->mBuffer, destination.page().mBuffer, 1, &copy);
                    if (upload.mMesh->mLastSubmission != mSubmission)
                    {
                        frame.mResources.push_back(upload.mMesh);
                        upload.mMesh->mLastSubmission = mSubmission;
                    }
                    copiedBuffers = true;
                }
                else
                {
                    upload.mImage->mUpload.reset();
                    auto& image = *upload.mImage;
                    image.mWriter = VK_PIPELINE_STAGE_TRANSFER_BIT;
                    image.mVisible = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                    auto barrier = vkInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
                    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.image = image.mImage;
                    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, image.mMipLevels, 0, 1 };
                    vkCmdPipelineBarrier(frame.mCommands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                    if (image.mTarget || !upload.mStaging)
                    {
                        const VkClearColorValue clear{};
                        vkCmdClearColorImage(frame.mCommands, image.mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            &clear, 1, &barrier.subresourceRange);
                    }
                    else
                        vkCmdCopyBufferToImage(frame.mCommands, upload.mStaging.mPage->mBuffer, image.mImage,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            static_cast<std::uint32_t>(upload.mImageCopies.size()), upload.mImageCopies.data());
                    std::int32_t mipWidth = static_cast<std::int32_t>(image.mWidth);
                    std::int32_t mipHeight = static_cast<std::int32_t>(image.mHeight);
                    for (std::uint32_t level = 1; upload.mGenerateMipmaps && level < image.mMipLevels; ++level)
                    {
                        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        barrier.subresourceRange.baseMipLevel = level - 1;
                        barrier.subresourceRange.levelCount = 1;
                        vkCmdPipelineBarrier(frame.mCommands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                        VkImageBlit blit{};
                        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1 };
                        blit.srcOffsets[1] = { mipWidth, mipHeight, 1 };
                        mipWidth = std::max(1, mipWidth / 2);
                        mipHeight = std::max(1, mipHeight / 2);
                        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
                        blit.dstOffsets[1] = { mipWidth, mipHeight, 1 };
                        vkCmdBlitImage(frame.mCommands, image.mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            image.mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                    }
                    if (upload.mGenerateMipmaps && image.mMipLevels > 1)
                    {
                        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
                        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        barrier.subresourceRange.baseMipLevel = 0;
                        barrier.subresourceRange.levelCount = image.mMipLevels - 1;
                        vkCmdPipelineBarrier(frame.mCommands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT), 0, 0,
                            nullptr, 0, nullptr, 1, &barrier);
                    }
                    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    barrier.subresourceRange.baseMipLevel = upload.mGenerateMipmaps ? image.mMipLevels - 1 : 0;
                    barrier.subresourceRange.levelCount = upload.mGenerateMipmaps ? 1 : image.mMipLevels;
                    vkCmdPipelineBarrier(frame.mCommands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT), 0, 0, nullptr, 0,
                        nullptr, 1, &barrier);
                    frame.mResources.push_back(upload.mImage);
                }
                if (upload.mStaging && upload.mStaging.mPage->mLastUploadSubmission != mSubmission)
                {
                    frame.mUploads.push_back(upload.mStaging.mPage);
                    upload.mStaging.mPage->mLastUploadSubmission = mSubmission;
                }
            }
            mPendingUploads.clear();
            if (copiedBuffers)
            {
                auto barrier = vkInfo<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask
                    = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(frame.mCommands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
                        | (mOptions.mPbr ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : 0),
                    0, 1, &barrier, 0, nullptr, 0, nullptr);
            }
        }

        void VulkanBackend::recordCapture(VkCommandBuffer commands, std::uint32_t imageIndex)
        {
            if (!mCaptureSupported)
                throw std::runtime_error("This Vulkan surface does not support screenshot readback");
            mReadback = createBuffer(
                VkDeviceSize(mExtent.width) * mExtent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryKind::Upload);
            auto barrier = vkInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = mSwapchainImages[imageIndex];
            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { mExtent.width, mExtent.height, 1 };
            vkCmdCopyImageToBuffer(
                commands, barrier.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mReadback->mBuffer, 1, &region);
            // GUI rendering follows scene-only capture in the same submission.
            // Its color load/writes must wait for the transfer read to finish.
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(commands,
                VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            auto host = vkInfo<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0,
                nullptr, 0, nullptr);
        }

        Render::CapturedImage VulkanBackend::takeCapture()
        {
            if (!mCaptureFrame)
                return {};
            const auto result = vkWaitForFences(mDevice, 1, &mFrames[*mCaptureFrame].mFence, VK_TRUE, 2'000'000'000);
            check(result, "Wait for Vulkan screenshot readback");
            Render::CapturedImage image;
            image.mWidth = mCaptureExtent.width;
            image.mHeight = mCaptureExtent.height;
            image.mRgba.resize(std::size_t(image.mWidth) * image.mHeight * 4);
            std::memcpy(image.mRgba.data(), mReadback->mMemory.mapped(), image.mRgba.size());
            if (mCaptureFormat == VK_FORMAT_B8G8R8A8_UNORM || mCaptureFormat == VK_FORMAT_B8G8R8A8_SRGB)
                for (std::size_t i = 0; i < image.mRgba.size(); i += 4)
                    std::swap(image.mRgba[i], image.mRgba[i + 2]);
            mCaptureFrame.reset();
            mReadback.reset();
            return image;
        }

        bool VulkanBackend::render(std::span<const Render::Draw> draws, const std::array<float, 4>& clearColor)
        {
            const Render::Pass pass{ draws, {}, clearColor, true, {} };
            return renderPasses({ &pass, 1 });
        }

        bool VulkanBackend::renderPasses(std::span<const Render::Pass> passes)
        {
            if (mFailed)
                throw std::runtime_error("Vulkan renderer cannot continue after submission failure");
            try
            {
                return renderFrame(passes);
            }
            catch (...)
            {
                // A failure after acquisition/recording may have consumed
                // upload work or left binary semaphores signaled. Require
                // orderly backend destruction instead of retrying that state.
                mFailed = true;
                throw;
            }
        }

        bool VulkanBackend::renderFrame(std::span<const Render::Pass> passes)
        {
            mFrameDraws.clear();
            mShadowDescriptors.beginFrame();
            mFramePbrDescriptors.beginFrame();
            mPreparedPasses.clear();
            for (const auto& pass : passes)
            {
                std::shared_ptr<Image> target;
                if (pass.mTarget)
                {
                    target = mTextures.get(pass.mTarget, "Invalid or expired Vulkan render target handle");
                    if (!target->mTarget)
                        throw std::invalid_argument("Render pass texture is not a framebuffer target");
                }
                mPreparedPasses.push_back(
                    { mFrameDraws.size(), pass.mDraws.size(), target, pass.mClearColor, pass.mClear, pass.mViewport });
                mPreparedPasses.back().mPreserveWorldDepth = pass.mPreserveWorldDepth;
                mPreparedPasses.back().mGenerateMipmaps = pass.mGenerateMipmaps;
                mPreparedPasses.back().mEffectBlend = pass.mEffectBlend;
                mPreparedPasses.back().mDiscardDepthAfter = pass.mDiscardDepthAfter;
                mPreparedPasses.back().mGui = pass.mGui;
                if (pass.mGenerateMipmaps && (!target || target->mDepth))
                    throw std::invalid_argument("Mip generation requires a color-only render target");
                mPreparedPasses.back().mOverwrite = pass.mOverwrite;
                mPreparedPasses.back().mReversedDepth = pass.mReversedDepth;
                if (pass.mOverwrite && (!pass.mEffect || pass.mEffectBlend.mEnabled || (target && target->mDepth)))
                    throw std::invalid_argument("Only color-only fullscreen effects can overwrite a target");
                Render::reserveFor(mFrameDraws, mFrameDraws.size() + pass.mDraws.size());
                for (const auto& draw : pass.mDraws)
                {
                    mFrameDraws.push_back(&draw);
                    mPreparedPasses.back().mWater |= draw.mWater;
                }
            }
            if (passes.empty()
                || std::none_of(passes.begin(), passes.end(), [](const auto& pass) { return !pass.mTarget; }))
                throw std::invalid_argument("A presented Vulkan frame requires a window pass");
            const auto& draws = mFrameDraws;
            const auto maxInstances
                = std::min<std::size_t>(mProperties.limits.maxStorageBufferRange / sizeof(InstanceData),
                    std::numeric_limits<std::size_t>::max() / sizeof(InstanceData));
            if (draws.size() > maxInstances)
                throw std::length_error("Vulkan frame exceeds the supported instance count");
            int width = 0;
            int height = 0;
            SDL_Vulkan_GetDrawableSize(mOptions.mWindow, &width, &height);
            if (width <= 0 || height <= 0)
                return false;
            Frame& frame = mFrames[mFrameIndex];
            if (!frame.mAcquiredImage)
                refreshSurface();
            if (!frame.mAcquiredImage
                && (mResize || !mSwapchain || static_cast<unsigned>(width) != mDrawableExtent.width
                    || static_cast<unsigned>(height) != mDrawableExtent.height))
                if (!createSwapchain())
                    return false;

            const auto reusable = vkWaitForFences(mDevice, 1, &frame.mFence, VK_TRUE, 2'000'000);
            if (reusable == VK_TIMEOUT)
                return false;
            check(reusable, "Wait for reusable frame");
            collectTimings(frame);
            frame.mRegions.clear();
            frame.mQueryCount = 0;
            if (mDiagnostics && mTimestampBits && !frame.mQueries)
            {
                auto queries = vkInfo<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
                queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
                queries.queryCount = 512;
                check(vkCreateQueryPool(mDevice, &queries, nullptr, &frame.mQueries), "Create GPU timing queries");
            }
            for (auto& fence : mImageFences)
                if (fence == frame.mFence)
                    fence = VK_NULL_HANDLE;
            frame.mResources.clear();
            frame.mUploads.clear();
            mPrepared.clear();
            for (auto pool : frame.mEffectPools)
                check(vkResetDescriptorPool(mDevice, pool, 0), "Recycle effect descriptors");
            frame.mEffectPoolIndex = 0;
            frame.mEffectUniforms.beginFrame();
            frame.mUniformPage = frame.mUniformOffset = 0;
            frame.mEffectDescriptorsUsed = 0;
            if (mSubmission % 16 == 0)
            {
                mStagingPages.trim(Render::uploadMemoryRetentionBytes(mInfo.mLocalMemoryBytes), 32);
                mGeometryPages.trim(32 * 1024 * 1024, 8);
                mAllocator->trim();
                if (!mPbrDescriptors.empty())
                {
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(100);
                    mPbrDescriptorSweep.sweep(
                        mPbrDescriptors,
                        [&](const auto& item) { return !item.second || item.second->mSubmission + 300 < mSubmission; },
                        [&] { return std::chrono::steady_clock::now() >= deadline; });
                }
            }
            Render::reserveFor(mPrepared, draws.size());
            Render::reserveFor(frame.mResources, draws.size() * 5 + mPendingUploads.size() * 3 + passes.size() * 4);
            ++mSubmission;
            if (mPassSamples.size() < mPreparedPasses.size())
                mPassSamples.resize(mPreparedPasses.size());
            for (std::size_t i = 0; i < mPreparedPasses.size(); ++i)
            {
                auto& pass = mPreparedPasses[i];
                pass.mLabel = passes[i].mLabel;
                pass.mEnvironment = passes[i].mEnvironment;
                auto& samples = mPassSamples[i];
                samples.clear();
                if (pass.mTarget)
                    frame.mResources.push_back(pass.mTarget);
                if (pass.mWater)
                {
                    if (pass.mTarget && !pass.mTarget->mDepth)
                        throw std::invalid_argument("Water requires a depth attachment");
                    auto& snapshot = pass.mTarget ? pass.mTarget->mWaterDepth : frame.mWaterDepth;
                    const auto w = pass.mTarget ? pass.mTarget->mWidth : mExtent.width;
                    const auto h = pass.mTarget ? pass.mTarget->mHeight : mExtent.height;
                    if (!snapshot || snapshot->mWidth != w || snapshot->mHeight != h)
                    {
                        snapshot = createDepthSnapshot(w, h);
                        (pass.mTarget ? pass.mTarget->mWaterDepthInitialized : frame.mWaterDepthInitialized) = false;
                    }
                    pass.mWaterDepth = snapshot;
                    frame.mResources.push_back(snapshot);
                }
                if (passes[i].mEffect)
                {
                    if (pass.mCount || (pass.mTarget && pass.mTarget->mDepth))
                        throw std::invalid_argument("Effects require a color-only output");
                    pass.mEffect = mEffects.get(passes[i].mEffect);
                    frame.mResources.push_back(pass.mEffect);
                    for (const auto& input : passes[i].mEffectInputs)
                    {
                        if (input.mTexture)
                        {
                            const auto binding
                                = std::find_if(pass.mEffect->mBindings.begin(), pass.mEffect->mBindings.end(),
                                    [&](const auto& b) { return b.mBinding == input.mBinding; });
                            if (binding == pass.mEffect->mBindings.end())
                                throw std::invalid_argument("Unknown effect texture binding");
                            const VkPipelineStageFlags stages
                                = (binding->mStages & 1 ? VK_PIPELINE_STAGE_VERTEX_SHADER_BIT : 0)
                                | (binding->mStages & 2 ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : 0);
                            samples.push_back({ mTextures.get(input.mTexture).get(), stages });
                        }
                        if (pass.mTarget && input.mTexture
                            && (mTextures.get(input.mTexture) == pass.mTarget
                                || mTextures.get(input.mTexture) == pass.mTarget->mDepth
                                || mTextures.get(input.mTexture) == pass.mTarget->mNormals))
                            throw std::invalid_argument("An effect cannot sample its own framebuffer attachment");
                    }
                    pass.mSamples = samples;
                    pass.mEffectDescriptor = effectDescriptor(frame, *pass.mEffect, passes[i].mEffectInputs);
                    pass.mEffectPipeline = effectPipeline(*pass.mEffect, pass.mTarget.get(), pass.mEffectBlend);
                    if (!pass.mEffectPipeline)
                        return false;
                    frame.mResources.push_back(mMeshes.get(mFullscreenMesh));
                }
            }
            for (std::size_t i = frame.mEffectDescriptorsUsed; i < frame.mEffectDescriptors.size(); ++i)
            {
                auto& unused = frame.mEffectDescriptors[i];
                if (unused.mSet)
                    check(vkFreeDescriptorSets(mDevice, unused.mPool, 1, &unused.mSet),
                        "Release unused effect descriptor");
                unused = {};
            }
            std::size_t drawPass = 0;
            std::size_t drawNumber = 0;
            for (const auto* source : draws)
            {
                const auto& draw = *source;
                const auto& mesh = mMeshes.get(draw.mMesh, "Invalid or expired Vulkan draw mesh handle");
                const auto& texture = mTextures.get(draw.mTexture ? draw.mTexture : mWhiteTexture,
                    "Invalid or expired Vulkan draw texture handle");
                const auto& mask = mTextures.get(draw.mMaskTexture ? draw.mMaskTexture : mWhiteTexture,
                    "Invalid or expired Vulkan blend or reflection texture handle");
                while (drawPass < mPreparedPasses.size()
                    && drawNumber >= mPreparedPasses[drawPass].mFirst + mPreparedPasses[drawPass].mCount)
                    ++drawPass;
                const auto& destination = mPreparedPasses[drawPass].mTarget;
                if (destination && (texture == destination || mask == destination))
                    throw std::invalid_argument("A render target cannot sample its own attachment");
                const bool offscreen = destination != nullptr;
                ++drawNumber;
                const bool reversedDepth = mPreparedPasses[drawPass].mReversedDepth;
                const auto comparison
                    = reversedDepth ? Render::reverseDepthComparison(draw.mDepthCompare) : draw.mDepthCompare;
                const std::size_t rasterKey = 4 * static_cast<std::size_t>(draw.mBlend) + (draw.mDoubleSided ? 2 : 0)
                    + (draw.mFlipWinding ? 1 : 0)
                    + (draw.mDepthTest ? draw.mDepthWrite ? DepthStride * 2 : DepthStride : 0)
                    + CompareStride * static_cast<std::size_t>(comparison);
                auto material = draw.mShadowCasting ? MaterialShader::Shadow
                    : draw.mDistortion              ? MaterialShader::Distortion
                    : draw.mSky                     ? MaterialShader::Sky
                    : draw.mWater                   ? MaterialShader::Water
                    : !offscreen && !draw.mLighting && !draw.mDepthTest && !draw.mDepthWrite && !draw.mClip
                        && !draw.mMaskTexture && draw.mAlphaCompare == Render::AlphaCompare::Always
                        && (draw.mFogEnd <= draw.mFogStart || draw.mFogEnd <= 0)
                    ? MaterialShader::Overlay
                    : draw.mAlphaCompare == Render::AlphaCompare::Always && !draw.mClip ? MaterialShader::SolidSurface
                                                                                        : MaterialShader::Surface;
                if (mOptions.mPbr && draw.mPbrBake != Render::Draw::PbrBake::None)
                    material = MaterialShader::PbrBake;
                else if (mOptions.mPbr && draw.mPbr
                    && (material == MaterialShader::Surface || material == MaterialShader::SolidSurface))
                {
                    const bool solid = material == MaterialShader::SolidSurface;
                    material = draw.mShadows
                        ? (solid ? MaterialShader::PbrSolidSurfaceShadowed : MaterialShader::PbrSurfaceShadowed)
                        : (solid ? MaterialShader::PbrSolidSurface : MaterialShader::PbrSurface);
                }
                else if (draw.mShadows && material == MaterialShader::Surface)
                    material = MaterialShader::SurfaceShadowed;
                else if (draw.mShadows && material == MaterialShader::SolidSurface)
                    material = MaterialShader::SolidSurfaceShadowed;
                const std::size_t pipeline
                    = (offscreen ? PipelinesPerTarget * (static_cast<std::size_t>(destination->mTargetFormat) + 1) : 0)
                    + RasterKeys * static_cast<std::size_t>(material)
                    + Render::bakedRasterKey(rasterKey, mDynamicRaster);
                if (draw.mBlend > Render::BlendMode::PremultipliedAlpha
                    || draw.mDepthCompare > Render::AlphaCompare::Greater || pipeline >= mPipelines.size())
                    throw std::invalid_argument("Invalid Vulkan blend mode");
                if (!mPipelines[pipeline])
                    createPipeline(pipeline);
                if (static_cast<unsigned>(draw.mAlphaCompare) > static_cast<unsigned>(Render::AlphaCompare::Greater)
                    || !std::isfinite(draw.mAlphaCutoff))
                    throw std::invalid_argument("Invalid Vulkan alpha comparison");
                if (mesh->mLastSubmission != mSubmission)
                {
                    frame.mResources.push_back(mesh);
                    retainGeometry(frame, mesh->mVertices.current());
                    retainGeometry(frame, mesh->mIndices.current());
                    mesh->mLastSubmission = mSubmission;
                }
                if (texture->mLastSubmission != mSubmission)
                {
                    frame.mResources.push_back(texture);
                    texture->mLastSubmission = mSubmission;
                }
                if (mask->mLastSubmission != mSubmission)
                {
                    frame.mResources.push_back(mask);
                    mask->mLastSubmission = mSubmission;
                }
                if (draw.mIndexCount > mesh->mIndexCount || draw.mIndexCount % 3)
                    throw std::invalid_argument("Invalid dynamic mesh index count");
                mPrepared.push_back({ mesh.get(), texture.get(), mask.get(), pipeline, rasterKey,
                    draw.mClearDepthBefore, draw.mIndexCount ? draw.mIndexCount : mesh->mIndexCount,
                    { draw.mFogColor[0], draw.mFogColor[1], draw.mFogColor[2], draw.mFogColor[3], draw.mFogStart,
                        draw.mFogEnd, draw.mBlend == Render::BlendMode::Additive && !draw.mTerrainLayer ? 0.f : 1.f,
                        draw.mBlend == Render::BlendMode::PremultipliedAlpha ? 1.f : 0.f },
                    draw.mWater });
                mPrepared.back().mReversedDepth = reversedDepth;
                mPrepared.back().mDepthBias = { draw.mDepthBiasConstant, draw.mDepthBiasSlope };
                if (isPbr(material))
                {
                    auto& prepared = mPrepared.back();
                    const auto& binding = pbrDescriptor(frame, draw);
                    prepared.mPbrDescriptor = binding.mSet;
                    prepared.mPbrTargets = binding.mTargets;
                    if (destination
                        && (binding.mTargets[0] == destination.get() || binding.mTargets[1] == destination.get()))
                        throw std::invalid_argument("PBR material cannot sample its own render target");
                    auto flags = binding.mMapFlags;
                    if (draw.mNormalXY)
                        flags |= 512u;
                    if (draw.mTerrainSpecularAlpha)
                        flags |= 1024u;
                    if (!texture->mSrgb)
                        flags |= 2048u;
                    if (offscreen
                        || (mColorFormat != VK_FORMAT_B8G8R8A8_SRGB && mColorFormat != VK_FORMAT_R8G8B8A8_SRGB))
                        flags |= 4096u;
                    if (draw.mPbrBake == Render::Draw::PbrBake::Normal)
                        flags |= 16384u;
                    if (draw.mPbrBakedTerrain)
                        flags |= 262144u;
                    flags |= (std::uint32_t{ draw.mPbrAlternateUV } & 15u) << 19;
                    prepared.mPbrFlags = flags;
                }
            }
            if (draws.size() > frame.mInstanceCapacity)
            {
                const auto grown
                    = frame.mInstanceCapacity > maxInstances / 2 ? maxInstances : frame.mInstanceCapacity * 2;
                const auto capacity
                    = std::max(draws.size(), std::min(maxInstances, std::max(std::size_t{ 256 }, grown)));
                auto upload = createBuffer(
                    capacity * sizeof(InstanceData), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryKind::Upload);
                auto instances = createBuffer(capacity * sizeof(InstanceData),
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemoryKind::Geometry);
                frame.mInstanceUpload = std::move(upload);
                frame.mInstances = std::move(instances);
                frame.mInstanceCapacity = capacity;
            }
            mLightData.clear();
            // At most one entry per draw, with a load factor <= 1/2. A flat
            // frame-local table reuses its backing allocation instead of
            // destroying/reallocating an unordered_map node per light set.
            mLightOffsets.assign(std::bit_ceil(std::max(std::size_t{ 2 }, draws.size() * 2)), {});
            mDrawLightOffsets.resize(draws.size());
            for (std::size_t i = 0; i < draws.size(); ++i)
            {
                const auto& set = draws[i]->mLights;
                if (!draws[i]->mLighting || draws[i]->mSky || draws[i]->mWater || draws[i]->mDistortion || !set
                    || set->mPoints.empty())
                {
                    mDrawLightOffsets[i] = 0;
                    continue;
                }
                const bool linear = mPrepared[i].mPbrDescriptor != VK_NULL_HANDLE;
                auto slot = (reinterpret_cast<std::uintptr_t>(set.get()) >> 3) ^ static_cast<unsigned>(linear);
                slot ^= slot >> 17;
                slot *= 0xed5ad4bbu;
                slot ^= slot >> 11;
                const auto mask = mLightOffsets.size() - 1;
                slot &= mask;
                while (mLightOffsets[slot].mSet
                    && (mLightOffsets[slot].mSet != set.get() || mLightOffsets[slot].mLinear != linear))
                    slot = (slot + 1) & mask;
                auto& entry = mLightOffsets[slot];
                if (!entry.mSet)
                {
                    if ((mLightData.size() + set->mPoints.size()) * sizeof(Render::PointLight)
                        > mProperties.limits.maxStorageBufferRange)
                        throw std::length_error("Vulkan point light data exceeds the device buffer limit");
                    entry = { set.get(), static_cast<std::uint32_t>(mLightData.size()), linear };
                    mLightData.insert(mLightData.end(), set->mPoints.begin(), set->mPoints.end());
                    if (linear)
                        for (std::size_t index = entry.mOffset; index < mLightData.size(); ++index)
                        {
                            auto& light = mLightData[index];
                            light.mAmbient = Render::linearRgbFromSrgb(light.mAmbient);
                            light.mDiffuse = Render::linearRgbFromSrgb(light.mDiffuse);
                            light.mSpecular = Render::linearRgbFromSrgb(light.mSpecular);
                        }
                }
                mDrawLightOffsets[i] = entry.mOffset;
            }
            if (!draws.empty() && (!frame.mLights || mLightData.size() > frame.mLightCapacity))
            {
                const auto maximum = mProperties.limits.maxStorageBufferRange / sizeof(Render::PointLight);
                const auto grown = frame.mLightCapacity > maximum / 2 ? maximum : frame.mLightCapacity * 2;
                const auto capacity
                    = std::max(mLightData.size(), std::min(maximum, std::max(std::size_t{ 64 }, grown)));
                auto upload = createBuffer(
                    capacity * sizeof(Render::PointLight), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryKind::Upload);
                auto data = createBuffer(capacity * sizeof(Render::PointLight),
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemoryKind::Geometry);
                frame.mLightUpload = std::move(upload);
                frame.mLights = std::move(data);
                frame.mLightCapacity = capacity;
            }
            if (!mLightData.empty())
                std::memcpy(frame.mLightUpload->mMemory.mapped(), mLightData.data(),
                    mLightData.size() * sizeof(Render::PointLight));
            for (std::size_t i = 0; i < draws.size(); ++i)
                mPrepared[i].mDataDescriptor = dataDescriptor(frame, draws[i]->mShadows.get());
            for (std::size_t i = 0; i < draws.size(); ++i)
            {
                const auto& draw = *draws[i];
                auto ambient = draw.mAmbient;
                auto emission = draw.mEmission;
                if (!draw.mSky)
                {
                    // Ambient/emission alpha do not contribute to surface alpha;
                    // these slots carry multipliers for either material or vertex colors.
                    ambient[3] = draw.mSpecStrength;
                    emission[3] = draw.mEmissiveMult;
                }
                auto specular = draw.mSpecular;
                if (draw.mWater)
                    ambient = draw.mWaterProjection;
                specular[3] = draw.mShininess;
                const bool linear = draw.mLighting && mPrepared[i].mPbrDescriptor != VK_NULL_HANDLE;
                const InstanceData instance{ draw.mClipFromModel,
                    linear ? Render::linearRgbFromSrgb(draw.mTint) : draw.mTint,
                    { draw.mAlphaCutoff, static_cast<float>(draw.mAlphaCompare),
                        draw.mDistortion ? draw.mDistortionStrength : draw.mOpacity,
                        draw.mDistortion        ? 3.f
                            : draw.mSky         ? (draw.mSkyPass >= 0 ? 4.f + draw.mSkyPass : 11.f)
                            : draw.mWater       ? (draw.mReflection ? 2.25f : 2.f)
                            : draw.mMaskTexture ? 1.f
                                                : 0.f },
                    { draw.mUVTransform[0], draw.mUVTransform[1], draw.mUVTransform[2], draw.mRoughness },
                    { draw.mUVTransform[3], draw.mUVTransform[4], draw.mUVTransform[5], draw.mMetalness },
                    { draw.mMaskUVTransform[0], draw.mMaskUVTransform[1], draw.mMaskUVTransform[2], 0 },
                    { draw.mMaskUVTransform[3], draw.mMaskUVTransform[4], draw.mMaskUVTransform[5], 0 },
                    draw.mViewFromModel, draw.mNormalFromModel,
                    linear ? Render::linearRgbFromSrgb(draw.mDiffuse) : draw.mDiffuse,
                    linear ? Render::linearRgbFromSrgb(ambient) : ambient,
                    linear ? Render::linearRgbFromSrgb(emission) : emission, specular, draw.mSunPosition,
                    linear ? Render::linearRgbFromSrgb(draw.mSunAmbient) : draw.mSunAmbient,
                    linear ? Render::linearRgbFromSrgb(draw.mSunDiffuse) : draw.mSunDiffuse,
                    linear ? Render::linearRgbFromSrgb(draw.mSunSpecular) : draw.mSunSpecular,
                    { mDrawLightOffsets[i],
                        draw.mLighting && !draw.mSky && !draw.mWater && !draw.mDistortion && draw.mLights
                            ? static_cast<std::uint32_t>(draw.mLights->mPoints.size())
                            : 0,
                        (draw.mLighting && !draw.mWater && !draw.mSky ? 1u : 0u) | (draw.mClassicFalloff ? 2u : 0u)
                            | (draw.mClip ? 4u : 0u) | (mPrepared[i].mReversedDepth ? 8u : 0u)
                            | (draw.mShadowAlphaTest ? 16u : 0u) | mPrepared[i].mPbrFlags,
                        draw.mUseVertexColor ? draw.mVertexColorMode : 0u },
                    draw.mClipPlane };
                std::memcpy(
                    frame.mInstanceUpload->mMemory.mapped() + i * sizeof(InstanceData), &instance, sizeof(instance));
            }
            std::uint32_t imageIndex = frame.mAcquiredImage.value_or(0);
            const auto acquired = frame.mAcquiredImage
                ? VK_SUCCESS
                : vkAcquireNextImageKHR(mDevice, mSwapchain, 2'000'000, frame.mAcquired, VK_NULL_HANDLE, &imageIndex);
            if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY)
                return false;
            if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
            {
                mResize = true;
                return false;
            }
            if (acquired != VK_SUBOPTIMAL_KHR)
                check(acquired, "Acquire Vulkan swapchain image");
            frame.mAcquiredImage = imageIndex;
            if (mImageFences[imageIndex])
            {
                const auto imageReady = vkWaitForFences(mDevice, 1, &mImageFences[imageIndex], VK_TRUE, 2'000'000);
                if (imageReady == VK_TIMEOUT)
                    return false;
                check(imageReady, "Wait for swapchain image");
            }
            check(vkResetCommandPool(mDevice, frame.mPool, 0), "Recycle Vulkan command pool");
            auto begin = vkInfo<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vkBeginCommandBuffer(frame.mCommands, &begin), "Begin Vulkan frame");
            if (mDiagnostics && frame.mQueries)
                vkCmdResetQueryPool(frame.mCommands, frame.mQueries, 0, 512);
            recordUploads(frame, draws.size() * sizeof(InstanceData), mLightData.size() * sizeof(Render::PointLight));
            bool windowWritten = mSwapchainInitialized[imageIndex];
            Render::CaptureBoundary capture(mCaptureRequested, mCaptureIncludesGui);
            for (const auto& view : mPreparedPasses)
            {
                if (capture.before(view.mGui, static_cast<bool>(view.mTarget)))
                    recordCapture(frame.mCommands, imageIndex);
                for (const auto& sample : view.mSamples)
                    sampleImage(frame, *sample.mImage, sample.mStages);
                for (std::size_t drawIndex = view.mFirst; drawIndex < view.mFirst + view.mCount; ++drawIndex)
                {
                    sampleImage(frame, *mPrepared[drawIndex].mTexture, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                    sampleImage(frame, *mPrepared[drawIndex].mMask, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                    for (auto* map : mPrepared[drawIndex].mPbrTargets)
                        if (map)
                            sampleImage(frame, *map, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                }
                const auto timing = beginTiming(frame,
                    !view.mLabel.empty() ? view.mLabel
                        : view.mGui      ? "GUI"
                        : view.mEffect   ? "Effect"
                                         : "Scene / camera");
                const VkExtent2D extent
                    = view.mTarget ? VkExtent2D{ view.mTarget->mWidth, view.mTarget->mHeight } : mExtent;
                const auto x = std::min(view.mViewport[0], extent.width);
                const auto y = std::min(view.mViewport[1], extent.height);
                const auto viewWidth = std::min(view.mViewport[2] ? view.mViewport[2] : extent.width, extent.width - x);
                const auto viewHeight
                    = std::min(view.mViewport[3] ? view.mViewport[3] : extent.height, extent.height - y);
                if (!viewWidth || !viewHeight)
                {
                    endTiming(frame, timing);
                    continue;
                }
                // Texture rows retain the source GL bottom-up convention. Only
                // the displayed window uses a negative-height viewport.
                const VkViewport viewport{ float(x), float(view.mTarget ? y : extent.height - y), float(viewWidth),
                    view.mTarget ? float(viewHeight) : -float(viewHeight), 0, 1 };
                const VkRect2D scissor{ { static_cast<std::int32_t>(x),
                                            static_cast<std::int32_t>(
                                                view.mTarget ? y : extent.height - y - viewHeight) },
                    { viewWidth, viewHeight } };
                // A base-only write may intentionally preserve the previous
                // pyramid. Finish that requested generation before replacing
                // level zero, even if no sampler has consumed it yet.
                if (view.mTarget && view.mTarget->mMipsDirty && !view.mGenerateMipmaps)
                    sampleImage(frame, *view.mTarget, 0);
                if (view.mTarget)
                    for (const auto& attachment : { view.mTarget, view.mTarget->mNormals })
                    {
                        if (!attachment)
                            continue;
                        auto barrier = vkInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
                        // Also cover LOAD after a prior attachment write or
                        // initial transfer clear, even if no shader sampled it.
                        barrier.srcAccessMask = (attachment->mReaders
                                                            & (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
                                                                | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
                                                        ? VK_ACCESS_SHADER_READ_BIT
                                                        : 0)
                            | (attachment->mWriter & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                    ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                    : 0)
                            | (attachment->mWriter & VK_PIPELINE_STAGE_TRANSFER_BIT ? VK_ACCESS_TRANSFER_WRITE_BIT : 0);
                        barrier.dstAccessMask
                            = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
                        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        barrier.image = attachment->mImage;
                        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                        vkCmdPipelineBarrier(frame.mCommands,
                            attachment->mReaders | attachment->mWriter | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                        attachment->mReaders = 0;
                    }
                if (view.mTarget && view.mTarget->mDepth && view.mTarget->mDepth->mDepthReadOnly)
                {
                    auto& depthImage = *view.mTarget->mDepth;
                    auto barrier = vkInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
                    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                        | (depthImage.mReaders ? VK_ACCESS_SHADER_READ_BIT : 0);
                    barrier.dstAccessMask
                        = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.image = depthImage.mImage;
                    barrier.subresourceRange = { depthImage.mAspect, 0, 1, 0, 1 };
                    if (mDepthFormat == VK_FORMAT_D24_UNORM_S8_UINT)
                        barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
                    constexpr auto tests
                        = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                    vkCmdPipelineBarrier(frame.mCommands, depthImage.mWriter | depthImage.mReaders, tests, 0, 0,
                        nullptr, 0, nullptr, 1, &barrier);
                    depthImage.mReaders = 0;
                    depthImage.mDepthReadOnly = false;
                }
                std::array<VkClearValue, 3> clear{};
                clear[2].color = { { .5f, .5f, .5f, 0.f } };
                std::copy(view.mClearColor.begin(), view.mClearColor.end(), clear[0].color.float32);
                clear[1].depthStencil = { view.mReversedDepth ? 0.f : 1.f, 0 };
                auto pass = vkInfo<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
                const auto targetSlot = view.mTarget ? static_cast<std::size_t>(view.mTarget->mTargetFormat) : 0;
                const bool discardDepth = view.mDiscardDepthAfter && view.mTarget && view.mTarget->mDepth
                    && !view.mWater && !view.mPreserveWorldDepth;
                pass.renderPass = discardDepth ? (view.mClear ? mTargetPasses[targetSlot].mDiscardDepthClear
                                                              : mTargetPasses[targetSlot].mDiscardDepthLoad)
                    : view.mTarget
                    ? (view.mClear              ? mTargetPasses[targetSlot].mClear
                              : view.mOverwrite ? mTargetPasses[targetSlot].mOverwrite
                                                : mTargetPasses[targetSlot].mLoad)
                    : (view.mClear || !windowWritten ? (view.mWater ? mMainWaterClearPass : mRenderPass)
                                                     : (view.mWater ? mMainWaterLoadPass : mMainLoadPass));
                if (!view.mTarget)
                    windowWritten = true;
                pass.framebuffer = view.mTarget ? view.mTarget->mFramebuffer
                                                : mFramebuffers[imageIndex * FramesInFlight + mFrameIndex];
                pass.renderArea = scissor;
                pass.clearValueCount = view.mTarget && view.mTarget->mNormals ? 3
                    : view.mTarget && !view.mTarget->mDepth                   ? 1
                                                                              : 2;
                pass.pClearValues = clear.data();
                vkCmdBeginRenderPass(frame.mCommands, &pass, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdSetViewport(frame.mCommands, 0, 1, &viewport);
                vkCmdSetScissor(frame.mCommands, 0, 1, &scissor);
                VkPipeline boundPipeline = VK_NULL_HANDLE;
                std::size_t boundRaster = std::numeric_limits<std::size_t>::max();
                VkDescriptorSet boundTexture = VK_NULL_HANDLE;
                VkDescriptorSet boundMask = VK_NULL_HANDLE;
                VkDescriptorSet boundData = VK_NULL_HANDLE;
                VkBuffer boundVertices = VK_NULL_HANDLE;
                VkBuffer boundIndices = VK_NULL_HANDLE;
                VkIndexType boundIndexType = VK_INDEX_TYPE_UINT32;
                VkDeviceSize boundVertexOffset = 0, boundIndexOffset = 0;
                std::optional<std::array<float, 8>> boundFog;
                vkCmdPushConstants(frame.mCommands, mPipelineLayout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 32, sizeof(Render::SceneEnvironment),
                    &view.mEnvironment);
                std::optional<std::array<float, 2>> boundDepthBias;
                const auto emptyDepth = mTextures.get(mWhiteTexture)->mDescriptor;
                VkDescriptorSet boundExtra = VK_NULL_HANDLE;
                if (!mPrepared.empty())
                {
                    vkCmdBindDescriptorSets(frame.mCommands, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 3, 1,
                        &emptyDepth, 0, nullptr);
                    boundExtra = emptyDepth;
                }
                bool savedWorldDepth = false;
                bool savedWaterDepth = false;
                const auto resume = [&](VkRenderPass renderPass) {
                    if (view.mTarget)
                        for (const auto& image : { view.mTarget, view.mTarget->mNormals })
                        {
                            if (!image)
                                continue;
                            auto barrier = vkInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
                            barrier.image = image->mImage;
                            barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                            barrier.dstAccessMask
                                = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                            vkCmdPipelineBarrier(frame.mCommands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                        }
                    pass.renderPass = renderPass;
                    vkCmdBeginRenderPass(frame.mCommands, &pass, VK_SUBPASS_CONTENTS_INLINE);
                };
                for (std::size_t first = view.mFirst; first < view.mFirst + view.mCount;)
                {
                    const auto& draw = mPrepared[first];
                    if (draw.mClearDepthBefore && view.mPreserveWorldDepth && view.mTarget && view.mTarget->mWorldDepth
                        && !savedWorldDepth)
                    {
                        vkCmdEndRenderPass(frame.mCommands);
                        copyWorldDepth(frame.mCommands, *view.mTarget);
                        savedWorldDepth = true;
                        // Preserve color/normals and clear depth for the weapon.
                        resume(mTargetPasses[targetSlot].mLoad);
                    }
                    else if (draw.mClearDepthBefore && (!view.mTarget || view.mTarget->mDepth))
                    {
                        VkClearAttachment depth{};
                        depth.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                        depth.clearValue.depthStencil = { view.mReversedDepth ? 0.f : 1.f, 0 };
                        const VkClearRect rect{ scissor, 0, 1 };
                        vkCmdClearAttachments(frame.mCommands, 1, &depth, 1, &rect);
                    }
                    if (draw.mWater && !savedWaterDepth)
                    {
                        vkCmdEndRenderPass(frame.mCommands);
                        const auto& source = view.mTarget ? view.mTarget->mDepth : mDepthImages[mFrameIndex];
                        auto& initialized
                            = view.mTarget ? view.mTarget->mWaterDepthInitialized : frame.mWaterDepthInitialized;
                        copyDepth(frame.mCommands, *source, *view.mWaterDepth, initialized);
                        // LOAD both color and depth. Clearing depth here would
                        // let later transparent draws show through the terrain.
                        resume(view.mTarget ? mTargetPasses[targetSlot].mResume : mMainResumePass);
                        savedWaterDepth = true;
                    }
                    std::size_t end = first + 1;
                    while (end < view.mFirst + view.mCount && mPrepared[end].mMesh == draw.mMesh
                        && mPrepared[end].mTexture == draw.mTexture && mPrepared[end].mPipeline == draw.mPipeline
                        && mPrepared[end].mRasterKey == draw.mRasterKey && mPrepared[end].mMask == draw.mMask
                        && mPrepared[end].mIndexCount == draw.mIndexCount && mPrepared[end].mFog == draw.mFog
                        && mPrepared[end].mDepthBias == draw.mDepthBias
                        && mPrepared[end].mDataDescriptor == draw.mDataDescriptor
                        && mPrepared[end].mPbrDescriptor == draw.mPbrDescriptor && !mPrepared[end].mClearDepthBefore)
                        ++end;
                    if (boundPipeline != mPipelines[draw.mPipeline])
                    {
                        boundPipeline = mPipelines[draw.mPipeline];
                        vkCmdBindPipeline(frame.mCommands, VK_PIPELINE_BIND_POINT_GRAPHICS, boundPipeline);
                    }
                    if (mDynamicRaster && boundRaster != draw.mRasterKey)
                    {
                        boundRaster = draw.mRasterKey;
                        mSetCullMode(
                            frame.mCommands, (boundRaster / 2) % 2 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
                        mSetFrontFace(frame.mCommands,
                            Render::counterClockwiseFrontFace(!view.mTarget, boundRaster % 2 != 0)
                                ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                : VK_FRONT_FACE_CLOCKWISE);
                        const auto depthMode = (boundRaster / DepthStride) % 3;
                        const bool hasDepth = !view.mTarget || view.mTarget->mDepth;
                        mSetDepthTest(frame.mCommands, hasDepth && depthMode != 0);
                        mSetDepthWrite(frame.mCommands, hasDepth && depthMode == 2);
                        constexpr std::array compare{ VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS,
                            VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_NOT_EQUAL,
                            VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_GREATER };
                        mSetDepthCompare(frame.mCommands, compare[boundRaster / CompareStride]);
                    }
                    if (!boundDepthBias || *boundDepthBias != draw.mDepthBias)
                    {
                        vkCmdSetDepthBias(frame.mCommands, draw.mDepthBias[0], 0, draw.mDepthBias[1]);
                        boundDepthBias = draw.mDepthBias;
                    }
                    if (boundData != draw.mDataDescriptor)
                    {
                        boundData = draw.mDataDescriptor;
                        vkCmdBindDescriptorSets(frame.mCommands, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 2, 1,
                            &boundData, 0, nullptr);
                    }
                    if (!boundFog || *boundFog != draw.mFog)
                    {
                        vkCmdPushConstants(frame.mCommands, mPipelineLayout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, draw.mFog.data());
                        boundFog = draw.mFog;
                    }
                    if (boundTexture != draw.mTexture->mDescriptor || boundMask != draw.mMask->mDescriptor)
                    {
                        boundTexture = draw.mTexture->mDescriptor;
                        boundMask = draw.mMask->mDescriptor;
                        const std::array descriptors{ boundTexture, boundMask };
                        vkCmdBindDescriptorSets(frame.mCommands, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 0, 2,
                            descriptors.data(), 0, nullptr);
                    }
                    const auto extra = draw.mPbrDescriptor ? draw.mPbrDescriptor
                        : savedWaterDepth                  ? view.mWaterDepth->mDescriptor
                                                           : emptyDepth;
                    if (boundExtra != extra)
                    {
                        boundExtra = extra;
                        vkCmdBindDescriptorSets(frame.mCommands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            draw.mPbrDescriptor ? mPbrPipelineLayout : mPipelineLayout, 3, 1, &boundExtra, 0, nullptr);
                    }
                    const auto& vertices = draw.mMesh->mVertices.current()->mRange;
                    const auto vertexNumber = vertices.offset() / sizeof(Render::Vertex);
                    const bool vertexFits = vertexNumber <= std::numeric_limits<std::int32_t>::max();
                    const VkDeviceSize vertexBinding = vertexFits ? 0 : vertices.offset();
                    const auto baseVertex = vertexFits ? static_cast<std::int32_t>(vertexNumber) : 0;
                    if (boundVertices != vertices.page().mBuffer || boundVertexOffset != vertexBinding)
                    {
                        boundVertices = vertices.page().mBuffer;
                        boundVertexOffset = vertexBinding;
                        vkCmdBindVertexBuffers(frame.mCommands, 0, 1, &boundVertices, &boundVertexOffset);
                    }
                    const auto& indices = draw.mMesh->mIndices.current()->mRange;
                    const auto indexNumber
                        = indices.offset() / (draw.mMesh->mIndexType == VK_INDEX_TYPE_UINT16 ? 2 : 4);
                    const bool indexFits = indexNumber <= std::numeric_limits<std::uint32_t>::max();
                    const VkDeviceSize indexBinding = indexFits ? 0 : indices.offset();
                    const auto firstIndex = indexFits ? static_cast<std::uint32_t>(indexNumber) : 0;
                    if (boundIndices != indices.page().mBuffer || boundIndexOffset != indexBinding
                        || boundIndexType != draw.mMesh->mIndexType)
                    {
                        boundIndices = indices.page().mBuffer;
                        boundIndexOffset = indexBinding;
                        boundIndexType = draw.mMesh->mIndexType;
                        vkCmdBindIndexBuffer(frame.mCommands, boundIndices, boundIndexOffset, boundIndexType);
                    }
                    vkCmdDrawIndexed(frame.mCommands, draw.mIndexCount, static_cast<std::uint32_t>(end - first),
                        firstIndex, baseVertex, static_cast<std::uint32_t>(first));
                    first = end;
                }
                if (view.mEffect)
                {
                    const auto& fullscreen = *mMeshes.get(mFullscreenMesh);
                    const VkDeviceSize offset = fullscreen.mVertices.current()->mRange.offset();
                    vkCmdBindPipeline(frame.mCommands, VK_PIPELINE_BIND_POINT_GRAPHICS, view.mEffectPipeline);
                    vkCmdBindDescriptorSets(frame.mCommands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        view.mEffect->mObjects->mLayout, 0, 1, &view.mEffectDescriptor, 0, nullptr);
                    vkCmdBindVertexBuffers(
                        frame.mCommands, 0, 1, &fullscreen.mVertices.current()->mRange.page().mBuffer, &offset);
                    vkCmdBindIndexBuffer(frame.mCommands, fullscreen.mIndices.current()->mRange.page().mBuffer,
                        fullscreen.mIndices.current()->mRange.offset(), fullscreen.mIndexType);
                    vkCmdDrawIndexed(frame.mCommands, 3, 1, 0, 0, 0);
                }
                vkCmdEndRenderPass(frame.mCommands);
                if (view.mPreserveWorldDepth && view.mTarget && view.mTarget->mWorldDepth && !savedWorldDepth)
                    copyWorldDepth(frame.mCommands, *view.mTarget);
                if (view.mTarget)
                    for (const auto& attachment : { view.mTarget, view.mTarget->mNormals })
                        if (attachment)
                        {
                            attachment->mWriter = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                            attachment->mVisible = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                        }
                if (view.mGenerateMipmaps)
                    view.mTarget->mMipsDirty = true;
                if (view.mTarget && view.mTarget->mDepth)
                {
                    view.mTarget->mDepth->mWriter
                        = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                    view.mTarget->mDepth->mReaders = 0;
                }
                if (view.mTarget && view.mTarget->mDepth && !discardDepth)
                {
                    view.mTarget->mDepth->mDepthReadOnly = true;
                    auto barrier = vkInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
                    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                    view.mTarget->mDepth->mWriter
                        = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                    view.mTarget->mDepth->mVisible = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                    barrier.image = view.mTarget->mDepth->mImage;
                    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                    if (mDepthFormat == VK_FORMAT_D24_UNORM_S8_UINT)
                        barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
                    vkCmdPipelineBarrier(frame.mCommands,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                }
                endTiming(frame, timing);
            }
            if (capture.atEnd())
                recordCapture(frame.mCommands, imageIndex);
            check(vkEndCommandBuffer(frame.mCommands), "Finish Vulkan frame");
            constexpr VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            auto submit = vkInfo<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &frame.mAcquired;
            submit.pWaitDstStageMask = &waitStage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &frame.mCommands;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &mPresented[imageIndex];
            check(vkResetFences(mDevice, 1, &frame.mFence), "Reset frame fence");
            const auto submitted = vkQueueSubmit(mGraphicsQueue, 1, &submit, frame.mFence);
            if (submitted != VK_SUCCESS)
            {
                mFailed = true;
                check(submitted, "Submit Vulkan frame");
            }
            mImageFences[imageIndex] = frame.mFence;
            mSwapchainInitialized[imageIndex] = windowWritten;
            frame.mAcquiredImage.reset();
            if (mCaptureRequested)
            {
                mCaptureFrame = mFrameIndex;
                mCaptureExtent = mExtent;
                mCaptureFormat = mColorFormat;
                mCaptureRequested = false;
            }
            auto present = vkInfo<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &mPresented[imageIndex];
            present.swapchainCount = 1;
            present.pSwapchains = &mSwapchain;
            present.pImageIndices = &imageIndex;
            const auto presented = vkQueuePresentKHR(mPresentQueue, &present);
            mFrameIndex = (mFrameIndex + 1) % FramesInFlight;
            if (presented == VK_ERROR_OUT_OF_DATE_KHR)
                mResize = true;
            if (presented == VK_SUBOPTIMAL_KHR || acquired == VK_SUBOPTIMAL_KHR)
                mSurfaceRefresh = true;
            if (presented != VK_ERROR_OUT_OF_DATE_KHR && presented != VK_SUBOPTIMAL_KHR)
                check(presented, "Present Vulkan frame");
            return true;
        }
    }

    std::unique_ptr<Render::Backend> createBackend(const Render::BackendOptions& options)
    {
        auto backend = std::make_unique<VulkanBackend>(options);
        backend->initialize();
        return backend;
    }
}
