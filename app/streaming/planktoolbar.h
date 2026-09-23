#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <QString>

#include "planktoolbarlogic.h"
#include "audio/microphone.h"
#include "camera/camera.h"

class StreamingPreferences;
class SdlInputHandler;
class PlankWaylandToolbar;

namespace Overlay {
class OverlayManager;
}

class PlankToolbar
{
public:
    enum class Action {
        None,
        Consumed,
        ToggleFullscreen,
        Minimize,
        Disconnect,
        KeepWaiting,
        ToggleMicrophone,
        ToggleCamera,
    };

    PlankToolbar(SDL_Window* window,
                          Overlay::OverlayManager& overlayManager,
                          SdlInputHandler& inputHandler,
                          StreamingPreferences& preferences,
                          int initialBitrateKbps);
    ~PlankToolbar();

    void setRenderedStats(float fps, float videoMbps, float packetLossPercent,
                          std::uint32_t networkRttMs);
    void setAppliedBitrate(int requestedKbps, int appliedKbps, int peakKbps);
    void setCameraState(bool supported, PlankCamera::State state);
    void setMicrophoneState(bool supported, PlankMicrophone::State state);
    Action update(Uint64 now, bool transportAvailable = true);
    void showReconnectPrompt(int unreachableSeconds);
    void hideReconnectPrompt();
    // Returns true when status is presented independently of video frames.
    bool setReconnectStatus(const QString& text, bool warning);
    void notifyWindowChanged();
    void notifyFocusLost();

    bool observeMouseMotion(const SDL_MouseMotionEvent& event);
    Action handleMouseButton(const SDL_MouseButtonEvent& event);
    bool handleMouseWheel(const SDL_MouseWheelEvent& event);

    int eventWaitTimeout() const;

private:
    enum class Control {
        None,
        Handle,
        Slider,
        Pin,
        Microphone,
        Camera,
        Fullscreen,
        Minimize,
        Disconnect,
    };

    void show(Uint64 now);
    void hide();
    void beginLocalPointerInteraction();
    void endLocalPointerInteraction();
    void createWaylandToolbar();
    void createWaylandReconnectPrompt();
    void redraw();
    void redrawReconnectPrompt();
    void refreshNativeReconnectStatus();
    void reconnectPromptPointerEnter(int parentX, int surfaceY);
    void reconnectPromptPointerLeave();
    void reconnectPromptPointerMotion(int parentX, int surfaceY);
    void reconnectPromptPointerButton(uint32_t button, bool down);
    int reconnectPromptLeft() const;
    int reconnectPromptTop() const;
    void updateBitrateFromPointer(int x, Uint64 now, bool forceSend);
    void queueBitrateRequest(Uint64 now, bool forceSend);
    void nativePointerEnter(int parentX, int parentY);
    void nativePointerLeave();
    void nativePointerMotion(int parentX, int parentY);
    void nativePointerButton(uint32_t button, bool down);
    void nativePointerWheel(int verticalSteps);
    Action handlePointerButton(const SDL_MouseButtonEvent& event);
    bool handlePointerWheel(const SDL_MouseWheelEvent& event);
    bool contains(int x, int y) const;
    bool sliderContains(int x, int y) const;
    bool handleContains(int x, int y) const;
    bool pinContains(int x, int y) const;
    bool cameraContains(int x, int y) const;
    bool microphoneContains(int x, int y) const;
    bool fullscreenContains(int x, int y) const;
    bool minimizeContains(int x, int y) const;
    bool disconnectContains(int x, int y) const;
    Control controlAt(int x, int y) const;
    int toolbarLeft() const;
    int sliderLeft() const;
    int sliderRight() const;

    SDL_Window* m_Window;
    Overlay::OverlayManager& m_OverlayManager;
    SdlInputHandler& m_InputHandler;
    StreamingPreferences& m_Preferences;
    std::unique_ptr<PlankWaylandToolbar> m_WaylandToolbar;
    std::unique_ptr<PlankWaylandToolbar> m_WaylandReconnectPrompt;
    QString m_ReconnectStatus;
    bool m_ReconnectStatusWarning = false;
    Action m_PendingAction;
    bool m_Visible;
    bool m_Pinned;
    bool m_DraggingToolbar;
    bool m_DraggingSlider;
    bool m_PointerInside;
    bool m_PointerInitialized;
    bool m_LocalPointerInteraction;
    bool m_BitrateSupported;
    bool m_CameraSupported = false;
    PlankCamera::State m_CameraState = PlankCamera::State::Off;
    bool m_MicrophoneSupported = false;
    PlankMicrophone::State m_MicrophoneState = PlankMicrophone::State::Off;
    bool m_ReconnectPromptVisible;
    bool m_ReconnectPromptPointerInside;
    bool m_ReconnectPromptButtonDown;
    int m_ReconnectPromptPointerX;
    int m_ReconnectPromptPointerY;
    int m_ReconnectPromptPressedButton;
    int m_ReconnectPromptSeconds;
    int m_WindowWidth;
    int m_WindowHeight;
    int m_WindowPixelWidth;
    int m_WindowPixelHeight;
    float m_PixelDensity;
    const int m_EncoderTargetWidth;
    int m_Width;
    int m_ToolbarLeft;
    int m_ToolbarDragOffsetX;
    int m_PointerX;
    int m_PointerY;
    Control m_PressedControl;
    PlankToolbarLogic::ButtonRouter m_ButtonRouter;
    int m_BitrateKbps;
    int m_LastSentBitrateKbps;
    int m_AppliedBitrateKbps;
    int m_AppliedPeakKbps;
    float m_RenderedFps;
    float m_VideoMbps;
    float m_PacketLossPercent;
    std::uint32_t m_NetworkRttMs;
    float m_LastDrawnFps;
    float m_LastDrawnVideoMbps;
    float m_LastDrawnPacketLossPercent;
    std::uint32_t m_LastDrawnNetworkRttMs;
    Uint64 m_HideDeadline;
    Uint64 m_LastBitrateSendTime;
    Uint64 m_LastBitrateChangeTime;
    Uint64 m_LastToolbarMoveDrawTime;
    Uint64 m_LastRedrawTime;
    Uint64 m_EdgeHoverStartTime;
};
