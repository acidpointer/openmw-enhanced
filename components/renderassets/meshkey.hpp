#ifndef OPENMW_COMPONENTS_RENDERASSETS_MESHKEY_H
#define OPENMW_COMPONENTS_RENDERASSETS_MESHKEY_H

#include <cstdint>
#include <osg/BufferObject>
#include <osg/Geometry>
#include <osg/observer_ptr>
#include <vector>

namespace RenderAssets
{
    struct MeshKey
    {
        std::vector<std::uintptr_t> mWords;
        std::vector<osg::observer_ptr<const osg::BufferData>> mBuffers;
        bool alive() const
        {
            for (const auto& buffer : mBuffers)
                if (!buffer.valid())
                    return false;
            return true;
        }
    };

    // Hierarchy clones often share all vertex/index arrays. Their instance
    // matrices belong to Draw, so those immutable GPU bytes can be shared.
    inline MeshKey meshKey(const osg::Geometry& geometry, unsigned uvUnit)
    {
        MeshKey key;
        key.mWords.reserve(21 + geometry.getNumPrimitiveSets() * 5);
        key.mBuffers.reserve(5 + geometry.getNumPrimitiveSets());
        key.mWords.push_back(uvUnit);
        const auto add = [&](const osg::BufferData* data) {
            key.mWords.push_back(reinterpret_cast<std::uintptr_t>(data));
            key.mWords.push_back(data ? data->getModifiedCount() : 0);
            if (data)
                key.mBuffers.emplace_back(data);
        };
        for (const auto* array : { geometry.getVertexArray(), geometry.getNormalArray(), geometry.getColorArray(),
                 geometry.getTexCoordArray(uvUnit), geometry.getTexCoordArray(0) })
        {
            add(array);
            key.mWords.push_back(array ? array->getNumElements() : 0);
            key.mWords.push_back(array ? array->getBinding() : 0);
        }
        for (const auto& primitive : geometry.getPrimitiveSetList())
        {
            add(primitive);
            key.mWords.push_back(primitive ? primitive->getMode() : 0);
            key.mWords.push_back(primitive ? primitive->getNumIndices() : 0);
            key.mWords.push_back(primitive ? primitive->getNumInstances() : 0);
        }
        return key;
    }
}

#endif
