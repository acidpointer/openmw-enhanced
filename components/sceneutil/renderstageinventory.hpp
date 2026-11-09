#ifndef OPENMW_COMPONENTS_SCENEUTIL_RENDERSTAGEINVENTORY_H
#define OPENMW_COMPONENTS_SCENEUTIL_RENDERSTAGEINVENTORY_H

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <osg/Geometry>
#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>

namespace SceneUtil
{
    struct SubmissionCounts
    {
        std::size_t mLeaves = 0, mGeometry = 0, mPrimitiveSets = 0;
        SubmissionCounts& operator+=(const SubmissionCounts& other)
        {
            mLeaves += other.mLeaves;
            mGeometry += other.mGeometry;
            mPrimitiveSets += other.mPrimitiveSets;
            return *this;
        }
    };
    template <class F>
    void visitRenderLeaves(const osgUtil::RenderBin& bin, F&& visitor)
    {
        for (const auto* leaf : bin.getRenderLeafList())
            visitor(*leaf);
        for (const auto* graph : bin.getStateGraphList())
            for (const auto& leaf : graph->_leaves)
                visitor(*leaf);
        for (const auto& [id, child] : bin.getRenderBinList())
            visitRenderLeaves(*child, visitor);
    }
    inline SubmissionCounts countRenderSubmissions(const osgUtil::RenderBin& bin)
    {
        SubmissionCounts counts;
        visitRenderLeaves(bin, [&](const osgUtil::RenderLeaf& leaf) {
            ++counts.mLeaves;
            if (const auto* geometry = leaf._drawable->asGeometry())
            {
                ++counts.mGeometry;
                counts.mPrimitiveSets += geometry->getNumPrimitiveSets();
            }
        });
        return counts;
    }
    inline bool hasRenderSubmissions(const osgUtil::RenderBin& bin)
    {
        if (!bin.getRenderLeafList().empty())
            return true;
        for (const auto* graph : bin.getStateGraphList())
            if (!graph->_leaves.empty())
                return true;
        for (const auto& [id, child] : bin.getRenderBinList())
            if (hasRenderSubmissions(*child))
                return true;
        return false;
    }
    inline bool hasLaterSubmissions(
        const osgUtil::RenderStage& stage, int cutoff, std::initializer_list<int> excluded = {})
    {
        for (auto it = stage.getRenderBinList().upper_bound(cutoff); it != stage.getRenderBinList().end(); ++it)
            if (std::find(excluded.begin(), excluded.end(), it->first) == excluded.end()
                && hasRenderSubmissions(*it->second))
                return true;
        return false;
    }
    inline SubmissionCounts countDependentSubmissions(const osgUtil::RenderStage& stage)
    {
        SubmissionCounts counts;
        for (const auto* list : { &stage.getPreRenderList(), &stage.getPostRenderList() })
            for (const auto& [order, child] : *list)
            {
                counts += countRenderSubmissions(*child);
                counts += countDependentSubmissions(*child);
            }
        return counts;
    }
    inline bool hasAnyStageSubmissions(const osgUtil::RenderStage& stage)
    {
        if (hasRenderSubmissions(stage))
            return true;
        for (const auto* list : { &stage.getPreRenderList(), &stage.getPostRenderList() })
            for (const auto& [order, child] : *list)
                if (hasAnyStageSubmissions(*child))
                    return true;
        return false;
    }
    inline bool hasUnknownCameraConsumers(const osgUtil::RenderStage& stage, const osg::Camera* colorOnly)
    {
        // Unknown cameras may sample the scene textures, including history from
        // the previous use of a framebuffer slot. Retain their legacy inputs.
        for (const auto& [order, child] : stage.getPreRenderList())
            if (hasAnyStageSubmissions(*child))
                return true;
        for (const auto& [order, child] : stage.getPostRenderList())
        {
            if (child->getCamera() != colorOnly && hasRenderSubmissions(*child))
                return true;
            if (countDependentSubmissions(*child).mLeaves)
                return true;
        }
        return false;
    }
}
#endif
