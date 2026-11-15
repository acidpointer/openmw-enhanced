// A separate shared object must use the executable's heap, not another static
// mimalloc instance. Keep this independent of the components/allocator targets.
#include <cstddef>
#include <cstdlib>
#include <new>

#define PROBE_EXPORT extern "C" __attribute__((visibility("default")))

PROBE_EXPORT void* allocatorProbeAllocate(std::size_t bytes)
{
    return std::malloc(bytes);
}

PROBE_EXPORT void* allocatorProbeResize(void* pointer, std::size_t bytes)
{
    return std::realloc(pointer, bytes);
}

PROBE_EXPORT void allocatorProbeRelease(void* pointer)
{
    std::free(pointer);
}

PROBE_EXPORT void* allocatorProbeAlignedNew(std::size_t bytes)
{
    return ::operator new(bytes, std::align_val_t(64), std::nothrow);
}

PROBE_EXPORT void allocatorProbeAlignedDelete(void* pointer)
{
    ::operator delete(pointer, std::align_val_t(64));
}
