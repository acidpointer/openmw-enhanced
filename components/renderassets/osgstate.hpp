#ifndef OPENMW_COMPONENTS_RENDERASSETS_OSGSTATE_H
#define OPENMW_COMPONENTS_RENDERASSETS_OSGSTATE_H

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <osg/Matrixd>
#include <osg/StateSet>

namespace osg
{
    class StateSet;
}

namespace Render
{
    struct EffectBlend;
}

namespace RenderAssets
{
    // Order matches the fixed, sorted native name table in osgstate.cpp.
    enum class NativeUniform : unsigned
    {
        LightBuffer,
        PointLightCount,
        ActorFade,
        Alpha,
        AlphaRef,
        AtmosphereFade,
        BlendMap,
        DiffuseColor,
        DiffuseMap,
        DistortionStrength,
        EmissiveMap,
        FogColor,
        FogEnd,
        FogStart,
        MaskMap,
        MaterialAmbient,
        MaterialDiffuse,
        MaterialEmission,
        MaterialEmissiveMult,
        MaterialShininess,
        MaterialSpecStrength,
        MaterialSpecular,
        MaterialVertexColorMode,
        MaximumShadowMapDistance,
        MoonBlend,
        NodePosition,
        NormalMap,
        Opacity,
        Pass,
        ProjectionMatrix,
        ReflectionMap,
        ShadowSpaceMatrix0,
        SpecularMap,
        SunAmbient,
        SunDiffuse,
        SunPosition,
        SunSpecular,
        TerrainSpecularAlpha,
        TexMat0,
        TexMat1,
        TexMat2,
        TexMat3,
        TexMat4,
        TexMat5,
        TexMat6,
        TexMat7,
        WaterSurface,
        WorldMatrix,
        Count
    };

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
        const osg::Uniform* getUniform(NativeUniform name) const;
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

    // A frame-local borrowed chain: no binding copies or OSG parent records.
    // Bind after culling each frame: binding addresses are memoized until the
    // next bind, while the Uniform/StateAttribute values are always read live.
    // The source cull graph must remain alive and structurally unchanged.
    class OsgStateView
    {
    public:
        OsgStateView() = default;
        OsgStateView(const OsgStateView* parent, const osg::StateSet* local)
            : mParent(parent)
            , mLocal(local)
        {
        }
        void bind(const OsgStateView* parent, const osg::StateSet* local)
        {
            mParent = parent;
            mLocal = local;
            mUniformSeen = 0;
            mAttributeSeen = 0;
            mModeSeen = 0;
            mNativeStatePrepared = false;
        }
        const OsgStateView* parent() const { return mParent; }
        const osg::StateSet* local() const { return mLocal; }
        const osg::Uniform* getUniform(std::string_view name) const;
        const osg::Uniform* getUniform(NativeUniform name) const;
        // Visit each local binding once instead of resolving every slot up
        // the hierarchy separately. Only the fixed pointer table is copied.
        void prepareNativeUniforms() const;
        // Fold bounded uniform/attribute/mode bindings in one pass per node.
        // Live values are still read from their OSG objects each frame.
        void prepareNativeState() const;
        const osg::StateAttribute* getAttribute(osg::StateAttribute::Type type, unsigned member = 0) const;
        const osg::StateAttribute* getTextureAttribute(unsigned unit, osg::StateAttribute::Type type) const;
        osg::StateAttribute::GLModeValue getMode(osg::StateAttribute::GLMode mode) const;
        unsigned textureUnits() const;
        unsigned nextTextureLabelUnit(unsigned start, unsigned end) const;
        OsgState snapshot() const;

    private:
        const osg::StateSet::RefUniformPair* uniformBinding(unsigned slot) const;
        const osg::StateSet::RefAttributePair* attributeBinding(
            int unit, osg::StateAttribute::Type type, unsigned member) const;
        const osg::StateAttribute::GLModeValue* modeBinding(osg::StateAttribute::GLMode mode) const;
        const OsgStateView* mParent = nullptr;
        const osg::StateSet* mLocal = nullptr;
        // Bounded native queries; arbitrary effect uniforms still use the
        // general resolver. No allocation or full inherited-state copy on bind.
        mutable std::array<const osg::StateSet::RefUniformPair*, static_cast<unsigned>(NativeUniform::Count)>
            mUniformCache{};
        // Ordinary texture/matrix slots 0..7 plus semantic labels through
        // unit 31. Reserved shadow/scene units must not force repeated walks
        // of the inherited chain when a material has no blend/normal map.
        mutable std::array<const osg::StateSet::RefAttributePair*, 57> mAttributeCache{};
        mutable std::array<const osg::StateAttribute::GLModeValue*, 5> mModeCache{};
        mutable std::uint64_t mUniformSeen = 0;
        mutable std::uint64_t mAttributeSeen = 0;
        mutable unsigned mModeSeen = 0;
        mutable unsigned mTextureUnits = 0;
        mutable std::uint32_t mTextureLabels = 0;
        mutable bool mNativeStatePrepared = false;
    };

    const osg::Uniform* nativeUniform(const osg::StateSet& state, NativeUniform name);
    using NativeBindings = std::vector<std::array<std::uintptr_t, 4>>;
    void collectNativeBindings(const osg::StateSet& state, NativeBindings& result);
    inline const osg::Uniform* nativeUniform(const OsgStateView& state, NativeUniform name)
    {
        return state.getUniform(name);
    }
    inline const osg::Uniform* nativeUniform(const OsgState& state, NativeUniform name)
    {
        return state.getUniform(name);
    }

    std::optional<unsigned> textureUnit(const OsgStateView& state, std::string_view name);
    std::array<float, 6> textureTransform(const OsgStateView& state, unsigned unit);
    osg::Matrixd clipProjection(const OsgStateView& state, const osg::Matrixd& projection, bool reversedShaderDepth,
        bool reversedNativeDepth = false);
    std::array<float, 4> depthCoefficients(const osg::Matrixd& projection, bool zeroToOne);
    // Explicit (add, one, zero) blending is replacement, just like blending off.
    Render::EffectBlend effectBlend(const OsgStateView& state);
    bool effectReplacesDestination(const OsgStateView& state);

    std::optional<unsigned> textureUnit(const osg::StateSet& state, std::string_view name);
    std::array<float, 6> textureTransform(const osg::StateSet& state, unsigned unit);
    // OSG's leaf projection is for CPU culling. OpenMW shaders may instead
    // receive a reversed, zero-to-one projection through the effective state.
    // Return zero-to-one depth; preserve reverse-Z when reversedNativeDepth is set.
    osg::Matrixd clipProjection(const osg::StateSet& state, const osg::Matrixd& cullProjection,
        bool reversedShaderDepth, bool reversedNativeDepth = false);
    std::optional<unsigned> textureUnit(const OsgState& state, std::string_view name);
    std::array<float, 6> textureTransform(const OsgState& state, unsigned unit);
    osg::Matrixd clipProjection(const OsgState& state, const osg::Matrixd& cullProjection, bool reversedShaderDepth,
        bool reversedNativeDepth = false);
}

#endif
