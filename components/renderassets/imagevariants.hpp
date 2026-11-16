#ifndef OPENMW_COMPONENTS_RENDERASSETS_IMAGEVARIANTS_H
#define OPENMW_COMPONENTS_RENDERASSETS_IMAGEVARIANTS_H

#include <components/render/cachesweep.hpp>

#include <osg/Image>
#include <osg/observer_ptr>

#include <array>
#include <memory>
#include <unordered_map>
#include <utility>

namespace RenderAssets
{
    // Sampler bindings share image storage. A full mip chain also serves a
    // base-level-only sampler; the reverse requires another upload. A source
    // edit invalidates both variants.
    // Weak entries neither pin decoded mod images nor keep GPU storage alive.
    template <class Storage>
    class ImageVariants
    {
    public:
        void reserve(std::size_t count) { mEntries.reserve(count); }

        std::shared_ptr<Storage> find(const osg::Image& source, bool mipmaps) const
        {
            return find(source, mipmaps, [](const Storage&) { return true; });
        }

        template <class Compatible>
        std::shared_ptr<Storage> find(const osg::Image& source, bool mipmaps, Compatible&& compatible) const
        {
            const auto found = mEntries.find(&source);
            if (found == mEntries.end() || found->second.mSource.get() != &source
                || found->second.mRevision != source.getModifiedCount())
                return {};
            if (auto full = found->second.mStorage[1].lock(); full && compatible(*full))
                return full;
            if (!mipmaps)
                if (auto base = found->second.mStorage[0].lock(); base && compatible(*base))
                    return base;
            return {};
        }

        void store(const osg::Image& source, bool mipmaps, const std::shared_ptr<Storage>& storage)
        {
            auto& entry = mEntries[&source];
            if (entry.mSource.get() != &source || entry.mRevision != source.getModifiedCount())
            {
                entry.mStorage = {};
                entry.mSource = &source;
                entry.mRevision = source.getModifiedCount();
            }
            entry.mStorage[mipmaps] = storage;
        }

        template <class Stop>
        std::size_t sweep(Stop&& stop)
        {
            return mSweep.sweep(
                mEntries,
                [](const auto& item) {
                    return !item.second.mSource.valid()
                        || (item.second.mStorage[0].expired() && item.second.mStorage[1].expired());
                },
                std::forward<Stop>(stop));
        }

    private:
        struct Entry
        {
            osg::observer_ptr<const osg::Image> mSource;
            unsigned mRevision = 0;
            std::array<std::weak_ptr<Storage>, 2> mStorage;
        };
        std::unordered_map<const osg::Image*, Entry> mEntries;
        Render::CacheSweep<decltype(mEntries)> mSweep;
    };
}

#endif
