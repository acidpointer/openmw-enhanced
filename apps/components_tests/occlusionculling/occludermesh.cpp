#include <components/occlusionculling/occludermesh.hpp>

#include <algorithm>
#include <components/sceneutil/occlusionculling.hpp>
#include <osg/AlphaFunc>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/LOD>
#include <osg/Switch>
#include <osg/UserDataContainer>

#include <gtest/gtest.h>

namespace
{
    osg::ref_ptr<osg::Geometry> geometry(const std::vector<osg::Vec3f>& positions)
    {
        osg::ref_ptr<osg::Geometry> result = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->assign(positions.begin(), positions.end());
        result->setVertexArray(vertices);
        result->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, positions.size()));
        return result;
    }

    TEST(OccluderMeshTest, boundedProxyKeepsOriginalTrianglesInsteadOfBridgingGaps)
    {
        std::vector<osg::Vec3f> vertices;
        for (int i = 0; i < 100; ++i)
        {
            const float x = static_cast<float>(i * 10);
            vertices.insert(vertices.end(), { { x, 0, 0 }, { x + 2, 0, 0 }, { x, 2, 0 } });
        }
        const auto source = geometry(vertices);
        const auto mesh = OcclusionCulling::buildSimplifiedMesh(source, 4, 1);
        ASSERT_EQ(mesh.indices.size(), 32 * 3);
        for (std::size_t i = 0; i < mesh.indices.size(); i += 3)
        {
            bool original = false;
            for (std::size_t j = 0; j < vertices.size(); j += 3)
                if (mesh.vertices[mesh.indices[i]] == vertices[j]
                    && mesh.vertices[mesh.indices[i + 1]] == vertices[j + 1]
                    && mesh.vertices[mesh.indices[i + 2]] == vertices[j + 2])
                    original = true;
            EXPECT_TRUE(original);
        }
    }

    TEST(OccluderMeshTest, insetStaysOnEachDisconnectedTriangle)
    {
        const auto source
            = geometry({ { -12, 0, 0 }, { -10, 0, 0 }, { -12, 2, 0 }, { 10, 0, 0 }, { 12, 0, 0 }, { 10, 2, 0 } });
        const auto mesh = OcclusionCulling::buildSimplifiedMesh(source, 4, 0.5f);
        ASSERT_EQ(mesh.vertices.size(), 6);
        for (std::size_t i = 0; i < 3; ++i)
        {
            EXPECT_LE(mesh.vertices[i].x(), -10);
            EXPECT_GE(mesh.vertices[i + 3].x(), 10);
            EXPECT_EQ(mesh.vertices[i].z(), 0);
        }
    }

    TEST(OccluderMeshTest, realOpeningRemainsVisibleWhileSolidWallOccludes)
    {
        std::vector<osg::Vec3f> vertices;
        auto rectangle = [&](float x0, float y0, float x1, float y1) {
            vertices.insert(vertices.end(),
                { { x0, y0, -10 }, { x1, y0, -10 }, { x1, y1, -10 }, { x0, y0, -10 }, { x1, y1, -10 },
                    { x0, y1, -10 } });
        };
        rectangle(-8, -6, -2, 6);
        rectangle(2, -6, 8, 6);
        rectangle(-2, -6, 2, -2);
        rectangle(-2, 2, 2, 6);
        const auto source = geometry(vertices);
        const auto mesh = OcclusionCulling::buildSimplifiedMesh(source, 4, 1);
        SceneUtil::OcclusionCuller culler(256, 128);
        culler.beginFrame(osg::Matrixd::identity(), osg::Matrixd::perspective(90, 1, 1, 100), 1);
        culler.rasterizeOccluder(mesh.vertices, mesh.indices);
        EXPECT_TRUE(culler.testVisibleAABB(osg::BoundingBox(-0.2, -0.2, -21, 0.2, 0.2, -20)));
        EXPECT_FALSE(culler.testVisibleAABB(osg::BoundingBox(-10.2, -0.2, -21, -9.8, 0.2, -20)));
    }

    TEST(OccluderMeshTest, shaderConvertedAlphaAndDynamicSwitchAreExcluded)
    {
        const auto source = geometry({ { 0, 0, 0 }, { 10, 0, 0 }, { 0, 10, 0 } });
        osg::ref_ptr<osg::StateSet> removed = new osg::StateSet;
        removed->setName("removedState");
        removed->setAttribute(new osg::AlphaFunc(osg::AlphaFunc::GREATER, 0.5f));
        source->getOrCreateStateSet()->getOrCreateUserDataContainer()->addUserObject(removed);
        EXPECT_TRUE(OcclusionCulling::buildSimplifiedMesh(source, 4, 1).indices.empty());
        source->setStateSet(nullptr);
        osg::ref_ptr<osg::Switch> dynamic = new osg::Switch;
        dynamic->addChild(source, true);
        EXPECT_TRUE(OcclusionCulling::buildSimplifiedMesh(dynamic, 4, 1).indices.empty());
        osg::ref_ptr<osg::LOD> lod = new osg::LOD;
        lod->addChild(source, 0, 100);
        EXPECT_TRUE(OcclusionCulling::buildSimplifiedMesh(lod, 4, 1).indices.empty());
    }

    TEST(OccluderMeshTest, inheritedAlphaTestCannotBecomeSolidOccluder)
    {
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back({ 0, 0, 0 });
        vertices->push_back({ 100, 0, 0 });
        vertices->push_back({ 0, 100, 0 });
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        geometry->setVertexArray(vertices);
        geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(geometry);
        osg::ref_ptr<osg::Group> root = new osg::Group;
        root->addChild(geode);
        ASSERT_FALSE(OcclusionCulling::buildSimplifiedMesh(root, 12, 0.9f).indices.empty());
        root->getOrCreateStateSet()->setAttributeAndModes(new osg::AlphaFunc(osg::AlphaFunc::GREATER, 0.5f));
        EXPECT_TRUE(OcclusionCulling::buildSimplifiedMesh(root, 12, 0.9f).indices.empty());
    }
}
