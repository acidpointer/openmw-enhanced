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
