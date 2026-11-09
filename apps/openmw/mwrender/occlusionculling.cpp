#include "occlusionculling.hpp"

#include "vismask.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <osg/BoundingBox>
#include <osg/BoundingSphere>
#include <osg/Camera>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Stats>
#include <osg/Timer>
#include <osg/Transform>
#include <osgUtil/CullVisitor>

#include <components/debug/debuglog.hpp>
#include <components/misc/constants.hpp>
#include <components/occlusionculling/occludermesh.hpp>
#include <components/sceneutil/enhancedsettings.hpp>
#include <components/sceneutil/occlusioncallback.hpp>
#include <components/sceneutil/occlusionculling.hpp>
#include <components/sceneutil/occlusiontraversal.hpp>
#include <components/sceneutil/renderstageinventory.hpp>
#include <components/sceneutil/userdata.hpp>
#include <components/sceneutil/util.hpp>
#include <components/sceneutil/viewpreparation.hpp>
#include <components/settings/values.hpp>
#include <components/terrain/terrainoccluder.hpp>

namespace MWRender
{
    namespace
    {
        class StaticCandidateTimer
        {
        public:
            explicit StaticCandidateTimer(SceneUtil::OcclusionCuller& culler)
                : mCuller(culler)
                , mStart(osg::Timer::instance()->tick())
            {
            }

            ~StaticCandidateTimer()
            {
                mCuller.recordStaticCandidate(osg::Timer::instance()->delta_m(mStart, osg::Timer::instance()->tick()));
            }

        private:
            SceneUtil::OcclusionCuller& mCuller;
            osg::Timer_t mStart;
        };

    }

    void PagedOccluderData::buildSpatialIndex()
    {
        mIndexed = mOccluderMeshes.size() >= 64;
        if (mIndexed)
            mIndex.build(mOccluderMeshes, [](const auto& mesh) { return osg::BoundingSphere(mesh->aabb); });
    }

    void PagedOccluderData::collectNearbyOccluders(const osg::Vec3f& eye, float maxDistanceSq,
        std::vector<std::shared_ptr<const OccluderMesh>>& out, std::vector<std::size_t>& candidates) const
    {
        const auto append = [&](const auto& mesh) {
            if (mesh->aabb.valid() && SceneUtil::occlusionDistanceSquared(mesh->aabb, eye) <= maxDistanceSq)
                out.push_back(mesh);
        };
        // The existing BVH is a conservative broad phase. The final predicate
        // uses distance to the full mesh bounds, not its centroid.
        if (mIndexed && mIndex.queryCandidates(osg::BoundingSphere(eye, std::sqrt(maxDistanceSq)), candidates))
            for (const auto index : candidates)
                append(mOccluderMeshes[index]);
        else
            for (const auto& mesh : mOccluderMeshes)
                append(mesh);
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
        osg::Camera* cam = cv->getCurrentCamera();
        const std::string& cameraName = cam->getName();
        const bool sceneCamera = cameraName == Constants::SceneCamera;

        if (!sceneCamera)
        {
            traverse(node, cv);
            return;
        }

        // A single buffer cannot represent nested cameras or concurrent cull
        // traversals. Claim it for this scoped world traversal; other traversals
        // render normally instead of reading or overwriting another view.
        std::unique_lock traversalLock(mTraversalMutex, std::try_to_lock);
        if (!traversalLock.owns_lock())
        {
            traverse(node, cv);
            return;
        }
        const unsigned int frameNumber = cv->getFrameStamp()->getFrameNumber();
        // MOC depth is reciprocal clip W and cannot represent orthographic depth.
        if ((*cv->getProjectionMatrix())(3, 3) != 0.0)
        {
            traverse(node, cv);
            return;
        }

        // Skip MSOC entirely in interiors (unless enabled via setting)
        if (mIsInterior && !mEnableInteriors)
        {
            traverse(node, cv);
            return;
        }

        // Begin occlusion frame with camera matrices
        const osg::Timer_t beginStart = osg::Timer::instance()->tick();
        mCuller->beginFrame(
            *cv->getModelViewMatrix(), *cv->getProjectionMatrix(), Settings::camera().mNearClip.get(), true);
        if (!mCuller->isFrameActive())
        {
            traverse(node, cv);
            return;
        }
        mTerrainBuildMs = 0.0;
        mTerrainRasterMs = 0.0;
        mBeginCallbackMs = 0.0;
        mEndCallbackMs = 0.0;
        mTerrainVerts = 0;
        mTerrainTris = 0;

        bool smallObjectTestingEnabled = SceneUtil::Enhanced::occlusionCullingSmallObjects();
        bool staticOccludersEnabled = SceneUtil::Enhanced::occlusionCullingStaticOccluders();

        const bool adaptiveStatics = sceneCamera && SceneUtil::Enhanced::occlusionAdaptiveStatics();
        const bool throttled = adaptiveStatics && mAdaptiveStaticCooldownFrames > 0;
        if (throttled)
        {
            --mAdaptiveStaticCooldownFrames;
        }
        if (!smallObjectTestingEnabled)
            staticOccludersEnabled = false;

        mCuller->setStaticOccludersEnabled(staticOccludersEnabled);
        mCuller->setSmallObjectTestingEnabled(smallObjectTestingEnabled);
        mCuller->setStaticRasterBudgetMs(
            SceneUtil::adaptiveOcclusionBudget(SceneUtil::Enhanced::occlusionStaticRasterTimeBudgetMs(), throttled));
        mCuller->setMinOccluderScreenRatio(SceneUtil::Enhanced::occlusionMinOccluderScreenRatio());
        mCuller->setMinOccludeeScreenRatio(SceneUtil::Enhanced::occlusionMinOccludeeScreenRatio());

        mActiveFrameNumber = frameNumber;
        mActiveCameraName = cameraName;
        mActiveSceneCamera = sceneCamera;
        mActiveAdaptiveStatics = adaptiveStatics;
        mActiveStaticOccludersEnabled = staticOccludersEnabled;
        mActiveSmallObjectTestingEnabled = smallObjectTestingEnabled;
        mFrameStarted = true;
        auto* stats = cam->getStats();
        const bool inventory = stats && stats->collectStats("rendering");
        const auto before = inventory ? SceneUtil::countRenderSubmissions(*cv->getCurrentRenderStage())
                                      : SceneUtil::SubmissionCounts{};
        const auto beforeNested = inventory ? SceneUtil::countDependentSubmissions(*cv->getCurrentRenderStage())
                                            : SceneUtil::SubmissionCounts{};
        try
        {
            // Prepare the exact view selected by terrain/paging without a cull
            // traversal. No render leaves, light lists or skinning run here.
            mPlan.start();
            if (staticOccludersEnabled)
            {
                StaticCandidateTimer timer(*mCuller);
                SceneUtil::OcclusionPreparationVisitor prepare(
                    *cv, mPlan, { Mask_Object, Mask_Actor, Mask_Groundcover });
                node->accept(prepare);
            }
            const bool terrain = mEnableTerrainOccluder && (cv->getTraversalMask() & Mask_Terrain) && !mIsQuasiExterior
                && !mIsInterior && mTerrainOccluder->hasTerrainData();
            if (terrain)
            {
                const auto start = osg::Timer::instance()->tick();
                mTerrainOccluder->update(cv->getEyePoint(), mRadiusCells);
                mTerrainBuildMs = osg::Timer::instance()->delta_m(start, osg::Timer::instance()->tick());
                mTerrainVerts = mTerrainOccluder->getPositions().size();
                mTerrainTris = mTerrainOccluder->getIndices().size() / 3;
            }
            SceneUtil::OcclusionViewKey key;
            key.mCamera = cam;
            key.mView = *cv->getModelViewMatrix();
            key.mProjection = *cv->getProjectionMatrix();
            const auto* viewport = cv->getViewport();
            key.mViewport.set(viewport->x(), viewport->y(), viewport->width(), viewport->height());
            key.mPlanes = cv->getCurrentCullingSet().getFrustum().getPlaneList();
            key.mTerrainRevision = mTerrainOccluder->getRevision();
            key.mTerrain = terrain;
            key.mStatics = staticOccludersEnabled;
            key.mTraversalMask = cv->getTraversalMask();
            key.mCullingMode = cv->getCurrentCullingSet().getCullingMask();
            key.mSmallFeature = cv->getCurrentCullingSet().getSmallFeatureCullingPixelSize();
            key.mLodScale = cv->getLODScale();
            key.mNear = Settings::camera().mNearClip.get();
            key.mMaxTriangles = SceneUtil::Enhanced::occlusionMaxTriangles();
            key.mMinOccluder = SceneUtil::Enhanced::occlusionMinOccluderScreenRatio();
            key.mBudget = SceneUtil::Enhanced::occlusionStaticRasterTimeBudgetMs();
            bool reused;
            {
                StaticCandidateTimer timer(*mCuller);
                reused = mPlan.prepare(key);
            }
            if (!reused)
            {
                mCuller->clearDepth();
                if (terrain)
                {
                    const auto start = osg::Timer::instance()->tick();
                    mCuller->rasterizeTerrainOccluder(mTerrainOccluder->getPositions(), mTerrainOccluder->getIndices(),
                        staticOccludersEnabled || smallObjectTestingEnabled);
                    mTerrainRasterMs = osg::Timer::instance()->delta_m(start, osg::Timer::instance()->tick());
                }
            }
            mCuller->resetStaticRasterBudgetBaseline();
            mPlan.rasterize(*mCuller);
            mBeginCallbackMs = osg::Timer::instance()->delta_m(beginStart, osg::Timer::instance()->tick());
            if (auto* stats = cam->getStats(); stats && stats->collectStats("rendering"))
            {
                stats->setAttribute(frameNumber, "Occlusion depth reused", reused);
                stats->setAttribute(frameNumber, "Occlusion plan complete", mPlan.complete());
            }
            mCuller->bindVisitor(*cv);
            if (inventory)
                stats->setAttribute(frameNumber, "Occlusion preparation submitted leaves",
                    SceneUtil::countRenderSubmissions(*cv->getCurrentRenderStage()).mLeaves - before.mLeaves);
            traverse(node, cv);
            if (inventory)
            {
                const auto after = SceneUtil::countRenderSubmissions(*cv->getCurrentRenderStage());
                const auto nested = SceneUtil::countDependentSubmissions(*cv->getCurrentRenderStage());
                stats->setAttribute(frameNumber, "World submitted leaves", after.mLeaves - before.mLeaves);
                stats->setAttribute(frameNumber, "World submitted geometry", after.mGeometry - before.mGeometry);
                stats->setAttribute(
                    frameNumber, "World submitted primitive sets", after.mPrimitiveSets - before.mPrimitiveSets);
                stats->setAttribute(frameNumber, "World dependent geometry", nested.mGeometry - beforeNested.mGeometry);
            }
            endFrame(node, cv);
        }
        catch (...)
        {
            mPlan.invalidate();
            mCuller->endFrame();
            mFrameStarted = false;
            throw;
        }
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
        if (auto* stats = cam->getStats(); stats && stats->collectStats("rendering"))
        {
            stats->setAttribute(frameNumber, "Occlusion objects tested", mCuller->getNumTested());
            stats->setAttribute(frameNumber, "Occlusion objects skipped", mCuller->getNumOccluded());
            stats->setAttribute(frameNumber, "Occlusion cells skipped", mCuller->getTerrainCellOccluded());
            stats->setAttribute(frameNumber, "Occlusion chunks skipped", mCuller->getTerrainPagedOccluded());
            stats->setAttribute(frameNumber, "Occlusion static triangles", mCuller->getNumBuildingTris());
            stats->setAttribute(frameNumber, "Occlusion CPU overhead time taken", occlusionOverheadMs * 0.001);
            stats->setAttribute(frameNumber, "Occlusion candidate time taken", mCuller->getStaticCandidateMs() * 0.001);
            stats->setAttribute(
                frameNumber, "Occlusion static raster time taken", mCuller->getStaticRasterMs() * 0.001);
        }
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
            const bool lowBenefit = benefitRatio < SceneUtil::Enhanced::occlusionAdaptiveMinBenefitRatio();
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
                    Log(Debug::Info) << "OcclusionCull: adaptive static budget reduced; visibility tests remain active"
                                     << " begin_marker_ms=" << mBeginCallbackMs
                                     << " child_traverse_ms=" << mCuller->getChildTraverseMs()
                                     << " callback_overhead_ms=" << callbackOverheadMs
                                     << " overhead_ms=" << occlusionOverheadMs << " tested=" << tested
                                     << " occluded=" << occluded
                                     << " terrain_cell_skips=" << mCuller->getTerrainCellOccluded() << "/"
                                     << mCuller->getTerrainCellTests()
                                     << " paged_chunk_skips=" << mCuller->getTerrainPagedOccluded() << "/"
                                     << mCuller->getTerrainPagedTests() << " benefit=" << benefitRatio
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
                const auto terrainTris = mTerrainTris;
                const auto bldgTris = mCuller->getNumBuildingTris();
                const auto terrainVerts = mTerrainVerts;
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
                    Log(Debug::Info) << "OcclusionCache: mem_hits=" << s.memHits << " db_hits=" << s.dbHits
                                     << " misses(built)=" << s.misses << " writes=" << s.writes
                                     << " ready_hits=" << s.readyHits << " queued=" << s.queued
                                     << " deferred=" << s.deferred << " rejected=" << s.rejected;
                }
            }
        }
    }

    PagedOccluderCallback::PagedOccluderCallback(SceneUtil::OcclusionCuller* culler)
        : mCuller(culler)
        , mMaxDistanceSq(std::pow(SceneUtil::Enhanced::occlusionOccluderMaxDistance(), 2))
    {
    }

    void PagedOccluderCallback::prepareOccluders(
        osg::Node& node, const osg::Matrixd& matrix, osgUtil::CullVisitor& cv, SceneUtil::OcclusionPlan& plan)
    {
        auto bound = node.getBound();
        SceneUtil::transformBoundingSphere(matrix, bound);
        if (node.isCullingActive() && !SceneUtil::maySubmitOccluderSphere(bound, cv.getCurrentCullingSet()))
            return;
        if (const auto* data = node.getUserDataContainer())
            for (unsigned int i = 0; i < data->getNumUserObjects(); ++i)
                if (const auto* paged = dynamic_cast<const PagedOccluderData*>(data->getUserObject(i)))
                {
                    auto& nearby = plan.nearbyScratch();
                    paged->collectNearbyOccluders(cv.getEyePoint(), mMaxDistanceSq, nearby, plan.indexScratch());
                    for (const auto& mesh : nearby)
                        plan.add(*mCuller, { mesh, {}, mesh->aabb, false });
                    break;
                }
    }

    void PagedOccluderCallback::operator()(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        if (!mCuller->isFrameActiveFor(*cv))
        {
            traverse(node, cv);
            return;
        }
        const osg::Timer_t callbackStart = osg::Timer::instance()->tick();
        double childTraverseMs = 0.0;

        // Transform chunk bounding sphere from local to world space.
        // The chunk sits under a PAT, so node->getBound() is in chunk-local space.
        const osg::BoundingSphere& bs = node->getBound();
        if (SceneUtil::mayTestOcclusionBounds(*node))
        {
            const osg::Matrixd& viewInverse = mCuller->getViewInverse();
            const osg::Matrixd modelToWorld = *cv->getModelViewMatrix() * viewInverse;

            osg::BoundingBox localBB;
            localBB.expandBy(bs);
            osg::BoundingBox worldBB;
            for (unsigned int i = 0; i < 8; ++i)
                worldBB.expandBy(localBB.corner(i) * modelToWorld);

            // Complete static depth is now available before any chunks draw.
            // Reject a whole hidden chunk before lighting or leaf queries.
            const bool terrainVisible = mCuller->testVisibleAABBTerrainOnly(worldBB);
            mCuller->recordTerrainPagedTest(terrainVisible);
            const bool chunkVisible
                = terrainVisible && (!mCuller->smallObjectTestingEnabled() || mCuller->testVisibleAABB(worldBB));
            if (!chunkVisible)
            {
                const double overheadMs = osg::Timer::instance()->delta_m(callbackStart, osg::Timer::instance()->tick());
                mCuller->recordPagedCallback(overheadMs);
                mCuller->recordOcclusionOverhead(overheadMs);
                return;
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
        float occluderMaxRadius, float occluderMaxDistance, bool enableStaticOccluders, OcclusionStorage* storage)
        : mCuller(culler)
        , mOccluderMinRadius(occluderMinRadius)
        , mOccluderMaxRadius(occluderMaxRadius)
        , mOccluderMaxDistanceSq(occluderMaxDistance * occluderMaxDistance)
        , mEnableStaticOccluders(enableStaticOccluders)
        , mStorage(storage)
    {
    }

    void CellOcclusionCallback::registerOccluder(osg::Node* node, StaticOccluderData* data)
    {
        mPlacements.insert_or_assign(node, PlacementEntry{ node, data });
    }

    void CellOcclusionCallback::collectOccluders(
        osg::Group& cell, const osg::Matrixd& parentToWorld, osgUtil::CullVisitor& cv, SceneUtil::OcclusionPlan& plan)
    {
        if (!mEnableStaticOccluders || !mCuller->staticOccludersEnabled())
            return;
        for (auto it = mPlacements.begin(); it != mPlacements.end();)
        {
            osg::ref_ptr<osg::Node> node;
            bool belongs = false;
            if (it->second.mNode.lock(node))
                for (unsigned int i = 0; i < node->getNumParents(); ++i)
                    if (node->getParent(i) == &cell)
                    {
                        belongs = true;
                        break;
                    }
            if (!belongs)
            {
                it = mPlacements.erase(it);
                continue;
            }
            auto& data = *it->second.mData;
            ++it;
            if (!cv.validNodeMask(*node) || !SceneUtil::mayTestOcclusionBounds(*node))
                continue;
            bool opaque = true;
            node->getUserValue("occlusionOpaque", opaque);
            const auto* visual = data.mVisual.get();
            if (!opaque || !visual || !cv.validNodeMask(*visual) || !node->asGroup()
                || !node->asGroup()->containsNode(visual))
                continue;
            const float radius = node->getBound().radius();
            if (radius < mOccluderMinRadius || radius > mOccluderMaxRadius)
                continue;
            auto bound = node->getBound();
            SceneUtil::transformBoundingSphere(parentToWorld, bound);
            if (!SceneUtil::maySubmitOccluderSphere(bound, cv.getCurrentCullingSet()))
                continue;
            if (!data.mMesh && mStorage)
                data.mMesh = mStorage->getReady(data.mModel, data.mResolution, data.mShrink);
            if (!data.mMesh || data.mMesh->indices.empty())
                continue;
            osg::Matrixd matrix = parentToWorld;
            if (const auto* transform = node->asTransform())
            {
                if (transform->getReferenceFrame() != osg::Transform::RELATIVE_RF)
                    continue;
                transform->computeLocalToWorldMatrix(matrix, nullptr);
            }
            const osg::Matrixf placement(matrix);
            if (data.mPlacement != placement || !data.mBounds.valid())
            {
                data.mPlacement = placement;
                data.mBounds = SceneUtil::occlusionWorldBounds(data.mMesh->aabb, placement);
            }
            if (SceneUtil::occlusionDistanceSquared(data.mBounds, cv.getEyePoint()) > mOccluderMaxDistanceSq)
                continue;
            plan.add(*mCuller, { data.mMesh, placement, data.mBounds, true });
        }
    }

    void CellOcclusionCallback::operator()(osg::Group* node, osgUtil::CullVisitor* cv)
    {
        // If occlusion is not active this frame (interior, shadow camera, etc.), traverse normally
        if (!mCuller->isFrameActiveFor(*cv))
        {
            traverse(node, cv);
            return;
        }
        const osg::Timer_t callbackStart = osg::Timer::instance()->tick();
        double childTraverseMs = 0.0;
        double smallTestMs = 0.0;

        auto acceptChild = [&](osg::Node* child) {
            const osg::Timer_t start = osg::Timer::instance()->tick();
            child->accept(*cv);
            childTraverseMs += osg::Timer::instance()->delta_m(start, osg::Timer::instance()->tick());
        };
        auto recordTotals = [&]() {
            const double totalMs = osg::Timer::instance()->delta_m(callbackStart, osg::Timer::instance()->tick());
            mCuller->recordChildTraverse(childTraverseMs);
            mCuller->recordSmallTest(smallTestMs);
            mCuller->recordCellCallback(totalMs);
            mCuller->recordOcclusionOverhead(std::max(0.0, totalMs - childTraverseMs));
        };

        const osg::Matrixd parentToWorld = *cv->getModelViewMatrix() * mCuller->getViewInverse();
        // Test cell bounding box against terrain-only depth — if fully hidden by terrain,
        // skip entire cell. Use terrain-only so buildings in adjacent cells don't
        // false-cull entire cells that are clearly in view.
        const osg::BoundingSphere& cellBS = node->getBound();
        if (SceneUtil::mayTestOcclusionBounds(*node))
        {
            osg::BoundingBox cellBB;
            cellBB.expandBy(cellBS);

            const bool cellVisible
                = mCuller->testVisibleAABBTerrainOnly(SceneUtil::occlusionWorldBounds(cellBB, parentToWorld));
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

        // Pass 2: Test all object sizes before lighting, skinning, or draw submission.
        for (unsigned int i = 0; i < numChildren; ++i)
        {
            osg::Node* child = node->getChild(i);
            if (!cv->validNodeMask(*child))
                continue;
            const osg::BoundingSphere& bs = child->getBound();

            if (!bs.valid())
            {
                acceptChild(child);
                continue;
            }

            if (!SceneUtil::mayTraverseForOcclusion(*child, *cv, cv->getCurrentCullingSet()))
                continue;

            osg::BoundingBox childBB;
            childBB.expandBy(bs);

            // Preserve explicit exclusions, including doors and disabled culling.
            if (!SceneUtil::mayTestOcclusionBounds(*child))
            {
                acceptChild(child);
                continue;
            }

            const osg::Timer_t smallTestStart = osg::Timer::instance()->tick();
            if (mCuller->testVisibleAABB(SceneUtil::occlusionWorldBounds(childBB, parentToWorld)))
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
