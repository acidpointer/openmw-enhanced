#ifndef OPENMW_MWRENDER_VULKAN_BACKEND_H
#define OPENMW_MWRENDER_VULKAN_BACKEND_H

#include <components/render/backend.hpp>

#include <memory>

namespace MWRender::Vulkan
{
    std::unique_ptr<Render::Backend> createBackend(const Render::BackendOptions& options);
}

#endif
