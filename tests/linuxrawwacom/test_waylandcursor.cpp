// Exercise the production cursor against a small in-process Wayland server.
// It enforces the parent-commit rule for subsurface positions. This is a
// protocol regression test, not compositor/visual hardware qualification.
#include "../../app/streaming/plankwaylandcursor.h"
#include "../../app/streaming/plankwaylandtoolbar.h"
#include "wayland-faults.h"

#include <wayland-client.h>
#include <wayland-server.h>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "check failed at line " << __LINE__ << ": " #condition "\n"; \
    std::abort(); \
} } while (false)

struct Surface {
    Surface* parent = nullptr;
    unsigned commits = 0;
    bool desync = false, inputEmpty = false;
    int pendingX = 0, pendingY = 0, x = 0, y = 0;
    bool pendingBuffer = false, hasBuffer = false;
};

class Server {
public:
    wl_display* display = wl_display_create();
    std::vector<std::unique_ptr<Surface>> surfaces;
    std::mutex mutex;

    explicit Server(int fd)
    {
        CHECK(display);
        CHECK(wl_display_init_shm(display) == 0);
        CHECK(wl_global_create(display, &wl_compositor_interface, 4, this, bindCompositor));
        CHECK(wl_global_create(display, &wl_subcompositor_interface, 1, this, bindSubcompositor));
        CHECK(wl_global_create(display, &wl_seat_interface, 7, this, bindSeat));
        CHECK(wl_client_create(display, fd));
        thread = std::thread([this] {
            while (!stopping.load()) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    CHECK(wl_event_loop_dispatch(wl_display_get_event_loop(display), 0) >= 0);
                    wl_display_flush_clients(display);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    ~Server()
    {
        stopping = true;
        thread.join();
        wl_display_destroy_clients(display);
        wl_display_destroy(display);
    }

private:
    std::atomic<bool> stopping {false};
    std::thread thread;
    static Server* server(wl_resource* resource)
    { return static_cast<Server*>(wl_resource_get_user_data(resource)); }
    static Surface* surface(wl_resource* resource)
    { return static_cast<Surface*>(wl_resource_get_user_data(resource)); }
    static void destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

    static void bindSeat(wl_client* client, void*, uint32_t version, uint32_t id)
    {
        static const struct wl_seat_interface impl {
            [](wl_client* client, wl_resource*, uint32_t id) {
                static const struct wl_pointer_interface pointerImpl {
                    [](wl_client*, wl_resource*, uint32_t, wl_resource*, int32_t, int32_t) {},
                    destroy};
                auto* resource = wl_resource_create(client, &wl_pointer_interface, 7, id);
                wl_resource_set_implementation(resource, &pointerImpl, nullptr, nullptr);
            },
            [](wl_client*, wl_resource*, uint32_t) { CHECK(false); },
            [](wl_client*, wl_resource*, uint32_t) { CHECK(false); },
            destroy};
        auto* resource = wl_resource_create(client, &wl_seat_interface, version, id);
        wl_resource_set_implementation(resource, &impl, nullptr, nullptr);
        wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_POINTER);
        wl_seat_send_name(resource, "test-seat");
    }

    static void bindCompositor(wl_client* client, void* data, uint32_t version, uint32_t id)
    {
        static const struct wl_compositor_interface impl {
            [](wl_client* client, wl_resource* compositor, uint32_t id) {
                auto* owner = server(compositor);
                auto state = std::make_unique<Surface>();
                auto* resource = wl_resource_create(client, &wl_surface_interface, 4, id);
                wl_resource_set_implementation(resource, &SurfaceImpl, state.get(), nullptr);
                owner->surfaces.push_back(std::move(state));
                wl_client_set_user_data(client, owner, nullptr);
            },
            [](wl_client* client, wl_resource*, uint32_t id) {
                static const struct wl_region_interface impl {
                    destroy, [](wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {},
                    [](wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}};
                auto* resource = wl_resource_create(client, &wl_region_interface, 1, id);
                wl_resource_set_implementation(resource, &impl, nullptr, nullptr);
            }};
        auto* resource = wl_resource_create(client, &wl_compositor_interface, version, id);
        wl_resource_set_implementation(resource, &impl, data, nullptr);
    }

    static void bindSubcompositor(wl_client* client, void*, uint32_t, uint32_t id)
    {
        static const struct wl_subcompositor_interface impl {destroy,
            [](wl_client* client, wl_resource*, uint32_t id, wl_resource* child, wl_resource* parent) {
                static const struct wl_subsurface_interface subImpl {
                    destroy,
                    [](wl_client*, wl_resource* sub, int32_t x, int32_t y) {
                        surface(sub)->pendingX = x; surface(sub)->pendingY = y;
                    },
                    [](wl_client*, wl_resource*, wl_resource*) {},
                    [](wl_client*, wl_resource*, wl_resource*) {},
                    [](wl_client*, wl_resource* sub) { surface(sub)->desync = false; },
                    [](wl_client*, wl_resource* sub) { surface(sub)->desync = true; }};
                auto* state = surface(child);
                state->parent = surface(parent);
                auto* resource = wl_resource_create(client, &wl_subsurface_interface, 1, id);
                wl_resource_set_implementation(resource, &subImpl, state, nullptr);
            }};
        auto* resource = wl_resource_create(client, &wl_subcompositor_interface, 1, id);
        wl_resource_set_implementation(resource, &impl, nullptr, nullptr);
    }

    static const struct wl_surface_interface SurfaceImpl;
};

const struct wl_surface_interface Server::SurfaceImpl {
    destroy,
    [](wl_client*, wl_resource* resource, wl_resource* buffer, int32_t, int32_t) {
        surface(resource)->pendingBuffer = buffer != nullptr;
        if (buffer) wl_buffer_send_release(buffer);
    },
    [](wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {},
    [](wl_client*, wl_resource*, uint32_t) { CHECK(false); }, // No video frame callbacks.
    [](wl_client*, wl_resource*, wl_resource*) {},
    [](wl_client*, wl_resource* resource, wl_resource* region) {
        surface(resource)->inputEmpty = region != nullptr;
    },
    [](wl_client* client, wl_resource* resource) {
        auto* state = surface(resource);
        ++state->commits;
        state->hasBuffer = state->pendingBuffer;
        auto* owner = static_cast<Server*>(wl_client_get_user_data(client));
        for (auto& child : owner->surfaces) {
            if (child->parent == state) {
                child->x = child->pendingX;
                child->y = child->pendingY;
            }
        }
    },
    [](wl_client*, wl_resource*, int32_t) {},
    [](wl_client*, wl_resource*, int32_t) {},
    [](wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {},
    [](wl_client*, wl_resource*, int32_t, int32_t) {}
};

int main()
{
    int sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    Server server(sockets[0]);
    wl_display* display = wl_display_connect_to_fd(sockets[1]);
    CHECK(display);
    auto* registry = wl_display_get_registry(display);
    wl_compositor* compositor = nullptr;
    static const wl_registry_listener listener {
        [](void* data, wl_registry* registry, uint32_t id, const char* name, uint32_t) {
            if (std::string(name) == "wl_compositor")
                *static_cast<wl_compositor**>(data) = static_cast<wl_compositor*>(
                    wl_registry_bind(registry, id, &wl_compositor_interface, 4));
        }, [](void*, wl_registry*, uint32_t) {}};
    wl_registry_add_listener(registry, &listener, &compositor);
    CHECK(wl_display_roundtrip(display) >= 0 && compositor);
    auto* videoSurface = wl_compositor_create_surface(compositor);
    CHECK(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    auto* window = SDL_CreateWindow("cursor protocol test", 320, 200, SDL_WINDOW_HIDDEN);
    CHECK(window);
    auto properties = SDL_GetWindowProperties(window);
    CHECK(SDL_SetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, display));
    CHECK(SDL_SetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, videoSurface));
    auto cursor = PlankWaylandCursor::create(window);
    CHECK(cursor);
    // Map once, then no further video/SDL parent commits.
    wl_surface_commit(videoSurface);
    QImage icon(16, 16, QImage::Format_ARGB32_Premultiplied);
    icon.fill(Qt::white);
    cursor->setImage(icon, 3, 5);
    cursor->setPosition(150, 90);
    cursor->setVisible(true);
    CHECK(wl_display_roundtrip(display) >= 0);
    {
        std::lock_guard<std::mutex> lock(server.mutex);
        CHECK(server.surfaces.size() == 3);
        auto& anchor = *server.surfaces[1];
        auto& child = *server.surfaces[2];
        CHECK(anchor.parent == server.surfaces[0].get() && anchor.desync);
        CHECK(child.parent == &anchor && !child.desync);
        CHECK(anchor.inputEmpty && child.inputEmpty);
        CHECK(anchor.hasBuffer && child.hasBuffer);
        CHECK(child.x == 147 && child.y == 85);
    }
    for (int i = 0; i < 100; ++i) cursor->setPosition(300 + i, 120 + i);
    CHECK(wl_display_roundtrip(display) >= 0);
    {
        std::lock_guard<std::mutex> lock(server.mutex);
        CHECK(server.surfaces[0]->commits == 1);
        CHECK(server.surfaces[2]->x == 396 && server.surfaces[2]->y == 214);
    }
    cursor->setVisible(false);
    CHECK(wl_display_roundtrip(display) >= 0);
    {
        std::lock_guard<std::mutex> lock(server.mutex);
        CHECK(!server.surfaces[2]->hasBuffer && server.surfaces[0]->commits == 1);
    }
    cursor->dispatchPending();
    cursor.reset();

    // A compositor/proxy allocation failure must not pass NULL to generated
    // Wayland methods. Repeat to exercise partially constructed cleanup too.
    for (auto fault : {WaylandFault::Pool, WaylandFault::Buffer,
                       WaylandFault::Seat, WaylandFault::Pointer}) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            waylandFault = fault;
            const auto before = waylandFaultsInjected;
            if (fault == WaylandFault::Pool || fault == WaylandFault::Buffer)
                CHECK(!PlankWaylandCursor::create(window));
            CHECK(!PlankWaylandToolbar::create(window, {}));
            CHECK(waylandFaultsInjected > before);
            waylandFault = WaylandFault::None;
            CHECK(wl_display_roundtrip(display) >= 0);
        }
    }

    // Replacement parents and late buffer releases must be safe after failed
    // creation; a rejected update must not make an existing surface unusable.
    for (int attempt = 0; attempt < 10; ++attempt) {
        cursor = PlankWaylandCursor::create(window);
        auto toolbar = PlankWaylandToolbar::create(window, {});
        CHECK(cursor && toolbar);
        toolbar->setLayout(320, 50, 150, 30);
        toolbar->setVisible(true);
        for (auto fault : {WaylandFault::Pool, WaylandFault::Buffer, WaylandFault::None}) {
            waylandFault = fault;
            cursor->setImage(icon, 3, 5);
            toolbar->present(icon);
        }
        cursor->setVisible(true);
        cursor->setPosition(100, 70);
        CHECK(wl_display_roundtrip(display) >= 0);
        // Destroy without dispatching the pending release notifications.
        cursor.reset();
        toolbar.reset();
        auto* replacement = wl_compositor_create_surface(compositor);
        CHECK(replacement);
        CHECK(SDL_SetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, replacement));
        wl_surface_destroy(videoSurface);
        videoSurface = replacement;
        CHECK(wl_display_roundtrip(display) >= 0);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    wl_surface_destroy(videoSurface);
    wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);
    CHECK(wl_display_roundtrip(display) >= 0);
    wl_display_disconnect(display);
    std::cout << "Wayland cursor/toolbar: commits, allocation failure, recreation and late releases passed\n";
}
