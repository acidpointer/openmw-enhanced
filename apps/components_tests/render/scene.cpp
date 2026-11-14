#include <components/render/math.hpp>
#include <components/render/scene.hpp>
#include <components/render/texture.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <set>

namespace
{
    class Backend final : public Render::Backend
    {
    public:
        Render::DeviceInfo mInfo;
        unsigned mCreatedMeshes = 0, mReleasedMeshes = 0, mCreatedTextures = 0, mReleasedTextures = 0;
        bool mCompressed = true, mFailTextures = false;
        std::vector<std::vector<std::byte>> mTextureLevels;
        std::vector<Render::Draw> mLastDraws;
        const Render::DeviceInfo& deviceInfo() const override { return mInfo; }
        bool supportsTextureFormat(Render::TextureFormat format, bool) const override
        {
            return format == Render::TextureFormat::RGBA8 || mCompressed;
        }
        Render::MeshHandle createMesh(std::span<const Render::Vertex>, std::span<const std::uint32_t>) override
        {
            const auto index = mCreatedMeshes++;
            mMeshes.insert(index);
            return { index, 1 };
        }
        Render::TextureHandle createTexture(const Render::TextureData& data, const Render::SamplerState&) override
        {
            if (mFailTextures)
                throw std::runtime_error("Texture allocation failed");
            Render::validateTexture(data);
            mTextureLevels.clear();
            for (const auto& level : data.mLevels)
                mTextureLevels.emplace_back(level.mBytes.begin(), level.mBytes.end());
            const auto index = mCreatedTextures++;
            mTextures.insert(index);
            return { index, 1 };
        }
        void releaseMesh(Render::MeshHandle handle) override
        {
            EXPECT_EQ(mMeshes.erase(handle.mIndex), 1u);
            ++mReleasedMeshes;
        }
        void releaseTexture(Render::TextureHandle handle) override
        {
            EXPECT_EQ(mTextures.erase(handle.mIndex), 1u);
            ++mReleasedTextures;
        }
        bool render(std::span<const Render::Draw> draws, const std::array<float, 4>&) override
        {
            for (const auto& draw : draws)
            {
                EXPECT_TRUE(mMeshes.contains(draw.mMesh.mIndex));
                if (draw.mTexture)
                {
                    EXPECT_TRUE(mTextures.contains(draw.mTexture.mIndex));
                }
            }
            mLastDraws.assign(draws.begin(), draws.end());
            return true;
        }

    private:
        std::set<std::uint32_t> mMeshes, mTextures;
    };

    std::shared_ptr<Render::ModelAsset> model(bool textured = false)
    {
        auto mesh = std::make_shared<Render::MeshAsset>();
        mesh->mVertices
            = { { { -0.1f, -0.1f, 0.25f }, {} }, { { 0.1f, -0.1f, 0.25f }, {} }, { { 0, 0.1f, 0.25f }, {} } };
        mesh->mIndices = { 0, 1, 2 };
        mesh->mBounds = { { -0.1f, -0.1f, 0.25f }, { 0.1f, 0.1f, 0.25f } };
        auto result = std::make_shared<Render::ModelAsset>();
        result->mBounds = mesh->mBounds;
        result->mParts.push_back({ mesh, {} });
        if (textured)
        {
            auto image = std::make_shared<Render::ImageAsset>();
            image->mLevels.push_back({ 1, 1, std::vector<std::byte>(4, std::byte{ 255 }) });
            auto texture = std::make_shared<Render::TextureAsset>();
            texture->mImage = image;
            result->mParts.front().mMaterial.mDiffuse = texture;
        }
        return result;
    }

    Render::Matrix translation(float x, float y = 0, float z = 0)
    {
        auto result = Render::IdentityMatrix;
        result[12] = x;
        result[13] = y;
        result[14] = z;
        return result;
    }

    TEST(RenderScene, InstancesShareUploadsAndTransformsDoNotRecreateResources)
    {
        Backend backend;
        Render::Scene scene(backend);
        auto source = model(true);
        const auto first = scene.insert(source);
        const auto second = scene.insert(source, translation(0.3f));
        EXPECT_EQ(backend.mCreatedMeshes, 1u);
        EXPECT_EQ(backend.mCreatedTextures, 1u);
        scene.setTransform(first, translation(-0.3f));
        EXPECT_TRUE(scene.render({}));
        ASSERT_EQ(backend.mLastDraws.size(), 2u);
        EXPECT_EQ(backend.mLastDraws[0].mMesh, backend.mLastDraws[1].mMesh);
        EXPECT_EQ(backend.mCreatedMeshes, 1u);
        EXPECT_EQ(backend.mCreatedTextures, 1u);
        scene.remove(first);
        EXPECT_EQ(backend.mReleasedMeshes, 0u);
        scene.remove(second);
        EXPECT_EQ(backend.mReleasedMeshes, 1u);
        EXPECT_EQ(backend.mReleasedTextures, 1u);
        EXPECT_THROW(scene.setTransform(first, Render::IdentityMatrix), std::invalid_argument);
    }

    TEST(RenderScene, DifferentModelsCanShareMeshAndImageSamplerResources)
    {
        Backend backend;
        Render::Scene scene(backend);
        auto first = model(true);
        auto second = std::make_shared<Render::ModelAsset>(*first);
        second->mParts[0].mMaterial.mDiffuse
            = std::make_shared<Render::TextureAsset>(*first->mParts[0].mMaterial.mDiffuse);
        const auto a = scene.insert(first);
        const auto b = scene.insert(second);
        EXPECT_EQ(backend.mCreatedMeshes, 1u);
        EXPECT_EQ(backend.mCreatedTextures, 1u);
        scene.remove(a);
        EXPECT_EQ(backend.mReleasedTextures, 0u);
        scene.remove(b);
        EXPECT_EQ(backend.mReleasedTextures, 1u);
    }

    TEST(RenderScene, ViewVisibilityDoesNotLeakIntoAnotherCamera)
    {
        Backend backend;
        Render::Scene scene(backend);
        const auto instance = scene.insert(model(), translation(4));
        EXPECT_TRUE(scene.prepare({}).empty());
        Render::View other;
        other.mClipFromWorld = translation(-4);
        ASSERT_EQ(scene.prepare(other).size(), 1u);
        EXPECT_TRUE(scene.prepare({}).empty());
        scene.setTransform(instance, Render::IdentityMatrix);
        ASSERT_EQ(scene.prepare({}).size(), 1u);
        scene.setMask(instance, 2);
        Render::View hidden;
        hidden.mMask = 1;
        EXPECT_TRUE(scene.prepare(hidden).empty());
        scene.setVisible(instance, false);
        EXPECT_TRUE(scene.prepare({}).empty());
    }

    TEST(RenderScene, UsesZeroToOneNearClipAndPreservesMirroredWinding)
    {
        Backend backend;
        Render::Scene scene(backend);
        const auto instance = scene.insert(model(), translation(0, 0, -0.5f));
        EXPECT_TRUE(scene.prepare({}).empty());
        auto mirrored = Render::IdentityMatrix;
        mirrored[0] = -1;
        scene.setTransform(instance, mirrored);
        const auto draws = scene.prepare({});
        ASSERT_EQ(draws.size(), 1u);
        EXPECT_TRUE(draws[0].mFlipWinding);
    }

    TEST(RenderScene, BlendedPartsAreSortedBackToFrontAndKeepAlphaPolicy)
    {
        Backend backend;
        Render::Scene scene(backend);
        auto source = model();
        source->mParts[0].mMaterial.mBlend = Render::BlendMode::Alpha;
        source->mParts[0].mMaterial.mAlphaCompare = Render::AlphaCompare::Greater;
        source->mParts[0].mMaterial.mAlphaCutoff = 0.5f;
        scene.insert(source);
        scene.insert(source, translation(0, 0, 0.4f));
        const auto draws = scene.prepare({});
        ASSERT_EQ(draws.size(), 2u);
        EXPECT_GT(draws[0].mClipFromModel[14], draws[1].mClipFromModel[14]);
        EXPECT_EQ(draws[0].mAlphaCompare, Render::AlphaCompare::Greater);
        EXPECT_FLOAT_EQ(draws[0].mAlphaCutoff, 0.5f);
    }

    TEST(RenderScene, PartialUploadFailureReleasesMeshesAndDoesNotPoisonTheCache)
    {
        Backend backend;
        Render::Scene scene(backend);
        auto source = model(true);
        backend.mFailTextures = true;
        EXPECT_THROW(scene.insert(source), std::runtime_error);
        EXPECT_EQ(backend.mCreatedMeshes, backend.mReleasedMeshes);
        EXPECT_TRUE(scene.prepare({}).empty());
        backend.mFailTextures = false;
        EXPECT_NO_THROW(scene.insert(source));
        EXPECT_EQ(scene.prepare({}).size(), 1u);
    }

    TEST(RenderScene, TransparencyUsesViewDepthInsteadOfRadialDistance)
    {
        Backend backend;
        Render::Scene scene(backend);
        auto source = model();
        source->mParts[0].mMaterial.mBlend = Render::BlendMode::Alpha;
        scene.insert(source, translation(0.8f, 0, 0.1f));
        scene.insert(source, translation(0, 0, 0.5f));
        const auto draws = scene.prepare({});
        ASSERT_EQ(draws.size(), 2u);
        EXPECT_FLOAT_EQ(draws[0].mClipFromModel[12], 0);
        EXPECT_FLOAT_EQ(draws[0].mClipFromModel[14], 0.5f);
    }

    TEST(RenderScene, DecodesCompressedImagesOnlyWhenTheDeviceNeedsAFallback)
    {
        Backend backend;
        Render::Scene scene(backend);
        auto source = model();
        auto rgba = std::make_shared<Render::ImageAsset>();
        rgba->mLevels.push_back({ 1, 1, std::vector<std::byte>(4) });
        auto compressed = std::make_shared<Render::ImageAsset>();
        compressed->mFormat = Render::TextureFormat::BC1RGBA;
        compressed->mLevels.push_back({ 1, 1, std::vector<std::byte>(8) });
        unsigned decodes = 0;
        compressed->mDecode = [&] {
            ++decodes;
            return rgba;
        };
        auto texture = std::make_shared<Render::TextureAsset>();
        texture->mImage = compressed;
        source->mParts[0].mMaterial.mDiffuse = texture;
        const auto native = scene.insert(source);
        EXPECT_EQ(decodes, 0u);
        scene.remove(native);
        backend.mCompressed = false;
        scene.insert(source);
        scene.insert(source);
        EXPECT_EQ(decodes, 1u);
        ASSERT_EQ(backend.mTextureLevels.size(), 1u);
        EXPECT_EQ(backend.mTextureLevels[0].size(), 4u);
    }

    TEST(RenderScene, RejectsNonfiniteStateBeforeItEntersResourceCacheKeys)
    {
        Backend backend;
        Render::Scene scene(backend);
        auto source = model(true);
        auto texture = std::make_shared<Render::TextureAsset>(*source->mParts[0].mMaterial.mDiffuse);
        texture->mSampler.mAnisotropy = std::numeric_limits<float>::quiet_NaN();
        source->mParts[0].mMaterial.mDiffuse = texture;
        EXPECT_THROW(scene.insert(source), std::invalid_argument);
        EXPECT_EQ(backend.mCreatedMeshes, backend.mReleasedMeshes);
    }
}
