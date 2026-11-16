#include "osgimporter.hpp"

#include <components/render/cachesweep.hpp>
#include <components/render/reserve.hpp>

#include <components/nifosg/autotransform.hpp>
#include <components/render/math.hpp>
#include <components/render/texture.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/texturetype.hpp>

#include <osg/AlphaFunc>
#include <osg/AutoTransform>
#include <osg/Billboard>
#include <osg/BlendEquation>
#include <osg/BlendFunc>
#include <osg/Camera>
#include <osg/CullFace>
#include <osg/Depth>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/LOD>
#include <osg/Material>
#include <osg/Sequence>
#include <osg/StateSet>
#include <osg/Switch>
#include <osg/TexEnv>
#include <osg/TexMat>
#include <osg/Texture2D>
#include <osg/observer_ptr>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>

namespace RenderAssets
{
    namespace
    {
        std::uint8_t byteColor(float value)
        {
            if (!std::isfinite(value))
                throw std::invalid_argument("Model contains a nonfinite color");
            return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.f, 1.f) * 255));
        }

        std::array<float, 4> color(const osg::Vec4f& value)
        {
            return { value.x(), value.y(), value.z(), value.w() };
        }

        template <class T>
        float component(const void* data, std::size_t index, bool normalize)
        {
            T value;
            std::memcpy(&value, static_cast<const std::byte*>(data) + index * sizeof(T), sizeof(T));
            if constexpr (std::is_integral_v<T>)
                if (normalize)
                    return std::max(-1.f, static_cast<float>(double(value) / double(std::numeric_limits<T>::max())));
            return static_cast<float>(value);
        }

        std::array<float, 4> readArray(
            const osg::Array& array, unsigned index, bool normalizedColor = false, bool finiteRequired = true)
        {
            if (index >= array.getNumElements())
                throw std::invalid_argument("Model vertex channel is shorter than its binding requires");
            const auto count = array.getDataSize();
            if (count < 1 || count > 4)
                throw std::invalid_argument("Unsupported model vertex channel width");
            const bool normalize = normalizedColor || array.getNormalize();
            const auto data = array.getDataPointer(index);
            std::array<float, 4> result{ 0, 0, 0, 1 };
            // Float channels already use the native scalar representation.
            // Copy once rather than dispatching the type for each component.
            if (array.getDataType() == GL_FLOAT)
            {
                std::memcpy(result.data(), data, static_cast<std::size_t>(count) * sizeof(float));
                if (finiteRequired
                    && !std::all_of(result.begin(), result.begin() + count, [](float v) { return std::isfinite(v); }))
                    throw std::invalid_argument("Model contains a nonfinite vertex channel");
                return result;
            }
            for (int i = 0; i < count; ++i)
            {
                switch (array.getDataType())
                {
                    case GL_FLOAT:
                        result[i] = component<float>(data, i, false);
                        break;
                    case GL_DOUBLE:
                        result[i] = component<double>(data, i, false);
                        break;
                    case GL_BYTE:
                        result[i] = component<std::int8_t>(data, i, normalize);
                        break;
                    case GL_UNSIGNED_BYTE:
                        result[i] = component<std::uint8_t>(data, i, normalize);
                        break;
                    case GL_SHORT:
                        result[i] = component<std::int16_t>(data, i, normalize);
                        break;
                    case GL_UNSIGNED_SHORT:
                        result[i] = component<std::uint16_t>(data, i, normalize);
                        break;
                    case GL_INT:
                        result[i] = component<std::int32_t>(data, i, normalize);
                        break;
                    case GL_UNSIGNED_INT:
                        result[i] = component<std::uint32_t>(data, i, normalize);
                        break;
                    default:
                        throw std::invalid_argument("Unsupported model vertex channel type");
                }
                if (finiteRequired && !std::isfinite(result[i]))
                    throw std::invalid_argument("Model contains a nonfinite vertex channel");
            }
            return result;
        }

        unsigned channelIndex(osg::Array::Binding binding, unsigned vertex, unsigned primitiveSet)
        {
            switch (binding)
            {
                case osg::Array::BIND_OVERALL:
                    return 0;
                case osg::Array::BIND_PER_VERTEX:
                    return vertex;
                case osg::Array::BIND_PER_PRIMITIVE_SET:
                    return primitiveSet;
                default:
                    throw std::invalid_argument("Unsupported model vertex channel binding");
            }
        }

        template <class Index, class Emit>
        void triangles(GLenum mode, std::size_t count, const Index& index, const Emit& emit)
        {
            switch (mode)
            {
                case GL_TRIANGLES:
                    if (count % 3)
                        throw std::invalid_argument("Triangle primitive has incomplete indices");
                    for (std::size_t i = 0; i < count; i += 3)
                        emit(index(i), index(i + 1), index(i + 2));
                    break;
                case GL_TRIANGLE_STRIP:
                    for (std::size_t i = 2; i < count; ++i)
                        if (i % 2)
                            emit(index(i - 2), index(i), index(i - 1));
                        else
                            emit(index(i - 2), index(i - 1), index(i));
                    break;
                case GL_POLYGON:
                case GL_TRIANGLE_FAN:
                    for (std::size_t i = 2; i < count; ++i)
                        emit(index(0), index(i - 1), index(i));
                    break;
                case GL_QUADS:
                    if (count % 4)
                        throw std::invalid_argument("Quad primitive has incomplete indices");
                    for (std::size_t i = 0; i < count; i += 4)
                    {
                        emit(index(i), index(i + 1), index(i + 2));
                        emit(index(i), index(i + 2), index(i + 3));
                    }
                    break;
                case GL_QUAD_STRIP:
                    if (count % 2)
                        throw std::invalid_argument("Quad strip has incomplete indices");
                    for (std::size_t i = 3; i < count; i += 2)
                    {
                        emit(index(i - 3), index(i - 2), index(i - 1));
                        emit(index(i - 2), index(i), index(i - 1));
                    }
                    break;
                default:
                    throw std::invalid_argument("Model primitive is not a supported triangle topology");
            }
        }

        // Resolve channel layout once per mesh, rather than dispatching OSG
        // virtual getters for every component of every imported vertex.
        struct ArrayView
        {
            const osg::Array* mArray;
            const std::byte* mPacked = nullptr;
            const osg::Vec4ubArray* mBytes = nullptr;
            unsigned mCount = 0;
            unsigned mWidth = 0;
            osg::Array::Binding mBinding = osg::Array::BIND_OFF;

            explicit ArrayView(const osg::Array* array)
                : mArray(array)
            {
                if (!array)
                    return;
                mCount = array->getNumElements();
                mWidth = array->getDataSize();
                mBinding = array->getBinding();
                // Only known contiguous OSG array types use direct addressing.
                // Unusual/integer/custom arrays retain the general reader.
                const auto& type = typeid(*array);
                if (type == typeid(osg::Vec2Array) || type == typeid(osg::Vec3Array) || type == typeid(osg::Vec4Array))
                    mPacked = static_cast<const std::byte*>(array->getDataPointer());
                if (type == typeid(osg::Vec4ubArray))
                    mBytes = static_cast<const osg::Vec4ubArray*>(array);
            }

            std::array<float, 4> read(unsigned index, bool color = false, bool finite = true) const
            {
                if (!mPacked)
                    return readArray(*mArray, index, color, finite);
                if (index >= mCount)
                    throw std::invalid_argument("Model vertex channel is shorter than its binding requires");
                std::array<float, 4> result{ 0, 0, 0, 1 };
                const auto* data = mPacked + std::size_t(index) * mWidth * sizeof(float);
                // Fixed copy widths inline into loads/stores. A runtime-sized
                // memcpy here can otherwise become a call for every channel.
                switch (mWidth)
                {
                    case 2:
                        std::memcpy(result.data(), data, 2 * sizeof(float));
                        break;
                    case 3:
                        std::memcpy(result.data(), data, 3 * sizeof(float));
                        break;
                    default:
                        std::memcpy(result.data(), data, 4 * sizeof(float));
                        break;
                }
                if (finite && !std::all_of(result.begin(), result.begin() + mWidth, [](float value) {
                        return std::isfinite(value);
                    }))
                    throw std::invalid_argument("Model contains a nonfinite vertex channel");
                return result;
            }
        };

        class VertexReader
        {
            ArrayView mPositions, mUV, mMaskUV, mColors, mNormals;
            const osg::Matrixf& mUVMatrix;
            bool mIdentityUV;
            unsigned mNormalIndex = std::numeric_limits<unsigned>::max();
            unsigned mColorIndex = std::numeric_limits<unsigned>::max();
            std::array<float, 3> mNormal{ 0, 0, 1 };
            Render::Vertex mColor;

        public:
            VertexReader(const osg::Geometry& geometry, unsigned uvUnit, const osg::Matrixf& matrix)
                : mPositions(geometry.getVertexArray())
                , mUV(geometry.getTexCoordArray(uvUnit))
                , mMaskUV(geometry.getTexCoordArray(0))
                , mColors(geometry.getColorArray())
                , mNormals(geometry.getNormalArray())
                , mUVMatrix(matrix)
                , mIdentityUV(matrix.isIdentity())
            {
            }

            Render::Vertex read(unsigned source, unsigned set)
            {
                Render::Vertex vertex{};
                const auto position = mPositions.read(source);
                std::copy_n(position.begin(), 3, vertex.mPosition.begin());
                std::array<float, 4> coord{};
                if (mUV.mArray)
                {
                    coord = mUV.read(source);
                    const osg::Vec4f transformed = mIdentityUV ? osg::Vec4f(coord[0], coord[1], 0, 1)
                                                               : osg::Vec4f(coord[0], coord[1], 0, 1) * mUVMatrix;
                    if (!std::isfinite(transformed.x()) || !std::isfinite(transformed.y()) || transformed.w() != 1)
                        throw std::invalid_argument("Unsupported projective model texture transform");
                    vertex.mUV = { transformed.x(), transformed.y() };
                }
                if (mMaskUV.mArray)
                {
                    const auto mask = mUV.mArray == mMaskUV.mArray ? coord : mMaskUV.read(source);
                    vertex.mMaskUV = { mask[0], mask[1] };
                }
                if (mColors.mArray && mColors.mBinding != osg::Array::BIND_OFF)
                {
                    const auto index = channelIndex(mColors.mBinding, source, set);
                    if (index != mColorIndex)
                    {
                        if (mColors.mBytes)
                        {
                            if (index >= mColors.mCount)
                                throw std::invalid_argument(
                                    "Model vertex channel is shorter than its binding requires");
                            std::memcpy(mColor.mColor.data(), (*mColors.mBytes)[index].ptr(), 4);
                            mColor.mAlpha = static_cast<float>(double(mColor.mColor[3]) / 255.0);
                        }
                        else
                        {
                            const auto value = mColors.read(index, true);
                            for (unsigned i = 0; i < 4; ++i)
                                mColor.mColor[i] = byteColor(value[i]);
                            mColor.mAlpha = std::clamp(value[3], 0.f, 1.f);
                        }
                        mColorIndex = index;
                    }
                    vertex.mColor = mColor.mColor;
                    vertex.mAlpha = mColor.mAlpha;
                }
                if (mNormals.mArray && mNormals.mBinding != osg::Array::BIND_OFF)
                {
                    const auto index = channelIndex(mNormals.mBinding, source, set);
                    if (index != mNormalIndex)
                    {
                        const auto value = mNormals.read(index, false, false);
                        mNormal = { 0, 0, 1 };
                        const double length
                            = double(value[0]) * value[0] + double(value[1]) * value[1] + double(value[2]) * value[2];
                        if (length > 1e-20 && std::isfinite(length))
                        {
                            const double inverse = 1.0 / std::sqrt(length);
                            for (unsigned i = 0; i < 3; ++i)
                                mNormal[i] = static_cast<float>(value[i] * inverse);
                        }
                        mNormalIndex = index;
                    }
                    vertex.mNormal = mNormal;
                }
                return vertex;
            }
        };

        std::shared_ptr<Render::MeshAsset> mesh(const osg::Geometry& geometry, unsigned uvUnit,
            const osg::Matrixf& uvMatrix, std::vector<std::uint64_t>* vertexSources = nullptr,
            bool assetMetadata = true)
        {
            const auto* positions = geometry.getVertexArray();
            if (!positions || positions->getDataSize() != 3)
                throw std::invalid_argument("Static model geometry requires three-component positions");
            const auto* uv = geometry.getTexCoordArray(uvUnit);
            if (uv && uv->getDataSize() != 2)
                throw std::invalid_argument("Static model geometry requires two-component texture coordinates");
            const auto sourceCount = positions->getNumElements();
            const auto* colors = geometry.getColorArray();
            const auto* normals = geometry.getNormalArray();
            auto result = std::make_shared<Render::MeshAsset>();
            // Reuse source vertices whenever channel bindings permit. A
            // per-primitive-set channel gets a separate key for that set.
            constexpr auto missingVertex = std::numeric_limits<std::uint32_t>::max();
            std::vector<std::uint32_t> vertices(sourceCount, missingVertex);
            result->mVertices.reserve(sourceCount);
            // Live streams and resident draws consume packed vertices, not the
            // asset-only normal sidecar or bounds. Skip unused metadata.
            const bool collectNormals
                = assetMetadata && !vertexSources && normals && normals->getBinding() != osg::Array::BIND_OFF;
            if (collectNormals)
                result->mNormals.reserve(sourceCount);
            VertexReader reader(geometry, uvUnit, uvMatrix);
            if (vertexSources)
                vertexSources->reserve(sourceCount);
            const bool perSet = (colors && colors->getBinding() == osg::Array::BIND_PER_PRIMITIVE_SET)
                || (normals && normals->getBinding() == osg::Array::BIND_PER_PRIMITIVE_SET);
            for (unsigned set = 0; set < geometry.getNumPrimitiveSets(); ++set)
            {
                // Per-set channels need independent packed vertices. All other
                // channels share this dense source-index remap across sets;
                // there is no hash lookup/node allocation per imported vertex.
                if (perSet && set)
                    std::fill(vertices.begin(), vertices.end(), missingVertex);
                const auto* primitive = geometry.getPrimitiveSet(set);
                if (!primitive)
                    throw std::invalid_argument("Model contains an absent primitive set");
                if (primitive->getNumInstances() > 1)
                    throw std::invalid_argument(
                        "Imported model contains unsupported shader-driven primitive instancing");
                const std::size_t indexCount = primitive->getNumIndices();
                const std::size_t expansion = primitive->getMode() == GL_TRIANGLES ? 1 : 3;
                if (indexCount > (result->mIndices.max_size() - result->mIndices.size()) / expansion)
                    throw std::length_error("Imported mesh exceeds index storage capacity");
                Render::reserveFor(result->mIndices, result->mIndices.size() + indexCount * expansion);
                const auto emit = [&](unsigned a, unsigned b, unsigned c) {
                    if (a >= sourceCount || b >= sourceCount || c >= sourceCount)
                        throw std::invalid_argument("Model triangle index exceeds its vertex data");
                    if (a == b || b == c || a == c)
                        return;
                    for (const unsigned source : { a, b, c })
                    {
                        auto& packed = vertices[source];
                        if (packed != missingVertex)
                        {
                            result->mIndices.push_back(packed);
                            continue;
                        }
                        if (result->mVertices.size() >= std::numeric_limits<std::uint32_t>::max())
                            throw std::length_error("Imported mesh exceeds 32-bit vertex indexing");
                        const auto vertex = reader.read(source, set);
                        const auto id = static_cast<std::uint32_t>(result->mVertices.size());
                        packed = id;
                        if (vertexSources)
                            vertexSources->push_back((std::uint64_t{ set } << 32) | source);
                        result->mVertices.push_back(vertex);
                        if (collectNormals)
                            result->mNormals.push_back(vertex.mNormal);
                        result->mIndices.push_back(id);
                    }
                };
                if (const auto* multi = dynamic_cast<const osg::MultiDrawArrays*>(primitive))
                {
                    if (multi->getFirsts().size() != multi->getCounts().size())
                        throw std::invalid_argument("Multi-draw primitive has mismatched ranges");
                    for (std::size_t range = 0; range < multi->getFirsts().size(); ++range)
                    {
                        const auto start = multi->getFirsts()[range];
                        const auto count = multi->getCounts()[range];
                        if (start < 0 || count < 0 || std::uint64_t(start) + std::uint64_t(count) > sourceCount)
                            throw std::invalid_argument("Multi-draw range exceeds its vertex array");
                        triangles(
                            primitive->getMode(), count, [&](auto i) { return static_cast<unsigned>(start + i); },
                            emit);
                    }
                }
                else if (const auto* lengths = dynamic_cast<const osg::DrawArrayLengths*>(primitive))
                {
                    if (lengths->getFirst() < 0)
                        throw std::invalid_argument("Model draw range starts before its vertex array");
                    std::uint64_t start = lengths->getFirst();
                    for (const auto length : *lengths)
                    {
                        if (length < 0 || start + static_cast<unsigned>(length) > sourceCount)
                            throw std::invalid_argument("Model draw range exceeds its vertex array");
                        triangles(
                            primitive->getMode(), length, [&](auto i) { return static_cast<unsigned>(start + i); },
                            emit);
                        start += length;
                    }
                }
                else if (const auto* range = dynamic_cast<const osg::DrawArrays*>(primitive))
                {
                    if (range->getFirst() < 0 || std::uint64_t(range->getFirst()) + indexCount > sourceCount)
                        throw std::invalid_argument("Model draw range exceeds its vertex array");
                    triangles(
                        primitive->getMode(), indexCount,
                        [&](auto i) { return static_cast<unsigned>(range->getFirst()) + static_cast<unsigned>(i); },
                        emit);
                }
                else if (const auto* small = dynamic_cast<const osg::DrawElementsUByte*>(primitive))
                    triangles(primitive->getMode(), indexCount, [&](auto i) { return (*small)[i]; }, emit);
                else if (const auto* medium = dynamic_cast<const osg::DrawElementsUShort*>(primitive))
                    triangles(primitive->getMode(), indexCount, [&](auto i) { return (*medium)[i]; }, emit);
                else if (const auto* large = dynamic_cast<const osg::DrawElementsUInt*>(primitive))
                    triangles(primitive->getMode(), indexCount, [&](auto i) { return (*large)[i]; }, emit);
                else
                    triangles(
                        primitive->getMode(), indexCount,
                        [&](auto i) { return primitive->index(static_cast<unsigned>(i)); }, emit);
            }
            if (result->mVertices.empty())
                throw std::invalid_argument("Imported model geometry has no nondegenerate triangles");
            if (assetMetadata && !vertexSources)
            {
                result->mBounds = { result->mVertices.front().mPosition, result->mVertices.front().mPosition };
                for (const auto& vertex : result->mVertices)
                    for (std::size_t i = 0; i < 3; ++i)
                    {
                        result->mBounds.mMin[i] = std::min(result->mBounds.mMin[i], vertex.mPosition[i]);
                        result->mBounds.mMax[i] = std::max(result->mBounds.mMax[i], vertex.mPosition[i]);
                    }
            }
            return result;
        }

        std::optional<Render::TextureFormat> compression(GLenum format)
        {
            switch (format)
            {
                case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
                case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
                    return Render::TextureFormat::BC1RGB;
                case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
                case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
                    return Render::TextureFormat::BC1RGBA;
                case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
                case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
                    return Render::TextureFormat::BC2;
                case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
                case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
                    return Render::TextureFormat::BC3;
                case GL_COMPRESSED_RED_RGTC1:
                    return Render::TextureFormat::BC4;
                case GL_COMPRESSED_RG_RGTC2:
                    return Render::TextureFormat::BC5;
                case GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT:
                    return Render::TextureFormat::BC6Unsigned;
                case GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT:
                    return Render::TextureFormat::BC6Signed;
                case GL_COMPRESSED_RGBA_BPTC_UNORM:
                case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM:
                    return Render::TextureFormat::BC7RGBA;
                default:
                    return std::nullopt;
            }
        }

        std::optional<Render::TextureFormat> pixelLayout(GLenum format)
        {
            using F = Render::TextureFormat;
            switch (format)
            {
                case GL_RGBA:
                    return F::RGBA8;
                case GL_BGRA:
                    return F::BGRA8;
                case GL_RGB:
                    return F::RGB8;
                case GL_BGR:
                    return F::BGR8;
                case GL_RED:
                    return F::R8;
                case GL_RG:
                    return F::RG8;
                case GL_LUMINANCE:
                    return F::Luminance8;
                case GL_LUMINANCE_ALPHA:
                    return F::LuminanceAlpha8;
                case GL_ALPHA:
                    return F::Alpha8;
                default:
                    return {};
            }
        }

        bool srgb(GLint format)
        {
            switch (format)
            {
                case GL_SRGB8:
                case GL_SRGB8_ALPHA8:
                case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM:
                case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
                case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
                case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
                case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
                    return true;
                default:
                    return false;
            }
        }

        std::shared_ptr<const Render::ImageAsset> image(const osg::Image& source, bool decode, bool preserveRows)
        {
            if (source.s() <= 0 || source.t() <= 0 || source.r() != 1 || !source.data())
                throw std::invalid_argument("Model texture must contain a nonempty 2D image");
            const auto compressed = compression(source.getPixelFormat());
            if (source.isCompressed() && !compressed)
                throw std::invalid_argument("Model texture uses an unsupported compression format");
            const bool cpuBlockDecode = compressed && *compressed >= Render::TextureFormat::BC1RGB
                && *compressed <= Render::TextureFormat::BC3;
            if (decode && compressed && !cpuBlockDecode)
                throw std::invalid_argument("Texture compression requires native device support");
            if (source.isCompressed() && !preserveRows && source.getOrigin() != osg::Image::TOP_LEFT)
                throw std::invalid_argument(
                    "Compressed model textures must be normalized to top-left origin by the image loader");
            if (!source.isCompressed())
                switch (source.getDataType())
                {
                    case GL_UNSIGNED_BYTE:
                    case GL_BYTE:
                    case GL_UNSIGNED_SHORT:
                    case GL_SHORT:
                    case GL_UNSIGNED_INT:
                    case GL_INT:
                    case GL_FLOAT:
                        break;
                    default:
                        throw std::invalid_argument("Unsupported texture sample type "
                            + std::to_string(source.getDataType()) + " in " + source.getFileName());
                }
            const bool scalarConversion = !compressed && source.getDataType() != GL_UNSIGNED_BYTE;
            const bool floating = scalarConversion && !srgb(source.getInternalTextureFormat());
            if (!source.isCompressed())
                switch (source.getPixelFormat())
                {
                    case GL_RGB:
                    case GL_RGBA:
                    case GL_BGR:
                    case GL_BGRA:
                    case GL_RED:
                    case GL_RG:
                    case GL_LUMINANCE:
                    case GL_LUMINANCE_ALPHA:
                    case GL_ALPHA:
                        break;
                    default:
                        throw std::invalid_argument("Model texture has an unsupported channel layout");
                }
            auto result = std::make_shared<Render::ImageAsset>();
            result->mName = source.getFileName();
            result->mFormat = !decode && compressed ? *compressed
                : floating                          ? Render::TextureFormat::RGBA32F
                                                    : Render::TextureFormat::RGBA8;
            result->mSrgb = srgb(source.getInternalTextureFormat());
            auto width = static_cast<std::uint32_t>(source.s());
            auto height = static_cast<std::uint32_t>(source.t());
            result->mLevels.reserve(source.getNumMipmapLevels());
            for (unsigned level = 0; level < source.getNumMipmapLevels(); ++level)
            {
                Render::ImageLevel mip{ width, height, {} };
                const auto* data = source.getMipmapData(level);
                if (!data)
                    throw std::invalid_argument("Model texture has a missing mip level");
                if (compressed && !decode)
                {
                    const auto count = Render::textureLevelSize(*compressed, width, height);
                    const auto offset = source.getMipmapOffset(level);
                    if (offset > source.getTotalSizeInBytesIncludingMipmaps()
                        || count > source.getTotalSizeInBytesIncludingMipmaps() - offset)
                        throw std::invalid_argument("Compressed model texture mip exceeds its data buffer");
                    mip.mBytes.resize(count);
                    std::memcpy(mip.mBytes.data(), data, count);
                }
                else
                {
                    osg::ref_ptr<osg::Image> view = new osg::Image;
                    GLenum pixelFormat = source.getPixelFormat();
                    if (compressed)
                        switch (*compressed)
                        {
                            case Render::TextureFormat::BC1RGB:
                                pixelFormat = GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
                                break;
                            case Render::TextureFormat::BC1RGBA:
                                pixelFormat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
                                break;
                            case Render::TextureFormat::BC2:
                                pixelFormat = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
                                break;
                            case Render::TextureFormat::BC3:
                                pixelFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
                                break;
                            default:
                                throw std::invalid_argument("Model texture has no CPU block decoder");
                        }
                    view->setImage(width, height, 1, source.getInternalTextureFormat(), pixelFormat,
                        source.getDataType(), const_cast<unsigned char*>(data), osg::Image::NO_DELETE,
                        source.getPacking());
                    if (level == 0)
                        view->setRowLength(source.getRowLength());
                    mip.mBytes.resize(Render::textureLevelSize(result->mFormat, width, height));
                    for (unsigned y = 0; y < height; ++y)
                    {
                        const unsigned sourceY
                            = preserveRows || source.getOrigin() == osg::Image::TOP_LEFT ? y : height - y - 1;
                        if (scalarConversion)
                        {
                            auto* output = mip.mBytes.data() + std::size_t(y) * width * (floating ? 16 : 4);
                            for (unsigned x = 0; x < width; ++x)
                            {
                                const auto value = view->getColor(x, sourceY);
                                if (floating)
                                    std::memcpy(output + std::size_t(x) * 16, value.ptr(), 16);
                                else
                                    for (unsigned channel = 0; channel < 4; ++channel)
                                        output[x * 4 + channel] = std::byte(static_cast<unsigned char>(
                                            std::clamp(value[channel], 0.f, 1.f) * 255.f + .5f));
                            }
                        }
                        else if (!compressed)
                        {
                            const auto* row = view->data(0, sourceY);
                            auto* output = mip.mBytes.data() + std::size_t(y) * width * 4;
                            if (pixelFormat == GL_RGBA)
                                std::memcpy(output, row, std::size_t(width) * 4);
                            else
                                for (unsigned x = 0; x < width; ++x, output += 4)
                                {
                                    unsigned char r = 255, g = 255, b = 255, a = 255;
                                    switch (pixelFormat)
                                    {
                                        case GL_RGB:
                                        case GL_BGR:
                                            r = row[x * 3 + (pixelFormat == GL_RGB ? 0 : 2)];
                                            g = row[x * 3 + 1];
                                            b = row[x * 3 + (pixelFormat == GL_RGB ? 2 : 0)];
                                            break;
                                        case GL_BGRA:
                                            r = row[x * 4 + 2];
                                            g = row[x * 4 + 1];
                                            b = row[x * 4];
                                            a = row[x * 4 + 3];
                                            break;
                                        case GL_RED:
                                            r = row[x];
                                            g = b = 0;
                                            break;
                                        case GL_RG:
                                            r = row[x * 2];
                                            g = row[x * 2 + 1];
                                            b = 0;
                                            break;
                                        case GL_LUMINANCE:
                                            r = g = b = row[x];
                                            break;
                                        case GL_LUMINANCE_ALPHA:
                                            r = g = b = row[x * 2];
                                            a = row[x * 2 + 1];
                                            break;
                                        case GL_ALPHA:
                                            a = row[x];
                                            break;
                                    }
                                    output[0] = std::byte{ r };
                                    output[1] = std::byte{ g };
                                    output[2] = std::byte{ b };
                                    output[3] = std::byte{ a };
                                }
                        }
                        else
                            for (unsigned x = 0; x < width; ++x)
                            {
                                const auto value = view->getColor(x, sourceY);
                                for (unsigned channel = 0; channel < 4; ++channel)
                                    mip.mBytes[(std::size_t(y) * width + x) * 4 + channel]
                                        = std::byte{ byteColor(value[channel]) };
                            }
                    }
                }
                result->mLevels.push_back(std::move(mip));
                width = std::max(1u, width / 2);
                height = std::max(1u, height / 2);
            }
            result->mGenerateMipmaps = !Render::compressedTexture(result->mFormat) && result->mLevels.size() == 1;
            if (cpuBlockDecode && !decode)
            {
                osg::ref_ptr<const osg::Image> retained = &source;
                result->mDecode = [retained, preserveRows] { return image(*retained, true, preserveRows); };
            }
            return result;
        }

        Render::TextureWrap wrap(osg::Texture::WrapMode mode)
        {
            switch (mode)
            {
                case osg::Texture::REPEAT:
                    return Render::TextureWrap::Repeat;
                case osg::Texture::MIRROR:
                    return Render::TextureWrap::Mirror;
                case osg::Texture::CLAMP_TO_EDGE:
                    return Render::TextureWrap::Clamp;
                case osg::Texture::CLAMP_TO_BORDER:
                    return Render::TextureWrap::Border;
                default:
                    throw std::invalid_argument("Model texture uses unsupported wrapping semantics");
            }
        }

        Render::SamplerState sampler(const osg::Texture& texture)
        {
            Render::SamplerState result;
            result.mWrapS = wrap(texture.getWrap(osg::Texture::WRAP_S));
            result.mWrapT = wrap(texture.getWrap(osg::Texture::WRAP_T));
            // OSG defaults the unused R axis of Texture2D to legacy CLAMP.
            // Importing it rejects otherwise valid UI/loading textures, and
            // also puts irrelevant state into the 2D sampler cache key.
            if (texture.getTextureTarget() == GL_TEXTURE_3D)
            {
                const auto r = texture.getWrap(osg::Texture::WRAP_R);
                result.mWrapR = r == osg::Texture::CLAMP ? Render::TextureWrap::Clamp : wrap(r);
            }
            result.mMagFilter = texture.getFilter(osg::Texture::MAG_FILTER) == osg::Texture::NEAREST
                ? Render::TextureFilter::Nearest
                : Render::TextureFilter::Linear;
            const auto min = texture.getFilter(osg::Texture::MIN_FILTER);
            if (min != osg::Texture::NEAREST && min != osg::Texture::LINEAR
                && min != osg::Texture::NEAREST_MIPMAP_NEAREST && min != osg::Texture::NEAREST_MIPMAP_LINEAR
                && min != osg::Texture::LINEAR_MIPMAP_NEAREST && min != osg::Texture::LINEAR_MIPMAP_LINEAR)
                throw std::invalid_argument("Model texture has an invalid minification filter");
            if (texture.getFilter(osg::Texture::MAG_FILTER) != osg::Texture::NEAREST
                && texture.getFilter(osg::Texture::MAG_FILTER) != osg::Texture::LINEAR)
                throw std::invalid_argument("Model texture has an invalid magnification filter");
            result.mMinFilter = min == osg::Texture::NEAREST || min == osg::Texture::NEAREST_MIPMAP_NEAREST
                    || min == osg::Texture::NEAREST_MIPMAP_LINEAR
                ? Render::TextureFilter::Nearest
                : Render::TextureFilter::Linear;
            result.mMipFilter
                = min == osg::Texture::LINEAR_MIPMAP_NEAREST || min == osg::Texture::NEAREST_MIPMAP_NEAREST
                ? Render::TextureFilter::Nearest
                : Render::TextureFilter::Linear;
            result.mUseMipmaps = min != osg::Texture::NEAREST && min != osg::Texture::LINEAR;
            if (!std::isfinite(texture.getMaxAnisotropy()))
                throw std::invalid_argument("Model texture anisotropy is not finite");
            result.mAnisotropy = std::max(1.f, texture.getMaxAnisotropy());
            const auto border = texture.getBorderColor();
            if (border == osg::Vec4f(0, 0, 0, 0))
                result.mBorderColor = Render::BorderColor::TransparentBlack;
            else if (border == osg::Vec4f(0, 0, 0, 1))
                result.mBorderColor = Render::BorderColor::Black;
            else if (border == osg::Vec4f(1, 1, 1, 1))
                result.mBorderColor = Render::BorderColor::White;
            else if (result.mWrapS == Render::TextureWrap::Border || result.mWrapT == Render::TextureWrap::Border)
                throw std::invalid_argument("Custom model texture border colors require Vulkan extension support");
            return result;
        }
    }

    class OsgImporter::Impl
    {
    public:
        explicit Impl(std::uint32_t mask)
            : mMask(mask)
        {
        }
        struct ImageEntry
        {
            osg::observer_ptr<const osg::Image> mSource;
            unsigned mRevision;
            std::weak_ptr<const Render::ImageAsset> mValue;
        };
        std::shared_ptr<const Render::ImageAsset> importImage(const osg::Image& source, bool preserveRows)
        {
            auto& images = mImages[preserveRows ? 1 : 0];
            const auto existing = [&]() -> std::shared_ptr<const Render::ImageAsset> {
                if (const auto found = images.find(&source); found != images.end())
                    if (found->second.mSource.get() == &source && found->second.mRevision == source.getModifiedCount())
                        return found->second.mValue.lock();
                return {};
            };
            {
                std::lock_guard lock(mMutex);
                if (auto value = existing())
                    return value;
            }
            auto value = image(source, false, preserveRows);
            std::lock_guard lock(mMutex);
            if (auto winner = existing())
                return winner;
            if (images.size() >= 4096)
            {
                // A full, live mod cache must not serialize every worker behind
                // a whole-table scan. Advance a bounded cursor under the lock.
                unsigned visited = 0;
                mImageSweeps[preserveRows ? 1 : 0].sweep(
                    images,
                    [](const auto& entry) { return !entry.second.mSource.valid() || entry.second.mValue.expired(); },
                    [&] { return visited++ >= 16; });
            }
            if (images.size() < 4096 || images.contains(&source))
                images[&source] = { &source, source.getModifiedCount(), value };
            return value;
        }
        std::uint32_t mMask;
        std::mutex mMutex;
        using Images = std::unordered_map<const osg::Image*, ImageEntry>;
        std::array<Images, 2> mImages;
        std::array<Render::CacheSweep<Images>, 2> mImageSweeps;
    };

    OsgImporter::OsgImporter(std::uint32_t mask)
        : mImpl(std::make_unique<Impl>(mask))
    {
    }
    OsgImporter::~OsgImporter() = default;
    std::shared_ptr<const Render::ImageAsset> OsgImporter::importImage(const osg::Image& source, bool preserveRows)
    {
        return mImpl->importImage(source, preserveRows);
    }

    bool OsgImporter::canImportImageInParallel(const osg::Image& source)
    {
        // Only builtin immutable images: stream/custom const accessors
        // may update private caches or pixels. The owner retains those paths.
        return typeid(source) == typeid(osg::Image) && source.getDataVariance() != osg::Object::DYNAMIC
            && (source.getDataType() == GL_UNSIGNED_BYTE || source.getDataType() == GL_FLOAT) && !source.isCompressed()
            && pixelLayout(source.getPixelFormat()).has_value();
    }

    std::optional<Render::TextureData> OsgImporter::borrowTexture(
        const osg::Image& source, std::vector<Render::TextureLevel>& levels, bool mipmaps)
    {
        levels.clear();
        if (source.s() <= 0 || source.t() <= 0 || source.r() != 1 || !source.data())
            throw std::invalid_argument("Live texture must contain a nonempty 2D image");
        const auto compressed = compression(source.getPixelFormat());
        auto layout = compressed ? compressed : pixelLayout(source.getPixelFormat());
        if (!compressed && source.getDataType() == GL_FLOAT)
        {
            // Vulkan has no sRGB float format. Keep that source's encoded
            // values through the checked RGBA8 conversion instead.
            if (srgb(source.getInternalTextureFormat()))
                return {};
            layout
                = source.getPixelFormat() == GL_RGBA ? std::optional{ Render::TextureFormat::RGBA32F } : std::nullopt;
        }
        if (!layout
            || (!compressed
                && (source.isCompressed()
                    || (source.getDataType() != GL_UNSIGNED_BYTE && source.getDataType() != GL_FLOAT)
                    || !source.isDataContiguous())))
            return {};
        auto width = static_cast<std::uint32_t>(source.s());
        auto height = static_cast<std::uint32_t>(source.t());
        const auto format = *layout;
        const auto count = mipmaps ? source.getNumMipmapLevels() : 1u;
        const auto available = source.getTotalSizeInBytesIncludingMipmaps();
        for (unsigned level = 0; level < count; ++level)
        {
            // Vulkan's tightly packed copies cannot consume padded source rows.
            if (!compressed
                && osg::Image::computeRowWidthInBytes(
                       width, source.getPixelFormat(), source.getDataType(), source.getPacking())
                    != Render::textureLevelSize(format, width, 1))
            {
                levels.clear();
                return {};
            }
            const auto offset = source.getMipmapOffset(level);
            const auto size = Render::textureLevelSize(format, width, height);
            if (offset > available || size > available - offset)
                throw std::invalid_argument("Live texture mip exceeds its source image buffer");
            levels.push_back({ width, height,
                { reinterpret_cast<const std::byte*>(source.data()) + offset, static_cast<std::size_t>(size) } });
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
        return Render::TextureData{ format, srgb(source.getInternalTextureFormat()),
            mipmaps && !compressed && count == 1, levels };
    }

    std::shared_ptr<const Render::ModelAsset> OsgImporter::import(const osg::Node& root, std::string_view name)
    {
        auto result = std::make_shared<Render::ModelAsset>();
        result->mName = name;
        using MeshKey = std::tuple<std::uintptr_t, unsigned, Render::Matrix>;
        std::map<MeshKey, std::shared_ptr<const Render::MeshAsset>> meshes;
        using TextureKey = std::pair<std::uintptr_t, Render::SamplerState>;
        std::map<TextureKey, std::shared_ptr<const Render::TextureAsset>> textures;
        std::set<const osg::Node*> path;
        bool boundsSet = false;
        const auto visit = [&](auto&& self, const osg::Node& node, const osg::Matrixd& parent,
                               const osg::StateSet& inherited) -> void {
            if (!(node.getNodeMask() & mImpl->mMask))
                return;
            if (path.size() >= 256 || !path.insert(&node).second)
                throw std::invalid_argument("Model hierarchy is cyclic or too deep");
            if (node.getUpdateCallback() || node.getEventCallback() || node.getCullCallback())
                throw std::invalid_argument("Animated model nodes require the Vulkan animation path");
            if (dynamic_cast<const osg::LOD*>(&node) || dynamic_cast<const osg::Sequence*>(&node)
                || dynamic_cast<const osg::AutoTransform*>(&node) || dynamic_cast<const NifOsg::AutoTransform*>(&node)
                || dynamic_cast<const osg::Billboard*>(&node) || dynamic_cast<const osg::Camera*>(&node))
                throw std::invalid_argument(
                    "View-dependent or sequenced model nodes require their native rendering service");
            osg::ref_ptr<osg::StateSet> state = new osg::StateSet(inherited, osg::CopyOp::SHALLOW_COPY);
            if (const auto* local = node.getStateSet())
            {
                if (local->getUpdateCallback() || local->getEventCallback())
                    throw std::invalid_argument("Animated model state requires the Vulkan animation path");
                state->merge(*local);
            }
            if (state->getAttribute(osg::StateAttribute::PROGRAM))
                throw std::invalid_argument("Import raw model state before OpenGL shader rewriting");
            osg::Matrixd world = parent;
            if (const auto* transform = node.asTransform())
            {
                if (transform->getReferenceFrame() != osg::Transform::RELATIVE_RF)
                    throw std::invalid_argument("Absolute model transforms require a separate render view");
                transform->computeLocalToWorldMatrix(world, nullptr);
            }
            if (const auto* drawable = node.asDrawable())
            {
                const auto* geometry = drawable->asGeometry();
                if (!geometry || std::string_view(geometry->className()) != "Geometry"
                    || geometry->getDataVariance() == osg::Object::DYNAMIC)
                    throw std::invalid_argument("Nonstatic model drawables require their native geometry service");
                Render::ModelPart part;
                for (std::size_t i = 0; i < part.mModelFromLocal.size(); ++i)
                    part.mModelFromLocal[i] = static_cast<float>(world.ptr()[i]);
                Render::validateTransform(part.mModelFromLocal);
                auto& material = part.mMaterial;
                if (const auto* source
                    = dynamic_cast<const SceneUtil::Material*>(state->getAttribute(osg::StateAttribute::MATERIAL)))
                {
                    material.mTint = color(source->getDiffuse());
                    material.mAmbient = color(source->getAmbient());
                    material.mSpecular = color(source->getSpecular());
                    material.mEmission = color(source->getEmission());
                    material.mShininess = source->getShininess();
                    const auto mode = source->getVertexColorMode();
                    material.mVertexColorMode = static_cast<std::uint32_t>(mode);
                    material.mUseVertexColor = mode != SceneUtil::VertexColorModes::None;
                }
                else if (const auto* osgMaterial
                    = dynamic_cast<const osg::Material*>(state->getAttribute(osg::StateAttribute::MATERIAL)))
                {
                    material.mTint = color(osgMaterial->getDiffuse(osg::Material::FRONT));
                    material.mAmbient = color(osgMaterial->getAmbient(osg::Material::FRONT));
                    material.mSpecular = color(osgMaterial->getSpecular(osg::Material::FRONT));
                    material.mEmission = color(osgMaterial->getEmission(osg::Material::FRONT));
                    material.mShininess = osgMaterial->getShininess(osg::Material::FRONT);
                    const auto mode = osgMaterial->getColorMode();
                    switch (mode)
                    {
                        case osg::Material::EMISSION:
                            material.mVertexColorMode = 1;
                            break;
                        case osg::Material::AMBIENT_AND_DIFFUSE:
                            material.mVertexColorMode = 2;
                            break;
                        case osg::Material::AMBIENT:
                            material.mVertexColorMode = 3;
                            break;
                        case osg::Material::DIFFUSE:
                            material.mVertexColorMode = 4;
                            break;
                        case osg::Material::SPECULAR:
                            material.mVertexColorMode = 5;
                            break;
                        default:
                            material.mVertexColorMode = 0;
                            break;
                    }
                    material.mUseVertexColor = mode != osg::Material::OFF;
                }
                if (state->getMode(GL_BLEND) & osg::StateAttribute::ON)
                {
                    const auto* blend
                        = dynamic_cast<const osg::BlendFunc*>(state->getAttribute(osg::StateAttribute::BLENDFUNC));
                    if (!blend || blend->getSourceRGB() != GL_SRC_ALPHA
                        || (blend->getDestinationRGB() != GL_ONE_MINUS_SRC_ALPHA
                            && blend->getDestinationRGB() != GL_ONE)
                        || blend->getSourceAlpha() != blend->getSourceRGB()
                        || blend->getDestinationAlpha() != blend->getDestinationRGB())
                        throw std::invalid_argument(
                            "Model blend factors require an additional native material pipeline");
                    if (const auto* equation = dynamic_cast<const osg::BlendEquation*>(
                            state->getAttribute(osg::StateAttribute::BLENDEQUATION));
                        equation
                        && (equation->getEquationRGB() != osg::BlendEquation::FUNC_ADD
                            || equation->getEquationAlpha() != osg::BlendEquation::FUNC_ADD))
                        throw std::invalid_argument(
                            "Model blend equation requires an additional native material pipeline");
                    material.mBlend
                        = blend->getDestinationRGB() == GL_ONE ? Render::BlendMode::Additive : Render::BlendMode::Alpha;
                    material.mSortTransparent = state->getRenderingHint() == osg::StateSet::TRANSPARENT_BIN
                        || state->getBinName() == "SORT_BACK_TO_FRONT" || state->getBinName() == "DepthSortedBin";
                }
                if (state->getMode(GL_ALPHA_TEST) & osg::StateAttribute::ON)
                {
                    if (const auto* alpha
                        = dynamic_cast<const osg::AlphaFunc*>(state->getAttribute(osg::StateAttribute::ALPHAFUNC)))
                    {
                        switch (alpha->getFunction())
                        {
                            case GL_ALWAYS:
                                material.mAlphaCompare = Render::AlphaCompare::Always;
                                break;
                            case GL_NEVER:
                                material.mAlphaCompare = Render::AlphaCompare::Never;
                                break;
                            case GL_LESS:
                                material.mAlphaCompare = Render::AlphaCompare::Less;
                                break;
                            case GL_LEQUAL:
                                material.mAlphaCompare = Render::AlphaCompare::LessEqual;
                                break;
                            case GL_EQUAL:
                                material.mAlphaCompare = Render::AlphaCompare::Equal;
                                break;
                            case GL_NOTEQUAL:
                                material.mAlphaCompare = Render::AlphaCompare::NotEqual;
                                break;
                            case GL_GEQUAL:
                                material.mAlphaCompare = Render::AlphaCompare::GreaterEqual;
                                break;
                            case GL_GREATER:
                                material.mAlphaCompare = Render::AlphaCompare::Greater;
                                break;
                            default:
                                throw std::invalid_argument("Model has an invalid alpha comparison");
                        }
                        material.mAlphaCutoff = std::clamp(alpha->getReferenceValue(), 0.f, 1.f);
                    }
                }
                material.mDoubleSided = !(state->getMode(GL_CULL_FACE) & osg::StateAttribute::ON);
                if (!material.mDoubleSided)
                    if (const auto* cull
                        = dynamic_cast<const osg::CullFace*>(state->getAttribute(osg::StateAttribute::CULLFACE));
                        cull && cull->getMode() != osg::CullFace::BACK)
                        throw std::invalid_argument("Model uses an unsupported culling mode");
                unsigned uvUnit = 0;
                osg::Matrixf uvMatrix;
                for (unsigned unit = 0; unit < state->getTextureAttributeList().size(); ++unit)
                {
                    const auto* sourceTexture = state->getTextureAttribute(unit, osg::StateAttribute::TEXTURE);
                    if (!sourceTexture)
                        continue;
                    const auto* texture = dynamic_cast<const osg::Texture2D*>(sourceTexture);
                    if (!texture || texture->getBorderWidth() || texture->getShadowComparison()
                        || state->getTextureAttribute(unit, osg::StateAttribute::TEXGEN))
                        throw std::invalid_argument("Model texture requires an additional native sampling path");
                    if (const auto* environment = state->getTextureAttribute(unit, osg::StateAttribute::TEXENV))
                    {
                        const auto* modulation = dynamic_cast<const osg::TexEnv*>(environment);
                        if (!modulation || modulation->getMode() != osg::TexEnv::MODULATE)
                            throw std::invalid_argument(
                                "Model texture environment requires an additional native material pipeline");
                    }
                    const auto* type = state->getTextureAttribute(unit, SceneUtil::TextureType::AttributeType);
                    if ((type && type->getName() != "diffuseMap") || material.mDiffuse)
                        throw std::invalid_argument(
                            "Additional model texture stages require their native material pipeline");
                    if (!texture->getImage() || texture->getUpdateCallback())
                        throw std::invalid_argument("Model texture is missing data or requires animation");
                    auto source = importImage(*texture->getImage());
                    const auto sampling = sampler(*texture);
                    const TextureKey key{ reinterpret_cast<std::uintptr_t>(source.get()), sampling };
                    auto& cached = textures[key];
                    if (!cached)
                    {
                        auto value = std::make_shared<Render::TextureAsset>();
                        value->mImage = std::move(source);
                        value->mSampler = sampling;
                        cached = std::move(value);
                    }
                    material.mDiffuse = cached;
                    uvUnit = unit;
                    if (const auto* matrix = dynamic_cast<const osg::TexMat*>(
                            state->getTextureAttribute(unit, osg::StateAttribute::TEXMAT)))
                        uvMatrix = matrix->getMatrix();
                    if (const auto* uniform = state->getUniform("texMat" + std::to_string(unit)))
                        if (!uniform->get(uvMatrix))
                            throw std::invalid_argument("Model texture matrix uniform has an invalid type");
                }
                Render::Matrix uvKey;
                std::copy_n(uvMatrix.ptr(), uvKey.size(), uvKey.begin());
                if (!std::all_of(uvKey.begin(), uvKey.end(), [](float value) { return std::isfinite(value); }))
                    throw std::invalid_argument("Model texture transform is not finite");
                const MeshKey key{ reinterpret_cast<std::uintptr_t>(geometry), uvUnit, uvKey };
                auto& cachedMesh = meshes[key];
                if (!cachedMesh)
                    cachedMesh = mesh(*geometry, uvUnit, uvMatrix);
                part.mMesh = cachedMesh;
                const auto bounds = Render::transformBounds(part.mMesh->mBounds, part.mModelFromLocal);
                if (!boundsSet)
                {
                    result->mBounds = bounds;
                    boundsSet = true;
                }
                else
                    for (std::size_t i = 0; i < 3; ++i)
                    {
                        result->mBounds.mMin[i] = std::min(result->mBounds.mMin[i], bounds.mMin[i]);
                        result->mBounds.mMax[i] = std::max(result->mBounds.mMax[i], bounds.mMax[i]);
                    }
                result->mParts.push_back(std::move(part));
            }
            else if (const auto* group = node.asGroup())
            {
                const auto* enabled = dynamic_cast<const osg::Switch*>(group);
                for (unsigned i = 0; i < group->getNumChildren(); ++i)
                    if (!enabled || enabled->getValue(i))
                        self(self, *group->getChild(i), world, *state);
            }
            path.erase(&node);
        };
        try
        {
            osg::ref_ptr<osg::StateSet> defaults = new osg::StateSet;
            defaults->setMode(GL_CULL_FACE, osg::StateAttribute::ON);
            visit(visit, root, osg::Matrixd{}, *defaults);
            if (result->mParts.empty())
                throw std::invalid_argument("Model contains no visible static triangles");
            Render::validateBounds(result->mBounds);
            return result;
        }
        catch (const std::exception& error)
        {
            throw std::runtime_error("Cannot import render model " + std::string(name) + ": " + error.what());
        }
    }

    std::shared_ptr<const Render::MeshAsset> OsgImporter::importMesh(
        const osg::Geometry& geometry, unsigned uvUnit, const osg::Matrixf& uvTransform)
    {
        return mesh(geometry, uvUnit, uvTransform);
    }

    bool OsgImporter::canImportInParallel(const osg::Geometry& geometry, unsigned uvUnit)
    {
        for (const auto* array : { geometry.getVertexArray(), geometry.getNormalArray(), geometry.getColorArray(),
                 geometry.getTexCoordArray(uvUnit), geometry.getTexCoordArray(0) })
        {
            if (!array)
                continue;
            const auto& type = typeid(*array);
            if (type != typeid(osg::Vec2Array) && type != typeid(osg::Vec3Array) && type != typeid(osg::Vec4Array)
                && type != typeid(osg::Vec4ubArray) && type != typeid(osg::Vec2dArray)
                && type != typeid(osg::Vec3dArray) && type != typeid(osg::Vec4dArray)
                && type != typeid(osg::Vec4usArray))
                return false;
        }
        for (const auto& primitive : geometry.getPrimitiveSetList())
        {
            if (!primitive)
                return false;
            const auto& type = typeid(*primitive);
            if (type != typeid(osg::DrawArrays) && type != typeid(osg::DrawArrayLengths)
                && type != typeid(osg::MultiDrawArrays) && type != typeid(osg::DrawElementsUByte)
                && type != typeid(osg::DrawElementsUShort) && type != typeid(osg::DrawElementsUInt))
                return false;
        }
        return true;
    }

    MeshBuffers OsgImporter::importMeshBuffers(const osg::Geometry& geometry, unsigned uvUnit)
    {
        auto imported = mesh(geometry, uvUnit, osg::Matrixf{}, nullptr, false);
        return { std::move(imported->mVertices), std::move(imported->mIndices) };
    }

    MeshStream OsgImporter::importMeshStream(const osg::Geometry& geometry, unsigned uvUnit)
    {
        MeshStream result;
        auto imported = mesh(geometry, uvUnit, osg::Matrixf{}, &result.mVertexSources);
        result.mVertices = std::move(imported->mVertices);
        result.mIndices = std::move(imported->mIndices);
        result.mSourceVertexCount = geometry.getVertexArray()->getNumElements();
        result.mUVUnit = uvUnit;
        const std::array channels{ geometry.getTexCoordArray(uvUnit), geometry.getColorArray(),
            geometry.getTexCoordArray(0) };
        for (unsigned i = 0; i < channels.size(); ++i)
        {
            result.mStaticChannels[i] = channels[i];
            result.mStaticRevisions[i] = channels[i] ? channels[i]->getModifiedCount() : 0;
        }
        return result;
    }

    void OsgImporter::updateMeshStream(const osg::Geometry& geometry, MeshStream& stream)
    {
        const auto* positions = geometry.getVertexArray();
        const auto* uv = geometry.getTexCoordArray(stream.mUVUnit);
        if (!positions || positions->getDataSize() != 3 || positions->getNumElements() != stream.mSourceVertexCount
            || stream.mVertices.size() != stream.mVertexSources.size() || (uv && uv->getDataSize() != 2))
            throw std::invalid_argument("Animated mesh stream requires its original topology and channel dimensions");
        const osg::Matrixf identity;
        const std::array channels{ uv, geometry.getColorArray(), geometry.getTexCoordArray(0) };
        bool staticChannelsUnchanged = true;
        for (unsigned i = 0; i < channels.size(); ++i)
            staticChannelsUnchanged &= stream.mStaticChannels[i].get() == channels[i]
                && stream.mStaticRevisions[i] == (channels[i] ? channels[i]->getModifiedCount() : 0);
        const auto* packedPositions = dynamic_cast<const osg::Vec3Array*>(positions);
        const auto* normals = geometry.getNormalArray();
        const auto* packedNormals = dynamic_cast<const osg::Vec3Array*>(normals);
        const auto normalBinding = normals ? normals->getBinding() : osg::Array::BIND_OFF;
        const bool hasNormals = normalBinding != osg::Array::BIND_OFF;
        if (staticChannelsUnchanged && packedPositions && (!hasNormals || packedNormals))
        {
            // Skinning changes pose, not UV/color channels. Keep those packed
            // bytes and topology in place; read float pose arrays directly.
            for (std::size_t i = 0; i < stream.mVertices.size(); ++i)
            {
                const auto source = stream.mVertexSources[i];
                const auto& p = (*packedPositions)[static_cast<unsigned>(source)];
                if (!std::isfinite(p.x()) || !std::isfinite(p.y()) || !std::isfinite(p.z()))
                    throw std::invalid_argument("Model contains a nonfinite vertex channel");
                auto& vertex = stream.mVertices[i];
                vertex.mPosition = { p.x(), p.y(), p.z() };
                vertex.mNormal = { 0, 0, 1 };
                if (hasNormals)
                {
                    const auto index = channelIndex(
                        normalBinding, static_cast<unsigned>(source), static_cast<unsigned>(source >> 32));
                    if (index >= packedNormals->size())
                        throw std::invalid_argument("Animated normal channel is too short");
                    const auto& n = (*packedNormals)[index];
                    const double length = double(n.x()) * n.x() + double(n.y()) * n.y() + double(n.z()) * n.z();
                    if (std::isfinite(length) && length > 1e-20)
                    {
                        const auto scale = 1.0 / std::sqrt(length);
                        vertex.mNormal = { static_cast<float>(n.x() * scale), static_cast<float>(n.y() * scale),
                            static_cast<float>(n.z() * scale) };
                    }
                }
            }
            return;
        }
        VertexReader reader(geometry, stream.mUVUnit, identity);
        for (std::size_t i = 0; i < stream.mVertices.size(); ++i)
        {
            const auto source = stream.mVertexSources[i];
            stream.mVertices[i] = reader.read(static_cast<unsigned>(source), static_cast<unsigned>(source >> 32));
        }
        for (unsigned i = 0; i < channels.size(); ++i)
        {
            stream.mStaticChannels[i] = channels[i];
            stream.mStaticRevisions[i] = channels[i] ? channels[i]->getModifiedCount() : 0;
        }
    }

    Render::SamplerState OsgImporter::importSampler(const osg::Texture& texture)
    {
        return sampler(texture);
    }

    void OsgImporter::clear()
    {
        std::lock_guard lock(mImpl->mMutex);
        for (auto& images : mImpl->mImages)
            images.clear();
        mImpl->mImageSweeps = {};
    }
}
