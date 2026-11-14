#include "osgstate.hpp"

#include <components/sceneutil/texturetype.hpp>

#include <osg/StateSet>
#include <osg/TexMat>
#include <osg/Texture2D>
#include <osg/Uniform>

#include <string>

namespace RenderAssets
{
    namespace
    {
        template <class Target, class Source, class Flags>
        void mergeBindings(Target& target, const Source& source, Flags flags)
        {
            for (const auto& [key, value] : source)
            {
                const auto found = target.find(key);
                if (found == target.end() || !(flags(found->second) & osg::StateAttribute::OVERRIDE)
                    || (flags(value) & osg::StateAttribute::PROTECTED))
                    target.insert_or_assign(key, value);
            }
        }
    }

    std::array<float, 4> depthCoefficients(const osg::Matrixd& p, bool zeroToOne)
    {
        return { static_cast<float>(zeroToOne ? p(2, 2) : .5 * (p(2, 2) + p(2, 3))),
            static_cast<float>(zeroToOne ? p(3, 2) : .5 * (p(3, 2) + p(3, 3))), static_cast<float>(p(2, 3)),
            static_cast<float>(p(3, 3)) };
    }

    OsgState::OsgState(const osg::StateSet& source)
        : mModes(source.getModeList())
        , mAttributes(source.getAttributeList())
        , mTextureModes(source.getTextureModeList())
        , mTextures(source.getTextureAttributeList())
        , mUniforms(source.getUniformList().begin(), source.getUniformList().end())
    {
    }

    bool OsgState::matches(const osg::StateSet& source) const
    {
        return mModes == source.getModeList() && mAttributes == source.getAttributeList()
            && mTextureModes == source.getTextureModeList() && mTextures == source.getTextureAttributeList()
            && mUniforms.size() == source.getUniformList().size()
            && std::equal(mUniforms.begin(), mUniforms.end(), source.getUniformList().begin(),
                [](const auto& a, const auto& b) { return a.first == b.first && a.second == b.second; });
    }

    void OsgState::merge(const osg::StateSet& source)
    {
        const auto modeFlags = [](auto value) { return value; };
        const auto pairFlags = [](const auto& value) { return value.second; };
        mergeBindings(mModes, source.getModeList(), modeFlags);
        mergeBindings(mAttributes, source.getAttributeList(), pairFlags);
        mergeBindings(mUniforms, source.getUniformList(), pairFlags);
        mTextures.resize(std::max(mTextures.size(), source.getTextureAttributeList().size()));
        for (unsigned unit = 0; unit < source.getTextureAttributeList().size(); ++unit)
            mergeBindings(mTextures[unit], source.getTextureAttributeList()[unit], pairFlags);
        mTextureModes.resize(std::max(mTextureModes.size(), source.getTextureModeList().size()));
        for (unsigned unit = 0; unit < source.getTextureModeList().size(); ++unit)
            mergeBindings(mTextureModes[unit], source.getTextureModeList()[unit], modeFlags);
    }

    const osg::Uniform* OsgState::getUniform(std::string_view name) const
    {
        const auto found = mUniforms.find(name);
        return found == mUniforms.end() ? nullptr : found->second.first.get();
    }
    const osg::StateAttribute* OsgState::getAttribute(osg::StateAttribute::Type type, unsigned member) const
    {
        const auto found = mAttributes.find({ type, member });
        return found == mAttributes.end() ? nullptr : found->second.first.get();
    }
    const osg::StateAttribute* OsgState::getTextureAttribute(unsigned unit, osg::StateAttribute::Type type) const
    {
        if (unit >= mTextures.size())
            return nullptr;
        const auto found = mTextures[unit].find({ type, 0 });
        return found == mTextures[unit].end() ? nullptr : found->second.first.get();
    }
    osg::StateAttribute::GLModeValue OsgState::getMode(osg::StateAttribute::GLMode mode) const
    {
        const auto found = mModes.find(mode);
        return found == mModes.end() ? static_cast<osg::StateAttribute::GLModeValue>(osg::StateAttribute::INHERIT)
                                     : found->second;
    }

    template <class State>
    std::optional<unsigned> textureUnitImpl(const State& state, std::string_view name)
    {
        if (const auto* uniform = state.getUniform(std::string(name)))
        {
            int unit = -1;
            if (uniform->get(unit) && unit >= 0)
                return static_cast<unsigned>(unit);
        }
        for (unsigned unit = 0; unit < state.getTextureAttributeList().size(); ++unit)
            if (const auto* type = state.getTextureAttribute(unit, SceneUtil::TextureType::AttributeType);
                type && type->getName() == name)
                return unit;
        // Unlabelled fixed-function assets conventionally use unit zero.
        // A named normal/blend/reflection sampler prevents that fallback.
        if (name == "diffuseMap" && !state.getUniform("normalMap") && !state.getUniform("reflectionMap")
            && state.getTextureAttribute(0, osg::StateAttribute::TEXTURE))
            return 0;
        return {};
    }

    template <class State>
    std::array<float, 6> textureTransformImpl(const State& state, unsigned unit)
    {
        osg::Matrixf matrix;
        if (const auto* uniform = state.getUniform("texMat" + std::to_string(unit)))
            uniform->get(matrix);
        if (const auto* attribute
            = dynamic_cast<const osg::TexMat*>(state.getTextureAttribute(unit, osg::StateAttribute::TEXMAT)))
            matrix = attribute->getMatrix();
        return { matrix(0, 0), matrix(1, 0), matrix(3, 0), matrix(0, 1), matrix(1, 1), matrix(3, 1) };
    }

    template <class State>
    osg::Matrixd clipProjectionImpl(const State& state, const osg::Matrixd& cullProjection, bool reversedShaderDepth)
    {
        osg::Matrixf shaderProjection;
        const auto* uniform = state.getUniform("projectionMatrix");
        const bool authored = uniform && uniform->get(shaderProjection);
        osg::Matrixd depth;
        if (authored && reversedShaderDepth)
        {
            depth(2, 2) = -1;
            depth(3, 2) = 1;
        }
        else
        {
            depth(2, 2) = .5;
            depth(3, 2) = .5;
        }
        return (authored ? osg::Matrixd(shaderProjection) : cullProjection) * depth;
    }
    std::optional<unsigned> textureUnit(const osg::StateSet& state, std::string_view name)
    {
        return textureUnitImpl(state, name);
    }
    std::optional<unsigned> textureUnit(const OsgState& state, std::string_view name)
    {
        return textureUnitImpl(state, name);
    }
    std::array<float, 6> textureTransform(const osg::StateSet& state, unsigned unit)
    {
        return textureTransformImpl(state, unit);
    }
    std::array<float, 6> textureTransform(const OsgState& state, unsigned unit)
    {
        return textureTransformImpl(state, unit);
    }
    osg::Matrixd clipProjection(const osg::StateSet& state, const osg::Matrixd& projection, bool reversed)
    {
        return clipProjectionImpl(state, projection, reversed);
    }
    osg::Matrixd clipProjection(const OsgState& state, const osg::Matrixd& projection, bool reversed)
    {
        return clipProjectionImpl(state, projection, reversed);
    }

}
