#ifndef OPENMW_COMPONENTS_RENDERASSETS_PBRMATERIAL_H
#define OPENMW_COMPONENTS_RENDERASSETS_PBRMATERIAL_H

#include <osg/Texture2D>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace Resource
{
    class ImageManager;
}

namespace RenderAssets
{
    // Carries authoring data through inherited OSG state without occupying a
    // legacy texture unit or entering the OpenGL shader selection machinery.
    class PbrMaterial final : public osg::StateAttribute
    {
    public:
        static constexpr auto AttributeType = static_cast<osg::StateAttribute::Type>(101);
        PbrMaterial() = default;
        PbrMaterial(const PbrMaterial& source, const osg::CopyOp& copy);
        META_StateAttribute(RenderAssets, PbrMaterial, AttributeType)
        int compare(const osg::StateAttribute& other) const override;

        std::string mSource;
        osg::ref_ptr<osg::Texture2D> mProperties;
        float mRoughness = -1;
        float mMetalness = 0;
    };

    class PbrMaterialCache
    {
    public:
        osg::ref_ptr<PbrMaterial> get(const osg::Texture& base, Resource::ImageManager& images);
        void clear();

    private:
        struct Definition
        {
            std::string mProperties;
            float mRoughness = -1;
            float mMetalness = 0;
        };
        static std::optional<Definition> read(const std::string& name, const Resource::ImageManager& images);

        std::mutex mMutex;
        std::unordered_map<std::string, std::optional<Definition>> mDefinitions;
        std::uint64_t mGeneration = 0;
    };
}

#endif
