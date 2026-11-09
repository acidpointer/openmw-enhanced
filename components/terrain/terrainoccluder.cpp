#include "terrainoccluder.hpp"
#include "storage.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <osg/Array>

namespace Terrain
{
    TerrainOccluder::TerrainOccluder(Storage* storage, float cellWorldSize)
        : mStorage(storage)
        , mCellWorldSize(cellWorldSize)
    {
    }

    void TerrainOccluder::setWorldspace(ESM::RefId worldspace)
    {
        if (mWorldspace == worldspace)
            return;
        mWorldspace = worldspace;
        if (!worldspace.empty())
            mCellWorldSize = mStorage->getCellWorldSize(worldspace);
        invalidate();
    }

    bool TerrainOccluder::hasTerrainData() const
    {
        // Only exterior worldspaces have terrain
        return !mWorldspace.empty() && std::isfinite(mCellWorldSize) && mCellWorldSize > 0;
    }

    void TerrainOccluder::update(const osg::Vec3f& eyePoint, int radiusCells)
    {
        if (!hasTerrainData())
        {
            invalidate();
            return;
        }
        radiusCells = std::clamp(radiusCells, 0, 32);
        auto& outPositions = mCachedPositions;
        auto& outIndices = mCachedIndices;

        // Invalid camera positions must not become integer conversion overflow.
        const double x = std::floor(static_cast<double>(eyePoint.x()) / mCellWorldSize);
        const double y = std::floor(static_cast<double>(eyePoint.y()) / mCellWorldSize);
        if (!std::isfinite(x) || !std::isfinite(y) || x < std::numeric_limits<int>::min() + radiusCells
            || x > std::numeric_limits<int>::max() - radiusCells || y < std::numeric_limits<int>::min() + radiusCells
            || y > std::numeric_limits<int>::max() - radiusCells)
        {
            invalidate();
            return;
        }
        const int cellX = static_cast<int>(x);
        const int cellY = static_cast<int>(y);
        osg::Vec2i cellPos(cellX, cellY);

        // Use cache if camera hasn't moved to a different cell
        if (mCacheValid && cellPos == mCachedCellPos && radiusCells == mCachedRadius)
            return;

        ++mRevision;
        outPositions.clear();
        outIndices.clear();

        // For each cell in range, generate a conservative coarse terrain mesh.
        // We fetch full-resolution heights (LOD 0) and min-pool them to the coarse grid,
        // ensuring the occluder is always at or below the actual terrain surface.
        // This prevents false occlusion in valleys and rapid elevation changes.
        const int step = 1 << mLodLevel;

        for (int cy = cellY - radiusCells; cy <= cellY + radiusCells; ++cy)
        {
            for (int cx = cellX - radiusCells; cx <= cellX + radiusCells; ++cx)
            {
                const auto [entry, inserted] = mCells.try_emplace(std::pair(cx, cy));
                auto& cellMesh = entry->second;
                if (!inserted)
                {
                    const auto base = static_cast<unsigned int>(outPositions.size());
                    outPositions.insert(outPositions.end(), cellMesh.positions.begin(), cellMesh.positions.end());
                    for (const auto index : cellMesh.indices)
                        outIndices.push_back(base + index);
                    continue;
                }
                const auto firstPosition = outPositions.size();
                const auto firstIndex = outIndices.size();
                osg::Vec2f center(cx + 0.5f, cy + 0.5f);

                // Get full-resolution heights
                osg::ref_ptr<osg::Vec3Array> fullRes(new osg::Vec3Array);
                osg::ref_ptr<osg::Vec3Array> normals(new osg::Vec3Array);
                osg::ref_ptr<osg::Vec4ubArray> colors(new osg::Vec4ubArray);
                colors->setNormalize(true);

                mStorage->fillVertexBuffers(0, 1.0f, center, mWorldspace, *fullRes, *normals, *colors);

                if (fullRes->empty())
                    continue;

                int fullPerSide = static_cast<int>(std::sqrt(static_cast<float>(fullRes->size())));
                if (fullPerSide < 2 || static_cast<std::size_t>(fullPerSide) * fullPerSide != fullRes->size())
                    continue;

                int coarsePerSide = (fullPerSide - 1) / step + 1;
                if (coarsePerSide < 2)
                    continue;

                // Pass 1: compute min height for each coarse quad
                int numQuads = (coarsePerSide - 1) * (coarsePerSide - 1);
                mQuadMins.resize(numQuads);
                for (int qj = 0; qj < coarsePerSide - 1; ++qj)
                {
                    for (int qi = 0; qi < coarsePerSide - 1; ++qi)
                    {
                        int startI = qi * step;
                        int startJ = qj * step;
                        int endI = std::min((qi + 1) * step, fullPerSide - 1);
                        int endJ = std::min((qj + 1) * step, fullPerSide - 1);

                        float minH = std::numeric_limits<float>::max();
                        for (int fj = startJ; fj <= endJ; ++fj)
                            for (int fi = startI; fi <= endI; ++fi)
                                minH = std::min(minH, (*fullRes)[fj * fullPerSide + fi].z());

                        mQuadMins[qj * (coarsePerSide - 1) + qi] = minH;
                    }
                }

                // Pass 2: each coarse vertex gets the min height of its surrounding quads
                osg::Vec3f worldOffset(center.x() * mCellWorldSize, center.y() * mCellWorldSize, 0.0f);
                unsigned int baseIndex = static_cast<unsigned int>(outPositions.size());

                for (int cj = 0; cj < coarsePerSide; ++cj)
                {
                    for (int ci = 0; ci < coarsePerSide; ++ci)
                    {
                        float minH = std::numeric_limits<float>::max();
                        // A vertex touches up to 4 quads
                        for (int dj = -1; dj <= 0; ++dj)
                        {
                            for (int di = -1; di <= 0; ++di)
                            {
                                int qi = ci + di;
                                int qj = cj + dj;
                                if (qi >= 0 && qi < coarsePerSide - 1 && qj >= 0 && qj < coarsePerSide - 1)
                                    minH = std::min(minH, mQuadMins[qj * (coarsePerSide - 1) + qi]);
                            }
                        }

                        osg::Vec3f pos = (*fullRes)[(cj * step) * fullPerSide + (ci * step)];
                        pos.z() = minH;
                        outPositions.push_back(pos + worldOffset);
                    }
                }

                // Generate triangle indices for the coarse grid
                for (int row = 0; row < coarsePerSide - 1; ++row)
                {
                    for (int col = 0; col < coarsePerSide - 1; ++col)
                    {
                        unsigned int tl = baseIndex + row * coarsePerSide + col;
                        unsigned int tr = tl + 1;
                        unsigned int bl = tl + coarsePerSide;
                        unsigned int br = bl + 1;

                        outIndices.push_back(tl);
                        outIndices.push_back(bl);
                        outIndices.push_back(tr);

                        outIndices.push_back(tr);
                        outIndices.push_back(bl);
                        outIndices.push_back(br);
                    }
                }
                cellMesh.positions.assign(outPositions.begin() + firstPosition, outPositions.end());
                cellMesh.indices.reserve(outIndices.size() - firstIndex);
                for (auto i = firstIndex; i < outIndices.size(); ++i)
                    cellMesh.indices.push_back(outIndices[i] - static_cast<unsigned int>(firstPosition));
            }
        }

        // Update cache
        mCachedCellPos = cellPos;
        mCachedRadius = radiusCells;
        mCacheValid = true;
        std::erase_if(mCells, [&](const auto& cell) {
            return std::abs(cell.first.first - cellX) > radiusCells
                || std::abs(cell.first.second - cellY) > radiusCells;
        });
    }
}
