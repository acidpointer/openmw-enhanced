#include <components/render/uploadpages.hpp>

#include <gtest/gtest.h>

#include <limits>

namespace
{
    TEST(RenderUploadPages, PacksAlignedSlicesWithoutOverlappingLiveUploads)
    {
        Render::UploadPages<int> pages(64);
        int created = 0;
        const auto create = [&](std::uint64_t) { return std::make_shared<int>(++created); };
        auto a = pages.allocate(7, 16, create);
        auto b = pages.allocate(3, 16, create);
        EXPECT_EQ(a.mPage, b.mPage);
        EXPECT_EQ(a.mOffset, 0u);
        EXPECT_EQ(b.mOffset, 16u);
        auto c = pages.allocate(48, 16, create);
        EXPECT_NE(c.mPage, a.mPage);
        EXPECT_EQ(created, 2);
    }

    TEST(RenderUploadPages, RetainedSubmissionPreventsReuseUntilCompletion)
    {
        Render::UploadPages<int> pages(64);
        int created = 0;
        const auto create = [&](std::uint64_t) { return std::make_shared<int>(++created); };
        auto pending = pages.allocate(64, 16, create);
        auto submitted = std::move(pending);
        auto next = pages.allocate(64, 16, create);
        EXPECT_NE(next.mPage, submitted.mPage);
        const auto firstPage = submitted.mPage.get();
        submitted = {};
        auto recycled = pages.allocate(64, 16, create);
        EXPECT_EQ(recycled.mPage.get(), firstPage);
        EXPECT_EQ(recycled.mOffset, 0u);
        EXPECT_EQ(created, 2);
    }

    TEST(RenderUploadPages, CancellingOneSliceDoesNotOverwriteOtherPendingData)
    {
        Render::UploadPages<int> pages(64);
        const auto create = [](std::uint64_t) { return std::make_shared<int>(0); };
        auto cancelled = pages.allocate(16, 16, create);
        auto pending = pages.allocate(16, 16, create);
        cancelled = {};
        auto replacement = pages.allocate(16, 16, create);
        EXPECT_EQ(replacement.mPage, pending.mPage);
        EXPECT_EQ(replacement.mOffset, 32u);
    }

    TEST(RenderUploadPages, TrimmingKeepsSubmittedPagesAndReleasesIdleOversizedPages)
    {
        Render::UploadPages<int> pages(64);
        const auto create = [](std::uint64_t capacity) { return std::make_shared<int>(static_cast<int>(capacity)); };
        auto submitted = pages.allocate(128, 16, create);
        std::weak_ptr<int> large = submitted.mPage;
        pages.trim();
        EXPECT_FALSE(large.expired());
        submitted = {};
        pages.trim();
        EXPECT_TRUE(large.expired());
        auto small = pages.allocate(8, 16, create);
        std::weak_ptr<int> cached = small.mPage;
        small = {};
        pages.trim();
        EXPECT_FALSE(cached.expired());
        pages.clear();
        EXPECT_TRUE(cached.expired());
    }

    TEST(RenderUploadPages, RejectsInvalidInputsAndHandlesAddressSpaceBoundaries)
    {
        Render::UploadPages<int> pages(64);
        const auto create = [](std::uint64_t) { return std::make_shared<int>(0); };
        EXPECT_THROW(pages.allocate(0, 16, create), std::invalid_argument);
        EXPECT_THROW(pages.allocate(8, 0, create), std::invalid_argument);
        auto a = pages.allocate(std::numeric_limits<std::uint64_t>::max(), 16, create);
        auto b = pages.allocate(1, 16, create);
        EXPECT_NE(a.mPage, b.mPage);
        EXPECT_EQ(b.mOffset, 0u);
        EXPECT_THROW(pages.allocate(64, 16, [](auto) { return std::shared_ptr<int>{}; }), std::runtime_error);
    }
}
