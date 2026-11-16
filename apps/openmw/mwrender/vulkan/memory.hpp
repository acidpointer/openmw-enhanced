#ifndef OPENMW_MWRENDER_VULKAN_MEMORY_H
#define OPENMW_MWRENDER_VULKAN_MEMORY_H

#include <components/render/allocationranges.hpp>

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace MWRender::Vulkan
{
    class AllocationFailure : public std::runtime_error
    {
    public:
        AllocationFailure(VkResult result, const char* operation)
            : std::runtime_error(std::string(operation) + " failed (Vulkan result " + std::to_string(result) + ")")
            , mResult(result)
        {
        }
        bool retryable() const
        {
            return mResult == VK_ERROR_OUT_OF_DEVICE_MEMORY || mResult == VK_ERROR_OUT_OF_HOST_MEMORY
                || mResult == VK_ERROR_MEMORY_MAP_FAILED;
        }

    private:
        VkResult mResult;
    };

    // Keep linear buffers and optimal images in separate blocks, satisfying
    // bufferImageGranularity without padding every small mesh allocation.
    enum class MemoryKind
    {
        Upload,
        Geometry,
        Image,
    };

    class MemoryBlock
    {
    public:
        MemoryBlock(VkDevice device, VkDeviceSize size, std::uint32_t type, MemoryKind kind,
            const VkMemoryDedicatedAllocateInfo* dedicated);
        ~MemoryBlock();
        MemoryBlock(const MemoryBlock&) = delete;
        MemoryBlock& operator=(const MemoryBlock&) = delete;

        std::optional<VkDeviceSize> allocate(VkDeviceSize size, VkDeviceSize alignment);
        void release(VkDeviceSize offset) noexcept;
        VkDeviceMemory memory() const { return mMemory; }
        std::byte* mapped() const { return mMapped; }
        std::uint32_t type() const { return mType; }
        MemoryKind kind() const { return mKind; }
        bool empty() const;
        void beginShutdown() { mShuttingDown = true; }

    private:
        VkDevice mDevice;
        VkDeviceMemory mMemory = VK_NULL_HANDLE;
        std::byte* mMapped = nullptr;
        std::uint32_t mType;
        MemoryKind mKind;
        Render::AllocationRanges mRanges;
        bool mShuttingDown = false;
    };

    class Allocation
    {
    public:
        Allocation() = default;
        Allocation(std::shared_ptr<MemoryBlock> block, VkDeviceSize offset);
        ~Allocation();
        Allocation(Allocation&& other) noexcept;
        Allocation& operator=(Allocation&& other) noexcept;
        Allocation(const Allocation&) = delete;
        Allocation& operator=(const Allocation&) = delete;

        VkDeviceMemory memory() const { return mBlock->memory(); }
        VkDeviceSize offset() const { return mOffset; }
        std::byte* mapped() const { return mBlock->mapped() + mOffset; }

    private:
        std::shared_ptr<MemoryBlock> mBlock;
        VkDeviceSize mOffset = 0;
    };

    class MemoryAllocator
    {
    public:
        MemoryAllocator(VkDevice device, const VkPhysicalDeviceMemoryProperties& properties);
        static constexpr VkDeviceSize blockSize(MemoryKind kind)
        {
            return kind == MemoryKind::Upload ? 4 * 1024 * 1024 : 16 * 1024 * 1024;
        }
        VkDeviceSize preferredBlockSize(MemoryKind kind) const
        {
            return kind == MemoryKind::Image ? mImageBlockSize : blockSize(kind);
        }
        Allocation allocate(const VkMemoryRequirements& requirements, MemoryKind kind,
            const VkMemoryDedicatedAllocateInfo* dedicated = nullptr);
        // Keep one unused block per type/kind for reuse and release excess
        // empty blocks. Call only after retiring completed frame resources.
        void trim();
        void beginShutdown();

    private:
        VkDevice mDevice;
        VkPhysicalDeviceMemoryProperties mProperties;
        VkDeviceSize mImageBlockSize = blockSize(MemoryKind::Image);
        std::vector<std::shared_ptr<MemoryBlock>> mBlocks;
    };
}

#endif
