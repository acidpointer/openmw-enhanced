#ifndef OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONPREPARATION_H
#define OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONPREPARATION_H

#include "occlusionplan.hpp"
#include "viewpreparation.hpp"
#include <algorithm>
#include <osg/LOD>
#include <osg/Projection>
#include <vector>

namespace SceneUtil
{
    class OcclusionSource
    {
    public:
        virtual ~OcclusionSource() = default;
        virtual void prepareOccluders(
            osg::Node& node, const osg::Matrixd& matrix, osgUtil::CullVisitor& visitor, OcclusionPlan& plan) = 0;
    };

    // The same preparation walker is used by the renderer and cull regressions.
    // Sources enumerate immutable metadata, never traverse their model contents.
    class OcclusionPreparationVisitor : public ViewPreparationVisitor
    {
    public:
        using ViewPreparationVisitor::apply;
        osg::Vec3 getEyePoint() const override { return cullVisitor().getEyePoint() * mInverse; }
        osg::Vec3 getViewPoint() const override { return cullVisitor().getViewPoint() * mInverse; }
        float getDistanceToEyePoint(const osg::Vec3& point, bool scale) const override
        {
            return (point - getEyePoint()).length() * (scale ? cullVisitor().getLODScale() : 1.f);
        }
        float getDistanceToViewPoint(const osg::Vec3& point, bool scale) const override
        {
            return (point - getViewPoint()).length() * (scale ? cullVisitor().getLODScale() : 1.f);
        }
        OcclusionPreparationVisitor(
            osgUtil::CullVisitor& cv, OcclusionPlan& plan, std::initializer_list<osg::Node::NodeMask> stopMasks = {})
            : ViewPreparationVisitor(cv)
            , mPlan(plan)
            , mStopMasks(stopMasks)
        {
        }
        void apply(osg::Group& node) override
        {
            for (auto* callback = node.getCullCallback(); callback; callback = callback->getNestedCallback())
                if (auto* source = dynamic_cast<OcclusionSource*>(callback))
                {
                    source->prepareOccluders(node, mMatrix, cullVisitor(), mPlan);
                    return;
                }
            if (std::find(mStopMasks.begin(), mStopMasks.end(), node.getNodeMask()) == mStopMasks.end())
                traverse(node);
        }
        void apply(osg::Transform& node) override
        {
            if (node.getReferenceFrame() != osg::Transform::RELATIVE_RF)
                return;
            const auto previous = mMatrix;
            const auto previousInverse = mInverse;
            node.computeLocalToWorldMatrix(mMatrix, this);
            if (mInverse.invert(mMatrix))
                apply(static_cast<osg::Group&>(node));
            mMatrix = previous;
            mInverse = previousInverse;
        }
        void apply(osg::Projection&) override {}
        void apply(osg::LOD& node) override
        {
            // Pixel LOD requires a CullStack. A metadata visitor must not select
            // a different child and let its walls occlude the real view.
            if (node.getRangeMode() == osg::LOD::DISTANCE_FROM_EYE_POINT)
                apply(static_cast<osg::Group&>(node));
        }

    private:
        OcclusionPlan& mPlan;
        std::vector<osg::Node::NodeMask> mStopMasks;
        osg::Matrixd mMatrix, mInverse;
    };
}
#endif
