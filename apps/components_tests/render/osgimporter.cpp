#include <components/renderassets/meshkey.hpp>
#include <components/renderassets/osgimporter.hpp>
#include <components/renderassets/osgstate.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/texturetype.hpp>

#include <gtest/gtest.h>
#include <limits>

#include <algorithm>
#include <cstring>

#include <osg/AlphaFunc>
#include <osg/BlendFunc>
#include <osg/ColorMask>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/Program>
#include <osg/Switch>
#include <osg/TexMat>
#include <osg/Texture2D>
#include <osg/Texture3D>

namespace
{
    TEST(RenderSamplerImport, IgnoresTheDefaultLegacyRWrapOnTwoDimensionalUiTextures)
    {
        // UI and loading-screen textures set S/T but leave the unused R axis
        // at OSG's CLAMP default. Importing R used to throw before the menu.
        osg::ref_ptr<osg::Texture2D> source = new osg::Texture2D;
        source->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        source->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        ASSERT_EQ(source->getWrap(osg::Texture::WRAP_R), osg::Texture::CLAMP);
        Render::SamplerState sampler;
        ASSERT_NO_THROW(sampler = RenderAssets::OsgImporter::importSampler(*source));
        EXPECT_EQ(sampler.mWrapS, Render::TextureWrap::Clamp);
        EXPECT_EQ(sampler.mWrapT, Render::TextureWrap::Clamp);
        // Changing an irrelevant axis must not split native sampler caches.
        source->setWrap(osg::Texture::WRAP_R, osg::Texture::MIRROR);
        EXPECT_EQ(RenderAssets::OsgImporter::importSampler(*source), sampler);
    }

    TEST(RenderSamplerImport, RetainsTheAuthoredRWrapForVolumeTextures)
    {
        osg::ref_ptr<osg::Texture3D> source = new osg::Texture3D;
        source->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        source->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
        source->setWrap(osg::Texture::WRAP_R, osg::Texture::MIRROR);
        EXPECT_EQ(RenderAssets::OsgImporter::importSampler(*source).mWrapR, Render::TextureWrap::Mirror);
        source->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP);
        EXPECT_EQ(RenderAssets::OsgImporter::importSampler(*source).mWrapR, Render::TextureWrap::Clamp);
    }

    TEST(RenderStateSnapshot, CopiesDoNotRegisterAsOsgParentsAndUniformsStayLive)
    {
        osg::ref_ptr<osg::StateSet> source = new osg::StateSet;
        osg::ref_ptr<osg::Uniform> uniform = new osg::Uniform("shared", 1.f);
        osg::ref_ptr<osg::Material> material = new osg::Material;
        source->addUniform(uniform);
        source->setAttribute(material);
        const auto uniformParents = uniform->getNumParents();
        const auto materialParents = material->getNumParents();
        std::vector<RenderAssets::OsgState> snapshots(512, RenderAssets::OsgState(*source));
        EXPECT_EQ(uniform->getNumParents(), uniformParents);
        EXPECT_EQ(material->getNumParents(), materialParents);
        uniform->set(2.f);
        float value = 0;
        ASSERT_TRUE(snapshots.back().getUniform("shared")->get(value));
        EXPECT_FLOAT_EQ(value, 2);
        EXPECT_TRUE(snapshots.back().matches(*source));
        snapshots.clear();
        EXPECT_EQ(uniform->getNumParents(), uniformParents);
        EXPECT_EQ(material->getNumParents(), materialParents);
    }

    TEST(RenderStateSnapshot, PreservesOverrideProtectedAndExplicitOffState)
    {
        osg::ref_ptr<osg::StateSet> parent = new osg::StateSet;
        osg::ref_ptr<osg::StateSet> child = new osg::StateSet;
        parent->addUniform(new osg::Uniform("value", 1), osg::StateAttribute::OVERRIDE);
        parent->setMode(GL_DEPTH_TEST, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        child->addUniform(new osg::Uniform("value", 2));
        child->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        RenderAssets::OsgState result(*parent);
        result.merge(*child);
        int value = 0;
        result.getUniform("value")->get(value);
        EXPECT_EQ(value, 1);
        EXPECT_TRUE(result.getMode(GL_DEPTH_TEST) & osg::StateAttribute::ON);
        child->addUniform(new osg::Uniform("value", 3), osg::StateAttribute::PROTECTED);
        child->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::PROTECTED);
        result.merge(*child);
        result.getUniform("value")->get(value);
        EXPECT_EQ(value, 3);
        EXPECT_FALSE(result.getMode(GL_DEPTH_TEST) & osg::StateAttribute::ON);
    }

    TEST(RenderStateView, ProtectedIntermediateStateResetsAnAncestorsOverride)
    {
        osg::ref_ptr<osg::StateSet> root = new osg::StateSet, middle = new osg::StateSet, leaf = new osg::StateSet;
        root->addUniform(new osg::Uniform("value", 1), osg::StateAttribute::OVERRIDE);
        middle->addUniform(new osg::Uniform("value", 2), osg::StateAttribute::PROTECTED);
        leaf->addUniform(new osg::Uniform("value", 3));
        root->setMode(GL_DEPTH_TEST, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        middle->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::PROTECTED);
        RenderAssets::OsgStateView a(nullptr, root), b(&a, middle), c(&b, leaf);
        int value = 0;
        ASSERT_TRUE(c.getUniform("value")->get(value));
        EXPECT_EQ(value, 3);
        EXPECT_EQ(c.getMode(GL_DEPTH_TEST), osg::StateAttribute::PROTECTED);
        const auto snapshot = c.snapshot();
        EXPECT_EQ(snapshot.getUniform("value"), c.getUniform("value"));
        EXPECT_EQ(snapshot.getMode(GL_DEPTH_TEST), c.getMode(GL_DEPTH_TEST));
        // Borrowing never registers an OSG parent or owns a source node.
        EXPECT_EQ(leaf->getUniform("value")->getNumParents(), 1u);
        leaf->getUniform("value")->set(4);
        c.getUniform("value")->get(value);
        EXPECT_EQ(value, 4);
    }

    TEST(RenderStateView, TextureUnitsRespectInheritedOverrideAndProtectedBindings)
    {
        osg::ref_ptr<osg::StateSet> root = new osg::StateSet, child = new osg::StateSet;
        osg::ref_ptr<osg::Texture2D> first = new osg::Texture2D, second = new osg::Texture2D;
        root->setTextureAttribute(3, first, osg::StateAttribute::OVERRIDE);
        child->setTextureAttribute(3, second);
        RenderAssets::OsgStateView a(nullptr, root), b(&a, child);
        EXPECT_EQ(b.textureUnits(), 4u);
        EXPECT_EQ(b.getTextureAttribute(3, osg::StateAttribute::TEXTURE), first.get());
        child->setTextureAttribute(3, second, osg::StateAttribute::PROTECTED);
        b.bind(&a, child);
        EXPECT_EQ(b.getTextureAttribute(3, osg::StateAttribute::TEXTURE), second.get());
    }

    TEST(RenderStateView, MemoizedBindingsReadLiveValuesAndInvalidateAtFrameBind)
    {
        osg::ref_ptr<osg::StateSet> root = new osg::StateSet, child = new osg::StateSet;
        osg::ref_ptr<osg::Uniform> first = new osg::Uniform("alpha", .5f);
        root->addUniform(first, osg::StateAttribute::OVERRIDE);
        child->addUniform(new osg::Uniform("alpha", .2f));
        root->setMode(GL_BLEND, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        RenderAssets::OsgStateView a(nullptr, root), b(&a, child);
        EXPECT_EQ(b.getUniform("alpha"), first.get());
        EXPECT_EQ(b.getUniform("waterSurface"), nullptr);
        EXPECT_TRUE(b.getMode(GL_BLEND) & osg::StateAttribute::ON);
        EXPECT_EQ(b.getAttribute(osg::StateAttribute::COLORMASK), nullptr);
        first->set(.75f);
        float alpha = 0;
        ASSERT_TRUE(b.getUniform("alpha")->get(alpha));
        EXPECT_FLOAT_EQ(alpha, .75f);

        osg::ref_ptr<osg::Uniform> replacement = new osg::Uniform("alpha", .3f);
        child->addUniform(replacement, osg::StateAttribute::PROTECTED);
        root->addUniform(new osg::Uniform("waterSurface", true));
        child->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::PROTECTED);
        osg::ref_ptr<osg::ColorMask> mask = new osg::ColorMask(false, false, false, false);
        root->setAttribute(mask);
        a.bind(nullptr, root);
        b.bind(&a, child);
        EXPECT_EQ(b.getUniform("alpha"), replacement.get());
        EXPECT_EQ(b.getUniform("waterSurface"), root->getUniform("waterSurface"));
        EXPECT_FALSE(b.getMode(GL_BLEND) & osg::StateAttribute::ON);
        EXPECT_EQ(b.getAttribute(osg::StateAttribute::COLORMASK), mask.get());
        EXPECT_EQ(b.getUniform("alpha"), b.snapshot().getUniform("alpha"));
        osg::ref_ptr<osg::StateSet> descendant = new osg::StateSet;
        descendant->addUniform(new osg::Uniform("alpha", .4f));
        RenderAssets::OsgStateView c(&b, descendant);
        EXPECT_EQ(c.getUniform("alpha"), descendant->getUniform("alpha"));
        EXPECT_EQ(c.getMode(GL_BLEND), b.getMode(GL_BLEND));
        EXPECT_EQ(first->getNumParents(), 1u);
    }

    TEST(RenderStateView, PreparedNativeSlotsMatchNamedBindingsAndRetainOverrideRules)
    {
        using RenderAssets::NativeUniform;
        osg::ref_ptr<osg::StateSet> root = new osg::StateSet, middle = new osg::StateSet, leaf = new osg::StateSet;
        root->addUniform(new osg::Uniform("alpha", .8f), osg::StateAttribute::OVERRIDE);
        middle->addUniform(new osg::Uniform("alpha", .4f), osg::StateAttribute::PROTECTED);
        leaf->addUniform(new osg::Uniform("alpha", .2f));
        root->addUniform(new osg::Uniform("sun.diffuse", osg::Vec4f(1, 2, 3, 1)));
        root->addUniform(new osg::Uniform("projectionMatrix", osg::Matrixf::scale(2, 3, 4)));
        root->addUniform(new osg::Uniform("effectCustomValue", 17.f));
        for (unsigned unit : { 0u, 3u, 7u })
        {
            const auto name = "texMat" + std::to_string(unit);
            root->addUniform(new osg::Uniform(name.c_str(), osg::Matrixf::scale(unit + 1, 1, 1)));
        }
        RenderAssets::OsgStateView a(nullptr, root), b(&a, middle), c(&b, leaf);
        c.prepareNativeUniforms();
        EXPECT_EQ(c.getUniform(NativeUniform::Alpha), leaf->getUniform("alpha"));
        EXPECT_EQ(c.getUniform(NativeUniform::SunDiffuse), root->getUniform("sun.diffuse"));
        EXPECT_EQ(c.getUniform(NativeUniform::ProjectionMatrix), root->getUniform("projectionMatrix"));
        EXPECT_EQ(c.getUniform("effectCustomValue"), root->getUniform("effectCustomValue"));
        EXPECT_EQ(c.getUniform(NativeUniform::WaterSurface), nullptr);
        EXPECT_EQ(c.getUniform(NativeUniform::Count), nullptr);
        for (unsigned unit : { 0u, 3u, 7u })
            EXPECT_FLOAT_EQ(RenderAssets::textureTransform(c, unit)[0], unit + 1);
        leaf->getUniform("alpha")->set(.3f);
        float alpha = 0;
        ASSERT_TRUE(c.getUniform(NativeUniform::Alpha)->get(alpha));
        EXPECT_FLOAT_EQ(alpha, .3f);
        root->addUniform(new osg::Uniform("waterSurface", true));
        a.bind(nullptr, root);
        b.bind(&a, middle);
        c.bind(&b, leaf);
        c.prepareNativeUniforms();
        EXPECT_EQ(c.getUniform(NativeUniform::WaterSurface), root->getUniform("waterSurface"));
        EXPECT_EQ(c.getUniform(NativeUniform::Alpha), c.snapshot().getUniform("alpha"));
        EXPECT_EQ(RenderAssets::nativeUniform(*root, NativeUniform::SunDiffuse), root->getUniform("sun.diffuse"));
    }

    TEST(RenderStateView, ColorlessOcclusionProxiesKeepTheirInheritedMask)
    {
        osg::ref_ptr<osg::StateSet> parent = new osg::StateSet, child = new osg::StateSet;
        osg::ref_ptr<osg::ColorMask> hidden = new osg::ColorMask(false, false, false, false);
        parent->setAttributeAndModes(hidden);
        RenderAssets::OsgStateView a(nullptr, parent), b(&a, child);
        EXPECT_EQ(b.getAttribute(osg::StateAttribute::COLORMASK), hidden.get());
    }

    TEST(RenderMeshStream, ByteColorsRetainExactChannelsAndFloatingAlpha)
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array(3);
        (*vertices)[1].x() = 1;
        (*vertices)[2].y() = 1;
        osg::ref_ptr<osg::Vec4ubArray> colors = new osg::Vec4ubArray(3);
        (*colors)[0].set(17, 43, 71, 102);
        (*colors)[1].set(255, 0, 128, 17);
        (*colors)[2].set(0, 255, 0, 255);
        geometry->setVertexArray(vertices);
        geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
        geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
        RenderAssets::OsgImporter importer;
        const auto mesh = importer.importMesh(*geometry);
        ASSERT_EQ(mesh->mVertices.size(), 3u);
        EXPECT_EQ(mesh->mVertices[0].mColor, (std::array<std::uint8_t, 4>{ 17, 43, 71, 102 }));
        EXPECT_FLOAT_EQ(mesh->mVertices[0].mAlpha, .4f);
        EXPECT_EQ(mesh->mVertices[1].mColor, (std::array<std::uint8_t, 4>{ 255, 0, 128, 17 }));
        EXPECT_FLOAT_EQ(mesh->mVertices[1].mAlpha, static_cast<float>(17.0 / 255.0));
    }

    TEST(RenderMeshStream, PoseFastPathStillPicksUpChangedUvAndColors)
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        const std::array<osg::Vec3f, 3> p{ osg::Vec3(0, 0, 0), osg::Vec3(1, 0, 0), osg::Vec3(0, 1, 0) };
        const std::array<osg::Vec2f, 3> t{ osg::Vec2(0, 0), osg::Vec2(1, 0), osg::Vec2(0, 1) };
        osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array(p.begin(), p.end());
        osg::ref_ptr<osg::Vec2Array> uv = new osg::Vec2Array(t.begin(), t.end());
        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array(1);
        (*colors)[0].set(1, 0, 0, 1);
        geometry->setVertexArray(positions);
        geometry->setTexCoordArray(0, uv);
        geometry->setColorArray(colors, osg::Array::BIND_OVERALL);
        geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
        RenderAssets::OsgImporter importer;
        auto stream = importer.importMeshStream(*geometry);
        (*positions)[1].z() = 2;
        positions->dirty();
        importer.updateMeshStream(*geometry, stream);
        EXPECT_FLOAT_EQ(stream.mVertices[1].mPosition[2], 2);
        (*uv)[1].x() = .25f;
        uv->dirty();
        (*colors)[0].set(0, 1, 0, 1);
        colors->dirty();
        importer.updateMeshStream(*geometry, stream);
        EXPECT_FLOAT_EQ(stream.mVertices[1].mUV[0], .25f);
        EXPECT_EQ(stream.mVertices[1].mColor, (std::array<std::uint8_t, 4>{ 0, 255, 0, 255 }));
        (*positions)[1].z() = 4;
        positions->dirty();
        importer.updateMeshStream(*geometry, stream);
        EXPECT_FLOAT_EQ(stream.mVertices[1].mPosition[2], 4);
        EXPECT_FLOAT_EQ(stream.mVertices[1].mUV[0], .25f);
    }

    TEST(RenderProjection, OpaqueWaterDepthRetainsWorldDistanceAtLongViewRanges)
    {
        RenderAssets::OsgState state;
        const auto projection
            = RenderAssets::clipProjection(state, osg::Matrixd::perspective(75, 1.6, 1, 81920), false);
        const auto coefficients = RenderAssets::depthCoefficients(projection, true);
        for (double distance : { 20., 2500., 5000., 40000. })
        {
            const auto clip = osg::Vec4d(0, 0, -distance, 1) * projection;
            const float sample = static_cast<float>(clip.z() / clip.w());
            const float viewZ
                = (coefficients[1] - sample * coefficients[3]) / (sample * coefficients[2] - coefficients[0]);
            EXPECT_NEAR(-viewZ, distance, distance * .01);
        }
    }

    TEST(RenderProjection, FirstPersonDepthCanBeReadInTheWorldProjection)
    {
        const auto world = osg::Matrixd::perspective(75, 1.6, 3, 5000);
        const auto fp = osg::Matrixd::perspective(90, 1.6, .01, 1000);
        RenderAssets::OsgState empty;
        const auto nativeFP = RenderAssets::clipProjection(empty, fp, false);
        const auto a = RenderAssets::depthCoefficients(nativeFP, true);
        const auto b = RenderAssets::depthCoefficients(world, false);
        for (double distance : { 5., 10., 30., 100. })
        {
            const auto clip = osg::Vec4d(0, 0, -distance, 1) * nativeFP;
            const double sample = clip.z() / clip.w();
            const double viewZ = (a[1] - sample * a[3]) / (sample * a[2] - a[0]);
            const double remapped = (b[0] * viewZ + b[1]) / (b[2] * viewZ + b[3]);
            const auto reference = osg::Vec4d(0, 0, -distance, 1) * world;
            EXPECT_NEAR(remapped, .5 * (reference.z() / reference.w() + 1), 1e-3);
        }
    }

    TEST(RenderProjection, CullProjectionKeepsNearSurfacesAheadOfFarSurfacesWithReverseZEnabled)
    {
        osg::ref_ptr<osg::StateSet> ownedState = new osg::StateSet;
        auto& state = *ownedState;
        const auto cull = osg::Matrixd::perspective(60, 1.5, 3, 10000);
        const auto projection = RenderAssets::clipProjection(state, cull, true);
        double previous = -1;
        for (double distance : { 3., 10., 100., 1000., 10000. })
        {
            const auto clip = osg::Vec4d(0, 0, -distance, 1) * projection;
            const auto depth = clip.z() / clip.w();
            EXPECT_NEAR(clip.w(), distance, 1e-6);
            EXPECT_GE(depth, -1e-6);
            EXPECT_LE(depth, 1. + 1e-6);
            EXPECT_GT(depth, previous);
            previous = depth;
        }
    }

    TEST(RenderProjection, AuthoredReversedProjectionAndStandardCullProjectionHaveMatchingDepth)
    {
        osg::ref_ptr<osg::StateSet> ownedState = new osg::StateSet;
        auto& state = *ownedState;
        const auto cull = osg::Matrixd::perspective(60, 1.5, 3, 10000);
        const auto expected = RenderAssets::clipProjection(state, cull, false);
        const auto reversed = SceneUtil::getReversedZProjectionMatrixAsPerspective(60, 1.5, 3, 10000);
        state.addUniform(new osg::Uniform("projectionMatrix", osg::Matrixf(reversed)));
        const auto actual = RenderAssets::clipProjection(state, cull, true);
        for (double distance : { 3., 10., 100., 1000., 10000. })
        {
            const auto a = osg::Vec4d(0, 0, -distance, 1) * actual;
            const auto b = osg::Vec4d(0, 0, -distance, 1) * expected;
            EXPECT_NEAR(a.z() / a.w(), b.z() / b.w(), 1e-6);
        }
    }

    TEST(RenderProjection, UsesEachViewsAuthoredProjectionIncludingOrthographicMaps)
    {
        osg::ref_ptr<osg::StateSet> ownedState = new osg::StateSet;
        auto& state = *ownedState;
        const auto cull = osg::Matrixd::perspective(60, 1.5, 3, 10000);
        const auto ortho = SceneUtil::getReversedZProjectionMatrixAsOrtho(-100, 100, -100, 100, 5, 1005);
        state.addUniform(new osg::Uniform("projectionMatrix", osg::Matrixf(ortho)));
        const auto projection = RenderAssets::clipProjection(state, cull, true);
        const auto nearClip = osg::Vec4d(0, 0, -5, 1) * projection;
        const auto farClip = osg::Vec4d(0, 0, -1005, 1) * projection;
        EXPECT_NEAR(nearClip.z() / nearClip.w(), 0, 1e-6);
        EXPECT_NEAR(farClip.z() / farClip.w(), 1, 1e-6);
        EXPECT_DOUBLE_EQ(nearClip.w(), 1);
    }

    osg::ref_ptr<osg::Geometry> triangle(GLenum mode = GL_TRIANGLES)
    {
        osg::ref_ptr<osg::Geometry> result = new osg::Geometry;
        const std::array<osg::Vec3f, 4> vertices{ { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 } } };
        result->setVertexArray(new osg::Vec3Array(vertices.begin(), vertices.end()));
        result->addPrimitiveSet(new osg::DrawArrays(mode, 0, mode == GL_TRIANGLE_STRIP ? 4 : 3));
        return result;
    }

    osg::ref_ptr<osg::Texture2D> texture()
    {
        osg::ref_ptr<osg::Image> source = new osg::Image;
        source->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        source->setOrigin(osg::Image::TOP_LEFT);
        source->setColor(osg::Vec4f(1, 0, 0, 1), 0, 0);
        osg::ref_ptr<osg::Texture2D> result = new osg::Texture2D(source);
        result->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        result->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        return result;
    }

    TEST(RenderMeshKey, SharesShallowClonesAndSeparatesDirtyGeometry)
    {
        auto original = triangle();
        osg::ref_ptr<osg::Geometry> clone = new osg::Geometry(*original, osg::CopyOp::SHALLOW_COPY);
        const auto key = RenderAssets::meshKey(*original, 0);
        EXPECT_EQ(key.mWords, RenderAssets::meshKey(*clone, 0).mWords);
        clone->setVertexArray(new osg::Vec3Array(*static_cast<const osg::Vec3Array*>(original->getVertexArray())));
        EXPECT_NE(key.mWords, RenderAssets::meshKey(*clone, 0).mWords);
        original->getVertexArray()->dirty();
        EXPECT_TRUE(key.alive());
        EXPECT_FALSE(key.current());
        EXPECT_NE(key.mWords, RenderAssets::meshKey(*original, 0).mWords);
    }
    TEST(RenderMeshKey, DoesNotKeepSourceArraysAlive)
    {
        RenderAssets::MeshKey key;
        {
            auto source = triangle();
            key = RenderAssets::meshKey(*source, 0);
            EXPECT_TRUE(key.alive());
        }
        EXPECT_FALSE(key.alive());
    }

    TEST(RenderMeshImport, MultiDrawStripsRestartTopologyAtEachRange)
    {
        auto geometry = triangle();
        osg::ref_ptr<osg::MultiDrawArrays> ranges = new osg::MultiDrawArrays(GL_TRIANGLE_STRIP);
        ranges->setFirsts({ 0, 1 });
        ranges->setCounts({ 3, 3 });
        geometry->setPrimitiveSet(0, ranges);
        RenderAssets::OsgImporter importer;
        const auto imported = importer.importMesh(*geometry);
        ASSERT_EQ(imported->mVertices.size(), 4u);
        EXPECT_EQ(imported->mIndices, (std::vector<std::uint32_t>{ 0, 1, 2, 1, 2, 3 }));
        ranges->setCounts({ 3, 4 });
        EXPECT_THROW(importer.importMesh(*geometry), std::invalid_argument);
    }

    TEST(RenderMeshImport, PerSetChannelsRetainIndependentVerticesAcrossDrawRanges)
    {
        auto geometry = triangle();
        geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
        const std::array<osg::Vec4f, 2> colors{ osg::Vec4f(1, 0, 0, 1), osg::Vec4f(0, 1, 0, 1) };
        geometry->setColorArray(new osg::Vec4Array(colors.begin(), colors.end()), osg::Array::BIND_PER_PRIMITIVE_SET);
        RenderAssets::OsgImporter importer;
        const auto imported = importer.importMesh(*geometry);
        ASSERT_EQ(imported->mVertices.size(), 6u);
        EXPECT_EQ(imported->mVertices[0].mColor, (std::array<std::uint8_t, 4>{ 255, 0, 0, 255 }));
        EXPECT_EQ(imported->mVertices[3].mColor, (std::array<std::uint8_t, 4>{ 0, 255, 0, 255 }));
    }

    TEST(RenderMeshStream, UpdatesPoseWithoutChangingTopologyOrAllocatingVertexStorage)
    {
        auto geometry = triangle(GL_TRIANGLE_STRIP);
        const osg::Vec3f normal(0, 0, 1);
        geometry->setNormalArray(new osg::Vec3Array(1, &normal), osg::Array::BIND_OVERALL);
        RenderAssets::OsgImporter importer;
        auto stream = importer.importMeshStream(*geometry);
        const auto* vertices = stream.mVertices.data();
        const auto* indices = stream.mIndices.data();
        const auto triangles = stream.mIndices;
        auto* positions = static_cast<osg::Vec3Array*>(geometry->getVertexArray());
        (*positions)[1].z() = 5;
        positions->dirty();
        importer.updateMeshStream(*geometry, stream);
        EXPECT_EQ(stream.mVertices.data(), vertices);
        EXPECT_EQ(stream.mIndices.data(), indices);
        EXPECT_EQ(stream.mIndices, triangles);
        EXPECT_FLOAT_EQ(stream.mVertices[1].mPosition[2], 5);
        EXPECT_EQ(stream.mVertices[1].mNormal, (std::array<float, 3>{ 0, 0, 1 }));
    }

    TEST(RenderMeshStream, PreservesSplitPrimitiveSetColorsDuringUpdates)
    {
        auto geometry = triangle();
        geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
        const std::array<osg::Vec4f, 2> values{ { { 1, 0, 0, .25f }, { 0, 1, 0, .5f } } };
        geometry->setColorArray(new osg::Vec4Array(values.begin(), values.end()), osg::Array::BIND_PER_PRIMITIVE_SET);
        RenderAssets::OsgImporter importer;
        auto stream = importer.importMeshStream(*geometry);
        ASSERT_EQ(stream.mVertices.size(), 6u);
        auto* colors = static_cast<osg::Vec4Array*>(geometry->getColorArray());
        (*colors)[0] = osg::Vec4f(0, 0, 1, .75f);
        (*colors)[1] = osg::Vec4f(1, 1, 0, 1);
        importer.updateMeshStream(*geometry, stream);
        EXPECT_EQ(stream.mVertices[0].mColor, (std::array<std::uint8_t, 4>{ 0, 0, 255, 191 }));
        EXPECT_FLOAT_EQ(stream.mVertices[0].mAlpha, .75f);
        EXPECT_EQ(stream.mVertices[3].mColor, (std::array<std::uint8_t, 4>{ 255, 255, 0, 255 }));
    }

    TEST(RenderMeshStream, RetainsIndependentDiffuseAndMaskCoordinates)
    {
        auto geometry = triangle();
        const osg::Vec2f uv(2, 3), mask(.25f, .75f);
        geometry->setTexCoordArray(1, new osg::Vec2Array(4), osg::Array::BIND_PER_VERTEX);
        geometry->setTexCoordArray(0, new osg::Vec2Array(4), osg::Array::BIND_PER_VERTEX);
        auto* diffuse = static_cast<osg::Vec2Array*>(geometry->getTexCoordArray(1));
        auto* blend = static_cast<osg::Vec2Array*>(geometry->getTexCoordArray(0));
        std::fill(diffuse->begin(), diffuse->end(), uv);
        std::fill(blend->begin(), blend->end(), mask);
        RenderAssets::OsgImporter importer;
        auto stream = importer.importMeshStream(*geometry, 1);
        (*diffuse)[0] = osg::Vec2f(4, 5);
        (*blend)[0] = osg::Vec2f(.5f, .5f);
        importer.updateMeshStream(*geometry, stream);
        EXPECT_EQ(stream.mVertices[0].mUV, (std::array<float, 2>{ 4, 5 }));
        EXPECT_EQ(stream.mVertices[0].mMaskUV, (std::array<float, 2>{ .5f, .5f }));
        static_cast<osg::Vec3Array*>(geometry->getVertexArray())->resize(2);
        EXPECT_THROW(importer.updateMeshStream(*geometry, stream), std::invalid_argument);
    }

    TEST(RenderModelImport, PreservesFiniteNormalizedNormalsWithoutRejectingDegenerateFaces)
    {
        auto geometry = triangle(GL_TRIANGLE_STRIP);
        const std::array<osg::Vec3f, 4> normals{ { { 0, 0, 4 }, { 0, 0, 0 },
            { std::numeric_limits<float>::quiet_NaN(), 0, 1 }, { 0, 3, 0 } } };
        geometry->setNormalArray(new osg::Vec3Array(normals.begin(), normals.end()), osg::Array::BIND_PER_VERTEX);
        RenderAssets::OsgImporter importer;
        const auto result = importer.importMesh(*geometry, 0);
        ASSERT_EQ(result->mVertices.size(), 4u);
        for (unsigned i = 0; i < 3; ++i)
            EXPECT_EQ(result->mVertices[i].mNormal, (std::array<float, 3>{ 0, 0, 1 }));
        EXPECT_EQ(result->mVertices[3].mNormal, (std::array<float, 3>{ 0, 1, 0 }));
    }

    TEST(RenderModelImport, PreservesAllAuthoredVertexColorMaterialModes)
    {
        for (const auto& [mode, expected] : { std::pair{ osg::Material::OFF, 0u }, { osg::Material::EMISSION, 1u },
                 { osg::Material::AMBIENT_AND_DIFFUSE, 2u }, { osg::Material::AMBIENT, 3u },
                 { osg::Material::DIFFUSE, 4u }, { osg::Material::SPECULAR, 5u } })
        {
            auto geometry = triangle();
            osg::ref_ptr<osg::Material> material = new osg::Material;
            material->setColorMode(mode);
            geometry->getOrCreateStateSet()->setAttribute(material);
            RenderAssets::OsgImporter importer;
            const auto asset = importer.import(*geometry, "vertex color mode");
            EXPECT_EQ(asset->mParts[0].mMaterial.mVertexColorMode, expected);
            EXPECT_EQ(asset->mParts[0].mMaterial.mUseVertexColor, mode != osg::Material::OFF);
        }
    }

    TEST(RenderModelImport, KeepsLocalGeometryAndFlattensOnlyTheTransformDescription)
    {
        auto geometry = triangle(GL_TRIANGLE_STRIP);
        osg::ref_ptr<osg::MatrixTransform> scale = new osg::MatrixTransform(osg::Matrixd::scale(2, 3, 4));
        scale->addChild(geometry);
        osg::ref_ptr<osg::MatrixTransform> root = new osg::MatrixTransform(osg::Matrixd::translate(10, 20, 30));
        root->addChild(scale);
        RenderAssets::OsgImporter importer;
        const auto result = importer.import(*root, "fixture.nif");
        ASSERT_EQ(result->mParts.size(), 1u);
        const auto& part = result->mParts[0];
        EXPECT_FLOAT_EQ(part.mModelFromLocal[0], 2);
        EXPECT_FLOAT_EQ(part.mModelFromLocal[5], 3);
        EXPECT_FLOAT_EQ(part.mModelFromLocal[12], 10);
        EXPECT_FLOAT_EQ(part.mMesh->mVertices[1].mPosition[0], 1);
        EXPECT_EQ(part.mMesh->mIndices, (std::vector<std::uint32_t>{ 0, 1, 2, 1, 3, 2 }));
        EXPECT_FLOAT_EQ(result->mBounds.mMin[0], 10);
        EXPECT_FLOAT_EQ(result->mBounds.mMax[0], 12);
        EXPECT_FLOAT_EQ(result->mBounds.mMax[1], 23);
        EXPECT_EQ(geometry->getStateSet(), nullptr);
    }

    TEST(RenderModelImport, HonorsOverrideAndProtectedMaterialState)
    {
        auto geometry = triangle();
        osg::ref_ptr<osg::Group> root = new osg::Group;
        root->addChild(geometry);
        root->getOrCreateStateSet()->setAttributeAndModes(new osg::AlphaFunc(osg::AlphaFunc::GREATER, 0.75f),
            osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        geometry->getOrCreateStateSet()->setAttributeAndModes(new osg::AlphaFunc(osg::AlphaFunc::LESS, 0.25f));
        RenderAssets::OsgImporter importer;
        const auto overridden = importer.import(*root, "override");
        EXPECT_EQ(overridden->mParts[0].mMaterial.mAlphaCompare, Render::AlphaCompare::Greater);
        EXPECT_FLOAT_EQ(overridden->mParts[0].mMaterial.mAlphaCutoff, 0.75f);
        geometry->getOrCreateStateSet()->setAttributeAndModes(
            new osg::AlphaFunc(osg::AlphaFunc::LESS, 0.25f), osg::StateAttribute::ON | osg::StateAttribute::PROTECTED);
        const auto protectedState = importer.import(*root, "protected");
        EXPECT_EQ(protectedState->mParts[0].mMaterial.mAlphaCompare, Render::AlphaCompare::Less);
        EXPECT_FLOAT_EQ(protectedState->mParts[0].mMaterial.mAlphaCutoff, 0.25f);
    }

    TEST(RenderModelImport, KeepsDiffuseUVTransformSamplerAndPackedVertexColors)
    {
        auto geometry = triangle();
        const std::array<osg::Vec2f, 4> coords{ { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } } };
        geometry->setTexCoordArray(1, new osg::Vec2Array(coords.begin(), coords.end()));
        const osg::Vec4ub packed(128, 64, 32, 255);
        geometry->setColorArray(new osg::Vec4ubArray(1, &packed), osg::Array::BIND_OVERALL);
        const auto image = texture();
        image->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST_MIPMAP_LINEAR);
        auto* state = geometry->getOrCreateStateSet();
        state->setTextureAttribute(1, image);
        state->setTextureAttribute(1, new SceneUtil::TextureType("diffuseMap"));
        state->setTextureAttribute(1, new osg::TexMat(osg::Matrixd::translate(0.25, 0.5, 0)));
        RenderAssets::OsgImporter importer;
        const auto result = importer.import(*geometry, "uv fixture");
        const auto& part = result->mParts[0];
        ASSERT_TRUE(part.mMaterial.mDiffuse);
        EXPECT_EQ(part.mMaterial.mDiffuse->mSampler.mWrapT, Render::TextureWrap::Clamp);
        EXPECT_EQ(part.mMaterial.mDiffuse->mSampler.mMinFilter, Render::TextureFilter::Nearest);
        EXPECT_EQ(part.mMaterial.mDiffuse->mSampler.mMipFilter, Render::TextureFilter::Linear);
        EXPECT_FLOAT_EQ(part.mMesh->mVertices[0].mUV[0], 0.25f);
        EXPECT_FLOAT_EQ(part.mMesh->mVertices[0].mUV[1], 0.5f);
        EXPECT_EQ(part.mMesh->mVertices[0].mColor, (std::array<std::uint8_t, 4>{ 128, 64, 32, 255 }));
    }

    TEST(RenderModelImport, SkipsInactiveSwitchBranchesAndUpdateOnlyCollisionNodes)
    {
        osg::ref_ptr<osg::Switch> root = new osg::Switch;
        root->addChild(triangle(), true);
        root->addChild(triangle(GL_POINTS), false);
        auto hidden = triangle(GL_POINTS);
        hidden->setNodeMask(1);
        root->addChild(hidden, true);
        RenderAssets::OsgImporter importer(~std::uint32_t{ 1 });
        EXPECT_EQ(importer.import(*root, "visibility fixture")->mParts.size(), 1u);
    }

    TEST(RenderModelImport, KeepsAuthoredAlphaExactForEqualityTests)
    {
        auto geometry = triangle();
        const osg::Vec4f value(1, 1, 1, 0.5f);
        geometry->setColorArray(new osg::Vec4Array(1, &value), osg::Array::BIND_OVERALL);
        geometry->getOrCreateStateSet()->setAttributeAndModes(new osg::AlphaFunc(osg::AlphaFunc::EQUAL, 0.5f));
        RenderAssets::OsgImporter importer;
        const auto result = importer.import(*geometry, "alpha equality");
        EXPECT_FLOAT_EQ(result->mParts[0].mMesh->mVertices[0].mAlpha, 0.5f);
        EXPECT_EQ(result->mParts[0].mMaterial.mAlphaCompare, Render::AlphaCompare::Equal);
        EXPECT_FLOAT_EQ(result->mParts[0].mMaterial.mAlphaCutoff, 0.5f);
    }

    TEST(RenderModelImport, RejectsMalformedTrianglesAndAdditionalShaderOperations)
    {
        auto geometry = triangle();
        geometry->removePrimitiveSet(0);
        osg::ref_ptr<osg::DrawElementsUInt> indices = new osg::DrawElementsUInt(GL_TRIANGLES);
        indices->push_back(0);
        indices->push_back(1);
        geometry->addPrimitiveSet(indices);
        RenderAssets::OsgImporter importer;
        EXPECT_THROW(importer.import(*geometry, "incomplete"), std::runtime_error);
        geometry = triangle();
        geometry->getOrCreateStateSet()->setAttribute(new osg::Program);
        EXPECT_THROW(importer.import(*geometry, "program"), std::runtime_error);
        geometry = triangle();
        geometry->getOrCreateStateSet()->setTextureAttribute(0, texture());
        geometry->getOrCreateStateSet()->setTextureAttribute(0, new SceneUtil::TextureType("normalMap"));
        EXPECT_THROW(importer.import(*geometry, "additional texture"), std::runtime_error);
    }

    TEST(RenderImageImport, RetainsAuthoredCompressedMipBytesAndDecodesOnlyOnDemand)
    {
        osg::ref_ptr<osg::Image> source = new osg::Image;
        auto* bytes = new unsigned char[16]{ 0, 248, 0, 0, 0, 0, 0, 0, 224, 7, 0, 0, 0, 0, 0, 0 };
        source->setImage(4, 4, 1, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_UNSIGNED_BYTE,
            bytes, osg::Image::USE_NEW_DELETE, 1);
        source->setOrigin(osg::Image::TOP_LEFT);
        source->setMipmapLevels(osg::Image::MipmapDataType{ 8 });
        RenderAssets::OsgImporter importer;
        const auto result = importer.importImage(*source);
        EXPECT_EQ(result->mFormat, Render::TextureFormat::BC1RGBA);
        ASSERT_EQ(result->mLevels.size(), 2u);
        EXPECT_EQ(result->mLevels[0].mBytes.size(), 8u);
        EXPECT_EQ(result->mLevels[1].mWidth, 2u);
        EXPECT_EQ(result->mLevels[1].mBytes[0], std::byte{ 224 });
        ASSERT_TRUE(result->mDecode);
        const auto decoded = result->mDecode();
        ASSERT_EQ(decoded->mLevels.size(), 2u);
        EXPECT_EQ(decoded->mLevels[0].mBytes[0], std::byte{ 255 });
        EXPECT_EQ(decoded->mLevels[0].mBytes[1], std::byte{ 0 });
        EXPECT_EQ(decoded->mLevels[1].mBytes[1], std::byte{ 255 });
        EXPECT_EQ(result, importer.importImage(*source));
    }

    TEST(RenderModelImport, KeepsMaskCoordinatesSeparateFromDiffuseUVSelection)
    {
        auto geometry = triangle();
        const std::array<osg::Vec2f, 4> base{ { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } } };
        const std::array<osg::Vec2f, 4> detail{ { { 4, 5 }, { 6, 5 }, { 4, 7 }, { 6, 7 } } };
        geometry->setTexCoordArray(0, new osg::Vec2Array(base.begin(), base.end()));
        geometry->setTexCoordArray(2, new osg::Vec2Array(detail.begin(), detail.end()));
        RenderAssets::OsgImporter importer;
        const auto mesh = importer.importMesh(*geometry, 2);
        EXPECT_EQ(mesh->mVertices[0].mUV, (std::array<float, 2>{ 4, 5 }));
        EXPECT_EQ(mesh->mVertices[0].mMaskUV, (std::array<float, 2>{ 0, 0 }));
    }

    TEST(RenderMaterialState, SelectsTheNamedDiffuseSamplerAndKeepsNormalMapsOutOfColour)
    {
        osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
        state->setTextureAttribute(0, new osg::Texture2D);
        state->addUniform(new osg::Uniform("normalMap", 0));
        EXPECT_FALSE(RenderAssets::textureUnit(*state, "diffuseMap"));
        EXPECT_EQ(RenderAssets::textureUnit(*state, "normalMap"), 0u);
        state->addUniform(new osg::Uniform("diffuseMap", 3));
        EXPECT_EQ(RenderAssets::textureUnit(*state, "diffuseMap"), 3u);
        state->addUniform(new osg::Uniform("blendMap", 1));
        EXPECT_EQ(RenderAssets::textureUnit(*state, "blendMap"), 1u);
    }

    TEST(RenderMaterialState, RetainsShaderTextureMatricesForTerrainTilingAndMasks)
    {
        osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
        osg::Matrixf tiled;
        tiled(0, 0) = 16;
        tiled(1, 1) = 16;
        state->addUniform(new osg::Uniform("texMat0", tiled));
        osg::Matrixf mask;
        mask(0, 0) = .8f;
        mask(1, 1) = .8f;
        mask(3, 0) = .1f;
        mask(3, 1) = .1f;
        state->addUniform(new osg::Uniform("texMat1", mask));
        const std::array<float, 6> expectedTile{ 16, 0, 0, 0, 16, 0 };
        const std::array<float, 6> expectedMask{ .8f, 0, .1f, 0, .8f, .1f };
        EXPECT_EQ(RenderAssets::textureTransform(*state, 0), expectedTile);
        EXPECT_EQ(RenderAssets::textureTransform(*state, 1), expectedMask);
    }

    TEST(RenderImageImport, BorrowsPackedLiveRgbaWithoutCopyingOrFlippingRows)
    {
        osg::ref_ptr<osg::Image> source = new osg::Image;
        source->allocateImage(1, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, 4);
        source->setOrigin(osg::Image::BOTTOM_LEFT);
        source->data()[0] = 17;
        std::vector<Render::TextureLevel> levels;
        const auto view = RenderAssets::OsgImporter::borrowTexture(*source, levels);
        ASSERT_TRUE(view);
        ASSERT_EQ(levels.size(), 1u);
        EXPECT_EQ(levels[0].mBytes.data(), reinterpret_cast<const std::byte*>(source->data()));
        EXPECT_EQ(levels[0].mBytes.size(), 8u);
        EXPECT_EQ(levels[0].mBytes[0], std::byte{ 17 });
        EXPECT_TRUE(view->mGenerateMipmaps);
        source->data()[0] = 23;
        EXPECT_EQ(levels[0].mBytes[0], std::byte{ 23 });
        const auto noMips = RenderAssets::OsgImporter::borrowTexture(*source, levels, false);
        ASSERT_TRUE(noMips);
        EXPECT_FALSE(noMips->mGenerateMipmaps);
    }

    TEST(RenderImageImport, BorrowingFallsBackForConvertedChannelsAndPaddedRows)
    {
        osg::ref_ptr<osg::Image> source = new osg::Image;
        std::vector<Render::TextureLevel> levels;
        source->allocateImage(1, 2, 1, GL_BGR, GL_UNSIGNED_BYTE, 4);
        EXPECT_FALSE(RenderAssets::OsgImporter::borrowTexture(*source, levels));
        source->allocateImage(1, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, 8);
        EXPECT_FALSE(RenderAssets::OsgImporter::borrowTexture(*source, levels));
        EXPECT_TRUE(levels.empty());
    }

    TEST(RenderImageImport, BorrowsCompressedMipBytesAndPreservesSrgb)
    {
        osg::ref_ptr<osg::Image> source = new osg::Image;
        auto* bytes = new unsigned char[24]{};
        source->setImage(4, 4, 1, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
            GL_UNSIGNED_BYTE, bytes, osg::Image::USE_NEW_DELETE);
        source->setMipmapLevels({ 8, 16 });
        std::vector<Render::TextureLevel> levels;
        const auto view = RenderAssets::OsgImporter::borrowTexture(*source, levels);
        ASSERT_TRUE(view);
        ASSERT_EQ(levels.size(), 3u);
        EXPECT_TRUE(view->mSrgb);
        EXPECT_EQ(view->mFormat, Render::TextureFormat::BC1RGBA);
        EXPECT_FALSE(view->mGenerateMipmaps);
        EXPECT_EQ(levels[2].mBytes.data(), reinterpret_cast<const std::byte*>(bytes + 16));
        EXPECT_EQ(levels[2].mWidth, 1u);
        EXPECT_EQ(levels[2].mBytes.size(), 8u);
    }

    TEST(RenderImageImport, CopiesPackedBGRRowsWithoutIncludingPadding)
    {
        osg::ref_ptr<osg::Image> source = new osg::Image;
        source->allocateImage(1, 2, 1, GL_BGR, GL_UNSIGNED_BYTE, 4);
        const std::array<unsigned char, 8> bytes{ 10, 20, 30, 99, 40, 50, 60, 99 };
        std::memcpy(source->data(), bytes.data(), bytes.size());
        RenderAssets::OsgImporter importer;
        const auto result = importer.importImage(*source, true);
        const std::vector<std::byte> expected{ std::byte{ 30 }, std::byte{ 20 }, std::byte{ 10 }, std::byte{ 255 },
            std::byte{ 60 }, std::byte{ 50 }, std::byte{ 40 }, std::byte{ 255 } };
        EXPECT_EQ(result->mLevels[0].mBytes, expected);
    }

    TEST(RenderImageImport, NormalizesOriginAndInvalidatesDirtyImages)
    {
        osg::ref_ptr<osg::Image> source = new osg::Image;
        source->allocateImage(1, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        source->setOrigin(osg::Image::BOTTOM_LEFT);
        source->setColor(osg::Vec4f(1, 0, 0, 1), 0, 0);
        source->setColor(osg::Vec4f(0, 1, 0, 1), 0, 1);
        RenderAssets::OsgImporter importer;
        const auto first = importer.importImage(*source);
        EXPECT_EQ(first->mLevels[0].mBytes[0], std::byte{ 0 });
        EXPECT_EQ(first->mLevels[0].mBytes[1], std::byte{ 255 });
        const auto live = importer.importImage(*source, true);
        EXPECT_EQ(live->mLevels[0].mBytes[0], std::byte{ 255 });
        EXPECT_EQ(live->mLevels[0].mBytes[1], std::byte{ 0 });
        EXPECT_EQ(live, importer.importImage(*source, true));
        EXPECT_EQ(first, importer.importImage(*source));
        source->setColor(osg::Vec4f(0, 0, 1, 1), 0, 1);
        source->dirty();
        const auto second = importer.importImage(*source);
        EXPECT_NE(first, second);
        EXPECT_EQ(second->mLevels[0].mBytes[2], std::byte{ 255 });
        EXPECT_EQ(first->mLevels[0].mBytes[1], std::byte{ 255 });
    }
}
