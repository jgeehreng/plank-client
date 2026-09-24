#include <Limelight.h>
#include <plank.h>
#include <SDL3/SDL.h>
#include "streaming/input/input.h"
#include "streaming/session.h"
#include "streaming/plankwaylandcursor.h"
#include "streaming/streamutils.h"
#include "utils.h"
#ifdef Q_OS_MACOS
#include "streaming/macquitshortcut.h"
#include "streaming/mackeyboardcapture.h"
#include "streaming/macwindow.h"
#endif

#ifdef HAVE_MAC_RAW_WACOM
#include "streaming/input/macrawwacom.h"
#include "streaming/input/macnormalizedpen.h"
#include "streaming/input/macnormalizedpenlogic.h"
#endif

#ifdef HAVE_LIBINPUT_TABLET
#include "streaming/input/linuxwacom.h"
#include "streaming/input/linuxrawwacom.h"
#endif

#include <QtGlobal>
#include <QDir>
#include <QGuiApplication>
#include <QtEndian>

#include <algorithm>
#include <cstring>

SdlInputHandler::SdlInputHandler(StreamingPreferences& prefs,
                                 int streamWidth,
                                 int streamHeight)
    : m_Window(nullptr),
      m_NeedsManualCaptureOnLeave(false),
      m_MouseWasInVideoRegion(false),
      m_PendingMouseButtonsAllUpOnVideoRegionLeave(false),
      m_PointerRegionLockActive(false),
      m_PointerRegionLockToggledByUser(false),
      m_LocalToolbarAvailable(false),
      m_LocalCursorSupported(false),
      m_RemoteCursorVisible(true),
      m_CompositorCursorRequestedVisible(true),
      m_TabletCursorActive(false),
      m_FakeMouseCaptureActive(false),
      m_KeyboardCaptureActive(false),
      m_CaptureSystemKeysMode(prefs.captureSysKeysMode),
      m_MouseCursorCapturedVisibilityState(false),
      m_StreamDimensions(
          (static_cast<std::uint64_t>(streamWidth) << 32) |
          static_cast<std::uint32_t>(streamHeight))
{
    // System keys are always captured when running without a DE
    if (!WMUtils::isRunningDesktopEnvironment()) {
        m_CaptureSystemKeysMode = StreamingPreferences::CSK_ALWAYS;
    }

    // Native SDL3 auto-capture is used; the SDL2 leave workaround is obsolete.
    m_NeedsManualCaptureOnLeave = false;

    // Opt-out of SDL's built-in Alt+Tab handling while keyboard grab is enabled
    SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");

    // Allow clicks to pass through to us when focusing the window. If we're in
    // absolute mouse mode, this will avoid the user having to click twice to
    // trigger a click on the host if the Moonlight window is not focused. In
    // relative mode, the click event will trigger the mouse to be recaptured.
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

    // Populate special key combo configuration
    m_SpecialKeyCombos[KeyComboQuit].keyCombo = KeyComboQuit;
    m_SpecialKeyCombos[KeyComboQuit].keyCode = SDLK_Q;
    m_SpecialKeyCombos[KeyComboQuit].scanCode = SDL_SCANCODE_Q;
    m_SpecialKeyCombos[KeyComboQuit].enabled = true;

    m_SpecialKeyCombos[KeyComboUngrabInput].keyCombo = KeyComboUngrabInput;
    m_SpecialKeyCombos[KeyComboUngrabInput].keyCode = SDLK_Z;
    m_SpecialKeyCombos[KeyComboUngrabInput].scanCode = SDL_SCANCODE_Z;
    m_SpecialKeyCombos[KeyComboUngrabInput].enabled = WMUtils::isRunningDesktopEnvironment();

    m_SpecialKeyCombos[KeyComboToggleFullScreen].keyCombo = KeyComboToggleFullScreen;
    m_SpecialKeyCombos[KeyComboToggleFullScreen].keyCode = SDLK_X;
    m_SpecialKeyCombos[KeyComboToggleFullScreen].scanCode = SDL_SCANCODE_X;
    m_SpecialKeyCombos[KeyComboToggleFullScreen].enabled = WMUtils::isRunningDesktopEnvironment();

    m_SpecialKeyCombos[KeyComboToggleStatsOverlay].keyCombo = KeyComboToggleStatsOverlay;
    m_SpecialKeyCombos[KeyComboToggleStatsOverlay].keyCode = SDLK_S;
    m_SpecialKeyCombos[KeyComboToggleStatsOverlay].scanCode = SDL_SCANCODE_S;
    m_SpecialKeyCombos[KeyComboToggleStatsOverlay].enabled = true;

    m_SpecialKeyCombos[KeyComboToggleMinimize].keyCombo = KeyComboToggleMinimize;
    m_SpecialKeyCombos[KeyComboToggleMinimize].keyCode = SDLK_D;
    m_SpecialKeyCombos[KeyComboToggleMinimize].scanCode = SDL_SCANCODE_D;
    m_SpecialKeyCombos[KeyComboToggleMinimize].enabled = WMUtils::isRunningDesktopEnvironment();

    m_SpecialKeyCombos[KeyComboPasteText].keyCombo = KeyComboPasteText;
    m_SpecialKeyCombos[KeyComboPasteText].keyCode = SDLK_V;
    m_SpecialKeyCombos[KeyComboPasteText].scanCode = SDL_SCANCODE_V;
    m_SpecialKeyCombos[KeyComboPasteText].enabled = true;

    m_SpecialKeyCombos[KeyComboTogglePointerRegionLock].keyCombo = KeyComboTogglePointerRegionLock;
    m_SpecialKeyCombos[KeyComboTogglePointerRegionLock].keyCode = SDLK_L;
    m_SpecialKeyCombos[KeyComboTogglePointerRegionLock].scanCode = SDL_SCANCODE_L;
    m_SpecialKeyCombos[KeyComboTogglePointerRegionLock].enabled = true;

    m_SpecialKeyCombos[KeyComboToggleKeyboardGrab].keyCombo = KeyComboToggleKeyboardGrab;
    m_SpecialKeyCombos[KeyComboToggleKeyboardGrab].keyCode = SDLK_K;
    m_SpecialKeyCombos[KeyComboToggleKeyboardGrab].scanCode = SDL_SCANCODE_K;
    m_SpecialKeyCombos[KeyComboToggleKeyboardGrab].enabled =
            WMUtils::isRunningDesktopEnvironment();
#ifdef Q_OS_MACOS
    auto ownsKeyboard = [this] { return isSystemKeyCaptureActive(); };
    m_MacQuitShortcut = std::make_unique<MacQuitShortcut>(ownsKeyboard);
    m_MacKeyboardCapture = std::make_unique<MacKeyboardCapture>(
        ownsKeyboard, [this] { raiseAllKeys(); });
#endif
}

#ifdef Q_OS_MACOS
bool SdlInputHandler::hasMacStreamKeyboardFocus() const
{
    // SDL focus notifications may still be queued. A local Qt dialog must
    // never inherit the stream's shortcut ownership or forward its typing.
    for (const auto& output : m_PresentationLayout.outputs) {
        if (MacWindow::hasKeyboardFocus(output.window)) return true;
    }
    return false;
}
#endif

void SdlInputHandler::setStreamDimensions(int streamWidth, int streamHeight)
{
    SDL_assert(streamWidth > 0);
    SDL_assert(streamHeight > 0);
    m_StreamDimensions.store(
                (static_cast<std::uint64_t>(streamWidth) << 32) |
                static_cast<std::uint32_t>(streamHeight),
                std::memory_order_relaxed);
}

QSize SdlInputHandler::streamDimensions() const
{
    const std::uint64_t dimensions =
            m_StreamDimensions.load(std::memory_order_relaxed);
    return QSize(static_cast<int>(dimensions >> 32),
                 static_cast<int>(dimensions & 0xffffffffu));
}

SdlInputHandler::~SdlInputHandler()
{
#ifdef Q_OS_MACOS
    m_MacKeyboardCapture.reset();
    m_MacQuitShortcut.reset();
#endif
#ifdef HAVE_MAC_RAW_WACOM
    m_MacRawWacomInput.reset();
    m_MacNormalizedPen.reset();
#endif
#ifdef HAVE_LIBINPUT_TABLET
    m_LinuxWacomInput.reset();
    m_LinuxRawWacomInput.reset();
#endif

    for (auto& output : m_WaylandTabletCursorOutputs) {
        output.cursor->setVisible(false);
        output.cursor->dispatchPending();
    }
    m_WaylandTabletCursorOutputs.clear();

    if (m_RemoteCursor != nullptr || m_BlankCursor != nullptr) {
        SDL_SetCursor(SDL_GetDefaultCursor());
    }
    if (m_RemoteCursor != nullptr) {
        SDL_DestroyCursor(m_RemoteCursor);
        m_RemoteCursor = nullptr;
    }
    if (m_BlankCursor != nullptr) {
        SDL_DestroyCursor(m_BlankCursor);
        m_BlankCursor = nullptr;
    }

#ifdef STEAM_LINK
    // Hide SDL's cursor on Steam Link after quitting the stream.
    // FIXME: We should also do this for other situations where SDL
    // and Qt will draw their own mouse cursors like KMSDRM or RPi
    // video backends.
    setCursorVisible(false);
#endif
}

void SdlInputHandler::setWindow(SDL_Window *window)
{
    m_Window = window;
    m_LocalCursorSupported =
            (LiGetHostFeatureFlags() & LI_FF_LOCAL_CURSOR) != 0;
    if (m_LocalCursorSupported) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK local cursor transport enabled");
        setCursorVisible(true);
        ensureWaylandTabletCursorAttached(m_Window);
    }
    else {
        // Mac Host embeds the cursor in ScreenCaptureKit, but tablet
        // proximity and a foreign framebuffer (Jump) often paint none.
        // Keep the local pointer over video so mouse and pen stay visible.
        m_MouseCursorCapturedVisibilityState = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "PLANK local cursor enabled for embedded host");
    }
#if defined(HAVE_LIBINPUT_TABLET) || defined(HAVE_MAC_RAW_WACOM)
    const auto requestTabletCursor = [this]() {
        if (!m_TabletCursorActivationPending.exchange(true)) {
            Session::postTabletCursorActivationEvent();
        }
    };
#endif
#ifdef HAVE_MAC_RAW_WACOM
    const auto hostFlags = LiGetHostFeatureFlags();
    const bool captureAndFocus = isCaptureActive() &&
            (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK Mac tablet host flags 0x%x",
                static_cast<unsigned>(hostFlags));
    if ((hostFlags & (LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND)) ==
            (LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PLANK Mac raw HID Wacom enabled");
        m_MacRawWacomInput.reset(new MacRawWacomInput(requestTabletCursor));
        m_MacRawWacomInput->setActive(captureAndFocus);
    }
    if ((hostFlags & LI_FF_PEN_TOUCH_EVENTS) != 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK Mac normalized pen enabled");
        m_MacNormalizedPen.reset(new MacNormalizedPen(requestTabletCursor));
        m_MacNormalizedPen->setNativeMapper(
            [this](float windowX, float windowY, float& nx, float& ny) {
                if (m_Window == nullptr) {
                    return false;
                }
                int height = 0;
                if (!SDL_GetWindowSize(m_Window, nullptr, &height) || height <= 0) {
                    return false;
                }
                return mapWindowPointToNormalized(
                    m_Window, windowX, static_cast<float>(height) - windowY,
                    nx, ny, true);
            });
        syncMacNormalizedPenActive(captureAndFocus);
    }
#endif
#ifdef HAVE_LIBINPUT_TABLET
    if (qEnvironmentVariableIntValue("PLANK_EXTERNAL_WACOM_BRIDGE") != 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                    "Normalized Wacom capture disabled for external raw-HID qualification");
    }
    else if ((LiGetHostFeatureFlags() &
              (LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND)) ==
             (LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND)) {
        const PlankWacomTransportDecision decision =
            plankWacomTransportForConnectedDevice();
        if (decision.transport == PlankWacomTransport::NormalizedPen) {
            if ((LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) != 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                            "Using normalized pen transport for first-generation Intuos Pro %04x:%04x",
                            static_cast<unsigned int>(decision.vendor),
                            static_cast<unsigned int>(decision.product));
                m_LinuxWacomInput.reset(new LinuxWacomInput(requestTabletCursor));
                m_LinuxWacomInput->setActive(
                    (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
            }
            else {
                SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                             "Host lacks normalized pen support required by first-generation Intuos Pro %04x:%04x",
                             static_cast<unsigned int>(decision.vendor),
                             static_cast<unsigned int>(decision.product));
            }
        }
        else {
            m_LinuxRawWacomInput.reset(new LinuxRawWacomInput(requestTabletCursor));
            m_LinuxRawWacomInput->setActive(
                (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
        }
    }
    else if ((LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) != 0) {
        m_LinuxWacomInput.reset(new LinuxWacomInput(requestTabletCursor));
        m_LinuxWacomInput->setActive(
            (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
    }
#endif
}

void SdlInputHandler::setPresentationLayout(
        const PlankPresentationLayout& layout)
{
    m_PresentationLayout = layout;
    if (m_PresentationLayout.outputs.isEmpty() && m_Window != nullptr) {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(m_Window, &width, &height);
        m_PresentationLayout.canvasSize = QSize(qMax(1, width), qMax(1, height));
        m_PresentationLayout.outputs.append(
            {m_Window, QRect(QPoint(0, 0), m_PresentationLayout.canvasSize), true});
    }
    reconcileWaylandTabletCursorOutputs();
    updateTabletCursorVisibility();
    updatePointerRegionLock();
}

#ifdef HAVE_MAC_RAW_WACOM
bool SdlInputHandler::macRawHidAttached() const
{
    return m_MacRawWacomInput && m_MacRawWacomInput->isAttached();
}

void SdlInputHandler::syncMacNormalizedPenActive(bool captureAndFocus)
{
    if (m_MacNormalizedPen) {
        m_MacNormalizedPen->setActive(captureAndFocus && !macRawHidAttached());
    }
}
#endif

bool SdlInputHandler::ignorePenAsMouse(unsigned mouseId) const
{
#ifdef HAVE_MAC_RAW_WACOM
    return m_MacNormalizedPen && !macRawHidAttached() &&
            (m_MacNormalizedPen->isNear() ||
             MacNormalizedPenLogic::isPenMouse(mouseId));
#else
    (void)mouseId;
    return false;
#endif
}

bool SdlInputHandler::mapWindowPointToNormalized(
        SDL_Window* window, float windowX, float windowY,
        float& nx, float& ny, bool allowClamped) const
{
    const auto* output = presentationOutput(window);
    if (output == nullptr) {
        return false;
    }
    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    if (windowWidth <= 0 || windowHeight <= 0) {
        return false;
    }
    const QSize streamSize = streamDimensions();
    if (streamSize.width() <= 0 || streamSize.height() <= 0) {
        return false;
    }
    QPointF streamPoint;
    if (!PlankPresentation::mapWindowPointToStream(
                QPointF(windowX, windowY), QSize(windowWidth, windowHeight),
                streamSize,
                m_PresentationLayout.canvasSize, output->canvasRect,
                streamPoint, allowClamped)) {
        return false;
    }
    nx = std::max(0.0f, std::min(1.0f, static_cast<float>(
            streamPoint.x() / static_cast<qreal>(streamSize.width()))));
    ny = std::max(0.0f, std::min(1.0f, static_cast<float>(
            streamPoint.y() / static_cast<qreal>(streamSize.height()))));
    return true;
}

void SdlInputHandler::handlePenEvent(const SDL_Event& event)
{
#ifdef HAVE_MAC_RAW_WACOM
    if (macRawHidAttached()) {
        syncMacNormalizedPenActive(false);
        return;
    }
    if (!m_MacNormalizedPen || !isCaptureActive()) {
        return;
    }
    if (m_MacNormalizedPen->nativeEventsActive()) {
        return;
    }
    float x = 0.0f;
    float y = 0.0f;
    SDL_WindowID windowId = 0;
    bool hasPoint = false;
    bool requirePoint = true;
    switch (event.type) {
    case SDL_EVENT_PEN_PROXIMITY_IN:
    case SDL_EVENT_PEN_PROXIMITY_OUT:
        requirePoint = false;
        break;
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP:
        windowId = event.ptouch.windowID;
        x = event.ptouch.x;
        y = event.ptouch.y;
        hasPoint = true;
        requirePoint = event.type == SDL_EVENT_PEN_DOWN;
        break;
    case SDL_EVENT_PEN_MOTION:
        windowId = event.pmotion.windowID;
        x = event.pmotion.x;
        y = event.pmotion.y;
        hasPoint = true;
        break;
    case SDL_EVENT_PEN_AXIS:
        windowId = event.paxis.windowID;
        x = event.paxis.x;
        y = event.paxis.y;
        hasPoint = true;
        break;
    case SDL_EVENT_PEN_BUTTON_DOWN:
    case SDL_EVENT_PEN_BUTTON_UP:
        windowId = event.pbutton.windowID;
        x = event.pbutton.x;
        y = event.pbutton.y;
        hasPoint = true;
        requirePoint = event.type == SDL_EVENT_PEN_BUTTON_DOWN;
        break;
    default:
        return;
    }
    float nx = 0.0f;
    float ny = 0.0f;
    bool mapped = false;
    if (hasPoint) {
        SDL_Window* window = presentationWindow(windowId);
        if (window == nullptr) {
            window = m_Window;
        }
        mapped = window != nullptr &&
                mapWindowPointToNormalized(window, x, y, nx, ny, !requirePoint);
        if (!mapped && requirePoint) {
            return;
        }
    }
    switch (event.type) {
    case SDL_EVENT_PEN_PROXIMITY_IN:
        if (!mapped && !m_MacNormalizedPen->isNear()) {
            // Do not advertise a 0,0 tablet to the Host. Photoshop treats a
            // proximity-only device as still present when it quits.
            return;
        }
        m_MacNormalizedPen->handleProximity(true);
        break;
    case SDL_EVENT_PEN_PROXIMITY_OUT:
        m_MacNormalizedPen->handleProximity(false);
        break;
    case SDL_EVENT_PEN_DOWN:
        m_MacNormalizedPen->handleTip(true, event.ptouch.eraser, nx, ny);
        break;
    case SDL_EVENT_PEN_UP:
        if (mapped) {
            m_MacNormalizedPen->handleTip(false, event.ptouch.eraser, nx, ny);
        }
        else {
            m_MacNormalizedPen->handleTip(false, event.ptouch.eraser);
        }
        break;
    case SDL_EVENT_PEN_MOTION:
        m_MacNormalizedPen->handleMotion(event.pmotion.pen_state, nx, ny);
        break;
    case SDL_EVENT_PEN_AXIS:
        m_MacNormalizedPen->handleAxis(event.paxis.axis, event.paxis.value,
                                       event.paxis.pen_state, nx, ny);
        break;
    case SDL_EVENT_PEN_BUTTON_DOWN:
        m_MacNormalizedPen->handleButton(event.pbutton.button, true, nx, ny);
        break;
    case SDL_EVENT_PEN_BUTTON_UP:
        if (mapped) {
            m_MacNormalizedPen->handleButton(event.pbutton.button, false, nx, ny);
        }
        else {
            m_MacNormalizedPen->handleButton(event.pbutton.button, false);
        }
        break;
    default:
        break;
    }
#else
    (void)event;
#endif
}

void SdlInputHandler::refreshWaylandTabletCursorParents()
{
    if (!m_LocalCursorSupported) {
        return;
    }

    for (auto& output : m_WaylandTabletCursorOutputs) {
        output.cursor->setVisible(false);
        output.cursor->dispatchPending();
    }
    m_WaylandTabletCursorOutputs.clear();

    // SDL_CreateRenderer() may replace a Wayland window's native wl_surface.
    // A replacement proxy can reuse the same client address, so raw pointer
    // identity cannot reliably prove that an existing subsurface still has a
    // live parent. Rebuild from the final SDL windows after renderer setup.
    reconcileWaylandTabletCursorOutputs();
    updateTabletCursorVisibility();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Refreshed PLANK Wacom cursor parents for %zu presentation output%s",
                m_WaylandTabletCursorOutputs.size(),
                m_WaylandTabletCursorOutputs.size() == 1 ? "" : "s");
}

bool SdlInputHandler::handleRemoteCursorChunk(const unsigned char* data,
                                              unsigned int length)
{
    if (data == nullptr || length < sizeof(PLANK_CURSOR_WIRE_HEADER) ||
            length > sizeof(PLANK_CURSOR_WIRE_HEADER) + PLANK_CURSOR_MAX_CHUNK_SIZE) {
        return false;
    }

    PLANK_CURSOR_WIRE_HEADER wire;
    std::memcpy(&wire, data, sizeof(wire));
    const std::uint32_t magic = qFromLittleEndian(wire.magic);
    const std::uint16_t version = qFromLittleEndian(wire.version);
    const std::uint16_t pixelFormat = qFromLittleEndian(wire.pixelFormat);
    const std::uint32_t flags = qFromLittleEndian(wire.flags);
    const std::uint64_t generation = qFromLittleEndian(wire.generation);
    const std::uint32_t width = qFromLittleEndian(wire.width);
    const std::uint32_t height = qFromLittleEndian(wire.height);
    const std::uint32_t hotspotX = qFromLittleEndian(wire.hotspotX);
    const std::uint32_t hotspotY = qFromLittleEndian(wire.hotspotY);
    const std::uint32_t imageSize = qFromLittleEndian(wire.imageSize);
    const std::uint32_t chunkOffset = qFromLittleEndian(wire.chunkOffset);
    const std::uint32_t chunkSize = qFromLittleEndian(wire.chunkSize);
    constexpr std::uint32_t knownFlags = PLANK_CURSOR_FLAG_VISIBLE |
                                          PLANK_CURSOR_FLAG_FIRST_CHUNK |
                                          PLANK_CURSOR_FLAG_LAST_CHUNK;

    if (magic != PLANK_CURSOR_WIRE_MAGIC || version != PLANK_CURSOR_WIRE_VERSION ||
            pixelFormat != PLANK_CURSOR_PIXEL_FORMAT_ARGB8888 ||
            (flags & ~knownFlags) != 0 || width == 0 || height == 0 ||
            width > PLANK_CURSOR_MAX_DIMENSION || height > PLANK_CURSOR_MAX_DIMENSION ||
            imageSize != width * height * 4U || imageSize > PLANK_CURSOR_MAX_IMAGE_SIZE ||
            hotspotX >= width || hotspotY >= height ||
            chunkSize > PLANK_CURSOR_MAX_CHUNK_SIZE ||
            sizeof(wire) + chunkSize != length ||
            chunkOffset > imageSize || chunkSize > imageSize - chunkOffset) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "Rejected malformed PLANK local cursor chunk");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_RemoteCursorMutex);
    if ((flags & PLANK_CURSOR_FLAG_FIRST_CHUNK) != 0) {
        if (chunkOffset != 0) {
            return false;
        }
        m_RemoteCursorAssembly = {};
        m_RemoteCursorAssembly.generation = generation;
        m_RemoteCursorAssembly.width = width;
        m_RemoteCursorAssembly.height = height;
        m_RemoteCursorAssembly.hotspotX = hotspotX;
        m_RemoteCursorAssembly.hotspotY = hotspotY;
        m_RemoteCursorAssembly.flags = flags & PLANK_CURSOR_FLAG_VISIBLE;
        m_RemoteCursorAssembly.pixels.resize(imageSize);
        m_RemoteCursorAssemblyActive = true;
    }

    if (!m_RemoteCursorAssemblyActive ||
            generation != m_RemoteCursorAssembly.generation ||
            width != m_RemoteCursorAssembly.width ||
            height != m_RemoteCursorAssembly.height ||
            hotspotX != m_RemoteCursorAssembly.hotspotX ||
            hotspotY != m_RemoteCursorAssembly.hotspotY ||
            (flags & PLANK_CURSOR_FLAG_VISIBLE) != m_RemoteCursorAssembly.flags ||
            chunkOffset != m_RemoteCursorAssembly.nextOffset) {
        m_RemoteCursorAssemblyActive = false;
        return false;
    }

    std::memcpy(m_RemoteCursorAssembly.pixels.data() + chunkOffset,
                data + sizeof(wire), chunkSize);
    m_RemoteCursorAssembly.nextOffset += chunkSize;

    if ((flags & PLANK_CURSOR_FLAG_LAST_CHUNK) == 0) {
        return false;
    }
    if (m_RemoteCursorAssembly.nextOffset != imageSize) {
        m_RemoteCursorAssemblyActive = false;
        return false;
    }

    m_ReadyRemoteCursor = std::move(m_RemoteCursorAssembly);
    m_ReadyRemoteCursorValid = true;
    m_RemoteCursorAssemblyActive = false;
    return !m_RemoteCursorUpdatePending.exchange(true);
}

void SdlInputHandler::applyPendingRemoteCursor()
{
    RemoteCursorState cursor;
    {
        std::lock_guard<std::mutex> lock(m_RemoteCursorMutex);
        if (!m_ReadyRemoteCursorValid) {
            m_RemoteCursorUpdatePending.store(false);
            return;
        }
        cursor = std::move(m_ReadyRemoteCursor);
        m_ReadyRemoteCursorValid = false;
        m_RemoteCursorUpdatePending.store(false);
    }

    SDL_Surface* surface = SDL_CreateSurfaceFrom(
                static_cast<int>(cursor.width),
                static_cast<int>(cursor.height),
                SDL_PIXELFORMAT_ARGB8888,
                cursor.pixels.data(),
                static_cast<int>(cursor.width * 4U));
    if (surface == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "Failed to create PLANK cursor surface: %s",
                    SDL_GetError());
        return;
    }

    SDL_Cursor* replacement = SDL_CreateColorCursor(
                surface,
                static_cast<int>(cursor.hotspotX),
                static_cast<int>(cursor.hotspotY));
    SDL_DestroySurface(surface);
    if (replacement == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "Failed to create PLANK compositor cursor: %s",
                    SDL_GetError());
        return;
    }

    const bool firstCursor = m_RemoteCursor == nullptr;
    SDL_Cursor* previous = m_RemoteCursor;
    m_RemoteCursor = replacement;
    m_RemoteCursorVisible =
            (cursor.flags & PLANK_CURSOR_FLAG_VISIBLE) != 0;
    m_AppliedRemoteCursor = cursor;
    m_AppliedRemoteCursorValid = true;
    reconcileWaylandTabletCursorOutputs();
    const QImage cursorImage(
                m_AppliedRemoteCursor.pixels.data(),
                static_cast<int>(m_AppliedRemoteCursor.width),
                static_cast<int>(m_AppliedRemoteCursor.height),
                static_cast<int>(m_AppliedRemoteCursor.width * 4U),
                QImage::Format_ARGB32_Premultiplied);
    for (auto& output : m_WaylandTabletCursorOutputs) {
        output.cursor->setImage(
                cursorImage,
                static_cast<int>(m_AppliedRemoteCursor.hotspotX),
                static_cast<int>(m_AppliedRemoteCursor.hotspotY));
        output.cursor->dispatchPending();
    }
    updateTabletCursorVisibility();
    syncCompositorCursor();
    if (isCaptureActive()) {
        if (!m_RemoteCursorVisible && m_TabletCursorActive && previous != nullptr) {
            SDL_DestroyCursor(m_RemoteCursor);
            m_RemoteCursor = previous;
            previous = nullptr;
            m_RemoteCursorVisible = true;
            m_HoldLocalPointer = true;
            activateCompositorCursor();
            setCursorVisible(true);
        }
        else if (m_HoldLocalPointer && !m_RemoteCursorVisible && previous != nullptr) {
            SDL_DestroyCursor(m_RemoteCursor);
            m_RemoteCursor = previous;
            previous = nullptr;
            m_RemoteCursorVisible = true;
            setCursorVisible(true);
        }
        else {
            if (m_RemoteCursorVisible) {
                m_HoldLocalPointer = false;
            }
            setCursorVisible(!m_MouseWasInVideoRegion || m_RemoteCursorVisible);
        }
    }
    if (previous != nullptr) {
        SDL_DestroyCursor(previous);
    }

    if (firstCursor) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Applied first PLANK local cursor (%ux%u hotspot %u,%u visible=%d)",
                    cursor.width, cursor.height, cursor.hotspotX, cursor.hotspotY,
                    m_RemoteCursorVisible ? 1 : 0);
    }
    else {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT,
                     "Applied PLANK local cursor generation %llu (%ux%u hotspot %u,%u visible=%d)",
                     static_cast<unsigned long long>(cursor.generation),
                     cursor.width, cursor.height, cursor.hotspotX, cursor.hotspotY,
                     m_RemoteCursorVisible ? 1 : 0);
    }
}

bool SdlInputHandler::handleRemoteCursorPosition(
        const unsigned char* data, unsigned int length)
{
    if (data == nullptr || length != sizeof(PLANK_CURSOR_POSITION_WIRE_MESSAGE)) {
        return false;
    }

    PLANK_CURSOR_POSITION_WIRE_MESSAGE wire;
    std::memcpy(&wire, data, sizeof(wire));
    RemoteCursorPosition position;
    const std::uint32_t magic = qFromLittleEndian(wire.magic);
    const std::uint16_t version = qFromLittleEndian(wire.version);
    const std::uint16_t reserved = qFromLittleEndian(wire.reserved);
    position.sequence = qFromLittleEndian(wire.sequence);
    position.x = qFromLittleEndian(wire.x);
    position.y = qFromLittleEndian(wire.y);
    position.frameWidth = qFromLittleEndian(wire.frameWidth);
    position.frameHeight = qFromLittleEndian(wire.frameHeight);

    const QSize streamSize = streamDimensions();
    if (magic != PLANK_CURSOR_POSITION_WIRE_MAGIC ||
            version != PLANK_CURSOR_POSITION_WIRE_VERSION || reserved != 0 ||
            position.sequence == 0 || position.frameWidth == 0 ||
            position.frameHeight == 0 || position.x >= position.frameWidth ||
            position.y >= position.frameHeight ||
            position.frameWidth != static_cast<std::uint32_t>(streamSize.width()) ||
            position.frameHeight != static_cast<std::uint32_t>(streamSize.height())) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "Rejected malformed PLANK cursor position");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_RemoteCursorPositionMutex);
    if (position.sequence <= m_HighestRemoteCursorPositionSequence) {
        return false;
    }
    m_HighestRemoteCursorPositionSequence = position.sequence;
    m_ReadyRemoteCursorPosition = position;
    m_ReadyRemoteCursorPositionValid = true;
    return !m_RemoteCursorPositionUpdatePending.exchange(true);
}

void SdlInputHandler::applyPendingRemoteCursorPosition()
{
    RemoteCursorPosition position;
    {
        std::lock_guard<std::mutex> lock(m_RemoteCursorPositionMutex);
        if (!m_ReadyRemoteCursorPositionValid) {
            m_RemoteCursorPositionUpdatePending.store(false);
            return;
        }
        position = m_ReadyRemoteCursorPosition;
        m_ReadyRemoteCursorPositionValid = false;
        m_RemoteCursorPositionUpdatePending.store(false);
    }

    m_AppliedRemoteCursorPosition = position;
    m_AppliedRemoteCursorPositionValid = true;
    m_AppliedRemoteCursorPositionSequence = position.sequence;
    SDL_Window* targetWindow = nullptr;
    int x = 0;
    int y = 0;
    if (!mapRemoteCursorPositionToWindow(position, targetWindow, x, y)) {
        return;
    }
#ifdef HAVE_MAC_RAW_WACOM
    updateTabletCursorVisibility();
    if (m_MacRawWacomInput) {
        followPointerFocus(targetWindow, 0, PointerFocusPosition::HostTablet);
    }
#endif
    PlankWaylandCursor* cursor =
            ensureWaylandTabletCursorAttached(targetWindow);
    if (cursor == nullptr) {
        return;
    }
    cursor->setPosition(x, y);
    cursor->dispatchPending();
    updateTabletCursorVisibility();
}

void SdlInputHandler::applyPendingTabletCursorActivation()
{
    // A mouse or toolbar event can cancel tablet ownership after this SDL event
    // was queued. Never let that stale event steal ownership back.
    if (!m_TabletCursorActivationPending.load()) {
        return;
    }
    if (!m_LocalCursorSupported) {
        // Mac Host has no independent tablet cursor image. Hiding here
        // leaves no pointer at all until macOS shake-to-locate.
        m_TabletCursorActivationPending.store(false);
        if (isCaptureActive()) {
            setCursorVisible(m_MouseCursorCapturedVisibilityState);
        }
        return;
    }
    reconcileWaylandTabletCursorOutputs();
    if (!m_LocalCursorSupported || !isCaptureActive() ||
            (m_WaylandTabletCursorOutputs.empty()
#ifdef HAVE_MAC_RAW_WACOM
             && !m_MacRawWacomInput
#endif
             )) {
        m_TabletCursorActivationPending.store(false);
        return;
    }

    if (!m_TabletCursorActive) {
        m_HoldLocalPointer = false;
        m_TabletCursorActive = true;
        m_TabletCursorActivationSequence =
                m_AppliedRemoteCursorPositionSequence;
        syncCompositorCursor();
        updateTabletCursorVisibility();
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT,
                     "Switched to host-authoritative Wacom cursor position");
    }
}

void SdlInputHandler::raiseAllKeys()
{
    if (m_KeysDown.isEmpty()) {
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Raising %d keys",
                (int)m_KeysDown.count());

    for (auto keyDown : std::as_const(m_KeysDown)) {
        LiSendKeyboardEvent(keyDown, KEY_ACTION_UP, 0);
    }

    m_KeysDown.clear();
}

void SdlInputHandler::notifyMouseLeave()
{
    // SDL on Windows doesn't send the mouse button up until the mouse re-enters the window
    // after leaving it. This breaks some of the Aero snap gestures, so we'll capture it to
    // allow us to receive the mouse button up events later.
    //
    // On macOS and X11, capturing the mouse allows us to receive mouse motion outside the
    // window (button up already worked without capture).
    if (m_NeedsManualCaptureOnLeave && isCaptureActive()) {
        // NB: Not using SDL_GetGlobalMouseState() because we want our state not the system's
        Uint32 mouseState = SDL_GetMouseState(nullptr, nullptr);
        for (Uint32 button = SDL_BUTTON_LEFT; button <= SDL_BUTTON_X2; button++) {
            if (mouseState & SDL_BUTTON_MASK(button)) {
                SDL_CaptureMouse(true);
                break;
            }
        }
    }
}

void SdlInputHandler::notifyFocusLost()
{
#ifdef Q_OS_MACOS
    m_MacKeyboardCapture->refresh();
    m_MacQuitShortcut->refresh();
#endif
    activateCompositorCursor();
#ifdef HAVE_MAC_RAW_WACOM
    if (m_MacRawWacomInput) m_MacRawWacomInput->setActive(false);
    if (m_MacNormalizedPen) m_MacNormalizedPen->setActive(false);
#endif
#ifdef HAVE_LIBINPUT_TABLET
    if (m_LinuxWacomInput) {
        m_LinuxWacomInput->setActive(false);
    }
    if (m_LinuxRawWacomInput) {
        m_LinuxRawWacomInput->setActive(false);
    }
#endif

    // Raise all keys that are currently pressed. If we don't do this, certain keys
    // used in shortcuts that cause focus loss (such as Alt+Tab) may get stuck down.
    raiseAllKeys();
}

void SdlInputHandler::notifyFocusGained()
{
#ifdef Q_OS_MACOS
    m_MacKeyboardCapture->refresh();
    m_MacQuitShortcut->refresh();
#endif
#ifdef HAVE_MAC_RAW_WACOM
    if (m_MacRawWacomInput) m_MacRawWacomInput->setActive(isCaptureActive());
    syncMacNormalizedPenActive(isCaptureActive());
#endif
#ifdef HAVE_LIBINPUT_TABLET
    if (m_LinuxWacomInput) {
        m_LinuxWacomInput->setActive(true);
    }
    if (m_LinuxRawWacomInput) {
        m_LinuxRawWacomInput->setActive(true);
    }
#endif
}

void SdlInputHandler::handleRawHidControl(const unsigned char* data,
                                          unsigned int length)
{
#ifdef HAVE_MAC_RAW_WACOM
    if (m_MacRawWacomInput) m_MacRawWacomInput->handleControl(data, length);
#elif defined(HAVE_LIBINPUT_TABLET)
    if (m_LinuxRawWacomInput) {
        m_LinuxRawWacomInput->handleControl(data, length);
    }
#else
    Q_UNUSED(data);
    Q_UNUSED(length);
#endif
}

void SdlInputHandler::beginRawHidReconnect()
{
#ifdef HAVE_MAC_RAW_WACOM
    if (m_MacRawWacomInput) m_MacRawWacomInput->beginReconnect();
#endif
#ifdef HAVE_LIBINPUT_TABLET
    if (m_LinuxRawWacomInput) {
        m_LinuxRawWacomInput->beginReconnect();
    }
#endif
}

void SdlInputHandler::finishRawHidReconnect()
{
#ifdef HAVE_MAC_RAW_WACOM
    if (m_MacRawWacomInput) m_MacRawWacomInput->finishReconnect();
#endif
#ifdef HAVE_LIBINPUT_TABLET
    if (m_LinuxRawWacomInput) {
        m_LinuxRawWacomInput->finishReconnect();
    }
#endif
}

void SdlInputHandler::resetRemoteCursorPositionEpoch()
{
    // The login-screen and authenticated-desktop media workers each start
    // their cursor position sequence at one. The input handler survives that
    // handoff, so discard the old worker's high-water mark once its transport
    // has stopped or the new worker's positions will be rejected until its
    // counter catches up.
    {
        std::lock_guard<std::mutex> lock(m_RemoteCursorPositionMutex);
        m_ReadyRemoteCursorPosition = {};
        m_ReadyRemoteCursorPositionValid = false;
        m_HighestRemoteCursorPositionSequence = 0;
        m_RemoteCursorPositionUpdatePending.store(false);
    }

    m_AppliedRemoteCursorPosition = {};
    m_AppliedRemoteCursorPositionValid = false;
    m_AppliedRemoteCursorPositionSequence = 0;
    m_TabletCursorActivationSequence = 0;
    m_TabletCursorActivationPending.store(false);
    for (auto& output : m_WaylandTabletCursorOutputs) {
        output.cursor->setVisible(false);
        output.cursor->dispatchPending();
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                "Reset PLANK remote cursor position epoch for desktop reconnect");
}

bool SdlInputHandler::isCaptureActive()
{
    return m_FakeMouseCaptureActive;
}

void SdlInputHandler::setToolbarInteractionActive(bool active)
{
    if (active) {
        activateCompositorCursor();
    }
    setCursorVisible(active ? true :
                     (!m_MouseWasInVideoRegion ||
                      (m_LocalCursorSupported ? m_RemoteCursorVisible :
                                                m_MouseCursorCapturedVisibilityState)));
}

void SdlInputHandler::setLocalToolbarAvailable(bool available)
{
    if (m_LocalToolbarAvailable == available) {
        return;
    }

    m_LocalToolbarAvailable = available;
    updatePointerRegionLock();
}

void SdlInputHandler::updateKeyboardGrabState()
{
    bool shouldGrab = m_CaptureSystemKeysMode != StreamingPreferences::CSK_OFF && isCaptureActive();
    if (shouldGrab) {
        Uint32 windowFlags = SDL_GetWindowFlags(m_Window);
        if (m_CaptureSystemKeysMode == StreamingPreferences::CSK_FULLSCREEN &&
            !(windowFlags & SDL_WINDOW_FULLSCREEN)) {
            // Ungrab if it's fullscreen only and we left fullscreen
            shouldGrab = false;
        }
    }

    // Don't close the window on Alt+F4 when keyboard grab is enabled
    SDL_SetHint(SDL_HINT_WINDOWS_CLOSE_ON_ALT_F4, shouldGrab ? "0" : "1");

#ifndef Q_OS_MACOS
    for (const auto& output : m_PresentationLayout.outputs) {
        SDL_SetWindowKeyboardGrab(output.window, shouldGrab ? true : false);
    }
#endif

    m_KeyboardCaptureActive = shouldGrab;
#ifdef Q_OS_MACOS
    m_MacKeyboardCapture->refresh();
    m_MacQuitShortcut->refresh();
#endif
}

bool SdlInputHandler::isSystemKeyCaptureActive()
{
    if (m_CaptureSystemKeysMode == StreamingPreferences::CSK_OFF) {
        return false;
    }

    if (m_Window == nullptr) {
        return false;
    }

    // NB: We used to check SDL_WINDOW_KEYBOARD_GRABBED here, but this isn't
    // always set when capture "fails" on SDL3, even though the user may have
    // configured the compositor to pass through system keys to us anyway.
    // See issues #1776 and #1900 for details.
    bool focused = false;
    bool fullscreen = false;
    for (const auto& output : m_PresentationLayout.outputs) {
        const Uint32 windowFlags = SDL_GetWindowFlags(output.window);
#ifdef Q_OS_MACOS
        // AppKit can return to a fullscreen Space before SDL's cached focus
        // flags recover. Do not require both native and cached focus to agree.
        focused = focused || MacWindow::hasKeyboardFocus(output.window);
#else
        focused = focused || (windowFlags & SDL_WINDOW_INPUT_FOCUS);
#endif
        fullscreen = fullscreen || (windowFlags & SDL_WINDOW_FULLSCREEN);
    }
    if (!focused || !m_KeyboardCaptureActive) {
        return false;
    }

    if (m_CaptureSystemKeysMode == StreamingPreferences::CSK_FULLSCREEN &&
            !fullscreen) {
        return false;
    }

    return true;
}

void SdlInputHandler::setCaptureActive(bool active)
{
#ifdef HAVE_MAC_RAW_WACOM
    SDL_Window* macFocus = SDL_GetKeyboardFocus();
    const bool macCaptureAndFocus = active && macFocus &&
            presentationWindow(SDL_GetWindowID(macFocus)) != nullptr;
    if (m_MacRawWacomInput) {
        m_MacRawWacomInput->setActive(macCaptureAndFocus);
    }
    syncMacNormalizedPenActive(macCaptureAndFocus);
#endif
    if (active) {
        setCursorVisible(m_LocalCursorSupported ?
                             (!m_MouseWasInVideoRegion || m_RemoteCursorVisible) :
                             m_MouseCursorCapturedVisibilityState);
        m_FakeMouseCaptureActive = true;

        // Synchronize the client and host cursor when activating absolute capture
        float mouseX, mouseY;
        int windowX, windowY;

        // We have to use SDL_GetGlobalMouseState() because macOS may not reflect
        // the new position of the mouse when outside the window.
        SDL_GetGlobalMouseState(&mouseX, &mouseY);

        // Convert global mouse state to window-relative
        SDL_GetWindowPosition(m_Window, &windowX, &windowY);
        mouseX -= windowX;
        mouseY -= windowY;

        if (isMouseInVideoRegion(mouseX, mouseY,
                                 SDL_GetWindowID(m_Window))) {
            // Synthesize a mouse event to synchronize the cursor
            SDL_MouseMotionEvent motionEvent = {};
            motionEvent.type = SDL_EVENT_MOUSE_MOTION;
            motionEvent.timestamp = SDL_GetTicksNS();
            motionEvent.windowID = SDL_GetWindowID(m_Window);
            motionEvent.x = mouseX;
            motionEvent.y = mouseY;
            handleMouseMotionEvent(&motionEvent);
        }
    }
    else {
        activateCompositorCursor();
        setCursorVisible(true);
        m_FakeMouseCaptureActive = false;
    }

    // Update mouse pointer region constraints
    updatePointerRegionLock();

    // Now update the keyboard grab
    updateKeyboardGrabState();
}

void SdlInputHandler::setCursorVisible(bool visible)
{
    m_CompositorCursorRequestedVisible = visible;
    if (!m_LocalCursorSupported && !m_EmbeddedCursor.setVisible(visible)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "Unable to update embedded-session cursor image: %s", SDL_GetError());
    }
    syncCompositorCursor();
    updateTabletCursorVisibility();
}

SDL_Cursor* SdlInputHandler::blankCursor()
{
    if (m_BlankCursor != nullptr) {
        return m_BlankCursor;
    }

    std::uint32_t pixel = 0;
    SDL_Surface* surface = SDL_CreateSurfaceFrom(
                1, 1, SDL_PIXELFORMAT_ARGB8888, &pixel, 4);
    if (surface == nullptr) {
        return nullptr;
    }
    m_BlankCursor = SDL_CreateColorCursor(surface, 0, 0);
    SDL_DestroySurface(surface);
    return m_BlankCursor;
}

void SdlInputHandler::syncCompositorCursor()
{
    if (m_TabletCursorActive) {
        // A Wayland compositor keeps painting the last pointer image after
        // SDL_HideCursor(). The transparent cursor is what removes the mouse
        // arrow while the pen cursor is on its own surface.
        if (SDL_Cursor* blank = blankCursor()) {
            SDL_SetCursor(blank);
        }
        SDL_HideCursor();
        return;
    }

    if (m_RemoteCursor != nullptr) {
        SDL_SetCursor(m_RemoteCursor);
    }
    if (m_CompositorCursorRequestedVisible) {
        SDL_ShowCursor();
    }
    else {
        SDL_HideCursor();
    }
}

void SdlInputHandler::activateCompositorCursor()
{
    m_TabletCursorActivationPending.store(false);
    if (!m_TabletCursorActive) {
        return;
    }

    m_TabletCursorActive = false;
#ifdef HAVE_MAC_RAW_WACOM
    for (const auto& output : m_PresentationLayout.outputs)
        MacWindow::hideTabletCursor(output.window);
    MacWindow::hideTabletCursor(m_Window);
#endif
    for (auto& output : m_WaylandTabletCursorOutputs) {
        output.cursor->setVisible(false);
        output.cursor->dispatchPending();
    }
    syncCompositorCursor();
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT,
                 "Restored Wayland compositor cursor for mouse input");
}

bool SdlInputHandler::mapRemoteCursorPositionToWindow(
        const RemoteCursorPosition& position, SDL_Window*& window,
        int& x, int& y) const
{
    if (m_Window == nullptr || position.frameWidth == 0 ||
            position.frameHeight == 0) {
        return false;
    }

    const QPointF streamPoint(position.x, position.y);
    for (const auto& output : m_PresentationLayout.outputs) {
        int windowWidth = 0;
        int windowHeight = 0;
        SDL_GetWindowSize(output.window, &windowWidth, &windowHeight);
        QPointF windowPoint;
        if (PlankPresentation::mapStreamPointToWindow(
                    streamPoint,
                    QSize(position.frameWidth, position.frameHeight),
                    m_PresentationLayout.canvasSize,
                    output.canvasRect,
                    QSize(windowWidth, windowHeight),
                    windowPoint)) {
            window = output.window;
            x = qRound(windowPoint.x());
            y = qRound(windowPoint.y());
            return true;
        }
    }
    return false;
}

PlankWaylandCursor*
SdlInputHandler::ensureWaylandTabletCursorAttached(SDL_Window* targetWindow)
{
    const char* driver = SDL_GetCurrentVideoDriver();
    if (!m_LocalCursorSupported || targetWindow == nullptr ||
            driver == nullptr || SDL_strcmp(driver, "wayland") != 0) {
        return nullptr;
    }

    auto existing = std::find_if(
            m_WaylandTabletCursorOutputs.begin(),
            m_WaylandTabletCursorOutputs.end(),
            [targetWindow](const WaylandTabletCursorOutput& output) {
                return output.window == targetWindow;
            });
    if (existing != m_WaylandTabletCursorOutputs.end() &&
            existing->cursor->isAttachedTo(targetWindow)) {
        return existing->cursor.get();
    }

    const bool replacing = existing != m_WaylandTabletCursorOutputs.end();
    if (replacing) {
        existing->cursor->setVisible(false);
        existing->cursor->dispatchPending();
        existing->cursor.reset();
    }

    std::unique_ptr<PlankWaylandCursor> cursor =
            PlankWaylandCursor::create(targetWindow);
    if (!cursor) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to attach PLANK Wayland Wacom cursor surface");
        if (replacing) {
            m_WaylandTabletCursorOutputs.erase(existing);
        }
        return nullptr;
    }

    if (m_AppliedRemoteCursorValid) {
        const QImage cursorImage(
                    m_AppliedRemoteCursor.pixels.data(),
                    static_cast<int>(m_AppliedRemoteCursor.width),
                    static_cast<int>(m_AppliedRemoteCursor.height),
                    static_cast<int>(m_AppliedRemoteCursor.width * 4U),
                    QImage::Format_ARGB32_Premultiplied);
        cursor->setImage(
                    cursorImage,
                    static_cast<int>(m_AppliedRemoteCursor.hotspotX),
                    static_cast<int>(m_AppliedRemoteCursor.hotspotY));
    }
    SDL_Window* positionWindow = nullptr;
    int positionX = 0;
    int positionY = 0;
    if (m_AppliedRemoteCursorPositionValid) {
        if (mapRemoteCursorPositionToWindow(
                    m_AppliedRemoteCursorPosition,
                    positionWindow, positionX, positionY) &&
                positionWindow == targetWindow) {
            cursor->setPosition(positionX, positionY);
        }
    }
    const bool visible = m_TabletCursorActive && isCaptureActive() &&
            m_RemoteCursorVisible && m_AppliedRemoteCursorPositionSequence >
                m_TabletCursorActivationSequence &&
            positionWindow == targetWindow;
    cursor->setVisible(visible);
    cursor->dispatchPending();

    PlankWaylandCursor* result = cursor.get();
    if (replacing) {
        existing->cursor = std::move(cursor);
    }
    else {
        m_WaylandTabletCursorOutputs.push_back(
                {targetWindow, std::move(cursor)});
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                replacing ?
                    "Reattached PLANK Wacom cursor to replacement Wayland parent surface for window %u" :
                    "PLANK Wayland Wacom cursor surface enabled for window %u",
                SDL_GetWindowID(targetWindow));
    return result;
}

void SdlInputHandler::reconcileWaylandTabletCursorOutputs()
{
    if (!m_LocalCursorSupported) {
        return;
    }

    const auto containsWindow = [this](SDL_Window* window) {
        if (m_PresentationLayout.outputs.isEmpty()) {
            return window == m_Window;
        }
        return std::any_of(
                m_PresentationLayout.outputs.cbegin(),
                m_PresentationLayout.outputs.cend(),
                [window](const PlankPresentationOutput& output) {
                    return output.window == window;
                });
    };
    auto output = m_WaylandTabletCursorOutputs.begin();
    while (output != m_WaylandTabletCursorOutputs.end()) {
        if (containsWindow(output->window)) {
            ++output;
            continue;
        }
        output->cursor->setVisible(false);
        output->cursor->dispatchPending();
        output = m_WaylandTabletCursorOutputs.erase(output);
    }

    if (m_PresentationLayout.outputs.isEmpty()) {
        ensureWaylandTabletCursorAttached(m_Window);
        return;
    }
    for (const auto& presentationOutput : m_PresentationLayout.outputs) {
        ensureWaylandTabletCursorAttached(presentationOutput.window);
    }
}

void SdlInputHandler::updateTabletCursorVisibility()
{
    reconcileWaylandTabletCursorOutputs();

    SDL_Window* positionWindow = nullptr;
    int x = 0;
    int y = 0;
    if (m_AppliedRemoteCursorPositionValid) {
        mapRemoteCursorPositionToWindow(
                m_AppliedRemoteCursorPosition,
                positionWindow, x, y);
    }
    const bool visible = m_TabletCursorActive && isCaptureActive() &&
            m_RemoteCursorVisible &&
            m_AppliedRemoteCursorPositionSequence >
                m_TabletCursorActivationSequence;
#ifdef HAVE_MAC_RAW_WACOM
    for (const auto& output : m_PresentationLayout.outputs) {
        if (output.window != positionWindow) MacWindow::hideTabletCursor(output.window);
    }
    if (positionWindow && m_AppliedRemoteCursorValid) {
        const auto& image = m_AppliedRemoteCursor;
        MacWindow::tabletCursor(positionWindow, image.pixels.data(), image.width, image.height,
            image.hotspotX, image.hotspotY, image.generation, x, y, visible);
    }
#endif
    for (auto& output : m_WaylandTabletCursorOutputs) {
        output.cursor->setVisible(visible && output.window == positionWindow);
        output.cursor->dispatchPending();
    }
}
