#include "occlusionculling.hpp"

#include "objects.hpp"
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

#include "../mwworld/class.hpp"
#include <components/debug/debuglog.hpp>
#include <components/misc/constants.hpp>
#include <components/occlusionculling/occludermesh.hpp>
#include <components/sceneutil/enhancedsettings.hpp>
#include <components/sceneutil/occlusionculling.hpp>
#include <components/sceneutil/occlusiontraversal.hpp>
#include <components/sceneutil/userdata.hpp>
#include <components/settings/values.hpp>
#include <components/terrain/terrainoccluder.hpp>

namespace MWRender
{
    namespace
    {
        constexpr unsigned int PagedOccluderBinDim = 16;

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

        VFS::Path::Normalized getModelPathForNode(osg::Node* node)
        {
            if (node)
                if (const auto* ptr = SceneUtil::findUserData<MWWorld::Ptr>(*node))
                    return ptr->getClass().getCorrectedModel(*ptr);
            return {};
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
        if (const auto it = mLastFrameNumbers.find(cameraName);
            it != mLastFrameNumbers.end() && it->second == frameNumber)
        {
            traverse(node, cv);
            return;
        }
        mLastFrameNumbers[cameraName] = frameNumber;
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
        mCuller->beginFrame(*cv->getModelViewMatrix(), *cv->getProjectionMatrix(), Settings::camera().mNearClip.get());
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
        if (mEnableTerrainOccluder && (cv->getTraversalMask() & Mask_Terrain) && !mIsQuasiExterior && !mIsInterior
            && mTerrainOccluder->hasTerrainData())
        {
            osg::Timer_t start = osg::Timer::instance()->tick();
            mTerrainOccluder->update(cv->getEyePoint(), mRadiusCells);
            const auto& positions = mTerrainOccluder->getPositions();
            const auto& indices = mTerrainOccluder->getIndices();
            mTerrainVerts = positions.size();
            mTerrainTris = indices.size() / 3;
            mTerrainBuildMs = osg::Timer::instance()->delta_m(start, osg::Timer::instance()->tick());

            if (!positions.empty())
            {
                start = osg::Timer::instance()->tick();
                const bool needFullBuffer = staticOccludersEnabled || smallObjectTestingEnabled;
                mCuller->rasterizeTerrainOccluder(positions, indices, needFullBuffer);
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
        mCuller->bindVisitor(*cv);
        try
        {
            traverse(node, cv);
            endFrame(node, cv);
        }
        catch (...)
        {
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

    PagedOccluderCallback::PagedOccluderCallback(
        SceneUtil::OcclusionCuller* culler, float maxDistance, unsigned int maxTriangles)
        : mCuller(culler)
        , mMaxDistanceSq(maxDistance * maxDistance)
        , mMaxTriangles(maxTriangles)
    {
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
            const osg::Matrixd& viewInverse = mCuller->getViewInverse();
            const osg::Matrixd modelToWorld = *cv->getModelViewMatrix() * viewInverse;

            osg::BoundingBox localBB;
            localBB.expandBy(bs);
            osg::BoundingBox worldBB;
            for (unsigned int i = 0; i < 8; ++i)
                worldBB.expandBy(localBB.corner(i) * modelToWorld);

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

            if (mCuller->staticOccludersEnabled() && mCuller->staticCandidateBudgetAvailable())
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
                            {
                                StaticCandidateTimer timer(*mCuller);
                                pod->collectNearbyOccluders(eyeWorld, mMaxDistanceSq, nearbyOccluders);
                            }
                            for (const OccluderMesh* occMeshPtr : nearbyOccluders)
                            {
                                if (!mCuller->staticCandidateBudgetAvailable())
                                {
                                    mCuller->incrementStaticOccludersSkippedBudget();
                                    break;
                                }
                                StaticCandidateTimer timer(*mCuller);
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

                {
                    StaticCandidateTimer timer(*mCuller);
                    std::sort(candidates.begin(), candidates.end(),
                        [](const Candidate& left, const Candidate& right) { return left.mScore > right.mScore; });
                }

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

    const CellOcclusionCallback::CachedMesh* CellOcclusionCallback::getOccluderMesh(
        osg::Node* node, const osg::Matrixd& parentToWorld)
    {
        if (!mStorage)
            return nullptr;
        bool skip = false;
        node->getUserValue("skipOcclusion", skip);
        if (skip)
            return nullptr;
        bool eligible = false;
        node->getUserValue("staticOccluderEligible", eligible);
        if (!eligible)
            return nullptr;
        const auto model = getModelPathForNode(node);
        if (model.empty())
            return nullptr;
        osg::Matrixd matrix = parentToWorld;
        if (const auto* transform = node->asTransform())
            transform->computeLocalToWorldMatrix(matrix, nullptr);
        const osg::Matrixf placement(matrix);
        if (const auto it = mMeshCache.find(node); it != mMeshCache.end() && it->second.mNode.get() == node
            && it->second.mPlacement == placement && it->second.mModel == model.value())
            return &it->second;

        const float radius = node->getBound().radius();
        const int meshRes
            = std::clamp(static_cast<int>(mOccluderMeshResolution * (radius / std::max(mOccluderMinRadius, 1.f))),
                mOccluderMeshResolution, mOccluderMaxMeshResolution);
        const auto ready
            = mStorage->getReady(model.value(), meshRes, OcclusionStorage::makeShrinkKey(mOccluderShrinkFactor));
        if (!ready)
            return nullptr; // No I/O, mesh generation, or waiting on the cull thread.
        auto& cached = mMeshCache[node];
        cached.mNode = node;
        cached.mPlacement = placement;
        cached.mModel = model.value();
        cached.mMesh = ready;
        cached.mWorldBounds.init();
        if (ready->aabb.valid())
            for (unsigned int i = 0; i < 8; ++i)
                cached.mWorldBounds.expandBy(ready->aabb.corner(i) * placement);
        return &cached;
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

        const osg::Matrixd parentToWorld = *cv->getModelViewMatrix() * mCuller->getViewInverse();
        const unsigned int numChildren = node->getNumChildren();
        const bool allowStaticOccluders
            = mEnableStaticOccluders && mCuller->staticOccludersEnabled() && mCuller->staticCandidateBudgetAvailable();

        struct Candidate
        {
            const OccluderMesh* mMesh = nullptr;
            const osg::Matrixf* mTransform = nullptr;
            unsigned int mTris = 0;
            double mScore = 0.0;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(64);

        auto addCandidate = [&](const OccluderMesh& mesh, const osg::BoundingBox& bounds,
                                const osg::Vec3f& distanceCenter, const osg::Matrixf* transform = nullptr) {
            if (!bounds.valid() || mesh.indices.empty())
                return;

            mCuller->incrementStaticOccluderCandidates();

            const float distSq = (distanceCenter - cv->getEyePoint()).length2();
            if (distSq >= mOccluderMaxDistanceSq)
            {
                mCuller->incrementStaticOccludersSkippedDistance();
                return;
            }

            osg::Vec3f center = bounds.center();
            osg::Vec3f halfExtent
                = (osg::Vec3f(bounds.xMax(), bounds.yMax(), bounds.zMax()) - center) * mOccluderInsideThreshold;
            osg::BoundingBox scaledBB;
            scaledBB.expandBy(center - halfExtent);
            scaledBB.expandBy(center + halfExtent);
            if (scaledBB.contains(cv->getEyePoint()))
                return;

            SceneUtil::OcclusionCuller::ProjectedBounds projected;
            if (!mCuller->projectBounds(bounds, projected)
                || projected.screenRatio() < mCuller->getMinOccluderScreenRatio())
            {
                mCuller->incrementStaticOccludersSkippedScreen();
                return;
            }

            if (!mCuller->testVisibleAABBTerrainOnly(projected))
                return;

            const unsigned int newTris = static_cast<unsigned int>(mesh.indices.size() / 3);
            const double score = projected.screenRatio() / std::sqrt(static_cast<double>(std::max(1u, newTris)));
            candidates.push_back(Candidate{ &mesh, transform, newTris, score });
        };

        // Pass 1: Large objects — test against terrain depth, optionally rasterize as occluders
        for (unsigned int i = 0; i < numChildren; ++i)
        {
            osg::Node* child = node->getChild(i);
            if (!cv->validNodeMask(*child))
                continue;
            const osg::BoundingSphere& bs = child->getBound();

            if (!bs.valid() || bs.radius() < mOccluderMinRadius)
                continue;
            if (!SceneUtil::mayTraverseForOcclusion(*child, *cv, cv->getCurrentCullingSet()))
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
                if (allowStaticOccluders && mCuller->staticCandidateBudgetAvailable())
                {
                    if (auto* udc = child->getUserDataContainer())
                    {
                        for (unsigned int j = 0; j < udc->getNumUserObjects(); ++j)
                        {
                            if (auto* pod = dynamic_cast<PagedOccluderData*>(udc->getUserObject(j)))
                            {
                                std::vector<const OccluderMesh*> nearbyOccluders;
                                nearbyOccluders.reserve(64);
                                {
                                    StaticCandidateTimer timer(*mCuller);
                                    pod->collectNearbyOccluders(
                                        cv->getEyePoint(), mOccluderMaxDistanceSq, nearbyOccluders);
                                }
                                for (const OccluderMesh* occMesh : nearbyOccluders)
                                {
                                    if (!mCuller->staticCandidateBudgetAvailable())
                                    {
                                        mCuller->incrementStaticOccludersSkippedBudget();
                                        break;
                                    }
                                    StaticCandidateTimer timer(*mCuller);
                                    addCandidate(*occMesh, occMesh->aabb, occMesh->aabb.center());
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

            // Rasterize as occluder if in range and camera is not inside the building.
            // Test against terrain-only buffer so other buildings don't prevent rasterization
            // of adjacent buildings (which would reduce culling coverage for Pass 2).
            if (allowStaticOccluders && mCuller->staticCandidateBudgetAvailable())
            {
                StaticCandidateTimer timer(*mCuller);
                if (const auto* mesh = getOccluderMesh(child, parentToWorld))
                    addCandidate(*mesh->mMesh, mesh->mWorldBounds, bs.center(), &mesh->mPlacement);
            }

            // Always traverse large buildings. Do NOT gate traversal on testVisibleAABB —
            // buildings testing against a buffer that includes previously rasterized
            // buildings causes false culling (flickering) when child ordering happens to
            // place one building in front of another in the depth buffer. Large buildings
            // are correctly culled by PVS and the cell-level AABB test above; MSOC
            // is reserved for culling small objects in Pass 2.
            acceptChild(child);
        }

        {
            StaticCandidateTimer timer(*mCuller);
            std::sort(candidates.begin(), candidates.end(),
                [](const Candidate& left, const Candidate& right) { return left.mScore > right.mScore; });
        }

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

            mCuller->rasterizeOccluder(candidate.mMesh->vertices, candidate.mMesh->indices, candidate.mTransform);
            mCuller->incrementBuildingOccluders(
                candidate.mTris, static_cast<unsigned int>(candidate.mMesh->vertices.size()));
        }

        // Pass 2: Small objects — test against enriched depth buffer (terrain + buildings)
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

            if (bs.radius() >= mOccluderMinRadius)
                continue; // Already handled in pass 1
            if (!SceneUtil::mayTraverseForOcclusion(*child, *cv, cv->getCurrentCullingSet()))
                continue;

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
