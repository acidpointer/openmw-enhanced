#ifndef OPENMW_COMPONENTS_OCCLUSIONCULLING_OCCLUDERMESH_H
#define OPENMW_COMPONENTS_OCCLUSIONCULLING_OCCLUDERMESH_H

#include <osg/BoundingBox>
#include <osg/Vec3f>

#include <vector>

namespace osg
{
    class Node;
}

namespace OcclusionCulling
{
    /// Simplified occluder mesh used for CPU-side software occlusion culling.
    /// Stored in local (model) space; callers apply a world transform before rasterizing.
    struct OccluderMesh
    {
        osg::BoundingBox aabb;
        std::vector<osg::Vec3f> vertices; // original triangles, optionally inset within each triangle
        std::vector<unsigned int> indices; // triangle indices
    };

    /// Select at most 2 * gridRes^2 original opaque triangles by area.
    /// Dynamic/transparent geometry is excluded. Original winding and openings
    /// are preserved; shrinkFactor only insets triangles within their own plane.
    OccluderMesh buildSimplifiedMesh(osg::Node* node, int gridRes, float shrinkFactor);

    /// Build a reusable local-space mesh without applying the root node's
    /// placement transform. Child transforms are still applied.
    OccluderMesh buildSimplifiedMeshWithoutRootTransform(osg::Node* node, int gridRes, float shrinkFactor);
}

#endif
