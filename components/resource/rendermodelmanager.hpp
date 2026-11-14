#ifndef OPENMW_COMPONENTS_RESOURCE_RENDERMODELMANAGER_H
#define OPENMW_COMPONENTS_RESOURCE_RENDERMODELMANAGER_H

#include "resourcemanager.hpp"

#include <components/renderassets/osgimporter.hpp>

namespace Resource
{
    class SceneManager;
    class ImageManager;

    class RenderModelManager final : public ResourceManager<std::shared_ptr<const Render::ModelAsset>>
    {
    public:
        RenderModelManager(const VFS::Manager* vfs, SceneManager& scenes, ImageManager& images, double expiryDelay);
        std::shared_ptr<const Render::ModelAsset> get(VFS::Path::NormalizedView path);
        void clearCache() override;
        void reportStats(unsigned frameNumber, osg::Stats* stats) const override;

    private:
        SceneManager& mScenes;
        ImageManager& mImages;
        RenderAssets::OsgImporter mImporter;
    };
}

#endif
