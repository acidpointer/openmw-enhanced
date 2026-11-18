#ifndef COMPONENTS_TERRAIN_HEIGHTCULL_H
#define COMPONENTS_TERRAIN_HEIGHTCULL_H

#include <osg/FrameStamp>
#include <osgUtil/CullVisitor>

#include <limits>

#include <components/sceneutil/nodecallback.hpp>

namespace osg
{
    class Node;
    class NodeVisitor;
}

namespace Terrain
{
    class HeightCullCallback : public SceneUtil::NodeCallback<HeightCullCallback>
    {
    public:
        explicit HeightCullCallback(bool perView) : mPerView(perView) {}
        void setLowZ(float z) { mLowZ = z; }

        void setHighZ(float highZ) { mHighZ = highZ; }
        float getHighZ() const { return mHighZ; }

        void setCullMask(unsigned int mask) { mMask = mask; }
        unsigned int getCullMask() const { return mMask; }

        void setViewVisible(osgUtil::CullVisitor& view, bool visible) const
        {
            if (const auto* stamp = view.getFrameStamp())
                sVisibility = { this, &view, view.getCurrentRenderStage(), stamp->getFrameNumber(), visible };
        }

        bool isVisible(osgUtil::CullVisitor& view) const
        {
            const auto* stamp = view.getFrameStamp();
            if (stamp && sVisibility.mOwner == this && sVisibility.mView == &view
                && sVisibility.mStage == view.getCurrentRenderStage() && sVisibility.mFrame == stamp->getFrameNumber())
                return sVisibility.mVisible;
            return mPerView || view.getEyePoint().z() <= mHighZ || mLowZ <= mHighZ;
        }

        void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            auto* view = dynamic_cast<osgUtil::CullVisitor*>(nv);
            if (!view || isVisible(*view))
                traverse(node, nv);
        }

    private:
        struct Visibility
        {
            const HeightCullCallback* mOwner;
            const osgUtil::CullVisitor* mView;
            const osgUtil::RenderStage* mStage;
            unsigned int mFrame;
            bool mVisible;
        };
        inline static thread_local Visibility sVisibility{};
        bool mPerView;
        float mLowZ{ -std::numeric_limits<float>::max() };
        float mHighZ{ std::numeric_limits<float>::max() };
        unsigned int mMask{ ~0u };
    };

}

#endif
