#ifndef OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONCULLING_H
#define OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONCULLING_H

#include <osg/BoundingBox>
#include <osg/Matrixd>
#include <osg/Referenced>
#include <osg/Vec3f>

#include <vector>

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
        /// Clears the depth buffer and stores the view-projection matrix.
        void beginFrame(const osg::Matrixd& viewMatrix, const osg::Matrixd& projectionMatrix);

        /// Rasterize terrain into the full buffer AND the terrain-only snapshot.
        /// Call this only for terrain, before any building occluders are added.
        void rasterizeTerrainOccluder(const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices);

        /// Rasterize world-space triangles as occluders into the full buffer only (not the terrain snapshot).
        /// Use for buildings.
        void rasterizeOccluder(const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices);

        /// Rasterize a world-space AABB as an occluder (12 triangles for 6 faces).
        void rasterizeAABBOccluder(const osg::BoundingBox& worldBB);

        /// Test against the terrain-only depth buffer.
        /// Use for cells and buildings — prevents buildings from false-occluding each other.
        bool testVisibleAABBTerrainOnly(const osg::BoundingBox& worldBB) const;

        /// Test against the full depth buffer (terrain + buildings).
        /// Use for small objects in Pass 2.
        bool testVisibleAABB(const osg::BoundingBox& worldBB) const;

        bool isActive() const { return mMOC != nullptr; }
        bool isFrameActive() const { return mFrameActive; }
        void endFrame() { mFrameActive = false; }
        const char* getImplementationName() const;

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
        void setStaticOccludersEnabled(bool enabled) { mStaticOccludersEnabled = enabled; }
        bool staticOccludersEnabled() const { return mStaticOccludersEnabled; }
        void setSmallObjectTestingEnabled(bool enabled) { mSmallObjectTestingEnabled = enabled; }
        bool smallObjectTestingEnabled() const { return mSmallObjectTestingEnabled; }
        void setStaticRasterBudgetMs(double value) { mStaticRasterBudgetMs = value; }
        void resetStaticRasterBudgetBaseline() { mStaticRasterBudgetBaselineMs = mRasterizeMs; }
        double getStaticRasterMs() const;
        bool staticRasterBudgetAvailable() const;
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
        void incrementStaticOccluderCandidates() { ++mStaticOccluderCandidates; }
        void incrementStaticOccludersSkippedBudget() { ++mStaticOccludersSkippedBudget; }
        void incrementStaticOccludersSkippedDistance() { ++mStaticOccludersSkippedDistance; }
        void incrementStaticOccludersSkippedScreen() { ++mStaticOccludersSkippedScreen; }
        void incrementSmallOccludeesSkippedScreen() { ++mSmallOccludeesSkippedScreen; }
        void recordPagedCallback(double ms) { mPagedCallbackMs += ms; }
        void recordCellCallback(double ms) { mCellCallbackMs += ms; }
        void recordSceneCallback(double ms) { mSceneCallbackMs += ms; }
        void recordChildTraverse(double ms) { mChildTraverseMs += ms; }
        void recordStaticCandidate(double ms) { mStaticCandidateMs += ms; }
        void recordSmallTest(double ms) { mSmallTestMs += ms; }
        void recordMeshBuild(double ms)
        {
            mMeshBuildMs += ms;
            ++mMeshBuilds;
        }

        /// Write the per-pixel depth buffer to depthData (width*height floats, bottom-to-top).
        void computePixelDepthBuffer(float* depthData) const;

        void getResolution(unsigned int& width, unsigned int& height) const;

    private:
        bool testVisibleAABBImpl(MaskedOcclusionCulling* moc, const osg::BoundingBox& worldBB) const;

        MaskedOcclusionCulling* mMOC;
        MaskedOcclusionCulling* mMOCTerrainOnly; // terrain-only snapshot for building visibility tests
        osg::Matrixd mViewProjection;
        float mVPFloat[16] = {};
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
        double mStaticCandidateMs = 0.0;
        double mSmallTestMs = 0.0;
        unsigned int mRasterizeCalls = 0;
        mutable unsigned int mTestCalls = 0;
        unsigned int mMeshBuilds = 0;
        unsigned int mStaticOccluderCandidates = 0;
        unsigned int mStaticOccludersSkippedBudget = 0;
        unsigned int mStaticOccludersSkippedDistance = 0;
        unsigned int mStaticOccludersSkippedScreen = 0;
        unsigned int mSmallOccludeesSkippedScreen = 0;
        bool mStaticOccludersEnabled = true;
        bool mSmallObjectTestingEnabled = true;
        double mStaticRasterBudgetMs = 0.0;
        double mStaticRasterBudgetBaselineMs = 0.0;
        double mMinOccluderScreenRatio = 0.0;
        double mMinOccludeeScreenRatio = 0.0;
    };
}

#endif
