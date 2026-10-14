#include "plankwaylandtoolbar.h"

#ifdef HAS_WAYLAND

#include <QColor>
#include <QPainter>
#include <QPainterPath>

#include <linux/input-event-codes.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-version.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

namespace {

int createAnonymousFile(size_t size)
{
    const int fd = static_cast<int>(syscall(
            SYS_memfd_create, "plank-toolbar", MFD_CLOEXEC));
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    return fd;
}

} // namespace

class PlankWaylandToolbar::Impl
{
public:
    struct Buffer {
        Impl* owner = nullptr;
        wl_buffer* object = nullptr;
        void* mapping = MAP_FAILED;
        size_t size = 0;
        bool released = false;
        bool cursor = false;
    };

    Impl(SDL_Window* parentWindow, Callbacks callbacks)
        : m_Callbacks(std::move(callbacks))
    {
        const SDL_PropertiesID properties = SDL_GetWindowProperties(parentWindow);
        m_Display = static_cast<wl_display*>(SDL_GetPointerProperty(
                properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
        m_ParentSurface = static_cast<wl_surface*>(SDL_GetPointerProperty(
                properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
    }

    ~Impl()
    {
        if (m_Pointer != nullptr) {
            wl_pointer_destroy(m_Pointer);
        }
        if (m_Seat != nullptr) {
            wl_seat_destroy(m_Seat);
        }
        if (m_Subsurface != nullptr) {
            wl_subsurface_destroy(m_Subsurface);
        }
        if (m_Surface != nullptr) {
            wl_surface_destroy(m_Surface);
        }
        if (m_CursorSurface != nullptr) {
            wl_surface_destroy(m_CursorSurface);
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
                m_Shm == nullptr || m_Seat == nullptr) {
            return false;
        }
        if (wl_display_roundtrip_queue(m_Display, m_Queue) < 0 ||
                m_Pointer == nullptr) {
            return false;
        }

        m_Surface = wl_compositor_create_surface(m_Compositor);
        m_CursorSurface = wl_compositor_create_surface(m_Compositor);
        if (m_Surface == nullptr || m_CursorSurface == nullptr) {
            return false;
        }
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(m_Surface), m_Queue);
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(m_CursorSurface), m_Queue);

        m_Subsurface = wl_subcompositor_get_subsurface(
                m_Subcompositor, m_Surface, m_ParentSurface);
        if (m_Subsurface == nullptr) {
            return false;
        }
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(m_Subsurface), m_Queue);
        wl_subsurface_set_desync(m_Subsurface);
        wl_subsurface_set_position(m_Subsurface, 0, 0);

        QImage cursorImage(24, 24, QImage::Format_ARGB32_Premultiplied);
        cursorImage.fill(Qt::transparent);
        QPainter painter(&cursorImage);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath cursor;
        cursor.moveTo(1.0, 1.0);
        cursor.lineTo(2.0, 19.0);
        cursor.lineTo(6.2, 14.7);
        cursor.lineTo(10.1, 23.0);
        cursor.lineTo(13.0, 21.6);
        cursor.lineTo(9.1, 13.3);
        cursor.lineTo(15.0, 13.0);
        cursor.closeSubpath();
        painter.setPen(QPen(QColor(20, 20, 20), 1.4,
                            Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(QColor(248, 248, 248));
        painter.drawPath(cursor);
        painter.end();

        Buffer* cursorBuffer = createBuffer(cursorImage, true);
        if (cursorBuffer == nullptr) {
            return false;
        }
        wl_surface_attach(m_CursorSurface, cursorBuffer->object, 0, 0);
        wl_surface_damage(m_CursorSurface, 0, 0,
                          cursorImage.width(), cursorImage.height());
        wl_surface_commit(m_CursorSurface);
        wl_display_flush(m_Display);
        return true;
    }

    void dispatchPending()
    {
        if (m_Display != nullptr && m_Queue != nullptr) {
            wl_display_dispatch_queue_pending(m_Display, m_Queue);
        }
    }

    bool isAttachedTo(SDL_Window* parentWindow) const
    {
        const SDL_PropertiesID properties = SDL_GetWindowProperties(parentWindow);
        return m_ParentSurface == static_cast<wl_surface*>(SDL_GetPointerProperty(
                    properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER,
                    nullptr));
    }

    void setLayout(int parentWidth, int toolbarX,
                   int toolbarWidth, int toolbarHeight)
    {
        setLayoutAt(parentWidth, 0, toolbarX, toolbarWidth, toolbarHeight);
    }

    void setLayoutAt(int parentWidth, int surfaceY, int contentX,
                     int contentWidth, int contentHeight)
    {
        m_ParentWidth = std::max(1, parentWidth);
        m_ToolbarWidth = std::clamp(contentWidth, 1, m_ParentWidth);
        m_ToolbarX = std::clamp(contentX, 0,
                                m_ParentWidth - m_ToolbarWidth);
        m_SurfaceY = std::max(0, surfaceY);
        m_Height = std::max(1, contentHeight);
        wl_subsurface_set_position(m_Subsurface, 0, m_SurfaceY);
        updateInputRegion();
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
        } else if (!m_LatestToolbarImage.isNull()) {
            publishLatestImage();
        }
        wl_display_flush(m_Display);
    }

    void present(const QImage& image)
    {
        m_LatestToolbarImage = image.convertToFormat(
                QImage::Format_ARGB32_Premultiplied);
        if (m_Visible) {
            publishLatestImage();
        }
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
        } else if (std::strcmp(interface, wl_seat_interface.name) == 0 &&
                   self->m_Seat == nullptr) {
            self->m_Seat = static_cast<wl_seat*>(wl_registry_bind(
                    registry, name, &wl_seat_interface,
                    std::min(version, 7u)));
            if (self->m_Seat != nullptr) {
                wl_seat_add_listener(self->m_Seat, &SeatListener, self);
            }
        }
    }

    static void registryRemove(void*, wl_registry*, uint32_t) {}

    static void seatCapabilities(void* data, wl_seat* seat,
                                 uint32_t capabilities)
    {
        auto* self = static_cast<Impl*>(data);
        if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 &&
                self->m_Pointer == nullptr) {
            self->m_Pointer = wl_seat_get_pointer(seat);
            if (self->m_Pointer != nullptr) {
                wl_pointer_add_listener(self->m_Pointer, &PointerListener, self);
            }
        } else if ((capabilities & WL_SEAT_CAPABILITY_POINTER) == 0 &&
                   self->m_Pointer != nullptr) {
            wl_pointer_destroy(self->m_Pointer);
            self->m_Pointer = nullptr;
        }
    }

    static void seatName(void*, wl_seat*, const char*) {}

    static void pointerEnter(void* data, wl_pointer* pointer, uint32_t serial,
                             wl_surface* surface, wl_fixed_t x, wl_fixed_t y)
    {
        auto* self = static_cast<Impl*>(data);
        if (surface != self->m_Surface) {
            return;
        }
        self->m_PointerInside = true;
        self->m_PointerX = wl_fixed_to_int(x);
        self->m_PointerY = wl_fixed_to_int(y);
        wl_pointer_set_cursor(pointer, serial, self->m_CursorSurface, 1, 1);
        wl_display_flush(self->m_Display);
        if (self->m_Callbacks.enter) {
            self->m_Callbacks.enter(self->m_PointerX, self->m_PointerY);
        }
    }

    static void pointerLeave(void* data, wl_pointer*, uint32_t,
                             wl_surface* surface)
    {
        auto* self = static_cast<Impl*>(data);
        if (surface != self->m_Surface) {
            return;
        }
        self->m_PointerInside = false;
        if (self->m_Callbacks.leave) {
            self->m_Callbacks.leave();
        }
    }

    static void pointerMotion(void* data, wl_pointer*, uint32_t,
                              wl_fixed_t x, wl_fixed_t y)
    {
        auto* self = static_cast<Impl*>(data);
        if (!self->m_PointerInside && self->m_PressedButtonCount == 0) {
            return;
        }
        self->m_PointerX = wl_fixed_to_int(x);
        self->m_PointerY = wl_fixed_to_int(y);
        if (self->m_Callbacks.motion) {
            self->m_Callbacks.motion(self->m_PointerX, self->m_PointerY);
        }
    }

    static void pointerButton(void* data, wl_pointer*, uint32_t, uint32_t,
                              uint32_t button, uint32_t state)
    {
        auto* self = static_cast<Impl*>(data);
        const bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
        if ((self->m_PointerInside || self->m_PressedButtonCount != 0) &&
                self->m_Callbacks.button) {
            self->m_Callbacks.button(
                    button, down);
            if (down) {
                ++self->m_PressedButtonCount;
            } else if (self->m_PressedButtonCount != 0) {
                --self->m_PressedButtonCount;
            }
        }
    }

    static void pointerAxis(void* data, wl_pointer*, uint32_t, uint32_t axis,
                            wl_fixed_t value)
    {
        auto* self = static_cast<Impl*>(data);
        if (!self->m_PointerInside ||
                axis != WL_POINTER_AXIS_VERTICAL_SCROLL ||
                !self->m_Callbacks.wheel) {
            return;
        }
        const double amount = wl_fixed_to_double(value);
        self->m_Callbacks.wheel(amount < 0.0 ? 1 : amount > 0.0 ? -1 : 0);
    }

    static void pointerFrame(void*, wl_pointer*) {}
    static void pointerAxisSource(void*, wl_pointer*, uint32_t) {}
    static void pointerAxisStop(void*, wl_pointer*, uint32_t, uint32_t) {}
    static void pointerAxisDiscrete(void*, wl_pointer*, uint32_t, int32_t) {}
    static void pointerAxisValue120(void*, wl_pointer*, uint32_t, int32_t) {}
#if WAYLAND_VERSION_MAJOR > 1 || WAYLAND_VERSION_MINOR >= 24
    static void pointerAxisRelativeDirection(
            void*, wl_pointer*, uint32_t, uint32_t) {}
#endif

    static void bufferRelease(void* data, wl_buffer*)
    {
        static_cast<Buffer*>(data)->released = true;
    }

    Buffer* createBuffer(const QImage& source, bool cursor)
    {
        const QImage image = source.convertToFormat(
                QImage::Format_ARGB32_Premultiplied);
        const int stride = image.bytesPerLine();
        const size_t size = static_cast<size_t>(stride) * image.height();
        const int fd = createAnonymousFile(size);
        if (fd < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to create Wayland toolbar shared memory: %s",
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
                        "Unable to create Wayland toolbar SHM pool");
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

        Buffer* buffer = new Buffer{this, object, mapping, size, false, cursor};
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
            Buffer* buffer = *it;
            if (buffer->released && !buffer->cursor) {
                destroyBuffer(buffer);
                it = m_Buffers.erase(it);
            } else {
                ++it;
            }
        }
    }

    void updateInputRegion()
    {
        if (m_Surface == nullptr || m_Compositor == nullptr) {
            return;
        }
        wl_region* region = wl_compositor_create_region(m_Compositor);
        if (region == nullptr) {
            return;
        }
        wl_region_add(region, m_ToolbarX, 0, m_ToolbarWidth, m_Height);
        wl_surface_set_input_region(m_Surface, region);
        wl_region_destroy(region);
    }

    void publishLatestImage()
    {
        if (m_ParentWidth <= 0 || m_Height <= 0 ||
                m_LatestToolbarImage.isNull()) {
            return;
        }
        collectReleasedBuffers();
        QImage surfaceImage(m_ParentWidth, m_Height,
                            QImage::Format_ARGB32_Premultiplied);
        surfaceImage.fill(Qt::transparent);
        QPainter painter(&surfaceImage);
        painter.drawImage(m_ToolbarX, 0, m_LatestToolbarImage);
        painter.end();
        Buffer* buffer = createBuffer(surfaceImage, false);
        if (buffer == nullptr) {
            return;
        }
        wl_surface_attach(m_Surface, buffer->object, 0, 0);
        wl_surface_damage(m_Surface, 0, 0,
                          surfaceImage.width(), surfaceImage.height());
        wl_surface_commit(m_Surface);
        wl_display_flush(m_Display);
    }

    static const wl_registry_listener RegistryListener;
    static const wl_seat_listener SeatListener;
    static const wl_pointer_listener PointerListener;
    static const wl_buffer_listener BufferListener;

    Callbacks m_Callbacks;
    wl_display* m_Display = nullptr;
    wl_surface* m_ParentSurface = nullptr;
    wl_event_queue* m_Queue = nullptr;
    wl_registry* m_Registry = nullptr;
    wl_compositor* m_Compositor = nullptr;
    wl_subcompositor* m_Subcompositor = nullptr;
    wl_shm* m_Shm = nullptr;
    wl_seat* m_Seat = nullptr;
    wl_pointer* m_Pointer = nullptr;
    wl_surface* m_Surface = nullptr;
    wl_subsurface* m_Subsurface = nullptr;
    wl_surface* m_CursorSurface = nullptr;
    std::vector<Buffer*> m_Buffers;
    QImage m_LatestToolbarImage;
    bool m_Visible = false;
    bool m_PointerInside = false;
    int m_PointerX = 0;
    int m_PointerY = 0;
    unsigned int m_PressedButtonCount = 0;
    int m_ParentWidth = 0;
    int m_ToolbarX = 0;
    int m_ToolbarWidth = 0;
    int m_SurfaceY = 0;
    int m_Height = 0;
};

const wl_registry_listener PlankWaylandToolbar::Impl::RegistryListener = {
    registryGlobal,
    registryRemove,
};

const wl_seat_listener PlankWaylandToolbar::Impl::SeatListener = {
    seatCapabilities,
    seatName,
};

const wl_pointer_listener PlankWaylandToolbar::Impl::PointerListener = {
    pointerEnter,
    pointerLeave,
    pointerMotion,
    pointerButton,
    pointerAxis,
    pointerFrame,
    pointerAxisSource,
    pointerAxisStop,
    pointerAxisDiscrete,
    pointerAxisValue120,
#if WAYLAND_VERSION_MAJOR > 1 || WAYLAND_VERSION_MINOR >= 24
    pointerAxisRelativeDirection,
#endif
};

const wl_buffer_listener PlankWaylandToolbar::Impl::BufferListener = {
    bufferRelease,
};

#else

class PlankWaylandToolbar::Impl {};

#endif

PlankWaylandToolbar::PlankWaylandToolbar(
        std::unique_ptr<Impl> impl)
    : m_Impl(std::move(impl))
{
}

PlankWaylandToolbar::~PlankWaylandToolbar() = default;

std::unique_ptr<PlankWaylandToolbar>
PlankWaylandToolbar::create(SDL_Window* parentWindow,
                                     Callbacks callbacks)
{
#ifdef HAS_WAYLAND
    std::unique_ptr<Impl> impl(new Impl(parentWindow, std::move(callbacks)));
    if (!impl->initialize()) {
        return nullptr;
    }
    return std::unique_ptr<PlankWaylandToolbar>(
            new PlankWaylandToolbar(std::move(impl)));
#else
    (void) parentWindow;
    (void) callbacks;
    return nullptr;
#endif
}

void PlankWaylandToolbar::dispatchPending()
{
#ifdef HAS_WAYLAND
    m_Impl->dispatchPending();
#endif
}

bool PlankWaylandToolbar::isAttachedTo(SDL_Window* parentWindow) const
{
#ifdef HAS_WAYLAND
    return m_Impl->isAttachedTo(parentWindow);
#else
    (void) parentWindow;
    return false;
#endif
}

void PlankWaylandToolbar::setLayout(
        int parentWidth, int toolbarX,
        int toolbarWidth, int toolbarHeight)
{
#ifdef HAS_WAYLAND
    m_Impl->setLayout(parentWidth, toolbarX,
                      toolbarWidth, toolbarHeight);
#else
    (void) parentWidth;
    (void) toolbarX;
    (void) toolbarWidth;
    (void) toolbarHeight;
#endif
}

void PlankWaylandToolbar::setLayoutAt(
        int parentWidth, int surfaceY, int contentX,
        int contentWidth, int contentHeight)
{
#ifdef HAS_WAYLAND
    m_Impl->setLayoutAt(parentWidth, surfaceY, contentX,
                        contentWidth, contentHeight);
#else
    (void) parentWidth;
    (void) surfaceY;
    (void) contentX;
    (void) contentWidth;
    (void) contentHeight;
#endif
}

void PlankWaylandToolbar::setVisible(bool visible)
{
#ifdef HAS_WAYLAND
    m_Impl->setVisible(visible);
#else
    (void) visible;
#endif
}

void PlankWaylandToolbar::present(const QImage& image)
{
#ifdef HAS_WAYLAND
    m_Impl->present(image);
#else
    (void) image;
#endif
}
