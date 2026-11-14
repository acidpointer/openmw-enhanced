#ifndef OPENMW_COMPONENTS_RENDER_MODULE_H
#define OPENMW_COMPONENTS_RENDER_MODULE_H

#include "backend.hpp"

#include <memory>

namespace Render
{
    // This owner outlives all calls into the backend. Keeping the library open
    // until after destruction also keeps resource deleters and vtables valid.
    class BackendModule
    {
    public:
        static std::unique_ptr<BackendModule> load(const std::filesystem::path& path, const BackendOptions& options);
        ~BackendModule();
        BackendModule(const BackendModule&) = delete;
        BackendModule& operator=(const BackendModule&) = delete;

        Backend& backend() const { return *mBackend; }

    private:
        BackendModule() = default;
        void* mLibrary = nullptr;
        Backend* mBackend = nullptr;
        DestroyBackend mDestroy = nullptr;
    };
}

#endif
