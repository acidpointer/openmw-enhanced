#ifndef OPENMW_COMPONENTS_RENDERASSETS_IMAGEBINDING_H
#define OPENMW_COMPONENTS_RENDERASSETS_IMAGEBINDING_H

#include <components/render/backend.hpp>

#include <osg/Image>
#include <osg/observer_ptr>

#include <memory>

namespace RenderAssets
{
    // One texture object's last resolved image/sampler binding. Weak ownership
    // lets residency eviction proceed; identity/revision/sampling changes miss.
    template <class Storage>
    class ImageBinding
    {
    public:
        std::shared_ptr<Storage> find(const osg::Image& source, const Render::SamplerState& sampling) const
        {
            if (mSource.get() != &source || mRevision != source.getModifiedCount() || mSampling != sampling)
                return {};
            return mStorage.lock();
        }

        void store(
            const osg::Image& source, const Render::SamplerState& sampling, const std::shared_ptr<Storage>& storage)
        {
            mSource = &source;
            mRevision = source.getModifiedCount();
            mSampling = sampling;
            mStorage = storage;
        }

    private:
        osg::observer_ptr<const osg::Image> mSource;
        unsigned mRevision = 0;
        Render::SamplerState mSampling;
        std::weak_ptr<Storage> mStorage;
    };
}

#endif
