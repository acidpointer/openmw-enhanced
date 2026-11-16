#include "memory.hpp"

#include "check.hpp"

#include <components/render/memorypolicy.hpp>

#include <algorithm>
#include <cstddef>
#include <set>
#include <utility>

namespace MWRender::Vulkan
{
    MemoryBlock::MemoryBlock(VkDevice device, VkDeviceSize size, std::uint32_t type, MemoryKind kind,
        const VkMemoryDedicatedAllocateInfo* dedicated)
        : mDevice(device)
        , mType(type)
        , mKind(kind)
        , mRanges(size)
    {
        auto info = vkInfo<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        info.pNext = dedicated;
        info.allocationSize = size;
        info.memoryTypeIndex = type;
        const auto allocated = vkAllocateMemory(mDevice, &info, nullptr, &mMemory);
        if (allocated != VK_SUCCESS)
            throw AllocationFailure(allocated, "Allocate Vulkan memory");
        if (kind == MemoryKind::Upload)
        {
            void* mapped = nullptr;
            const VkResult result = vkMapMemory(mDevice, mMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
            if (result != VK_SUCCESS)
            {
                vkFreeMemory(mDevice, mMemory, nullptr);
                mMemory = VK_NULL_HANDLE;
                throw AllocationFailure(result, "Map Vulkan upload memory");
            }
            mMapped = static_cast<std::byte*>(mapped);
        }
    }

    MemoryBlock::~MemoryBlock()
    {
        if (mMapped)
            vkUnmapMemory(mDevice, mMemory);
        vkFreeMemory(mDevice, mMemory, nullptr);
    }

    std::optional<VkDeviceSize> MemoryBlock::allocate(VkDeviceSize size, VkDeviceSize alignment)
    {
        return mRanges.allocate(size, alignment);
    }

    void MemoryBlock::release(VkDeviceSize offset) noexcept
    {
        if (!mShuttingDown)
            mRanges.release(offset);
    }

    bool MemoryBlock::empty() const
    {
        return mRanges.empty();
    }

    Allocation::Allocation(std::shared_ptr<MemoryBlock> block, VkDeviceSize offset)
        : mBlock(std::move(block))
        , mOffset(offset)
    {
    }

    Allocation::~Allocation()
    {
        if (mBlock)
            mBlock->release(mOffset);
    }

    Allocation::Allocation(Allocation&& other) noexcept
        : mBlock(std::move(other.mBlock))
        , mOffset(other.mOffset)
    {
    }

    Allocation& Allocation::operator=(Allocation&& other) noexcept
    {
        if (this != &other)
        {
            if (mBlock)
                mBlock->release(mOffset);
            mBlock = std::move(other.mBlock);
            mOffset = other.mOffset;
        }
        return *this;
    }

    MemoryAllocator::MemoryAllocator(VkDevice device, const VkPhysicalDeviceMemoryProperties& properties)
        : mDevice(device)
        , mProperties(properties)
    {
        VkDeviceSize local = 0;
        for (std::uint32_t heap = 0; heap < properties.memoryHeapCount; ++heap)
            if (properties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                local = std::max(local, properties.memoryHeaps[heap].size);
        mImageBlockSize = Render::imageMemoryBlockSize(local);
    }
    void MemoryAllocator::beginShutdown()
    {
        for (const auto& block : mBlocks)
            block->beginShutdown();
    }

    Allocation MemoryAllocator::allocate(
        const VkMemoryRequirements& requirements, MemoryKind kind, const VkMemoryDedicatedAllocateInfo* dedicated)
    {
        const VkMemoryPropertyFlags flags = kind == MemoryKind::Upload
            ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
            : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        std::uint32_t type = 0;
        for (; type < mProperties.memoryTypeCount; ++type)
            if ((requirements.memoryTypeBits & (1u << type))
                && (mProperties.memoryTypes[type].propertyFlags & flags) == flags)
                break;
        if (type == mProperties.memoryTypeCount)
            throw std::runtime_error("No compatible Vulkan memory type for rendering resource");

        if (!dedicated)
            for (const auto& block : mBlocks)
                if (block->type() == type && block->kind() == kind)
                    if (const auto offset = block->allocate(requirements.size, requirements.alignment))
                        return Allocation(block, *offset);

        const VkDeviceSize pageSize = preferredBlockSize(kind);
        const VkDeviceSize size = dedicated ? requirements.size : std::max(requirements.size, pageSize);
        std::shared_ptr<MemoryBlock> block;
        try
        {
            block = std::make_shared<MemoryBlock>(mDevice, size, type, kind, dedicated);
        }
        catch (const AllocationFailure& error)
        {
            // A preferred pool must not reject an image that still fits exactly.
            if (dedicated || size == requirements.size || !error.retryable())
                throw;
            block = std::make_shared<MemoryBlock>(mDevice, requirements.size, type, kind, nullptr);
        }
        const auto offset = block->allocate(requirements.size, requirements.alignment);
        if (!offset)
            throw std::logic_error("A new Vulkan memory block cannot fit its first allocation");
        if (!dedicated)
            mBlocks.push_back(block);
        return Allocation(std::move(block), *offset);
    }

    void MemoryAllocator::trim()
    {
        std::set<std::pair<std::uint32_t, MemoryKind>> retained;
        std::erase_if(mBlocks, [&](const auto& block) {
            return block->empty() && !retained.emplace(block->type(), block->kind()).second;
        });
    }
}
