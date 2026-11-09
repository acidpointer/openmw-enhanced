#ifndef OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONCALLBACK_H
#define OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONCALLBACK_H

#include "nodecallback.hpp"
#include "occlusionculling.hpp"
#include "templateref.hpp"

#include <algorithm>
#include <cmath>
#include <osg/Drawable>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Transform>
#include <osgUtil/CullVisitor>

namespace SceneUtil
{
    inline double adaptiveOcclusionBudget(double configured, bool throttled)
    {
        if (!throttled)
            return configured;
        // Keep fresh occluders every frame, including terrain-free interiors.
        // Respect a smaller user cap; zero otherwise means unlimited.
        return configured > 0 ? std::min(configured, std::max(0.125, configured * 0.25)) : 0.25;
    }
    inline bool mayTestOcclusionBounds(const osg::Node& node)
    {
        if (!node.isCullingActive() || node.asCamera())
            return false;
        if (const auto* transform = node.asTransform();
            transform && transform->getReferenceFrame() != osg::Transform::RELATIVE_RF)
            return false;
        bool skip = false;
        node.getUserValue("skipOcclusion", skip);
        return !skip;
    }

    inline osg::BoundingBox occlusionWorldBounds(const osg::BoundingBox& local, const osg::Matrixd& localToWorld)
    {
        osg::BoundingBox world;
        if (local.valid())
            for (unsigned int i = 0; i < 8; ++i)
            {
                const auto corner = local.corner(i) * localToWorld;
                if (!std::isfinite(corner.x()) || !std::isfinite(corner.y()) || !std::isfinite(corner.z()))
                    return {};
                world.expandBy(corner);
            }
        return world;
    }

    inline bool occludedNode(osg::Node& node, osgUtil::CullVisitor& visitor, const OcclusionCuller& culler)
    {
        if (!culler.isFrameActiveFor(visitor) || !culler.smallObjectTestingEnabled() || !mayTestOcclusionBounds(node))
            return false;
        osg::BoundingBox local;
        if (auto* drawable = node.asDrawable())
            local = drawable->getBoundingBox();
        else if (const auto* transform = node.asTransform())
        {
            // Transform callbacks run after OSG pushes the node's matrix. Bounds
            // of its children are local; node.getBound() would transform twice.
            osg::BoundingSphere bound;
            for (unsigned int i = 0; i < transform->getNumChildren(); ++i)
                bound.expandBy(transform->getChild(i)->getBound());
            if (bound.valid())
                local.expandBy(bound);
        }
        else if (node.getBound().valid())
            local.expandBy(node.getBound());
        const auto matrix = *visitor.getModelViewMatrix() * culler.getViewInverse();
        return !culler.testVisibleAABB(occlusionWorldBounds(local, matrix));
    }

    inline bool drawableOutsideOcclusionFrustum(const osg::BoundingBox& bounds, const osg::CullingSet& culling)
    {
        if (!bounds.valid() || !(culling.getCullingMask() & osg::CullingSet::VIEW_FRUSTUM_CULLING))
            return false;
        auto mask = culling.getFrustum().getCurrentMask();
        for (const auto& plane : culling.getFrustum().getPlaneList())
        {
            if ((mask & 1) && plane.intersect(bounds) < 0)
                return true;
            mask >>= 1;
            if (!mask)
                break;
        }
        return false;
    }

    class OcclusionNodeCallback : public NodeCallback<OcclusionNodeCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        explicit OcclusionNodeCallback(OcclusionCuller* culler = nullptr)
            : mCuller(culler)
        {
        }
        OcclusionNodeCallback(const OcclusionNodeCallback& other, const osg::CopyOp& op = {})
            : osg::Object(other, op)
            , NodeCallback(other, op)
            , mCuller(other.mCuller)
        {
        }
        META_Object(SceneUtil, OcclusionNodeCallback)
        void operator()(osg::Node* node, osgUtil::CullVisitor* visitor)
        {
            if (!mCuller || !occludedNode(*node, *visitor, *mCuller))
                traverse(node, visitor);
        }

    private:
        osg::ref_ptr<OcclusionCuller> mCuller;
    };

    class OcclusionDrawableCallback : public osg::Drawable::CullCallback
    {
    public:
        explicit OcclusionDrawableCallback(OcclusionCuller* culler = nullptr)
            : mCuller(culler)
        {
        }
        OcclusionDrawableCallback(const OcclusionDrawableCallback& other, const osg::CopyOp& op = {})
            : osg::Object(other, op)
            , osg::Callback(other, op)
            , osg::Drawable::CullCallback(other, op)
            , mCuller(other.mCuller)
        {
        }
        META_Object(SceneUtil, OcclusionDrawableCallback)
        bool cull(osg::NodeVisitor* visitor, osg::Drawable* drawable, osg::State*) const override
        {
            auto* cv = dynamic_cast<osgUtil::CullVisitor*>(visitor);
            if (!cv || !mCuller || !mCuller->isFrameActiveFor(*cv) || !mCuller->smallObjectTestingEnabled()
                || !mayTestOcclusionBounds(*drawable))
                return false;
            // OSG calls drawable callbacks before its own frustum test. Reject
            // these cheap misses before projecting bounds into software depth.
            if (drawableOutsideOcclusionFrustum(drawable->getBoundingBox(), cv->getCurrentCullingSet()))
                return true;
            const auto matrix = *cv->getModelViewMatrix() * mCuller->getViewInverse();
            return !mCuller->testVisibleAABB(occlusionWorldBounds(drawable->getBoundingBox(), matrix));
        }

    private:
        osg::ref_ptr<OcclusionCuller> mCuller;
    };

    // Paged geometry may share immutable arrays and drawable templates. Own the
    // callback-bearing drawable and its user data; never patch the template.
    class InstallOcclusionDrawables : public osg::NodeVisitor
    {
    public:
        explicit InstallOcclusionDrawables(OcclusionCuller* culler)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mCuller(culler)
        {
        }
        void apply(osg::Group& group) override
        {
            for (unsigned int i = 0; i < group.getNumChildren(); ++i)
                if (auto* drawable = group.getChild(i)->asDrawable())
                {
                    if (auto copy = clone(drawable))
                        group.setChild(i, copy);
                }
                else
                    group.getChild(i)->accept(*this);
        }
        void apply(osg::Geode& geode) override
        {
            for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                if (auto copy = clone(geode.getDrawable(i)))
                    geode.setDrawable(i, copy);
        }

    private:
        osg::ref_ptr<osg::Drawable> clone(osg::Drawable* drawable)
        {
            if (!drawable->asGeometry() || drawable->getCullCallback() || drawable->getUpdateCallback())
                return {};
            osg::ref_ptr<osg::Drawable> copy
                = static_cast<osg::Drawable*>(drawable->clone(osg::CopyOp::DEEP_COPY_USERDATA));
            addTemplateRef(*copy, drawable);
            copy->setCullCallback(new OcclusionDrawableCallback(mCuller));
            return copy;
        }
        osg::ref_ptr<OcclusionCuller> mCuller;
    };
}

#endif
