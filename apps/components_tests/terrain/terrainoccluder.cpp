#include <components/terrain/storage.hpp>
#include <components/terrain/terrainoccluder.hpp>

#include <gtest/gtest.h>

namespace
{
    struct FlatStorage : Terrain::Storage
    {
        int reads = 0;
        bool empty = false;
        float height = 0;
        void getBounds(float&, float&, float&, float&, ESM::RefId) override {}
        bool getMinMaxHeights(float, const osg::Vec2f&, ESM::RefId, float&, float&) override { return false; }
        void fillVertexBuffers(int, float, const osg::Vec2f&, ESM::RefId, osg::Vec3Array& positions, osg::Vec3Array&,
            osg::Vec4ubArray&) override
        {
            ++reads;
            if (!empty)
                for (int x = 0; x < 9; ++x)
                    for (int y = 0; y < 9; ++y)
                        positions.push_back({ static_cast<float>(x - 4), static_cast<float>(y - 4), height });
        }
        void getBlendmaps(float, const osg::Vec2f&, ImageVector&, std::vector<Terrain::LayerInfo>&, ESM::RefId) override
        {
        }
        float getHeightAt(const osg::Vec3f&, ESM::RefId) override { return height; }
        float getCellWorldSize(ESM::RefId) override { return 8; }
        int getCellVertices(ESM::RefId) override { return 9; }
        int getTextureTileCount(float, ESM::RefId) override { return 1; }
    };

    TEST(TerrainOccluderTest, reusesMeshInCellAndOnlyLoadsNewBorderCells)
    {
        FlatStorage storage;
        Terrain::TerrainOccluder occluder(&storage, 8);
        occluder.setWorldspace(ESM::RefId::stringRefId("world"));
        occluder.setLodLevel(1);
        occluder.update({ 1, 1, 1 }, 1);
        EXPECT_EQ(storage.reads, 9);
        const auto* positions = occluder.getPositions().data();
        const auto revision = occluder.getRevision();
        occluder.update({ 2, 2, 1 }, 1);
        EXPECT_EQ(storage.reads, 9);
        EXPECT_EQ(positions, occluder.getPositions().data());
        EXPECT_EQ(occluder.getRevision(), revision);
        occluder.update({ 9, 1, 1 }, 1);
        EXPECT_GT(occluder.getRevision(), revision);
        EXPECT_EQ(storage.reads, 12);
        for (const auto index : occluder.getIndices())
            EXPECT_LT(index, occluder.getPositions().size());
        const auto& vertices = occluder.getPositions();
        const auto& indices = occluder.getIndices();
        for (std::size_t i = 0; i < indices.size(); i += 3)
            EXPECT_GT(
                ((vertices[indices[i + 1]] - vertices[indices[i]]) ^ (vertices[indices[i + 2]] - vertices[indices[i]]))
                    .z(),
                0);
        occluder.setLodLevel(0);
        occluder.update({ 9, 1, 1 }, 1);
        EXPECT_EQ(storage.reads, 21);
        storage.height = -5;
        occluder.setWorldspace(ESM::RefId::stringRefId("other"));
        occluder.update({ 9, 1, 1 }, 1);
        EXPECT_EQ(storage.reads, 30);
        for (const auto& vertex : occluder.getPositions())
            EXPECT_EQ(vertex.z(), -5);
    }

    TEST(TerrainOccluderTest, emptyTerrainIsCachedAndDisablingWorldspaceClearsGeometry)
    {
        FlatStorage storage;
        storage.empty = true;
        Terrain::TerrainOccluder occluder(&storage, 8);
        occluder.setWorldspace(ESM::RefId::stringRefId("empty"));
        occluder.update({ 1, 1, 1 }, 1);
        occluder.update({ 2, 2, 1 }, 1);
        EXPECT_EQ(storage.reads, 9);
        EXPECT_TRUE(occluder.getPositions().empty());
        storage.empty = false;
        occluder.setWorldspace(ESM::RefId::stringRefId("land"));
        occluder.update({ 1, 1, 1 }, 1);
        ASSERT_FALSE(occluder.getPositions().empty());
        occluder.setWorldspace({});
        EXPECT_TRUE(occluder.getPositions().empty());
        EXPECT_TRUE(occluder.getIndices().empty());
    }
}
