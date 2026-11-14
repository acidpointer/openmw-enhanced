#include "osgimporter.hpp"

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

        unsigned channelIndex(const osg::Array& channel, unsigned vertex, unsigned primitiveSet)
        {
            switch (channel.getBinding())
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

        Render::Vertex streamVertex(const osg::Geometry& geometry, unsigned uvUnit, const osg::Matrixf& uvMatrix,
            bool identityUV, unsigned source, unsigned set)
        {
            const auto* positions = geometry.getVertexArray();
            const auto* uv = geometry.getTexCoordArray(uvUnit);
            const auto* colors = geometry.getColorArray();
            const auto* normals = geometry.getNormalArray();
            Render::Vertex vertex{};
            const auto position = readArray(*positions, source);
            std::copy_n(position.begin(), 3, vertex.mPosition.begin());
            if (uv)
            {
                const auto coord = readArray(*uv, source);
                const osg::Vec4f transformed = identityUV ? osg::Vec4f(coord[0], coord[1], 0, 1)
                                                          : osg::Vec4f(coord[0], coord[1], 0, 1) * uvMatrix;
                if (!std::isfinite(transformed.x()) || !std::isfinite(transformed.y()) || transformed.w() != 1)
                    throw std::invalid_argument("Unsupported projective model texture transform");
                vertex.mUV = { transformed.x(), transformed.y() };
            }
            const auto* maskUV = geometry.getTexCoordArray(0);
            if (maskUV)
            {
                const auto coord = readArray(*maskUV, source);
                vertex.mMaskUV = { coord[0], coord[1] };
            }
            if (colors && colors->getBinding() != osg::Array::BIND_OFF)
            {
                const auto value = readArray(*colors, channelIndex(*colors, source, set), true);
                for (std::size_t i = 0; i < 4; ++i)
                    vertex.mColor[i] = byteColor(value[i]);
                vertex.mAlpha = std::clamp(value[3], 0.f, 1.f);
            }
            std::array<float, 3> normal{ 0, 0, 1 };
            if (normals && normals->getBinding() != osg::Array::BIND_OFF)
            {
                const auto value = readArray(*normals, channelIndex(*normals, source, set), false, false);
                if (std::all_of(value.begin(), value.begin() + 3, [](float v) { return std::isfinite(v); }))
                {
                    const double length
                        = double(value[0]) * value[0] + double(value[1]) * value[1] + double(value[2]) * value[2];
                    if (length > 1e-20 && std::isfinite(length))
                        for (unsigned i = 0; i < 3; ++i)
                            normal[i] = value[i] / std::sqrt(length);
                }
            }
            vertex.mNormal = normal;
            return vertex;
        }

        std::shared_ptr<Render::MeshAsset> mesh(const osg::Geometry& geometry, unsigned uvUnit,
            const osg::Matrixf& uvMatrix, std::vector<std::uint64_t>* vertexSources = nullptr)
        {
            const auto* positions = geometry.getVertexArray();
            if (!positions || positions->getDataSize() != 3)
                throw std::invalid_argument("Static model geometry requires three-component positions");
            const auto* uv = geometry.getTexCoordArray(uvUnit);
            if (uv && uv->getDataSize() != 2)
                throw std::invalid_argument("Static model geometry requires two-component texture coordinates");
            const auto* colors = geometry.getColorArray();
            const auto* normals = geometry.getNormalArray();
            auto result = std::make_shared<Render::MeshAsset>();
            // Reuse source vertices whenever channel bindings permit. A
            // per-primitive-set channel gets a separate key for that set.
            std::unordered_map<std::uint64_t, std::uint32_t> vertices;
            vertices.reserve(positions->getNumElements());
            result->mVertices.reserve(positions->getNumElements());
            const bool identityUV = uvMatrix.isIdentity();
            if (vertexSources)
                vertexSources->reserve(positions->getNumElements());
            const bool perSet = (colors && colors->getBinding() == osg::Array::BIND_PER_PRIMITIVE_SET)
                || (normals && normals->getBinding() == osg::Array::BIND_PER_PRIMITIVE_SET);
            for (unsigned set = 0; set < geometry.getNumPrimitiveSets(); ++set)
            {
                const auto* primitive = geometry.getPrimitiveSet(set);
                if (!primitive)
                    throw std::invalid_argument("Model contains an absent primitive set");
                const auto emit = [&](unsigned a, unsigned b, unsigned c) {
                    if (a >= positions->getNumElements() || b >= positions->getNumElements()
                        || c >= positions->getNumElements())
                        throw std::invalid_argument("Model triangle index exceeds its vertex data");
                    if (a == b || b == c || a == c)
                        return;
                    for (const unsigned source : { a, b, c })
                    {
                        const auto key = (std::uint64_t{ perSet ? set : 0 } << 32) | source;
                        if (const auto found = vertices.find(key); found != vertices.end())
                        {
                            result->mIndices.push_back(found->second);
                            continue;
                        }
                        if (result->mVertices.size() >= std::numeric_limits<std::uint32_t>::max())
                            throw std::length_error("Imported mesh exceeds 32-bit vertex indexing");
                        const auto vertex = streamVertex(geometry, uvUnit, uvMatrix, identityUV, source, set);
                        const auto id = static_cast<std::uint32_t>(result->mVertices.size());
                        vertices.emplace(key, id);
                        if (vertexSources)
                            vertexSources->push_back((std::uint64_t{ set } << 32) | source);
                        result->mVertices.push_back(vertex);
                        if (normals && normals->getBinding() != osg::Array::BIND_OFF)
                            result->mNormals.push_back(vertex.mNormal);
                        result->mIndices.push_back(id);
                    }
                };
                if (const auto* lengths = dynamic_cast<const osg::DrawArrayLengths*>(primitive))
                {
                    if (lengths->getFirst() < 0)
                        throw std::invalid_argument("Model draw range starts before its vertex array");
                    std::uint64_t start = lengths->getFirst();
                    for (const auto length : *lengths)
                    {
                        if (length < 0 || start + static_cast<unsigned>(length) > positions->getNumElements())
                            throw std::invalid_argument("Model draw range exceeds its vertex array");
                        triangles(
                            primitive->getMode(), length, [&](auto i) { return static_cast<unsigned>(start + i); },
                            emit);
                        start += length;
                    }
                }
                else
                {
                    if (primitive->getNumInstances() > 1 || dynamic_cast<const osg::MultiDrawArrays*>(primitive))
                        throw std::invalid_argument(
                            "Imported model contains unsupported primitive instancing or multi-draw state");
                    triangles(
                        primitive->getMode(), primitive->getNumIndices(),
                        [&](auto i) { return primitive->index(static_cast<unsigned>(i)); }, emit);
                }
            }
            if (result->mVertices.empty())
                throw std::invalid_argument("Imported model geometry has no nondegenerate triangles");
            result->mBounds = { result->mVertices.front().mPosition, result->mVertices.front().mPosition };
            for (const auto& vertex : result->mVertices)
                for (std::size_t i = 0; i < 3; ++i)
                {
                    result->mBounds.mMin[i] = std::min(result->mBounds.mMin[i], vertex.mPosition[i]);
                    result->mBounds.mMax[i] = std::max(result->mBounds.mMax[i], vertex.mPosition[i]);
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
                default:
                    return std::nullopt;
            }
        }

        std::shared_ptr<const Render::ImageAsset> image(const osg::Image& source, bool decode, bool preserveRows)
        {
            if (source.s() <= 0 || source.t() <= 0 || source.r() != 1 || !source.data())
                throw std::invalid_argument("Model texture must contain a nonempty 2D image");
            const auto compressed = compression(source.getPixelFormat());
            if (source.isCompressed() && !compressed)
                throw std::invalid_argument("Model texture uses an unsupported compression format");
            if (decode && compressed
                && (*compressed == Render::TextureFormat::BC4 || *compressed == Render::TextureFormat::BC5))
                throw std::invalid_argument("CPU fallback for RGTC model textures is not implemented");
            if (source.isCompressed() && !preserveRows && source.getOrigin() != osg::Image::TOP_LEFT)
                throw std::invalid_argument(
                    "Compressed model textures must be normalized to top-left origin by the image loader");
            if (!source.isCompressed() && source.getDataType() != GL_UNSIGNED_BYTE)
                throw std::invalid_argument("Model texture requires an unsupported pixel conversion");
            if (!source.isCompressed())
                switch (source.getPixelFormat())
                {
                    case GL_RGB:
                    case GL_RGBA:
                    case GL_BGR:
                    case GL_BGRA:
                    case GL_LUMINANCE:
                    case GL_LUMINANCE_ALPHA:
                    case GL_ALPHA:
                        break;
                    default:
                        throw std::invalid_argument("Model texture has an unsupported channel layout");
                }
            auto result = std::make_shared<Render::ImageAsset>();
            result->mName = source.getFileName();
            result->mFormat = !decode && compressed ? *compressed : Render::TextureFormat::RGBA8;
            switch (source.getInternalTextureFormat())
            {
                case GL_SRGB8:
                case GL_SRGB8_ALPHA8:
                case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
                case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
                case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
                case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
                    result->mSrgb = true;
                    break;
                default:
                    break;
            }
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
                    mip.mBytes.resize(Render::textureLevelSize(Render::TextureFormat::RGBA8, width, height));
                    for (unsigned y = 0; y < height; ++y)
                    {
                        const unsigned sourceY
                            = preserveRows || source.getOrigin() == osg::Image::TOP_LEFT ? y : height - y - 1;
                        if (!compressed)
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
            result->mGenerateMipmaps = result->mFormat == Render::TextureFormat::RGBA8 && result->mLevels.size() == 1;
            if (compressed && !decode && *compressed != Render::TextureFormat::BC4
                && *compressed != Render::TextureFormat::BC5)
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
                std::erase_if(images,
                    [](const auto& entry) { return !entry.second.mSource.valid() || entry.second.mValue.expired(); });
            if (images.size() < 4096 || images.contains(&source))
                images[&source] = { &source, source.getModifiedCount(), value };
            return value;
        }
        std::uint32_t mMask;
        std::mutex mMutex;
        std::array<std::unordered_map<const osg::Image*, ImageEntry>, 2> mImages;
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

    MeshStream OsgImporter::importMeshStream(const osg::Geometry& geometry, unsigned uvUnit)
    {
        MeshStream result;
        auto imported = mesh(geometry, uvUnit, osg::Matrixf{}, &result.mVertexSources);
        result.mVertices = std::move(imported->mVertices);
        result.mIndices = std::move(imported->mIndices);
        result.mSourceVertexCount = geometry.getVertexArray()->getNumElements();
        result.mUVUnit = uvUnit;
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
        for (std::size_t i = 0; i < stream.mVertices.size(); ++i)
        {
            const auto source = stream.mVertexSources[i];
            stream.mVertices[i] = streamVertex(geometry, stream.mUVUnit, identity, true, static_cast<unsigned>(source),
                static_cast<unsigned>(source >> 32));
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
    }
}
