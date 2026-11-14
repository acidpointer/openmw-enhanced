#ifndef OPENMW_COMPONENTS_RENDERASSETS_MODEL_H
#define OPENMW_COMPONENTS_RENDERASSETS_MODEL_H

#include <components/render/backend.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Render
{
    using Matrix = std::array<float, 16>;
    inline constexpr Matrix IdentityMatrix{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

    struct Bounds
    {
        std::array<float, 3> mMin;
        std::array<float, 3> mMax;
    };

    struct ImageLevel
    {
        std::uint32_t mWidth;
        std::uint32_t mHeight;
        std::vector<std::byte> mBytes;
    };

    struct ImageAsset
    {
        std::string mName;
        TextureFormat mFormat = TextureFormat::RGBA8;
        bool mSrgb = false;
        bool mGenerateMipmaps = false;
        std::vector<ImageLevel> mLevels;
        // CPU fallback for devices without the source compression format.
        std::shared_ptr<const ImageAsset> mUncompressed;
        std::function<std::shared_ptr<const ImageAsset>()> mDecode;
    };

    struct TextureAsset
    {
        std::shared_ptr<const ImageAsset> mImage;
        SamplerState mSampler;
    };

    struct MaterialAsset
    {
        std::shared_ptr<const TextureAsset> mDiffuse;
        std::array<float, 4> mTint{ 1, 1, 1, 1 };
        std::array<float, 4> mAmbient{ 1, 1, 1, 1 };
        std::array<float, 4> mSpecular{ 0, 0, 0, 0 };
        std::array<float, 4> mEmission{ 0, 0, 0, 1 };
        float mShininess = 0;
        BlendMode mBlend = BlendMode::Opaque;
        AlphaCompare mAlphaCompare = AlphaCompare::Always;
        float mAlphaCutoff = 0;
        bool mDoubleSided = false;
        bool mSortTransparent = true;
        bool mUseVertexColor = true;
        std::uint32_t mVertexColorMode = 4;
    };

    struct MeshAsset
    {
        std::vector<Vertex> mVertices;
        std::vector<std::uint32_t> mIndices;
        std::vector<std::array<float, 3>> mNormals;
        Bounds mBounds;
    };

    struct ModelPart
    {
        std::shared_ptr<const MeshAsset> mMesh;
        MaterialAsset mMaterial;
        Matrix mModelFromLocal = IdentityMatrix;
    };

    struct ModelAsset
    {
        std::string mName;
        Bounds mBounds;
        std::vector<ModelPart> mParts;
    };
}

#endif
