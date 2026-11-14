#include "backend.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <stdexcept>

#if defined(_WIN32)
#define OPENMW_RENDER_EXPORT __declspec(dllexport)
#else
#define OPENMW_RENDER_EXPORT __attribute__((visibility("default")))
#endif

extern "C" OPENMW_RENDER_EXPORT Render::Backend* openmwCreateRendererV1(
    std::uint32_t abi, const Render::BackendOptions* options, char* error, std::size_t errorSize)
{
    const auto report = [&](const char* message) {
        if (error && errorSize)
        {
            const auto length = std::min(std::strlen(message), errorSize - 1);
            std::memcpy(error, message, length);
            error[length] = '\0';
        }
    };
    if (error && errorSize)
        error[0] = '\0';
    try
    {
        if (abi != Render::BackendAbi || !options)
            throw std::invalid_argument("Incompatible OpenMW renderer module ABI");
        return MWRender::Vulkan::createBackend(*options).release();
    }
    catch (const std::exception& exception)
    {
        report(exception.what());
    }
    catch (...)
    {
        report("Unknown error while creating Vulkan renderer");
    }
    return nullptr;
}

extern "C" OPENMW_RENDER_EXPORT void openmwDestroyRendererV1(Render::Backend* backend)
{
    delete backend;
}
