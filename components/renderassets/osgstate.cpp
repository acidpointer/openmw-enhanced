#include "osgstate.hpp"
#include "pbrmaterial.hpp"

#include <bit>

#include <components/render/backend.hpp>

#include <components/sceneutil/texturetype.hpp>

#include <osg/BlendColor>
#include <osg/BlendEquation>
#include <osg/BlendFunc>
#include <osg/StateSet>
#include <osg/TexMat>
#include <osg/Texture2D>
#include <osg/Uniform>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <type_traits>

namespace RenderAssets
{
    namespace
    {
        // Sorted, fixed native interface. Keep user-defined effect names out
        // of this table so cache size cannot grow with a mod's uniform count.
        const std::array<std::string, static_cast<unsigned>(NativeUniform::Count)> nativeUniforms{ "LightBuffer",
            "PointLightCount", "actorFade", "alpha", "alphaRef", "atmosphereFade", "blendMap", "diffuseColor",
            "diffuseMap", "distortionStrength", "emissiveMap", "fog.color", "fog.end", "fog.start", "maskMap",
            "material.ambient", "material.diffuse", "material.emission", "material.emissiveMult", "material.shininess",
            "material.specStrength", "material.specular", "material.vertexColorMode", "maximumShadowMapDistance",
            "moonBlend", "nodePosition", "normalMap", "opacity", "pass", "projectionMatrix", "reflectionMap",
            "shadowSpaceMatrix0", "specularMap", "sun.ambient", "sun.diffuse", "sun.position", "sun.specular",
            "terrainSpecularAlpha", "texMat0", "texMat1", "texMat2", "texMat3", "texMat4", "texMat5", "texMat6",
            "texMat7", "waterSurface", "worldMatrix" };

        constexpr std::array<osg::StateAttribute::GLMode, 5> nativeModes{ GL_BLEND, GL_DEPTH_TEST, GL_ALPHA_TEST,
            GL_CULL_FACE, GL_CLIP_PLANE0 };

        template <class Binding, class Flags>
        const Binding* selectBinding(const Binding* inherited, const Binding* local, const Flags& flags)
        {
            return local
                    && (!inherited || !(flags(*inherited) & osg::StateAttribute::OVERRIDE)
                        || (flags(*local) & osg::StateAttribute::PROTECTED))
                ? local
                : inherited;
        }

        int attributeSlot(int unit, osg::StateAttribute::Type type, unsigned member)
        {
            if (member)
                return -1;
            if (unit >= 0)
            {
                if (unit >= 32)
                    return -1;
                if (unit >= 8)
                    return type == SceneUtil::TextureType::AttributeType ? 32 + unit - 8 : -1;
                const int kind = type == osg::StateAttribute::TEXTURE ? 0
                    : type == osg::StateAttribute::TEXMAT             ? 1
                    : type == SceneUtil::TextureType::AttributeType   ? 2
                                                                      : -1;
                return kind < 0 ? -1 : 8 + unit * 3 + kind;
            }
            if (type == PbrMaterial::AttributeType)
                return 56;
            switch (type)
            {
                case osg::StateAttribute::MATERIAL:
                    return 0;
                case osg::StateAttribute::BLENDFUNC:
                    return 1;
                case osg::StateAttribute::DEPTH:
                    return 2;
                case osg::StateAttribute::ALPHAFUNC:
                    return 3;
                case osg::StateAttribute::COLORMASK:
                    return 4;
                case osg::StateAttribute::FRONTFACE:
                    return 5;
                case osg::StateAttribute::CULLFACE:
                    return 6;
                case osg::StateAttribute::PROGRAM:
                    return 7;
                default:
                    return -1;
            }
        }

        template <class Get, class Flags>
        auto resolve(const OsgStateView& view, const Get& get, const Flags& flags)
            -> decltype(get(static_cast<const osg::StateSet*>(nullptr)))
        {
            auto* inherited = view.parent() ? resolve(*view.parent(), get, flags) : nullptr;
            auto* local = view.local() ? get(view.local()) : nullptr;
            return local
                    && (!inherited || !(flags(*inherited) & osg::StateAttribute::OVERRIDE)
                        || (flags(*local) & osg::StateAttribute::PROTECTED))
                ? local
                : inherited;
        }
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

    const osg::Uniform* OsgStateView::getUniform(std::string_view name) const
    {
        const auto native = std::lower_bound(nativeUniforms.begin(), nativeUniforms.end(), name);
        if (native != nativeUniforms.end() && *native == name)
        {
            const auto* binding = uniformBinding(static_cast<unsigned>(native - nativeUniforms.begin()));
            return binding ? binding->first.get() : nullptr;
        }
        // OSG's map comparator is not heterogeneous. Intern lookup keys once
        // instead of allocating a temporary string at every ancestor lookup.
        static thread_local std::set<std::string, std::less<>> names;
        auto key = names.find(name);
        if (key == names.end())
            key = names.emplace(name).first;
        const auto* binding = resolve(
            *this,
            [&](const osg::StateSet* source) {
                const auto& list = source->getUniformList();
                const auto found = list.find(*key);
                return found == list.end() ? nullptr : &found->second;
            },
            [](const auto& value) { return value.second; });
        return binding ? binding->first.get() : nullptr;
    }

    const osg::Uniform* OsgStateView::getUniform(NativeUniform name) const
    {
        const auto slot = static_cast<unsigned>(name);
        if (slot >= nativeUniforms.size())
            return nullptr;
        const auto* binding = uniformBinding(slot);
        return binding ? binding->first.get() : nullptr;
    }

    void OsgStateView::prepareNativeUniforms() const
    {
        constexpr auto count = static_cast<unsigned>(NativeUniform::Count);
        static_assert(count < 64);
        constexpr auto all = (std::uint64_t{ 1 } << count) - 1;
        if (mUniformSeen == all)
            return;
        if (mParent)
        {
            mParent->prepareNativeUniforms();
            mUniformCache = mParent->mUniformCache;
        }
        else
            mUniformCache.fill(nullptr);
        if (mLocal)
            for (const auto& [name, binding] : mLocal->getUniformList())
            {
                const auto found = std::lower_bound(nativeUniforms.begin(), nativeUniforms.end(), name);
                if (found == nativeUniforms.end() || *found != name)
                    continue;
                const auto slot = static_cast<unsigned>(found - nativeUniforms.begin());
                mUniformCache[slot]
                    = selectBinding(mUniformCache[slot], &binding, [](const auto& value) { return value.second; });
            }
        mUniformSeen = all;
    }

    void collectNativeBindings(const osg::StateSet& state, NativeBindings& result)
    {
        result.clear();
        // Record where each binding lives as well as its override value. The
        // flattened state borrows these entries and reads their values live.
        const auto append = [&](std::uintptr_t slot, const auto& binding, const void* object, unsigned flags) {
            result.push_back(
                { slot, reinterpret_cast<std::uintptr_t>(&binding), reinterpret_cast<std::uintptr_t>(object), flags });
        };
        for (const auto& [name, binding] : state.getUniformList())
        {
            const auto found = std::lower_bound(nativeUniforms.begin(), nativeUniforms.end(), name);
            if (found != nativeUniforms.end() && *found == name)
                append(100 + (found - nativeUniforms.begin()), binding, binding.first.get(), binding.second);
        }
        const auto attributes = [&](int unit, const osg::StateSet::AttributeList& list) {
            for (const auto& [key, binding] : list)
                if (const int slot = attributeSlot(unit, key.first, key.second); slot >= 0)
                    append(slot, binding, binding.first.get(), binding.second);
        };
        attributes(-1, state.getAttributeList());
        const auto& textures = state.getTextureAttributeList();
        result.push_back({ 200, textures.size(), 0 });
        for (unsigned unit = 0; unit < std::min<std::size_t>(textures.size(), 32); ++unit)
            attributes(unit, textures[unit]);
        for (const auto& [mode, binding] : state.getModeList())
        {
            const auto found = std::find(nativeModes.begin(), nativeModes.end(), mode);
            if (found != nativeModes.end())
                append(201 + (found - nativeModes.begin()), binding, nullptr, binding);
        }
    }

    void OsgStateView::prepareNativeState() const
    {
        if (mNativeStatePrepared)
            return;
        if (mParent)
        {
            mParent->prepareNativeState();
            mAttributeCache = mParent->mAttributeCache;
            mModeCache = mParent->mModeCache;
            mTextureUnits = mParent->mTextureUnits;
            mTextureLabels = mParent->mTextureLabels;
        }
        else
        {
            mAttributeCache.fill(nullptr);
            mModeCache.fill(nullptr);
            mTextureUnits = 0;
            mTextureLabels = 0;
        }
        prepareNativeUniforms();
        if (mLocal)
        {
            const auto attributes = [&](int unit, const osg::StateSet::AttributeList& list) {
                for (const auto& [key, binding] : list)
                {
                    const int slot = attributeSlot(unit, key.first, key.second);
                    if (slot >= 0)
                    {
                        mAttributeCache[slot] = selectBinding(
                            mAttributeCache[slot], &binding, [](const auto& value) { return value.second; });
                        if (unit >= 0 && key.first == SceneUtil::TextureType::AttributeType)
                        {
                            const auto bit = std::uint32_t{ 1 } << unit;
                            if (mAttributeCache[slot] && mAttributeCache[slot]->first)
                                mTextureLabels |= bit;
                            else
                                mTextureLabels &= ~bit;
                        }
                    }
                }
            };
            attributes(-1, mLocal->getAttributeList());
            const auto& textures = mLocal->getTextureAttributeList();
            mTextureUnits = std::max(mTextureUnits, static_cast<unsigned>(textures.size()));
            for (unsigned unit = 0; unit < std::min<std::size_t>(textures.size(), 32); ++unit)
                attributes(unit, textures[unit]);
            for (const auto& [mode, binding] : mLocal->getModeList())
            {
                const auto found = std::find(nativeModes.begin(), nativeModes.end(), mode);
                if (found != nativeModes.end())
                {
                    auto& cached = mModeCache[found - nativeModes.begin()];
                    cached = selectBinding(cached, &binding, [](auto value) { return value; });
                }
            }
        }
        mAttributeSeen = std::numeric_limits<std::uint64_t>::max();
        mModeSeen = (1u << nativeModes.size()) - 1;
        mNativeStatePrepared = true;
    }

    const osg::StateSet::RefUniformPair* OsgStateView::uniformBinding(unsigned slot) const
    {
        const auto bit = std::uint64_t{ 1 } << slot;
        if (!(mUniformSeen & bit))
        {
            const osg::StateSet::RefUniformPair* local = nullptr;
            if (mLocal)
            {
                const auto& list = mLocal->getUniformList();
                const auto found = list.find(nativeUniforms[slot]);
                if (found != list.end())
                    local = &found->second;
            }
            // A protected local binding already determines the result.
            const auto* inherited = mParent && !(local && (local->second & osg::StateAttribute::PROTECTED))
                ? mParent->uniformBinding(slot)
                : nullptr;
            mUniformCache[slot] = selectBinding(inherited, local, [](const auto& pair) { return pair.second; });
            mUniformSeen |= bit;
        }
        return mUniformCache[slot];
    }

    const osg::StateAttribute* OsgStateView::getAttribute(osg::StateAttribute::Type type, unsigned member) const
    {
        const auto* binding = attributeBinding(-1, type, member);
        return binding ? binding->first.get() : nullptr;
    }
    const osg::StateAttribute* OsgStateView::getTextureAttribute(unsigned unit, osg::StateAttribute::Type type) const
    {
        if (unit > static_cast<unsigned>(std::numeric_limits<int>::max()))
            return nullptr;
        const auto* binding = attributeBinding(static_cast<int>(unit), type, 0);
        return binding ? binding->first.get() : nullptr;
    }

    const osg::StateSet::RefAttributePair* OsgStateView::attributeBinding(
        int unit, osg::StateAttribute::Type type, unsigned member) const
    {
        const int slot = attributeSlot(unit, type, member);
        const auto bit = slot < 0 ? std::uint64_t{ 0 } : std::uint64_t{ 1 } << slot;
        if (mAttributeSeen & bit)
            return mAttributeCache[slot];
        const osg::StateSet::RefAttributePair* local = nullptr;
        if (mLocal && (unit < 0 || static_cast<unsigned>(unit) < mLocal->getTextureAttributeList().size()))
        {
            const auto& list = unit < 0 ? mLocal->getAttributeList() : mLocal->getTextureAttributeList()[unit];
            const auto found = list.find({ type, member });
            if (found != list.end())
                local = &found->second;
        }
        const auto* inherited = mParent && !(local && (local->second & osg::StateAttribute::PROTECTED))
            ? mParent->attributeBinding(unit, type, member)
            : nullptr;
        const auto* result = selectBinding(inherited, local, [](const auto& pair) { return pair.second; });
        if (bit)
        {
            mAttributeCache[slot] = result;
            mAttributeSeen |= bit;
        }
        return result;
    }

    const osg::StateAttribute::GLModeValue* OsgStateView::modeBinding(osg::StateAttribute::GLMode mode) const
    {
        const auto foundMode = std::find(nativeModes.begin(), nativeModes.end(), mode);
        const auto slot = foundMode - nativeModes.begin();
        const unsigned bit = foundMode == nativeModes.end() ? 0u : 1u << slot;
        if (mModeSeen & bit)
            return mModeCache[slot];
        const osg::StateAttribute::GLModeValue* local = nullptr;
        if (mLocal)
        {
            const auto& list = mLocal->getModeList();
            const auto found = list.find(mode);
            if (found != list.end())
                local = &found->second;
        }
        const auto* inherited
            = mParent && !(local && (*local & osg::StateAttribute::PROTECTED)) ? mParent->modeBinding(mode) : nullptr;
        const auto* result = selectBinding(inherited, local, [](auto value) { return value; });
        if (bit)
        {
            mModeCache[slot] = result;
            mModeSeen |= bit;
        }
        return result;
    }

    osg::StateAttribute::GLModeValue OsgStateView::getMode(osg::StateAttribute::GLMode mode) const
    {
        const auto* binding = modeBinding(mode);
        return binding ? *binding : static_cast<osg::StateAttribute::GLModeValue>(osg::StateAttribute::INHERIT);
    }
    unsigned OsgStateView::textureUnits() const
    {
        if (mNativeStatePrepared)
            return mTextureUnits;
        return std::max(mParent ? mParent->textureUnits() : 0u,
            mLocal ? static_cast<unsigned>(mLocal->getTextureAttributeList().size()) : 0u);
    }

    unsigned OsgStateView::nextTextureLabelUnit(unsigned start, unsigned end) const
    {
        if (!mNativeStatePrepared || start >= 32)
            return start;
        const auto candidates = mTextureLabels & (~std::uint32_t{ 0 } << start);
        return std::min(static_cast<unsigned>(std::countr_zero(candidates)), end);
    }
    OsgState OsgStateView::snapshot() const
    {
        auto result = mParent ? mParent->snapshot() : OsgState{};
        if (mLocal)
            result.merge(*mLocal);
        return result;
    }

    Render::EffectBlend effectBlend(const OsgStateView& state)
    {
        Render::EffectBlend result;
        if (effectReplacesDestination(state))
            return result;
        result.mEnabled = true;
        const auto factor = [](GLenum value) -> Render::EffectBlendFactor {
            using F = Render::EffectBlendFactor;
            switch (value)
            {
                case GL_ZERO:
                    return F::Zero;
                case GL_ONE:
                    return F::One;
                case GL_SRC_COLOR:
                    return F::SourceColor;
                case GL_ONE_MINUS_SRC_COLOR:
                    return F::OneMinusSourceColor;
                case GL_DST_COLOR:
                    return F::DestinationColor;
                case GL_ONE_MINUS_DST_COLOR:
                    return F::OneMinusDestinationColor;
                case GL_SRC_ALPHA:
                    return F::SourceAlpha;
                case GL_ONE_MINUS_SRC_ALPHA:
                    return F::OneMinusSourceAlpha;
                case GL_DST_ALPHA:
                    return F::DestinationAlpha;
                case GL_ONE_MINUS_DST_ALPHA:
                    return F::OneMinusDestinationAlpha;
                case GL_CONSTANT_COLOR:
                    return F::ConstantColor;
                case GL_ONE_MINUS_CONSTANT_COLOR:
                    return F::OneMinusConstantColor;
                case GL_CONSTANT_ALPHA:
                    return F::ConstantAlpha;
                case GL_ONE_MINUS_CONSTANT_ALPHA:
                    return F::OneMinusConstantAlpha;
                case GL_SRC_ALPHA_SATURATE:
                    return F::SourceAlphaSaturate;
                default:
                    throw std::invalid_argument("Unsupported OMWFX blend factor");
            }
        };
        if (const auto* binding
            = dynamic_cast<const osg::BlendFunc*>(state.getAttribute(osg::StateAttribute::BLENDFUNC)))
            result.mFactors = { factor(binding->getSourceRGB()), factor(binding->getDestinationRGB()),
                factor(binding->getSourceAlpha()), factor(binding->getDestinationAlpha()) };
        const auto operation = [](GLenum value) -> Render::EffectBlendOp {
            using O = Render::EffectBlendOp;
            switch (value)
            {
                case GL_FUNC_ADD:
                    return O::Add;
                case GL_FUNC_SUBTRACT:
                    return O::Subtract;
                case GL_FUNC_REVERSE_SUBTRACT:
                    return O::ReverseSubtract;
                case GL_MIN:
                    return O::Min;
                case GL_MAX:
                    return O::Max;
                default:
                    throw std::invalid_argument("Unsupported OMWFX blend operation");
            }
        };
        if (const auto* binding
            = dynamic_cast<const osg::BlendEquation*>(state.getAttribute(osg::StateAttribute::BLENDEQUATION)))
            result.mOperations = { operation(binding->getEquationRGB()), operation(binding->getEquationAlpha()) };
        if (const auto* binding
            = dynamic_cast<const osg::BlendColor*>(state.getAttribute(osg::StateAttribute::BLENDCOLOR)))
        {
            const auto& color = binding->getConstantColor();
            for (unsigned i = 0; i < 4; ++i)
            {
                if (!std::isfinite(color[i]))
                    throw std::invalid_argument("Nonfinite OMWFX blend color");
                result.mConstant[i] = std::clamp(color[i], 0.f, 1.f);
            }
        }
        return result;
    }

    bool effectReplacesDestination(const OsgStateView& state)
    {
        if (!(state.getMode(GL_BLEND) & osg::StateAttribute::ON))
            return true;
        const auto* blend = dynamic_cast<const osg::BlendFunc*>(state.getAttribute(osg::StateAttribute::BLENDFUNC));
        const auto* equation
            = dynamic_cast<const osg::BlendEquation*>(state.getAttribute(osg::StateAttribute::BLENDEQUATION));
        return (!blend
                   || (blend->getSourceRGB() == GL_ONE && blend->getDestinationRGB() == GL_ZERO
                       && blend->getSourceAlpha() == GL_ONE && blend->getDestinationAlpha() == GL_ZERO))
            && (!equation
                || (equation->getEquationRGB() == GL_FUNC_ADD && equation->getEquationAlpha() == GL_FUNC_ADD));
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
    const osg::Uniform* OsgState::getUniform(NativeUniform name) const
    {
        const auto slot = static_cast<unsigned>(name);
        return slot < nativeUniforms.size() ? getUniform(nativeUniforms[slot]) : nullptr;
    }
    const osg::Uniform* nativeUniform(const osg::StateSet& state, NativeUniform name)
    {
        const auto slot = static_cast<unsigned>(name);
        return slot < nativeUniforms.size() ? state.getUniform(nativeUniforms[slot]) : nullptr;
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
        const auto* uniform = [&] {
            if (name == "diffuseMap")
                return nativeUniform(state, NativeUniform::DiffuseMap);
            if (name == "blendMap")
                return nativeUniform(state, NativeUniform::BlendMap);
            if (name == "normalMap")
                return nativeUniform(state, NativeUniform::NormalMap);
            if (name == "specularMap")
                return nativeUniform(state, NativeUniform::SpecularMap);
            if (name == "emissiveMap")
                return nativeUniform(state, NativeUniform::EmissiveMap);
            if (name == "reflectionMap")
                return nativeUniform(state, NativeUniform::ReflectionMap);
            if (name == "maskMap")
                return nativeUniform(state, NativeUniform::MaskMap);
            if constexpr (std::is_same_v<State, osg::StateSet>)
                return state.getUniform(std::string(name));
            else
                return state.getUniform(name);
        }();
        if (uniform)
        {
            int unit = -1;
            if (uniform->get(unit) && unit >= 0)
                return static_cast<unsigned>(unit);
        }
        const auto units = [&] {
            if constexpr (std::is_same_v<State, OsgStateView>)
                return state.textureUnits();
            else
                return static_cast<unsigned>(state.getTextureAttributeList().size());
        }();
        const auto next = [&](unsigned unit) {
            if constexpr (std::is_same_v<State, OsgStateView>)
                return state.nextTextureLabelUnit(unit, units);
            else
                return unit;
        };
        for (unsigned unit = next(0); unit < units; unit = next(unit + 1))
            if (const auto* type = state.getTextureAttribute(unit, SceneUtil::TextureType::AttributeType);
                type && type->getName() == name)
                return unit;
        // Unlabelled fixed-function assets conventionally use unit zero.
        // A named normal/blend/reflection sampler prevents that fallback.
        if (name == "diffuseMap" && !nativeUniform(state, NativeUniform::NormalMap)
            && !nativeUniform(state, NativeUniform::ReflectionMap)
            && state.getTextureAttribute(0, osg::StateAttribute::TEXTURE))
            return 0;
        return {};
    }

    template <class State>
    std::array<float, 6> textureTransformImpl(const State& state, unsigned unit)
    {
        osg::Matrixf matrix;
        const auto* texMatrix = unit < 8
            ? nativeUniform(state, static_cast<NativeUniform>(static_cast<unsigned>(NativeUniform::TexMat0) + unit))
            : state.getUniform("texMat" + std::to_string(unit));
        if (const auto* uniform = texMatrix)
            uniform->get(matrix);
        if (const auto* attribute
            = dynamic_cast<const osg::TexMat*>(state.getTextureAttribute(unit, osg::StateAttribute::TEXMAT)))
            matrix = attribute->getMatrix();
        return { matrix(0, 0), matrix(1, 0), matrix(3, 0), matrix(0, 1), matrix(1, 1), matrix(3, 1) };
    }

    template <class State>
    osg::Matrixd clipProjectionImpl(
        const State& state, const osg::Matrixd& cullProjection, bool reversedShaderDepth, bool reversedNativeDepth)
    {
        osg::Matrixf shaderProjection;
        const auto* uniform = nativeUniform(state, NativeUniform::ProjectionMatrix);
        const bool authored = uniform && uniform->get(shaderProjection);
        // Authored reverse-Z matrices already use zero-to-one depth. Do not
        // subtract them from one and back: that loses their small coefficients.
        if (authored && reversedShaderDepth && reversedNativeDepth)
            return osg::Matrixd(shaderProjection);
        osg::Matrixd depth;
        if (authored && reversedShaderDepth)
        {
            depth(2, 2) = -1;
            depth(3, 2) = 1;
        }
        else
        {
            depth(2, 2) = reversedNativeDepth ? -.5 : .5;
            depth(3, 2) = .5;
        }
        return (authored ? osg::Matrixd(shaderProjection) : cullProjection) * depth;
    }
    std::optional<unsigned> textureUnit(const osg::StateSet& state, std::string_view name)
    {
        return textureUnitImpl(state, name);
    }
    std::optional<unsigned> textureUnit(const OsgStateView& state, std::string_view name)
    {
        return textureUnitImpl(state, name);
    }
    std::array<float, 6> textureTransform(const OsgStateView& state, unsigned unit)
    {
        return textureTransformImpl(state, unit);
    }
    osg::Matrixd clipProjection(
        const OsgStateView& state, const osg::Matrixd& projection, bool reversed, bool reversedNative)
    {
        return clipProjectionImpl(state, projection, reversed, reversedNative);
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
    osg::Matrixd clipProjection(
        const osg::StateSet& state, const osg::Matrixd& projection, bool reversed, bool reversedNative)
    {
        return clipProjectionImpl(state, projection, reversed, reversedNative);
    }
    osg::Matrixd clipProjection(
        const OsgState& state, const osg::Matrixd& projection, bool reversed, bool reversedNative)
    {
        return clipProjectionImpl(state, projection, reversed, reversedNative);
    }

}
