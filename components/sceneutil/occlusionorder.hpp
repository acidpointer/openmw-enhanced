#ifndef OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONORDER_H
#define OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONORDER_H

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <osg/Camera>
#include <osg/Group>
#include <osgUtil/CullVisitor>

namespace SceneUtil
{
    // Keep the terrain/paging phase first, then visit nearby cells before far
    // cells so their occluders are available before far objects are submitted.
    // Update traversal and other cameras retain the original graph order.
    class OcclusionOrderGroup : public osg::Group
    {
    public:
        explicit OcclusionOrderGroup(std::string camera = {}, unsigned int fixedPrefix = 1)
            : mCamera(std::move(camera))
            , mFixedPrefix(fixedPrefix)
        {
        }
        OcclusionOrderGroup(const OcclusionOrderGroup& other, const osg::CopyOp& op = {})
            : osg::Group(other, op)
            , mCamera(other.mCamera)
            , mFixedPrefix(other.mFixedPrefix)
        {
        }
        META_Node(SceneUtil, OcclusionOrderGroup)

        void traverse(osg::NodeVisitor& visitor) override
        {
            auto* cv = dynamic_cast<osgUtil::CullVisitor*>(&visitor);
            if (!cv || !cv->getCurrentCamera() || cv->getCurrentCamera()->getName() != mCamera
                || getNumChildren() <= mFixedPrefix + 1)
            {
                osg::Group::traverse(visitor);
                return;
            }
            struct Child
            {
                osg::Node* node;
                double distance;
            };
            std::vector<Child> cells;
            cells.reserve(getNumChildren() - mFixedPrefix);
            const osg::Vec3d eye = cv->getEyeLocal();
            for (unsigned int i = mFixedPrefix; i < getNumChildren(); ++i)
            {
                auto* child = getChild(i);
                if (!visitor.validNodeMask(*child))
                    continue;
                const auto& bound = child->getBound();
                const double distance = bound.valid() ? (osg::Vec3d(bound.center()) - eye).length2() : 0;
                cells.push_back({ child, std::isfinite(distance) ? distance : 0 });
            }
            std::stable_sort(
                cells.begin(), cells.end(), [](const Child& a, const Child& b) { return a.distance < b.distance; });
            for (unsigned int i = 0; i < std::min(mFixedPrefix, getNumChildren()); ++i)
                getChild(i)->accept(visitor);
            for (const auto& child : cells)
                child.node->accept(visitor);
        }

    private:
        std::string mCamera;
        unsigned int mFixedPrefix;
    };
}

#endif
