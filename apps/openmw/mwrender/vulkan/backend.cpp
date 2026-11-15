#include "backend.hpp"

#include "check.hpp"
#include "memory.hpp"

#include <components/render/bufferarena.hpp>
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
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MWRender::Vulkan
{
    namespace
    {
        constexpr std::size_t FramesInFlight = 2;

        VkFormat textureFormat(Render::TextureFormat format, bool srgb)
        {
            switch (format)
            {
                case Render::TextureFormat::RGBA8:
                    return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
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
        constexpr std::size_t TextureFormats = static_cast<std::size_t>(Render::TextureFormat::BC5) + 1;
        constexpr std::array TargetVkFormats{ VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_R8_UNORM, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
            VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT };
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
            std::uint64_t mLastUploadSubmission = 0;
            explicit Buffer(VkDevice device)
                : mDevice(device)
            {
            }
            ~Buffer() { vkDestroyBuffer(mDevice, mBuffer, nullptr); }
        };

        struct Mesh
        {
            Render::BufferArena<Buffer>::Slice mVertices;
            Render::BufferArena<Buffer>::Slice mIndices;
            std::uint32_t mIndexCount = 0;
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
            std::shared_ptr<DescriptorPage> mPool;
            VkDescriptorSet mDescriptor = VK_NULL_HANDLE;
            Allocation mMemory;
            std::uint32_t mWidth = 0;
            std::uint32_t mHeight = 0;
            std::uint32_t mMipLevels = 1;
            VkSampler mSampler = VK_NULL_HANDLE;
            bool mTarget = false;
            Render::TargetFormat mTargetFormat = Render::TargetFormat::RGBA8;
            VkFramebuffer mFramebuffer = VK_NULL_HANDLE;
            std::shared_ptr<Image> mDepth;
            std::shared_ptr<Image> mNormals;
            std::shared_ptr<Image> mWorldDepth;
            bool mWorldDepthInitialized = false;
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
                vkDestroyImageView(mDevice, mView, nullptr);
                vkDestroyImage(mDevice, mImage, nullptr);
            }
        };

        struct Effect
        {
            VkDevice mDevice;
            VkDescriptorSetLayout mDescriptorLayout = VK_NULL_HANDLE;
            VkPipelineLayout mLayout = VK_NULL_HANDLE;
            std::array<VkShaderModule, 2> mModules{};
            std::array<VkPipeline, TargetFormats> mPipelines{};
            std::vector<Render::EffectBinding> mBindings;
            explicit Effect(VkDevice device)
                : mDevice(device)
            {
            }
            ~Effect()
            {
                for (auto pipeline : mPipelines)
                    vkDestroyPipeline(mDevice, pipeline, nullptr);
                for (auto module : mModules)
                    vkDestroyShaderModule(mDevice, module, nullptr);
                vkDestroyPipelineLayout(mDevice, mLayout, nullptr);
                vkDestroyDescriptorSetLayout(mDevice, mDescriptorLayout, nullptr);
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
            bool mDataDescriptorDirty = true;
            std::vector<VkDescriptorPool> mEffectPools;
            std::size_t mEffectPoolIndex = 0;
            std::optional<std::uint32_t> mAcquiredImage;
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
            Count
        };
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
            bool mPreserveWorldDepth = false;
            bool mOverwrite = false;
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
            void requestCapture() override
            {
                if (mCaptureFrame)
                    takeCapture();
                mCaptureRequested = true;
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
            bool supportsTextureFormat(Render::TextureFormat format, bool srgb) const override;
            Render::TextureHandle createTexture(
                const Render::TextureData& data, const Render::SamplerState& sampler = {}) override;
            Render::TextureHandle createRenderTarget(std::uint32_t width, std::uint32_t height,
                const Render::SamplerState& sampler, Render::TargetFormat format) override;
            Render::TextureHandle createVolume(std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                std::span<const std::byte> rgba, const Render::SamplerState& sampling) override;
            Render::TextureHandle depthTexture(Render::TextureHandle target) override;
            Render::TextureHandle normalTexture(Render::TextureHandle target) override;
            Render::TextureHandle worldDepthTexture(Render::TextureHandle target) override;
            Render::EffectHandle createEffect(const Render::EffectProgram& program) override;
            void releaseEffect(Render::EffectHandle handle) override { mEffects.erase(handle); }
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
                bool depth = true, bool overwrite = false);
            void copyWorldDepth(VkCommandBuffer commands, Image& target);
            void createPipeline(std::size_t index);
            void warmPipelines(std::size_t target);
            void loadPipelineCache();
            void savePipelineCache() noexcept;
            VkPipeline effectPipeline(Effect& effect, Render::TargetFormat format);
            VkDescriptorSet effectDescriptor(Frame& frame, Effect& effect, std::span<const Render::EffectInput> inputs);
            void recordUploads(Frame& frame, VkDeviceSize instanceBytes, VkDeviceSize lightBytes);
            void cancelUpload(std::optional<std::size_t>& index);
            bool renderFrame(std::span<const Render::Pass> passes);
            std::unique_ptr<Buffer> createBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, MemoryKind kind);
            std::shared_ptr<Image> createImage(std::uint32_t width, std::uint32_t height, VkFormat format,
                VkImageUsageFlags usage, VkImageAspectFlags aspect, std::uint32_t mipLevels = 1,
                std::uint32_t depth = 1);
            Staging allocateStaging(VkDeviceSize size, VkDeviceSize alignment = 16);
            Staging stage(std::span<const std::byte> bytes);
            void allocateDescriptor(Image& image);
            VkSampler sampler(Render::SamplerState state);

            Render::BackendOptions mOptions;
            Render::DeviceInfo mInfo;
            VkInstance mInstance = VK_NULL_HANDLE;
            VkSurfaceKHR mSurface = VK_NULL_HANDLE;
            VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
            VkPhysicalDeviceProperties mProperties{};
            VkPhysicalDeviceFeatures mFeatures{};
            std::array<std::array<VkFormatProperties, 2>, TextureFormats> mTextureProperties{};
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
            VkDescriptorSetLayout mDataLayout = VK_NULL_HANDLE;
            VkDescriptorPool mDataPool = VK_NULL_HANDLE;
            std::vector<Render::PointLight> mLightData;
            struct LightOffset
            {
                const Render::LightSet* mSet = nullptr;
                std::uint32_t mOffset = 0;
            };
            std::vector<LightOffset> mLightOffsets;
            std::vector<std::uint32_t> mDrawLightOffsets;
            VkPipelineCache mPipelineCache = VK_NULL_HANDLE;
            std::array<VkShaderModule, 2> mShaderModules{};
            std::vector<std::unique_ptr<Sampler>> mSamplers;
            std::vector<std::shared_ptr<DescriptorPage>> mDescriptorPools;
            std::array<Frame, FramesInFlight> mFrames;
            std::size_t mFrameIndex = 0;
            std::uint64_t mSubmission = 0;
            VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;
            VkFormat mColorFormat = VK_FORMAT_UNDEFINED;
            VkFormat mDepthFormat = VK_FORMAT_UNDEFINED;
            VkExtent2D mExtent{};
            VkExtent2D mDrawableExtent{};
            VkRenderPass mRenderPass = VK_NULL_HANDLE;
            VkRenderPass mMainLoadPass = VK_NULL_HANDLE;
            std::array<TargetPasses, TargetFormats> mTargetPasses{};
            std::array<VkPipeline, PipelinesPerTarget*(TargetFormats + 1)> mPipelines{};
            std::vector<const Render::Draw*> mFrameDraws;
            std::vector<PreparedPass> mPreparedPasses;
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
            Render::UploadPages<Buffer> mStagingPages{ MemoryAllocator::blockSize(MemoryKind::Upload) };
            Render::BufferArena<Buffer> mGeometryPages{ MemoryAllocator::blockSize(MemoryKind::Geometry) };
            std::vector<PreparedDraw> mPrepared;
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
                for (auto& frame : mFrames)
                {
                    frame.mResources.clear();
                    frame.mUploads.clear();
                    frame.mInstances.reset();
                    frame.mInstanceUpload.reset();
                    frame.mLightUpload.reset();
                    frame.mLights.reset();
                    for (auto pool : frame.mEffectPools)
                        vkDestroyDescriptorPool(mDevice, pool, nullptr);
                    vkDestroySemaphore(mDevice, frame.mAcquired, nullptr);
                    vkDestroyFence(mDevice, frame.mFence, nullptr);
                    vkDestroyCommandPool(mDevice, frame.mPool, nullptr);
                }
                mPendingUploads.clear();
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
                }
                mDescriptorPools.clear();
                mSamplers.clear();
                for (const auto shader : mShaderModules)
                    vkDestroyShaderModule(mDevice, shader, nullptr);
                vkDestroyPipelineCache(mDevice, mPipelineCache, nullptr);
                vkDestroyPipelineLayout(mDevice, mPipelineLayout, nullptr);
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
                    if (format != VK_FORMAT_UNDEFINED)
                        vkGetPhysicalDeviceFormatProperties(mPhysicalDevice, format, &mTextureProperties[i][srgb]);
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
            for (std::size_t i = 0; i < TargetFormats; ++i)
            {
                auto& properties = mTargetProperties[i];
                vkGetPhysicalDeviceFormatProperties(mPhysicalDevice, TargetVkFormats[i], &properties);
                constexpr auto required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
                    | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
                if ((properties.optimalTilingFeatures & required) != required)
                    continue;
                if (hasNormals(static_cast<Render::TargetFormat>(i)))
                {
                    VkFormatProperties normalProperties{};
                    vkGetPhysicalDeviceFormatProperties(
                        mPhysicalDevice, VK_FORMAT_R16G16B16A16_SFLOAT, &normalProperties);
                    constexpr auto normalRequired = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
                        | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
                        | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
                    if ((normalProperties.optimalTilingFeatures & normalRequired) != normalRequired)
                        continue;
                }
                const auto targetFormat = static_cast<Render::TargetFormat>(i);
                mTargetPasses[i] = { createRenderPass(TargetVkFormats[i], true, true, hasNormals(targetFormat),
                                         Render::targetHasDepth(targetFormat)),
                    createRenderPass(TargetVkFormats[i], true, false, hasNormals(targetFormat),
                        Render::targetHasDepth(targetFormat)),
                    Render::targetHasDepth(targetFormat)
                        ? VK_NULL_HANDLE
                        : createRenderPass(TargetVkFormats[i], true, false, false, false, true) };
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
            std::array<VkDescriptorSetLayoutBinding, 2> dataBindings{};
            for (unsigned i = 0; i < dataBindings.size(); ++i)
            {
                dataBindings[i].binding = i;
                dataBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                dataBindings[i].descriptorCount = 1;
                dataBindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            }
            descriptor.bindingCount = dataBindings.size();
            descriptor.pBindings = dataBindings.data();
            check(vkCreateDescriptorSetLayout(mDevice, &descriptor, nullptr, &mDataLayout), "Create frame data layout");
            const VkDescriptorPoolSize dataSize{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * FramesInFlight };
            auto pool = vkInfo<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
            pool.maxSets = FramesInFlight;
            pool.poolSizeCount = 1;
            pool.pPoolSizes = &dataSize;
            check(vkCreateDescriptorPool(mDevice, &pool, nullptr, &mDataPool), "Create frame data descriptor pool");
            const std::array materialLayouts{ mDescriptorLayout, mDescriptorLayout, mDataLayout };
            layout.setLayoutCount = 3;
            layout.pSetLayouts = materialLayouts.data();
            const VkPushConstantRange fogConstants{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
            layout.pushConstantRangeCount = 1;
            layout.pPushConstantRanges = &fogConstants;
            check(vkCreatePipelineLayout(mDevice, &layout, nullptr, &mPipelineLayout), "Create Vulkan pipeline layout");
            loadPipelineCache();
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
            VkImageUsageFlags usage, VkImageAspectFlags aspect, std::uint32_t mipLevels, std::uint32_t depth)
        {
            auto image = std::make_shared<Image>(mDevice);
            image->mWidth = width;
            image->mHeight = height;
            image->mMipLevels = mipLevels;
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
                            && requirements.memoryRequirements.size >= MemoryAllocator::blockSize(MemoryKind::Image))
                    ? &allocation
                    : nullptr);
            check(vkBindImageMemory(mDevice, image->mImage, image->mMemory.memory(), image->mMemory.offset()),
                "Bind Vulkan image memory");
            auto view = vkInfo<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
            view.image = image->mImage;
            view.viewType = depth == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_3D;
            view.format = format;
            view.subresourceRange = { aspect, 0, mipLevels, 0, 1 };
            check(vkCreateImageView(mDevice, &view, nullptr, &image->mView), "Create Vulkan image view");
            return image;
        }

        Staging VulkanBackend::allocateStaging(VkDeviceSize size, VkDeviceSize alignment)
        {
            // Four-byte buffer copies and all supported compressed texture
            // blocks are aligned by a shared sixteen-byte slice boundary.
            return mStagingPages.allocate(size, alignment, [&](VkDeviceSize capacity) -> std::shared_ptr<Buffer> {
                return createBuffer(capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    MemoryKind::Upload);
            });
        }

        Staging VulkanBackend::stage(std::span<const std::byte> bytes)
        {
            auto staging = allocateStaging(bytes.size());
            std::memcpy(staging.mPage->mMemory.mapped() + staging.mOffset, bytes.data(), bytes.size());
            return staging;
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
            mesh->mVertexBytes = vertices.size_bytes();
            mesh->mIndexBytes = indices.size_bytes();
            const auto createPage = [&](VkDeviceSize capacity) -> std::shared_ptr<Buffer> {
                return createBuffer(capacity,
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                        | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    MemoryKind::Geometry);
            };
            mesh->mVertices = mGeometryPages.allocate(vertices.size_bytes(), 16, createPage);
            mesh->mIndices = mGeometryPages.allocate(indices.size_bytes(), 4, createPage);
            auto vertexStaging = stage(std::as_bytes(vertices));
            auto indexStaging = stage(std::as_bytes(indices));
            Render::reserveFor(mPendingUploads, mPendingUploads.size() + 2);
            const auto handle = mMeshes.insert(mesh);
            mesh->mVertexUpload = mPendingUploads.size();
            mPendingUploads.push_back({ std::move(vertexStaging), mesh, {}, vertices.size_bytes(), false, {}, false });
            mesh->mIndexUpload = mPendingUploads.size();
            mPendingUploads.push_back({ std::move(indexStaging), mesh, {}, indices.size_bytes(), true, {}, false });
            return handle;
        }

        void VulkanBackend::updateMesh(
            Render::MeshHandle handle, std::span<const Render::Vertex> vertices, std::span<const std::uint32_t> indices)
        {
            const auto& mesh = mMeshes.get(handle);
            if (vertices.empty() || vertices.size_bytes() > mesh->mVertexBytes
                || (!indices.empty() && vertices.size_bytes() != mesh->mVertexBytes)
                || (!indices.empty() && indices.size_bytes() != mesh->mIndexBytes)
                || std::any_of(indices.begin(), indices.end(), [&](auto index) { return index >= vertices.size(); }))
                throw std::invalid_argument("Resident mesh updates require valid dimensions and indices");
            Render::reserveFor(mPendingUploads, mPendingUploads.size() + 2);
            // Coalesce repeated CPU updates before the next submission.
            for (bool index : { false, true })
            {
                if (index && indices.empty())
                    continue;
                const auto bytes = index ? std::as_bytes(indices) : std::as_bytes(vertices);
                auto& pending = index ? mesh->mIndexUpload : mesh->mVertexUpload;
                if (pending)
                {
                    auto& upload = mPendingUploads[*pending];
                    if (bytes.size() <= upload.mSize)
                    {
                        // This slice has not been recorded/submitted. Reuse
                        // its host bytes, preserving a longer pending tail
                        // when a newer update replaces only the live prefix.
                        std::memcpy(upload.mStaging.mPage->mMemory.mapped() + upload.mStaging.mOffset, bytes.data(),
                            bytes.size());
                    }
                    else
                    {
                        upload.mStaging = stage(bytes);
                        upload.mSize = bytes.size();
                    }
                }
                else
                {
                    auto staging = stage(bytes);
                    pending = mPendingUploads.size();
                    mPendingUploads.push_back({ std::move(staging), mesh, {}, bytes.size(), index, {}, false });
                }
            }
        }

        void VulkanBackend::allocateDescriptor(Image& image)
        {
            auto info = vkInfo<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
            info.descriptorSetCount = 1;
            info.pSetLayouts = &mDescriptorLayout;
            VkResult result = VK_ERROR_OUT_OF_POOL_MEMORY;
            VkDescriptorSet descriptor = VK_NULL_HANDLE;
            std::shared_ptr<DescriptorPage> selected;
            for (const auto& page : mDescriptorPools)
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
                VkDescriptorPoolSize size{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 };
                auto pool = vkInfo<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
                pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
                pool.maxSets = size.descriptorCount;
                pool.poolSizeCount = 1;
                pool.pPoolSizes = &size;
                selected = std::make_shared<DescriptorPage>(mDevice);
                check(vkCreateDescriptorPool(mDevice, &pool, nullptr, &selected->mPool),
                    "Create material descriptor pool");
                // Register the page before allocating a set so exceptions do
                // not strand an allocated set outside its page owner.
                mDescriptorPools.push_back(selected);
                info.descriptorPool = selected->mPool;
                result = vkAllocateDescriptorSets(mDevice, &info, &descriptor);
            }
            check(result, "Allocate texture descriptor");
            image.mPool = std::move(selected);
            image.mDescriptor = descriptor;
            ++image.mPool->mLive;
            VkDescriptorImageInfo binding{ image.mSampler, image.mView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            auto write = vkInfo<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            write.dstSet = image.mDescriptor;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &binding;
            vkUpdateDescriptorSets(mDevice, 1, &write, 0, nullptr);
        }

        bool VulkanBackend::supportsTextureFormat(Render::TextureFormat format, bool srgb) const
        {
            if (format != Render::TextureFormat::RGBA8 && !mFeatures.textureCompressionBC)
                return false;
            const auto slot = static_cast<std::size_t>(format);
            if (slot >= TextureFormats)
                return false;
            const auto& properties = mTextureProperties[slot][srgb];
            constexpr auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            return (properties.optimalTilingFeatures & required) == required;
        }

        Render::TextureHandle VulkanBackend::createTexture(
            const Render::TextureData& data, const Render::SamplerState& state)
        {
            Render::validateTexture(data);
            const auto width = data.mLevels.front().mWidth;
            const auto height = data.mLevels.front().mHeight;
            if (width > mProperties.limits.maxImageDimension2D || height > mProperties.limits.maxImageDimension2D)
                throw std::invalid_argument("Texture dimensions exceed the Vulkan device limit");
            if (!supportsTextureFormat(data.mFormat, data.mSrgb))
                throw std::runtime_error("Texture format is not supported by the Vulkan device");
            const VkFormat format = textureFormat(data.mFormat, data.mSrgb);
            const auto& properties = mTextureProperties[static_cast<std::size_t>(data.mFormat)][data.mSrgb];
            const bool generate = data.mGenerateMipmaps && data.mLevels.size() == 1;
            const auto levels = generate ? static_cast<std::uint32_t>(std::bit_width(std::max(width, height)))
                                         : static_cast<std::uint32_t>(data.mLevels.size());
            VkFormatFeatureFlags required = 0;
            if (state.mMinFilter == Render::TextureFilter::Linear || state.mMagFilter == Render::TextureFilter::Linear
                || generate)
                required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            if (generate && levels > 1)
                required |= VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT
                    | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
            if ((properties.optimalTilingFeatures & required) != required)
                throw std::runtime_error(
                    "Vulkan texture format does not support the requested filtering or mip generation");
            auto image = createImage(width, height, format,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                    | (generate && levels > 1 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0),
                VK_IMAGE_ASPECT_COLOR_BIT, levels);
            image->mSampler = sampler(state);
            allocateDescriptor(*image);
            std::size_t bytes = 0;
            for (const auto& level : data.mLevels)
            {
                if (level.mBytes.size() > std::numeric_limits<std::size_t>::max() - bytes)
                    throw std::length_error("Texture upload size overflows the host address space");
                bytes += level.mBytes.size();
            }
            Upload upload;
            upload.mStaging = allocateStaging(bytes);
            upload.mImage = image;
            upload.mGenerateMipmaps = generate;
            upload.mImageCopies.reserve(data.mLevels.size());
            VkDeviceSize offset = 0;
            for (std::size_t i = 0; i < data.mLevels.size(); ++i)
            {
                const auto& level = data.mLevels[i];
                std::memcpy(upload.mStaging.mPage->mMemory.mapped() + upload.mStaging.mOffset + offset,
                    level.mBytes.data(), level.mBytes.size());
                VkBufferImageCopy copy{};
                copy.bufferOffset = upload.mStaging.mOffset + offset;
                copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, static_cast<std::uint32_t>(i), 0, 1 };
                copy.imageExtent = { level.mWidth, level.mHeight, 1 };
                upload.mImageCopies.push_back(copy);
                offset += level.mBytes.size();
            }
            Render::reserveFor(mPendingUploads, mPendingUploads.size() + 1);
            const auto handle = mTextures.insert(image);
            image->mUpload = mPendingUploads.size();
            mPendingUploads.push_back(std::move(upload));
            return handle;
        }

        Render::TextureHandle VulkanBackend::createRenderTarget(std::uint32_t width, std::uint32_t height,
            const Render::SamplerState& requestedSampling, Render::TargetFormat format)
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
            auto image = createImage(width, height, TargetVkFormats[slot],
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT);
            image->mTarget = true;
            image->mTargetFormat = format;
            sampling.mUseMipmaps = false;
            image->mSampler = sampler(sampling);
            allocateDescriptor(*image);
            if (Render::targetHasDepth(format))
                image->mDepth = createImage(width, height, mDepthFormat,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT);
            if (hasNormals(format))
            {
                image->mNormals = createImage(width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
                image->mNormals->mSampler = image->mSampler;
                allocateDescriptor(*image->mNormals);
                image->mNormals->mUpload = mPendingUploads.size();
                mPendingUploads.push_back({ {}, {}, image->mNormals, 0, false, {}, false });
            }
            const std::array attachments{ image->mView, image->mDepth ? image->mDepth->mView : VK_NULL_HANDLE,
                image->mNormals ? image->mNormals->mView : VK_NULL_HANDLE };
            auto info = vkInfo<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
            info.renderPass = mTargetPasses[slot].mClear;
            info.attachmentCount = image->mNormals ? 3 : image->mDepth ? 2 : 1;
            info.pAttachments = attachments.data();
            info.width = width;
            info.height = height;
            info.layers = 1;
            check(vkCreateFramebuffer(mDevice, &info, nullptr, &image->mFramebuffer), "Create Vulkan render target");
            if (Render::targetHasDepth(format) || format == Render::TargetFormat::ColorRGBA16F)
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
            {
                auto depth = createImage(image->mWidth, image->mHeight, mDepthFormat,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
                Render::SamplerState sampling;
                sampling.mWrapS = sampling.mWrapT = Render::TextureWrap::Clamp;
                sampling.mMinFilter = sampling.mMagFilter = Render::TextureFilter::Nearest;
                sampling.mUseMipmaps = false;
                depth->mSampler = sampler(sampling);
                allocateDescriptor(*depth);
                image->mWorldDepth = std::move(depth);
            }
            return mTextures.insert(image->mWorldDepth);
        }
        void VulkanBackend::copyWorldDepth(VkCommandBuffer commands, Image& target)
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
            barriers[0].image = target.mDepth->mImage;
            barriers[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barriers[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barriers[1].image = target.mWorldDepth->mImage;
            barriers[1].oldLayout
                = target.mWorldDepthInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barriers[1].srcAccessMask = target.mWorldDepthInitialized ? VK_ACCESS_SHADER_READ_BIT : 0;
            barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(commands,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                    | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers.data());
            VkImageCopy copy{};
            copy.srcSubresource = copy.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
            copy.extent = { target.mWidth, target.mHeight, 1 };
            vkCmdCopyImage(commands, target.mDepth->mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                target.mWorldDepth->mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
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
            target.mWorldDepthInitialized = true;
        }

        Render::EffectHandle VulkanBackend::createEffect(const Render::EffectProgram& program)
        {
            auto effect = std::make_shared<Effect>(mDevice);
            effect->mBindings.assign(program.mBindings.begin(), program.mBindings.end());
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            std::uint32_t textures = 0, buffers = 0;
            for (const auto& binding : effect->mBindings)
            {
                if (std::any_of(
                        bindings.begin(), bindings.end(), [&](const auto& b) { return b.binding == binding.mBinding; }))
                    throw std::invalid_argument("Duplicate effect descriptor binding");
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
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });
            }
            const auto& limits = mProperties.limits;
            if (textures > std::min({ limits.maxPerStageDescriptorSamplers, limits.maxPerStageDescriptorSampledImages,
                    limits.maxDescriptorSetSamplers, limits.maxDescriptorSetSampledImages })
                || buffers > std::min(limits.maxPerStageDescriptorUniformBuffers, limits.maxDescriptorSetUniformBuffers)
                || textures + buffers > limits.maxPerStageResources)
                throw std::invalid_argument("Effect descriptor count exceeds device limits");
            auto descriptor
                = vkInfo<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
            descriptor.bindingCount = static_cast<std::uint32_t>(bindings.size());
            descriptor.pBindings = bindings.data();
            check(vkCreateDescriptorSetLayout(mDevice, &descriptor, nullptr, &effect->mDescriptorLayout),
                "Create effect descriptors");
            auto layout = vkInfo<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
            layout.setLayoutCount = 1;
            layout.pSetLayouts = &effect->mDescriptorLayout;
            check(vkCreatePipelineLayout(mDevice, &layout, nullptr, &effect->mLayout), "Create effect layout");
            const std::array code{ program.mVertex, program.mFragment };
            for (std::size_t i = 0; i < code.size(); ++i)
            {
                if (code[i].size() < 5 || code[i][0] != 0x07230203)
                    throw std::invalid_argument("Invalid effect SPIR-V");
                auto module = vkInfo<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
                module.codeSize = code[i].size_bytes();
                module.pCode = code[i].data();
                check(vkCreateShaderModule(mDevice, &module, nullptr, &effect->mModules[i]), "Create effect shader");
            }
            return mEffects.insert(std::move(effect));
        }

        VkPipeline VulkanBackend::effectPipeline(Effect& effect, Render::TargetFormat format)
        {
            const auto slot = static_cast<std::size_t>(format);
            if (slot >= TargetFormats || !mTargetPasses[slot].mClear)
                throw std::invalid_argument("Unsupported effect output format");
            if (effect.mPipelines[slot])
                return effect.mPipelines[slot];
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
            auto blend
                = vkInfo<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
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
            info.renderPass = mTargetPasses[slot].mClear;
            check(vkCreateGraphicsPipelines(mDevice, mPipelineCache, 1, &info, nullptr, &effect.mPipelines[slot]),
                "Create native effect pipeline");
            return effect.mPipelines[slot];
        }

        VkDescriptorSet VulkanBackend::effectDescriptor(
            Frame& frame, Effect& effect, std::span<const Render::EffectInput> inputs)
        {
            VkDescriptorSet descriptor = VK_NULL_HANDLE;
            for (;;)
            {
                if (frame.mEffectPoolIndex == frame.mEffectPools.size())
                {
                    const std::array sizes{ VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 },
                        VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256 } };
                    auto pool = vkInfo<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
                    pool.maxSets = 64;
                    pool.poolSizeCount = sizes.size();
                    pool.pPoolSizes = sizes.data();
                    VkDescriptorPool value = VK_NULL_HANDLE;
                    check(vkCreateDescriptorPool(mDevice, &pool, nullptr, &value), "Create effect descriptor page");
                    frame.mEffectPools.push_back(value);
                }
                auto info = vkInfo<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
                info.descriptorPool = frame.mEffectPools[frame.mEffectPoolIndex];
                info.descriptorSetCount = 1;
                info.pSetLayouts = &effect.mDescriptorLayout;
                const auto result = vkAllocateDescriptorSets(mDevice, &info, &descriptor);
                if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL)
                {
                    ++frame.mEffectPoolIndex;
                    continue;
                }
                check(result, "Allocate effect descriptor");
                break;
            }
            auto& writes = mEffectWrites;
            auto& buffers = mEffectBuffers;
            auto& images = mEffectImages;
            writes.clear();
            buffers.clear();
            images.clear();
            writes.reserve(effect.mBindings.size());
            buffers.reserve(effect.mBindings.size());
            images.reserve(effect.mBindings.size());
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
                    frame.mResources.push_back(image);
                    images.push_back({ image->mSampler, image->mView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
                    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    write.pImageInfo = &images.back();
                }
                else
                {
                    auto slice = allocateStaging(binding.mSize,
                        std::max(VkDeviceSize{ 16 }, mProperties.limits.minUniformBufferOffsetAlignment));
                    if (slice.mPage->mLastUploadSubmission != mSubmission)
                    {
                        frame.mUploads.push_back(slice.mPage);
                        slice.mPage->mLastUploadSubmission = mSubmission;
                    }
                    auto* destination = slice.mPage->mMemory.mapped() + slice.mOffset;
                    std::memset(destination, 0, binding.mSize);
                    if (input != inputs.end())
                    {
                        if (input->mData.size() > binding.mSize)
                            throw std::invalid_argument("Effect uniform data exceeds reflected block");
                        if (!input->mData.empty())
                            std::memcpy(destination, input->mData.data(), input->mData.size());
                    }
                    buffers.push_back({ slice.mPage->mBuffer, slice.mOffset, binding.mSize });
                    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                    write.pBufferInfo = &buffers.back();
                }
                writes.push_back(write);
            }
            vkUpdateDescriptorSets(mDevice, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
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
            for (auto& pipeline : std::span(mPipelines).first(PipelinesPerTarget))
            {
                vkDestroyPipeline(mDevice, pipeline, nullptr);
                pipeline = VK_NULL_HANDLE;
            }
            vkDestroyRenderPass(mDevice, mRenderPass, nullptr);
            vkDestroyRenderPass(mDevice, mMainLoadPass, nullptr);
            mRenderPass = VK_NULL_HANDLE;
            mMainLoadPass = VK_NULL_HANDLE;
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
                warmPipelines(0);
            }
            mDepthImages.reserve(FramesInFlight);
            for (std::size_t i = 0; i < FramesInFlight; ++i)
                mDepthImages.push_back(createImage(mExtent.width, mExtent.height, mDepthFormat,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT));
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

        VkRenderPass VulkanBackend::createRenderPass(
            VkFormat format, bool offscreen, bool clear, bool normals, bool hasDepth, bool overwrite)
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
            attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[1].storeOp = offscreen ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachments[2] = attachments[0];
            attachments[2].format = VK_FORMAT_R16G16B16A16_SFLOAT;
            const std::array colors{ VkAttachmentReference{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
                VkAttachmentReference{ 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } };
            VkAttachmentReference depth{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = normals ? 2 : 1;
            subpass.pColorAttachments = colors.data();
            subpass.pDepthStencilAttachment = hasDepth ? &depth : nullptr;
            VkSubpassDependency dependency{};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass = 0;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                | (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            dependency.dstStageMask
                = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            auto info = vkInfo<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
            info.attachmentCount = normals ? 3 : hasDepth ? 2 : 1;
            info.pAttachments = attachments.data();
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            info.dependencyCount = 1;
            info.pDependencies = &dependency;
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
            if (target && !Render::targetHasDepth(static_cast<Render::TargetFormat>(target - 1)))
            {
                warm(MaterialShader::Distortion, Render::BlendMode::Alpha);
                warm(MaterialShader::Distortion, Render::BlendMode::Additive);
                return;
            }
            for (const auto blend : { Render::BlendMode::Opaque, Render::BlendMode::Alpha, Render::BlendMode::Additive,
                     Render::BlendMode::WeightedReplace, Render::BlendMode::PremultipliedAlpha })
            {
                warm(MaterialShader::Surface, blend);
                warm(MaterialShader::SolidSurface, blend);
            }
            warm(MaterialShader::Water, Render::BlendMode::Alpha);
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
            const std::array<std::uint32_t, 2> constants{ material,
                targetGroup && hasNormals(static_cast<Render::TargetFormat>(targetGroup - 1)) ? VK_TRUE : VK_FALSE };
            const std::array<VkSpecializationMapEntry, 2> entries{ {
                { 0, 0, sizeof(std::uint32_t) },
                { 1, sizeof(std::uint32_t), sizeof(std::uint32_t) },
            } };
            const VkSpecializationInfo specialization{ static_cast<std::uint32_t>(entries.size()), entries.data(),
                sizeof(constants), constants.data() };
            auto& modules = mShaderModules;
            std::array<VkPipelineShaderStageCreateInfo, 2> shaders{};
            const std::array<const char*, 2> names{ "basic.vert.spv", "basic.frag.spv" };
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
                VK_DYNAMIC_STATE_CULL_MODE_EXT, VK_DYNAMIC_STATE_FRONT_FACE_EXT, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT,
                VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT };
            auto dynamic
                = vkInfo<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
            dynamic.dynamicStateCount = mDynamicRaster ? static_cast<std::uint32_t>(dynamicStates.size()) : 2;
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
            pipeline.layout = mPipelineLayout;
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

        void VulkanBackend::recordUploads(Frame& frame, VkDeviceSize instanceBytes, VkDeviceSize lightBytes)
        {
            bool copiedBuffers = instanceBytes != 0 || lightBytes != 0;
            // Mesh updates share the graphics queue. Order previous frame
            // vertex reads and earlier copies before overwriting resident data.
            auto before = vkInfo<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
            before.srcAccessMask
                = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(frame.mCommands, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
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
            Render::reserveFor(frame.mResources, frame.mResources.size() + mPendingUploads.size());
            for (auto& upload : mPendingUploads)
            {
                if (!upload.mStaging && !upload.mImage)
                    continue;
                if (upload.mMesh)
                {
                    (upload.mIndices ? upload.mMesh->mIndexUpload : upload.mMesh->mVertexUpload).reset();
                    const auto& destination = upload.mIndices ? upload.mMesh->mIndices : upload.mMesh->mVertices;
                    const VkBufferCopy copy{ upload.mStaging.mOffset, destination.offset(), upload.mSize };
                    vkCmdCopyBuffer(
                        frame.mCommands, upload.mStaging.mPage->mBuffer, destination.page().mBuffer, 1, &copy);
                    frame.mResources.push_back(upload.mMesh);
                    copiedBuffers = true;
                }
                else
                {
                    upload.mImage->mUpload.reset();
                    const auto& image = *upload.mImage;
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
                    VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &barrier, 0,
                    nullptr, 0, nullptr);
            }
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
            mPreparedPasses.clear();
            for (const auto& pass : passes)
            {
                std::shared_ptr<Image> target;
                if (pass.mTarget)
                {
                    target = mTextures.get(pass.mTarget);
                    if (!target->mTarget)
                        throw std::invalid_argument("Render pass texture is not a framebuffer target");
                }
                mPreparedPasses.push_back(
                    { mFrameDraws.size(), pass.mDraws.size(), target, pass.mClearColor, pass.mClear, pass.mViewport });
                mPreparedPasses.back().mPreserveWorldDepth = pass.mPreserveWorldDepth;
                mPreparedPasses.back().mOverwrite = pass.mOverwrite;
                if (pass.mOverwrite && (!pass.mEffect || !target || target->mDepth))
                    throw std::invalid_argument("Only color-only fullscreen effects can overwrite a target");
                Render::reserveFor(mFrameDraws, mFrameDraws.size() + pass.mDraws.size());
                for (const auto& draw : pass.mDraws)
                    mFrameDraws.push_back(&draw);
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
            for (auto& fence : mImageFences)
                if (fence == frame.mFence)
                    fence = VK_NULL_HANDLE;
            frame.mResources.clear();
            frame.mUploads.clear();
            mPrepared.clear();
            for (auto pool : frame.mEffectPools)
                check(vkResetDescriptorPool(mDevice, pool, 0), "Recycle effect descriptors");
            frame.mEffectPoolIndex = 0;
            if (mSubmission % 16 == 0)
            {
                mStagingPages.trim(32 * 1024 * 1024, 8);
                mGeometryPages.trim(32 * 1024 * 1024, 8);
                mAllocator->trim();
            }
            Render::reserveFor(mPrepared, draws.size());
            Render::reserveFor(frame.mResources, draws.size() * 3 + mPendingUploads.size());
            ++mSubmission;
            for (std::size_t i = 0; i < mPreparedPasses.size(); ++i)
            {
                auto& pass = mPreparedPasses[i];
                if (pass.mTarget)
                    frame.mResources.push_back(pass.mTarget);
                if (passes[i].mEffect)
                {
                    if (!pass.mTarget || pass.mCount)
                        throw std::invalid_argument("Effects require an offscreen color-only pass");
                    pass.mEffect = mEffects.get(passes[i].mEffect);
                    frame.mResources.push_back(pass.mEffect);
                    for (const auto& input : passes[i].mEffectInputs)
                        if (input.mTexture
                            && (mTextures.get(input.mTexture) == pass.mTarget
                                || mTextures.get(input.mTexture) == pass.mTarget->mDepth
                                || mTextures.get(input.mTexture) == pass.mTarget->mNormals))
                            throw std::invalid_argument("An effect cannot sample its own framebuffer attachment");
                    pass.mEffectDescriptor = effectDescriptor(frame, *pass.mEffect, passes[i].mEffectInputs);
                    effectPipeline(*pass.mEffect, pass.mTarget->mTargetFormat);
                    frame.mResources.push_back(mMeshes.get(mFullscreenMesh));
                }
            }
            std::size_t drawPass = 0;
            std::size_t drawNumber = 0;
            for (const auto* source : draws)
            {
                const auto& draw = *source;
                const auto& mesh = mMeshes.get(draw.mMesh);
                const auto& texture = mTextures.get(draw.mTexture ? draw.mTexture : mWhiteTexture);
                const auto& mask = mTextures.get(draw.mMaskTexture ? draw.mMaskTexture : mWhiteTexture);
                while (drawPass < mPreparedPasses.size()
                    && drawNumber >= mPreparedPasses[drawPass].mFirst + mPreparedPasses[drawPass].mCount)
                    ++drawPass;
                const auto& destination = mPreparedPasses[drawPass].mTarget;
                if (destination && (texture == destination || mask == destination))
                    throw std::invalid_argument("A render target cannot sample its own attachment");
                const bool offscreen = destination != nullptr;
                ++drawNumber;
                const std::size_t rasterKey = 4 * static_cast<std::size_t>(draw.mBlend) + (draw.mDoubleSided ? 2 : 0)
                    + (draw.mFlipWinding ? 1 : 0)
                    + (draw.mDepthTest ? draw.mDepthWrite ? DepthStride * 2 : DepthStride : 0)
                    + CompareStride * static_cast<std::size_t>(draw.mDepthCompare);
                const auto material = draw.mDistortion ? MaterialShader::Distortion
                    : draw.mSky                        ? MaterialShader::Sky
                    : draw.mWater                      ? MaterialShader::Water
                    : !offscreen && !draw.mLighting && !draw.mDepthTest && !draw.mDepthWrite && !draw.mClip
                        && !draw.mMaskTexture && draw.mAlphaCompare == Render::AlphaCompare::Always
                        && (draw.mFogEnd <= draw.mFogStart || draw.mFogEnd <= 0)
                    ? MaterialShader::Overlay
                    : draw.mAlphaCompare == Render::AlphaCompare::Always && !draw.mClip ? MaterialShader::SolidSurface
                                                                                        : MaterialShader::Surface;
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
                        draw.mFogEnd, 0, 0 } });
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
                frame.mDataDescriptorDirty = true;
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
                auto slot = reinterpret_cast<std::uintptr_t>(set.get()) >> 4;
                slot ^= slot >> 17;
                slot *= 0xed5ad4bbu;
                slot ^= slot >> 11;
                const auto mask = mLightOffsets.size() - 1;
                slot &= mask;
                while (mLightOffsets[slot].mSet && mLightOffsets[slot].mSet != set.get())
                    slot = (slot + 1) & mask;
                auto& entry = mLightOffsets[slot];
                if (!entry.mSet)
                {
                    if (set->mPoints.size() > 40
                        || (mLightData.size() + set->mPoints.size()) * sizeof(Render::PointLight)
                            > mProperties.limits.maxStorageBufferRange)
                        throw std::length_error("Vulkan point light data exceeds the device buffer limit");
                    entry = { set.get(), static_cast<std::uint32_t>(mLightData.size()) };
                    mLightData.insert(mLightData.end(), set->mPoints.begin(), set->mPoints.end());
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
                frame.mDataDescriptorDirty = true;
            }
            if (!mLightData.empty())
                std::memcpy(frame.mLightUpload->mMemory.mapped(), mLightData.data(),
                    mLightData.size() * sizeof(Render::PointLight));
            if (!draws.empty() && frame.mDataDescriptorDirty)
            {
                const std::array<VkDescriptorBufferInfo, 2> bindings{ {
                    { frame.mInstances->mBuffer, 0, VK_WHOLE_SIZE },
                    { frame.mLights->mBuffer, 0, VK_WHOLE_SIZE },
                } };
                std::array<VkWriteDescriptorSet, 2> writes{};
                for (unsigned i = 0; i < writes.size(); ++i)
                {
                    writes[i] = vkInfo<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
                    writes[i].dstSet = frame.mDataDescriptor;
                    writes[i].dstBinding = i;
                    writes[i].descriptorCount = 1;
                    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    writes[i].pBufferInfo = &bindings[i];
                }
                vkUpdateDescriptorSets(mDevice, writes.size(), writes.data(), 0, nullptr);
                frame.mDataDescriptorDirty = false;
            }
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
                specular[3] = draw.mShininess;
                const InstanceData instance{ draw.mClipFromModel, draw.mTint,
                    { draw.mAlphaCutoff, static_cast<float>(draw.mAlphaCompare),
                        draw.mDistortion ? draw.mDistortionStrength : draw.mOpacity,
                        draw.mDistortion        ? 3.f
                            : draw.mSky         ? (draw.mSkyPass >= 0 ? 4.f + draw.mSkyPass : 11.f)
                            : draw.mWater       ? (draw.mReflection ? 2.25f : 2.f)
                            : draw.mMaskTexture ? 1.f
                                                : 0.f },
                    { draw.mUVTransform[0], draw.mUVTransform[1], draw.mUVTransform[2], 0 },
                    { draw.mUVTransform[3], draw.mUVTransform[4], draw.mUVTransform[5], 0 },
                    { draw.mMaskUVTransform[0], draw.mMaskUVTransform[1], draw.mMaskUVTransform[2], 0 },
                    { draw.mMaskUVTransform[3], draw.mMaskUVTransform[4], draw.mMaskUVTransform[5], 0 },
                    draw.mViewFromModel, draw.mNormalFromModel, draw.mDiffuse, ambient, emission, specular,
                    draw.mSunPosition, draw.mSunAmbient, draw.mSunDiffuse, draw.mSunSpecular,
                    { mDrawLightOffsets[i],
                        draw.mLighting && !draw.mSky && !draw.mWater && !draw.mDistortion && draw.mLights
                            ? static_cast<std::uint32_t>(draw.mLights->mPoints.size())
                            : 0,
                        (draw.mLighting && !draw.mWater && !draw.mSky ? 1u : 0u) | (draw.mClassicFalloff ? 2u : 0u)
                            | (draw.mClip ? 4u : 0u),
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
            recordUploads(frame, draws.size() * sizeof(InstanceData), mLightData.size() * sizeof(Render::PointLight));
            bool windowWritten = mSwapchainInitialized[imageIndex];
            for (const auto& view : mPreparedPasses)
            {
                const VkExtent2D extent
                    = view.mTarget ? VkExtent2D{ view.mTarget->mWidth, view.mTarget->mHeight } : mExtent;
                const auto x = std::min(view.mViewport[0], extent.width);
                const auto y = std::min(view.mViewport[1], extent.height);
                const auto viewWidth = std::min(view.mViewport[2] ? view.mViewport[2] : extent.width, extent.width - x);
                const auto viewHeight
                    = std::min(view.mViewport[3] ? view.mViewport[3] : extent.height, extent.height - y);
                if (!viewWidth || !viewHeight)
                    continue;
                // Texture rows retain the source GL bottom-up convention. Only
                // the displayed window uses a negative-height viewport.
                const VkViewport viewport{ float(x), float(view.mTarget ? y : extent.height - y), float(viewWidth),
                    view.mTarget ? float(viewHeight) : -float(viewHeight), 0, 1 };
                const VkRect2D scissor{ { static_cast<std::int32_t>(x),
                                            static_cast<std::int32_t>(
                                                view.mTarget ? y : extent.height - y - viewHeight) },
                    { viewWidth, viewHeight } };
                if (view.mTarget)
                    for (const auto& attachment : { view.mTarget, view.mTarget->mNormals })
                    {
                        if (!attachment)
                            continue;
                        auto barrier = vkInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
                        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                        barrier.dstAccessMask
                            = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
                        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        barrier.image = attachment->mImage;
                        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                        vkCmdPipelineBarrier(frame.mCommands,
                            (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT),
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                    }
                std::array<VkClearValue, 3> clear{};
                clear[2].color = { { .5f, .5f, .5f, 0.f } };
                std::copy(view.mClearColor.begin(), view.mClearColor.end(), clear[0].color.float32);
                clear[1].depthStencil = { 1, 0 };
                auto pass = vkInfo<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
                const auto targetSlot = view.mTarget ? static_cast<std::size_t>(view.mTarget->mTargetFormat) : 0;
                pass.renderPass = view.mTarget ? (view.mClear              ? mTargetPasses[targetSlot].mClear
                                                         : view.mOverwrite ? mTargetPasses[targetSlot].mOverwrite
                                                                           : mTargetPasses[targetSlot].mLoad)
                                               : (view.mClear || !windowWritten ? mRenderPass : mMainLoadPass);
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
                VkBuffer boundVertices = VK_NULL_HANDLE;
                VkBuffer boundIndices = VK_NULL_HANDLE;
                VkDeviceSize boundVertexOffset = 0, boundIndexOffset = 0;
                if (!mPrepared.empty())
                {
                    vkCmdBindDescriptorSets(frame.mCommands, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 2, 1,
                        &frame.mDataDescriptor, 0, nullptr);
                }
                bool savedWorldDepth = false;
                for (std::size_t first = view.mFirst; first < view.mFirst + view.mCount;)
                {
                    const auto& draw = mPrepared[first];
                    if (draw.mClearDepthBefore && view.mPreserveWorldDepth && view.mTarget && view.mTarget->mWorldDepth
                        && !savedWorldDepth)
                    {
                        vkCmdEndRenderPass(frame.mCommands);
                        copyWorldDepth(frame.mCommands, *view.mTarget);
                        savedWorldDepth = true;
                        // Ending the pass preserves color/normals. Reopen with
                        // LOAD color and a fresh depth buffer for the weapon.
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
                        pass.renderPass = mTargetPasses[targetSlot].mLoad;
                        vkCmdBeginRenderPass(frame.mCommands, &pass, VK_SUBPASS_CONTENTS_INLINE);
                    }
                    else if (draw.mClearDepthBefore && (!view.mTarget || view.mTarget->mDepth))
                    {
                        VkClearAttachment depth{};
                        depth.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                        depth.clearValue.depthStencil = { 1, 0 };
                        const VkClearRect rect{ scissor, 0, 1 };
                        vkCmdClearAttachments(frame.mCommands, 1, &depth, 1, &rect);
                    }
                    std::size_t end = first + 1;
                    while (end < view.mFirst + view.mCount && mPrepared[end].mMesh == draw.mMesh
                        && mPrepared[end].mTexture == draw.mTexture && mPrepared[end].mPipeline == draw.mPipeline
                        && mPrepared[end].mRasterKey == draw.mRasterKey && mPrepared[end].mMask == draw.mMask
                        && mPrepared[end].mIndexCount == draw.mIndexCount && mPrepared[end].mFog == draw.mFog
                        && !mPrepared[end].mClearDepthBefore)
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
                    vkCmdPushConstants(
                        frame.mCommands, mPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, draw.mFog.data());
                    if (boundTexture != draw.mTexture->mDescriptor || boundMask != draw.mMask->mDescriptor)
                    {
                        boundTexture = draw.mTexture->mDescriptor;
                        boundMask = draw.mMask->mDescriptor;
                        const std::array descriptors{ boundTexture, boundMask };
                        vkCmdBindDescriptorSets(frame.mCommands, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 0, 2,
                            descriptors.data(), 0, nullptr);
                    }
                    const auto& vertices = draw.mMesh->mVertices;
                    if (boundVertices != vertices.page().mBuffer || boundVertexOffset != vertices.offset())
                    {
                        boundVertices = vertices.page().mBuffer;
                        boundVertexOffset = vertices.offset();
                        vkCmdBindVertexBuffers(frame.mCommands, 0, 1, &boundVertices, &boundVertexOffset);
                    }
                    const auto& indices = draw.mMesh->mIndices;
                    if (boundIndices != indices.page().mBuffer || boundIndexOffset != indices.offset())
                    {
                        boundIndices = indices.page().mBuffer;
                        boundIndexOffset = indices.offset();
                        vkCmdBindIndexBuffer(frame.mCommands, boundIndices, boundIndexOffset, VK_INDEX_TYPE_UINT32);
                    }
                    vkCmdDrawIndexed(frame.mCommands, draw.mIndexCount, static_cast<std::uint32_t>(end - first), 0, 0,
                        static_cast<std::uint32_t>(first));
                    first = end;
                }
                if (view.mEffect)
                {
                    const auto& fullscreen = *mMeshes.get(mFullscreenMesh);
                    const VkDeviceSize offset = fullscreen.mVertices.offset();
                    vkCmdBindPipeline(frame.mCommands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        effectPipeline(*view.mEffect, view.mTarget->mTargetFormat));
                    vkCmdBindDescriptorSets(frame.mCommands, VK_PIPELINE_BIND_POINT_GRAPHICS, view.mEffect->mLayout, 0,
                        1, &view.mEffectDescriptor, 0, nullptr);
                    vkCmdBindVertexBuffers(frame.mCommands, 0, 1, &fullscreen.mVertices.page().mBuffer, &offset);
                    vkCmdBindIndexBuffer(frame.mCommands, fullscreen.mIndices.page().mBuffer,
                        fullscreen.mIndices.offset(), VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(frame.mCommands, 3, 1, 0, 0, 0);
                }
                vkCmdEndRenderPass(frame.mCommands);
                if (view.mPreserveWorldDepth && view.mTarget && view.mTarget->mWorldDepth && !savedWorldDepth)
                    copyWorldDepth(frame.mCommands, *view.mTarget);
                if (view.mTarget)
                {
                    auto barrier = vkInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
                    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.image = view.mTarget->mImage;
                    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    vkCmdPipelineBarrier(frame.mCommands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT), 0, 0, nullptr, 0,
                        nullptr, 1, &barrier);
                    if (view.mTarget->mNormals)
                    {
                        barrier.image = view.mTarget->mNormals->mImage;
                        vkCmdPipelineBarrier(frame.mCommands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT), 0, 0,
                            nullptr, 0, nullptr, 1, &barrier);
                    }
                    if (!view.mTarget->mDepth)
                        continue;
                    barrier.image = view.mTarget->mDepth->mImage;
                    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                    if (mDepthFormat == VK_FORMAT_D24_UNORM_S8_UINT)
                        barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
                    vkCmdPipelineBarrier(frame.mCommands,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT), 0, 0, nullptr, 0,
                        nullptr, 1, &barrier);
                }
            }
            if (mCaptureRequested)
            {
                if (!mCaptureSupported)
                    throw std::runtime_error("This Vulkan surface does not support screenshot readback");
                mReadback = createBuffer(VkDeviceSize(mExtent.width) * mExtent.height * 4,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryKind::Upload);
                auto barrier = vkInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
                barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = mSwapchainImages[imageIndex];
                barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCmdPipelineBarrier(frame.mCommands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.imageExtent = { mExtent.width, mExtent.height, 1 };
                vkCmdCopyImageToBuffer(frame.mCommands, barrier.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    mReadback->mBuffer, 1, &region);
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                barrier.dstAccessMask = 0;
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                vkCmdPipelineBarrier(frame.mCommands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                auto host = vkInfo<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
                host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(frame.mCommands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                    &host, 0, nullptr, 0, nullptr);
            }
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
