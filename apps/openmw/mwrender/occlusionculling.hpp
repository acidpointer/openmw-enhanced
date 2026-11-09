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
#include <components/sceneutil/lightboundsindex.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/occlusionplan.hpp>
#include <components/sceneutil/occlusionpreparation.hpp>

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
            , mIndex(copy.mIndex)
            , mIndexed(copy.mIndexed)
        {
        }
        META_Object(MWRender, PagedOccluderData)

        void buildSpatialIndex();
        void collectNearbyOccluders(const osg::Vec3f& eyeWorld, float maxDistanceSq,
            std::vector<std::shared_ptr<const OccluderMesh>>& out, std::vector<std::size_t>& scratch) const;

        std::vector<std::shared_ptr<const OccluderMesh>> mOccluderMeshes;

    private:
        SceneUtil::LightBoundsIndex mIndex;
        bool mIndexed = false;
    };

    // Per-placement metadata, owning immutable mesh data but no scene nodes.
    class StaticOccluderData : public osg::Object
    {
    public:
        StaticOccluderData() { setName("OpenMW Static Occluder"); }
        StaticOccluderData(const StaticOccluderData& other, const osg::CopyOp& op = {})
            : osg::Object(other, op)
            , mModel(other.mModel)
            , mResolution(other.mResolution)
            , mShrink(other.mShrink)
            , mMesh(other.mMesh)
            , mVisual(other.mVisual)
            , mPlacement(other.mPlacement)
            , mBounds(other.mBounds)
        {
        }
        META_Object(MWRender, StaticOccluderData)
        std::string mModel;
        int mResolution = 0, mShrink = 0;
        std::shared_ptr<const OccluderMesh> mMesh;
        osg::observer_ptr<osg::Node> mVisual;
        osg::Matrixf mPlacement;
        osg::BoundingBox mBounds;
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
        SceneUtil::OcclusionPlan mPlan;
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

    /// Consumes completed software depth before traversing paged geometry.
    class PagedOccluderCallback
        : public SceneUtil::NodeCallback<PagedOccluderCallback, osg::Node*, osgUtil::CullVisitor*>,
          public SceneUtil::OcclusionSource
    {
    public:
        explicit PagedOccluderCallback(SceneUtil::OcclusionCuller* culler);

        void operator()(osg::Node* node, osgUtil::CullVisitor* cv);
        void prepareOccluders(osg::Node& node, const osg::Matrixd& matrix, osgUtil::CullVisitor& cv,
            SceneUtil::OcclusionPlan& plan) override;

    private:
        osg::ref_ptr<SceneUtil::OcclusionCuller> mCuller;
        float mMaxDistanceSq;
    };

    /// Owns registered placements for preparation and consumes completed depth.
    class CellOcclusionCallback
        : public SceneUtil::NodeCallback<CellOcclusionCallback, osg::Group*, osgUtil::CullVisitor*>,
          public SceneUtil::OcclusionSource
    {
    public:
        CellOcclusionCallback(SceneUtil::OcclusionCuller* culler, float occluderMinRadius, float occluderMaxRadius,
            float occluderMaxDistance, bool enableStaticOccluders, OcclusionStorage* storage = nullptr);

        void operator()(osg::Group* node, osgUtil::CullVisitor* cv);

        void registerOccluder(osg::Node* node, StaticOccluderData* data);
        void collectOccluders(osg::Group& cell, const osg::Matrixd& parentToWorld, osgUtil::CullVisitor& cv,
            SceneUtil::OcclusionPlan& plan);
        void setStorage(OcclusionStorage* storage) { mStorage = storage; }
        void prepareOccluders(osg::Node& node, const osg::Matrixd& matrix, osgUtil::CullVisitor& cv,
            SceneUtil::OcclusionPlan& plan) override
        {
            if (auto* group = node.asGroup())
                collectOccluders(*group, matrix, cv, plan);
        }

    private:

        osg::ref_ptr<SceneUtil::OcclusionCuller> mCuller;
        float mOccluderMinRadius;
        float mOccluderMaxRadius;
        float mOccluderMaxDistanceSq;
        bool mEnableStaticOccluders;

        struct PlacementEntry
        {
            osg::observer_ptr<osg::Node> mNode;
            osg::ref_ptr<StaticOccluderData> mData;
        };
        std::unordered_map<osg::Node*, PlacementEntry> mPlacements;
        OcclusionStorage* mStorage = nullptr;
    };
}

#endif
