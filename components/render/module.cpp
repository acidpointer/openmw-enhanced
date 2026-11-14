#include "module.hpp"

#include <SDL_loadso.h>

#include <array>
#include <stdexcept>

namespace Render
{
    std::unique_ptr<BackendModule> BackendModule::load(const std::filesystem::path& path, const BackendOptions& options)
    {
        std::unique_ptr<BackendModule> result(new BackendModule);
        const auto name = path.u8string();
        result->mLibrary = SDL_LoadObject(reinterpret_cast<const char*>(name.c_str()));
        if (!result->mLibrary)
            throw std::runtime_error("Cannot load renderer module: " + std::string(SDL_GetError()));
        const auto create
            = reinterpret_cast<CreateBackend>(SDL_LoadFunction(result->mLibrary, "openmwCreateRendererV1"));
        result->mDestroy
            = reinterpret_cast<DestroyBackend>(SDL_LoadFunction(result->mLibrary, "openmwDestroyRendererV1"));
        if (!create || !result->mDestroy)
            throw std::runtime_error("Renderer module has no compatible OpenMW entry points");
        std::array<char, 2048> error{};
        result->mBackend = create(BackendAbi, &options, error.data(), error.size());
        if (!result->mBackend)
        {
            error.back() = '\0';
            throw std::runtime_error("Cannot initialize renderer module: " + std::string(error.data()));
        }
        return result;
    }

    BackendModule::~BackendModule()
    {
        if (mBackend)
            mDestroy(mBackend);
        if (mLibrary)
            SDL_UnloadObject(mLibrary);
    }
}
