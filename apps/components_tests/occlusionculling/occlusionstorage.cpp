#include <components/occlusionculling/occlusionstorage.hpp>
#include <components/testing/util.hpp>

#include <chrono>
#include <gtest/gtest.h>
#include <osg/Geometry>
#include <sqlite3.h>
#include <thread>

namespace
{
    constexpr VFS::Path::NormalizedView Model("meshes/test.nif");

    OcclusionCulling::OccluderMesh triangle()
    {
        OcclusionCulling::OccluderMesh mesh;
        mesh.vertices = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
        mesh.indices = { 0, 1, 2 };
        for (const auto& vertex : mesh.vertices)
            mesh.aabb.expandBy(vertex);
        return mesh;
    }

    TEST(OcclusionStorageTest, persistedMeshDependsOnWinningAssetContents)
    {
        const auto path = (TestingOpenMW::currentTestDirPath() / "cache.sqlite").string();
        TestingOpenMW::VFSTestFile originalFile("original mesh bytes");
        TestingOpenMW::VFSTestFile replacedFile("replacement mesh bytes");
        const auto originalVfs = TestingOpenMW::createTestVFS({ { Model, &originalFile } });
        const auto replacedVfs = TestingOpenMW::createTestVFS({ { Model, &replacedFile } });
        const auto mesh = triangle();
        {
            OcclusionCulling::OcclusionStorage storage(path, originalVfs.get());
            ASSERT_TRUE(storage.isOpen());
            storage.put(Model.value(), 12, 900000, mesh);
        }
        {
            OcclusionCulling::OcclusionStorage storage(path, originalVfs.get());
            OcclusionCulling::OccluderMesh restored;
            ASSERT_TRUE(storage.get(Model.value(), 12, 900000, restored));
            EXPECT_EQ(restored.vertices, mesh.vertices);
            EXPECT_EQ(restored.indices, mesh.indices);
        }
        {
            OcclusionCulling::OcclusionStorage storage(path, replacedVfs.get());
            OcclusionCulling::OccluderMesh restored;
            EXPECT_FALSE(storage.get(Model.value(), 12, 900000, restored));
        }
    }

    TEST(OcclusionStorageTest, rejectsPersistedOutOfRangeTriangleIndex)
    {
        const auto path = (TestingOpenMW::currentTestDirPath() / "cache.sqlite").string();
        auto mesh = triangle();
        mesh.indices.back() = 3;
        {
            OcclusionCulling::OcclusionStorage storage(path);
            ASSERT_TRUE(storage.isOpen());
            storage.put(Model.value(), 12, 900000, mesh);
        }
        OcclusionCulling::OcclusionStorage storage(path);
        OcclusionCulling::OccluderMesh restored;
        EXPECT_FALSE(storage.get(Model.value(), 12, 900000, restored));
    }

    TEST(OcclusionStorageTest, proxiesFromBeforeHiddenGeometryFilteringAreNotReused)
    {
        const auto path = (TestingOpenMW::currentTestDirPath() / "cache.sqlite").string();
        {
            OcclusionCulling::OcclusionStorage storage(path);
            storage.put(Model.value(), 12, 900000, triangle());
        }
        {
            sqlite3* database = nullptr;
            const auto result = sqlite3_open(path.c_str(), &database);
            const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> owner(database, &sqlite3_close);
            ASSERT_EQ(result, SQLITE_OK);
            ASSERT_EQ(sqlite3_exec(database, "UPDATE occlusion_meshes SET model_path = 'mesh-v3:meshes/test.nif'",
                          nullptr, nullptr, nullptr),
                SQLITE_OK);
        }
        OcclusionCulling::OcclusionStorage storage(path);
        OcclusionCulling::OccluderMesh restored;
        EXPECT_FALSE(storage.get(Model.value(), 12, 900000, restored));
        storage.put(Model.value(), 12, 900000, triangle());
        EXPECT_TRUE(storage.get(Model.value(), 12, 900000, restored));
    }
    TEST(OcclusionStorageTest, denseLoadEventuallyPreparesOverflowRequests)
    {
        OcclusionCulling::OcclusionStorage storage(":memory:");
        osg::ref_ptr<osg::Geometry> source = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back({ 0, 0, 0 });
        vertices->push_back({ 10, 0, 0 });
        vertices->push_back({ 0, 10, 0 });
        source->setVertexArray(vertices);
        source->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
        std::vector<std::string> names;
        for (int i = 0; i < 512; ++i)
        {
            names.push_back("meshes/queued-" + std::to_string(i) + ".nif");
            storage.prepare(names.back(), source.get(), 4, 1.f);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool allReady = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            allReady = true;
            for (const auto& name : names)
                if (!storage.getReady(name, 4, OcclusionCulling::OcclusionStorage::makeShrinkKey(1)))
                {
                    allReady = false;
                    break;
                }
            if (allReady)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ASSERT_TRUE(allReady);
        const auto stats = storage.getAndResetStats();
        EXPECT_EQ(stats.queued, 512);
        EXPECT_EQ(stats.rejected, 0);
    }

}
