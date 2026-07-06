#include "occlusionculling.hpp"

#include "objects.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <osg/BoundingBox>
#include <osg/BoundingSphere>
#include <osg/Camera>
#include <osg/Geometry>
#include <osg/Geode>
#include <osg/Group>
#include <osg/Timer>
#include <osgUtil/CullVisitor>

#include <components/debug/debuglog.hpp>
#include <components/misc/constants.hpp>
#include <components/occlusionculling/occludermesh.hpp>
#include <components/sceneutil/enhancedsettings.hpp>
#include <components/sceneutil/occlusionculling.hpp>
#include <components/terrain/terrainoccluder.hpp>
#include "../mwworld/class.hpp"

namespace MWRender
{
    namespace
    {
        constexpr unsigned int PagedOccluderBinDim = 16;

        bool enhancedWaterOcclusionEnabled()
        {
            const std::string value = SceneUtil::Enhanced::occlusionWaterCameras();
            return value.find("water") != std::string::npos || value.find("all") != std::string::npos;
        }

        class SceneOcclusionEndCallback
            : public SceneUtil::NodeCallback<SceneOcclusionEndCallback, osg::Node*, osgUtil::CullVisitor*>
        {
        public:
            explicit SceneOcclusionEndCallback(SceneOcclusionCallback* controller)
                : mController(controller)
            {
            }

            void operator()(osg::Node* node, osgUtil::CullVisitor* cv)
            {
                if (mController)
                    mController->endFrame(node, cv);
            }

        private:
            osg::ref_ptr<SceneOcclusionCallback> mController;
        };

        std::string_view getModelPathForNode(osg::Node* node)
        {
            if (!node)
                return {};

            if (auto* udc = node->getUserDataContainer())
            {
                for (unsigned int i = 0; i < udc->getNumUserObjects(); ++i)
                {
                    if (auto* holder = dynamic_cast<PtrHolder*>(udc->getUserObject(i)))
                        return holder->mPtr.getClass().getCorrectedModel(holder->mPtr);
                }
            }
            return {};
        }

        OccluderMesh transformLocalMesh(const OccluderMesh& localMesh, const osg::Matrixf& matrix)
        {
            OccluderMesh worldMesh;
            worldMesh.indices = localMesh.indices;
            worldMesh.vertices.reserve(localMesh.vertices.size());
            for (const auto& v : localMesh.vertices)
            {
                const osg::Vec3f transformed = v * matrix;
                worldMesh.vertices.push_back(transformed);
                worldMesh.aabb.expandBy(transformed);
            }

            if (localMesh.vertices.empty() && localMesh.aabb.valid())
            {
                for (unsigned int i = 0; i < 8; ++i)
                    worldMesh.aabb.expandBy(localMesh.aabb.corner(i) * matrix);
            }
            return worldMesh;
        }

        unsigned int occluderBinIndex(float value, float minValue, float maxValue, unsigned int dim)
        {
            if (dim <= 1 || maxValue <= minValue)
                return 0;

            const float normalized = (value - minValue) / (maxValue - minValue);
            return std::min(dim - 1, static_cast<unsigned int>(std::max(0.0f, normalized) * dim));
        }

        float distanceSqToRect2D(const osg::Vec3f& point, float minX, float minY, float maxX, float maxY)
        {
            const float dx = point.x() < minX ? minX - point.x() : (point.x() > maxX ? point.x() - maxX : 0.0f);
            const float dy = point.y() < minY ? minY - point.y() : (point.y() > maxY ? point.y() - maxY : 0.0f);
            return dx * dx + dy * dy;
        }
    }

    void PagedOccluderData::buildSpatialBins()
    {
        mOccluderBins.clear();
        mOccluderBounds = osg::BoundingBox();
        mOccluderBinDim = 0;

        if (mOccluderMeshes.size() < 64)
            return;

        for (const OccluderMesh& mesh : mOccluderMeshes)
            if (mesh.aabb.valid())
                mOccluderBounds.expandBy(mesh.aabb.center());

        if (!mOccluderBounds.valid() || mOccluderBounds.xMax() <= mOccluderBounds.xMin()
            || mOccluderBounds.yMax() <= mOccluderBounds.yMin())
            return;

        mOccluderBinDim = PagedOccluderBinDim;
        mOccluderBins.resize(mOccluderBinDim * mOccluderBinDim);

        for (unsigned int i = 0; i < mOccluderMeshes.size(); ++i)
        {
            const OccluderMesh& mesh = mOccluderMeshes[i];
            if (!mesh.aabb.valid())
                continue;

            const osg::Vec3f center = mesh.aabb.center();
            const unsigned int x = occluderBinIndex(center.x(), mOccluderBounds.xMin(), mOccluderBounds.xMax(),
                mOccluderBinDim);
            const unsigned int y = occluderBinIndex(center.y(), mOccluderBounds.yMin(), mOccluderBounds.yMax(),
                mOccluderBinDim);
            mOccluderBins[x + y * mOccluderBinDim].push_back(i);
        }
    }

    void PagedOccluderData::collectNearbyOccluders(
        const osg::Vec3f& eyeWorld, float maxDistanceSq, std::vector<const OccluderMesh*>& out) const
    {
        if (mOccluderBins.empty() || !mOccluderBounds.valid() || mOccluderBinDim == 0)
        {
            out.reserve(out.size() + mOccluderMeshes.size());
            for (const OccluderMesh& mesh : mOccluderMeshes)
            {
                if (!mesh.aabb.valid())
                    continue;
                if ((mesh.aabb.center() - eyeWorld).length2() <= maxDistanceSq)
                    out.push_back(&mesh);
            }
            return;
        }

        const float radius = std::sqrt(maxDistanceSq);
        const float binWidth = (mOccluderBounds.xMax() - mOccluderBounds.xMin()) / static_cast<float>(mOccluderBinDim);
        const float binHeight = (mOccluderBounds.yMax() - mOccluderBounds.yMin()) / static_cast<float>(mOccluderBinDim);
        const unsigned int minX = occluderBinIndex(eyeWorld.x() - radius, mOccluderBounds.xMin(),
            mOccluderBounds.xMax(), mOccluderBinDim);
        const unsigned int maxX = occluderBinIndex(eyeWorld.x() + radius, mOccluderBounds.xMin(),
            mOccluderBounds.xMax(), mOccluderBinDim);
        const unsigned int minY = occluderBinIndex(eyeWorld.y() - radius, mOccluderBounds.yMin(),
            mOccluderBounds.yMax(), mOccluderBinDim);
        const unsigned int maxY = occluderBinIndex(eyeWorld.y() + radius, mOccluderBounds.yMin(),
            mOccluderBounds.yMax(), mOccluderBinDim);

        for (unsigned int y = minY; y <= maxY; ++y)
        {
            for (unsigned int x = minX; x <= maxX; ++x)
            {
                const float binMinX = mOccluderBounds.xMin() + static_cast<float>(x) * binWidth;
                const float binMinY = mOccluderBounds.yMin() + static_cast<float>(y) * binHeight;
                const float binMaxX = binMinX + binWidth;
                const float binMaxY = binMinY + binHeight;
                if (distanceSqToRect2D(eyeWorld, binMinX, binMinY, binMaxX, binMaxY) > maxDistanceSq)
                    continue;

                const std::vector<unsigned int>& bin = mOccluderBins[x + y * mOccluderBinDim];
                out.reserve(out.size() + bin.size());
                for (unsigned int index : bin)
                    if (index < mOccluderMeshes.size())
                    {
                        const OccluderMesh& mesh = mOccluderMeshes[index];
                        if (!mesh.aabb.valid())
                            continue;
                        if ((mesh.aabb.center() - eyeWorld).length2() <= maxDistanceSq)
                            out.push_back(&mesh);
                    }
            }
        }
    }

    SceneOcclusionCallback::SceneOcclusionCallback(SceneUtil::OcclusionCuller* culler,
        Terrain::TerrainOccluder* occluder, int radiusCells, bool enableTerrainOccluder, bool enableDebugOverlay,
        bool enableDebugMessages, bool enableInteriors, OcclusionStorage* storage)
        : mCuller(culler)
        , mTerrainOccluder(occluder)
        , mRadiusCells(radiusCells)
        , mEnableTerrainOccluder(enableTerrainOccluder)
        , mEnableDebugOverlay(enableDebugOverlay)
        , mEnableDebugMessages(enableDebugMessages)
        , mEnableInteriors(enableInteriors)
        , mStorage(storage)
    {
    }

    void SceneOcclusionCallback::setCellType(bool isInterior, bool isQuasiExterior)
    {
        mIsInterior = isInterior;
        mIsQuasiExterior = isQuasiExterior;
    }

    void SceneOcclusionCallback::setupDebugOverlay()
    {
        unsigned int w, h;
        mCuller->getResolution(w, h);
        if (w == 0 || h == 0)
            return;

        mDepthPixels.resize(w * h);

        // Create image to hold depth data (luminance float -> converted to RGBA)
        mDebugImage = new osg::Image;
        mDebugImage->allocateImage(w, h, 1, GL_LUMINANCE, GL_FLOAT);

        // Create texture from image
        mDebugTexture = new osg::Texture2D(mDebugImage);
        mDebugTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        mDebugTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        mDebugTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        mDebugTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        mDebugTexture->setResizeNonPowerOfTwoHint(false);

        // Create POST_RENDER camera in corner of screen
        mDebugCamera = new osg::Camera;
        mDebugCamera->setName("OcclusionDebugCamera");
        mDebugCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        mDebugCamera->setRenderOrder(osg::Camera::POST_RENDER, 100);
        mDebugCamera->setAllowEventFocus(false);
        mDebugCamera->setClearMask(0);
        mDebugCamera->setProjectionMatrix(osg::Matrix::ortho2D(0, 1, 0, 1));
        mDebugCamera->setViewMatrix(osg::Matrix::identity());
        mDebugCamera->getOrCreateStateSet()->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        mDebugCamera->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
        mDebugCamera->setCullingActive(false);

        // Scale viewport to show in bottom-left corner (400px wide, aspect-correct height)
        float displayWidth = 400.0f;
        float displayHeight = displayWidth * static_cast<float>(h) / static_cast<float>(w);
        mDebugCamera->setViewport(0, 0, static_cast<int>(displayWidth), static_cast<int>(displayHeight));

        // Create textured quad
        osg::ref_ptr<osg::Geometry> quad
            = osg::createTexturedQuadGeometry(osg::Vec3(0, 0, 0), osg::Vec3(1, 0, 0), osg::Vec3(0, 1, 0));
        quad->setCullingActive(false);

        osg::StateSet* ss = quad->getOrCreateStateSet();
        ss->setTextureAttributeAndModes(0, mDebugTexture, osg::StateAttribute::ON);

        mDebugCamera->addChild(quad);
    }

    void SceneOcclusionCallback::updateDebugOverlay(osgUtil::CullVisitor* cv)
    {
        if (!mDebugCamera)
            return;

        unsigned int w, h;
        mCuller->getResolution(w, h);

        // Read depth buffer from MOC
        mCuller->computePixelDepthBuffer(mDepthPixels.data());

        // Copy to image (normalize: MOC stores 1/w, so closer = larger values)
        float* imageData = reinterpret_cast<float*>(mDebugImage->data());
        for (unsigned int i = 0; i < w * h; ++i)
        {
            float d = mDepthPixels[i];
            // MOC depth is 1/w (reciprocal clip-space w). 0 = far/empty, larger = closer.
            // Clamp and invert for visualization: dark = far, bright = near
            imageData[i] = std::min(d * 50.0f, 1.0f);
        }
        mDebugImage->dirty();

        // Inject debug camera into the cull visitor so it gets rendered
        unsigned int traversalMask = cv->getTraversalMask();
        cv->setTraversalMask(0xffffffff);
        mDebugCamera->accept(*cv);
        cv->setTraversalMask(traversalMask);
    }

    void SceneOcclusionCallback::operator()(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        (void)node;

        osg::Camera* cam = cv->getCurrentCamera();
        const std::string& cameraName = cam->getName();
        const bool sceneCamera = cameraName == Constants::SceneCamera;
        const bool waterCamera = enhancedWaterOcclusionEnabled()
            && (cameraName == "ReflectionCamera" || cameraName == "RefractionCamera");

        if (!sceneCamera && !waterCamera)
            return;

        // The scene is traversed multiple times per frame: once for the main cull pass,
        // and again by MWShadowTechnique::cullShadowReceivingScene (same camera name).
        // Only set up MOC on the first traversal; subsequent marker passes are no-ops.
        unsigned int frameNumber = cv->getFrameStamp()->getFrameNumber();
        if (mFrameStarted && (frameNumber != mActiveFrameNumber || cameraName != mActiveCameraName))
            mFrameStarted = false;
        unsigned int& lastFrameNumber = mLastFrameNumbers[cameraName];
        if (frameNumber == lastFrameNumber)
            return;
        lastFrameNumber = frameNumber;

        // Skip MSOC entirely in interiors (unless enabled via setting)
        if (mIsInterior && !mEnableInteriors)
            return;

        // Begin occlusion frame with camera matrices
        const osg::Timer_t beginStart = osg::Timer::instance()->tick();
        mCuller->beginFrame(cam->getViewMatrix(), cam->getProjectionMatrix());
        mTerrainBuildMs = 0.0;
        mTerrainRasterMs = 0.0;
        mBeginCallbackMs = 0.0;
        mEndCallbackMs = 0.0;
        mPositions.clear();
        mIndices.clear();

        bool smallObjectTestingEnabled = SceneUtil::Enhanced::occlusionCullingSmallObjects();
        bool staticOccludersEnabled = SceneUtil::Enhanced::occlusionCullingStaticOccluders();
        if (waterCamera && !SceneUtil::Enhanced::occlusionWaterStaticOccluders())
            staticOccludersEnabled = false;

        const bool adaptiveStatics = sceneCamera && SceneUtil::Enhanced::occlusionAdaptiveStatics();
        if (adaptiveStatics && mAdaptiveStaticCooldownFrames > 0)
        {
            staticOccludersEnabled = false;
            smallObjectTestingEnabled = false;
            --mAdaptiveStaticCooldownFrames;
        }
        if (!smallObjectTestingEnabled)
            staticOccludersEnabled = false;

        mCuller->setStaticOccludersEnabled(staticOccludersEnabled);
        mCuller->setSmallObjectTestingEnabled(smallObjectTestingEnabled);
        mCuller->setStaticRasterBudgetMs(SceneUtil::Enhanced::occlusionStaticRasterTimeBudgetMs());
        mCuller->setMinOccluderScreenRatio(SceneUtil::Enhanced::occlusionMinOccluderScreenRatio());
        mCuller->setMinOccludeeScreenRatio(SceneUtil::Enhanced::occlusionMinOccludeeScreenRatio());

        // Build and rasterize terrain occluder mesh (skip for quasi-exteriors and interiors — no real terrain)
        if (mEnableTerrainOccluder && !mIsQuasiExterior && !mIsInterior && mTerrainOccluder->hasTerrainData())
        {
            osg::Timer_t start = osg::Timer::instance()->tick();
            mTerrainOccluder->build(cv->getEyePoint(), mRadiusCells, mPositions, mIndices);
            mTerrainBuildMs = osg::Timer::instance()->delta_m(start, osg::Timer::instance()->tick());

            if (!mPositions.empty())
            {
                start = osg::Timer::instance()->tick();
                const bool needFullBuffer = staticOccludersEnabled || smallObjectTestingEnabled;
                mCuller->rasterizeTerrainOccluder(mPositions, mIndices, needFullBuffer);
                mTerrainRasterMs = osg::Timer::instance()->delta_m(start, osg::Timer::instance()->tick());
            }
        }
        mCuller->resetStaticRasterBudgetBaseline();
        mBeginCallbackMs = osg::Timer::instance()->delta_m(beginStart, osg::Timer::instance()->tick());
        mActiveFrameNumber = frameNumber;
        mActiveCameraName = cameraName;
        mActiveSceneCamera = sceneCamera;
        mActiveAdaptiveStatics = adaptiveStatics;
        mActiveStaticOccludersEnabled = staticOccludersEnabled;
        mActiveSmallObjectTestingEnabled = smallObjectTestingEnabled;
        mFrameStarted = true;
    }

    osg::ref_ptr<osg::Callback> SceneOcclusionCallback::createEndCallback()
    {
        return new SceneOcclusionEndCallback(this);
    }

    void SceneOcclusionCallback::endFrame(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        (void)node;
        if (!mFrameStarted)
            return;

        osg::Camera* cam = cv->getCurrentCamera();
        const std::string& cameraName = cam->getName();
        const unsigned int frameNumber = cv->getFrameStamp()->getFrameNumber();
        if (frameNumber != mActiveFrameNumber || cameraName != mActiveCameraName)
            return;

        const osg::Timer_t endStart = osg::Timer::instance()->tick();
        const bool sceneCamera = mActiveSceneCamera;
        const bool adaptiveStatics = mActiveAdaptiveStatics;
        const bool staticOccludersEnabled = mActiveStaticOccludersEnabled;
        const bool smallObjectTestingEnabled = mActiveSmallObjectTestingEnabled;
        const double callbackOverheadMs = mCuller->getOcclusionOverheadMs();
        const double occlusionOverheadMs = mBeginCallbackMs + callbackOverheadMs;
        if (adaptiveStatics && (staticOccludersEnabled || smallObjectTestingEnabled))
        {
            const unsigned int tested = mCuller->getNumTested();
            const unsigned int occluded = mCuller->getNumOccluded();
            const double weightedTests = static_cast<double>(tested)
                + static_cast<double>(mCuller->getTerrainCellTests()) * 16.0
                + static_cast<double>(mCuller->getTerrainPagedTests()) * 8.0;
            const double weightedOccluded = static_cast<double>(occluded)
                + static_cast<double>(mCuller->getTerrainCellOccluded()) * 16.0
                + static_cast<double>(mCuller->getTerrainPagedOccluded()) * 8.0;
            const double benefitRatio = weightedTests > 0.0 ? weightedOccluded / weightedTests : 0.0;
            const bool lowBenefit = benefitRatio < 0.15;
            const bool expensiveCellPath = occlusionOverheadMs > 2.0;
            if (expensiveCellPath && lowBenefit && weightedTests > 0.0)
                ++mAdaptiveStaticBadFrames;
            else
                mAdaptiveStaticBadFrames = 0;

            if (mAdaptiveStaticBadFrames >= 2)
            {
                mAdaptiveStaticBadFrames = 0;
                mAdaptiveStaticCooldownFrames = SceneUtil::Enhanced::occlusionAdaptiveCooldownFrames();
                if (mEnableDebugMessages)
                    Log(Debug::Info) << "OcclusionCull: adaptive enhanced cell occlusion cooldown triggered"
                                     << " begin_marker_ms=" << mBeginCallbackMs
                                     << " child_traverse_ms=" << mCuller->getChildTraverseMs()
                                     << " callback_overhead_ms=" << callbackOverheadMs
                                     << " overhead_ms=" << occlusionOverheadMs
                                     << " tested=" << tested
                                     << " occluded=" << occluded
                                     << " terrain_cell_skips=" << mCuller->getTerrainCellOccluded()
                                     << "/" << mCuller->getTerrainCellTests()
                                     << " paged_chunk_skips=" << mCuller->getTerrainPagedOccluded()
                                     << "/" << mCuller->getTerrainPagedTests()
                                     << " benefit=" << benefitRatio
                                     << " candidates=" << mCuller->getStaticOccluderCandidates()
                                     << " cooldown=" << mAdaptiveStaticCooldownFrames;
            }
        }
        else if (!adaptiveStatics)
        {
            mAdaptiveStaticBadFrames = 0;
        }
        mEndCallbackMs = osg::Timer::instance()->delta_m(endStart, osg::Timer::instance()->tick());
        mCuller->recordSceneCallback(mBeginCallbackMs + mEndCallbackMs);
        // End the occlusion frame so sub-camera traversals (water reflection/refraction,
        // shadow cameras) that share this scene graph don't incorrectly cull against
        // the main camera's occlusion buffer.
        mCuller->endFrame();
        mFrameStarted = false;

        // Update debug overlay AFTER traversal (terrain + building occluders now in buffer)
        if (sceneCamera && mEnableDebugOverlay)
        {
            if (!mDebugCamera)
                setupDebugOverlay();
            updateDebugOverlay(cv);
        }

        if (sceneCamera && mEnableDebugMessages)
        {
            static int frameCount = 0;
            if (++frameCount % 300 == 0)
            {
                const auto terrainTris = mIndices.size() / 3;
                const auto bldgTris = mCuller->getNumBuildingTris();
                const auto terrainVerts = mPositions.size();
                const auto bldgVerts = mCuller->getNumBuildingVerts();
                Log(Debug::Info) << "OcclusionCull: terrain tris=" << terrainTris << " terrain verts=" << terrainVerts
                                 << " bldg occluders=" << mCuller->getNumBuildingOccluders()
                                 << " bldg tris=" << bldgTris << " bldg verts=" << bldgVerts
                                 << " total tris=" << (terrainTris + bldgTris)
                                 << " total verts=" << (terrainVerts + bldgVerts)
                                 << " tested=" << mCuller->getNumTested()
                                 << " occluded=" << mCuller->getNumOccluded()
                                 << " camera=" << cameraName
                                 << " moc_impl=" << mCuller->getImplementationName()
                                 << " static_enabled=" << mCuller->staticOccludersEnabled()
                                 << " small_enabled=" << mCuller->smallObjectTestingEnabled()
                                 << " static_cooldown=" << mAdaptiveStaticCooldownFrames
                                 << " adaptive_bad_frames=" << mAdaptiveStaticBadFrames
                                 << " terrain_build_ms=" << mTerrainBuildMs
                                 << " terrain_raster_ms=" << mTerrainRasterMs
                                 << " static_raster_ms=" << mCuller->getStaticRasterMs()
                                 << " static_budget_ms=" << SceneUtil::Enhanced::occlusionStaticRasterTimeBudgetMs()
                                 << " raster_ms=" << mCuller->getRasterizeMs()
                                 << " raster_calls=" << mCuller->getRasterizeCalls()
                                 << " test_ms=" << mCuller->getTestMs()
                                 << " test_calls=" << mCuller->getTestCalls()
                                 << " mesh_build_ms=" << mCuller->getMeshBuildMs()
                                 << " mesh_builds=" << mCuller->getMeshBuilds()
                                 << " scene_cb_ms=" << mCuller->getSceneCallbackMs()
                                 << " begin_marker_ms=" << mBeginCallbackMs
                                 << " end_marker_ms=" << mEndCallbackMs
                                 << " callback_overhead_ms=" << callbackOverheadMs
                                 << " occlusion_overhead_ms=" << occlusionOverheadMs
                                 << " paged_cb_ms=" << mCuller->getPagedCallbackMs()
                                 << " cell_cb_ms=" << mCuller->getCellCallbackMs()
                                 << " child_traverse_ms=" << mCuller->getChildTraverseMs()
                                 << " static_candidate_ms=" << mCuller->getStaticCandidateMs()
                                 << " small_test_ms=" << mCuller->getSmallTestMs()
                                 << " terrain_cell_skips=" << mCuller->getTerrainCellOccluded()
                                 << "/" << mCuller->getTerrainCellTests()
                                 << " paged_chunk_skips=" << mCuller->getTerrainPagedOccluded()
                                 << "/" << mCuller->getTerrainPagedTests()
                                 << " static_candidates=" << mCuller->getStaticOccluderCandidates()
                                 << " static_skip_budget=" << mCuller->getStaticOccludersSkippedBudget()
                                 << " static_skip_distance=" << mCuller->getStaticOccludersSkippedDistance()
                                 << " static_skip_screen=" << mCuller->getStaticOccludersSkippedScreen()
                                 << " small_skip_screen=" << mCuller->getSmallOccludeesSkippedScreen();
                if (mStorage)
                {
                    const auto s = mStorage->getAndResetStats();
                    Log(Debug::Info) << "OcclusionCache: mem_hits=" << s.memHits
                                     << " db_hits=" << s.dbHits
                                     << " misses(built)=" << s.misses
                                     << " writes=" << s.writes;
                }
            }
        }
    }

    PagedOccluderCallback::PagedOccluderCallback(
        SceneUtil::OcclusionCuller* culler, float maxDistance, unsigned int maxTriangles)
        : mCuller(culler)
        , mMaxDistanceSq(maxDistance * maxDistance)
        , mMaxTriangles(maxTriangles)
    {
    }

    void PagedOccluderCallback::operator()(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        if (!mCuller->isFrameActive())
        {
            traverse(node, cv);
            return;
        }
        const osg::Timer_t callbackStart = osg::Timer::instance()->tick();
        double childTraverseMs = 0.0;

        struct Candidate
        {
            const OccluderMesh* mMesh = nullptr;
            unsigned int mTris = 0;
            double mScore = 0.0;
        };

        // Transform chunk bounding sphere from local to world space.
        // The chunk sits under a PAT, so node->getBound() is in chunk-local space.
        const osg::BoundingSphere& bs = node->getBound();
        if (bs.valid())
        {
            osg::Matrixd viewInverse;
            viewInverse.invert(cv->getCurrentCamera()->getViewMatrix());
            const osg::Matrixd modelToWorld = *cv->getModelViewMatrix() * viewInverse;
            const osg::Vec3f worldCenter = bs.center() * modelToWorld;
            const float r = bs.radius();

            osg::BoundingBox worldBB(worldCenter.x() - r, worldCenter.y() - r, worldCenter.z() - r, worldCenter.x() + r,
                worldCenter.y() + r, worldCenter.z() + r);

            // If entire chunk is occluded, skip rasterization AND traversal
            const bool chunkVisible = mCuller->testVisibleAABBTerrainOnly(worldBB);
            mCuller->recordTerrainPagedTest(chunkVisible);
            if (!chunkVisible)
            {
                const double overheadMs = osg::Timer::instance()->delta_m(callbackStart, osg::Timer::instance()->tick());
                mCuller->recordPagedCallback(overheadMs);
                mCuller->recordOcclusionOverhead(overheadMs);
                return;
            }

            // Rasterize nearby building occluder meshes for visible chunks
            const osg::Vec3f eyeWorld(viewInverse(3, 0), viewInverse(3, 1), viewInverse(3, 2));

            if (mCuller->staticOccludersEnabled() && mCuller->staticRasterBudgetAvailable())
            {
                std::vector<Candidate> candidates;
                candidates.reserve(64);
                if (auto* udc = node->getUserDataContainer())
                {
                    for (unsigned int i = 0; i < udc->getNumUserObjects(); ++i)
                    {
                        if (auto* pod = dynamic_cast<PagedOccluderData*>(udc->getUserObject(i)))
                        {
                            std::vector<const OccluderMesh*> nearbyOccluders;
                            nearbyOccluders.reserve(64);
                            pod->collectNearbyOccluders(eyeWorld, mMaxDistanceSq, nearbyOccluders);
                            for (const OccluderMesh* occMeshPtr : nearbyOccluders)
                            {
                                const OccluderMesh& occMesh = *occMeshPtr;
                                if (occMesh.indices.empty() || !occMesh.aabb.valid())
                                    continue;

                                mCuller->incrementStaticOccluderCandidates();

                                const osg::Vec3f center = occMesh.aabb.center();
                                if (occMesh.aabb.contains(eyeWorld))
                                    continue;

                                if ((center - eyeWorld).length2() > mMaxDistanceSq)
                                {
                                    mCuller->incrementStaticOccludersSkippedDistance();
                                    continue;
                                }

                                double screenRatio = 0.0;
                                if (!mCuller->estimateScreenRatio(occMesh.aabb, screenRatio)
                                    || screenRatio < mCuller->getMinOccluderScreenRatio())
                                {
                                    mCuller->incrementStaticOccludersSkippedScreen();
                                    continue;
                                }

                                const unsigned int newTris = static_cast<unsigned int>(occMesh.indices.size() / 3);
                                const double score = screenRatio / std::sqrt(static_cast<double>(std::max(1u, newTris)));
                                candidates.push_back(Candidate{ &occMesh, newTris, score });
                            }
                            break;
                        }
                    }
                }

                std::sort(candidates.begin(), candidates.end(),
                    [](const Candidate& left, const Candidate& right) { return left.mScore > right.mScore; });

                for (const Candidate& candidate : candidates)
                {
                    if (!mCuller->staticRasterBudgetAvailable())
                    {
                        mCuller->incrementStaticOccludersSkippedBudget();
                        break;
                    }

                    if (mMaxTriangles > 0 && mCuller->getNumBuildingTris() + candidate.mTris > mMaxTriangles)
                    {
                        mCuller->incrementStaticOccludersSkippedBudget();
                        continue;
                    }

                    mCuller->rasterizeOccluder(candidate.mMesh->vertices, candidate.mMesh->indices);
                    mCuller->incrementBuildingOccluders(
                        candidate.mTris, static_cast<unsigned int>(candidate.mMesh->vertices.size()));
                }
            }
        }

        const double overheadMs = osg::Timer::instance()->delta_m(callbackStart, osg::Timer::instance()->tick());
        mCuller->recordPagedCallback(overheadMs);
        mCuller->recordOcclusionOverhead(overheadMs);
        const osg::Timer_t traverseStart = osg::Timer::instance()->tick();
        traverse(node, cv);
        childTraverseMs += osg::Timer::instance()->delta_m(traverseStart, osg::Timer::instance()->tick());
        mCuller->recordChildTraverse(childTraverseMs);
    }

    CellOcclusionCallback::CellOcclusionCallback(SceneUtil::OcclusionCuller* culler, float occluderMinRadius,
        float occluderMaxRadius, float occluderShrinkFactor, int occluderMeshResolution, int occluderMaxMeshResolution,
        float occluderInsideThreshold, float occluderMaxDistance, bool enableStaticOccluders,
        unsigned int maxTriangles, OcclusionStorage* storage)
        : mCuller(culler)
        , mOccluderMinRadius(occluderMinRadius)
        , mOccluderMaxRadius(occluderMaxRadius)
        , mOccluderShrinkFactor(occluderShrinkFactor)
        , mOccluderMeshResolution(occluderMeshResolution)
        , mOccluderMaxMeshResolution(occluderMaxMeshResolution)
        , mOccluderInsideThreshold(occluderInsideThreshold)
        , mOccluderMaxDistanceSq(occluderMaxDistance * occluderMaxDistance)
        , mEnableStaticOccluders(enableStaticOccluders)
        , mMaxTriangles(maxTriangles)
        , mStorage(storage)
    {
    }

    const OccluderMesh& CellOcclusionCallback::getOccluderMesh(osg::Node* node)
    {
        auto it = mMeshCache.find(node);
        if (it != mMeshCache.end())
            return it->second;

        int meshRes = mOccluderMeshResolution;
        float radius = node->getBound().radius();
        if (radius > mOccluderMinRadius && mOccluderMinRadius > 0)
        {
            float scale = radius / mOccluderMinRadius;
            meshRes = std::clamp(
                static_cast<int>(mOccluderMeshResolution * scale), mOccluderMeshResolution, mOccluderMaxMeshResolution);
        }

        OccluderMesh mesh;
        OccluderMesh localMesh;
        const auto nodePaths = node->getParentalNodePaths();
        osg::Matrixf localToWorld;
        localToWorld.makeIdentity();
        if (!nodePaths.empty())
            localToWorld = osg::computeLocalToWorld(nodePaths.front());

        const std::string_view modelPath = getModelPathForNode(node);
        if (mStorage && mStorage->isOpen() && !modelPath.empty())
        {
            if (mStorage->get(modelPath, meshRes, OcclusionStorage::makeShrinkKey(mOccluderShrinkFactor), localMesh))
                mesh = transformLocalMesh(localMesh, localToWorld);
        }

        if (!mesh.aabb.valid() && mesh.vertices.empty() && mesh.indices.empty())
        {
            if (mStorage)
                mStorage->recordMiss();
            const osg::Timer_t start = osg::Timer::instance()->tick();
            localMesh = OcclusionCulling::buildSimplifiedMeshWithoutRootTransform(node, meshRes, mOccluderShrinkFactor);
            mCuller->recordMeshBuild(osg::Timer::instance()->delta_m(start, osg::Timer::instance()->tick()));
            // Persist to SQLite so future sessions skip buildSimplifiedMesh entirely.
            if (mStorage && mStorage->isOpen() && !modelPath.empty())
                mStorage->put(modelPath, meshRes, OcclusionStorage::makeShrinkKey(mOccluderShrinkFactor), localMesh);
            mesh = transformLocalMesh(localMesh, localToWorld);
        }

        return mMeshCache.emplace(node, std::move(mesh)).first->second;
    }

    void CellOcclusionCallback::operator()(osg::Group* node, osgUtil::CullVisitor* cv)
    {
        // If occlusion is not active this frame (interior, shadow camera, etc.), traverse normally
        if (!mCuller->isFrameActive())
        {
            traverse(node, cv);
            return;
        }
        const osg::Timer_t callbackStart = osg::Timer::instance()->tick();
        double childTraverseMs = 0.0;
        double staticCandidateMs = 0.0;
        double smallTestMs = 0.0;

        auto acceptChild = [&](osg::Node* child) {
            const osg::Timer_t start = osg::Timer::instance()->tick();
            child->accept(*cv);
            childTraverseMs += osg::Timer::instance()->delta_m(start, osg::Timer::instance()->tick());
        };
        auto recordTotals = [&]() {
            const double totalMs = osg::Timer::instance()->delta_m(callbackStart, osg::Timer::instance()->tick());
            mCuller->recordChildTraverse(childTraverseMs);
            mCuller->recordStaticCandidate(staticCandidateMs);
            mCuller->recordSmallTest(smallTestMs);
            mCuller->recordCellCallback(totalMs);
            mCuller->recordOcclusionOverhead(std::max(0.0, totalMs - childTraverseMs));
        };

        // Test cell bounding box against terrain-only depth — if fully hidden by terrain,
        // skip entire cell. Use terrain-only so buildings in adjacent cells don't
        // false-cull entire cells that are clearly in view.
        const osg::BoundingSphere& cellBS = node->getBound();
        if (cellBS.valid())
        {
            osg::BoundingBox cellBB;
            cellBB.expandBy(cellBS);

            const bool cellVisible = mCuller->testVisibleAABBTerrainOnly(cellBB);
            mCuller->recordTerrainCellTest(cellVisible);
            if (!cellVisible)
            {
                recordTotals();
                return; // Entire cell hidden by terrain — no children traversed
            }
        }

        if (!mCuller->smallObjectTestingEnabled())
        {
            const osg::Timer_t traverseStart = osg::Timer::instance()->tick();
            traverse(node, cv);
            childTraverseMs += osg::Timer::instance()->delta_m(traverseStart, osg::Timer::instance()->tick());
            recordTotals();
            return;
        }

        const unsigned int numChildren = node->getNumChildren();
        const bool allowStaticOccluders
            = mEnableStaticOccluders && mCuller->staticOccludersEnabled() && mCuller->staticRasterBudgetAvailable();

        struct Candidate
        {
            const OccluderMesh* mMesh = nullptr;
            unsigned int mTris = 0;
            double mScore = 0.0;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(64);

        auto addCandidate = [&](const OccluderMesh& mesh, const osg::Vec3f& distanceCenter) {
            if (!mesh.aabb.valid() || mesh.indices.empty())
                return;

            mCuller->incrementStaticOccluderCandidates();

            const float distSq = (distanceCenter - cv->getEyePoint()).length2();
            if (distSq >= mOccluderMaxDistanceSq)
            {
                mCuller->incrementStaticOccludersSkippedDistance();
                return;
            }

            osg::Vec3f center = mesh.aabb.center();
            osg::Vec3f halfExtent = (osg::Vec3f(mesh.aabb.xMax(), mesh.aabb.yMax(), mesh.aabb.zMax()) - center)
                * mOccluderInsideThreshold;
            osg::BoundingBox scaledBB;
            scaledBB.expandBy(center - halfExtent);
            scaledBB.expandBy(center + halfExtent);
            if (scaledBB.contains(cv->getEyePoint()))
                return;

            double screenRatio = 0.0;
            if (!mCuller->estimateScreenRatio(mesh.aabb, screenRatio)
                || screenRatio < mCuller->getMinOccluderScreenRatio())
            {
                mCuller->incrementStaticOccludersSkippedScreen();
                return;
            }

            if (!mCuller->testVisibleAABBTerrainOnly(mesh.aabb))
                return;

            const unsigned int newTris = static_cast<unsigned int>(mesh.indices.size() / 3);
            const double score = screenRatio / std::sqrt(static_cast<double>(std::max(1u, newTris)));
            candidates.push_back(Candidate{ &mesh, newTris, score });
        };

        // Pass 1: Large objects — test against terrain depth, optionally rasterize as occluders
        for (unsigned int i = 0; i < numChildren; ++i)
        {
            osg::Node* child = node->getChild(i);
            const osg::BoundingSphere& bs = child->getBound();

            if (!bs.valid() || bs.radius() < mOccluderMinRadius)
                continue;

            // Paged chunks and other oversized objects — test visibility, rasterize stored occluders
            if (bs.radius() > mOccluderMaxRadius)
            {
                osg::BoundingBox pageBB;
                pageBB.expandBy(bs);
                const bool pageVisible = mCuller->testVisibleAABBTerrainOnly(pageBB);
                mCuller->recordTerrainPagedTest(pageVisible);
                if (!pageVisible)
                    continue;

                // Rasterize sub-object occluder meshes stored at chunk creation time
                if (allowStaticOccluders)
                {
                    if (auto* udc = child->getUserDataContainer())
                    {
                        for (unsigned int j = 0; j < udc->getNumUserObjects(); ++j)
                        {
                            if (auto* pod = dynamic_cast<PagedOccluderData*>(udc->getUserObject(j)))
                            {
                                std::vector<const OccluderMesh*> nearbyOccluders;
                                nearbyOccluders.reserve(64);
                                pod->collectNearbyOccluders(cv->getEyePoint(), mOccluderMaxDistanceSq,
                                    nearbyOccluders);
                                for (const OccluderMesh* occMesh : nearbyOccluders)
                                {
                                    const osg::Timer_t start = osg::Timer::instance()->tick();
                                    addCandidate(*occMesh, occMesh->aabb.center());
                                    staticCandidateMs += osg::Timer::instance()->delta_m(
                                        start, osg::Timer::instance()->tick());
                                }
                                break; // Only one PagedOccluderData per chunk
                            }
                        }
                    }
                }

                // Test chunk visibility against terrain-only depth — paged chunks are large
                // geometry that should only be culled by terrain, not adjacent buildings.
                acceptChild(child);
                continue;
            }

            // Get cached occluder mesh (with AABB for visibility test)
            const OccluderMesh& mesh = getOccluderMesh(child);

            // Rasterize as occluder if in range and camera is not inside the building.
            // Test against terrain-only buffer so other buildings don't prevent rasterization
            // of adjacent buildings (which would reduce culling coverage for Pass 2).
            if (allowStaticOccluders)
            {
                const osg::Timer_t start = osg::Timer::instance()->tick();
                addCandidate(mesh, bs.center());
                staticCandidateMs += osg::Timer::instance()->delta_m(start, osg::Timer::instance()->tick());
            }

            // Always traverse large buildings. Do NOT gate traversal on testVisibleAABB —
            // buildings testing against a buffer that includes previously rasterized
            // buildings causes false culling (flickering) when child ordering happens to
            // place one building in front of another in the depth buffer. Large buildings
            // are correctly culled by PVS and the cell-level AABB test above; MSOC
            // is reserved for culling small objects in Pass 2.
            acceptChild(child);
        }

        std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) { return left.mScore > right.mScore; });

        for (const Candidate& candidate : candidates)
        {
            if (!mCuller->staticRasterBudgetAvailable())
            {
                mCuller->incrementStaticOccludersSkippedBudget();
                break;
            }

            if (mMaxTriangles > 0 && mCuller->getNumBuildingTris() + candidate.mTris > mMaxTriangles)
            {
                mCuller->incrementStaticOccludersSkippedBudget();
                continue;
            }

            mCuller->rasterizeOccluder(candidate.mMesh->vertices, candidate.mMesh->indices);
            mCuller->incrementBuildingOccluders(
                candidate.mTris, static_cast<unsigned int>(candidate.mMesh->vertices.size()));
        }

        // Pass 2: Small objects — test against enriched depth buffer (terrain + buildings)
        for (unsigned int i = 0; i < numChildren; ++i)
        {
            osg::Node* child = node->getChild(i);
            const osg::BoundingSphere& bs = child->getBound();

            if (!bs.valid())
            {
                acceptChild(child);
                continue;
            }

            if (bs.radius() >= mOccluderMinRadius)
                continue; // Already handled in pass 1

            // Never occlude doors — they sit flush against building surfaces
            // and are easily falsely hidden by the parent building's AABB occluder
            bool skipOcclusion = false;
            child->getUserValue("skipOcclusion", skipOcclusion);

            osg::BoundingBox childBB;
            childBB.expandBy(bs);

            if (skipOcclusion || !mCuller->smallObjectTestingEnabled())
            {
                acceptChild(child);
                continue;
            }

            const osg::Timer_t smallTestStart = osg::Timer::instance()->tick();
            double screenRatio = 0.0;
            if (mCuller->estimateScreenRatio(childBB, screenRatio)
                && screenRatio < mCuller->getMinOccludeeScreenRatio())
            {
                mCuller->incrementSmallOccludeesSkippedScreen();
                smallTestMs += osg::Timer::instance()->delta_m(smallTestStart, osg::Timer::instance()->tick());
                acceptChild(child);
                continue;
            }

            if (mCuller->testVisibleAABB(childBB))
            {
                smallTestMs += osg::Timer::instance()->delta_m(smallTestStart, osg::Timer::instance()->tick());
                acceptChild(child);
            }
            else
                smallTestMs += osg::Timer::instance()->delta_m(smallTestStart, osg::Timer::instance()->tick());
            // else: occluded — skip
        }
        recordTotals();
    }
}
