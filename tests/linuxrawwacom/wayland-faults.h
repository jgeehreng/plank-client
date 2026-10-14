// Test-only constructor failures. Include before the production translation
// units: all successful calls still use the real Wayland client/server ABI.
#pragma once
#include <wayland-client.h>
#include <cstring>

enum class WaylandFault { None, Pool, Buffer, Seat, Pointer };
inline WaylandFault waylandFault = WaylandFault::None;
inline unsigned waylandFaultsInjected = 0;

inline bool failWayland(WaylandFault kind)
{
    if (waylandFault != kind) return false;
    ++waylandFaultsInjected;
    return true;
}

inline wl_shm_pool* testShmPool(wl_shm* shm, int fd, int32_t size)
{
    return failWayland(WaylandFault::Pool) ? nullptr : wl_shm_create_pool(shm, fd, size);
}
inline wl_buffer* testBuffer(wl_shm_pool* pool, int32_t offset, int32_t width,
                            int32_t height, int32_t stride, uint32_t format)
{
    return failWayland(WaylandFault::Buffer) ? nullptr :
        wl_shm_pool_create_buffer(pool, offset, width, height, stride, format);
}
inline void* testRegistryBind(wl_registry* registry, uint32_t name,
                             const wl_interface* interface, uint32_t version)
{
    if (std::strcmp(interface->name, "wl_seat") == 0 && failWayland(WaylandFault::Seat))
        return nullptr;
    return wl_registry_bind(registry, name, interface, version);
}
inline wl_pointer* testPointer(wl_seat* seat)
{
    return failWayland(WaylandFault::Pointer) ? nullptr : wl_seat_get_pointer(seat);
}

#define wl_shm_create_pool testShmPool
#define wl_shm_pool_create_buffer testBuffer
#define wl_registry_bind testRegistryBind
#define wl_seat_get_pointer testPointer
