#include "streamutils.h"

#include <Qt>
#include <QDir>

#ifdef Q_OS_DARWIN
#include <ApplicationServices/ApplicationServices.h>
#include "macwindow.h"
#include "macdisplaygeometry.h"
#include "macdisplaymode.h"
#endif

#ifdef Q_OS_UNIX
#include <unistd.h>
#include <fcntl.h>

#include <SDL3/SDL_system.h>
#endif

Uint32 StreamUtils::getPlatformWindowFlags()
{
#if defined(HAVE_LIBPLACEBO_VULKAN)
    // We'll fall back to GL if Vulkan fails
    return SDL_WINDOW_VULKAN;
#elif defined(Q_OS_DARWIN)
    // Vulkan needs to supersede Metal, otherwise the Vulkan library won't be loaded
    return SDL_WINDOW_METAL;
#else
    return 0;
#endif
}

SDL_Window* StreamUtils::createTestWindow()
{
    SDL_Window* testWindow;
    Uint32 baseFlags = 0;

    // Test windows are always hidden
    baseFlags |= SDL_WINDOW_HIDDEN;

    // Creating a Vulkan surface with KMSDRM requires finding a display mode
    // that exactly matches the window size. This is not always possible,
    // particularly with drivers (Nvidia) that only expose modes matching
    // the current resolution (only differing by refresh rate). Fullscreen
    // desktop mode ensures the window size exactly matches the display mode
    // which prevents false Vulkan renderer failures during decoder probing.
    if (QString(SDL_GetCurrentVideoDriver()) == "KMSDRM") {
        baseFlags |= SDL_WINDOW_FULLSCREEN;
    }

    // Try to add the platform-specific flags first and fall back if that fails
    testWindow = SDL_CreateWindow("", 1280, 720,
                                  baseFlags | StreamUtils::getPlatformWindowFlags());
    if (!testWindow) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to create test window with platform flags: %s",
                    SDL_GetError());

        testWindow = SDL_CreateWindow("", 1280, 720, baseFlags);
        if (!testWindow) {
            return nullptr;
        }
    }

    return testWindow;
}

int StreamUtils::getDisplayCount()
{
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    SDL_free(displays);
    return count;
}

SDL_DisplayID StreamUtils::getDisplayId(int displayIndex)
{
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    const SDL_DisplayID id = displays != nullptr && displayIndex >= 0 && displayIndex < count ?
                                 displays[displayIndex] : 0;
    SDL_free(displays);
    return id;
}

int StreamUtils::getDisplayIndex(SDL_DisplayID display)
{
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    int index = -1;
    for (int i = 0; displays != nullptr && i < count; ++i) {
        if (displays[i] == display) {
            index = i;
            break;
        }
    }
    SDL_free(displays);
    return index;
}

int StreamUtils::getDisplayModeCount(int displayIndex)
{
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(getDisplayId(displayIndex), &count);
    SDL_free(modes);
    return count;
}

bool StreamUtils::getDisplayMode(int displayIndex, int modeIndex, SDL_DisplayMode* mode)
{
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(getDisplayId(displayIndex), &count);
    const bool valid = modes != nullptr && modeIndex >= 0 && modeIndex < count;
    if (valid) {
        *mode = *modes[modeIndex];
    }
    SDL_free(modes);
    return valid;
}

void StreamUtils::scaleSourceToDestinationSurface(SDL_Rect* src, SDL_Rect* dst)
{
    int dstH = SDL_ceilf((float)dst->w * src->h / src->w);
    int dstW = SDL_ceilf((float)dst->h * src->w / src->h);

    if (dstH > dst->h) {
        dst->x += (dst->w - dstW) / 2;
        dst->w = dstW;
    }
    else {
        dst->y += (dst->h - dstH) / 2;
        dst->h = dstH;
    }
}

void StreamUtils::screenSpaceToNormalizedDeviceCoords(SDL_FRect* rect, int viewportWidth, int viewportHeight)
{
    rect->x = (rect->x / (viewportWidth / 2.0f)) - 1.0f;
    rect->y = (rect->y / (viewportHeight / 2.0f)) - 1.0f;
    rect->w = rect->w / (viewportWidth / 2.0f);
    rect->h = rect->h / (viewportHeight / 2.0f);
}

void StreamUtils::screenSpaceToNormalizedDeviceCoords(SDL_Rect* src, SDL_FRect* dst, int viewportWidth, int viewportHeight)
{
    dst->x = ((float)src->x / (viewportWidth / 2.0f)) - 1.0f;
    dst->y = ((float)src->y / (viewportHeight / 2.0f)) - 1.0f;
    dst->w = (float)src->w / (viewportWidth / 2.0f);
    dst->h = (float)src->h / (viewportHeight / 2.0f);
}

int StreamUtils::getDisplayRefreshRate(SDL_Window* window)
{
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    if (display == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to get current display: %s",
                     SDL_GetError());

        return 60;
    }

    const SDL_DisplayMode* mode = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) ?
                                      SDL_GetWindowFullscreenMode(window) :
                                      SDL_GetCurrentDisplayMode(display);
    if (mode == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to query display mode: %s", SDL_GetError());
        return 60;
    }

    // May be zero if undefined
    if (mode->refresh_rate == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Refresh rate unknown; assuming 60 Hz");
        return 60;
    }

    return qRound(mode->refresh_rate);
}

#ifdef Q_OS_DARWIN
bool StreamUtils::getMacCurrentDisplayMode(Uint32 displayId, SDL_DisplayMode* mode, SDL_Rect* bounds, bool fullscreen)
{
    SDL_zerop(mode);
    MacDisplayMode::Snapshot current;
    if (!MacDisplayMode::currentSnapshot(displayId, current)) return false;
    mode->w = current.width;
    mode->h = current.height;
    const CGRect logical = CGDisplayBounds(displayId);
    *bounds = {qRound(logical.origin.x), qRound(logical.origin.y),
               qRound(logical.size.width), qRound(logical.size.height)};
    if (fullscreen) {
        int top = 0;
        if (!MacWindow::fullscreenTopInset(displayId, &top) ||
                !MacDisplayGeometry::insetTop(bounds->w, bounds->h, mode->w, mode->h, top))
            return false;
        bounds->y += top;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK Mac display: fullscreen viewport=%dx%d backing=%dx%d top-inset=%d",
                    bounds->w, bounds->h, mode->w, mode->h, top);
    }
    return mode->w > 0 && mode->h > 0 && bounds->w > 0 && bounds->h > 0;
}

bool StreamUtils::getMacCurrentDisplayModeForBounds(const SDL_Rect& bounds, SDL_DisplayMode* mode, SDL_Rect* matchedBounds, bool fullscreen)
{
    CGDirectDisplayID ids[16], selected = 0;
    uint32_t count = 0;
    if (CGGetActiveDisplayList(16, ids, &count) != kCGErrorSuccess) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const CGRect cg = CGDisplayBounds(ids[i]);
        if (qRound(cg.origin.x) == bounds.x && qRound(cg.origin.y) == bounds.y &&
                qRound(cg.size.width) == bounds.w && qRound(cg.size.height) == bounds.h) {
            if (selected) return false;
            selected = ids[i];
        }
    }
    // Selection uses the complete display bounds. The returned viewport may
    // exclude the notch, but must not replace the display's placement/identity.
    return selected && getMacCurrentDisplayMode(selected, mode, matchedBounds, fullscreen);
}

bool StreamUtils::getMacNativeDisplayMode(Uint32 displayId, SDL_DisplayMode* mode, SDL_Rect* safeArea)
{
    SDL_zerop(mode);
    MacDisplayMode::Snapshot snapshot;
    if (!MacDisplayMode::snapshot(displayId, snapshot)) {
        SDL_SetError("The Mac display has neither a valid native nor current pixel mode");
        return false;
    }
    mode->w = snapshot.width;
    mode->h = snapshot.height;
    *safeArea = {0, 0, snapshot.width, snapshot.safeHeight};
    if (!snapshot.native) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Mac display has no advertised native mode; using current backing pixels %dx%d",
                    mode->w, mode->h);
    }
    return true;
}
#endif

bool StreamUtils::getNativeDesktopMode(int displayIndex, SDL_DisplayMode* mode, SDL_Rect* safeArea)
{
#ifdef Q_OS_DARWIN
    CGDirectDisplayID displayIds[16];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(16, displayIds, &count) != kCGErrorSuccess ||
            displayIndex < 0 || displayIndex >= static_cast<int>(count)) return false;
    CGDirectDisplayID displayId = displayIds[displayIndex];
    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        // SDL and CoreGraphics do not promise the same enumeration order.
        SDL_Rect bounds;
        if (!SDL_GetDisplayBounds(getDisplayId(displayIndex), &bounds)) return false;
        displayId = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const CGRect cg = CGDisplayBounds(displayIds[i]);
            if (qRound(cg.origin.x) == bounds.x && qRound(cg.origin.y) == bounds.y &&
                    qRound(cg.size.width) == bounds.w && qRound(cg.size.height) == bounds.h) {
                if (displayId) return false;
                displayId = displayIds[i];
            }
        }
        if (!displayId) return false;
    }
    if (!getMacNativeDisplayMode(displayId, mode, safeArea)) return false;

    // Special case for probing for notched displays prior to video subsystem initialization
    // in Session::initialize() for Darwin only!
    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        // Now find the SDL mode that matches the CG native mode
        for (int i = 0; i < getDisplayModeCount(displayIndex); i++) {
            SDL_DisplayMode thisMode;
            if (getDisplayMode(displayIndex, i, &thisMode)) {
                if (thisMode.w == mode->w && thisMode.h == mode->h &&
                    thisMode.refresh_rate >= mode->refresh_rate) {
                    *mode = thisMode;
                    break;
                }
            }
        }
    }
#else
    SDL_assert(SDL_WasInit(SDL_INIT_VIDEO));

    const SDL_DisplayID display = getDisplayId(displayIndex);
    if (display == 0) {
        return false;
    }

    // We need to get the true display resolution without DPI scaling (since we use High DPI).
    // Windows returns the real display resolution here, even if DPI scaling is enabled.
    // macOS and Wayland report a resolution that includes the DPI scaling factor. Picking
    // the first mode on Wayland will get the native resolution without the scaling factor
    // (and macOS is handled in the #ifdef above).
    if (!strcmp(SDL_GetCurrentVideoDriver(), "wayland")) {
        if (!getDisplayMode(displayIndex, 0, mode)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_GetDisplayMode() failed: %s",
                         SDL_GetError());
            return false;
        }
    }
    else {
        const SDL_DisplayMode* desktopMode = SDL_GetDesktopDisplayMode(display);
        if (desktopMode == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_GetDesktopDisplayMode() failed: %s",
                         SDL_GetError());
            return false;
        }
        *mode = *desktopMode;
    }

    if (!SDL_GetDisplayUsableBounds(display, safeArea)) {
        safeArea->x = 0;
        safeArea->y = 0;
        safeArea->w = mode->w;
        safeArea->h = mode->h;
    }
#endif

    return true;
}

int StreamUtils::getDrmFdForWindow(SDL_Window* window, bool* mustClose)
{
    *mustClose = false;

#if defined(SDL_VIDEO_DRIVER_KMSDRM)
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    if (properties != 0) {
        const Sint64 drmFd = SDL_GetNumberProperty(
            properties, SDL_PROP_WINDOW_KMSDRM_DRM_FD_NUMBER, -1);
        if (drmFd >= 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Sharing DRM FD with SDL");
            return static_cast<int>(drmFd);
        }
        const Sint64 deviceIndex = SDL_GetNumberProperty(
            properties, SDL_PROP_WINDOW_KMSDRM_DEVICE_INDEX_NUMBER, -1);
        if (deviceIndex >= 0) {
            char path[128];
            snprintf(path, sizeof(path), "/dev/dri/card%lld",
                     static_cast<long long>(deviceIndex));
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Opening DRM FD from SDL by path: %s",
                        path);
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                *mustClose = true;
            }
            return fd;
        }
    }
#else
    Q_UNUSED(window);
#endif

    return -1;
}

int StreamUtils::getDrmFd(bool preferRenderNode)
{
#ifdef Q_OS_UNIX
    const char* userDevice = SDL_getenv("DRM_DEV");
    if (userDevice != nullptr) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Opening user-specified DRM device: %s",
                    userDevice);

        return open(userDevice, O_RDWR | O_CLOEXEC);
    }
    else {
        QDir driDir("/dev/dri");
        int fd;

        // We have to explicitly ask for devices to be returned
        driDir.setFilter(QDir::Files | QDir::System);

        if (preferRenderNode) {
            // Try a render node first since we aren't using DRM for output in this codepath
            for (QFileInfo& node : driDir.entryInfoList(QStringList("renderD*"))) {
                QByteArray absolutePath = node.absoluteFilePath().toUtf8();
                fd = open(absolutePath.constData(), O_RDWR | O_CLOEXEC);
                if (fd >= 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Opened DRM render node: %s",
                                absolutePath.constData());
                    return fd;
                }
            }
        }

        // If that fails, try to use a primary node and hope for the best
        for (QFileInfo& node : driDir.entryInfoList(QStringList("card*"))) {
            QByteArray absolutePath = node.absoluteFilePath().toUtf8();
            fd = open(absolutePath.constData(), O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Opened DRM primary node: %s",
                            absolutePath.constData());
                return fd;
            }
        }
    }
#else
    Q_UNUSED(preferRenderNode);
#endif

    return -1;
}

extern QAtomicInt g_AsyncLoggingEnabled;

void StreamUtils::enterAsyncLoggingMode()
{
    g_AsyncLoggingEnabled.ref();
}

void StreamUtils::exitAsyncLoggingMode()
{
    g_AsyncLoggingEnabled.deref();
}
