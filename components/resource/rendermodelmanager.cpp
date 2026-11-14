#include "rendermodelmanager.hpp"

#include "imagemanager.hpp"
#include "scenemanager.hpp"

#include <osg/Stats>

namespace Resource
{
    RenderModelManager::RenderModelManager(
        const VFS::Manager* vfs, SceneManager& scenes, ImageManager& images, double expiryDelay)
        : ResourceManager(vfs, expiryDelay)
        , mScenes(scenes)
        , mImages(images)
        // The update-only bit marks hidden collision and controller nodes in
        // game assets. It must not make them visible in the static render asset.
        , mImporter(~std::uint32_t{ 1 })
    {
    }

    std::shared_ptr<const Render::ModelAsset> RenderModelManager::get(VFS::Path::NormalizedView path)
    {
        if (auto existing = mCache->getRefFromObjectCache(path))
            return existing;
        const auto raw = mScenes.readModel(path, mImages);
        if (!raw)
            throw std::runtime_error("Model reader returned no CPU model data");
        auto model = mImporter.import(*raw, path.value());
        mCache->addEntryToObjectCache(path.value(), model);
        return model;
    }

    void RenderModelManager::clearCache()
    {
        ResourceManager::clearCache();
        mImporter.clear();
    }

    void RenderModelManager::reportStats(unsigned frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Render model", frameNumber, mCache->getStats(), *stats);
    }
}
