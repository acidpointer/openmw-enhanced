#include <apps/openmw/mwrender/occlusionculling.hpp>
#include <gtest/gtest.h>

#include <components/sceneutil/occlusioncallback.hpp>
#include <components/sceneutil/occlusionorder.hpp>
#include <components/sceneutil/occlusionplan.hpp>
#include <components/sceneutil/occlusionpreparation.hpp>
#include <components/sceneutil/renderstageinventory.hpp>
#include <components/sceneutil/viewpreparation.hpp>

#include <osg/Camera>
#include <osg/FrameBufferObject>
#include <osg/FrameStamp>
#include <osg/MatrixTransform>
#include <osg/Texture2D>
#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>

namespace
{
    struct TestCullVisitor : osgUtil::CullVisitor
    {
        ~TestCullVisitor() override = default;
    };

    class CountCallback : public SceneUtil::NodeCallback<CountCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        explicit CountCallback(int& calls)
            : mCalls(calls)
        {
        }
        void operator()(osg::Node* node, osgUtil::CullVisitor* visitor)
        {
            ++mCalls;
            traverse(node, visitor);
        }
        int& mCalls;
    };

    osg::ref_ptr<osg::Geometry> triangle(float z)
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back({ -0.5f, -0.5f, z });
        vertices->push_back({ 0.5f, -0.5f, z });
        vertices->push_back({ 0, 0.5f, z });
        geometry->setVertexArray(vertices);
        geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
        return geometry;
    }

    struct OcclusionDispatch : testing::Test
    {
        osg::ref_ptr<SceneUtil::OcclusionCuller> culler = new SceneUtil::OcclusionCuller(256, 128);
        osg::ref_ptr<osgUtil::RenderStage> stage = new osgUtil::RenderStage;
        osg::ref_ptr<osg::Camera> camera = new osg::Camera;
        osg::ref_ptr<osg::Group> world = new osg::Group;
        TestCullVisitor visitor;

        OcclusionDispatch()
        {
            const auto projection = osg::Matrixd::perspective(90, 1, 1, 100);
            stage->setCamera(camera);
            visitor.setRenderStage(stage);
            visitor.setStateGraph(new osgUtil::StateGraph);
            visitor.setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
            visitor.setCullingMode(osg::CullingSet::VIEW_FRUSTUM_CULLING);
            osg::ref_ptr<osg::FrameStamp> stamp = new osg::FrameStamp;
            stamp->setFrameNumber(1);
            visitor.setFrameStamp(stamp);
            visitor.setTraversalNumber(1);
            visitor.pushViewport(new osg::Viewport(0, 0, 256, 128));
            visitor.pushProjectionMatrix(new osg::RefMatrix(projection));
            visitor.pushModelViewMatrix(new osg::RefMatrix(osg::Matrix::identity()), osg::Transform::ABSOLUTE_RF);
            visitor.pushOntoNodePath(world);
            culler->beginFrame(osg::Matrixd::identity(), projection, 1);
            culler->rasterizeTerrainOccluder(
                { { -8, -6, -10 }, { 8, -6, -10 }, { 8, 6, -10 }, { -8, 6, -10 } }, { 0, 1, 2, 0, 2, 3 });
            culler->bindVisitor(visitor);
        }
        ~OcclusionDispatch() override
        {
            culler->endFrame();
            visitor.popFromNodePath();
            visitor.popModelViewMatrix();
            visitor.popProjectionMatrix();
            visitor.popViewport();
        }
        std::size_t leaves() const
        {
            std::size_t count = 0;
            for (const auto* graph : stage->getStateGraphList())
                count += graph->_leaves.size();
            return count;
        }
    };

    TEST_F(OcclusionDispatch, HiddenSubtreeStopsNestedCallbacksAndRenderLeafSubmission)
    {
        int calls = 0;
        osg::ref_ptr<osg::Group> actor = new osg::Group;
        actor->addChild(triangle(-20));
        actor->setCullCallback(new SceneUtil::OcclusionNodeCallback(culler));
        actor->addCullCallback(new CountCallback(calls));
        actor->accept(visitor);
        EXPECT_EQ(calls, 0);
        EXPECT_EQ(leaves(), 0);
        EXPECT_GT(culler->getNumOccluded(), 0);
    }

    TEST_F(OcclusionDispatch, MetadataPreparationDoesNotRunCullCallbacksOrSubmitRenderLeaves)
    {
        int calls = 0;
        world->addChild(triangle(-5));
        world->setCullCallback(new CountCallback(calls));
        SceneUtil::ViewPreparationVisitor preparation(visitor);
        EXPECT_NE(preparation.getVisitorType(), osg::NodeVisitor::CULL_VISITOR);
        world->accept(preparation);
        EXPECT_EQ(calls, 0);
        EXPECT_EQ(SceneUtil::countRenderSubmissions(*stage).mLeaves, 0);
    }

    struct WallModel
    {
        osg::ref_ptr<osg::MatrixTransform> placement = new osg::MatrixTransform;
        osg::ref_ptr<osg::Group> visual = new osg::Group;
        osg::ref_ptr<MWRender::StaticOccluderData> data = new MWRender::StaticOccluderData;
        WallModel()
        {
            auto mesh = std::make_shared<OcclusionCulling::OccluderMesh>();
            mesh->vertices = { { -8, -6, -10 }, { 8, -6, -10 }, { 8, 6, -10 }, { -8, 6, -10 } };
            mesh->indices = { 0, 1, 2, 0, 2, 3 };
            for (const auto& vertex : mesh->vertices)
                mesh->aabb.expandBy(vertex);
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(new osg::Vec3Array(mesh->vertices.begin(), mesh->vertices.end()));
            geometry->addPrimitiveSet(
                new osg::DrawElementsUInt(GL_TRIANGLES, mesh->indices.begin(), mesh->indices.end()));
            visual->addChild(geometry);
            placement->addChild(visual);
            data->mMesh = mesh;
            data->mVisual = visual.get();
        }
    };

    TEST_F(OcclusionDispatch, WallInTheLastCellHidesTheFirstCellBeforeNestedCullWork)
    {
        int calls = 0;
        osg::ref_ptr<osg::Group> firstCell = new osg::Group, lastCell = new osg::Group;
        osg::ref_ptr<osg::Group> actor = new osg::Group;
        actor->addChild(triangle(-20));
        actor->setCullCallback(new CountCallback(calls));
        firstCell->addChild(actor);
        firstCell->setCullCallback(new MWRender::CellOcclusionCallback(culler, .1, 100, 1000, true));
        osg::ref_ptr<MWRender::CellOcclusionCallback> callback
            = new MWRender::CellOcclusionCallback(culler, .1, 100, 1000, true);
        lastCell->setCullCallback(callback);
        WallModel wall;
        lastCell->addChild(wall.placement);
        callback->registerOccluder(wall.placement, wall.data);
        world->addChild(firstCell);
        world->addChild(lastCell);
        culler->clearDepth();
        SceneUtil::OcclusionPlan plan;
        SceneUtil::OcclusionPreparationVisitor preparation(visitor, plan);
        plan.start();
        world->accept(preparation);
        EXPECT_EQ(leaves(), 0);
        SceneUtil::OcclusionViewKey key;
        key.mCamera = camera;
        key.mProjection = *visitor.getProjectionMatrix();
        key.mNear = 1;
        EXPECT_FALSE(plan.prepare(key));
        plan.rasterize(*culler);
        world->traverse(visitor);
        EXPECT_EQ(calls, 0);
        EXPECT_EQ(leaves(), 1);
        world->removeChild(firstCell);
        world->addChild(firstCell);
        plan.start();
        world->accept(preparation);
        EXPECT_TRUE(plan.prepare(key));
        const auto before = leaves();
        world->traverse(visitor);
        EXPECT_EQ(leaves() - before, 1);
        EXPECT_EQ(calls, 0);
    }

    TEST_F(OcclusionDispatch, CellRegistryInvalidatesRemovedMovedFadedAndReplacedWalls)
    {
        osg::ref_ptr<osg::Group> cell = new osg::Group;
        osg::ref_ptr<MWRender::CellOcclusionCallback> callback
            = new MWRender::CellOcclusionCallback(culler, .1, 100, 1000, true);
        cell->setCullCallback(callback);
        cell->addChild(triangle(-20));
        WallModel wall;
        cell->addChild(wall.placement);
        callback->registerOccluder(wall.placement, wall.data);
        world->addChild(cell);
        SceneUtil::OcclusionPlan plan;
        SceneUtil::OcclusionViewKey key;
        key.mProjection = *visitor.getProjectionMatrix();
        const auto render = [&] {
            culler->endFrame();
            culler->beginFrame({}, key.mProjection, 1, true);
            plan.start();
            SceneUtil::OcclusionPreparationVisitor preparation(visitor, plan);
            world->accept(preparation);
            if (!plan.prepare(key))
                culler->clearDepth();
            plan.rasterize(*culler);
            culler->bindVisitor(visitor);
            const auto before = leaves();
            world->traverse(visitor);
            return leaves() - before;
        };
        EXPECT_EQ(render(), 1);
        cell->removeChild(wall.placement); // still alive: validate membership, not just weak expiry
        EXPECT_EQ(render(), 1);
        cell->addChild(wall.placement);
        callback->registerOccluder(wall.placement, wall.data);
        EXPECT_EQ(render(), 1);
        wall.placement->setUserValue("occlusionOpaque", false);
        EXPECT_EQ(render(), 2);
        wall.placement->setUserValue("occlusionOpaque", true);
        EXPECT_EQ(render(), 1);
        wall.visual->setNodeMask(0);
        EXPECT_EQ(render(), 1);
        EXPECT_EQ(culler->getNumBuildingOccluders(), 0);
        wall.visual->setNodeMask(~0u);
        EXPECT_EQ(render(), 1);
        EXPECT_EQ(culler->getNumBuildingOccluders(), 1);
        wall.placement->setMatrix(osg::Matrix::translate(100, 0, 0));
        EXPECT_EQ(render(), 1);
        wall.placement->setMatrix(osg::Matrix::identity());
        EXPECT_EQ(render(), 1);
        wall.placement->removeChild(wall.visual);
        wall.placement->addChild(triangle(-10));
        EXPECT_EQ(render(), 2);
    }

    TEST(PagedOccluders, SpatialIndexKeepsWallsWhoseCentresAreOutsideTheQuery)
    {
        MWRender::PagedOccluderData data;
        for (int i = 0; i < 64; ++i)
        {
            auto mesh = std::make_shared<OcclusionCulling::OccluderMesh>();
            mesh->aabb = osg::BoundingBox(10000 + i, 10000, -1, 10001 + i, 10001, 1);
            data.mOccluderMeshes.push_back(mesh);
        }
        auto nearby = std::make_shared<OcclusionCulling::OccluderMesh>();
        nearby->aabb = osg::BoundingBox(1, -1, -1, 10000, 1, 1);
        data.mOccluderMeshes.push_back(nearby);
        data.buildSpatialIndex();
        std::vector<std::shared_ptr<const OcclusionCulling::OccluderMesh>> found;
        std::vector<std::size_t> scratch;
        data.collectNearbyOccluders({}, 4, found, scratch);
        ASSERT_EQ(found.size(), 1);
        EXPECT_EQ(found.front(), nearby);
    }

    TEST_F(OcclusionDispatch, OutputFormatsAndNormalDefinesDoNotChangeWorldSubmissions)
    {
        auto hidden = triangle(-20), visible = triangle(-5);
        hidden->setCullCallback(new SceneUtil::OcclusionDrawableCallback(culler));
        world->addChild(hidden);
        world->addChild(visible);
        auto mesh = std::make_shared<OcclusionCulling::OccluderMesh>();
        mesh->vertices = { { -8, -6, -10 }, { 8, -6, -10 }, { 8, 6, -10 }, { -8, 6, -10 } };
        mesh->indices = { 0, 1, 2, 0, 2, 3 };
        for (const auto& vertex : mesh->vertices)
            mesh->aabb.expandBy(vertex);
        SceneUtil::OcclusionPlan plan;
        SceneUtil::OcclusionViewKey key;
        key.mCamera = camera;
        key.mProjection = *visitor.getProjectionMatrix();
        key.mViewport.set(0, 0, 256, 128);
        key.mNear = 1;
        for (unsigned int frame = 0; frame < 8; ++frame)
        {
            const auto before = SceneUtil::countRenderSubmissions(*stage).mLeaves;
            camera->getOrCreateStateSet()->setDefine("OMW_WRITE_NORMALS", frame % 2 ? "1" : "0");
            osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D;
            texture->setInternalFormat(frame % 3 ? GL_RGBA16F : GL_RGBA);
            osg::ref_ptr<osg::FrameBufferObject> framebuffer = new osg::FrameBufferObject;
            framebuffer->setAttachment(osg::Camera::COLOR_BUFFER0, osg::FrameBufferAttachment(texture));
            stage->setFrameBufferObject(framebuffer);
            culler->endFrame();
            culler->beginFrame(key.mView, key.mProjection, key.mNear, true);
            plan.start();
            plan.add(*culler, { mesh, {}, mesh->aabb, false });
            EXPECT_EQ(plan.prepare(key), frame != 0);
            if (!frame)
                culler->clearDepth();
            plan.rasterize(*culler);
            culler->bindVisitor(visitor);
            visitor.pushStateSet(camera->getOrCreateStateSet());
            world->traverse(visitor);
            visitor.popStateSet();
            EXPECT_EQ(SceneUtil::countRenderSubmissions(*stage).mLeaves - before, 1);
        }
    }

    TEST_F(OcclusionDispatch, NestedCameraGeometryIsCountedSeparatelyFromMainWorld)
    {
        world->addChild(triangle(-5));
        osg::ref_ptr<osg::Camera> reflection = new osg::Camera;
        reflection->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        reflection->setProjectionMatrix(*visitor.getProjectionMatrix());
        reflection->setRenderOrder(osg::Camera::PRE_RENDER);
        reflection->addChild(triangle(-5));
        world->addChild(reflection);
        world->traverse(visitor);
        EXPECT_EQ(SceneUtil::countRenderSubmissions(*stage).mGeometry, 1);
        EXPECT_EQ(SceneUtil::countDependentSubmissions(*stage).mGeometry, 1);
        EXPECT_TRUE(SceneUtil::hasUnknownCameraConsumers(*stage, nullptr));
    }

    TEST_F(OcclusionDispatch, OnlyTheDeclaredColorCompositorCanBeExcludedFromDepthConsumers)
    {
        osg::ref_ptr<osg::Camera> compositor = new osg::Camera;
        compositor->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        compositor->setProjectionMatrix(*visitor.getProjectionMatrix());
        compositor->setRenderOrder(osg::Camera::POST_RENDER);
        compositor->addChild(triangle(-5));
        world->addChild(compositor);
        world->traverse(visitor);
        EXPECT_TRUE(SceneUtil::hasUnknownCameraConsumers(*stage, nullptr));
        EXPECT_FALSE(SceneUtil::hasUnknownCameraConsumers(*stage, compositor));
    }

    TEST_F(OcclusionDispatch, LateGeometryKeepsDepthConsumersAndEmptyBinsDoNot)
    {
        auto* later = stage->find_or_insert(12, "RenderBin");
        EXPECT_FALSE(SceneUtil::hasLaterSubmissions(*stage, 10));
        auto geometry = triangle(-5);
        geometry->getOrCreateStateSet()->setRenderBinDetails(12, "RenderBin");
        geometry->accept(visitor);
        EXPECT_TRUE(SceneUtil::hasRenderSubmissions(*later));
        EXPECT_TRUE(SceneUtil::hasLaterSubmissions(*stage, 10));
        EXPECT_FALSE(SceneUtil::hasLaterSubmissions(*stage, 10, { 12 }));
    }

    TEST_F(OcclusionDispatch, ProtectedNodesAndOtherCamerasRemainVisible)
    {
        auto geometry = triangle(-20);
        geometry->setCullCallback(new SceneUtil::OcclusionDrawableCallback(culler));
        geometry->setUserValue("skipOcclusion", true);
        geometry->accept(visitor);
        EXPECT_EQ(leaves(), 1);
        geometry->setUserValue("skipOcclusion", false);
        osg::ref_ptr<osg::Camera> reflection = new osg::Camera;
        stage->setCamera(reflection);
        geometry->accept(visitor);
        EXPECT_EQ(leaves(), 2);
    }

    TEST_F(OcclusionDispatch, DrawableCallbackActuallyPreventsSubmission)
    {
        auto geometry = triangle(-20);
        geometry->setCullCallback(new SceneUtil::OcclusionDrawableCallback(culler));
        geometry->accept(visitor);
        EXPECT_EQ(leaves(), 0);
        geometry = triangle(-5);
        geometry->setCullCallback(new SceneUtil::OcclusionDrawableCallback(culler));
        geometry->accept(visitor);
        EXPECT_EQ(leaves(), 1);
    }

    TEST_F(OcclusionDispatch, FrustumMissDoesNotSpendAnOcclusionQuery)
    {
        auto geometry = triangle(-20);
        auto* vertices = static_cast<osg::Vec3Array*>(geometry->getVertexArray());
        for (auto& vertex : *vertices)
            vertex.x() += 1000;
        geometry->dirtyBound();
        geometry->setCullCallback(new SceneUtil::OcclusionDrawableCallback(culler));
        const auto before = culler->getTestCalls();
        geometry->accept(visitor);
        EXPECT_EQ(leaves(), 0);
        EXPECT_EQ(culler->getTestCalls(), before);
    }

    TEST_F(OcclusionDispatch, DisabledCullingIsRespectedBeforeAndAfterCallbackCloning)
    {
        auto geometry = triangle(-20);
        geometry->setCullingActive(false);
        osg::ref_ptr<SceneUtil::OcclusionDrawableCallback> original = new SceneUtil::OcclusionDrawableCallback(culler);
        original->setNestedCallback(new osg::Callback);
        osg::ref_ptr<osg::Callback> copy = dynamic_cast<osg::Callback*>(original->clone(osg::CopyOp::DEEP_COPY_ALL));
        ASSERT_NE(copy, nullptr);
        ASSERT_NE(copy->asDrawableCullCallback(), nullptr);
        EXPECT_EQ(copy->getNestedCallback(), original->getNestedCallback());
        geometry->setCullCallback(copy);
        geometry->accept(visitor);
        EXPECT_EQ(leaves(), 1);
        geometry->setCullingActive(true);
        geometry->accept(visitor);
        EXPECT_EQ(leaves(), 1);
    }

    TEST_F(OcclusionDispatch, RelativeTransformIsAppliedOnce)
    {
        osg::ref_ptr<osg::MatrixTransform> node = new osg::MatrixTransform(osg::Matrix::translate(0, 0, -7));
        node->addChild(triangle(0));
        node->setCullCallback(new SceneUtil::OcclusionNodeCallback(culler));
        node->accept(visitor);
        EXPECT_EQ(leaves(), 1);
    }

    TEST_F(OcclusionDispatch, StaticBudgetThrottlingDoesNotDisableAvailableDepthQueries)
    {
        culler->setStaticOccludersEnabled(false);
        culler->setSmallObjectTestingEnabled(true);
        auto geometry = triangle(-20);
        geometry->setCullCallback(new SceneUtil::OcclusionDrawableCallback(culler));
        geometry->accept(visitor);
        EXPECT_EQ(leaves(), 0);
        EXPECT_GT(SceneUtil::adaptiveOcclusionBudget(0.5, true), 0);
        EXPECT_LT(SceneUtil::adaptiveOcclusionBudget(0.5, true), 0.5);
        EXPECT_EQ(SceneUtil::adaptiveOcclusionBudget(0.05, true), 0.05);
        EXPECT_GT(SceneUtil::adaptiveOcclusionBudget(0, true), 0);
        EXPECT_EQ(SceneUtil::adaptiveOcclusionBudget(0, false), 0);
    }

    TEST_F(OcclusionDispatch, InstallingPagedCallbacksDoesNotMutateSharedTemplates)
    {
        auto original = triangle(-20);
        original->setUserValue("templateMarker", 42);
        osg::ref_ptr<osg::Geode> first = new osg::Geode;
        osg::ref_ptr<osg::Geode> second = new osg::Geode;
        first->addDrawable(original);
        second->addDrawable(original);
        SceneUtil::InstallOcclusionDrawables install(culler);
        first->accept(install);
        EXPECT_NE(first->getDrawable(0), original.get());
        EXPECT_EQ(second->getDrawable(0), original.get());
        EXPECT_EQ(original->getCullCallback(), nullptr);
        EXPECT_NE(first->getDrawable(0)->getUserDataContainer(), original->getUserDataContainer());
        EXPECT_NE(first->getDrawable(0)->getCullCallback()->asDrawableCullCallback(), nullptr);
        first->accept(visitor);
        EXPECT_EQ(leaves(), 0);
    }

    class RecordOrderCallback : public SceneUtil::NodeCallback<RecordOrderCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        RecordOrderCallback(std::vector<int>& order, int id)
            : mOrder(order)
            , mId(id)
        {
        }
        void operator()(osg::Node*, osgUtil::CullVisitor*) { mOrder.push_back(mId); }

    private:
        std::vector<int>& mOrder;
        int mId;
    };

    TEST_F(OcclusionDispatch, MainCameraVisitsTerrainThenNearCellsWithoutReorderingTheGraph)
    {
        camera->setName("main");
        osg::ref_ptr<SceneUtil::OcclusionOrderGroup> group = new SceneUtil::OcclusionOrderGroup("main", 1);
        std::vector<int> order;
        for (const auto [id, distance] : { std::pair{ 0, 30.f }, { 1, 20.f }, { 2, 5.f }, { 3, 5.f } })
        {
            osg::ref_ptr<osg::Group> cell = new osg::Group;
            cell->setInitialBound(osg::BoundingSphere(osg::Vec3f(0, 0, -distance), 1));
            cell->setCullCallback(new RecordOrderCallback(order, id));
            group->addChild(cell);
        }
        group->accept(visitor);
        EXPECT_EQ(order, (std::vector<int>{ 0, 2, 3, 1 }));
        order.clear();
        camera->setName("reflection");
        group->accept(visitor);
        EXPECT_EQ(order, (std::vector<int>{ 0, 1, 2, 3 }));
    }
}
