#ifndef OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONCULLING_H
#define OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONCULLING_H

#include <osg/BoundingBox>
#include <osg/Matrixd>
#include <osg/Matrixf>
#include <osg/Referenced>
#include <osg/Vec3f>

#include <atomic>
#include <vector>

namespace osgUtil
{
    class CullVisitor;
}
namespace osg
{
    class Camera;
    class Node;
}

class MaskedOcclusionCulling;

namespace SceneUtil
{
    /// Wraps Intel's Masked Software Occlusion Culling library.
    /// Provides a CPU-based hierarchical depth buffer for occlusion testing during cull traversal.
    class OcclusionCuller : public osg::Referenced
    {
    public:
        OcclusionCuller(unsigned int bufferWidth, unsigned int bufferHeight);
        ~OcclusionCuller();

        /// Call at the start of each frame's cull traversal.
        /// Stores the view-projection matrix and clears depth by default.
        /// Retaining depth requires its owner to validate the complete view and
        /// occluder set, then clear invalid depth before binding a visitor.
        void beginFrame(const osg::Matrixd& viewMatrix, const osg::Matrixd& projectionMatrix, float nearClip = 0.1f,
            bool retainDepth = false);
        void clearDepth();
        void bindVisitor(osgUtil::CullVisitor& visitor);
        bool isFrameActiveFor(osgUtil::CullVisitor& visitor) const;

        /// Rasterize terrain into the terrain-only snapshot, and optionally the full buffer.
        /// Call this only for terrain, before any building occluders are added.
        void rasterizeTerrainOccluder(const std::vector<osg::Vec3f>& worldPositions,
            const std::vector<unsigned int>& indices, bool fullBuffer = true);

        /// Rasterize world-space triangles as occluders into the full buffer only (not the terrain snapshot).
        /// Use for buildings.
        void rasterizeOccluder(const std::vector<osg::Vec3f>& positions, const std::vector<unsigned int>& indices,
            const osg::Matrixf* modelToWorld = nullptr, bool backfaceCull = true);

        /// Conservative projection for visibility queries. Near crossings fail open.
        struct ProjectedBounds
        {
            float minX, minY, maxX, maxY, minW;
            double screenRatio() const { return static_cast<double>((maxX - minX) * (maxY - minY)) * 0.25; }
        };
        bool projectBounds(const osg::BoundingBox& worldBB, ProjectedBounds& projected) const;
        /// Candidate ranking may include opaque triangles crossing the near
        /// plane. MOC clips those triangles; their AABB is not an occludee.
        bool projectOccluderBounds(const osg::BoundingBox& worldBB, ProjectedBounds& projected) const;
        /// Terrain-only depth is used for coarse traversal and candidate selection.
        bool testVisibleAABBTerrainOnly(const osg::BoundingBox& worldBB) const;
        bool testVisibleAABBTerrainOnly(const ProjectedBounds& projected) const;

        /// Test against the full depth buffer (terrain + buildings).
        /// Use for conservative object and drawable bounds of any size.
        bool testVisibleAABB(const osg::BoundingBox& worldBB) const;

        bool isActive() const { return mMOC != nullptr; }
        bool isFrameActive() const { return mFrameActive; }
        void endFrame()
        {
            mVisitor.store(nullptr, std::memory_order_release);
            mFrameActive = false;
        }
        const char* getImplementationName() const;
        const osg::Matrixd& getViewInverse() const { return mViewInverse; }

        unsigned int getNumOccluded() const { return mNumOccluded; }
        unsigned int getNumTested() const { return mNumTested; }
        unsigned int getNumBuildingOccluders() const { return mNumBuildingOccluders; }
        unsigned int getNumBuildingTris() const { return mNumBuildingTris; }
        unsigned int getNumBuildingVerts() const { return mNumBuildingVerts; }
        double getRasterizeMs() const { return mRasterizeMs; }
        double getTestMs() const { return mTestMs; }
        double getMeshBuildMs() const { return mMeshBuildMs; }
        double getPagedCallbackMs() const { return mPagedCallbackMs; }
        double getCellCallbackMs() const { return mCellCallbackMs; }
        double getSceneCallbackMs() const { return mSceneCallbackMs; }
        double getChildTraverseMs() const { return mChildTraverseMs; }
        double getOcclusionOverheadMs() const { return mOcclusionOverheadMs; }
        double getStaticCandidateMs() const { return mStaticCandidateMs; }
        double getSmallTestMs() const { return mSmallTestMs; }
        unsigned int getRasterizeCalls() const { return mRasterizeCalls; }
        unsigned int getTestCalls() const { return mTestCalls; }
        unsigned int getMeshBuilds() const { return mMeshBuilds; }
        unsigned int getStaticOccluderCandidates() const { return mStaticOccluderCandidates; }
        unsigned int getStaticOccludersSkippedBudget() const { return mStaticOccludersSkippedBudget; }
        unsigned int getStaticOccludersSkippedDistance() const { return mStaticOccludersSkippedDistance; }
        unsigned int getStaticOccludersSkippedScreen() const { return mStaticOccludersSkippedScreen; }
        unsigned int getSmallOccludeesSkippedScreen() const { return mSmallOccludeesSkippedScreen; }
        unsigned int getTerrainCellTests() const { return mTerrainCellTests; }
        unsigned int getTerrainCellOccluded() const { return mTerrainCellOccluded; }
        unsigned int getTerrainPagedTests() const { return mTerrainPagedTests; }
        unsigned int getTerrainPagedOccluded() const { return mTerrainPagedOccluded; }
        void setStaticOccludersEnabled(bool enabled) { mStaticOccludersEnabled = enabled; }
        bool staticOccludersEnabled() const { return mStaticOccludersEnabled; }
        void setSmallObjectTestingEnabled(bool enabled) { mSmallObjectTestingEnabled = enabled; }
        bool smallObjectTestingEnabled() const { return mSmallObjectTestingEnabled; }
        void setStaticRasterBudgetMs(double value) { mStaticRasterBudgetMs = value; }
        void resetStaticRasterBudgetBaseline() { mStaticRasterBudgetBaselineMs = mRasterizeMs; }
        double getStaticRasterMs() const;
        // Includes candidate gathering/preparation as well as rasterization.
        // One operation may finish over budget; later candidates are skipped.
        bool staticRasterBudgetAvailable() const;
        bool staticCandidateBudgetAvailable() const
        {
            return staticRasterBudgetAvailable()
                && (mStaticRasterBudgetMs <= 0 || mStaticCandidateMs < mStaticRasterBudgetMs * 0.5);
        }
        void setMinOccluderScreenRatio(double value) { mMinOccluderScreenRatio = value; }
        double getMinOccluderScreenRatio() const { return mMinOccluderScreenRatio; }
        void setMinOccludeeScreenRatio(double value) { mMinOccludeeScreenRatio = value; }
        double getMinOccludeeScreenRatio() const { return mMinOccludeeScreenRatio; }
        bool estimateScreenRatio(const osg::BoundingBox& worldBB, double& ratio) const;
        void incrementBuildingOccluders(unsigned int tris, unsigned int verts)
        {
            ++mNumBuildingOccluders;
            mNumBuildingTris += tris;
            mNumBuildingVerts += verts;
        }
        void setBuildingOccluderCounts(unsigned int meshes, unsigned int triangles, unsigned int vertices)
        {
            mNumBuildingOccluders = meshes;
            mNumBuildingTris = triangles;
            mNumBuildingVerts = vertices;
        }
        void incrementStaticOccluderCandidates() { ++mStaticOccluderCandidates; }
        void incrementStaticOccludersSkippedBudget() { ++mStaticOccludersSkippedBudget; }
        void incrementStaticOccludersSkippedDistance() { ++mStaticOccludersSkippedDistance; }
        void incrementStaticOccludersSkippedScreen() { ++mStaticOccludersSkippedScreen; }
        void incrementSmallOccludeesSkippedScreen() { ++mSmallOccludeesSkippedScreen; }
        void recordPagedCallback(double ms) { mPagedCallbackMs += ms; }
        void recordCellCallback(double ms) { mCellCallbackMs += ms; }
        void recordSceneCallback(double ms) { mSceneCallbackMs += ms; }
        void recordChildTraverse(double ms) { mChildTraverseMs += ms; }
        void recordOcclusionOverhead(double ms) { mOcclusionOverheadMs += ms; }
        void recordStaticCandidate(double ms) { mStaticCandidateMs += ms; }
        void recordSmallTest(double ms) { mSmallTestMs += ms; }
        void recordTerrainCellTest(bool visible)
        {
            ++mTerrainCellTests;
            if (!visible)
                ++mTerrainCellOccluded;
        }
        void recordTerrainPagedTest(bool visible)
        {
            ++mTerrainPagedTests;
            if (!visible)
                ++mTerrainPagedOccluded;
        }
        void recordMeshBuild(double ms)
        {
            mMeshBuildMs += ms;
            ++mMeshBuilds;
        }

        /// Write the per-pixel depth buffer to depthData (width*height floats, bottom-to-top).
        void computePixelDepthBuffer(float* depthData) const;

        void getResolution(unsigned int& width, unsigned int& height) const;

    private:
        bool testVisibleAABBImpl(MaskedOcclusionCulling* moc, const ProjectedBounds& projected) const;

        MaskedOcclusionCulling* mMOC;
        MaskedOcclusionCulling* mMOCTerrainOnly; // terrain-only snapshot for building visibility tests
        osg::Matrixd mViewProjection;
        osg::Matrixd mProjection;
        std::atomic<const osgUtil::CullVisitor*> mVisitor{ nullptr };
        const osg::Camera* mCamera = nullptr;
        const osg::Node* mWorldRoot = nullptr;
        std::size_t mRootDepth = 0;
        unsigned int mFrameNumber = 0;
        float mVPFloat[16] = {};
        float mNearClip = 0.1f;
        osg::Matrixd mViewInverse;
        bool mFrameActive = false;

        mutable unsigned int mNumOccluded = 0;
        mutable unsigned int mNumTested = 0;
        unsigned int mNumBuildingOccluders = 0;
        unsigned int mNumBuildingTris = 0;
        unsigned int mNumBuildingVerts = 0;
        double mRasterizeMs = 0.0;
        mutable double mTestMs = 0.0;
        double mMeshBuildMs = 0.0;
        double mPagedCallbackMs = 0.0;
        double mCellCallbackMs = 0.0;
        double mSceneCallbackMs = 0.0;
        double mChildTraverseMs = 0.0;
        double mOcclusionOverheadMs = 0.0;
        double mStaticCandidateMs = 0.0;
        double mSmallTestMs = 0.0;
        unsigned int mRasterizeCalls = 0;
        mutable unsigned int mTestCalls = 0;
        unsigned int mMeshBuilds = 0;
        unsigned int mStaticOccluderCandidates = 0;
        unsigned int mStaticOccludersSkippedBudget = 0;
        unsigned int mStaticOccludersSkippedDistance = 0;
        unsigned int mStaticOccludersSkippedScreen = 0;
        mutable unsigned int mSmallOccludeesSkippedScreen = 0;
        unsigned int mTerrainCellTests = 0;
        unsigned int mTerrainCellOccluded = 0;
        unsigned int mTerrainPagedTests = 0;
        unsigned int mTerrainPagedOccluded = 0;
        bool mStaticOccludersEnabled = true;
        bool mSmallObjectTestingEnabled = true;
        double mStaticRasterBudgetMs = 0.0;
        double mStaticRasterBudgetBaselineMs = 0.0;
        double mMinOccluderScreenRatio = 0.0;
        double mMinOccludeeScreenRatio = 0.0;
    };
}

#endif
