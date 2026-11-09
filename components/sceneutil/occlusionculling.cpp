#include "occlusionculling.hpp"

#include <MaskedOcclusionCulling.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

#include <osg/FrameStamp>
#include <osg/Timer>
#include <osgUtil/CullVisitor>

namespace SceneUtil
{
    OcclusionCuller::OcclusionCuller(unsigned int bufferWidth, unsigned int bufferHeight)
        : mMOC(nullptr)
        , mMOCTerrainOnly(nullptr)
    {
        // Width must be multiple of 8, height multiple of 4
        bufferWidth = (bufferWidth + 7) & ~7u;
        bufferHeight = (bufferHeight + 3) & ~3u;

        mMOC = MaskedOcclusionCulling::Create();
        if (mMOC)
        {
            mMOC->SetResolution(bufferWidth, bufferHeight);
            mMOC->SetNearClipPlane(0.1f);
        }
        mMOCTerrainOnly = MaskedOcclusionCulling::Create();
        if (mMOCTerrainOnly)
        {
            mMOCTerrainOnly->SetResolution(bufferWidth, bufferHeight);
            mMOCTerrainOnly->SetNearClipPlane(0.1f);
        }
    }

    OcclusionCuller::~OcclusionCuller()
    {
        if (mMOC)
            MaskedOcclusionCulling::Destroy(mMOC);
        if (mMOCTerrainOnly)
            MaskedOcclusionCulling::Destroy(mMOCTerrainOnly);
    }

    void OcclusionCuller::beginFrame(
        const osg::Matrixd& viewMatrix, const osg::Matrixd& projectionMatrix, float nearClip, bool retainDepth)
    {
        mVisitor.store(nullptr, std::memory_order_release);
        mFrameActive = false;
        if (!mMOC || !std::isfinite(nearClip))
            return;

        mNearClip = std::max(nearClip, 0.001f);
        mMOC->SetNearClipPlane(mNearClip);
        if (mMOCTerrainOnly)
            mMOCTerrainOnly->SetNearClipPlane(mNearClip);
        if (!mViewInverse.invert(viewMatrix))
            return;
        if (!retainDepth)
            clearDepth();
        mViewProjection = viewMatrix * projectionMatrix;
        mProjection = projectionMatrix;

        const double* vpDouble = mViewProjection.ptr();
        for (int i = 0; i < 16; ++i)
        {
            mVPFloat[i] = static_cast<float>(vpDouble[i]);
            if (!std::isfinite(mVPFloat[i]))
                return;
        }

        mNumOccluded = 0;
        mNumTested = 0;
        mNumBuildingOccluders = 0;
        mNumBuildingTris = 0;
        mNumBuildingVerts = 0;
        mRasterizeMs = 0.0;
        mTestMs = 0.0;
        mMeshBuildMs = 0.0;
        mPagedCallbackMs = 0.0;
        mCellCallbackMs = 0.0;
        mSceneCallbackMs = 0.0;
        mChildTraverseMs = 0.0;
        mOcclusionOverheadMs = 0.0;
        mStaticCandidateMs = 0.0;
        mSmallTestMs = 0.0;
        mRasterizeCalls = 0;
        mTestCalls = 0;
        mMeshBuilds = 0;
        mStaticOccluderCandidates = 0;
        mStaticOccludersSkippedBudget = 0;
        mStaticOccludersSkippedDistance = 0;
        mStaticOccludersSkippedScreen = 0;
        mSmallOccludeesSkippedScreen = 0;
        mTerrainCellTests = 0;
        mTerrainCellOccluded = 0;
        mTerrainPagedTests = 0;
        mTerrainPagedOccluded = 0;
        mStaticRasterBudgetBaselineMs = 0.0;
        mFrameActive = true;
    }

    void OcclusionCuller::clearDepth()
    {
        if (mMOC)
            mMOC->ClearBuffer();
        if (mMOCTerrainOnly)
            mMOCTerrainOnly->ClearBuffer();
    }

    void OcclusionCuller::bindVisitor(osgUtil::CullVisitor& visitor)
    {
        mCamera = visitor.getCurrentCamera();
        mFrameNumber = visitor.getFrameStamp()->getFrameNumber();
        mRootDepth = visitor.getNodePath().size();
        mWorldRoot = mRootDepth ? visitor.getNodePath().back() : nullptr;
        mVisitor.store(&visitor, std::memory_order_release);
    }

    bool OcclusionCuller::isFrameActiveFor(osgUtil::CullVisitor& visitor) const
    {
        // Other cull threads must return before reading mutable frame data.
        return mVisitor.load(std::memory_order_acquire) == &visitor && mFrameActive
            && mCamera == visitor.getCurrentCamera() && visitor.getFrameStamp()
            && mFrameNumber == visitor.getFrameStamp()->getFrameNumber()
            && mProjection == *visitor.getProjectionMatrix()
            && (!mRootDepth
                || (visitor.getNodePath().size() >= mRootDepth && visitor.getNodePath()[mRootDepth - 1] == mWorldRoot
                    && std::find(visitor.getNodePath().begin() + mRootDepth, visitor.getNodePath().end(), mWorldRoot)
                        == visitor.getNodePath().end()));
    }

    void OcclusionCuller::rasterizeTerrainOccluder(
        const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices, bool fullBuffer)
    {
        if (!mFrameActive || worldPositions.empty() || indices.empty())
            return;
        if (!mMOCTerrainOnly)
        {
            if (fullBuffer)
                rasterizeOccluder(worldPositions, indices);
            return;
        }

        const int numTris = static_cast<int>(indices.size()) / 3;
        if (numTris <= 0)
            return;

#ifndef NDEBUG
        const unsigned int vertexCount = static_cast<unsigned int>(worldPositions.size());
        for (const auto idx : indices)
            if (idx >= vertexCount)
                return;

        for (const auto& v : worldPositions)
        {
            float w = mVPFloat[3] * v.x() + mVPFloat[7] * v.y() + mVPFloat[11] * v.z() + mVPFloat[15];
            if (!std::isfinite(w))
                return;
        }
#endif

        MaskedOcclusionCulling::VertexLayout vtxLayout(12, 4, 8);
        const osg::Timer_t start = osg::Timer::instance()->tick();
        mMOCTerrainOnly->RenderTriangles(reinterpret_cast<const float*>(worldPositions.data()), indices.data(), numTris,
            mVPFloat, MaskedOcclusionCulling::BACKFACE_CW, MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);
        mRasterizeMs += osg::Timer::instance()->delta_m(start, osg::Timer::instance()->tick());
        ++mRasterizeCalls;
        if (fullBuffer)
        {
            if (mMOC->GetImplementation() == mMOCTerrainOnly->GetImplementation())
            {
                // Merge the SIMD depth tiles instead of transforming, clipping
                // and rasterizing the same terrain triangles a second time.
                // MOC's private tile layouts require matching implementations.
                const auto mergeStart = osg::Timer::instance()->tick();
                mMOC->MergeBuffer(mMOCTerrainOnly);
                mRasterizeMs += osg::Timer::instance()->delta_m(mergeStart, osg::Timer::instance()->tick());
            }
            else
                rasterizeOccluder(worldPositions, indices);
        }
    }

    void OcclusionCuller::rasterizeOccluder(const std::vector<osg::Vec3f>& worldPositions,
        const std::vector<unsigned int>& indices, const osg::Matrixf* modelToWorld, bool backfaceCull)
    {
        if (!mFrameActive || worldPositions.empty() || indices.empty())
            return;

        const int numTris = static_cast<int>(indices.size()) / 3;
        if (numTris <= 0)
            return;

        const osg::Matrixf modelToClip(modelToWorld ? osg::Matrixd(*modelToWorld) * mViewProjection : mViewProjection);
        const float* transform = modelToWorld ? modelToClip.ptr() : mVPFloat;

        // Validate all vertex indices are in range to prevent out-of-bounds reads.
#ifndef NDEBUG
        const unsigned int vertexCount = static_cast<unsigned int>(worldPositions.size());
        for (const auto idx : indices)
        {
            if (idx >= vertexCount)
                return;
        }

        // Non-finite coordinates are invalid. Finite near-plane crossings,
        // including w == 0, are handled by MOC's homogeneous triangle clipper.
        for (const auto& v : worldPositions)
        {
            float w = transform[3] * v.x() + transform[7] * v.y() + transform[11] * v.z() + transform[15];
            if (!std::isfinite(w))
                return;
        }
#endif

        // Vec3f layout: stride=12 bytes, yOffset=4, zOffset=8
        // MOC treats (x,y,z) as (x,y,w_component) and transforms via the VP matrix
        MaskedOcclusionCulling::VertexLayout vtxLayout(12, 4, 8);

        const osg::Timer_t start = osg::Timer::instance()->tick();
        mMOC->RenderTriangles(reinterpret_cast<const float*>(worldPositions.data()), indices.data(), numTris, transform,
            backfaceCull ? MaskedOcclusionCulling::BACKFACE_CW : MaskedOcclusionCulling::BACKFACE_NONE,
            MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);
        mRasterizeMs += osg::Timer::instance()->delta_m(start, osg::Timer::instance()->tick());
        ++mRasterizeCalls;
    }

    bool OcclusionCuller::projectBounds(const osg::BoundingBox& worldBB, ProjectedBounds& projected) const
    {
        if (!mFrameActive || !worldBB.valid())
            return false;
        osg::Vec3f corners[8];
        for (unsigned int i = 0; i < 8; ++i)
            corners[i] = worldBB.corner(i);
        float clip[8 * 4];
        // Reuse MOC's SSE/NEON transform instead of scalar matrix products.
        // Use the same float VP matrix as occluder rasterization.
        MaskedOcclusionCulling::TransformVertices(mVPFloat, reinterpret_cast<const float*>(corners), clip, 8);
        projected = { 1, 1, -1, -1, std::numeric_limits<float>::max() };
        for (unsigned int i = 0; i < 8; ++i)
        {
            const float w = clip[i * 4 + 3];
            if (!std::isfinite(w) || w <= mNearClip)
                return false; // Near-plane crossings always remain visible.
            const float inverseW = 1.f / w;
            const float x = clip[i * 4] * inverseW;
            const float y = clip[i * 4 + 1] * inverseW;
            if (!std::isfinite(x) || !std::isfinite(y))
                return false;
            projected.minX = std::min(projected.minX, x);
            projected.minY = std::min(projected.minY, y);
            projected.maxX = std::max(projected.maxX, x);
            projected.maxY = std::max(projected.maxY, y);
            projected.minW = std::min(projected.minW, w);
        }
        projected.minX = std::max(projected.minX, -1.f);
        projected.minY = std::max(projected.minY, -1.f);
        projected.maxX = std::min(projected.maxX, 1.f);
        projected.maxY = std::min(projected.maxY, 1.f);
        return projected.minX < projected.maxX && projected.minY < projected.maxY;
    }

    bool OcclusionCuller::projectOccluderBounds(const osg::BoundingBox& worldBB, ProjectedBounds& projected) const
    {
        if (projectBounds(worldBB, projected))
            return true;
        if (!mFrameActive || !worldBB.valid())
            return false;
        osg::Vec3f corners[8];
        for (unsigned int i = 0; i < 8; ++i)
            corners[i] = worldBB.corner(i);
        float clip[8 * 4];
        MaskedOcclusionCulling::TransformVertices(mVPFloat, reinterpret_cast<const float*>(corners), clip, 8);
        bool outside[5] = { true, true, true, true, true };
        bool crossesNear = false;
        for (unsigned int i = 0; i < 8; ++i)
        {
            const float x = clip[i * 4], y = clip[i * 4 + 1], w = clip[i * 4 + 3];
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w))
                return false;
            crossesNear |= w <= mNearClip;
            outside[0] &= w <= mNearClip;
            outside[1] &= x < -w;
            outside[2] &= x > w;
            outside[3] &= y < -w;
            outside[4] &= y > w;
        }
        if (!crossesNear || std::any_of(std::begin(outside), std::end(outside), [](bool value) { return value; }))
            return false;
        // Conservative importance only: do not divide through a near-plane
        // crossing. Rasterization still uses the original opaque triangles,
        // with all clipping planes enabled, never this screen rectangle.
        projected = { -1, -1, 1, 1, mNearClip };
        return true;
    }

    bool OcclusionCuller::testVisibleAABBImpl(MaskedOcclusionCulling* moc, const ProjectedBounds& projected) const
    {
        if (!moc)
            return true;
        unsigned int width, height;
        moc->GetResolution(width, height);
        // Expand queries one software pixel and round depth towards the camera.
        // Rounding can reduce culling, but must never hide a visible edge.
        const float padX = 2.f / width;
        const float padY = 2.f / height;
        const osg::Timer_t start = osg::Timer::instance()->tick();
        const auto result = moc->TestRect(std::max(-1.f, projected.minX - padX), std::max(-1.f, projected.minY - padY),
            std::min(1.f, projected.maxX + padX), std::min(1.f, projected.maxY + padY),
            std::nextafter(projected.minW, 0.f));
        mTestMs += osg::Timer::instance()->delta_m(start, osg::Timer::instance()->tick());
        ++mTestCalls;
        return result != MaskedOcclusionCulling::OCCLUDED;
    }

    bool OcclusionCuller::testVisibleAABBTerrainOnly(const osg::BoundingBox& worldBB) const
    {
        ProjectedBounds projected;
        return !projectBounds(worldBB, projected) || testVisibleAABBTerrainOnly(projected);
    }

    bool OcclusionCuller::testVisibleAABBTerrainOnly(const ProjectedBounds& projected) const
    {
        return !mFrameActive || testVisibleAABBImpl(mMOCTerrainOnly, projected);
    }

    bool OcclusionCuller::testVisibleAABB(const osg::BoundingBox& worldBB) const
    {
        ProjectedBounds projected;
        if (!projectBounds(worldBB, projected))
            return true;
        if (projected.screenRatio() < mMinOccludeeScreenRatio)
        {
            ++mSmallOccludeesSkippedScreen;
            return true;
        }
        ++mNumTested;
        const bool visible = testVisibleAABBImpl(mMOC, projected);
        if (!visible)
            ++mNumOccluded;
        return visible;
    }

    const char* OcclusionCuller::getImplementationName() const
    {
        if (!mMOC)
            return "none";

        switch (mMOC->GetImplementation())
        {
            case MaskedOcclusionCulling::SSE2:
                return "SSE2";
            case MaskedOcclusionCulling::SSE41:
                return "SSE4.1";
            case MaskedOcclusionCulling::AVX2:
                return "AVX2";
            case MaskedOcclusionCulling::AVX512:
                return "AVX512";
        }

        return "unknown";
    }

    double OcclusionCuller::getStaticRasterMs() const
    {
        return std::max(0.0, mRasterizeMs - mStaticRasterBudgetBaselineMs);
    }

    bool OcclusionCuller::staticRasterBudgetAvailable() const
    {
        return mStaticRasterBudgetMs <= 0.0 || getStaticRasterMs() + mStaticCandidateMs < mStaticRasterBudgetMs;
    }

    bool OcclusionCuller::estimateScreenRatio(const osg::BoundingBox& worldBB, double& ratio) const
    {
        ProjectedBounds projected;
        if (!projectBounds(worldBB, projected))
        {
            ratio = 0;
            return false;
        }
        ratio = projected.screenRatio();
        return true;
    }

    void OcclusionCuller::computePixelDepthBuffer(float* depthData) const
    {
        if (mMOC)
            mMOC->ComputePixelDepthBuffer(depthData, true); // flipY for OpenGL bottom-to-top
    }

    void OcclusionCuller::getResolution(unsigned int& width, unsigned int& height) const
    {
        if (mMOC)
            mMOC->GetResolution(width, height);
        else
        {
            width = 0;
            height = 0;
        }
    }
}
