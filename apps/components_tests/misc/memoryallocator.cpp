#include <gtest/gtest.h>

#ifdef OPENMW_USE_MIMALLOC
#include <mimalloc.h>

#include <SDL_loadso.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <thread>

namespace
{
    TEST(ProcessAllocator, StandardAndAlignedAllocationsBelongToMimalloc)
    {
        EXPECT_EQ(mi_version(), 30601);
        auto* bytes = std::calloc(128, 1);
        ASSERT_NE(bytes, nullptr);
        ASSERT_TRUE(mi_check_owned(bytes));
        EXPECT_EQ(static_cast<unsigned char*>(bytes)[127], 0);
        std::free(bytes);

        void* aligned = nullptr;
        ASSERT_EQ(posix_memalign(&aligned, 256, 1024), 0);
        ASSERT_TRUE(mi_check_owned(aligned));
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(aligned) % 256, 0u);
        std::free(aligned);

        auto value = std::make_unique<int>(42);
        EXPECT_TRUE(mi_check_owned(value.get()));
        EXPECT_EQ(*value, 42);
    }

    TEST(ProcessAllocator, ModuleAndWorkerAllocationsCanBeFreedAcrossBoundaries)
    {
        const std::unique_ptr<void, decltype(&SDL_UnloadObject)> module(
            SDL_LoadObject(OPENMW_ALLOCATOR_PROBE_PATH), &SDL_UnloadObject);
        ASSERT_NE(module, nullptr) << SDL_GetError();
        const auto allocate
            = reinterpret_cast<void* (*)(std::size_t)>(SDL_LoadFunction(module.get(), "allocatorProbeAllocate"));
        const auto resize
            = reinterpret_cast<void* (*)(void*, std::size_t)>(SDL_LoadFunction(module.get(), "allocatorProbeResize"));
        const auto release = reinterpret_cast<void (*)(void*)>(SDL_LoadFunction(module.get(), "allocatorProbeRelease"));
        const auto alignedNew
            = reinterpret_cast<void* (*)(std::size_t)>(SDL_LoadFunction(module.get(), "allocatorProbeAlignedNew"));
        const auto alignedDelete
            = reinterpret_cast<void (*)(void*)>(SDL_LoadFunction(module.get(), "allocatorProbeAlignedDelete"));
        ASSERT_TRUE(allocate && resize && release && alignedNew && alignedDelete);

        void* pointer = nullptr;
        std::thread worker([&] { pointer = allocate(4096); });
        worker.join();
        ASSERT_NE(pointer, nullptr);
        ASSERT_TRUE(mi_check_owned(pointer));
        std::memset(pointer, 0x5a, 4096);
        auto* grown = static_cast<unsigned char*>(resize(pointer, 8192));
        ASSERT_NE(grown, nullptr);
        ASSERT_TRUE(mi_check_owned(grown));
        EXPECT_EQ(grown[0], 0x5a);
        EXPECT_EQ(grown[4095], 0x5a);
        std::free(grown);

        void* host = std::malloc(256);
        ASSERT_NE(host, nullptr);
        ASSERT_TRUE(mi_check_owned(host));
        release(host);

        void* aligned = alignedNew(512);
        ASSERT_NE(aligned, nullptr);
        ASSERT_TRUE(mi_check_owned(aligned));
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(aligned) % 64, 0u);
        ::operator delete(aligned, std::align_val_t(64));
        aligned = ::operator new(512, std::align_val_t(64));
        alignedDelete(aligned);
    }
}
#endif
