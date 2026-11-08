#ifndef OPENMW_MWRENDER_OCCLUSIONCULLING_H
#define OPENMW_MWRENDER_OCCLUSIONCULLING_H

#include <osg/BoundingBox>
#include <osg/Camera>
#include <osg/Image>
#include <osg/Matrixf>
#include <osg/Object>
#include <osg/Texture2D>
#include <osg/Vec3f>
#include <osg/observer_ptr>
#include <osg/ref_ptr>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <components/occlusionculling/occludermesh.hpp>
#include <components/occlusionculling/occlusionstorage.hpp>
#include <components/sceneutil/nodecallback.hpp>

namespace MWRender
{
    // Bring component types into MWRender namespace for backward compatibility.
    using OccluderMesh = OcclusionCulling::OccluderMesh;
    using OcclusionStorage = OcclusionCulling::OcclusionStorage;
    using OcclusionCulling::buildSimplifiedMesh;

    /// Stored on paged chunk nodes (UserDataContainer) to provide sub-object
    /// occluder meshes for MOC rasterization during cull traversal.
    class PagedOccluderData : public osg::Object
    {
    public:
        PagedOccluderData() = default;
        PagedOccluderData(const PagedOccluderData& copy, const osg::CopyOp& = {})
            : osg::Object(copy)
            , mOccluderMeshes(copy.mOccluderMeshes)
            , mOccluderBins(copy.mOccluderBins)
            , mOccluderBounds(copy.mOccluderBounds)
            , mOccluderBinDim(copy.mOccluderBinDim)
        {
        }
        META_Object(MWRender, PagedOccluderData)

        void buildSpatialBins();
        void collectNearbyOccluders(
            const osg::Vec3f& eyeWorld, float maxDistanceSq, std::vector<const OccluderMesh*>& out) const;

        std::vector<OccluderMesh> mOccluderMeshes;
        std::vector<std::vector<unsigned int>> mOccluderBins;
        osg::BoundingBox mOccluderBounds;
        unsigned int mOccluderBinDim = 0;
    };
}

namespace osgUtil
{
    class CullVisitor;
}

namespace osg
{
    class Group;
    class Node;
}

namespace SceneUtil
{
    class OcclusionCuller;
}

namespace Terrain
{
    class TerrainOccluder;
}

namespace MWRender
{
    /// Wraps world traversal so a single camera owns the depth buffer for the
    /// entire subtree. Nested/concurrent cameras fall back to normal traversal.
    class SceneOcclusionCallback
        : public SceneUtil::NodeCallback<SceneOcclusionCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        SceneOcclusionCallback(SceneUtil::OcclusionCuller* culler, Terrain::TerrainOccluder* occluder, int radiusCells,
            bool enableTerrainOccluder, bool enableDebugOverlay, bool enableDebugMessages, bool enableInteriors,
            OcclusionStorage* storage = nullptr);

        void operator()(osg::Node* node, osgUtil::CullVisitor* cv);
        void endFrame(osg::Node* node, osgUtil::CullVisitor* cv);

        /// Update cell type flags. Call when the player transitions cells.
        void setCellType(bool isInterior, bool isQuasiExterior);

        void setStorage(OcclusionStorage* storage) { mStorage = storage; }

    private:
        void setupDebugOverlay();
        void updateDebugOverlay(osgUtil::CullVisitor* cv);

        osg::ref_ptr<SceneUtil::OcclusionCuller> mCuller;
        Terrain::TerrainOccluder* mTerrainOccluder;
        int mRadiusCells;
        bool mEnableTerrainOccluder;
        bool mEnableDebugOverlay;
        bool mEnableDebugMessages;
        bool mEnableInteriors;
        bool mIsInterior = false;
        bool mIsQuasiExterior = false;
        std::unordered_map<std::string, unsigned int> mLastFrameNumbers;
        OcclusionStorage* mStorage = nullptr;

        // Scratch buffers reused across frames
        std::size_t mTerrainVerts = 0;
        std::size_t mTerrainTris = 0;
        double mTerrainBuildMs = 0.0;
        double mTerrainRasterMs = 0.0;
        double mBeginCallbackMs = 0.0;
        double mEndCallbackMs = 0.0;
        unsigned int mActiveFrameNumber = 0;
        std::string mActiveCameraName;
        bool mFrameStarted = false;
        std::mutex mTraversalMutex;
        bool mActiveSceneCamera = false;
        bool mActiveAdaptiveStatics = false;
        bool mActiveStaticOccludersEnabled = false;
        bool mActiveSmallObjectTestingEnabled = false;
        int mAdaptiveStaticCooldownFrames = 0;
        int mAdaptiveStaticBadFrames = 0;

        // Debug overlay
        osg::ref_ptr<osg::Camera> mDebugCamera;
        osg::ref_ptr<osg::Image> mDebugImage;
        osg::ref_ptr<osg::Texture2D> mDebugTexture;
        std::vector<float> mDepthPixels;
    };

    /// Installed on paged chunk nodes (from ObjectPaging). Rasterizes the chunk's
    /// pre-built occluder meshes into MOC during the terrain traversal, before cell
    /// objects are tested.
    class PagedOccluderCallback
        : public SceneUtil::NodeCallback<PagedOccluderCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        PagedOccluderCallback(SceneUtil::OcclusionCuller* culler, float maxDistance, unsigned int maxTriangles);

        void operator()(osg::Node* node, osgUtil::CullVisitor* cv);

    private:
        osg::ref_ptr<SceneUtil::OcclusionCuller> mCuller;
        float mMaxDistanceSq;
        unsigned int mMaxTriangles;
    };

    /// Installed on each Cell Root group. Two-pass approach:
    /// Pass 1: Large objects are traversed, and eligible opaque mesh subsets
    ///         are rasterized within the static time/triangle budget.
    /// Pass 2: Small objects are tested against the enriched depth buffer (terrain + buildings).
    class CellOcclusionCallback
        : public SceneUtil::NodeCallback<CellOcclusionCallback, osg::Group*, osgUtil::CullVisitor*>
    {
    public:
        CellOcclusionCallback(SceneUtil::OcclusionCuller* culler, float occluderMinRadius, float occluderMaxRadius,
            float occluderShrinkFactor, int occluderMeshResolution, int occluderMaxMeshResolution,
            float occluderInsideThreshold, float occluderMaxDistance, bool enableStaticOccluders,
            unsigned int maxTriangles, OcclusionStorage* storage = nullptr);

        void operator()(osg::Group* node, osgUtil::CullVisitor* cv);

    private:
        /// Get cached occluder mesh (actual triangles + AABB) for a node.
        struct CachedMesh;
        const CachedMesh* getOccluderMesh(osg::Node* node, const osg::Matrixd& parentToWorld);

        osg::ref_ptr<SceneUtil::OcclusionCuller> mCuller;
        float mOccluderMinRadius;
        float mOccluderMaxRadius;
        float mOccluderShrinkFactor;
        int mOccluderMeshResolution;
        int mOccluderMaxMeshResolution;
        float mOccluderInsideThreshold;
        float mOccluderMaxDistanceSq;
        bool mEnableStaticOccluders;
        unsigned int mMaxTriangles;

        struct CachedMesh
        {
            osg::observer_ptr<osg::Node> mNode;
            osg::Matrixf mPlacement;
            std::string mModel;
            std::shared_ptr<const OccluderMesh> mMesh;
            osg::BoundingBox mWorldBounds;
        };
        std::unordered_map<osg::Node*, CachedMesh> mMeshCache;
        OcclusionStorage* mStorage = nullptr;
    };
}

#endif
