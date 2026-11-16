#ifndef OPENMW_COMPONENTS_RENDERASSETS_OSGIMPORTER_H
#define OPENMW_COMPONENTS_RENDERASSETS_OSGIMPORTER_H

#include "model.hpp"

#include <memory>
#include <optional>
#include <string_view>

#include <osg/Array>
#include <osg/Matrixf>
#include <osg/observer_ptr>

namespace osg
{
    class Node;
    class Image;
    class Geometry;
    class Texture;
}

namespace RenderAssets
{
    struct MeshBuffers
    {
        std::vector<Render::Vertex> mVertices;
        std::vector<std::uint32_t> mIndices;
    };

    struct MeshStream : MeshBuffers
    {
        // Packed primitive-set/source-vertex pairs preserve split channels.
        std::vector<std::uint64_t> mVertexSources;
        std::size_t mSourceVertexCount = 0;
        unsigned mUVUnit = 0;
        std::array<osg::observer_ptr<const osg::Array>, 3> mStaticChannels;
        std::array<unsigned, 3> mStaticRevisions{};
    };

    // CPU import bridge for existing readers. It consumes raw, immutable
    // loader output before GL shader rewriting and never traverses live scenes.
    // View-dependent/animated nodes and unsupported material operations report
    // errors instead of freezing them into a misleading static asset.
    class OsgImporter
    {
    public:
        explicit OsgImporter(std::uint32_t traversalMask = ~std::uint32_t{ 0 });
        ~OsgImporter();
        OsgImporter(const OsgImporter&) = delete;
        OsgImporter& operator=(const OsgImporter&) = delete;

        std::shared_ptr<const Render::ModelAsset> import(const osg::Node& root, std::string_view name);
        // Live OSG textures upload rows verbatim regardless of origin metadata;
        // fonts, video and generated UI images depend on that convention.
        std::shared_ptr<const Render::ImageAsset> importImage(const osg::Image& image, bool preserveRows = false);
        // Borrow already uploadable, tightly packed RGBA8/BC mip data. The
        // image and caller-owned level metadata must outlive the synchronous
        // backend upload call. Rows are preserved for live OSG images.
        // Other layouts return no view and use the normal conversion path.
        static std::optional<Render::TextureData> borrowTexture(
            const osg::Image& image, std::vector<Render::TextureLevel>& levels, bool mipmaps = true);
        // Import only changed geometry from a CPU cull result. GPU names and
        // GL programs are deliberately not part of this bridge.
        std::shared_ptr<const Render::MeshAsset> importMesh(
            const osg::Geometry& geometry, unsigned uvUnit = 0, const osg::Matrixf& uvTransform = {});
        // Resident draws already have OSG bounds and packed normals. Import
        // only GPU buffers, without constructing asset-only sidecars or bounds.
        MeshBuffers importMeshBuffers(const osg::Geometry& geometry, unsigned uvUnit = 0);
        // Animated streams retain the source mapping and triangle topology.
        // Reimport after changing indices, primitive modes or channel bindings.
        MeshStream importMeshStream(const osg::Geometry& geometry, unsigned uvUnit = 0);
        void updateMeshStream(const osg::Geometry& geometry, MeshStream& stream);
        static Render::SamplerState importSampler(const osg::Texture& texture);
        void clear();

    private:
        class Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
