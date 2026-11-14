#ifndef OPENMW_COMPONENTS_RENDER_SCENE_H
#define OPENMW_COMPONENTS_RENDER_SCENE_H

#include "backend.hpp"

#include <components/renderassets/model.hpp>

#include <memory>

namespace Render
{
    using InstanceHandle = Handle<struct InstanceTag>;

    struct View
    {
        Matrix mClipFromWorld = IdentityMatrix;
        std::array<float, 3> mEye{};
        std::array<float, 3> mForward{ 0, 0, 1 };
        std::array<float, 4> mClearColor{ 0, 0, 0, 1 };
        std::uint32_t mMask = ~std::uint32_t{ 0 };
    };

    // The backend outlives the scene. All methods run on the render owner
    // thread; immutable ModelAssets can be prepared by background workers.
    class Scene
    {
    public:
        explicit Scene(Backend& backend);
        ~Scene();
        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;

        InstanceHandle insert(std::shared_ptr<const ModelAsset> model, const Matrix& worldFromModel = IdentityMatrix);
        void remove(InstanceHandle instance);
        void setTransform(InstanceHandle instance, const Matrix& worldFromModel);
        void setVisible(InstanceHandle instance, bool visible);
        void setMask(InstanceHandle instance, std::uint32_t mask);
        void setTint(InstanceHandle instance, const std::array<float, 4>& tint);
        // Borrowed draw storage. Submit before removing referenced instances;
        // render() performs preparation and resource capture together.
        std::span<const Draw> prepare(const View& view);
        bool render(const View& view);

    private:
        class Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
