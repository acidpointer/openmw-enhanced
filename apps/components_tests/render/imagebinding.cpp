#include <components/renderassets/imagebinding.hpp>

#include <gtest/gtest.h>

namespace
{
    TEST(ResidentImageBinding, RevisionIdentityAndSamplerChangesInvalidateTheBinding)
    {
        RenderAssets::ImageBinding<int> binding;
        osg::ref_ptr<osg::Image> image = new osg::Image, other = new osg::Image;
        Render::SamplerState sampling;
        const auto storage = std::make_shared<int>(1);
        EXPECT_FALSE(binding.find(*image, sampling));
        binding.store(*image, sampling, storage);
        EXPECT_EQ(binding.find(*image, sampling), storage);
        EXPECT_FALSE(binding.find(*other, sampling));
        auto changed = sampling;
        changed.mWrapS = Render::TextureWrap::Clamp;
        EXPECT_FALSE(binding.find(*image, changed));
        image->dirty();
        EXPECT_FALSE(binding.find(*image, sampling));
        binding.store(*image, changed, storage);
        EXPECT_EQ(binding.find(*image, changed), storage);
    }

    TEST(ResidentImageBinding, CacheDoesNotPinGpuStorageOrDecodedImages)
    {
        RenderAssets::ImageBinding<int> binding;
        osg::ref_ptr<osg::Image> image = new osg::Image;
        Render::SamplerState sampling;
        auto storage = std::make_shared<int>(1);
        binding.store(*image, sampling, storage);
        EXPECT_EQ(storage.use_count(), 1);
        storage.reset();
        EXPECT_FALSE(binding.find(*image, sampling));
        EXPECT_EQ(image->referenceCount(), 1);
        image = nullptr;
        osg::ref_ptr<osg::Image> replacement = new osg::Image;
        EXPECT_FALSE(binding.find(*replacement, sampling));
    }
}
