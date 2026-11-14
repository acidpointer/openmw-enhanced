#ifndef OPENMW_MWRENDER_VULKAN_MEMORY_H
#define OPENMW_MWRENDER_VULKAN_MEMORY_H

#include <components/render/allocationranges.hpp>

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace MWRender::Vulkan
{
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

    private:
        VkDevice mDevice;
        VkDeviceMemory mMemory = VK_NULL_HANDLE;
        std::byte* mMapped = nullptr;
        std::uint32_t mType;
        MemoryKind mKind;
        Render::AllocationRanges mRanges;
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
        Allocation allocate(const VkMemoryRequirements& requirements, MemoryKind kind,
            const VkMemoryDedicatedAllocateInfo* dedicated = nullptr);
        // Keep one unused block per type/kind for reuse and release excess
        // empty blocks. Call only after retiring completed frame resources.
        void trim();

    private:
        VkDevice mDevice;
        VkPhysicalDeviceMemoryProperties mProperties;
        std::vector<std::shared_ptr<MemoryBlock>> mBlocks;
    };
}

#endif
