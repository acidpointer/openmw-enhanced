#include "framedriver.hpp"
#include "../pingpongcanvas.hpp"
#include "../renderbin.hpp"
#include "../vismask.hpp"
#include "effects.hpp"
#include "statsoverlay.hpp"

#include <components/debug/debuglog.hpp>
#include <components/fallback/fallback.hpp>
#include <components/myguiplatform/myguirendermanager.hpp>
#include <components/nifosg/particle.hpp>
#include <components/render/cachesweep.hpp>
#include <components/render/drawranges.hpp>
#include <components/render/framecache.hpp>
#include <components/render/math.hpp>
#include <components/render/meshindices.hpp>
#include <components/render/module.hpp>
#include <components/render/presentation.hpp>
#include <components/render/reserve.hpp>
#include <components/render/residency.hpp>
#include <components/render/texture.hpp>
#include <components/renderassets/imagevariants.hpp>
#include <components/renderassets/meshkey.hpp>
#include <components/renderassets/osgimporter.hpp>
#include <components/renderassets/osgstate.hpp>
#include <components/renderassets/particles.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/parallelwork.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/settings/values.hpp>
#include <components/shader/removedalphafunc.hpp>
#include <components/terrain/compositemaprenderer.hpp>
#include <components/terrain/terraindrawable.hpp>

#include <MyGUI_VertexData.h>

#include <osg/BlendFunc>
#include <osg/ClipPlane>
#include <osg/ColorMask>
#include <osg/CullFace>
#include <osg/Depth>
#include <osg/FrontFace>
#include <osg/Geometry>
#include <osg/Material>
#include <osg/TexMat>
#include <osg/Texture2D>
#include <osg/observer_ptr>
#include <osgUtil/RenderLeaf>
#include <osgUtil/RenderStage>
#include <osgUtil/SceneView>
#include <osgUtil/StateGraph>
#include <osgViewer/Renderer>
#include <osgViewer/Viewer>

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <tuple>
#include <unordered_map>

namespace MWRender
{
    using RenderAssets::NativeUniform;
    namespace
    {
        // Keep the viewer's rendering entry point: loading screens, videos and
        // modal dialogs also call renderingTraversals() outside the game loop.
        class NativeRenderer final : public osgViewer::Renderer, public Render::Presentation
        {
        public:
            std::function<void()> mRender;
            std::function<void(int)> mStats;
            void setStatsLevel(int level) override
            {
                if (mStats)
                    mStats(level);
            }
            std::function<void(Render::PresentMode)> mPresentMode;
            std::function<Render::CapturedImage(bool)> mCapture;
            void setPresentMode(Render::PresentMode mode) override { mPresentMode(mode); }
            Render::CapturedImage capture(bool includeGui) override { return mCapture(includeGui); }
            explicit NativeRenderer(osg::Camera* camera)
                : Renderer(camera)
            {
                setCompileOnNextDraw(false);
            }
            void cull() override {}
            void draw() override { cull_draw(); }
            void cull_draw() override
            {
                if (mRender)
                    mRender();
            }
        };

        std::filesystem::path modulePath()
        {
            char* base = SDL_GetBasePath();
            if (!base)
                throw std::runtime_error("Cannot locate the Vulkan renderer: " + std::string(SDL_GetError()));
            const std::filesystem::path directory(base);
            SDL_free(base);
#ifdef _WIN32
            return directory / "openmw-renderer-vulkan.dll";
#elif defined(__APPLE__)
            return directory / "openmw-renderer-vulkan.so";
#else
            return directory / "openmw-renderer-vulkan.so";
#endif
        }

        Render::Matrix matrix(const osg::Matrixd& source)
        {
            Render::Matrix result;
            std::copy_n(source.ptr(), 16, result.begin());
            return result;
        }

        struct PrepareTimer
        {
            using Clock = std::chrono::steady_clock;
            double& mMilliseconds;
            Clock::time_point mStart = Clock::now();
            ~PrepareTimer()
            {
                mMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - mStart).count();
            }
        };

        struct MeshStorage
        {
            Render::Backend& mBackend;
            Render::Residency& mBudget;
            Render::Residency::Charge mResidency;
            Render::MeshHandle mHandle;
            std::size_t mVertices = 0, mIndices = 0;
            bool mCanonical = false;
            std::uint64_t mSeen = 0;
            MeshStorage(Render::Backend& backend, Render::Residency& budget)
                : mBackend(backend)
                , mBudget(budget)
                , mResidency(&budget)
            {
            }
            ~MeshStorage()
            {
                if (mHandle)
                    mBackend.releaseMesh(mHandle);
            }
        };

        struct GpuMesh;
        struct SharedMesh
        {
            RenderAssets::MeshKey mKey;
            std::shared_ptr<MeshStorage> mStorage;
            // Cleared before retirement, including unused/failed frame plans.
            std::uint64_t mPendingFrame = 0;
            GpuMesh* mPendingOwner = nullptr;
        };

        enum class MeshWork
        {
            None,
            Static,
            BuildStream,
            UpdateStream,
            Alias
        };

        struct GpuMesh
        {
            Render::Backend& mBackend;
            std::shared_ptr<MeshStorage> mStorage;
            Render::MeshHandle mHandle;
            Render::MeshHandle mCheckedHandle;
            std::uint64_t mCheckedFrame = 0;
            osg::observer_ptr<const osg::Object> mSource;
            std::size_t mRevision = 0;
            std::optional<std::size_t> mRejectedRevision;
            std::size_t mVertices = 0;
            std::size_t mIndices = 0;
            std::size_t mTopologyRevision = 0;
            std::unique_ptr<RenderAssets::MeshStream> mStream;
            std::uint64_t mSeen = 0;
            std::uint64_t mPreparedFrame = 0;
            std::size_t mPreparedRevision = 0, mPreparedTopology = 0;
            std::size_t mWorkSize = 0;
            unsigned mPreparedUV = 0;
            MeshWork mWork = MeshWork::None;
            bool mWorkDone = false;
            osg::ref_ptr<const osg::Geometry> mPreparedSource;
            RenderAssets::MeshBuffers mPreparedBuffers;
            std::exception_ptr mPreparationError;
            GpuMesh* mPreparedOwner = nullptr;
            SharedMesh* mCanonical = nullptr;
            explicit GpuMesh(Render::Backend& backend, Render::Residency& residency)
                : mBackend(backend)
                , mStorage(std::make_shared<MeshStorage>(backend, residency))
            {
            }
            void upload(std::span<const Render::Vertex> vertices, std::span<const std::uint32_t> indices,
                bool verticesOnly = false, std::size_t liveVertices = 0)
            {
                if (mStorage.use_count() > 1 || mStorage->mCanonical)
                {
                    mStorage = std::make_shared<MeshStorage>(mBackend, mStorage->mBudget);
                    mHandle = {};
                }
                if (mHandle && mVertices == vertices.size() && mIndices == indices.size())
                    mBackend.updateMesh(mHandle, verticesOnly && liveVertices ? vertices.first(liveVertices) : vertices,
                        verticesOnly ? std::span<const std::uint32_t>{} : indices);
                else
                {
                    const auto replacement = mBackend.createMesh(vertices, indices);
                    if (mHandle)
                        mBackend.releaseMesh(mHandle);
                    mHandle = replacement;
                }
                mStorage->mHandle = mHandle;
                mStorage->mVertices = vertices.size();
                mStorage->mIndices = indices.size();
                mStorage->mResidency.set(vertices.size_bytes()
                    + Render::meshIndexBytes(indices.size(), Render::narrowMeshIndices(vertices.size())));
                mStorage->mResidency.touch();
                mVertices = vertices.size();
                mIndices = indices.size();
            }
        };

        struct GpuTexture
        {
            Render::Backend& mBackend;
            Render::Residency::Charge mResidency;
            // Alias bindings retain the one charged, immutable image upload.
            std::shared_ptr<GpuTexture> mStorage;
            Render::TextureHandle mHandle;
            osg::observer_ptr<const osg::Image> mImage;
            unsigned mRevision = 0;
            std::uint64_t mSeen = 0;
            explicit GpuTexture(Render::Backend& backend, Render::Residency* residency = nullptr)
                : mBackend(backend)
                , mResidency(residency)
            {
            }
            ~GpuTexture()
            {
                if (mHandle)
                    mBackend.releaseTexture(mHandle);
            }
        };

        std::size_t geometryRevision(const osg::Geometry& geometry, unsigned uvUnit)
        {
            std::size_t result = 0;
            const auto add = [&](const osg::BufferData* data) {
                const auto value = reinterpret_cast<std::uintptr_t>(data)
                    ^ (data ? std::size_t(data->getModifiedCount()) * 0x9e3779b9u : 0);
                result ^= value + 0x9e3779b9u + (result << 6) + (result >> 2);
            };
            add(geometry.getVertexArray());
            result ^= geometry.getVertexArray() ? geometry.getVertexArray()->getNumElements() : 0;
            add(geometry.getTexCoordArray(uvUnit));
            if (uvUnit != 0)
                add(geometry.getTexCoordArray(0));
            add(geometry.getColorArray());
            add(geometry.getNormalArray());
            for (const auto* channel : { geometry.getColorArray(), geometry.getNormalArray() })
                result = result * 31 + (channel ? static_cast<unsigned>(channel->getBinding()) : 0);
            for (const auto& primitive : geometry.getPrimitiveSetList())
            {
                add(primitive);
                result = result * 31 + (primitive ? primitive->getMode() : 0);
                result = result * 31 + (primitive ? primitive->getNumIndices() : 0);
                result = result * 31 + (primitive ? primitive->getNumInstances() : 0);
            }
            return result;
        }

        std::size_t topologyRevision(const osg::Geometry& geometry)
        {
            std::size_t result = geometry.getVertexArray() ? geometry.getVertexArray()->getNumElements() : 0;
            for (const auto* channel : { geometry.getColorArray(), geometry.getNormalArray() })
                result = result * 31 + (channel ? static_cast<unsigned>(channel->getBinding()) : 0);
            for (const auto& primitive : geometry.getPrimitiveSetList())
            {
                result ^= reinterpret_cast<std::uintptr_t>(primitive.get())
                    + (primitive ? std::size_t(primitive->getModifiedCount()) << 1 : 0) + (result << 6) + (result >> 2);
                result = result * 31 + (primitive ? primitive->getMode() : 0);
                result = result * 31 + (primitive ? primitive->getNumIndices() : 0);
                result = result * 31 + (primitive ? primitive->getNumInstances() : 0);
            }
            return result;
        }

        Render::AlphaCompare alphaComparison(GLenum function)
        {
            switch (function)
            {
                case GL_NEVER:
                    return Render::AlphaCompare::Never;
                case GL_LESS:
                    return Render::AlphaCompare::Less;
                case GL_LEQUAL:
                    return Render::AlphaCompare::LessEqual;
                case GL_EQUAL:
                    return Render::AlphaCompare::Equal;
                case GL_NOTEQUAL:
                    return Render::AlphaCompare::NotEqual;
                case GL_GEQUAL:
                    return Render::AlphaCompare::GreaterEqual;
                case GL_GREATER:
                    return Render::AlphaCompare::Greater;
                default:
                    return Render::AlphaCompare::Always;
            }
        }
    }

    class VulkanFrameDriver::Impl final : public MyGUIPlatform::DrawSink
    {
    public:
        Impl(osgViewer::Viewer& viewer, const Render::BackendOptions& options)
            : mViewer(viewer)
            , mWindow(options.mWindow)
            , mModule(Render::BackendModule::load(modulePath(), options))
            , mMeshWorkers(SceneUtil::ParallelWork::workersFor(std::thread::hardware_concurrency()))
        {
            // CPU scene callbacks (including skinning and terrain selection)
            // run once on the owner thread; they never submit a GL world frame.
            constexpr std::uint64_t MiB = 1024 * 1024;
            mStates.reserve(8192);
            mMeshes.reserve(4096);
            mTextureBindings.reserve(2048);
            mImageVariants.reserve(2048);
            mLightSets.reserve(1024);
            mGuiMeshes.reserve(128);
            const auto& device = backend().deviceInfo();
            const auto localMemory = device.mLocalMemoryBudgetBytes
                ? std::min(device.mLocalMemoryBytes, device.mLocalMemoryBudgetBytes)
                : device.mLocalMemoryBytes;
            mMeshResidency.setLimit(Render::Residency::limit(localMemory, 16, 32 * MiB, 512 * MiB, 64 * MiB),
                Render::Residency::limit(localMemory, 4, 64 * MiB, 2048 * MiB, 256 * MiB));
            mTextureResidency.setLimit(Render::Residency::limit(localMemory, 8, 64 * MiB, 2048 * MiB, 256 * MiB),
                Render::Residency::limit(localMemory, 2, 128 * MiB, 4096 * MiB, 1024 * MiB));
            viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
            viewer.setIncrementalCompileOperation(nullptr);
            mSceneView = new osgUtil::SceneView;
            mSceneView->setDefaults(osgUtil::SceneView::NO_SCENEVIEW_LIGHT);
            osg::ref_ptr<osg::DisplaySettings> display = new osg::DisplaySettings;
            display->setStereo(false);
            mSceneView->setDisplaySettings(display);
            mSceneView->setCamera(viewer.getCamera(), false);
            mSceneView->setState(viewer.getCamera()->getGraphicsContext()->getState());
            mRenderer = new NativeRenderer(viewer.getCamera());
            mRenderer->mRender = [this] { render(); };
            mRenderer->mStats = [this](int level) {
                if (!mStats)
                    mStats = std::make_unique<Vulkan::StatsOverlay>(backend());
                mStats->setLevel(level);
            };
            mRenderer->mPresentMode = [this](Render::PresentMode mode) { backend().setPresentMode(mode); };
            mRenderer->mCapture = [this](bool includeGui) {
                struct RestoreGui
                {
                    bool& mValue;
                    bool mPrevious;
                    ~RestoreGui() { mValue = mPrevious; }
                } restore{ mDrawGui, mDrawGui };
                mDrawGui = includeGui;
                backend().requestCapture();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                do
                {
                    render();
                    auto pixels = backend().takeCapture();
                    if (pixels.mWidth)
                        return pixels;
                    SDL_PumpEvents();
                } while (std::chrono::steady_clock::now() < deadline);
                throw std::runtime_error("Vulkan screenshot could not acquire a frame within two seconds");
            };
            const std::array<std::byte, 4> transparent{};
            mTransparent = backend().createTexture(1, 1, transparent, false, false);
            Log(Debug::Info) << "Vulkan Renderer: " << backend().deviceInfo().mName;
            SDL_SetWindowTitle(options.mWindow, "OpenMW — Vulkan (experimental)");
            // Publish the callback only after construction can no longer fail
            // while allocating renderer resources.
            viewer.getCamera()->setRenderer(mRenderer);
        }

        ~Impl() override
        {
            mRenderer->mRender = {};
            mRenderer->mStats = {};
            mRenderer->mPresentMode = {};
            mRenderer->mCapture = {};
            mMeshes.clear();
            mGuiMeshes.clear();
            mParticleMeshes.clear();
            mTextures.clear();
            mTargets.clear();
            backend().releaseTexture(mTransparent);
        }

        bool mShadowCasting = false;
        std::vector<std::pair<const osg::Uniform*, std::shared_ptr<Render::ShadowSet>>> mShadowSets;
        std::size_t mShadowSetsUsed = 0;

        Render::Backend& backend() { return mModule->backend(); }

        struct TargetRecord
        {
            osg::observer_ptr<const osg::Texture2D> mSource;
            std::unique_ptr<GpuTexture> mTexture;
            std::unique_ptr<GpuTexture> mShadowDepth;
            unsigned mWidth = 0;
            unsigned mHeight = 0;
            Render::TargetFormat mFormat = Render::TargetFormat::RGBA8;
        };

        Render::TextureHandle target(
            const osg::Texture2D& source, unsigned fallbackWidth = 0, unsigned fallbackHeight = 0)
        {
            const auto found = mTargets.find(&source);
            if (found != mTargets.end() && found->second.mSource.get() == &source)
            {
                if (!fallbackWidth)
                    fallbackWidth = found->second.mWidth;
                if (!fallbackHeight)
                    fallbackHeight = found->second.mHeight;
            }
            const auto width
                = source.getTextureWidth() > 0 ? static_cast<unsigned>(source.getTextureWidth()) : fallbackWidth;
            const auto height
                = source.getTextureHeight() > 0 ? static_cast<unsigned>(source.getTextureHeight()) : fallbackHeight;
            if (!width || !height)
                return mTransparent;
            const auto format = source.getInternalFormat() == GL_DEPTH_COMPONENT
                    || source.getInternalFormat() == GL_DEPTH_COMPONENT16
                    || source.getInternalFormat() == GL_DEPTH_COMPONENT24
                    || source.getInternalFormat() == GL_DEPTH_COMPONENT32F
                ? Render::TargetFormat::ShadowMap
                : source.getInternalFormat() == GL_RGBA16F || source.getInternalFormat() == GL_RGB16F
                ? Render::TargetFormat::RGBA16F
                : Render::TargetFormat::RGBA8;
            auto& record = mTargets[&source];
            if (record.mSource.get() != &source || record.mWidth != width || record.mHeight != height
                || record.mFormat != format)
            {
                PrepareTimer timer{ mPrepare.mTargets };
                auto replacement = std::make_unique<GpuTexture>(backend());
                auto sampling = RenderAssets::OsgImporter::importSampler(source);
                if (format == Render::TargetFormat::ShadowMap)
                {
                    sampling.mMinFilter = sampling.mMagFilter = Render::TextureFilter::Nearest;
                    sampling.mWrapS = sampling.mWrapT = Render::TextureWrap::Clamp;
                }
                replacement->mHandle = backend().createRenderTarget(width, height, sampling, format);
                record.mSource = &source;
                record.mShadowDepth.reset();
                record.mTexture = std::move(replacement);
                record.mWidth = width;
                record.mHeight = height;
                record.mFormat = format;
            }
            return record.mTexture->mHandle;
        }

        Render::TextureHandle texture(const osg::Texture2D* source, bool gui)
        {
            if (!source)
                return {};
            auto& binding = mTextureBindings[source];
            if (binding.mSource.get() != source)
            {
                binding = {};
                binding.mSource = source;
            }
            const unsigned slot = gui ? 1 : 0;
            if (binding.mFrame[slot] != mSerial)
            {
                binding.mHandles[slot] = importTexture(source, gui);
                binding.mFrame[slot] = mSerial;
            }
            return binding.mHandles[slot];
        }

        Render::TextureHandle importTexture(const osg::Texture2D* source, bool gui)
        {
            if (!source)
                return {};
            const auto existing = mTargets.find(source);
            if (existing != mTargets.end() && existing->second.mSource.get() == source)
                return target(*source);
            const auto* sourceImage = source->getImage();
            if (!sourceImage || !sourceImage->data())
                return source->getTextureWidth() > 0 && source->getTextureHeight() > 0 ? target(*source)
                    : gui                                                              ? mTransparent
                                                                                       : Render::TextureHandle{};
            const auto sampling = RenderAssets::OsgImporter::importSampler(*source);
            const auto key = std::tuple{ reinterpret_cast<std::uintptr_t>(sourceImage), sampling };
            auto& slot = mTextures[key];
            if (!slot || slot->mImage.get() != sourceImage || !slot->mHandle
                || slot->mRevision != sourceImage->getModifiedCount())
            {
                PrepareTimer timer{ mPrepare.mTextures };
                if (auto existingStorage = mImageVariants.find(*sourceImage, sampling.mUseMipmaps))
                {
                    auto view = std::make_shared<GpuTexture>(backend());
                    view->mImage = sourceImage;
                    view->mRevision = sourceImage->getModifiedCount();
                    view->mStorage = std::move(existingStorage);
                    view->mHandle = backend().createTextureSamplerView(view->mStorage->mHandle, sampling);
                    slot = std::move(view);
                }
                else
                {
                    auto data = RenderAssets::OsgImporter::borrowTexture(*sourceImage, mLevels, sampling.mUseMipmaps);
                    std::shared_ptr<const Render::ImageAsset> converted;
                    if (!data || !backend().supportsTextureFormat(data->mFormat, data->mSrgb))
                    {
                        converted = mImporter.importImage(*sourceImage, true);
                        if (!backend().supportsTextureFormat(converted->mFormat, converted->mSrgb))
                            converted = converted->mDecode ? converted->mDecode() : converted->mUncompressed;
                        if (!converted)
                            throw std::runtime_error("Live texture has no supported device format");
                        mLevels.clear();
                        for (const auto& level : converted->mLevels)
                        {
                            mLevels.push_back({ level.mWidth, level.mHeight, level.mBytes });
                            if (!sampling.mUseMipmaps)
                                break;
                        }
                        data = Render::TextureData{ converted->mFormat, converted->mSrgb,
                            converted->mGenerateMipmaps && sampling.mUseMipmaps, mLevels };
                    }
                    // Keep uploaded image generations immutable. Animated images
                    // replace their storage; already recorded views retain the old
                    // generation until their GPU frames finish.
                    auto storage = std::make_shared<GpuTexture>(backend(), &mTextureResidency);
                    storage->mImage = sourceImage;
                    storage->mRevision = sourceImage->getModifiedCount();
                    storage->mHandle = backend().createTexture(*data, sampling);
                    std::uint64_t bytes = 0;
                    for (const auto& level : mLevels)
                        bytes += level.mBytes.size();
                    if (data->mGenerateMipmaps && mLevels.size() == 1)
                    {
                        auto width = mLevels.front().mWidth, height = mLevels.front().mHeight;
                        while (width > 1 || height > 1)
                        {
                            width = std::max(1u, width / 2);
                            height = std::max(1u, height / 2);
                            bytes += Render::textureLevelSize(data->mFormat, width, height);
                        }
                    }
                    storage->mResidency.set(bytes);
                    mImageVariants.store(*sourceImage, sampling.mUseMipmaps, storage);
                    slot = std::move(storage);
                    ++mImageChanges;
                }
            }
            auto& cached = *slot;
            cached.mSeen = mSerial;
            auto& storage = cached.mStorage ? *cached.mStorage : cached;
            storage.mSeen = mSerial;
            storage.mResidency.touch();
            return cached.mHandle;
        }

        void resident(GpuMesh& mesh)
        {
            mesh.mStorage->mSeen = mSerial;
            mesh.mStorage->mResidency.touch();
            mesh.mCheckedFrame = mSerial;
            mesh.mCheckedHandle = mesh.mHandle;
        }

        GpuMesh& planMesh(const osg::Geometry& geometry, unsigned uvUnit)
        {
            auto& slot = mMeshes[{ &geometry, uvUnit }];
            if (!slot || slot->mSource.get() != &geometry)
            {
                slot = std::make_unique<GpuMesh>(backend(), mMeshResidency);
                slot->mSource = &geometry;
            }
            auto& mesh = *slot;
            mesh.mSeen = mSerial;
            if (mesh.mCheckedFrame == mSerial || mesh.mPreparedFrame == mSerial)
                return mesh;
            mesh.mPreparedFrame = mSerial;
            mesh.mPreparedRevision = geometryRevision(geometry, uvUnit);
            if (mesh.mRejectedRevision == mesh.mPreparedRevision)
            {
                mesh.mCheckedFrame = mSerial;
                mesh.mCheckedHandle = {};
                return mesh;
            }
            if (mesh.mHandle && mesh.mRevision == mesh.mPreparedRevision)
            {
                resident(mesh);
                return mesh;
            }
            mesh.mPreparedTopology = topologyRevision(geometry);
            mesh.mPreparedUV = uvUnit;
            mesh.mPreparedSource = &geometry;
            mesh.mPreparationError = nullptr;
            mesh.mWorkDone = false;
            mesh.mPreparedOwner = nullptr;
            mesh.mCanonical = nullptr;
            Render::reserveFor(mMeshPlans, mMeshPlans.size() + 1);
            mMeshPlans.push_back(&mesh);
            if (mesh.mHandle)
                mesh.mWork = !mesh.mStream || mesh.mTopologyRevision != mesh.mPreparedTopology ? MeshWork::BuildStream
                                                                                               : MeshWork::UpdateStream;
            else if (const auto* vbo = geometry.getVertexArray()->getVertexBufferObject();
                vbo && (vbo->getUsage() == GL_DYNAMIC_DRAW_ARB || vbo->getUsage() == GL_STREAM_DRAW_ARB))
                mesh.mWork = MeshWork::BuildStream;
            else
            {
                auto key = RenderAssets::meshKey(geometry, uvUnit);
                auto& shared = mSharedMeshes[key.mWords];
                if (shared.mKey.current() && shared.mStorage)
                {
                    mesh.mStorage = shared.mStorage;
                    mesh.mHandle = mesh.mStorage->mHandle;
                    mesh.mVertices = mesh.mStorage->mVertices;
                    mesh.mIndices = mesh.mStorage->mIndices;
                    mesh.mRevision = mesh.mPreparedRevision;
                    mesh.mTopologyRevision = mesh.mPreparedTopology;
                    resident(mesh);
                    return mesh;
                }
                if (shared.mPendingFrame == mSerial && shared.mKey.current())
                {
                    mesh.mWork = MeshWork::Alias;
                    mesh.mPreparedOwner = shared.mPendingOwner;
                    return mesh;
                }
                shared.mKey = std::move(key);
                shared.mStorage.reset();
                shared.mPendingFrame = mSerial;
                shared.mPendingOwner = &mesh;
                mesh.mCanonical = &shared;
                mesh.mWork = MeshWork::Static;
            }
            if (mesh.mWork == MeshWork::UpdateStream)
                ++mPrepare.mMeshUpdates;
            else
                ++mPrepare.mMeshBuilds;
            mesh.mWorkSize = mesh.mWork == MeshWork::UpdateStream ? mesh.mStream->mVertices.size()
                                                                  : geometry.getVertexArray()->getNumElements();
            if (RenderAssets::OsgImporter::canImportInParallel(geometry, uvUnit))
            {
                Render::reserveFor(mMeshJobs, mMeshJobs.size() + 1);
                mMeshJobs.push_back(&mesh);
            }
            return mesh;
        }

        static void prepareMesh(GpuMesh& mesh) noexcept
        {
            try
            {
                switch (mesh.mWork)
                {
                    case MeshWork::Static:
                        mesh.mPreparedBuffers
                            = RenderAssets::OsgImporter::importMeshBuffers(*mesh.mPreparedSource, mesh.mPreparedUV);
                        break;
                    case MeshWork::BuildStream:
                        mesh.mStream = std::make_unique<RenderAssets::MeshStream>(
                            RenderAssets::OsgImporter::importMeshStream(*mesh.mPreparedSource, mesh.mPreparedUV));
                        break;
                    case MeshWork::UpdateStream:
                        RenderAssets::OsgImporter::updateMeshStream(*mesh.mPreparedSource, *mesh.mStream);
                        break;
                    default:
                        break;
                }
            }
            catch (...)
            {
                mesh.mPreparationError = std::current_exception();
            }
            mesh.mWorkDone = true;
        }

        Render::MeshHandle finishMesh(GpuMesh& mesh)
        {
            if (mesh.mCheckedFrame == mSerial)
                return mesh.mCheckedHandle;
            mesh.mCheckedFrame = mSerial;
            mesh.mCheckedHandle = {};
            try
            {
                if (mesh.mWork == MeshWork::Alias)
                {
                    if (!finishMesh(*mesh.mPreparedOwner))
                    {
                        mesh.mRejectedRevision = mesh.mPreparedRevision;
                        return {};
                    }
                    mesh.mStorage = mesh.mPreparedOwner->mStorage;
                    mesh.mHandle = mesh.mStorage->mHandle;
                    mesh.mVertices = mesh.mStorage->mVertices;
                    mesh.mIndices = mesh.mStorage->mIndices;
                }
                else
                {
                    if (!mesh.mWorkDone)
                        prepareMesh(mesh); // Late composite/other uncaptured geometry.
                    if (mesh.mPreparationError)
                        std::rethrow_exception(mesh.mPreparationError);
                    if (mesh.mWork == MeshWork::Static)
                    {
                        mesh.upload(mesh.mPreparedBuffers.mVertices, mesh.mPreparedBuffers.mIndices);
                        mesh.mStorage->mCanonical = true;
                        if (mesh.mCanonical && mesh.mCanonical->mPendingOwner == &mesh)
                            mesh.mCanonical->mStorage = mesh.mStorage;
                        mesh.mPreparedBuffers = {};
                    }
                    else
                        mesh.upload(mesh.mStream->mVertices, mesh.mStream->mIndices,
                            mesh.mHandle && mesh.mPreparedTopology == mesh.mTopologyRevision);
                    ++mMeshChanges;
                }
                mesh.mRevision = mesh.mPreparedRevision;
                mesh.mTopologyRevision = mesh.mPreparedTopology;
            }
            catch (const std::invalid_argument& error)
            {
                mesh.mStream.reset();
                mesh.mRejectedRevision = mesh.mPreparedRevision;
                if (mWarnings.insert(error.what()).second)
                    Log(Debug::Warning) << "Vulkan geometry: " << error.what();
                return {};
            }
            resident(mesh);
            return mesh.mCheckedHandle;
        }

        Render::MeshHandle mesh(GpuMesh& prepared)
        {
            if (prepared.mCheckedFrame == mSerial)
                return prepared.mCheckedHandle;
            PrepareTimer timer{ mPrepare.mMeshes };
            return finishMesh(prepared);
        }

        Render::MeshHandle mesh(const osg::Geometry& geometry, unsigned uvUnit)
        {
            return mesh(planMesh(geometry, uvUnit));
        }

        void clearMeshPlans()
        {
            for (auto* mesh : mMeshPlans)
            {
                mesh->mPreparedSource = nullptr;
                mesh->mPreparedBuffers = {};
                mesh->mPreparationError = nullptr;
                mesh->mPreparedOwner = nullptr;
                if (mesh->mCanonical && mesh->mCanonical->mPendingOwner == mesh)
                {
                    mesh->mCanonical->mPendingFrame = 0;
                    mesh->mCanonical->mPendingOwner = nullptr;
                }
                mesh->mCanonical = nullptr;
                mesh->mPreparedFrame = 0;
                mesh->mWork = MeshWork::None;
            }
            mMeshPlans.clear();
            mMeshJobs.clear();
        }

        void drawGui(
            const osg::Array& source, std::size_t count, osg::Texture2D* sourceTexture, osg::StateSet* state) override
        {
            if (!count)
                return;
            if (count % 3 || count > source.getTotalDataSize() / sizeof(MyGUI::Vertex))
                throw std::invalid_argument("Invalid native GUI vertex range");
            auto& slot = mGuiMeshes[&source];
            if (!slot || slot->mSource.get() != &source)
            {
                slot = std::make_unique<GpuMesh>(backend(), mMeshResidency);
                slot->mSource = &source;
            }
            slot->mSeen = mSerial;
            if (!slot->mHandle || slot->mRevision != source.getModifiedCount() || slot->mVertices != count)
            {
                mGuiVertices.resize(count);
                mGuiIndices.resize(count);
                if (!slot->mHandle || slot->mIndices != count)
                    std::iota(mGuiIndices.begin(), mGuiIndices.end(), 0u);
                for (std::size_t i = 0; i < count; ++i)
                {
                    MyGUI::Vertex vertex;
                    std::memcpy(&vertex, static_cast<const std::byte*>(source.getDataPointer()) + i * sizeof(vertex),
                        sizeof(vertex));
                    auto& target = mGuiVertices[i];
                    target.mPosition = { vertex.x, vertex.y, vertex.z };
                    target.mUV = { vertex.u, vertex.v };
                    std::memcpy(target.mColor.data(), &vertex.colour, 4);
                    target.mAlpha = -1;
                }
                slot->upload(mGuiVertices, mGuiIndices, true);
                slot->mRevision = source.getModifiedCount();
                ++mMeshChanges;
            }
            slot->mStorage->mResidency.touch();
            Render::Draw draw{};
            draw.mMesh = slot->mHandle;
            draw.mTexture = texture(sourceTexture, true);
            draw.mClipFromModel = Render::IdentityMatrix;
            draw.mBlend = Render::BlendMode::Alpha;
            draw.mDoubleSided = true;
            draw.mDepthTest = false;
            draw.mDepthWrite = false;
            if (state)
                applyState(draw, *state);
            mGuiDraws.push_back(draw);
        }

        template <class StateType>
        std::shared_ptr<const Render::LightSet> lights(const StateType& state)
        {
            const auto* values = RenderAssets::nativeUniform(state, NativeUniform::LightBuffer);
            const auto* countUniform = RenderAssets::nativeUniform(state, NativeUniform::PointLightCount);
            int count = 0;
            if (!values || !countUniform || !countUniform->get(count) || count <= 0)
                return {};
            count = std::min({ count, 40, static_cast<int>(values->getNumElements()) });
            auto& entry = mLightSets[values];
            if (entry.mSource.get() != values || entry.mRevision != values->getModifiedCount() || entry.mCount != count
                || !entry.mLights)
            {
                // Draws borrow these CPU light sets only through renderPasses;
                // the backend packs their bytes before returning. Reuse the
                // decoded buffer on the next frame instead of allocating a new
                // shared owner and vector for every changing light state.
                if (!entry.mLights || entry.mSource.get() != values)
                    entry.mLights = std::make_shared<Render::LightSet>();
                auto& points = entry.mLights->mPoints;
                points.clear();
                points.reserve(count);
                for (int i = 0; i < count; ++i)
                {
                    osg::Matrixf packed;
                    if (!values->getElement(i, packed))
                        continue;
                    if (!std::all_of(packed.ptr(), packed.ptr() + 16, [](float v) { return std::isfinite(v); }))
                        continue;
                    Render::PointLight light;
                    std::copy_n(packed.ptr(), 4, light.mPosition.begin());
                    std::copy_n(packed.ptr() + 4, 4, light.mAmbient.begin());
                    std::copy_n(packed.ptr() + 8, 4, light.mDiffuse.begin());
                    std::copy_n(packed.ptr() + 12, 4, light.mSpecular.begin());
                    light.mAttenuation = { packed.ptr()[3], packed.ptr()[7], packed.ptr()[11], packed.ptr()[15] };
                    points.push_back(light);
                }
                entry.mSource = values;
                entry.mRevision = values->getModifiedCount();
                entry.mCount = count;
            }
            entry.mSeen = mSerial;
            return entry.mLights;
        }

        std::shared_ptr<const Render::ShadowSet> shadows(const RenderAssets::OsgStateView& state)
        {
            if (!Settings::shadows().mEnableShadows)
                return {};
            const auto* source = state.getUniform(NativeUniform::ShadowSpaceMatrix0);
            if (!source)
                return {};
            float maximum = Settings::shadows().mMaximumShadowMapDistance;
            if (const auto* uniform = state.getUniform(NativeUniform::MaximumShadowMapDistance))
                uniform->get(maximum);
            if (maximum > 0 && maximum <= .0001f)
                return {}; // OpenMW's protected, explicitly unshadowed state.
            for (std::size_t i = 0; i < mShadowSetsUsed; ++i)
                if (mShadowSets[i].first == source)
                    return mShadowSets[i].second->mCount ? mShadowSets[i].second : nullptr;
            if (mShadowSetsUsed == mShadowSets.size())
                mShadowSets.emplace_back(nullptr, std::make_shared<Render::ShadowSet>());
            auto& [key, owner] = mShadowSets[mShadowSetsUsed++];
            key = source;
            auto& result = *owner;
            result = {};
            result.mMaximumDistance = maximum;
            result.mFadeStart = maximum * Settings::shadows().mShadowFadeStart;
            if (const auto* uniform = state.getUniform("shadowFadeStart"))
                uniform->get(result.mFadeStart);
            result.mNormalOffset = Settings::shadows().mNormalOffsetDistance;
            result.mSoft = Settings::shadows().mSoftShadows;
            static constexpr std::array<std::array<std::string_view, 3>, 8> bindings{ {
                { "shadowSpaceMatrix0", "shadowTexture0", "validRegionMatrix0" },
                { "shadowSpaceMatrix1", "shadowTexture1", "validRegionMatrix1" },
                { "shadowSpaceMatrix2", "shadowTexture2", "validRegionMatrix2" },
                { "shadowSpaceMatrix3", "shadowTexture3", "validRegionMatrix3" },
                { "shadowSpaceMatrix4", "shadowTexture4", "validRegionMatrix4" },
                { "shadowSpaceMatrix5", "shadowTexture5", "validRegionMatrix5" },
                { "shadowSpaceMatrix6", "shadowTexture6", "validRegionMatrix6" },
                { "shadowSpaceMatrix7", "shadowTexture7", "validRegionMatrix7" },
            } };
            for (unsigned i = 0; i < Render::ShadowSet::MaxMaps; ++i)
            {
                const auto* transform = state.getUniform(bindings[i][0]);
                const auto unit = RenderAssets::textureUnit(state, bindings[i][1]);
                if (!transform || !unit)
                    break;
                const auto* image = dynamic_cast<const osg::Texture2D*>(
                    state.getTextureAttribute(*unit, osg::StateAttribute::TEXTURE));
                if (!image || image->getImage())
                    break; // Dummy maps disable shadows, they are not depth targets.
                osg::Matrixf matrix;
                if (!transform->get(matrix))
                    break;
                const auto shadowTarget = target(*image);
                auto& record = mTargets.at(image);
                if (!record.mShadowDepth)
                {
                    record.mShadowDepth = std::make_unique<GpuTexture>(backend());
                    record.mShadowDepth->mHandle = backend().depthTexture(shadowTarget);
                }
                result.mTextures[i] = record.mShadowDepth->mHandle;
                std::copy_n(matrix.ptr(), 16, result.mViewToShadow[i].begin());
                if (const auto* region = state.getUniform(bindings[i][2]); region && region->get(matrix))
                {
                    std::copy_n(matrix.ptr(), 16, result.mValidRegions[i].begin());
                    result.mRegionMask |= 1u << i;
                }
                ++result.mCount;
            }
            return result.mCount ? owner : nullptr;
        }

        template <class StateType>
        void applyState(Render::Draw& draw, const StateType& state)
        {
            if (const auto* strength = RenderAssets::nativeUniform(state, NativeUniform::DistortionStrength))
                strength->get(draw.mDistortionStrength);
            if (const auto* material
                = dynamic_cast<const SceneUtil::Material*>(state.getAttribute(osg::StateAttribute::MATERIAL)))
            {
                std::copy_n(material->getDiffuse().ptr(), 4, draw.mDiffuse.begin());
                std::copy_n(material->getAmbient().ptr(), 4, draw.mAmbient.begin());
                std::copy_n(material->getEmission().ptr(), 4, draw.mEmission.begin());
                std::copy_n(material->getSpecular().ptr(), 4, draw.mSpecular.begin());
                draw.mShininess = material->getShininess();
                draw.mVertexColorMode = static_cast<std::uint32_t>(material->getVertexColorMode());
                draw.mUseVertexColor = draw.mVertexColorMode != 0;
            }
            else if (const auto* osgMaterial
                = dynamic_cast<const osg::Material*>(state.getAttribute(osg::StateAttribute::MATERIAL)))
            {
                std::copy_n(osgMaterial->getDiffuse(osg::Material::FRONT).ptr(), 4, draw.mDiffuse.begin());
                std::copy_n(osgMaterial->getAmbient(osg::Material::FRONT).ptr(), 4, draw.mAmbient.begin());
                std::copy_n(osgMaterial->getEmission(osg::Material::FRONT).ptr(), 4, draw.mEmission.begin());
                std::copy_n(osgMaterial->getSpecular(osg::Material::FRONT).ptr(), 4, draw.mSpecular.begin());
                draw.mShininess = osgMaterial->getShininess(osg::Material::FRONT);
                switch (osgMaterial->getColorMode())
                {
                    case osg::Material::EMISSION:
                        draw.mVertexColorMode = 1;
                        break;
                    case osg::Material::AMBIENT_AND_DIFFUSE:
                        draw.mVertexColorMode = 2;
                        break;
                    case osg::Material::AMBIENT:
                        draw.mVertexColorMode = 3;
                        break;
                    case osg::Material::DIFFUSE:
                        draw.mVertexColorMode = 4;
                        break;
                    case osg::Material::SPECULAR:
                        draw.mVertexColorMode = 5;
                        break;
                    default:
                        draw.mVertexColorMode = 0;
                        break;
                }
                draw.mUseVertexColor = draw.mVertexColorMode != 0;
            }
            const auto vectorUniform = [&](NativeUniform name, std::array<float, 4>& value) {
                if (const auto* uniform = RenderAssets::nativeUniform(state, name))
                {
                    osg::Vec4f data;
                    if (uniform->get(data))
                        std::copy_n(data.ptr(), 4, value.begin());
                }
            };
            vectorUniform(NativeUniform::MaterialDiffuse, draw.mDiffuse);
            vectorUniform(NativeUniform::MaterialAmbient, draw.mAmbient);
            vectorUniform(NativeUniform::MaterialEmission, draw.mEmission);
            vectorUniform(NativeUniform::MaterialSpecular, draw.mSpecular);
            vectorUniform(NativeUniform::SunPosition, draw.mSunPosition);
            vectorUniform(NativeUniform::SunAmbient, draw.mSunAmbient);
            vectorUniform(NativeUniform::SunDiffuse, draw.mSunDiffuse);
            vectorUniform(NativeUniform::SunSpecular, draw.mSunSpecular);
            if (const auto* uniform = RenderAssets::nativeUniform(state, NativeUniform::MaterialShininess))
                uniform->get(draw.mShininess);
            if (const auto* uniform = RenderAssets::nativeUniform(state, NativeUniform::MaterialVertexColorMode))
            {
                int mode = 0;
                if (uniform->get(mode) && mode >= 0 && mode <= 5)
                {
                    draw.mVertexColorMode = mode;
                    draw.mUseVertexColor = mode != 0;
                }
            }
            if (const auto* uniform = RenderAssets::nativeUniform(state, NativeUniform::MaterialEmissiveMult))
                uniform->get(draw.mEmissiveMult);
            if (const auto* uniform = RenderAssets::nativeUniform(state, NativeUniform::MaterialSpecStrength))
                uniform->get(draw.mSpecStrength);
            draw.mClassicFalloff = Settings::shaders().mClassicFalloff;
            if (state.getMode(GL_BLEND) & osg::StateAttribute::ON)
            {
                const auto* blend
                    = dynamic_cast<const osg::BlendFunc*>(state.getAttribute(osg::StateAttribute::BLENDFUNC));
                draw.mBlend = blend && blend->getDestinationRGB() == GL_ONE ? Render::BlendMode::Additive
                                                                            : Render::BlendMode::Alpha;
                if (blend && blend->getSourceRGB() == GL_ONE && blend->getDestinationRGB() == GL_ONE_MINUS_SRC_ALPHA)
                    draw.mBlend = Render::BlendMode::PremultipliedAlpha;
                if (blend && blend->getDestinationRGB() == GL_ZERO)
                    draw.mBlend = Render::BlendMode::WeightedReplace;
                draw.mDepthWrite = false;
            }
            if (const auto* depth = dynamic_cast<const osg::Depth*>(state.getAttribute(osg::StateAttribute::DEPTH)))
            {
                draw.mDepthWrite = depth->getWriteMask();
                draw.mDepthCompare = alphaComparison(depth->getFunction());
            }
            if (const auto mode = state.getMode(GL_DEPTH_TEST); !(mode & osg::StateAttribute::INHERIT))
                draw.mDepthTest = (mode & osg::StateAttribute::ON) != 0;
            if (const auto* alpha
                = dynamic_cast<const osg::AlphaFunc*>(state.getAttribute(osg::StateAttribute::ALPHAFUNC));
                alpha
                && ((state.getMode(GL_ALPHA_TEST) & osg::StateAttribute::ON)
                    || dynamic_cast<const Shader::RemovedAlphaFunc*>(alpha)))
            {
                draw.mAlphaCompare = alphaComparison(alpha->getFunction());
                draw.mAlphaCutoff = alpha->getReferenceValue();
                if (const auto* threshold = RenderAssets::nativeUniform(state, NativeUniform::AlphaRef))
                    threshold->get(draw.mAlphaCutoff);
            }
            for (const auto name : { NativeUniform::Alpha, NativeUniform::ActorFade })
                if (const auto* uniform = RenderAssets::nativeUniform(state, name))
                {
                    float value = 1;
                    if (uniform->get(value))
                        draw.mTint[3] *= value;
                }
            if (const auto* start = RenderAssets::nativeUniform(state, NativeUniform::FogStart))
                start->get(draw.mFogStart);
            if (const auto* end = RenderAssets::nativeUniform(state, NativeUniform::FogEnd))
                end->get(draw.mFogEnd);
            if (const auto* color = RenderAssets::nativeUniform(state, NativeUniform::FogColor))
            {
                osg::Vec4f fog;
                if (color->get(fog))
                    std::copy_n(fog.ptr(), 4, draw.mFogColor.begin());
            }
        }

        struct State
        {
            RenderAssets::OsgStateView mState;
            Render::Draw mDraw;
            std::shared_ptr<const Render::LightSet> mLights;
            unsigned mUVUnit = 0;
            bool mLightsPrepared = false;
            bool mShadowsPrepared = false;
            std::uint64_t mDrawFrame = 0;
            std::uint64_t mFrame = 0;
        };

        void prepareDraw(State& cached)
        {
            const auto& resolved = cached.mState;
            auto& draw = cached.mDraw;
            applyState(draw, resolved);
            cached.mLightsPrepared = false;
            cached.mShadowsPrepared = false;
            cached.mLights.reset();
            cached.mUVUnit = 0;
            bool water = false;
            if (const auto* surface = RenderAssets::nativeUniform(resolved, NativeUniform::WaterSurface))
                surface->get(water);
            draw.mWater = water;
            if (water)
            {
                draw.mOpacity = std::clamp(Fallback::Map::getFloat("Water_World_Alpha"), 0.f, 1.f);
                if (const auto unit = RenderAssets::textureUnit(resolved, "reflectionMap"))
                {
                    draw.mMaskTexture = texture(dynamic_cast<const osg::Texture2D*>(
                                                    resolved.getTextureAttribute(*unit, osg::StateAttribute::TEXTURE)),
                        false);
                    draw.mReflection = static_cast<bool>(draw.mMaskTexture);
                }
            }
            const auto diffuseUnit = RenderAssets::textureUnit(resolved, water ? "normalMap" : "diffuseMap");
            if (diffuseUnit)
            {
                auto* image = dynamic_cast<const osg::Texture2D*>(
                    resolved.getTextureAttribute(*diffuseUnit, osg::StateAttribute::TEXTURE));
                draw.mTexture = texture(image, false);
                cached.mUVUnit = *diffuseUnit;
                draw.mUVTransform = RenderAssets::textureTransform(resolved, cached.mUVUnit);
                if (water)
                {
                    const auto time = mViewer.getFrameStamp()->getSimulationTime();
                    osg::Vec3f position;
                    if (const auto* node = RenderAssets::nativeUniform(resolved, NativeUniform::NodePosition))
                        node->get(position);
                    // Water uses world-space wave coordinates, not the
                    // simple-water texture coordinates on the huge grid.
                    draw.mEmission = { position.x(), position.y(), static_cast<float>(std::fmod(time, 200.0)), 1 };
                    draw.mBlend = Render::BlendMode::Alpha;
                    draw.mDepthWrite = false;
                    // The legacy shader outputs alpha=1 after its own
                    // reflection/refraction mix. Our Fresnel blend weight
                    // is not an authored alpha-test value.
                    draw.mAlphaCompare = Render::AlphaCompare::Always;
                }
            }
            if (const auto blendUnit
                = water ? std::optional<unsigned>{} : RenderAssets::textureUnit(resolved, "blendMap"))
            {
                auto* mask = dynamic_cast<const osg::Texture2D*>(
                    resolved.getTextureAttribute(*blendUnit, osg::StateAttribute::TEXTURE));
                draw.mMaskTexture = texture(mask, false);
                draw.mMaskUVTransform = RenderAssets::textureTransform(resolved, *blendUnit);
            }
            draw.mDoubleSided = !(resolved.getMode(GL_CULL_FACE) & osg::StateAttribute::ON);
            // The cull result already contains the complete transform.
            // Match its effective GL face convention; do not compensate
            // for the determinant again on this live-scene bridge.
            if (const auto* face
                = dynamic_cast<const osg::FrontFace*>(resolved.getAttribute(osg::StateAttribute::FRONTFACE)))
                draw.mFlipWinding = face->getMode() == osg::FrontFace::CLOCKWISE;
            if (const auto* cull
                = dynamic_cast<const osg::CullFace*>(resolved.getAttribute(osg::StateAttribute::CULLFACE));
                cull && cull->getMode() == osg::CullFace::FRONT)
                draw.mFlipWinding = !draw.mFlipWinding;
        }

        struct MeshLookupHash
        {
            std::size_t operator()(const std::pair<const osg::Geometry*, unsigned>& key) const
            {
                auto value = reinterpret_cast<std::uintptr_t>(key.first) >> 4;
                value ^= value >> 17;
                return value ^ (std::size_t(key.second) * 0x9e3779b9u);
            }
        };

        using TransformKey = std::tuple<const osg::Matrixd*, const osg::Matrixd*, const osg::Uniform*, bool>;
        struct TransformHash
        {
            std::size_t operator()(const TransformKey& key) const
            {
                std::size_t value = 0;
                const auto add = [&](const void* pointer) {
                    value
                        ^= (reinterpret_cast<std::uintptr_t>(pointer) >> 4) + 0x9e3779b9u + (value << 6) + (value >> 2);
                };
                add(std::get<0>(key));
                add(std::get<1>(key));
                add(std::get<2>(key));
                value ^= static_cast<std::size_t>(std::get<3>(key)) * 0x9e3779b9u;
                return value;
            }
        };
        struct Transform
        {
            osg::Matrixd mProjection;
            Render::Matrix mClip{}, mView{}, mNormal = Render::IdentityMatrix;
            bool mInvertible = false;
        };
        const Transform& transform(const osgUtil::RenderLeaf& leaf, const RenderAssets::OsgStateView& state)
        {
            auto [entry, inserted] = mTransforms.getOrCreate({ leaf._modelview.get(), leaf._projection.get(),
                state.getUniform(NativeUniform::ProjectionMatrix), mShadowCasting });
            if (inserted)
            {
                ++mPrepare.mTransforms;
                if (mShadowCasting)
                {
                    osg::Matrixd depth;
                    depth(2, 2) = .5;
                    depth(3, 2) = .5;
                    entry.mProjection = *leaf._projection * depth;
                }
                else
                    entry.mProjection = RenderAssets::clipProjection(
                        state, *leaf._projection, SceneUtil::AutoDepth::isReversed(), true);
                entry.mClip = matrix(*leaf._modelview * entry.mProjection);
                entry.mView = matrix(*leaf._modelview);
                entry.mNormal = Render::IdentityMatrix;
                osg::Matrixd inverse;
                entry.mInvertible = inverse.invert(*leaf._modelview);
                if (entry.mInvertible)
                {
                    osg::Matrixd normal;
                    normal.transpose(inverse);
                    entry.mNormal = matrix(normal);
                }
            }
            return entry;
        }

        State& state(osgUtil::StateGraph* graph)
        {
            if (!graph)
                return mDefaultState;
            auto& entry = mStates[graph];
            if (entry.mFrame != mSerial)
            {
                const auto& parent = state(graph->_parent).mState;
                entry.mState.bind(&parent, graph->getStateSet());
                entry.mState.prepareNativeState();
                ++mPrepare.mStateBindings;
                entry.mFrame = mSerial;
            }
            return entry;
        }

        struct ParticleRecord
        {
            std::unique_ptr<GpuMesh> mMesh;
            std::vector<Render::Vertex> mVertices;
            std::vector<std::uint32_t> mIndices;
            RenderAssets::ParticleOrder mOrder;
        };

        void particles(const osgParticle::ParticleSystem& source, const osgUtil::RenderLeaf& leaf, Render::Draw& draw)
        {
            PrepareTimer timer{ mPrepare.mParticles };
            auto& record = mParticleMeshes[{ &source, mCurrentStage, mParticleOccurrences[&source]++ }];
            if (!record.mMesh || record.mMesh->mSource.get() != &source)
            {
                record = {};
                record.mMesh = std::make_unique<GpuMesh>(backend(), mMeshResidency);
                record.mMesh->mSource = &source;
            }
            record.mMesh->mSeen = mSerial;
            if (const auto* nif = dynamic_cast<const NifOsg::ParticleSystem*>(&source))
                nif->markRendered(mViewer.getFrameStamp()->getFrameNumber());
            draw.mIndexCount = RenderAssets::importParticles(
                source, *leaf._modelview, record.mVertices, record.mIndices, record.mOrder);
            mPrepare.mParticleWait += record.mOrder.mLockWaitMs;
            if (!draw.mIndexCount)
                return;
            record.mMesh->upload(record.mVertices, record.mIndices, true, std::size_t(draw.mIndexCount / 6) * 4);
            ++mMeshChanges;
            draw.mMesh = record.mMesh->mHandle;
            draw.mDoubleSided = true;
        }

        struct CompositeRecord
        {
            std::weak_ptr<Terrain::CompositeMap> mSource;
            std::uint64_t mQueued = 0;
            bool mReady = false;
            std::uint64_t mPreparedFrame = 0;
        };

        void planComposite(const std::shared_ptr<Terrain::CompositeMap>& source)
        {
            if (!source || !source->mTexture)
                return;
            auto& record = mComposites[source.get()];
            if (record.mSource.lock() != source)
                record = { source, 0, false, 0 };
            if (record.mReady || record.mPreparedFrame == mSerial)
                return;
            record.mPreparedFrame = mSerial;
            for (const auto& drawable : source->mDrawables)
            {
                if (!drawable)
                    continue;
                const auto* geometry = drawable->asGeometry();
                const auto* state = drawable->getStateSet();
                if (geometry && geometry->getVertexArray() && state)
                    planMesh(*geometry, RenderAssets::textureUnit(*state, "diffuseMap").value_or(0));
            }
        }

        void bakeComposite(const std::shared_ptr<Terrain::CompositeMap>& source)
        {
            if (!source || !source->mTexture)
                return;
            auto& record = mComposites[source.get()];
            if (record.mSource.lock() != source)
                record = { source, 0, false, 0 };
            if (record.mReady || record.mQueued == mSerial)
                return;
            if (mCompositeViewCount == mCompositeViews.size())
                mCompositeViews.emplace_back();
            auto& view = mCompositeViews[mCompositeViewCount];
            view.mDraws.clear();
            view.mTarget = target(*source->mTexture);
            view.mColor = { 0, 0, 0, 1 };
            view.mViewport = {};
            view.mClear = true;
            for (const auto& drawable : source->mDrawables)
            {
                if (!drawable)
                    continue;
                const auto* geometry = drawable->asGeometry();
                const auto* state = drawable->getStateSet();
                if (!geometry || !state)
                    throw std::invalid_argument("Terrain composite requires textured geometry");
                Render::Draw draw{};
                applyState(draw, *state);
                const auto diffuseUnit = RenderAssets::textureUnit(*state, "diffuseMap");
                const auto unit = diffuseUnit.value_or(0);
                draw.mTexture = texture(
                    dynamic_cast<const osg::Texture2D*>(state->getTextureAttribute(unit, osg::StateAttribute::TEXTURE)),
                    false);
                draw.mUVTransform = RenderAssets::textureTransform(*state, unit);
                if (const auto blendUnit = RenderAssets::textureUnit(*state, "blendMap"))
                {
                    draw.mMaskTexture = texture(dynamic_cast<const osg::Texture2D*>(state->getTextureAttribute(
                                                    *blendUnit, osg::StateAttribute::TEXTURE)),
                        false);
                    draw.mMaskUVTransform = RenderAssets::textureTransform(*state, *blendUnit);
                }
                draw.mMesh = mesh(*geometry, unit);
                if (!draw.mMesh)
                    throw std::invalid_argument("Terrain composite mesh could not be imported");
                // Composite quads are already in clip space. They bake only
                // authored albedo/weights; world lighting is applied later.
                draw.mClipFromModel = Render::IdentityMatrix;
                draw.mDepthTest = false;
                draw.mDepthWrite = false;
                draw.mDoubleSided = true;
                draw.mLighting = false;
                draw.mFogEnd = 0;
                view.mDraws.push_back(std::move(draw));
            }
            record.mQueued = mSerial;
            ++mCompositeViewCount;
            mSubmittedComposites.push_back(source);
        }

        void leaf(const osgUtil::RenderLeaf& leaf, std::vector<Render::Draw>& draws, bool sky,
            State* preparedState = nullptr, GpuMesh* preparedMesh = nullptr)
        {
            const auto* geometry = leaf.getDrawable()->asGeometry();
            const auto* particleSystem = dynamic_cast<const osgParticle::ParticleSystem*>(leaf.getDrawable());
            if ((!geometry || !geometry->getVertexArray()) && !particleSystem)
                return;
            auto& cached = preparedState ? *preparedState : state(leaf._parent);
            const auto& resolved = cached.mState;
            if (const auto* mask
                = dynamic_cast<const osg::ColorMask*>(resolved.getAttribute(osg::StateAttribute::COLORMASK));
                !mShadowCasting && mask && !mask->getRedMask() && !mask->getGreenMask() && !mask->getBlueMask()
                && !mask->getAlphaMask())
                return; // GL occlusion-query proxies have no visible output.
            try
            {
                if (const auto* terrain = dynamic_cast<const Terrain::TerrainDrawable*>(geometry))
                    bakeComposite(terrain->getCompositeMap());
                if (cached.mDrawFrame != mSerial)
                {
                    cached.mDraw = {};
                    prepareDraw(cached);
                    cached.mDrawFrame = mSerial;
                }
                Render::Draw draw = cached.mDraw;
                const unsigned uvUnit = cached.mUVUnit;
                const bool water = draw.mWater;
                if (mClipPlane && (resolved.getMode(GL_CLIP_PLANE0) & osg::StateAttribute::ON))
                {
                    draw.mClipPlane = *mClipPlane;
                    draw.mClip = true;
                }
                if (particleSystem)
                    particles(*particleSystem, leaf, draw);
                else if (preparedMesh && preparedMesh->mSource.get() == geometry && preparedMesh->mPreparedUV == uvUnit)
                    draw.mMesh = mesh(*preparedMesh);
                else
                    draw.mMesh = mesh(*geometry, uvUnit);
                if (!draw.mMesh)
                    return;
                // OSG uses row vectors. Transposing its product is exactly the
                // column-major matrix consumed by the native vertex pipeline.
                const auto& transforms = transform(leaf, resolved);
                const auto& projection = transforms.mProjection;
                if (water)
                    draw.mWaterProjection = RenderAssets::depthCoefficients(projection, true);
                if (mProjectionOutput && !mProjectionOutput->has_value() && !sky)
                {
                    // The native effect API uses reverse, zero-to-one depth.
                    *mProjectionOutput = projection;
                }
                draw.mClipFromModel = particleSystem ? matrix(projection) : transforms.mClip;
                draw.mViewFromModel = particleSystem ? Render::IdentityMatrix : transforms.mView;
                draw.mNormalFromModel = transforms.mNormal;
                draw.mDepthProjection = RenderAssets::depthCoefficients(projection, true);
                if (transforms.mInvertible)
                {
                    if (geometry && geometry->getNormalArray() && !sky && !water)
                    {
                        if (!cached.mLightsPrepared)
                        {
                            cached.mLights = lights(resolved);
                            cached.mLightsPrepared = true;
                        }
                        draw.mLights = cached.mLights;
                        draw.mLighting
                            = RenderAssets::nativeUniform(resolved, NativeUniform::SunDiffuse) || draw.mLights;
                    }
                }
                if (particleSystem)
                {
                    draw.mDoubleSided = true;
                    draw.mLighting = false;
                    draw.mDepthWrite = false;
                    draw.mVertexColorMode = 4;
                    draw.mUseVertexColor = true;
                }
                draw.mSky = sky;
                draw.mShadows.reset();
                if (!mShadowCasting && !sky && !water && !particleSystem && draw.mLighting)
                {
                    if (!cached.mShadowsPrepared)
                    {
                        cached.mDraw.mShadows = shadows(resolved);
                        cached.mShadowsPrepared = true;
                    }
                    draw.mShadows = cached.mDraw.mShadows;
                    if (draw.mShadows && draw.mShadows->mMaximumDistance > 0)
                    {
                        // Receiver geometry wholly beyond the fade range uses
                        // the unshadowed variant, without shadow varyings/work.
                        const auto& bound = geometry->getBound();
                        const auto& view = *leaf._modelview;
                        const auto center = osg::Vec3d(bound.center()) * view;
                        const double radius = bound.radius()
                            * std::sqrt(view(0, 2) * view(0, 2) + view(1, 2) * view(1, 2) + view(2, 2) * view(2, 2));
                        if (bound.valid() && -center.z() - radius >= draw.mShadows->mMaximumDistance)
                            draw.mShadows.reset();
                    }
                }
                if (sky)
                {
                    draw.mDepthWrite = false;
                    draw.mFogEnd = 0;
                    draw.mLighting = false;
                    if (const auto* pass = RenderAssets::nativeUniform(resolved, NativeUniform::Pass))
                        pass->get(draw.mSkyPass);
                    if (draw.mSkyPass == 5)
                        return; // An occlusion query proxy has no visible color.
                    if (const auto* opacity = RenderAssets::nativeUniform(resolved, NativeUniform::Opacity))
                        opacity->get(draw.mOpacity);
                    const auto skyVector = [&](NativeUniform name, std::array<float, 4>& output) {
                        if (const auto* uniform = resolved.getUniform(name))
                        {
                            osg::Vec4f value;
                            if (uniform->get(value))
                                std::copy_n(value.ptr(), 4, output.begin());
                        }
                    };
                    skyVector(NativeUniform::DiffuseColor, draw.mDiffuse);
                    skyVector(NativeUniform::MoonBlend, draw.mEmission);
                    skyVector(NativeUniform::AtmosphereFade, draw.mAmbient);
                    if (draw.mSkyPass == 3)
                        if (const auto unit = RenderAssets::textureUnit(resolved, "maskMap"))
                            draw.mMaskTexture
                                = texture(dynamic_cast<const osg::Texture2D*>(
                                              resolved.getTextureAttribute(*unit, osg::StateAttribute::TEXTURE)),
                                    false);
                }
                if (mShadowCasting)
                {
                    draw.mShadowCasting = true;
                    draw.mDepthBiasConstant = Settings::shadows().mPolygonOffsetUnits;
                    draw.mDepthBiasSlope = Settings::shadows().mPolygonOffsetFactor;
                    draw.mLighting = false;
                    draw.mShadows.reset();
                    draw.mSky = draw.mWater = draw.mDistortion = false;
                    draw.mDepthTest = draw.mDepthWrite = true;
                    draw.mDepthCompare = Render::AlphaCompare::LessEqual;
                    draw.mBlend = Render::BlendMode::Opaque;
                    draw.mFogEnd = 0;
                    bool alphaTest = false;
                    if (const auto* uniform = resolved.getUniform("alphaTestShadows"))
                        uniform->get(alphaTest);
                    draw.mShadowAlphaTest = alphaTest;
                }
                draws.push_back(std::move(draw));
            }
            catch (const std::invalid_argument& error)
            {
                // Unsupported diagnostic primitives must not abort a loaded
                // game. Record each reason once, without spamming every frame.
                if (mWarnings.insert(error.what()).second)
                    Log(Debug::Warning) << "Vulkan geometry: " << error.what();
            }
        }

        struct LeafRequest
        {
            const osgUtil::RenderLeaf* mLeaf;
            State* mState;
            GpuMesh* mMesh;
            bool mSky, mProjection, mDistortion = false;
        };
        struct DrawPlan
        {
            std::vector<LeafRequest> mLeaves;
            Render::DrawRanges mRanges;
            void clear()
            {
                mLeaves.clear();
                mRanges.clear();
            }
        };

        void queueLeaf(const osgUtil::RenderLeaf& leaf, DrawPlan& plan, bool sky)
        {
            const auto* geometry = leaf.getDrawable()->asGeometry();
            const auto* particle = dynamic_cast<const osgParticle::ParticleSystem*>(leaf.getDrawable());
            if ((!geometry || !geometry->getVertexArray()) && !particle)
                return;
            auto& cached = state(leaf._parent);
            const auto& resolved = cached.mState;
            if (const auto* mask
                = dynamic_cast<const osg::ColorMask*>(resolved.getAttribute(osg::StateAttribute::COLORMASK));
                !mShadowCasting && mask && !mask->getRedMask() && !mask->getGreenMask() && !mask->getBlueMask()
                && !mask->getAlphaMask())
                return;
            GpuMesh* mesh = nullptr;
            if (geometry && !particle)
            {
                try
                {
                    if (const auto* terrain = dynamic_cast<const Terrain::TerrainDrawable*>(geometry))
                        planComposite(terrain->getCompositeMap());
                    bool water = false;
                    if (const auto* uniform = resolved.getUniform(NativeUniform::WaterSurface))
                        uniform->get(water);
                    const auto uv = RenderAssets::textureUnit(resolved, water ? "normalMap" : "diffuseMap").value_or(0);
                    mesh = &planMesh(*geometry, uv);
                }
                catch (const std::invalid_argument&)
                {
                    // The ordinary leaf path publishes the existing diagnostic.
                }
            }
            Render::reserveFor(plan.mLeaves, plan.mLeaves.size() + 1);
            plan.mLeaves.push_back({ &leaf, &cached, mesh, sky, mProjectionOutput != nullptr });
        }

        void bin(osgUtil::RenderBin& bin, DrawPlan& plan, bool parentSky = false)
        {
            if (bin.getBinNum() == RenderBin_Distortion && mDistortionOutput)
            {
                auto* output = mDistortionOutput;
                mDistortionOutput = nullptr;
                auto* projection = mProjectionOutput;
                mProjectionOutput = nullptr;
                const auto begin = output->mLeaves.size();
                this->bin(bin, *output, parentSky);
                for (std::size_t i = begin; i < output->mLeaves.size(); ++i)
                    output->mLeaves[i].mDistortion = true;
                mDistortionOutput = output;
                mProjectionOutput = projection;
                return;
            }
            auto* projection = mProjectionOutput;
            if (bin.getBinNum() == RenderBin_FirstPerson)
                mProjectionOutput = nullptr;
            const bool sky = parentSky || bin.getBinNum() == RenderBin_Sky;
            const auto first = plan.mLeaves.size();
            auto next = bin.getRenderBinList().begin();
            for (; next != bin.getRenderBinList().end() && next->first < 0; ++next)
                this->bin(*next->second, plan, sky);
            for (const auto& entry : bin.getRenderLeafList())
                queueLeaf(*entry, plan, sky);
            for (const auto& graph : bin.getStateGraphList())
                for (const auto& entry : graph->_leaves)
                    queueLeaf(*entry, plan, sky);
            for (; next != bin.getRenderBinList().end(); ++next)
                this->bin(*next->second, plan, sky);
            if (!mShadowCasting && bin.getBinNum() == RenderBin_FirstPerson && plan.mLeaves.size() != first)
            {
                plan.mRanges.clearDepth(first, plan.mLeaves.size());
            }
            mProjectionOutput = projection;
        }

        struct CapturedView
        {
            std::vector<Render::Draw> mDraws;
            std::vector<Render::Draw> mDistortion;
            DrawPlan mScenePlan, mDistortionPlan;
            const osgUtil::RenderStage* mStage = nullptr;
            std::optional<std::array<float, 4>> mClip;
            std::optional<osg::Matrixd> mProjection;
            Render::TextureHandle mTarget;
            std::array<float, 4> mColor{};
            std::array<std::uint32_t, 4> mViewport{};
            bool mClear = true;
            bool mReversedDepth = true;
        };

        void assemble(DrawPlan& plan, std::vector<Render::Draw>& draws, CapturedView& view)
        {
            Render::reserveFor(draws, plan.mLeaves.size());
            plan.mRanges.begin(plan.mLeaves.size(), draws.size());
            for (std::size_t i = 0; i < plan.mLeaves.size(); ++i)
            {
                const auto& request = plan.mLeaves[i];
                mProjectionOutput = request.mProjection ? &view.mProjection : nullptr;
                const auto begin = draws.size();
                leaf(*request.mLeaf, draws, request.mSky, request.mState, request.mMesh);
                if (request.mDistortion)
                    for (auto j = begin; j < draws.size(); ++j)
                    {
                        draws[j].mDistortion = true;
                        draws[j].mLighting = false;
                        draws[j].mFogEnd = 0;
                        draws[j].mDepthWrite = false;
                    }
                plan.mRanges.emitted(i, draws.size());
            }
            plan.mRanges.apply([&](std::size_t draw) { draws[draw].mClearDepthBefore = true; });
        }

        void prepareViews()
        {
            {
                PrepareTimer timer{ mPrepare.mMeshes };
                // Start large mod meshes early instead of leaving one large
                // conversion on a worker after every other task has finished.
                if (mMeshWorkers.workerCount() && mMeshJobs.size() >= 8)
                    std::sort(mMeshJobs.begin(), mMeshJobs.end(),
                        [](const auto* left, const auto* right) { return left->mWorkSize > right->mWorkSize; });
                mMeshWorkers.run(
                    mMeshJobs.size(),
                    [](void* context, std::size_t index) {
                        auto& owner = *static_cast<Impl*>(context);
                        prepareMesh(*owner.mMeshJobs[index]);
                    },
                    this);
            }
            for (std::size_t i = 0; i < mViewCount; ++i)
            {
                auto& view = mViews[i];
                mCurrentStage = view.mStage;
                mParticleOccurrences.clear();
                mClipPlane = view.mClip;
                mShadowCasting = !view.mReversedDepth;
                assemble(view.mScenePlan, view.mDraws, view);
                assemble(view.mDistortionPlan, view.mDistortion, view);
                std::erase_if(view.mDraws, [&](const auto& draw) {
                    return view.mTarget && (draw.mTexture == view.mTarget || draw.mMaskTexture == view.mTarget);
                });
                mShadowCasting = false;
                mProjectionOutput = nullptr;
            }
            mClipPlane.reset();
        }

        void captureEffects(osgUtil::RenderBin& bin)
        {
            const auto capture = [&](const osgUtil::RenderLeaf& leaf) {
                if (auto* canvas = dynamic_cast<MWRender::PingPongCanvas*>(leaf._drawable.get()))
                {
                    mEffectCanvas = canvas;
                    mEffectState = state(leaf._parent).mState;
                }
            };
            for (const auto& entry : bin.getRenderLeafList())
                capture(*entry);
            for (const auto& graph : bin.getStateGraphList())
                for (const auto& entry : graph->_leaves)
                    capture(*entry);
            for (auto& [order, child] : bin.getRenderBinList())
                captureEffects(*child);
        }

        void captureStage(osgUtil::RenderStage& stage, bool window = false)
        {
            // OSG also guards RenderStage::draw against repeated references.
            // Keep legitimate terrain material passes; deduplicate only cameras.
            if (std::find(mCapturedStages.begin(), mCapturedStages.end(), &stage) != mCapturedStages.end())
                return;
            mCapturedStages.push_back(&stage);
            for (const auto& [order, child] : stage.getPreRenderList())
                captureStage(*child);
            auto* camera = stage.getCamera();
            osg::Texture2D* colorTarget = nullptr;
            bool shadow = false;
            if (camera && !window)
            {
                const auto& attachments = camera->getBufferAttachmentMap();
                auto color = attachments.find(osg::Camera::COLOR_BUFFER);
                if (color == attachments.end())
                    color = attachments.find(osg::Camera::COLOR_BUFFER0);
                if (color != attachments.end())
                    colorTarget = dynamic_cast<osg::Texture2D*>(color->second._texture.get());
                if (!colorTarget)
                    if (const auto depth = attachments.find(osg::Camera::DEPTH_BUFFER); depth != attachments.end())
                    {
                        colorTarget = dynamic_cast<osg::Texture2D*>(depth->second._texture.get());
                        shadow = colorTarget && colorTarget->getShadowComparison();
                        if (!shadow)
                            colorTarget = nullptr;
                    }
            }
            if (!window && !colorTarget)
                captureEffects(stage);
            if (window || colorTarget)
            {
                if (mViewCount == mViews.size())
                    mViews.emplace_back();
                auto& view = mViews[mViewCount++];
                view.mDraws.clear();
                view.mTarget = {};
                view.mViewport = {};
                const auto color = stage.getClearColor();
                view.mColor = { color.r(), color.g(), color.b(), color.a() };
                view.mReversedDepth = !shadow;
                view.mClear = shadow || (stage.getClearMask() & GL_COLOR_BUFFER_BIT) != 0;
                if (shadow)
                    view.mColor = { 1, 1, 1, 1 };
                const auto* viewport = stage.getViewport();
                if (viewport)
                    view.mViewport = { static_cast<unsigned>(std::max(0., viewport->x())),
                        static_cast<unsigned>(std::max(0., viewport->y())),
                        static_cast<unsigned>(std::max(0., viewport->width())),
                        static_cast<unsigned>(std::max(0., viewport->height())) };
                if (colorTarget)
                    view.mTarget = target(
                        *colorTarget, view.mViewport[0] + view.mViewport[2], view.mViewport[1] + view.mViewport[3]);
                view.mStage = &stage;
                mClipPlane.reset();
                for (const auto& [attribute, transform] : stage.getPositionalStateContainer()->getAttrMatrixList())
                    if (const auto* clip = dynamic_cast<const osg::ClipPlane*>(attribute.get());
                        clip && clip->getClipPlaneNum() == 0)
                    {
                        osg::Plane plane(clip->getClipPlane());
                        if (transform)
                            plane.transform(*transform);
                        std::array<float, 4> value;
                        std::copy_n(plane.ptr(), 4, value.begin());
                        mClipPlane = value;
                    }
                view.mClip = mClipPlane;
                view.mScenePlan.clear();
                view.mDistortionPlan.clear();
                view.mDistortion.clear();
                view.mProjection.reset();
                mProjectionOutput = shadow ? nullptr : &view.mProjection;
                mDistortionOutput = shadow ? nullptr : &view.mDistortionPlan;
                mShadowCasting = shadow;
                bin(stage, view.mScenePlan);
                mShadowCasting = false;
                mDistortionOutput = nullptr;
                mProjectionOutput = nullptr;
            }
            for (const auto& [order, child] : stage.getPostRenderList())
                captureStage(*child);
        }

        struct FrameTiming
        {
            using Clock = std::chrono::steady_clock;
            Clock::time_point mStart = Clock::now();
            Clock::time_point mPhase = mStart;
            std::array<double, 5> mMilliseconds{};
            void finish(unsigned index)
            {
                const auto now = Clock::now();
                mMilliseconds[index] = std::chrono::duration<double, std::milli>(now - mPhase).count();
                mPhase = now;
            }
        };

        void reportSlowFrame(const FrameTiming& timing)
        {
            const auto now = FrameTiming::Clock::now();
            const double total = std::chrono::duration<double, std::milli>(now - timing.mStart).count();
            const double outside = mLastFrameEnd == FrameTiming::Clock::time_point{}
                ? 0
                : std::chrono::duration<double, std::milli>(timing.mStart - mLastFrameEnd).count();
            const double sinceSubmit = mLastSubmit == FrameTiming::Clock::time_point{}
                ? 0
                : std::chrono::duration<double, std::milli>(now - mLastSubmit).count();
            const bool waiting
                = !mFrameSubmitted && sinceSubmit >= 250 && !(SDL_GetWindowFlags(mWindow) & SDL_WINDOW_MINIMIZED);
            if ((total >= 250 || timing.mMilliseconds[2] >= 32 || outside >= 250 || waiting)
                && now >= mNextSlowFrameReport)
            {
                mNextSlowFrameReport = now + std::chrono::seconds(5);
                const auto& phases = timing.mMilliseconds;
                Log(Debug::Warning) << "Vulkan slow frame: renderer=" << total << "ms, outside=" << outside
                                    << "ms, ready=" << phases[0] << "ms, cull=" << phases[1]
                                    << "ms, prepare=" << phases[2] << "ms, submit=" << phases[3]
                                    << "ms, retire=" << phases[4] << "ms, since submit=" << sinceSubmit
                                    << "ms, last submitted draws=" << mLastSubmittedDraws << ", views=" << mViewCount
                                    << ", composites=" << mCompositeViewCount << ", mesh uploads=" << mMeshChanges
                                    << ", image uploads=" << mImageChanges << ", retirement cache=" << mRetireName
                                    << ", retired=" << mRetired << ", prepare scene=" << mPrepare.mScene
                                    << "ms effects=" << mPrepare.mEffects << "ms overlay=" << mPrepare.mOverlay
                                    << "ms (nested mesh=" << mPrepare.mMeshes << "ms texture=" << mPrepare.mTextures
                                    << "ms target=" << mPrepare.mTargets << "ms), mesh builds=" << mPrepare.mMeshBuilds
                                    << ", stream updates=" << mPrepare.mMeshUpdates
                                    << ", particles=" << mPrepare.mParticles << "ms (lock=" << mPrepare.mParticleWait
                                    << "ms), native states=" << mPrepare.mStateBindings
                                    << ", transforms=" << mPrepare.mTransforms;
            }
            mLastFrameEnd = FrameTiming::Clock::now();
        }

        void render()
        {
            if (mStopped)
                return;
            FrameTiming timing;
            struct Report
            {
                Impl& mOwner;
                const FrameTiming& mTiming;
                ~Report() { mOwner.reportSlowFrame(mTiming); }
            } report{ *this, timing };
            struct ClearPlans
            {
                Impl& mOwner;
                ~ClearPlans() { mOwner.clearMeshPlans(); }
            } clear{ *this };
            mFrameSubmitted = false;
            mMeshChanges = 0;
            mImageChanges = 0;
            mPrepare = {};
            mViewCount = 0;
            mCompositeViewCount = 0;
            const bool ready = backend().ready();
            timing.finish(0);
            if (!ready)
                return;
            ++mSerial;
            mTransforms.beginFrame();
            mShadowSetsUsed = 0;
            mMeshResidency.beginFrame();
            mTextureResidency.beginFrame();
            mSubmittedComposites.clear();
            mCapturedStages.clear();
            mEffectCanvas = nullptr;
            mEffectState = {};
            mPasses.clear();
            mGuiDraws.clear();
            auto* gui = MyGUIPlatform::RenderManager::getInstancePtr();
            struct ResetSink
            {
                MyGUIPlatform::RenderManager* mGui;
                ~ResetSink()
                {
                    if (mGui)
                        mGui->setDrawSink(nullptr);
                }
            } reset{ gui };
            if (gui)
                gui->setDrawSink(this);
            if (mStats)
                mStats->update(*mViewer.getViewerStats(), mMeshes.size(), mTextures.size(), mMeshResidency.bytes(),
                    mTextureResidency.bytes());
            auto* camera = mViewer.getCamera();
            mSceneView->setFrameStamp(mViewer.getFrameStamp());
            mSceneView->setGlobalStateSet(camera->getStateSet());
            mSceneView->inheritCullSettings(*camera);
            mSceneView->setCullMask(camera->getCullMask());
            mSceneView->getCullVisitor()->setDatabaseRequestHandler(mViewer.getDatabasePager());
            mSceneView->getCullVisitor()->setImageRequestHandler(mViewer.getImagePager());
            mSceneView->cull();
            auto* stage = mSceneView->getRenderStage();
            // SceneView::cull has already sorted the stage and its children.
            timing.finish(1);
            // GUI uploads can occur during culling. Keep the nested asset
            // counters below scoped to preparation, matching its parent time.
            mPrepare = {};
            {
                PrepareTimer timer{ mPrepare.mScene };
                captureStage(*stage, true);
                prepareViews();
                Render::reserveFor(mPasses, mCompositeViewCount + mViewCount);
                for (std::size_t i = 0; i < mCompositeViewCount; ++i)
                {
                    const auto& view = mCompositeViews[i];
                    mPasses.push_back({ view.mDraws, view.mTarget, view.mColor, view.mClear, view.mViewport });
                }
                for (std::size_t i = 0; i < mViewCount; ++i)
                {
                    const auto& view = mViews[i];
                    mPasses.push_back({ view.mDraws, view.mTarget, view.mColor, view.mClear, view.mViewport });
                    mPasses.back().mReversedDepth = view.mReversedDepth;
                }
            }
            if (mEffectCanvas.valid())
            {
                PrepareTimer timer{ mPrepare.mEffects };
                if (!mEffects)
                    mEffects = std::make_unique<Vulkan::Effects>(
                        backend(), [this](const osg::Texture2D* source) { return texture(source, false); });
                std::span<const Render::Draw> distortion;
                // Sky-only views have no opaque leaf from which to capture a
                // projection. Their fallback must use the same native depth
                // convention as ordinary scene and effect passes.
                osg::Matrixd projection = RenderAssets::clipProjection(
                    RenderAssets::OsgState{}, camera->getProjectionMatrix(), false, true);
                for (std::size_t i = 0; i < mViewCount; ++i)
                    if (!mViews[i].mTarget)
                    {
                        distortion = mViews[i].mDistortion;
                        projection = mViews[i].mProjection.value_or(projection);
                        break;
                    }
                mEffects->append(
                    mPasses, *mEffectCanvas, mEffectState, projection, camera->getViewMatrix(), distortion);
            }
            // Interface colors and text remain outside the scene effects.
            {
                PrepareTimer timer{ mPrepare.mOverlay };
                if (mDrawGui && !mGuiDraws.empty())
                    mPasses.push_back({ mGuiDraws, {}, {}, false });
                if (mStats && mDrawGui)
                {
                    const auto graph = mStats->graph();
                    if (!graph.empty())
                        mPasses.push_back({ graph, {}, {}, false });
                }
            }
            // Release CPU references held by views that disappeared this frame.
            {
                PrepareTimer timer{ mPrepare.mScene };
                for (std::size_t i = mViewCount; i < mViews.size(); ++i)
                    mViews[i].mDraws.clear();
                for (std::size_t i = mCompositeViewCount; i < mCompositeViews.size(); ++i)
                    mCompositeViews[i].mDraws.clear();
                clearMeshPlans();
            }
            timing.finish(2);
            if (mStats)
                mStats->prepared(timing.mMilliseconds[2], mPrepare);
            const bool submitted = backend().renderPasses(mPasses);
            timing.finish(3);
            if (submitted)
            {
                mFrameSubmitted = true;
                if (mEffects)
                    mEffects->submitted();
                mLastSubmit = FrameTiming::Clock::now();
                mLastSubmittedDraws = 0;
                for (const auto& pass : mPasses)
                    mLastSubmittedDraws += pass.mDraws.size();
                for (const auto& composite : mSubmittedComposites)
                {
                    mComposites.at(composite.get()).mReady = true;
                    composite->mCompiled = composite->mDrawables.size();
                    composite->mDrawables.clear();
                }
            }
            const auto deadline = FrameTiming::Clock::now() + std::chrono::milliseconds(1);
            const auto stop = [&] { return FrameTiming::Clock::now() >= deadline; };
            const auto expired = [&](const auto& item) { return item.second->mSeen + 16 < mSerial; };
            switch (mRetireCategory++ % 11)
            {
                case 10:
                    mRetireName = "image variants";
                    mRetired = mImageVariants.sweep(stop);
                    break;
                case 0:
                    mRetireName = "meshes";
                    mRetired = mMeshSweep.sweep(
                        mMeshes,
                        [&](const auto& item) {
                            return mMeshResidency.evict(item.second->mSource.valid(), mSerial, item.second->mSeen);
                        },
                        stop);
                    break;
                case 1:
                    mRetireName = "gui";
                    mRetired = mGuiSweep.sweep(mGuiMeshes, expired, stop);
                    break;
                case 2:
                    mRetireName = "particles";
                    mRetired = mParticleSweep.sweep(
                        mParticleMeshes, [&](const auto& item) { return item.second.mMesh->mSeen + 16 < mSerial; },
                        stop);
                    break;
                case 3:
                    mRetireName = "textures";
                    mRetired = mTextureSweep.sweep(
                        mTextures,
                        [&](const auto& item) {
                            const auto& texture = *item.second;
                            osg::ref_ptr<const osg::Image> source;
                            if (!texture.mImage.lock(source) || texture.mRevision != source->getModifiedCount())
                                return true; // Stale animated-image generations cannot be reused.
                            return mTextureResidency.evict(true, mSerial, texture.mSeen)
                                || (texture.mStorage
                                    && texture.mSeen + (mTextures.size() > 16384 ? 32 : 600) < mSerial);
                        },
                        stop);
                    break;
                case 4:
                    mRetireName = "targets";
                    mRetired = mTargetSweep.sweep(
                        mTargets, [](const auto& item) { return !item.second.mSource.valid(); }, stop);
                    break;
                case 5:
                    mRetireName = "lights";
                    mRetired = mLightSweep.sweep(
                        mLightSets,
                        [&](const auto& item) {
                            return !item.second.mSource.valid()
                                || item.second.mSeen + (mLightSets.size() > 2048 ? 16 : 600) < mSerial;
                        },
                        stop);
                    break;
                case 6:
                    mRetireName = "states";
                    mRetired = mStateSweep.sweep(
                        mStates,
                        [&](const auto& item) {
                            // Returning to the same direction should reuse the
                            // CPU state records too, not only GPU resources.
                            constexpr auto limit = 32 * 1024 * 1024 / sizeof(State);
                            return item.second.mFrame + (mStates.size() > limit ? 16 : 600) < mSerial;
                        },
                        stop);
                    break;
                case 9:
                    mRetireName = "shared meshes";
                    mRetired = mSharedMeshSweep.sweep(
                        mSharedMeshes,
                        [&](const auto& item) {
                            const auto& storage = item.second.mStorage;
                            return !storage || !item.second.mKey.current()
                                || (storage.use_count() == 1 && mMeshResidency.evict(true, mSerial, storage->mSeen));
                        },
                        stop);
                    break;
                case 8:
                    mRetireName = "texture bindings";
                    mRetired = mBindingSweep.sweep(
                        mTextureBindings,
                        [&](const auto& item) {
                            return !item.second.mSource.valid()
                                || std::max(item.second.mFrame[0], item.second.mFrame[1])
                                    + (mTextureBindings.size() > 8192 ? 16 : 600)
                                < mSerial;
                        },
                        stop);
                    break;
                case 7:
                    mRetireName = "composites";
                    mRetired = mCompositeSweep.sweep(
                        mComposites, [](const auto& item) { return item.second.mSource.expired(); }, stop);
                    break;
            }
            timing.finish(4);
            if (mFrameSubmitted && mStats)
            {
                const auto now = FrameTiming::Clock::now();
                const double interval = mStatsLastSubmit.time_since_epoch().count()
                    ? std::chrono::duration<double, std::milli>(now - mStatsLastSubmit).count()
                    : 0;
                mStats->submitted(interval, timing.mMilliseconds, mLastSubmittedDraws, mMeshChanges, mImageChanges);
                mStatsLastSubmit = now;
            }
        }

        osgViewer::Viewer& mViewer;
        SDL_Window* mWindow;
        // Module outlives every cache containing its resource handles.
        std::unique_ptr<Render::BackendModule> mModule;
        osg::ref_ptr<NativeRenderer> mRenderer;
        osg::ref_ptr<osgUtil::SceneView> mSceneView;
        SceneUtil::ParallelWork mMeshWorkers;
        std::vector<GpuMesh*> mMeshPlans, mMeshJobs;
        Render::Residency mMeshResidency{ 64 * 1024 * 1024 };
        Render::Residency mTextureResidency{ 256 * 1024 * 1024 };
        RenderAssets::OsgImporter mImporter;
        std::unique_ptr<Vulkan::Effects> mEffects;
        std::unique_ptr<Vulkan::StatsOverlay> mStats;
        std::chrono::steady_clock::time_point mStatsLastSubmit{};
        osg::observer_ptr<MWRender::PingPongCanvas> mEffectCanvas;
        RenderAssets::OsgStateView mEffectState;
        std::map<std::vector<std::uintptr_t>, SharedMesh> mSharedMeshes;
        std::unordered_map<std::pair<const osg::Geometry*, unsigned>, std::unique_ptr<GpuMesh>, MeshLookupHash> mMeshes;
        std::unordered_map<const osg::Array*, std::unique_ptr<GpuMesh>> mGuiMeshes;
        std::map<std::tuple<const osgParticle::ParticleSystem*, const osgUtil::RenderStage*, unsigned>, ParticleRecord>
            mParticleMeshes;
        std::map<const osgParticle::ParticleSystem*, unsigned> mParticleOccurrences;
        const osgUtil::RenderStage* mCurrentStage = nullptr;
        std::map<std::tuple<std::uintptr_t, Render::SamplerState>, std::shared_ptr<GpuTexture>> mTextures;
        RenderAssets::ImageVariants<GpuTexture> mImageVariants;
        struct TextureBinding
        {
            osg::observer_ptr<const osg::Texture2D> mSource;
            std::array<std::uint64_t, 2> mFrame{};
            std::array<Render::TextureHandle, 2> mHandles{};
        };
        std::unordered_map<const osg::Texture2D*, TextureBinding> mTextureBindings;
        std::unordered_map<osgUtil::StateGraph*, State> mStates;
        Render::FrameCache<TransformKey, Transform, TransformHash> mTransforms;
        struct LightRecord
        {
            osg::observer_ptr<const osg::Uniform> mSource;
            unsigned mRevision = 0;
            int mCount = 0;
            std::shared_ptr<Render::LightSet> mLights;
            std::uint64_t mSeen = 0;
        };
        std::unordered_map<const osg::Uniform*, LightRecord> mLightSets;
        State mDefaultState;
        Render::TextureHandle mTransparent;
        std::vector<CapturedView> mViews;
        std::vector<CapturedView> mCompositeViews;
        std::size_t mCompositeViewCount = 0;
        std::map<const Terrain::CompositeMap*, CompositeRecord> mComposites;
        std::vector<std::shared_ptr<Terrain::CompositeMap>> mSubmittedComposites;
        std::vector<const osgUtil::RenderStage*> mCapturedStages;
        std::size_t mViewCount = 0;
        std::vector<Render::Pass> mPasses;
        std::map<const osg::Texture2D*, TargetRecord> mTargets;
        std::vector<Render::Draw> mGuiDraws;
        DrawPlan* mDistortionOutput = nullptr;
        std::optional<osg::Matrixd>* mProjectionOutput = nullptr;
        std::vector<Render::TextureLevel> mLevels;
        std::vector<Render::Vertex> mGuiVertices;
        std::vector<std::uint32_t> mGuiIndices;
        std::set<std::string> mWarnings;
        std::uint64_t mSerial = 0;
        std::size_t mMeshChanges = 0;
        std::size_t mImageChanges = 0;
        Vulkan::PrepareStats mPrepare;
        bool mFrameSubmitted = false;
        std::size_t mLastSubmittedDraws = 0;
        FrameTiming::Clock::time_point mLastSubmit{};
        FrameTiming::Clock::time_point mLastFrameEnd{};
        FrameTiming::Clock::time_point mNextSlowFrameReport{};
        Render::CacheSweep<decltype(mMeshes)> mMeshSweep;
        Render::CacheSweep<decltype(mSharedMeshes)> mSharedMeshSweep;
        Render::CacheSweep<decltype(mGuiMeshes)> mGuiSweep;
        Render::CacheSweep<decltype(mParticleMeshes)> mParticleSweep;
        Render::CacheSweep<decltype(mTextures)> mTextureSweep;
        Render::CacheSweep<decltype(mTextureBindings)> mBindingSweep;
        Render::CacheSweep<decltype(mTargets)> mTargetSweep;
        Render::CacheSweep<decltype(mLightSets)> mLightSweep;
        Render::CacheSweep<decltype(mStates)> mStateSweep;
        Render::CacheSweep<decltype(mComposites)> mCompositeSweep;
        unsigned mRetireCategory = 0;
        const char* mRetireName = "none";
        std::size_t mRetired = 0;
        bool mStopped = false;
        bool mDrawGui = true;
        std::optional<std::array<float, 4>> mClipPlane;
    };

    VulkanFrameDriver::VulkanFrameDriver(osgViewer::Viewer& viewer, const Render::BackendOptions& options)
        : mImpl(std::make_unique<Impl>(viewer, options))
    {
    }
    VulkanFrameDriver::~VulkanFrameDriver() = default;
    bool VulkanFrameDriver::isDone() const
    {
        return mImpl->mViewer.done();
    }
    void VulkanFrameDriver::advance(double time)
    {
        mImpl->mViewer.advance(time);
    }
    unsigned VulkanFrameDriver::frameNumber() const
    {
        return mImpl->mViewer.getFrameStamp()->getFrameNumber();
    }
    void VulkanFrameDriver::processEvents()
    {
        mImpl->mViewer.eventTraversal();
    }
    void VulkanFrameDriver::updateScene()
    {
        mImpl->mViewer.updateTraversal();
    }
    void VulkanFrameDriver::renderFrame()
    {
        mImpl->mViewer.renderingTraversals();
    }
    void VulkanFrameDriver::stop()
    {
        if (mImpl->mStopped)
            return;
        mImpl->mStopped = true;
        if (mImpl->mEffects)
            mImpl->mEffects->stop();
        if (mImpl->mStats)
            mImpl->mStats->shutdown();
        mImpl->mViewer.stopThreading();
    }
}
