#include "plankwaylandcursor.h"

#ifdef HAS_WAYLAND

#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

namespace {

int createAnonymousFile(size_t size)
{
    const int fd = static_cast<int>(syscall(
            SYS_memfd_create, "plank-wacom-cursor", MFD_CLOEXEC));
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    return fd;
}

} // namespace

class PlankWaylandCursor::Impl
{
public:
    struct Buffer {
        wl_buffer* object = nullptr;
        void* mapping = MAP_FAILED;
        size_t size = 0;
        bool released = false;
    };

    explicit Impl(SDL_Window* parentWindow)
    {
        const SDL_PropertiesID properties = SDL_GetWindowProperties(parentWindow);
        m_Display = static_cast<wl_display*>(SDL_GetPointerProperty(
                properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
        m_ParentSurface = static_cast<wl_surface*>(SDL_GetPointerProperty(
                properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
    }

    ~Impl()
    {
        if (m_Subsurface != nullptr) {
            wl_subsurface_destroy(m_Subsurface);
        }
        if (m_Surface != nullptr) {
            wl_surface_destroy(m_Surface);
        }
        if (m_AnchorSubsurface != nullptr) {
            wl_subsurface_destroy(m_AnchorSubsurface);
        }
        if (m_AnchorSurface != nullptr) {
            wl_surface_destroy(m_AnchorSurface);
        }
        for (Buffer* buffer : m_Buffers) {
            destroyBuffer(buffer);
        }
        if (m_Shm != nullptr) {
            wl_shm_destroy(m_Shm);
        }
        if (m_Subcompositor != nullptr) {
            wl_subcompositor_destroy(m_Subcompositor);
        }
        if (m_Compositor != nullptr) {
            wl_compositor_destroy(m_Compositor);
        }
        if (m_Registry != nullptr) {
            wl_registry_destroy(m_Registry);
        }
        if (m_Queue != nullptr) {
            wl_event_queue_destroy(m_Queue);
        }
    }

    bool initialize()
    {
        if (m_Display == nullptr || m_ParentSurface == nullptr) {
            return false;
        }

        m_Queue = wl_display_create_queue(m_Display);
        m_Registry = wl_display_get_registry(m_Display);
        if (m_Queue == nullptr || m_Registry == nullptr) {
            return false;
        }
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(m_Registry), m_Queue);
        wl_registry_add_listener(m_Registry, &RegistryListener, this);
        if (wl_display_roundtrip_queue(m_Display, m_Queue) < 0 ||
                m_Compositor == nullptr || m_Subcompositor == nullptr ||
                m_Shm == nullptr) {
            return false;
        }

        // Subsurface positions are applied by their parent's commit, even in
        // desynchronized mode. Own that parent: a fixed, transparent 1x1
        // anchor lets cursor updates commit independently of video swaps.
        // Its children are not clipped to the anchor's buffer bounds.
        m_AnchorSurface = wl_compositor_create_surface(m_Compositor);
        m_Surface = wl_compositor_create_surface(m_Compositor);
        if (m_AnchorSurface == nullptr || m_Surface == nullptr) {
            return false;
        }
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(m_AnchorSurface), m_Queue);
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(m_Surface), m_Queue);
        m_AnchorSubsurface = wl_subcompositor_get_subsurface(
                m_Subcompositor, m_AnchorSurface, m_ParentSurface);
        m_Subsurface = wl_subcompositor_get_subsurface(
                m_Subcompositor, m_Surface, m_AnchorSurface);
        if (m_AnchorSubsurface == nullptr || m_Subsurface == nullptr) {
            return false;
        }
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(m_AnchorSubsurface), m_Queue);
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(m_Subsurface), m_Queue);
        wl_subsurface_set_desync(m_AnchorSubsurface);
        // Keep the cursor synchronized with our anchor so image, hotspot and
        // position become visible together. Never commit SDL's video surface.

        wl_region* emptyRegion = wl_compositor_create_region(m_Compositor);
        if (emptyRegion == nullptr) {
            return false;
        }
        wl_surface_set_input_region(m_Surface, emptyRegion);
        wl_surface_set_input_region(m_AnchorSurface, emptyRegion);
        wl_region_destroy(emptyRegion);
        QImage transparentPixel(1, 1, QImage::Format_ARGB32_Premultiplied);
        transparentPixel.fill(Qt::transparent);
        Buffer* anchorBuffer = createBuffer(transparentPixel);
        if (anchorBuffer == nullptr) {
            return false;
        }
        wl_surface_attach(m_AnchorSurface, anchorBuffer->object, 0, 0);
        wl_surface_damage(m_AnchorSurface, 0, 0, 1, 1);
        wl_surface_commit(m_Surface);
        wl_surface_commit(m_AnchorSurface);
        wl_display_flush(m_Display);
        return true;
    }

    void dispatchPending()
    {
        if (m_Display != nullptr && m_Queue != nullptr) {
            wl_display_dispatch_queue_pending(m_Display, m_Queue);
            collectReleasedBuffers();
        }
    }

    bool isAttachedTo(SDL_Window* parentWindow) const
    {
        const SDL_PropertiesID properties = SDL_GetWindowProperties(parentWindow);
        return m_ParentSurface == static_cast<wl_surface*>(SDL_GetPointerProperty(
                    properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER,
                    nullptr));
    }

    void setImage(const QImage& source, int hotspotX, int hotspotY)
    {
        m_Image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        m_HotspotX = std::clamp(hotspotX, 0, std::max(0, m_Image.width() - 1));
        m_HotspotY = std::clamp(hotspotY, 0, std::max(0, m_Image.height() - 1));
        updatePosition();
        if (m_Visible) {
            publishImage();
        }
    }

    void setPosition(int hotspotX, int hotspotY)
    {
        m_PositionX = hotspotX;
        m_PositionY = hotspotY;
        m_HasPosition = true;
        updatePosition();
        wl_surface_commit(m_AnchorSurface);
        wl_display_flush(m_Display);
    }

    void setVisible(bool visible)
    {
        if (m_Visible == visible || m_Surface == nullptr) {
            return;
        }
        m_Visible = visible;
        if (!m_Visible) {
            wl_surface_attach(m_Surface, nullptr, 0, 0);
            wl_surface_commit(m_Surface);
            wl_surface_commit(m_AnchorSurface);
        } else if (!m_Image.isNull() && m_HasPosition) {
            publishImage();
        }
        wl_display_flush(m_Display);
    }

private:
    static void registryGlobal(void* data, wl_registry* registry,
                               uint32_t name, const char* interface,
                               uint32_t version)
    {
        auto* self = static_cast<Impl*>(data);
        if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
            self->m_Compositor = static_cast<wl_compositor*>(wl_registry_bind(
                    registry, name, &wl_compositor_interface,
                    std::min(version, 4u)));
        } else if (std::strcmp(interface,
                               wl_subcompositor_interface.name) == 0) {
            self->m_Subcompositor = static_cast<wl_subcompositor*>(
                    wl_registry_bind(registry, name,
                                     &wl_subcompositor_interface, 1));
        } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
            self->m_Shm = static_cast<wl_shm*>(wl_registry_bind(
                    registry, name, &wl_shm_interface, 1));
        }
    }

    static void registryRemove(void*, wl_registry*, uint32_t) {}

    static void bufferRelease(void* data, wl_buffer*)
    {
        static_cast<Buffer*>(data)->released = true;
    }

    Buffer* createBuffer(const QImage& image)
    {
        const int stride = image.bytesPerLine();
        const size_t size = static_cast<size_t>(stride) * image.height();
        const int fd = createAnonymousFile(size);
        if (fd < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to create Wayland Wacom cursor shared memory: %s",
                        std::strerror(errno));
            return nullptr;
        }
        void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, 0);
        if (mapping == MAP_FAILED) {
            close(fd);
            return nullptr;
        }
        std::memcpy(mapping, image.constBits(), size);

        wl_shm_pool* pool = wl_shm_create_pool(m_Shm, fd,
                                                static_cast<int32_t>(size));
        if (pool == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to create Wayland Wacom cursor SHM pool");
            close(fd);
            munmap(mapping, size);
            return nullptr;
        }
        wl_buffer* object = wl_shm_pool_create_buffer(
                pool, 0, image.width(), image.height(), stride,
                WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        if (object == nullptr) {
            munmap(mapping, size);
            return nullptr;
        }

        Buffer* buffer = new Buffer{object, mapping, size, false};
        wl_buffer_add_listener(object, &BufferListener, buffer);
        m_Buffers.push_back(buffer);
        return buffer;
    }

    static void destroyBuffer(Buffer* buffer)
    {
        if (buffer->object != nullptr) {
            wl_buffer_destroy(buffer->object);
        }
        if (buffer->mapping != MAP_FAILED) {
            munmap(buffer->mapping, buffer->size);
        }
        delete buffer;
    }

    void collectReleasedBuffers()
    {
        auto it = m_Buffers.begin();
        while (it != m_Buffers.end()) {
            if ((*it)->released) {
                destroyBuffer(*it);
                it = m_Buffers.erase(it);
            } else {
                ++it;
            }
        }
    }

    void updatePosition()
    {
        if (m_Subsurface != nullptr && m_HasPosition) {
            wl_subsurface_set_position(
                    m_Subsurface,
                    m_PositionX - m_HotspotX,
                    m_PositionY - m_HotspotY);
        }
    }

    void publishImage()
    {
        if (m_Image.isNull() || !m_HasPosition) {
            return;
        }
        collectReleasedBuffers();
        Buffer* buffer = createBuffer(m_Image);
        if (buffer == nullptr) {
            return;
        }
        wl_surface_attach(m_Surface, buffer->object, 0, 0);
        wl_surface_damage(m_Surface, 0, 0, m_Image.width(), m_Image.height());
        wl_surface_commit(m_Surface);
        wl_surface_commit(m_AnchorSurface);
        wl_display_flush(m_Display);
    }

    static const wl_registry_listener RegistryListener;
    static const wl_buffer_listener BufferListener;

    wl_display* m_Display = nullptr;
    wl_surface* m_ParentSurface = nullptr;
    wl_surface* m_AnchorSurface = nullptr;
    wl_subsurface* m_AnchorSubsurface = nullptr;
    wl_event_queue* m_Queue = nullptr;
    wl_registry* m_Registry = nullptr;
    wl_compositor* m_Compositor = nullptr;
    wl_subcompositor* m_Subcompositor = nullptr;
    wl_shm* m_Shm = nullptr;
    wl_surface* m_Surface = nullptr;
    wl_subsurface* m_Subsurface = nullptr;
    std::vector<Buffer*> m_Buffers;
    QImage m_Image;
    int m_HotspotX = 0;
    int m_HotspotY = 0;
    int m_PositionX = 0;
    int m_PositionY = 0;
    bool m_HasPosition = false;
    bool m_Visible = false;
};

const wl_registry_listener PlankWaylandCursor::Impl::RegistryListener = {
    registryGlobal,
    registryRemove,
};

const wl_buffer_listener PlankWaylandCursor::Impl::BufferListener = {
    bufferRelease,
};

#else

class PlankWaylandCursor::Impl {};

#endif

PlankWaylandCursor::PlankWaylandCursor(
        std::unique_ptr<Impl> impl)
    : m_Impl(std::move(impl))
{
}

PlankWaylandCursor::~PlankWaylandCursor() = default;

std::unique_ptr<PlankWaylandCursor>
PlankWaylandCursor::create(SDL_Window* parentWindow)
{
#ifdef HAS_WAYLAND
    std::unique_ptr<Impl> impl(new Impl(parentWindow));
    if (!impl->initialize()) {
        return nullptr;
    }
    return std::unique_ptr<PlankWaylandCursor>(
            new PlankWaylandCursor(std::move(impl)));
#else
    (void) parentWindow;
    return nullptr;
#endif
}

void PlankWaylandCursor::dispatchPending()
{
#ifdef HAS_WAYLAND
    m_Impl->dispatchPending();
#endif
}

bool PlankWaylandCursor::isAttachedTo(SDL_Window* parentWindow) const
{
#ifdef HAS_WAYLAND
    return m_Impl->isAttachedTo(parentWindow);
#else
    (void) parentWindow;
    return false;
#endif
}

void PlankWaylandCursor::setImage(
        const QImage& image, int hotspotX, int hotspotY)
{
#ifdef HAS_WAYLAND
    m_Impl->setImage(image, hotspotX, hotspotY);
#else
    (void) image;
    (void) hotspotX;
    (void) hotspotY;
#endif
}

void PlankWaylandCursor::setPosition(int hotspotX, int hotspotY)
{
#ifdef HAS_WAYLAND
    m_Impl->setPosition(hotspotX, hotspotY);
#else
    (void) hotspotX;
    (void) hotspotY;
#endif
}

void PlankWaylandCursor::setVisible(bool visible)
{
#ifdef HAS_WAYLAND
    m_Impl->setVisible(visible);
#else
    (void) visible;
#endif
}
