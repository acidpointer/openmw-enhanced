#include "occludermesh.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>

#include <osg/AlphaFunc>
#include <osg/AutoTransform>
#include <osg/Billboard>
#include <osg/BoundingBox>
#include <osg/ComputeBoundsVisitor>
#include <osg/CullFace>
#include <osg/FrontFace>
#include <osg/Geometry>
#include <osg/LOD>
#include <osg/NodeVisitor>
#include <osg/PolygonMode>
#include <osg/Sequence>
#include <osg/Switch>
#include <osg/Transform>
#include <osg/UserDataContainer>

namespace
{
    class CollectMeshVisitor : public osg::NodeVisitor
    {
    public:
        explicit CollectMeshVisitor(osg::Transform* skippedRootTransform = nullptr)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mSkippedRootTransform(skippedRootTransform)
        {
        }

        void apply(osg::Transform& transform) override
        {
            osg::Matrix matrix;
            if (!mMatrixStack.empty())
                matrix = mMatrixStack.back();
            if (&transform != mSkippedRootTransform)
                transform.computeLocalToWorldMatrix(matrix, this);
            mMatrixStack.push_back(matrix);
            traverse(transform);
            mMatrixStack.pop_back();
        }

        void apply(osg::Drawable& drawable) override
        {
            auto* geom = drawable.asGeometry();
            if (!geom)
                return;

            // Skip alpha-tested drawables (e.g. tree leaf billboards) —
            // their transparent regions would cause false occlusion.
            if (hasUnsafeState())
                return;

            const auto* verts = dynamic_cast<const osg::Vec3Array*>(geom->getVertexArray());
            if (!verts || verts->empty())
                return;

            osg::Matrix matrix;
            if (!mMatrixStack.empty())
                matrix = mMatrixStack.back();

            unsigned int baseVertex = static_cast<unsigned int>(mVertices.size());
            for (const auto& v : *verts)
                mVertices.push_back(v * matrix);

            for (unsigned int p = 0; p < geom->getNumPrimitiveSets(); ++p)
            {
                const auto* primitive = geom->getPrimitiveSet(p);
                bool valid = true;
                for (unsigned int i = 0; i < primitive->getNumIndices(); ++i)
                    if (primitive->index(i) >= verts->size())
                        valid = false;
                if (valid)
                    collectTriangles(primitive, baseVertex);
            }
        }

        std::vector<osg::Vec3f> mVertices;
        std::vector<unsigned int> mIndices;

    private:
        static bool unsafeStateSet(const osg::StateSet* state)
        {
            if (!state)
                return false;
            if (state->getUpdateCallback() || state->getAttribute(osg::StateAttribute::ALPHAFUNC)
                || (state->getMode(GL_BLEND) & osg::StateAttribute::ON)
                || state->getRenderingHint() == osg::StateSet::TRANSPARENT_BIN)
                return true;
            if (const auto* mode
                = dynamic_cast<const osg::PolygonMode*>(state->getAttribute(osg::StateAttribute::POLYGONMODE)))
                if (mode->getMode(osg::PolygonMode::FRONT) != osg::PolygonMode::FILL
                    || mode->getMode(osg::PolygonMode::BACK) != osg::PolygonMode::FILL)
                    return true;
            if (const auto* face
                = dynamic_cast<const osg::CullFace*>(state->getAttribute(osg::StateAttribute::CULLFACE)))
                if (face->getMode() != osg::CullFace::BACK)
                    return true;
            if (const auto* front
                = dynamic_cast<const osg::FrontFace*>(state->getAttribute(osg::StateAttribute::FRONTFACE)))
                if (front->getMode() != osg::FrontFace::COUNTER_CLOCKWISE)
                    return true;
            return false;
        }

        bool hasUnsafeState() const
        {
            // Only the visited path is stable while templates share drawables
            // with instances. Do not walk a drawable's mutable parent list.
            for (const auto* node : getNodePath())
            {
                if (node->getUpdateCallback() || dynamic_cast<const osg::Switch*>(node)
                    || dynamic_cast<const osg::LOD*>(node) || dynamic_cast<const osg::Sequence*>(node)
                    || dynamic_cast<const osg::Billboard*>(node) || dynamic_cast<const osg::AutoTransform*>(node))
                    return true;
                const auto* state = node->getStateSet();
                if (unsafeStateSet(state))
                    return true;
                // Shader conversion moves fixed-function alpha tests here.
                if (state && state->getUserDataContainer())
                    if (unsafeStateSet(dynamic_cast<const osg::StateSet*>(
                            state->getUserDataContainer()->getUserObject("removedState"))))
                        return true;
            }
            return false;
        }

        void collectTriangles(const osg::PrimitiveSet* pset, unsigned int baseVertex)
        {
            unsigned int count = pset->getNumIndices();
            switch (pset->getMode())
            {
                case GL_TRIANGLES:
                    for (unsigned int i = 0; i + 2 < count; i += 3)
                    {
                        mIndices.push_back(baseVertex + pset->index(i));
                        mIndices.push_back(baseVertex + pset->index(i + 1));
                        mIndices.push_back(baseVertex + pset->index(i + 2));
                    }
                    break;
                case GL_TRIANGLE_STRIP:
                    for (unsigned int i = 0; i + 2 < count; ++i)
                    {
                        if (i % 2 == 0)
                        {
                            mIndices.push_back(baseVertex + pset->index(i));
                            mIndices.push_back(baseVertex + pset->index(i + 1));
                            mIndices.push_back(baseVertex + pset->index(i + 2));
                        }
                        else
                        {
                            mIndices.push_back(baseVertex + pset->index(i + 1));
                            mIndices.push_back(baseVertex + pset->index(i));
                            mIndices.push_back(baseVertex + pset->index(i + 2));
                        }
                    }
                    break;
                case GL_TRIANGLE_FAN:
                    for (unsigned int i = 1; i + 1 < count; ++i)
                    {
                        mIndices.push_back(baseVertex + pset->index(0));
                        mIndices.push_back(baseVertex + pset->index(i));
                        mIndices.push_back(baseVertex + pset->index(i + 1));
                    }
                    break;
                default:
                    break;
            }
        }

        std::vector<osg::Matrix> mMatrixStack;
        osg::Transform* mSkippedRootTransform = nullptr;
    };
}

namespace OcclusionCulling
{
    namespace
    {
    OccluderMesh buildSimplifiedMeshImpl(
        osg::Node* node, int gridRes, float shrinkFactor, osg::Transform* skippedRootTransform)
    {
        OccluderMesh mesh;
        if (!node || !std::isfinite(shrinkFactor))
            return mesh;
        CollectMeshVisitor collector(skippedRootTransform);
        node->accept(collector);
        struct Triangle
        {
            std::size_t offset;
            double area;
        };
        std::vector<Triangle> triangles;
        triangles.reserve(collector.mIndices.size() / 3);
        for (std::size_t i = 0; i + 2 < collector.mIndices.size(); i += 3)
        {
            const osg::Vec3d a(collector.mVertices[collector.mIndices[i]]);
            const osg::Vec3d b(collector.mVertices[collector.mIndices[i + 1]]);
            const osg::Vec3d c(collector.mVertices[collector.mIndices[i + 2]]);
            const double area = ((b - a) ^ (c - a)).length2();
            if (std::isfinite(area) && area > 0)
                triangles.push_back({ i, area });
        }
        // A subset of original opaque triangles cannot bridge windows or move
        // concave surfaces into empty space, unlike vertex clustering or
        // shrinking the entire model towards a single centroid.
        const std::size_t resolution = std::clamp(gridRes, 1, 256);
        const std::size_t limit = 2 * resolution * resolution;
        if (triangles.size() > limit)
        {
            std::nth_element(triangles.begin(), triangles.begin() + limit, triangles.end(),
                [](const Triangle& a, const Triangle& b) {
                    return a.area != b.area ? a.area > b.area : a.offset < b.offset;
                });
            triangles.resize(limit);
        }
        std::sort(triangles.begin(), triangles.end(),
            [](const Triangle& a, const Triangle& b) { return a.offset < b.offset; });
        const double shrink = std::clamp(static_cast<double>(shrinkFactor), 0.0, 1.0);
        mesh.vertices.reserve(triangles.size() * 3);
        mesh.indices.reserve(triangles.size() * 3);
        for (const auto& triangle : triangles)
        {
            osg::Vec3d vertices[3];
            for (int i = 0; i < 3; ++i)
                vertices[i] = osg::Vec3d(collector.mVertices[collector.mIndices[triangle.offset + i]]);
            const osg::Vec3d center = (vertices[0] + vertices[1] + vertices[2]) / 3.0;
            for (const auto& vertex : vertices)
            {
                const osg::Vec3f position(shrink == 1 ? vertex : center + (vertex - center) * shrink);
                mesh.indices.push_back(static_cast<unsigned int>(mesh.vertices.size()));
                mesh.vertices.push_back(position);
                mesh.aabb.expandBy(position);
            }
        }
        return mesh;
    }
    }

    OccluderMesh buildSimplifiedMesh(osg::Node* node, int gridRes, float shrinkFactor)
    {
        return buildSimplifiedMeshImpl(node, gridRes, shrinkFactor, nullptr);
    }

    OccluderMesh buildSimplifiedMeshWithoutRootTransform(osg::Node* node, int gridRes, float shrinkFactor)
    {
        return buildSimplifiedMeshImpl(node, gridRes, shrinkFactor, node ? node->asTransform() : nullptr);
    }
}
