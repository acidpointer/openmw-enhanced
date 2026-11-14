#ifndef OPENMW_COMPONENTS_RENDERASSETS_OSGSTATE_H
#define OPENMW_COMPONENTS_RENDERASSETS_OSGSTATE_H

#include <array>
#include <optional>
#include <string_view>

#include <osg/Matrixd>
#include <osg/StateSet>

namespace osg
{
    class StateSet;
}

namespace RenderAssets
{
    // A CPU snapshot must not become an OSG parent of every inherited uniform
    // and attribute. Thousands of real StateSet clones make removing shared
    // parents quadratic when the native cache retires them.
    class OsgState
    {
    public:
        OsgState() = default;
        explicit OsgState(const osg::StateSet& source);
        bool matches(const osg::StateSet& source) const;
        void merge(const osg::StateSet& source);
        const osg::Uniform* getUniform(std::string_view name) const;
        const osg::StateAttribute* getAttribute(osg::StateAttribute::Type type, unsigned member = 0) const;
        const osg::StateAttribute* getTextureAttribute(unsigned unit, osg::StateAttribute::Type type) const;
        osg::StateAttribute::GLModeValue getMode(osg::StateAttribute::GLMode mode) const;
        const auto& getUniformList() const { return mUniforms; }
        const auto& getModeList() const { return mModes; }
        const auto& getTextureAttributeList() const { return mTextures; }

    private:
        osg::StateSet::ModeList mModes;
        osg::StateSet::AttributeList mAttributes;
        osg::StateSet::TextureModeList mTextureModes;
        osg::StateSet::TextureAttributeList mTextures;
        std::map<std::string, osg::StateSet::RefUniformPair, std::less<>> mUniforms;
    };

    std::array<float, 4> depthCoefficients(const osg::Matrixd& projection, bool zeroToOne);

    std::optional<unsigned> textureUnit(const osg::StateSet& state, std::string_view name);
    std::array<float, 6> textureTransform(const osg::StateSet& state, unsigned unit);
    // OSG's leaf projection is for CPU culling. OpenMW shaders may instead
    // receive a reversed, zero-to-one projection through the effective state.
    // Return a forward zero-to-one projection for the native depth buffer.
    osg::Matrixd clipProjection(
        const osg::StateSet& state, const osg::Matrixd& cullProjection, bool reversedShaderDepth);
    std::optional<unsigned> textureUnit(const OsgState& state, std::string_view name);
    std::array<float, 6> textureTransform(const OsgState& state, unsigned unit);
    osg::Matrixd clipProjection(const OsgState& state, const osg::Matrixd& cullProjection, bool reversedShaderDepth);
}

#endif
