#ifndef OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONPLAN_H
#define OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONPLAN_H

#include "occlusionculling.hpp"
#include <components/occlusionculling/occludermesh.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <osg/Plane>
#include <osg/Vec4d>
#include <vector>

namespace SceneUtil
{
    inline double occlusionDistanceSquared(const osg::BoundingBox& bounds, const osg::Vec3d& point)
    {
        double result = 0;
        for (int axis = 0; axis < 3; ++axis)
        {
            const double d
                = std::max({ double(bounds._min[axis]) - point[axis], 0.0, point[axis] - double(bounds._max[axis]) });
            result += d * d;
        }
        return result;
    }

    struct OcclusionPlacement
    {
        std::shared_ptr<const OcclusionCulling::OccluderMesh> mMesh;
        osg::Matrixf mTransform;
        osg::BoundingBox mBounds;
        bool mLocal = false;
        bool operator==(const OcclusionPlacement& other) const
        {
            return mMesh == other.mMesh && mTransform == other.mTransform && mBounds == other.mBounds
                && mLocal == other.mLocal;
        }
    };

    struct OcclusionViewKey
    {
        const void* mCamera = nullptr;
        osg::Matrixd mView, mProjection;
        osg::Vec4d mViewport;
        std::vector<osg::Plane> mPlanes;
        std::uint64_t mTerrainRevision = 0;
        unsigned int mTraversalMask = 0, mMaxTriangles = 0;
        unsigned int mCullingMode = 0;
        double mBudget = 0, mMinOccluder = 0;
        float mNear = 0;
        float mLodScale = 1, mSmallFeature = 0;
        bool mTerrain = false, mStatics = false;
        bool operator==(const OcclusionViewKey&) const = default;
    };

    // Owns only immutable CPU meshes, never live scene nodes. The depth buffers
    // stay in OcclusionCuller. Call invalidate if their contents are changed by
    // any other owner. Only an exact view and complete source-set match reuses
    // depth; changing/removing an occluder clears it before querying objects.
    class OcclusionPlan
    {
    public:
        void invalidate() { mValid = false; }
        void start() { mCandidates.clear(); }
        auto& nearbyScratch()
        {
            mNearby.clear();
            return mNearby;
        }
        auto& indexScratch() { return mIndices; }
        void add(OcclusionCuller& culler, OcclusionPlacement placement)
        {
            if (!placement.mMesh || placement.mMesh->indices.empty() || !placement.mBounds.valid())
                return;
            OcclusionCuller::ProjectedBounds projected;
            if (!culler.projectOccluderBounds(placement.mBounds, projected)
                || projected.screenRatio() < culler.getMinOccluderScreenRatio())
                return;
            culler.incrementStaticOccluderCandidates();
            const double score = projected.screenRatio()
                / std::sqrt(double(std::max<std::size_t>(1, placement.mMesh->indices.size() / 3)));
            mCandidates.push_back({ std::move(placement), score, projected.minW });
        }
        bool prepare(const OcclusionViewKey& key)
        {
            std::sort(mCandidates.begin(), mCandidates.end(), [](const Candidate& a, const Candidate& b) {
                if (a.mScore != b.mScore)
                    return a.mScore > b.mScore;
                if (a.mNear != b.mNear)
                    return a.mNear < b.mNear;
                for (int i = 0; i < 8; ++i)
                    if (a.mPlacement.mBounds.corner(i) != b.mPlacement.mBounds.corner(i))
                        return a.mPlacement.mBounds.corner(i) < b.mPlacement.mBounds.corner(i);
                if (a.mPlacement.mTransform != b.mPlacement.mTransform)
                    return a.mPlacement.mTransform < b.mPlacement.mTransform;
                if (a.mPlacement.mLocal != b.mPlacement.mLocal)
                    return a.mPlacement.mLocal < b.mPlacement.mLocal;
                return std::less<const void*>{}(a.mPlacement.mMesh.get(), b.mPlacement.mMesh.get());
            });
            const bool reused = mValid && mKey == key && mCandidates.size() == mPrevious.size()
                && std::equal(mCandidates.begin(), mCandidates.end(), mPrevious.begin(),
                    [](const Candidate& a, const Candidate& b) { return a.mPlacement == b.mPlacement; });
            if (!reused)
            {
                mNext = 0;
                mTriangles = mVertices = mMeshes = 0;
                mKey = key;
                mPrevious = mCandidates;
            }
            mValid = true;
            return reused;
        }
        void rasterize(OcclusionCuller& culler)
        {
            culler.setBuildingOccluderCounts(mMeshes, mTriangles, mVertices);
            unsigned int operations = 0;
            for (; mNext < mPrevious.size(); ++mNext)
            {
                const auto& placement = mPrevious[mNext].mPlacement;
                const auto& mesh = *placement.mMesh;
                const unsigned int triangles = mesh.indices.size() / 3;
                if (mKey.mMaxTriangles && triangles > mKey.mMaxTriangles - mTriangles)
                    continue;
                // A single bounded mesh may finish over the time target. Always
                // make progress, even if preparation consumed this frame's
                // allowance. Retain completed coverage on subsequent frames.
                if (operations && !culler.staticRasterBudgetAvailable())
                    break;
                culler.rasterizeOccluder(
                    mesh.vertices, mesh.indices, placement.mLocal ? &placement.mTransform : nullptr);
                ++operations;
                ++mMeshes;
                mTriangles += triangles;
                mVertices += mesh.vertices.size();
                culler.incrementBuildingOccluders(triangles, mesh.vertices.size());
            }
        }
        bool complete() const { return mNext == mPrevious.size(); }

    private:
        struct Candidate
        {
            OcclusionPlacement mPlacement;
            double mScore;
            float mNear;
        };
        OcclusionViewKey mKey;
        std::vector<Candidate> mCandidates, mPrevious;
        std::vector<std::shared_ptr<const OcclusionCulling::OccluderMesh>> mNearby;
        std::vector<std::size_t> mIndices;
        std::size_t mNext = 0;
        unsigned int mTriangles = 0, mVertices = 0, mMeshes = 0;
        bool mValid = false;
    };
}
#endif
