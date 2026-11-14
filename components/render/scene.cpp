#include "scene.hpp"

#include "math.hpp"
#include "reserve.hpp"
#include "resourcepool.hpp"
#include "texture.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

namespace Render
{
    namespace
    {
        struct ResidentMesh
        {
            Backend& mBackend;
            std::shared_ptr<const MeshAsset> mSource;
            MeshHandle mHandle;
            explicit ResidentMesh(Backend& backend, std::shared_ptr<const MeshAsset> source)
                : mBackend(backend)
                , mSource(std::move(source))
                , mHandle(backend.createMesh(mSource->mVertices, mSource->mIndices))
            {
            }
            ~ResidentMesh() { mBackend.releaseMesh(mHandle); }
        };

        struct ResidentTexture
        {
            Backend& mBackend;
            std::shared_ptr<const TextureAsset> mSource;
            TextureHandle mHandle;
            explicit ResidentTexture(Backend& backend, std::shared_ptr<const TextureAsset> source)
                : mBackend(backend)
                , mSource(std::move(source))
            {
                auto image = mSource->mImage;
                if (!image)
                    throw std::invalid_argument("Model texture has no image");
                if (!backend.supportsTextureFormat(image->mFormat, image->mSrgb))
                {
                    image = image->mUncompressed ? image->mUncompressed : image->mDecode ? image->mDecode() : nullptr;
                    if (!image || !backend.supportsTextureFormat(image->mFormat, image->mSrgb))
                        throw std::runtime_error("Model texture format is unsupported and has no compatible fallback");
                }
                std::vector<TextureLevel> levels;
                levels.reserve(image->mLevels.size());
                for (const auto& level : image->mLevels)
                {
                    levels.push_back({ level.mWidth, level.mHeight, level.mBytes });
                    if (!mSource->mSampler.mUseMipmaps)
                        break;
                }
                const TextureData data{ image->mFormat, image->mSrgb,
                    image->mGenerateMipmaps && mSource->mSampler.mUseMipmaps, levels };
                validateTexture(data);
                mHandle = backend.createTexture(data, mSource->mSampler);
            }
            ~ResidentTexture() { mBackend.releaseTexture(mHandle); }
        };

        using TextureKey = std::tuple<std::uintptr_t, SamplerState>;
        TextureKey textureKey(const TextureAsset& texture)
        {
            return { reinterpret_cast<std::uintptr_t>(texture.mImage.get()), texture.mSampler };
        }

        struct ResidentPart
        {
            std::shared_ptr<ResidentMesh> mMesh;
            std::shared_ptr<ResidentTexture> mTexture;
            const ModelPart* mSource;
        };

        struct ResidentModel
        {
            std::shared_ptr<const ModelAsset> mSource;
            std::vector<ResidentPart> mParts;
        };

        struct InstancePart
        {
            Matrix mWorldFromLocal;
            Bounds mWorldBounds;
            bool mFlipWinding;
        };

        struct Instance
        {
            std::shared_ptr<ResidentModel> mModel;
            Bounds mWorldBounds;
            std::vector<InstancePart> mParts;
            std::array<float, 4> mTint{ 1, 1, 1, 1 };
            std::size_t mActiveIndex = 0;
            std::uint32_t mMask = ~std::uint32_t{ 0 };
            bool mVisible = true;

            void setTransform(const Matrix& matrix)
            {
                validateTransform(matrix);
                mParts.resize(mModel->mParts.size());
                mWorldBounds = transformBounds(mModel->mSource->mBounds, matrix);
                for (std::size_t i = 0; i < mParts.size(); ++i)
                {
                    const auto& source = *mModel->mParts[i].mSource;
                    const auto world = multiply(matrix, source.mModelFromLocal);
                    mParts[i] = { world, transformBounds(source.mMesh->mBounds, world), reversesWinding(world) };
                }
            }
        };

        struct Entry
        {
            Draw mDraw;
            double mDepth;
            std::size_t mOrder;
            unsigned mBucket;
        };

        double distanceSquared(const Bounds& bounds, const std::array<float, 3>& eye)
        {
            double result = 0;
            for (std::size_t i = 0; i < 3; ++i)
            {
                const double delta = double(bounds.mMin[i]) * 0.5 + double(bounds.mMax[i]) * 0.5 - eye[i];
                result += delta * delta;
            }
            return std::isfinite(result) ? result : std::numeric_limits<double>::max();
        }

        double viewDepth(const Bounds& bounds, const View& view)
        {
            double depth = 0;
            for (std::size_t i = 0; i < 3; ++i)
                depth
                    += (double(bounds.mMin[i]) * 0.5 + double(bounds.mMax[i]) * 0.5 - view.mEye[i]) * view.mForward[i];
            return std::isfinite(depth) ? depth : std::numeric_limits<double>::max();
        }
    }

    class Scene::Impl
    {
    public:
        explicit Impl(Backend& backend)
            : mBackend(backend)
        {
        }

        void prune(const ModelAsset& model)
        {
            if (const auto found = mModels.find(&model); found != mModels.end() && found->second.expired())
                mModels.erase(found);
            for (const auto& part : model.mParts)
            {
                if (const auto found = mMeshes.find(part.mMesh.get());
                    found != mMeshes.end() && found->second.use_count() <= 1)
                    mMeshes.erase(found);
                if (part.mMaterial.mDiffuse && validSampler(part.mMaterial.mDiffuse->mSampler))
                    if (const auto found = mTextures.find(textureKey(*part.mMaterial.mDiffuse));
                        found != mTextures.end() && found->second.use_count() <= 1)
                        mTextures.erase(found);
            }
        }

        std::shared_ptr<ResidentModel> model(const std::shared_ptr<const ModelAsset>& source)
        {
            if (!source || source->mParts.empty())
                throw std::invalid_argument("Scene instance requires a model with geometry");
            if (const auto found = mModels.find(source.get()); found != mModels.end())
                if (auto existing = found->second.lock())
                    return existing;
            validateBounds(source->mBounds);
            try
            {
                auto model = std::make_shared<ResidentModel>();
                model->mSource = source;
                model->mParts.reserve(source->mParts.size());
                for (const auto& part : source->mParts)
                {
                    if (!part.mMesh)
                        throw std::invalid_argument("Model part has no mesh");
                    const auto& material = part.mMaterial;
                    for (const auto* values :
                        { &material.mTint, &material.mAmbient, &material.mSpecular, &material.mEmission })
                        if (!std::all_of(
                                values->begin(), values->end(), [](float value) { return std::isfinite(value); }))
                            throw std::invalid_argument("Model material contains a nonfinite color");
                    if (!std::isfinite(material.mAlphaCutoff) || !std::isfinite(material.mShininess)
                        || material.mBlend > BlendMode::PremultipliedAlpha
                        || material.mAlphaCompare > AlphaCompare::Greater || material.mVertexColorMode > 5)
                        throw std::invalid_argument("Model material state is invalid");
                    validateBounds(part.mMesh->mBounds);
                    validateTransform(part.mModelFromLocal);
                    auto& mesh = mMeshes[part.mMesh.get()];
                    if (!mesh)
                        mesh = std::make_shared<ResidentMesh>(mBackend, part.mMesh);
                    std::shared_ptr<ResidentTexture> texture;
                    if (part.mMaterial.mDiffuse)
                    {
                        validateSampler(part.mMaterial.mDiffuse->mSampler);
                        auto& cached = mTextures[textureKey(*part.mMaterial.mDiffuse)];
                        if (!cached)
                            cached = std::make_shared<ResidentTexture>(mBackend, part.mMaterial.mDiffuse);
                        texture = cached;
                    }
                    model->mParts.push_back({ mesh, texture, &part });
                }
                mModels[source.get()] = model;
                return model;
            }
            catch (...)
            {
                prune(*source);
                throw;
            }
        }

        Backend& mBackend;
        // Declare caches before instances so instances release their model
        // references before cached GPU resources during destruction.
        std::map<const MeshAsset*, std::shared_ptr<ResidentMesh>> mMeshes;
        std::map<TextureKey, std::shared_ptr<ResidentTexture>> mTextures;
        std::map<const ModelAsset*, std::weak_ptr<ResidentModel>> mModels;
        ResourcePool<Instance, InstanceTag> mInstances;
        std::vector<InstanceHandle> mActive;
        std::vector<Entry> mOpaque;
        std::vector<Entry> mUnsorted;
        std::vector<Entry> mTransparent;
        std::vector<Draw> mDraws;
    };

    Scene::Scene(Backend& backend)
        : mImpl(std::make_unique<Impl>(backend))
    {
    }

    Scene::~Scene() = default;

    InstanceHandle Scene::insert(std::shared_ptr<const ModelAsset> source, const Matrix& matrix)
    {
        validateTransform(matrix);
        auto instance = std::make_shared<Instance>();
        try
        {
            instance->mModel = mImpl->model(source);
            instance->setTransform(matrix);
            instance->mActiveIndex = mImpl->mActive.size();
            reserveFor(mImpl->mActive, mImpl->mActive.size() + 1);
            const auto handle = mImpl->mInstances.insert(instance);
            mImpl->mActive.push_back(handle);
            return handle;
        }
        catch (...)
        {
            instance.reset();
            if (source)
                mImpl->prune(*source);
            throw;
        }
    }

    void Scene::remove(InstanceHandle handle)
    {
        const auto& instance = mImpl->mInstances.get(handle);
        const auto source = instance->mModel->mSource;
        const auto index = instance->mActiveIndex;
        const auto last = mImpl->mActive.back();
        mImpl->mActive[index] = last;
        mImpl->mInstances.get(last)->mActiveIndex = index;
        mImpl->mActive.pop_back();
        mImpl->mInstances.erase(handle);
        mImpl->prune(*source);
    }

    void Scene::setTransform(InstanceHandle handle, const Matrix& matrix)
    {
        mImpl->mInstances.get(handle)->setTransform(matrix);
    }

    void Scene::setVisible(InstanceHandle handle, bool visible)
    {
        mImpl->mInstances.get(handle)->mVisible = visible;
    }
    void Scene::setMask(InstanceHandle handle, std::uint32_t mask)
    {
        mImpl->mInstances.get(handle)->mMask = mask;
    }

    void Scene::setTint(InstanceHandle handle, const std::array<float, 4>& tint)
    {
        if (!std::all_of(tint.begin(), tint.end(), [](float value) { return std::isfinite(value); }))
            throw std::invalid_argument("Instance tint must be finite");
        mImpl->mInstances.get(handle)->mTint = tint;
    }

    std::span<const Draw> Scene::prepare(const View& view)
    {
        if (!std::all_of(
                view.mClearColor.begin(), view.mClearColor.end(), [](float value) { return std::isfinite(value); }))
            throw std::invalid_argument("View clear color must be finite");
        if (!std::all_of(view.mEye.begin(), view.mEye.end(), [](float value) { return std::isfinite(value); }))
            throw std::invalid_argument("View position must be finite");
        if (!std::all_of(view.mForward.begin(), view.mForward.end(), [](float value) { return std::isfinite(value); })
            || std::all_of(view.mForward.begin(), view.mForward.end(), [](float value) { return value == 0; }))
            throw std::invalid_argument("View direction must be finite and nonzero");
        const Frustum frustum(view.mClipFromWorld);
        auto& impl = *mImpl;
        impl.mOpaque.clear();
        impl.mUnsorted.clear();
        impl.mTransparent.clear();
        impl.mDraws.clear();
        std::size_t order = 0;
        for (const auto handle : impl.mActive)
        {
            const auto& instance = *impl.mInstances.get(handle);
            if (!instance.mVisible || !(instance.mMask & view.mMask) || !frustum.intersects(instance.mWorldBounds))
                continue;
            for (std::size_t i = 0; i < instance.mParts.size(); ++i)
            {
                const auto& transformed = instance.mParts[i];
                if (!frustum.intersects(transformed.mWorldBounds))
                    continue;
                const auto& resident = instance.mModel->mParts[i];
                const auto& material = resident.mSource->mMaterial;
                Draw draw{};
                draw.mMesh = resident.mMesh->mHandle;
                draw.mTexture = resident.mTexture ? resident.mTexture->mHandle : TextureHandle{};
                draw.mClipFromModel = multiply(view.mClipFromWorld, transformed.mWorldFromLocal);
                for (std::size_t j = 0; j < 4; ++j)
                    draw.mTint[j] = instance.mTint[j];
                draw.mDiffuse = material.mTint;
                draw.mAmbient = material.mAmbient;
                draw.mEmission = material.mEmission;
                draw.mSpecular = material.mSpecular;
                draw.mShininess = material.mShininess;
                draw.mVertexColorMode = material.mVertexColorMode;
                draw.mBlend = material.mBlend;
                draw.mDepthWrite = material.mBlend == BlendMode::Opaque;
                draw.mAlphaCompare = material.mAlphaCompare;
                draw.mAlphaCutoff = material.mAlphaCutoff;
                draw.mDoubleSided = material.mDoubleSided;
                draw.mFlipWinding = transformed.mFlipWinding;
                draw.mUseVertexColor = material.mUseVertexColor;
                const double distance = distanceSquared(transformed.mWorldBounds, view.mEye);
                const unsigned bucket = static_cast<unsigned>(std::bit_cast<std::uint64_t>(distance) >> 52);
                auto& target = material.mBlend == BlendMode::Opaque ? impl.mOpaque
                    : material.mSortTransparent                     ? impl.mTransparent
                                                                    : impl.mUnsorted;
                target.push_back({ draw, viewDepth(transformed.mWorldBounds, view), order++, bucket });
            }
        }
        std::sort(impl.mOpaque.begin(), impl.mOpaque.end(), [](const Entry& a, const Entry& b) {
            const auto key = [](const Entry& entry) {
                const auto& d = entry.mDraw;
                return std::tuple{ entry.mBucket, d.mDoubleSided, d.mFlipWinding, d.mMesh.mIndex, d.mTexture.mIndex,
                    entry.mOrder };
            };
            return key(a) < key(b);
        });
        std::sort(impl.mTransparent.begin(), impl.mTransparent.end(), [](const Entry& a, const Entry& b) {
            if (a.mDepth != b.mDepth)
                return a.mDepth > b.mDepth;
            return a.mOrder < b.mOrder;
        });
        reserveFor(impl.mDraws, impl.mOpaque.size() + impl.mUnsorted.size() + impl.mTransparent.size());
        for (const auto* list : { &impl.mOpaque, &impl.mUnsorted, &impl.mTransparent })
            for (const auto& entry : *list)
                impl.mDraws.push_back(entry.mDraw);
        return impl.mDraws;
    }

    bool Scene::render(const View& view)
    {
        return mImpl->mBackend.render(prepare(view), view.mClearColor);
    }
}
