#include "input.h"
#include "plankpointerlogic.h"
#include "plankmousemotion.h"

#include <Limelight.h>
#include <SDL3/SDL.h>
#include "streaming/streamutils.h"

void SdlInputHandler::handleMouseButtonEvent(SDL_MouseButtonEvent* event)
{
    int button;
    SDL_Window* window = presentationWindow(event->windowID);
    if (window == nullptr) {
        return;
    }

    if (event->which == SDL_TOUCH_MOUSEID ||
            ignorePenAsMouse(event->which)) {
        // Ignore synthetic mouse events
        return;
    }
    float x = event->x, y = event->y;
    window = pointerPresentationWindow(window, x, y);
    activateCompositorCursor();
    if (!isCaptureActive()) {
        if (event->button == SDL_BUTTON_LEFT && !event->down &&
                isMouseInVideoRegion(qRound(x), qRound(y),
                                     SDL_GetWindowID(window))) {
            // Capture the mouse again if clicked when unbound.
            // We start capture on left button released instead of
            // pressed to avoid sending an errant mouse button released
            // event to the host when clicking into our window (since
            // the pressed event was consumed by this code).
            setCaptureActive(true);
        }

        // Not capturing
        return;
    }
    else if (!isMouseInVideoRegion(qRound(x), qRound(y),
                                   SDL_GetWindowID(window)) && event->down) {
        // Ignore button presses outside the video region, but allow button releases
        return;
    }

    switch (event->button)
    {
        case SDL_BUTTON_LEFT:
            button = BUTTON_LEFT;
            break;
        case SDL_BUTTON_MIDDLE:
            button = BUTTON_MIDDLE;
            break;
        case SDL_BUTTON_RIGHT:
            button = BUTTON_RIGHT;
            break;
        case SDL_BUTTON_X1:
            button = BUTTON_X1;
            break;
        case SDL_BUTTON_X2:
            button = BUTTON_X2;
            break;
        default:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Unhandled button event: %d",
                        event->button);
            return;
    }

    // Button packets carry no coordinates. Reassert the SDL button event's
    // absolute position immediately before the button so a stale tablet or
    // coalesced motion sample cannot make the remote click land elsewhere.
    if (event->down && !sendAbsoluteMousePosition(
                window, qRound(x), qRound(y), false)) {
        return;
    }

    LiSendMouseButtonEvent(event->down ?
                               BUTTON_ACTION_PRESS :
                               BUTTON_ACTION_RELEASE,
                           button);
    if (!event->down) {
        // A captured drag can finish on the other output without another move.
        // Forward its release before transferring native window focus.
        followPointerFocus(window, 0);
    }
}

void SdlInputHandler::handleMouseMotionEvent(SDL_MouseMotionEvent* event,
                                             bool batchPendingEvents)
{
    if (!isCaptureActive()) {
        // Not capturing
        return;
    }
    else if (event->which == SDL_TOUCH_MOUSEID ||
            ignorePenAsMouse(event->which)) {
        // Ignore synthetic mouse events
        return;
    }
    activateCompositorCursor();

    SDL_Window* window = presentationWindow(event->windowID);
    if (window == nullptr) {
        return;
    }

    SDL_MouseMotionEvent motion = *event;
    if (batchPendingEvents) {
        PlankMouseMotion::coalescePending(motion);
    }
    float x = motion.x, y = motion.y;

    // We should not reference the original event anymore
    event = nullptr;

    window = pointerPresentationWindow(window, x, y);

    int windowWidth, windowHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);

    bool mouseInVideoRegion;

    mouseInVideoRegion = isMouseInVideoRegion(
                qRound(x), qRound(y), SDL_GetWindowID(window), windowWidth, windowHeight);

    // Send the mouse position update if one of the following is true:
    // a) it is in the video region now
    // b) it just left the video region (to ensure the mouse is clamped to the video boundary)
    // c) a mouse button is still down from before the cursor left the video region (to allow smooth dragging)
    Uint32 buttonState = SDL_GetMouseState(nullptr, nullptr);
    if (buttonState == 0) {
        if (m_PendingMouseButtonsAllUpOnVideoRegionLeave) {
            if (m_NeedsManualCaptureOnLeave) {
                SDL_CaptureMouse(false);
            }
            m_PendingMouseButtonsAllUpOnVideoRegionLeave = false;
        }
    }
    if (mouseInVideoRegion || m_MouseWasInVideoRegion || m_PendingMouseButtonsAllUpOnVideoRegionLeave) {
        sendAbsoluteMousePosition(window, qRound(x), qRound(y), true);
    }

    // Adjust the cursor visibility if applicable
    if (mouseInVideoRegion ^ m_MouseWasInVideoRegion) {
        setCursorVisible(!mouseInVideoRegion ||
                         (m_LocalCursorSupported ? m_RemoteCursorVisible :
                                                   m_MouseCursorCapturedVisibilityState));
        if (!mouseInVideoRegion && buttonState != 0) {
            // If we still have a button pressed on leave, wait for that to come up
            // before we stop sending mouse position events.
            m_PendingMouseButtonsAllUpOnVideoRegionLeave = true;
        }
    }

    m_MouseWasInVideoRegion = mouseInVideoRegion;
    followPointerFocus(window, motion.state);
}

bool SdlInputHandler::sendAbsoluteMousePosition(
        SDL_Window* window, int windowX, int windowY,
        bool allowClampedPosition)
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
    QPointF streamPoint;
    if (!PlankPresentation::mapWindowPointToStream(
                QPointF(windowX, windowY), QSize(windowWidth, windowHeight),
                streamSize,
                m_PresentationLayout.canvasSize, output->canvasRect,
                streamPoint, allowClampedPosition)) {
        return false;
    }
    return LiSendMousePositionEvent(
                static_cast<short>(qBound(0, qRound(streamPoint.x()),
                                          streamSize.width())),
                static_cast<short>(qBound(0, qRound(streamPoint.y()),
                                          streamSize.height())),
                static_cast<short>(streamSize.width()),
                static_cast<short>(streamSize.height())) == 0;
}

void SdlInputHandler::handleMouseWheelEvent(SDL_MouseWheelEvent* event)
{
    if (!isCaptureActive()) {
        // Not capturing
        return;
    }
    else if (event->which == SDL_TOUCH_MOUSEID ||
            ignorePenAsMouse(event->which)) {
        // Ignore synthetic mouse events
        return;
    }
    activateCompositorCursor();

    SDL_Window* window = presentationWindow(event->windowID);
    if (window == nullptr) {
        return;
    }

    const int mouseX = qRound(event->mouse_x);
    const int mouseY = qRound(event->mouse_y);
    if (!isMouseInVideoRegion(mouseX, mouseY, event->windowID)) {
        // Ignore scroll events outside the video region
        return;
    }

    if (event->y != 0.0f) {
#ifdef Q_OS_DARWIN
        // HACK: Clamp the scroll values on macOS to prevent OS scroll acceleration
        // from generating wild scroll deltas when scrolling quickly.
        event->y = SDL_clamp(event->y, -1.0f, 1.0f);
#endif

        LiSendHighResScrollEvent((short)(event->y * 120)); // WHEEL_DELTA
    }

    if (event->x != 0.0f) {
#ifdef Q_OS_DARWIN
        // HACK: Clamp the scroll values on macOS to prevent OS scroll acceleration
        // from generating wild scroll deltas when scrolling quickly.
        event->x = SDL_clamp(event->x, -1.0f, 1.0f);
#endif

        LiSendHighResHScrollEvent((short)(event->x * 120)); // WHEEL_DELTA
    }
}

bool SdlInputHandler::isMouseInVideoRegion(int mouseX, int mouseY,
                                           Uint32 windowId,
                                           int windowWidth, int windowHeight)
{
    SDL_Window* window = presentationWindow(windowId);
    const auto* output = presentationOutput(window);
    if (window == nullptr || output == nullptr) {
        return false;
    }

    if (windowWidth < 0 || windowHeight < 0) {
        SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    }
    const QSize streamSize = streamDimensions();
    QPointF streamPoint;
    return PlankPresentation::mapWindowPointToStream(
                QPointF(mouseX, mouseY), QSize(windowWidth, windowHeight),
                streamSize,
                m_PresentationLayout.canvasSize, output->canvasRect,
                streamPoint, false);
}

SDL_Window* SdlInputHandler::pointerPresentationWindow(SDL_Window* source,
                                                      float& x, float& y) const
{
#ifdef Q_OS_DARWIN
    if (m_PresentationLayout.isMultiOutput()) {
        QVector<QRect> windowRects;
        int sourceOutput = -1;
        for (const auto& output : m_PresentationLayout.outputs) {
            int wx, wy, width, height;
            if (!SDL_GetWindowPosition(output.window, &wx, &wy) ||
                    !SDL_GetWindowSize(output.window, &width, &height)) {
                return source;
            }
            if (output.window == source) sourceOutput = windowRects.size();
            windowRects.append(QRect(wx, wy, width, height));
        }
        QPointF point;
        const int target = PlankPresentation::resolvePointerOutput(
            windowRects, sourceOutput, QPointF(x, y), point);
        if (target >= 0) {
            x = point.x();
            y = point.y();
            return m_PresentationLayout.outputs.at(target).window;
        }
    }
#else
    Q_UNUSED(x);
    Q_UNUSED(y);
#endif
    return source;
}

SDL_Window* SdlInputHandler::presentationWindow(Uint32 windowId) const
{
    if (windowId == 0) {
        return m_Window;
    }
    SDL_Window* window = SDL_GetWindowFromID(windowId);
    return presentationOutput(window) != nullptr ? window : nullptr;
}

void SdlInputHandler::followPointerFocus(SDL_Window* target,
                                        SDL_MouseButtonFlags eventButtons,
                                        PointerFocusPosition position)
{
#ifdef Q_OS_DARWIN
    // The fullscreen surfaces are one remote desktop. Transfer native focus
    // on hover so a secondary click does not need a preceding activation click.
    // Never switch away from the Cocoa window that owns an in-progress drag.
    if (!m_PresentationLayout.isMultiOutput() || !isCaptureActive() ||
            eventButtons != 0 || SDL_GetMouseState(nullptr, nullptr) != 0) {
        return;
    }
    const bool tablet = position == PointerFocusPosition::HostTablet;
    if (tablet && !PlankPointerLogic::tabletFocusPositionIsCurrent(
                m_TabletCursorActive, m_AppliedRemoteCursorPositionValid,
                m_AppliedRemoteCursorPositionSequence, m_TabletCursorActivationSequence)) {
        return;
    }
    SDL_Window* focused = SDL_GetKeyboardFocus();
    if (focused == nullptr || focused == target ||
            presentationOutput(focused) == nullptr ||
            presentationOutput(target) == nullptr) {
        return;
    }
    const auto targetFlags = SDL_GetWindowFlags(target);
    if (!(targetFlags & SDL_WINDOW_FULLSCREEN) ||
            (targetFlags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED))) {
        return;
    }

    // Raw tablet reports go directly to the Host and do not move the Mac's
    // mouse pointer. Their target was mapped from the fresh Host position.
    // Only local mouse events can be checked against the Mac desktop pointer.
    // Both paths retain native mouse drags and require owned presentation focus.
    float gx, gy;
    if (SDL_GetGlobalMouseState(&gx, &gy) != 0) {
        return;
    }
    if (!tablet) {
        int wx, wy, width, height;
        if (!SDL_GetWindowPosition(target, &wx, &wy) ||
                !SDL_GetWindowSize(target, &width, &height) ||
                gx < wx || gy < wy || gx >= wx + width || gy >= wy + height) {
            return;
        }
    }
    if (!SDL_RaiseWindow(target)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "Unable to transfer fullscreen pointer focus: %s",
                    SDL_GetError());
    }
    else {
        SDL_LogInfo(tablet ? SDL_LOG_CATEGORY_APPLICATION : SDL_LOG_CATEGORY_INPUT,
                    "PLANK fullscreen %s focus: %u -> %u",
                    tablet ? "tablet" : "pointer",
                    SDL_GetWindowID(focused), SDL_GetWindowID(target));
    }
#else
    Q_UNUSED(target);
    Q_UNUSED(eventButtons);
    Q_UNUSED(position);
#endif
}

const PlankPresentationOutput* SdlInputHandler::presentationOutput(
        SDL_Window* window) const
{
    for (const auto& output : m_PresentationLayout.outputs) {
        if (output.window == window) {
            return &output;
        }
    }
    return nullptr;
}

void SdlInputHandler::updatePointerRegionLock()
{
    if (m_Window == nullptr) {
        return;
    }

    // Our pointer lock behavior tracks with the fullscreen mode unless the user has
    // toggled it themselves using the keyboard shortcut. If that's the case, they
    // have full control over it and we don't touch it anymore.
    if (!m_PointerRegionLockToggledByUser) {
        // Lock the pointer in true full-screen mode or in any fullscreen mode when only a single monitor is present
        const bool fullscreen = (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_FULLSCREEN) != 0;
        m_PointerRegionLockActive = fullscreen && StreamUtils::getDisplayCount() == 1;
    }

    // If region lock is enabled, grab the cursor so it can't accidentally leave our window.
    if (isCaptureActive() && m_PointerRegionLockActive) {
        SDL_Rect src, videoRect;
        const QSize streamSize = streamDimensions();

        src.x = src.y = 0;
        src.w = streamSize.width();
        src.h = streamSize.height();

        videoRect.x = videoRect.y = 0;
        SDL_GetWindowSize(m_Window, &videoRect.w, &videoRect.h);
        const PlankPointerLogic::Rect windowRect = {
            0, 0, videoRect.w, videoRect.h
        };

        // Use the stream and window sizes to determine the video region.
        StreamUtils::scaleSourceToDestinationSurface(&src, &videoRect);
        // A PLANK toolbar is anchored to the window's top edge, not
        // the scaled video's top edge. Keep the pointer inside the window while
        // allowing it to cross letterbox/pillarbox regions and reach the reveal
        // strip. Mouse motion outside the video rectangle remains local and is
        // not forwarded to the host by handleMouseMotionEvent().
        const auto confinementRect =
                PlankPointerLogic::pointerConfinementRect(
                    windowRect,
                    {videoRect.x, videoRect.y, videoRect.w, videoRect.h},
                    m_LocalToolbarAvailable);
        const SDL_Rect dst = {
            confinementRect.x,
            confinementRect.y,
            confinementRect.w,
            confinementRect.h,
        };

        SDL_SetWindowMouseRect(m_Window, &dst);
    }
    else {
        // Allow the cursor to leave the bounds of our video region or window
        SDL_SetWindowMouseRect(m_Window, nullptr);
    }
}
