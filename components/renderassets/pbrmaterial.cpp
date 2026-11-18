#include "pbrmaterial.hpp"

#include <components/debug/debuglog.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/vfs/manager.hpp>

#include <yaml-cpp/yaml.h>

#include <array>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace RenderAssets
{
    PbrMaterial::PbrMaterial(const PbrMaterial& source, const osg::CopyOp& copy)
        : StateAttribute(source, copy)
        , mSource(source.mSource)
        , mProperties(source.mProperties)
        , mRoughness(source.mRoughness)
        , mMetalness(source.mMetalness)
    {
    }

    int PbrMaterial::compare(const osg::StateAttribute& other) const
    {
        COMPARE_StateAttribute_Types(PbrMaterial, other);
        COMPARE_StateAttribute_Parameter(mSource);
        COMPARE_StateAttribute_Parameter(mRoughness);
        COMPARE_StateAttribute_Parameter(mMetalness);
        if (mProperties && rhs.mProperties)
            return mProperties->compare(*rhs.mProperties);
        return static_cast<int>(mProperties.valid()) - static_cast<int>(rhs.mProperties.valid());
    }

    std::optional<PbrMaterialCache::Definition> PbrMaterialCache::read(
        const std::string& name, const Resource::ImageManager& images)
    {
        const VFS::Path::Normalized sidecar(name + ".pbr.yaml");
        try
        {
            auto file = images.getVFS()->find(sidecar);
            if (!file)
                return {};
            std::array<char, 4097> contents{};
            file->read(contents.data(), contents.size());
            if (file->bad() || file->gcount() == contents.size())
                throw std::runtime_error("material document exceeds 4096 bytes or could not be read");
            const auto document = YAML::Load(std::string(contents.data(), file->gcount()));
            if (!document.IsMap() || !document["version"] || document["version"].as<int>() != 1)
                throw std::runtime_error("expected PBR material version 1");
            for (const auto& entry : document)
            {
                const auto key = entry.first.as<std::string>();
                if (key != "version" && key != "roughness" && key != "metalness" && key != "properties")
                    throw std::runtime_error("unknown PBR material field: " + key);
            }
            const auto scalar = [&](std::string_view field, float fallback) {
                const auto node = document[std::string(field)];
                const float value = node ? node.as<float>() : fallback;
                if (node && (!std::isfinite(value) || value < 0 || value > 1))
                    throw std::runtime_error("PBR material factors must be finite values in [0, 1]");
                return value;
            };
            Definition material;
            material.mRoughness = scalar("roughness", -1.f);
            material.mMetalness = scalar("metalness", 0.f);
            if (const auto properties = document["properties"])
            {
                const VFS::Path::Normalized path(properties.as<std::string>());
                if (!images.getVFS()->exists(path))
                    throw std::runtime_error("missing packed PBR properties texture");
                material.mProperties = path.value();
            }
            return material;
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "Vulkan PBR material " << sidecar.value() << ": " << e.what();
            return {};
        }
    }

    osg::ref_ptr<PbrMaterial> PbrMaterialCache::get(const osg::Texture& base, Resource::ImageManager& images)
    {
        const auto* source = base.getImage(0);
        if (!source || source->getFileName().empty())
            return {};
        const auto& name = source->getFileName();
        const auto definition = [&]() {
            std::uint64_t generation;
            {
                const std::lock_guard lock(mMutex);
                if (const auto found = mDefinitions.find(name); found != mDefinitions.end())
                    return found->second;
                generation = mGeneration;
            }
            auto loaded = read(name, images);
            const std::lock_guard lock(mMutex);
            if (generation != mGeneration)
                return loaded;
            return mDefinitions.try_emplace(name, std::move(loaded)).first->second;
        }();
        if (!definition)
            return {};
        osg::ref_ptr<PbrMaterial> material = new PbrMaterial;
        material->mSource = name;
        material->mRoughness = definition->mRoughness;
        material->mMetalness = definition->mMetalness;
        if (!definition->mProperties.empty())
        {
            auto image = images.getImage(VFS::Path::Normalized(definition->mProperties));
            if (image && image != images.getWarningImage())
            {
                material->mProperties = new osg::Texture2D(image);
                material->mProperties->setWrap(osg::Texture::WRAP_S, base.getWrap(osg::Texture::WRAP_S));
                material->mProperties->setWrap(osg::Texture::WRAP_T, base.getWrap(osg::Texture::WRAP_T));
                material->mProperties->setFilter(osg::Texture::MIN_FILTER, base.getFilter(osg::Texture::MIN_FILTER));
                material->mProperties->setFilter(osg::Texture::MAG_FILTER, base.getFilter(osg::Texture::MAG_FILTER));
                material->mProperties->setMaxAnisotropy(base.getMaxAnisotropy());
            }
        }
        return material;
    }

    void PbrMaterialCache::clear()
    {
        const std::lock_guard lock(mMutex);
        mDefinitions.clear();
        ++mGeneration;
    }
}
