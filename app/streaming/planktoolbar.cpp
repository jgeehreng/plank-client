#include "planktoolbar.h"
#include "planktoolbarstats.h"
#include "plankwaylandtoolbar.h"
#include "videopacketlosswindow.h"

#include "input/input.h"
#include "settings/streamingpreferences.h"
#include "video/overlaymanager.h"

#include <Limelight.h>
#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#ifdef Q_OS_DARWIN
#include "macwindow.h"
#endif

#include <algorithm>
#include <cmath>
#include <utility>

#ifdef HAS_WAYLAND
#include <linux/input-event-codes.h>
#endif

namespace {
constexpr int ToolbarHeight = 39;
constexpr int EdgeRevealHeight = 3;
constexpr Uint32 EdgeActivationDelayMs = 1000;
constexpr Uint32 AutoHideDelayMs = 5000;
constexpr Uint32 BitrateSettleDelayMs = 250;
constexpr Uint32 BitrateAcknowledgementRetryMs = 1000;
constexpr Uint32 RedrawIntervalMs = 200;
constexpr Uint32 ToolbarMoveRedrawIntervalMs = 16;
constexpr int BitrateMinimumKbps =
        StreamingPreferences::PlankBitrateMinimumKbps;
constexpr int BitrateMaximumKbps =
        StreamingPreferences::PlankBitrateMaximumKbps;
constexpr int BitrateStepKbps =
        StreamingPreferences::PlankBitrateStepKbps;
constexpr qreal WindowGlyphScale = 0.85;
constexpr qreal WindowButtonSize = 28.0 * WindowGlyphScale;
constexpr qreal WindowButtonRadius = 5.0 * WindowGlyphScale;
constexpr int ReconnectPromptWidth = 460;
constexpr int ReconnectPromptHeight = 156;
constexpr int ReconnectPromptNoButton = 0;
constexpr int ReconnectPromptDisconnectButton = 1;
constexpr int ReconnectPromptWaitButton = 2;

QColor interpolateColor(const QColor& start, const QColor& end, qreal fraction)
{
    fraction = qBound(0.0, fraction, 1.0);
    return QColor(qRound(start.red() + (end.red() - start.red()) * fraction),
                  qRound(start.green() + (end.green() - start.green()) * fraction),
                  qRound(start.blue() + (end.blue() - start.blue()) * fraction));
}

QColor packetLossColor(float packetLossPercent)
{
    const QColor blue(52, 132, 228);
    const QColor green(52, 199, 110);
    const QColor red(239, 88, 88);
    const qreal clampedLoss = qBound(0.0, qreal(packetLossPercent), 10.0);

    if (clampedLoss <= 5.0) {
        return interpolateColor(blue, green, clampedLoss / 5.0);
    }
    return interpolateColor(green, red, (clampedLoss - 5.0) / 5.0);
}
}

PlankToolbar::PlankToolbar(
        SDL_Window* window,
        Overlay::OverlayManager& overlayManager,
        SdlInputHandler& inputHandler,
        StreamingPreferences& preferences,
        int initialBitrateKbps)
    : m_Window(window),
      m_OverlayManager(overlayManager),
      m_InputHandler(inputHandler),
      m_Preferences(preferences),
      m_PendingAction(Action::None),
      m_Visible(preferences.plankToolbarPinned),
      m_Pinned(preferences.plankToolbarPinned),
      m_DraggingToolbar(false),
      m_DraggingSlider(false),
      m_PointerInside(false),
      m_PointerInitialized(false),
      m_LocalPointerInteraction(false),
      m_BitrateSupported(
              (LiGetHostFeatureFlags() &
               (LI_FF_DYNAMIC_VIDEO_BITRATE | LI_FF_ENCODER_TARGET_ACK)) ==
              (LI_FF_DYNAMIC_VIDEO_BITRATE | LI_FF_ENCODER_TARGET_ACK)),
      m_ReconnectPromptVisible(false),
      m_ReconnectPromptPointerInside(false),
      m_ReconnectPromptButtonDown(false),
      m_ReconnectPromptPointerX(0),
      m_ReconnectPromptPointerY(0),
      m_ReconnectPromptPressedButton(ReconnectPromptNoButton),
      m_ReconnectPromptSeconds(0),
      m_WindowWidth(0),
      m_WindowHeight(0),
      m_WindowPixelWidth(0),
      m_WindowPixelHeight(0),
      m_PixelDensity(1.0f),
      m_EncoderTargetWidth(PlankToolbarStats::encoderTargetWidth(
              BitrateMinimumKbps, BitrateMaximumKbps, BitrateStepKbps)),
      m_Width(PlankToolbarStats::EncoderTargetLeft + m_EncoderTargetWidth +
              PlankToolbarStats::WindowControlsWidth),
      m_ToolbarLeft(-1),
      m_ToolbarDragOffsetX(0),
      m_PointerX(0),
      m_PointerY(0),
      m_PressedControl(Control::None),
      m_BitrateKbps(qBound(BitrateMinimumKbps,
                           initialBitrateKbps,
                           BitrateMaximumKbps)),
      m_LastSentBitrateKbps(-1),
      m_AppliedBitrateKbps(-1),
      m_AppliedPeakKbps(-1),
      m_RenderedFps(0.0f),
      m_VideoMbps(0.0f),
      m_PacketLossPercent(-1.0f),
      m_NetworkRttMs(0),
      m_LastDrawnFps(-1.0f),
      m_LastDrawnVideoMbps(-1.0f),
      m_LastDrawnPacketLossPercent(-2.0f),
      m_LastDrawnNetworkRttMs(0),
      m_HideDeadline(0),
      m_LastBitrateSendTime(0),
      m_LastBitrateChangeTime(0),
      m_LastToolbarMoveDrawTime(0),
      m_LastRedrawTime(0),
      m_EdgeHoverStartTime(0)
{
    createWaylandToolbar();
    createWaylandReconnectPrompt();

    m_InputHandler.setLocalToolbarAvailable(true);
    notifyWindowChanged();
    redraw();
    if (m_WaylandToolbar) {
        m_WaylandToolbar->setVisible(m_Visible);
    } else {
        m_OverlayManager.setOverlayState(Overlay::OverlayToolbar, m_Visible);
    }
    // The host receives the exact target during native setup. Send it over the live
    // control stream too, then retry until the host confirms its applied
    // target.
    queueBitrateRequest(SDL_GetTicks(), true);
}

void PlankToolbar::createWaylandReconnectPrompt()
{
    if (!m_WaylandToolbar) {
        return;
    }

    PlankWaylandToolbar::Callbacks callbacks;
    callbacks.enter = [this](int x, int y) {
        reconnectPromptPointerEnter(x, y);
    };
    callbacks.leave = [this]() { reconnectPromptPointerLeave(); };
    callbacks.motion = [this](int x, int y) {
        reconnectPromptPointerMotion(x, y);
    };
    callbacks.button = [this](uint32_t button, bool down) {
        reconnectPromptPointerButton(button, down);
    };
    m_WaylandReconnectPrompt = PlankWaylandToolbar::create(
                m_Window, std::move(callbacks));
    if (!m_WaylandReconnectPrompt) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to create native Wayland reconnect prompt; the toolbar disconnect control remains available");
    }
}

void PlankToolbar::createWaylandToolbar()
{
    PlankWaylandToolbar::Callbacks callbacks;
    callbacks.enter = [this](int x, int y) { nativePointerEnter(x, y); };
    callbacks.leave = [this]() { nativePointerLeave(); };
    callbacks.motion = [this](int x, int y) { nativePointerMotion(x, y); };
    callbacks.button = [this](uint32_t button, bool down) {
        nativePointerButton(button, down);
    };
    callbacks.wheel = [this](int steps) { nativePointerWheel(steps); };
    m_WaylandToolbar = PlankWaylandToolbar::create(
                m_Window, std::move(callbacks));
    if (m_WaylandToolbar) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK toolbar using a native Wayland subsurface");
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK toolbar using the SDL overlay fallback");
    }
}

PlankToolbar::~PlankToolbar()
{
    endLocalPointerInteraction();
    m_InputHandler.setLocalToolbarAvailable(false);
    m_OverlayManager.setOverlayState(Overlay::OverlayToolbar, false);
}

void PlankToolbar::setRenderedStats(
        float fps, float videoMbps, float packetLossPercent,
        std::uint32_t networkRttMs)
{
    m_RenderedFps = std::max(0.0f, fps);
    m_VideoMbps = std::max(0.0f, videoMbps);
    m_PacketLossPercent = packetLossPercent < 0.0f ?
                -1.0f : qBound(0.0f, packetLossPercent, 100.0f);
    m_NetworkRttMs = networkRttMs;
}

void PlankToolbar::setAppliedBitrate(
        int requestedKbps, int appliedKbps, int peakKbps)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK encoder target confirmed: requested=%d Kbps, applied=%d Kbps, peak=%d Kbps",
                requestedKbps, appliedKbps, peakKbps);

    // An acknowledgement for an older slider position must not suppress the
    // newest pending target. The normal update loop will continue retrying.
    if (requestedKbps != m_BitrateKbps) {
        return;
    }

    m_AppliedBitrateKbps = appliedKbps;
    m_AppliedPeakKbps = peakKbps;
    if (appliedKbps != m_BitrateKbps) {
        m_BitrateKbps = appliedKbps;
    }
    redraw();
}

PlankToolbar::Action PlankToolbar::update(
        Uint64 now, bool transportAvailable)
{
    if (m_WaylandToolbar) {
        m_WaylandToolbar->dispatchPending();
    }
    if (m_WaylandReconnectPrompt) {
        m_WaylandReconnectPrompt->dispatchPending();
    }

    if (!m_Visible && !m_LocalPointerInteraction &&
            m_EdgeHoverStartTime != 0 &&
            m_PointerY <= EdgeRevealHeight &&
            now >= m_EdgeHoverStartTime + EdgeActivationDelayMs) {
        show(now);
    }

    if (m_Visible && !m_ButtonRouter.hasLocalButtons() &&
            !m_DraggingSlider && !m_PointerInside &&
            m_HideDeadline != 0 && now >= m_HideDeadline) {
        if (m_Pinned) {
            endLocalPointerInteraction();
            m_HideDeadline = 0;
        } else {
            hide();
        }
    }

    if (m_Visible && now >= m_LastRedrawTime + RedrawIntervalMs &&
            (std::fabs(m_RenderedFps - m_LastDrawnFps) >= 0.05f ||
             std::fabs(m_VideoMbps - m_LastDrawnVideoMbps) >= 0.05f ||
             std::fabs(m_PacketLossPercent -
                       m_LastDrawnPacketLossPercent) >= 0.05f ||
             m_NetworkRttMs != m_LastDrawnNetworkRttMs)) {
        redraw();
    }

    if (transportAvailable) {
        queueBitrateRequest(now, false);
    }

    const Action action = m_PendingAction;
    m_PendingAction = Action::None;
    return action;
}

bool PlankToolbar::setReconnectStatus(const QString& text, bool warning)
{
    m_ReconnectStatus = text;
    m_ReconnectStatusWarning = warning;
#ifdef Q_OS_DARWIN
    if (!m_WaylandReconnectPrompt) {
        if (text.isEmpty()) {
            MacWindow::hideReconnectStatus(m_Window);
        } else {
            MacWindow::showReconnectStatus(m_Window, text.toUtf8().constData(), warning);
        }
        return true;
    }
#endif
    if (!m_WaylandReconnectPrompt) return false;
    if (m_ReconnectPromptVisible || !m_ReconnectStatus.isEmpty()) {
        redrawReconnectPrompt();
        m_WaylandReconnectPrompt->setVisible(true);
    } else {
        m_WaylandReconnectPrompt->setVisible(false);
    }
    return true;
}

void PlankToolbar::showReconnectPrompt(int unreachableSeconds)
{
    m_ReconnectPromptSeconds = std::max(1, unreachableSeconds);
    m_ReconnectPromptVisible = true;
    m_ReconnectPromptPointerInside = false;
    m_ReconnectPromptButtonDown = false;
    m_ReconnectPromptPressedButton = ReconnectPromptNoButton;

    if (m_WaylandReconnectPrompt) {
        redrawReconnectPrompt();
        m_WaylandReconnectPrompt->setVisible(true);
    } else {
        m_OverlayManager.updateOverlayText(
                    Overlay::OverlayStatusUpdate,
                    "Workstation unreachable. Use the toolbar X to disconnect, or wait while PLANK retries.");
        m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);
    }
}

void PlankToolbar::hideReconnectPrompt()
{
    if (!m_ReconnectPromptVisible) {
        return;
    }
    m_ReconnectPromptVisible = false;
    m_ReconnectPromptPointerInside = false;
    m_ReconnectPromptButtonDown = false;
    m_ReconnectPromptPressedButton = ReconnectPromptNoButton;
    if (m_WaylandReconnectPrompt) {
        if (!m_ReconnectStatus.isEmpty()) redrawReconnectPrompt();
        m_WaylandReconnectPrompt->setVisible(!m_ReconnectStatus.isEmpty());
    }
}

void PlankToolbar::notifyWindowChanged()
{
    if (m_WaylandToolbar && !m_WaylandToolbar->isAttachedTo(m_Window)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Recreating PLANK toolbar for a replacement Wayland parent surface");
        m_WaylandToolbar.reset();
        m_WaylandReconnectPrompt.reset();
        createWaylandToolbar();
        createWaylandReconnectPrompt();
        if (!m_WaylandToolbar) {
            m_OverlayManager.setOverlayState(Overlay::OverlayToolbar,
                                             m_Visible);
        }
    }

    const int oldLogicalWidth = m_WindowWidth;
    const int oldLogicalHeight = m_WindowHeight;
    const int oldPixelWidth = m_WindowPixelWidth;
    const int oldPixelHeight = m_WindowPixelHeight;
    const float oldPixelDensity = m_PixelDensity;
    const float oldHorizontalPosition = m_ToolbarLeft >= 0 ?
                PlankToolbarLogic::horizontalPosition(
                    m_ToolbarLeft, m_WindowWidth, m_Width) : 0.5f;

    SDL_GetWindowSize(m_Window, &m_WindowWidth, &m_WindowHeight);
    SDL_GetWindowSizeInPixels(m_Window, &m_WindowPixelWidth,
                              &m_WindowPixelHeight);
    m_PixelDensity = PlankToolbarLogic::resolvePixelDensity(
                SDL_GetWindowPixelDensity(m_Window),
                m_WindowWidth, m_WindowHeight,
                m_WindowPixelWidth, m_WindowPixelHeight);
    m_Width = std::min(PlankToolbarStats::EncoderTargetLeft + m_EncoderTargetWidth +
                      PlankToolbarStats::WindowControlsWidth,
                      std::max(m_WindowWidth, 1));
    if (m_ToolbarLeft < 0) {
        m_ToolbarLeft = std::max(0, (m_WindowWidth - m_Width) / 2);
    } else {
        m_ToolbarLeft = PlankToolbarLogic::logicalLeftFromPosition(
                    oldHorizontalPosition, m_WindowWidth, m_Width);
    }
#ifdef Q_OS_DARWIN
    m_ToolbarLeft = MacWindow::unobscuredToolbarLeft(m_Window, m_ToolbarLeft, m_Width);
#endif
    if (!m_PointerInitialized) {
        m_PointerX = m_WindowWidth / 2;
        m_PointerY = m_WindowHeight / 2;
        m_PointerInitialized = true;
    } else {
        m_PointerX = qBound(0, m_PointerX, std::max(0, m_WindowWidth - 1));
        m_PointerY = qBound(0, m_PointerY, std::max(0, m_WindowHeight - 1));
    }
    if (m_WaylandToolbar) {
        m_WaylandToolbar->setLayout(m_WindowWidth, toolbarLeft(),
                                    m_Width, ToolbarHeight);
    }
    if (m_WaylandReconnectPrompt) {
        m_WaylandReconnectPrompt->setLayoutAt(
                    m_WindowWidth, reconnectPromptTop(), reconnectPromptLeft(),
                    std::min(ReconnectPromptWidth, m_WindowWidth),
                    ReconnectPromptHeight);
        if (m_ReconnectPromptVisible || !m_ReconnectStatus.isEmpty()) {
            redrawReconnectPrompt();
            m_WaylandReconnectPrompt->setVisible(true);
        }
    }
#ifdef Q_OS_DARWIN
    else if (!m_ReconnectStatus.isEmpty()) {
        MacWindow::showReconnectStatus(
                    m_Window, m_ReconnectStatus.toUtf8().constData(),
                    m_ReconnectStatusWarning);
    }
#endif
    if (m_Visible) {
        redraw();
    }

    if (m_WaylandToolbar) {
        // Renderer recreation can replace SDL's parent wl_surface. The new
        // subsurface starts unmapped, so restore the model's pinned/visible
        // state after its retained image and geometry have been published.
        m_WaylandToolbar->setVisible(m_Visible);
    }

    if (oldLogicalWidth != m_WindowWidth ||
            oldLogicalHeight != m_WindowHeight ||
            oldPixelWidth != m_WindowPixelWidth ||
            oldPixelHeight != m_WindowPixelHeight ||
            std::fabs(oldPixelDensity - m_PixelDensity) >= 0.001f) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK toolbar geometry: logical=%dx%d, pixels=%dx%d, density=%.3f, surface=%dx%d",
                    m_WindowWidth, m_WindowHeight,
                    m_WindowPixelWidth, m_WindowPixelHeight,
                    m_PixelDensity,
                    PlankToolbarLogic::physicalExtent(
                        m_Width, m_PixelDensity),
                    PlankToolbarLogic::physicalExtent(
                        ToolbarHeight, m_PixelDensity));
    }
}

void PlankToolbar::notifyFocusLost()
{
    m_ButtonRouter.reset();
    m_PressedControl = Control::None;
    m_DraggingToolbar = false;
    m_DraggingSlider = false;
    endLocalPointerInteraction();
}

bool PlankToolbar::observeMouseMotion(const SDL_MouseMotionEvent& event)
{
    if (event.which == SDL_TOUCH_MOUSEID) {
        return false;
    }

    // SDL also sees seat motion delivered for the toolbar's native child, but
    // that coordinate is child-local. Consume that duplicate here: the native
    // listener forwards the authoritative parent-relative position to the host
    // and remains the sole source of toolbar coordinates for this sequence.
    if (PlankToolbarLogic::nativeChildOwnsPointerSequence(
                m_WaylandToolbar != nullptr,
                m_PointerInside,
                m_ButtonRouter.hasLocalButtons())) {
        return true;
    }

    const Uint64 now = SDL_GetTicks();
    // PLANK sessions use absolute desktop input. SDL's window
    // coordinates are the single source of truth for both the remote pointer
    // and the receiver toolbar.
    m_PointerX = event.x;
    m_PointerY = event.y;

    // The edge must be held continuously for one second. Leaving the edge
    // cancels the activation, so ordinary host UI at the top remains usable.
    if (!m_Visible && !m_LocalPointerInteraction) {
        if (m_PointerY <= EdgeRevealHeight) {
            if (m_EdgeHoverStartTime == 0) {
                m_EdgeHoverStartTime = now;
            } else if (now >= m_EdgeHoverStartTime + EdgeActivationDelayMs) {
                show(now);
            }
        } else {
            m_EdgeHoverStartTime = 0;
        }
    }

    // Revealing does not claim the pointer. The user must move down from the
    // edge into the visible toolbar before local interaction begins.
    const bool pointerEnteredVisibleToolbar =
            m_Visible && m_PointerY > EdgeRevealHeight &&
            contains(m_PointerX, m_PointerY);

    // A native child surface receives authoritative surface-local input from
    // the compositor. Parent-window motion remains useful only for detecting
    // the reveal edge; it must never be transformed into toolbar hit tests.
    if (m_WaylandToolbar) {
        return false;
    }

    if (!m_Visible) {
        return false;
    }

    m_PointerInside = contains(m_PointerX, m_PointerY);
    if (m_PointerInside || m_ButtonRouter.hasLocalButtons()) {
        m_HideDeadline = 0;
    } else {
        m_HideDeadline = m_Pinned ? 0 : now + AutoHideDelayMs;
    }

    const auto owner = m_ButtonRouter.routeMotion(
                pointerEnteredVisibleToolbar);
    if (owner == PlankToolbarLogic::ButtonRouter::Owner::Remote) {
        if (!m_ButtonRouter.hasLocalButtons()) {
            endLocalPointerInteraction();
        }
        return false;
    }

    if (!m_LocalPointerInteraction) {
        beginLocalPointerInteraction();
    }

    if (m_DraggingToolbar) {
        const int newLeft = qBound(0, m_PointerX - m_ToolbarDragOffsetX,
                                   std::max(0, m_WindowWidth - m_Width));
        if (newLeft != m_ToolbarLeft) {
            m_ToolbarLeft = newLeft;
            if (now >= m_LastToolbarMoveDrawTime + ToolbarMoveRedrawIntervalMs) {
                redraw();
                m_LastToolbarMoveDrawTime = now;
            }
        }
        // Motion remains absolute remote-desktop motion even while the local
        // toolbar owns the associated button sequence. Forwarding it keeps
        // the captured host cursor synchronized beneath the opaque overlay.
        return false;
    }

    if (m_DraggingSlider) {
        updateBitrateFromPointer(m_PointerX, now, false);
        redraw();
        return false;
    }

    redraw();
    // Hover is local state, but absolute motion is still forwarded to the
    // host. Buttons and wheel events are the only events exclusively owned by
    // toolbar controls.
    return false;
}

PlankToolbar::Action PlankToolbar::handleMouseButton(
        const SDL_MouseButtonEvent& event)
{
    // SDL receives a duplicate seat event for the visible native child. The
    // native listener owns this sequence and will invoke handlePointerButton()
    // with the authoritative parent-relative coordinate.
    if (PlankToolbarLogic::nativeChildOwnsPointerSequence(
                m_WaylandToolbar != nullptr,
                m_PointerInside,
                m_ButtonRouter.hasLocalButtons())) {
        return Action::Consumed;
    }
    return handlePointerButton(event);
}

PlankToolbar::Action PlankToolbar::handlePointerButton(
        const SDL_MouseButtonEvent& event)
{
    m_PointerX = event.x;
    m_PointerY = event.y;
    const int pointerX = m_PointerX;
    const int pointerY = m_PointerY;
    const Uint64 now = SDL_GetTicks();
    const bool pointerInside = m_Visible && contains(pointerX, pointerY);
    const auto owner = m_ButtonRouter.routeButton(
                event.button, event.down, pointerInside);
    if (owner == PlankToolbarLogic::ButtonRouter::Owner::Remote) {
        if (!m_ButtonRouter.hasLocalButtons()) {
            endLocalPointerInteraction();
        }
        return Action::None;
    }

    if (!m_LocalPointerInteraction) {
        beginLocalPointerInteraction();
    }

    m_PointerInside = pointerInside;
    m_HideDeadline = 0;

    if (event.button != SDL_BUTTON_LEFT) {
        return Action::Consumed;
    }

    if (event.down) {
        m_PressedControl = controlAt(pointerX, pointerY);
        if (m_PressedControl == Control::Handle) {
            m_DraggingToolbar = true;
            m_ToolbarDragOffsetX = pointerX - toolbarLeft();
            redraw();
        } else if (m_PressedControl == Control::Slider &&
                   m_BitrateSupported) {
            m_DraggingSlider = true;
            updateBitrateFromPointer(pointerX, now, false);
        }
        return Action::Consumed;
    }

    const Control releasedControl = controlAt(pointerX, pointerY);
    const Control pressedControl = m_PressedControl;
    m_PressedControl = Control::None;

    if (m_DraggingToolbar) {
        m_DraggingToolbar = false;
        redraw();
    }
    if (m_DraggingSlider) {
        updateBitrateFromPointer(pointerX, now, true);
        m_DraggingSlider = false;
    }

    Action action = Action::Consumed;
    if (pressedControl == releasedControl) {
        switch (pressedControl) {
        case Control::Pin:
            m_Pinned = !m_Pinned;
            m_Preferences.plankToolbarPinned = m_Pinned;
            m_Preferences.save();
            redraw();
            break;
        case Control::Fullscreen:
            action = Action::ToggleFullscreen;
            break;
        case Control::Minimize:
            action = Action::Minimize;
            break;
        case Control::Disconnect:
            action = Action::Disconnect;
            break;
        default:
            break;
        }
    }

    if (!m_ButtonRouter.hasLocalButtons() && !pointerInside) {
        m_HideDeadline = m_Pinned ? 0 : now + AutoHideDelayMs;
        endLocalPointerInteraction();
    }
    return action;
}

bool PlankToolbar::handleMouseWheel(const SDL_MouseWheelEvent& event)
{
    // Consume SDL's duplicate child-surface wheel event. The native callback
    // below owns scrolling while the compositor pointer is in the toolbar.
    if (m_WaylandToolbar && m_PointerInside) {
        return true;
    }
    return handlePointerWheel(event);
}

bool PlankToolbar::handlePointerWheel(const SDL_MouseWheelEvent& event)
{
    const int mouseX = qRound(event.mouse_x);
    const int mouseY = qRound(event.mouse_y);
    m_PointerX = mouseX;
    m_PointerY = mouseY;
    if (!m_Visible) {
        return false;
    }
    if (!contains(mouseX, mouseY)) {
        endLocalPointerInteraction();
        return false;
    }
    if (!m_LocalPointerInteraction) {
        beginLocalPointerInteraction();
    }
    if (!m_BitrateSupported || !sliderContains(mouseX, mouseY)) {
        return true;
    }

    const int delta = event.y > 0 ? BitrateStepKbps :
                      event.y < 0 ? -BitrateStepKbps : 0;
    if (delta != 0) {
        m_BitrateKbps = qBound(BitrateMinimumKbps,
                               m_BitrateKbps + delta, BitrateMaximumKbps);
        redraw();
        queueBitrateRequest(SDL_GetTicks(), true);
    }
    return true;
}

int PlankToolbar::eventWaitTimeout() const
{
    return (m_Visible || m_EdgeHoverStartTime != 0) ? 50 : 1000;
}

void PlankToolbar::show(Uint64 now)
{
    m_Visible = true;
    m_EdgeHoverStartTime = 0;
    m_HideDeadline = m_Pinned ? 0 : now + AutoHideDelayMs;
    redraw();
    if (m_WaylandToolbar) {
        m_WaylandToolbar->setVisible(true);
    } else {
        m_OverlayManager.setOverlayState(Overlay::OverlayToolbar, true);
    }
}

void PlankToolbar::hide()
{
    endLocalPointerInteraction();
    m_Visible = false;
    m_EdgeHoverStartTime = 0;
    m_PointerInside = false;
    m_HideDeadline = 0;
    if (m_WaylandToolbar) {
        m_WaylandToolbar->setVisible(false);
    } else {
        m_OverlayManager.setOverlayState(Overlay::OverlayToolbar, false);
    }
}

void PlankToolbar::beginLocalPointerInteraction()
{
    if (m_LocalPointerInteraction) {
        return;
    }

    if (!m_WaylandToolbar) {
        m_InputHandler.setToolbarInteractionActive(true);
    }

    m_LocalPointerInteraction = true;
    m_PointerInside = true;
    m_HideDeadline = 0;
    redraw();
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                 "PLANK toolbar routed pointer to local controls");
}

void PlankToolbar::endLocalPointerInteraction()
{
    if (!m_LocalPointerInteraction) {
        return;
    }

    m_LocalPointerInteraction = false;
    m_DraggingToolbar = false;
    m_DraggingSlider = false;
    m_PointerInside = false;

    // The next absolute event resumes host routing at the same coordinate; no
    // warp, flush, capture toggle, or synthetic synchronization event.
    if (!m_WaylandToolbar) {
        m_InputHandler.setToolbarInteractionActive(false);
    }
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                 "PLANK toolbar routed pointer to remote desktop");
}

void PlankToolbar::redraw()
{
    const int surfaceWidth = m_WaylandToolbar ? m_Width :
            PlankToolbarLogic::physicalExtent(m_Width, m_PixelDensity);
    const int surfaceHeight = m_WaylandToolbar ? ToolbarHeight :
            PlankToolbarLogic::physicalExtent(ToolbarHeight,
                                                       m_PixelDensity);
    SDL_Surface* surface = SDL_CreateSurface(
                surfaceWidth, surfaceHeight, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to allocate PLANK toolbar surface: %s",
                    SDL_GetError());
        return;
    }

    QImage image(static_cast<uchar*>(surface->pixels), surface->w, surface->h,
                 surface->pitch, QImage::Format_ARGB32_Premultiplied);
    // Keep the toolbar fully opaque over the video surface.
    image.fill(QColor(22, 27, 34));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (!m_WaylandToolbar) {
        painter.scale(m_PixelDensity, m_PixelDensity);
    }
    // Paint the background through the first pixel row so the toolbar meets
    // the physical top edge without an antialiased one-pixel gap.
    painter.fillRect(QRect(0, 0, m_Width, ToolbarHeight),
                     QColor(22, 27, 34));
    painter.setPen(QPen(QColor(255, 255, 255, 42), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(0.5, 0.5, m_Width - 1.0,
                                   ToolbarHeight - 1.0), 0, 0);

    QFont labelFont;
    labelFont.setPixelSize(10);
    labelFont.setWeight(QFont::DemiBold);
    painter.setFont(labelFont);
    painter.setPen(QColor(235, 239, 244));

    // Six-dot grip for moving the toolbar horizontally along the top edge.
    const bool handleHovered = m_LocalPointerInteraction &&
                               handleContains(m_PointerX, m_PointerY);
    painter.setPen(Qt::NoPen);
    painter.setBrush(handleHovered || m_DraggingToolbar ?
                         QColor(205, 214, 224) : QColor(123, 134, 147));
    for (int y : {15, 19, 24}) {
        painter.drawEllipse(QPointF(9, y), 1.2, 1.2);
        painter.drawEllipse(QPointF(14, y), 1.2, 1.2);
    }

    // RGS-style upright outline thumbtack. A slash indicates auto-hide mode;
    // the unmodified thumbtack indicates that the toolbar is pinned.
    const bool pinHovered = m_LocalPointerInteraction &&
                            pinContains(m_PointerX, m_PointerY);
    QPainterPath pin;
    pin.moveTo(31, 11);
    pin.lineTo(43, 11);
    pin.lineTo(40, 15);
    pin.lineTo(40, 19);
    pin.lineTo(43, 23);
    pin.lineTo(38, 23);
    pin.lineTo(37, 29);
    pin.lineTo(36, 23);
    pin.lineTo(30, 23);
    pin.lineTo(33, 19);
    pin.lineTo(33, 15);
    pin.closeSubpath();
    painter.setBrush(Qt::NoBrush);
    const QColor pinColor = pinHovered ? QColor(255, 255, 255) :
                                         QColor(238, 242, 247);
    painter.setPen(QPen(pinColor, 1.2, Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(pin);
    if (!m_Pinned) {
        painter.setPen(QPen(pinColor, 1.6, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(QPointF(28, 8), QPointF(46, 30));
    }

    painter.setPen(QColor(151, 161, 174));
    painter.drawText(QRect(57, 5, 34, 12), Qt::AlignLeft | Qt::AlignVCenter, "FPS");
    QFont valueFont = labelFont;
    valueFont.setPixelSize(13);
    painter.setFont(valueFont);
    painter.setPen(QColor(246, 248, 250));
    painter.drawText(QRect(57, 16, 50, 17), Qt::AlignLeft | Qt::AlignVCenter,
                     QString::number(m_RenderedFps, 'f', 1));

    painter.setFont(labelFont);
    painter.setPen(QColor(151, 161, 174));
    painter.drawText(QRect(104, 5, 65, 12), Qt::AlignLeft | Qt::AlignVCenter,
                     "Video Mbps");
    painter.setFont(valueFont);
    painter.setPen(QColor(246, 248, 250));
    painter.drawText(QRect(104, 16, 65, 17), Qt::AlignLeft | Qt::AlignVCenter,
                     QString::number(m_VideoMbps, 'f', 1));

    painter.setFont(labelFont);
    painter.setPen(QColor(151, 161, 174));
    painter.drawText(QRect(172, 5, 52, 12), Qt::AlignLeft | Qt::AlignVCenter,
                     "Loss");
    const QColor lossColor = m_PacketLossPercent < 0.0f ?
                QColor(112, 120, 130) : packetLossColor(m_PacketLossPercent);
    QFont lossFont = valueFont;
    lossFont.setPixelSize(12);
    painter.setFont(lossFont);
    painter.setPen(lossColor);
    painter.drawText(QRect(174, 16, 52, 17), Qt::AlignLeft | Qt::AlignVCenter,
                     m_PacketLossPercent < 0.0f ?
                         QString("--") :
                         QString("%1%").arg(m_PacketLossPercent, 0, 'f',
                                             VideoPacketLossDisplayDecimalPlaces));

    painter.setFont(labelFont);
    painter.setPen(QColor(151, 161, 174));
    painter.drawText(QRect(PlankToolbarStats::RttLeft, 5,
                          PlankToolbarStats::RttWidth, 12),
                     Qt::AlignLeft | Qt::AlignVCenter, "RTT");
    painter.setFont(lossFont);
    painter.setPen(QColor(246, 248, 250));
    painter.drawText(QRect(PlankToolbarStats::RttLeft, 16,
                          PlankToolbarStats::RttWidth, 17),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     PlankToolbarStats::networkRttText(m_NetworkRttMs));

    const int targetLeft = sliderLeft() - toolbarLeft();
    const int targetWidth = sliderRight() - sliderLeft();
    painter.setFont(PlankToolbarStats::encoderTargetFont());
    painter.setPen(m_BitrateSupported ? QColor(235, 239, 244) : QColor(135, 143, 153));
    painter.drawText(QRect(targetLeft, 3, targetWidth, 17),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     PlankToolbarStats::encoderTargetText(m_BitrateKbps));

    const int trackLeft = sliderLeft() - toolbarLeft();
    const int trackRight = sliderRight() - toolbarLeft();
    const int trackY = 27;
    painter.setPen(QPen(QColor(92, 102, 114), 3, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(trackLeft, trackY, trackRight, trackY);

    const qreal fraction = qreal(m_BitrateKbps - BitrateMinimumKbps) /
                           qreal(BitrateMaximumKbps - BitrateMinimumKbps);
    const int thumbX = trackLeft + qRound(fraction * (trackRight - trackLeft));
    if (m_BitrateSupported) {
        painter.setPen(QPen(QColor(52, 132, 228), 3, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(trackLeft, trackY, thumbX, trackY);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_BitrateSupported ? QColor(243, 246, 250) : QColor(112, 120, 130));
    painter.drawEllipse(QPointF(thumbX, trackY), 5, 5);

    const QPointF fullscreenCenter(m_Width - 88.0, 19.0);
    const QRectF fullscreenRect(fullscreenCenter.x() - WindowButtonSize / 2.0,
                                fullscreenCenter.y() - WindowButtonSize / 2.0,
                                WindowButtonSize,
                                WindowButtonSize);
    const bool fullscreenHovered = m_LocalPointerInteraction &&
                                   fullscreenContains(m_PointerX, m_PointerY);
    painter.setPen(QPen(QColor(104, 116, 131), 1));
    painter.setBrush(fullscreenHovered ? QColor(68, 78, 90, 230) :
                                        QColor(48, 57, 68, 210));
    painter.drawRoundedRect(fullscreenRect,
                            WindowButtonRadius,
                            WindowButtonRadius);

    // Four corners point outward when entering fullscreen and inward when the
    // next click will restore the decorated window.
    const bool isFullscreen =
            (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_FULLSCREEN) != 0;
    const qreal outer = 6.0 * WindowGlyphScale;
    const qreal inner = 2.0 * WindowGlyphScale;
    const qreal cornerDistance = isFullscreen ? inner : outer;
    const qreal armLength = outer - inner;
    painter.setPen(QPen(QColor(226, 232, 239), 1.4,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    for (const qreal sx : {-1.0, 1.0}) {
        for (const qreal sy : {-1.0, 1.0}) {
            const QPointF corner = fullscreenCenter +
                    QPointF(sx * cornerDistance, sy * cornerDistance);
            painter.drawLine(corner,
                             corner + QPointF((isFullscreen ? sx : -sx) *
                                              armLength, 0));
            painter.drawLine(corner,
                             corner + QPointF(0, (isFullscreen ? sy : -sy) *
                                              armLength));
        }
    }

    const QPointF minimizeCenter(m_Width - 55.0, 19.0);
    const QRectF minimizeRect(minimizeCenter.x() - WindowButtonSize / 2.0,
                              minimizeCenter.y() - WindowButtonSize / 2.0,
                              WindowButtonSize,
                              WindowButtonSize);
    const bool minimizeHovered = m_LocalPointerInteraction &&
                                 minimizeContains(m_PointerX, m_PointerY);
    painter.setPen(QPen(QColor(104, 116, 131), 1));
    painter.setBrush(minimizeHovered ? QColor(68, 78, 90, 230) :
                                      QColor(48, 57, 68, 210));
    painter.drawRoundedRect(minimizeRect,
                            WindowButtonRadius,
                            WindowButtonRadius);
    painter.setPen(QPen(QColor(226, 232, 239), 1.5, Qt::SolidLine, Qt::RoundCap));
    const qreal minimizeHalfWidth = 5.0 * WindowGlyphScale;
    painter.drawLine(QPointF(minimizeCenter.x() - minimizeHalfWidth, 21),
                     QPointF(minimizeCenter.x() + minimizeHalfWidth, 21));

    const QPointF disconnectCenter(m_Width - 22.0, 19.0);
    const QRectF disconnectRect(disconnectCenter.x() - WindowButtonSize / 2.0,
                                disconnectCenter.y() - WindowButtonSize / 2.0,
                                WindowButtonSize,
                                WindowButtonSize);
    const bool disconnectHovered = m_LocalPointerInteraction &&
                                   disconnectContains(m_PointerX, m_PointerY);
    painter.setPen(QPen(QColor(239, 88, 88), 1));
    painter.setBrush(disconnectHovered ? QColor(151, 43, 49, 220) :
                                        QColor(126, 35, 40, 185));
    painter.drawRoundedRect(disconnectRect,
                            WindowButtonRadius,
                            WindowButtonRadius);
    painter.setPen(QPen(QColor(255, 228, 228), 1.5, Qt::SolidLine, Qt::RoundCap));
    const qreal disconnectHalfExtent = 6.0 * WindowGlyphScale;
    painter.drawLine(
                disconnectCenter + QPointF(-disconnectHalfExtent,
                                            -disconnectHalfExtent),
                disconnectCenter + QPointF(disconnectHalfExtent,
                                            disconnectHalfExtent));
    painter.drawLine(
                disconnectCenter + QPointF(disconnectHalfExtent,
                                            -disconnectHalfExtent),
                disconnectCenter + QPointF(-disconnectHalfExtent,
                                            disconnectHalfExtent));

    if (!m_BitrateSupported) {
        QFont hintFont = labelFont;
        hintFont.setPixelSize(10);
        painter.setFont(hintFont);
        painter.setPen(QColor(183, 151, 92));
        painter.fillRect(QRect(targetLeft - 4, 21, targetWidth + 8, 17), QColor(22, 27, 34));
        painter.drawText(QRect(targetLeft, 22, targetWidth, 15), Qt::AlignLeft | Qt::AlignVCenter,
                         "Bitrate control unavailable");
    }

    painter.end();
    m_LastDrawnFps = m_RenderedFps;
    m_LastDrawnVideoMbps = m_VideoMbps;
    m_LastDrawnPacketLossPercent = m_PacketLossPercent;
    m_LastDrawnNetworkRttMs = m_NetworkRttMs;
    m_LastRedrawTime = SDL_GetTicks();
    if (m_WaylandToolbar) {
        m_WaylandToolbar->setLayout(m_WindowWidth, toolbarLeft(),
                                    m_Width, ToolbarHeight);
        m_WaylandToolbar->present(image.copy());
        SDL_DestroySurface(surface);
    } else {
        const float horizontalPosition =
                PlankToolbarLogic::horizontalPosition(
                    m_ToolbarLeft, m_WindowWidth, m_Width);
        m_OverlayManager.setOverlayHorizontalPosition(Overlay::OverlayToolbar,
                                                       horizontalPosition);
        m_OverlayManager.updateOverlaySurface(Overlay::OverlayToolbar, surface);
    }
}

int PlankToolbar::reconnectPromptLeft() const
{
    return std::max(0, (m_WindowWidth -
                        std::min(ReconnectPromptWidth, m_WindowWidth)) / 2);
}

int PlankToolbar::reconnectPromptTop() const
{
    return std::max(0, (m_WindowHeight - ReconnectPromptHeight) / 2);
}

void PlankToolbar::redrawReconnectPrompt()
{
    if (!m_WaylandReconnectPrompt) {
        return;
    }

    const int width = std::min(ReconnectPromptWidth,
                               std::max(1, m_WindowWidth));
    QImage image(width, ReconnectPromptHeight,
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(22, 27, 34));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(106, 117, 132), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(0.5, 0.5, width - 1.0,
                                   ReconnectPromptHeight - 1.0), 8, 8);

    if (!m_ReconnectPromptVisible) {
        // This desynchronized local surface commits without a video frame.
        // Keep the established prompt geometry so a stopped parent does not
        // need to commit a new subsurface position during the transition.
        QFont statusFont;
        statusFont.setPixelSize(18);
        statusFont.setWeight(QFont::DemiBold);
        painter.setFont(statusFont);
        painter.setPen(m_ReconnectStatusWarning ? QColor(239, 88, 88) : QColor(224, 224, 224));
        painter.drawText(QRect(22, 18, width - 44, ReconnectPromptHeight - 36),
                         Qt::AlignCenter | Qt::TextWordWrap, m_ReconnectStatus);
        painter.end();
        m_WaylandReconnectPrompt->present(image);
        return;
    }

    QFont titleFont;
    titleFont.setPixelSize(18);
    titleFont.setWeight(QFont::DemiBold);
    painter.setFont(titleFont);
    painter.setPen(QColor(246, 248, 250));
    painter.drawText(QRect(22, 18, width - 44, 26),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     "Workstation unreachable");

    QFont bodyFont;
    bodyFont.setPixelSize(12);
    painter.setFont(bodyFont);
    painter.setPen(QColor(190, 199, 210));
    painter.drawText(QRect(22, 50, width - 44, 42),
                     Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                     QString("No response for %1 seconds. Disconnect now, or keep trying while the host restarts.")
                         .arg(m_ReconnectPromptSeconds));

    const QRect disconnectRect(width - 252, 108, 108, 32);
    const QRect waitRect(width - 132, 108, 110, 32);
    const auto buttonAt = [&](int parentX, int surfaceY) {
        const QPoint local(parentX - reconnectPromptLeft(), surfaceY);
        if (disconnectRect.contains(local)) {
            return ReconnectPromptDisconnectButton;
        }
        if (waitRect.contains(local)) {
            return ReconnectPromptWaitButton;
        }
        return ReconnectPromptNoButton;
    };
    const int hoveredButton = m_ReconnectPromptPointerInside ?
                buttonAt(m_ReconnectPromptPointerX,
                         m_ReconnectPromptPointerY) :
                ReconnectPromptNoButton;

    const auto drawButton = [&](const QRect& rect, int id,
                                const QString& label, bool destructive) {
        const bool active = hoveredButton == id;
        const bool pressed = m_ReconnectPromptButtonDown &&
                             m_ReconnectPromptPressedButton == id;
        painter.setPen(QPen(destructive ? QColor(239, 88, 88) :
                                         QColor(104, 116, 131), 1));
        painter.setBrush(destructive ?
                    (active ? QColor(151, 43, 49) : QColor(126, 35, 40)) :
                    (active ? QColor(68, 78, 90) : QColor(48, 57, 68)));
        QRectF paintedRect(rect);
        if (pressed) {
            paintedRect.translate(0, 1);
        }
        painter.drawRoundedRect(paintedRect, 5, 5);
        painter.setPen(QColor(246, 248, 250));
        painter.setFont(bodyFont);
        painter.drawText(paintedRect, Qt::AlignCenter, label);
    };
    drawButton(disconnectRect, ReconnectPromptDisconnectButton,
               "Disconnect", true);
    drawButton(waitRect, ReconnectPromptWaitButton,
               "Keep Waiting", false);
    painter.end();

    m_WaylandReconnectPrompt->setLayoutAt(
                m_WindowWidth, reconnectPromptTop(), reconnectPromptLeft(),
                width, ReconnectPromptHeight);
    m_WaylandReconnectPrompt->present(image);
}

void PlankToolbar::reconnectPromptPointerEnter(
        int parentX, int surfaceY)
{
    m_ReconnectPromptPointerInside = true;
    m_ReconnectPromptPointerX = parentX;
    m_ReconnectPromptPointerY = surfaceY;
    redrawReconnectPrompt();
}

void PlankToolbar::reconnectPromptPointerLeave()
{
    m_ReconnectPromptPointerInside = false;
    if (!m_ReconnectPromptButtonDown) {
        m_ReconnectPromptPressedButton = ReconnectPromptNoButton;
    }
    redrawReconnectPrompt();
}

void PlankToolbar::reconnectPromptPointerMotion(
        int parentX, int surfaceY)
{
    m_ReconnectPromptPointerInside = true;
    m_ReconnectPromptPointerX = parentX;
    m_ReconnectPromptPointerY = surfaceY;
    redrawReconnectPrompt();
}

void PlankToolbar::reconnectPromptPointerButton(
        uint32_t button, bool down)
{
#ifdef HAS_WAYLAND
    if (button != BTN_LEFT || !m_ReconnectPromptVisible) {
        return;
    }

    const int width = std::min(ReconnectPromptWidth,
                               std::max(1, m_WindowWidth));
    const QPoint local(m_ReconnectPromptPointerX - reconnectPromptLeft(),
                       m_ReconnectPromptPointerY);
    int buttonAtPointer = ReconnectPromptNoButton;
    if (QRect(width - 252, 108, 108, 32).contains(local)) {
        buttonAtPointer = ReconnectPromptDisconnectButton;
    } else if (QRect(width - 132, 108, 110, 32).contains(local)) {
        buttonAtPointer = ReconnectPromptWaitButton;
    }

    if (down) {
        m_ReconnectPromptButtonDown = true;
        m_ReconnectPromptPressedButton = buttonAtPointer;
        redrawReconnectPrompt();
        return;
    }

    const int pressedButton = m_ReconnectPromptPressedButton;
    m_ReconnectPromptButtonDown = false;
    m_ReconnectPromptPressedButton = ReconnectPromptNoButton;
    if (pressedButton == buttonAtPointer) {
        if (pressedButton == ReconnectPromptDisconnectButton) {
            m_PendingAction = Action::Disconnect;
        } else if (pressedButton == ReconnectPromptWaitButton) {
            m_PendingAction = Action::KeepWaiting;
        }
    }
    redrawReconnectPrompt();
#else
    (void) button;
    (void) down;
#endif
}

void PlankToolbar::nativePointerEnter(int parentX, int parentY)
{
    m_PointerX = parentX;
    m_PointerY = parentY;
    m_PointerInside = true;
    m_HideDeadline = 0;
    beginLocalPointerInteraction();
    redraw();
}

void PlankToolbar::nativePointerLeave()
{
    m_PointerInside = false;
    if (!m_ButtonRouter.hasLocalButtons()) {
        endLocalPointerInteraction();
        m_HideDeadline = m_Pinned ? 0 : SDL_GetTicks() + AutoHideDelayMs;
    }
    redraw();
}

void PlankToolbar::nativePointerMotion(int parentX, int parentY)
{
    const Uint64 now = SDL_GetTicks();
    m_PointerX = parentX;
    m_PointerY = parentY;
    m_PointerInside = true;
    m_HideDeadline = 0;

    if (m_DraggingToolbar) {
        const int newLeft = qBound(0, m_PointerX - m_ToolbarDragOffsetX,
                                   std::max(0, m_WindowWidth - m_Width));
        if (newLeft != m_ToolbarLeft) {
            m_ToolbarLeft = newLeft;
            m_WaylandToolbar->setLayout(m_WindowWidth, m_ToolbarLeft,
                                        m_Width, ToolbarHeight);
            if (now >= m_LastToolbarMoveDrawTime + ToolbarMoveRedrawIntervalMs) {
                redraw();
                m_LastToolbarMoveDrawTime = now;
            }
        }
    } else if (m_DraggingSlider) {
        updateBitrateFromPointer(m_PointerX, now, false);
    } else {
        redraw();
    }
}

void PlankToolbar::nativePointerButton(uint32_t button, bool down)
{
#ifdef HAS_WAYLAND
    uint8_t sdlButton = 0;
    switch (button) {
    case BTN_LEFT: sdlButton = SDL_BUTTON_LEFT; break;
    case BTN_MIDDLE: sdlButton = SDL_BUTTON_MIDDLE; break;
    case BTN_RIGHT: sdlButton = SDL_BUTTON_RIGHT; break;
    case BTN_SIDE: sdlButton = SDL_BUTTON_X1; break;
    case BTN_EXTRA: sdlButton = SDL_BUTTON_X2; break;
    default: return;
    }
    SDL_MouseButtonEvent event{};
    event.button = sdlButton;
    event.down = down;
    event.x = m_PointerX;
    event.y = m_PointerY;
    const Action action = handlePointerButton(event);
    if (action != Action::None && action != Action::Consumed) {
        m_PendingAction = action;
    }
#else
    (void) button;
    (void) down;
#endif
}

void PlankToolbar::nativePointerWheel(int verticalSteps)
{
    if (verticalSteps == 0) {
        return;
    }
    SDL_MouseWheelEvent event{};
    event.y = static_cast<float>(verticalSteps);
    event.mouse_x = static_cast<float>(m_PointerX);
    event.mouse_y = static_cast<float>(m_PointerY);
    handlePointerWheel(event);
}

void PlankToolbar::updateBitrateFromPointer(int x, Uint64 now, bool forceSend)
{
    const qreal fraction = qBound(
                qreal(0.0),
                qreal(x - sliderLeft()) / qreal(std::max(1, sliderRight() - sliderLeft())),
                qreal(1.0));
    int bitrate = BitrateMinimumKbps +
                  qRound(fraction * (BitrateMaximumKbps - BitrateMinimumKbps));
    bitrate = qRound(qreal(bitrate) / BitrateStepKbps) * BitrateStepKbps;
    bitrate = qBound(BitrateMinimumKbps, bitrate, BitrateMaximumKbps);
    if (bitrate != m_BitrateKbps) {
        m_BitrateKbps = bitrate;
        m_LastBitrateChangeTime = now;
        redraw();
    }
    queueBitrateRequest(now, forceSend);
}

void PlankToolbar::queueBitrateRequest(Uint64 now, bool forceSend)
{
    if (!m_BitrateSupported) {
        return;
    }
    if (m_BitrateKbps == m_AppliedBitrateKbps) {
        return;
    }
    if (!forceSend &&
            (now < m_LastBitrateChangeTime + BitrateSettleDelayMs ||
             now < m_LastBitrateSendTime +
                              (m_BitrateKbps == m_LastSentBitrateKbps ?
                                   BitrateAcknowledgementRetryMs :
                                   BitrateSettleDelayMs))) {
        return;
    }
    if (LiSetVideoBitrate(m_BitrateKbps) == 0) {
        m_LastSentBitrateKbps = m_BitrateKbps;
        m_LastBitrateSendTime = now;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Requested PLANK encoder target: %d Kbps",
                    m_BitrateKbps);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to send PLANK bitrate update: %d Kbps",
                    m_BitrateKbps);
    }
}

bool PlankToolbar::contains(int x, int y) const
{
    return x >= toolbarLeft() && x < toolbarLeft() + m_Width &&
           y >= 0 && y < ToolbarHeight;
}

bool PlankToolbar::sliderContains(int x, int y) const
{
    return x >= sliderLeft() - 7 && x <= sliderRight() + 7 &&
           y >= 17 && y <= 37;
}

bool PlankToolbar::pinContains(int x, int y) const
{
    return x >= toolbarLeft() + 23 && x <= toolbarLeft() + 51 &&
           y >= 5 && y <= 33;
}

bool PlankToolbar::handleContains(int x, int y) const
{
    return x >= toolbarLeft() && x <= toolbarLeft() + 22 &&
           y >= 0 && y <= ToolbarHeight - 5;
}

bool PlankToolbar::minimizeContains(int x, int y) const
{
    return x >= toolbarLeft() + m_Width - 69 &&
           x <= toolbarLeft() + m_Width - 41 && y >= 5 && y <= 33;
}

bool PlankToolbar::fullscreenContains(int x, int y) const
{
    return x >= toolbarLeft() + m_Width - 102 &&
           x <= toolbarLeft() + m_Width - 74 && y >= 5 && y <= 33;
}

bool PlankToolbar::disconnectContains(int x, int y) const
{
    return x >= toolbarLeft() + m_Width - 36 &&
           x <= toolbarLeft() + m_Width - 8 && y >= 5 && y <= 33;
}

PlankToolbar::Control PlankToolbar::controlAt(int x, int y) const
{
    if (!contains(x, y)) {
        return Control::None;
    }
    if (handleContains(x, y)) {
        return Control::Handle;
    }
    if (pinContains(x, y)) {
        return Control::Pin;
    }
    if (fullscreenContains(x, y)) {
        return Control::Fullscreen;
    }
    if (minimizeContains(x, y)) {
        return Control::Minimize;
    }
    if (disconnectContains(x, y)) {
        return Control::Disconnect;
    }
    if (sliderContains(x, y)) {
        return Control::Slider;
    }
    return Control::None;
}

int PlankToolbar::toolbarLeft() const
{
    return std::max(0, m_ToolbarLeft);
}

int PlankToolbar::sliderLeft() const
{
    return toolbarLeft() + PlankToolbarStats::EncoderTargetLeft;
}

int PlankToolbar::sliderRight() const
{
    return toolbarLeft() + std::max(PlankToolbarStats::EncoderTargetLeft,
                                   m_Width - PlankToolbarStats::WindowControlsWidth);
}
