#ifndef OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONTRAVERSAL_H
#define OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONTRAVERSAL_H

#include <osg/CullingSet>
#include <osg/NodeVisitor>
#include <osg/Transform>

namespace SceneUtil
{
    inline bool maySubmitOccluderSphere(const osg::BoundingSphere& bound, const osg::CullingSet& culling)
    {
        if (!bound.valid())
            return false;
        if (culling.getCullingMask() & osg::CullingSet::SMALL_FEATURE_CULLING)
            if ((bound.center() * culling.getPixelSizeVector()) * culling.getSmallFeatureCullingPixelSize()
                > bound.radius())
                return false;
        if (culling.getCullingMask() & osg::CullingSet::VIEW_FRUSTUM_CULLING)
        {
            auto mask = culling.getFrustum().getCurrentMask();
            for (const auto& plane : culling.getFrustum().getPlaneList())
            {
                if ((mask & 1) && plane.intersect(bound) < 0)
                    return false;
                mask >>= 1;
                if (!mask)
                    break;
            }
        }
        return true;
    }
    // An early rejection for occluder preparation, in the current parent's
    // coordinates. Do not mutate OSG's plane masks: sibling traversal still
    // needs them. Disabled culling and unusual/uncertain bounds stay visible.
    inline bool mayTraverseForOcclusion(
        const osg::Node& node, const osg::NodeVisitor& visitor, const osg::CullingSet& culling)
    {
        if (!visitor.validNodeMask(node))
            return false;
        if (!node.isCullingActive() || node.asCamera())
            return true;
        if (const auto* transform = node.asTransform();
            transform && transform->getReferenceFrame() != osg::Transform::RELATIVE_RF)
            return true;
        const auto& bound = node.getBound();
        if (!bound.valid() || !(culling.getCullingMask() & osg::CullingSet::VIEW_FRUSTUM_CULLING))
            return true;

        const auto& frustum = culling.getFrustum();
        auto mask = frustum.getCurrentMask();
        for (const auto& plane : frustum.getPlaneList())
        {
            if ((mask & 1) && plane.intersect(bound) < 0)
                return false;
            mask >>= 1;
            if (mask == 0)
                break;
        }
        return true;
    }
}

#endif
