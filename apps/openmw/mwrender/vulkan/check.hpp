#ifndef OPENMW_MWRENDER_VULKAN_CHECK_H
#define OPENMW_MWRENDER_VULKAN_CHECK_H

#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>

namespace MWRender::Vulkan
{
    template <class T>
    T vkInfo(VkStructureType type, const void* next = nullptr)
    {
        T value{};
        value.sType = type;
        value.pNext = const_cast<void*>(next);
        return value;
    }

    inline void check(VkResult result, const char* operation)
    {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(operation) + " failed (Vulkan result " + std::to_string(result) + ")");
    }
}

#endif
