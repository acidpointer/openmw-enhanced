#ifndef OPENMW_COMPONENTS_SCENEUTIL_VIEWPREPARATION_H
#define OPENMW_COMPONENTS_SCENEUTIL_VIEWPREPARATION_H

#include <osg/FrameStamp>
#include <osgUtil/CullVisitor>

namespace SceneUtil
{
    // A metadata-only traversal. Never identify as a CullVisitor: custom
    // drawables must not skin vertices or submit render leaves in this phase.
    class ViewPreparationVisitor : public osg::NodeVisitor
    {
    public:
        explicit ViewPreparationVisitor(osgUtil::CullVisitor& visitor)
            : osg::NodeVisitor(TRAVERSE_ACTIVE_CHILDREN)
            , mVisitor(visitor)
        {
            setTraversalMask(visitor.getTraversalMask());
            setNodeMaskOverride(visitor.getNodeMaskOverride());
            if (visitor.getFrameStamp())
                setFrameStamp(new osg::FrameStamp(*visitor.getFrameStamp()));
            setTraversalNumber(visitor.getTraversalNumber());
        }
        osgUtil::CullVisitor& cullVisitor() const { return mVisitor; }
        osg::Vec3 getEyePoint() const override { return mVisitor.getEyePoint(); }
        osg::Vec3 getViewPoint() const override { return mVisitor.getViewPoint(); }
        float getDistanceToEyePoint(const osg::Vec3& point, bool scale) const override
        {
            return mVisitor.getDistanceToEyePoint(point, scale);
        }
        float getDistanceToViewPoint(const osg::Vec3& point, bool scale) const override
        {
            return mVisitor.getDistanceToViewPoint(point, scale);
        }
        void apply(osg::Drawable&) override {}
        void apply(osg::Camera&) override {}

    private:
        osgUtil::CullVisitor& mVisitor;
    };
}
#endif
