#pragma once

#include "settings/streamingpreferences.h"
#include "backend/computermanager.h"
#include "streaming/plankpresentation.h"
#include "streaming/plankembeddedcursor.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#ifdef HAVE_LIBINPUT_TABLET
class LinuxWacomInput;
class LinuxRawWacomInput;
#endif
class PlankWaylandCursor;
#ifdef Q_OS_MACOS
class MacQuitShortcut;
class MacKeyboardCapture;
#endif
#ifdef HAVE_MAC_RAW_WACOM
class MacRawWacomInput;
class MacNormalizedPen;
#endif

class SdlInputHandler
{
public:
    explicit SdlInputHandler(StreamingPreferences& prefs,
                             int streamWidth,
                             int streamHeight);

    ~SdlInputHandler();

    void setWindow(SDL_Window* window);

    void setStreamDimensions(int streamWidth, int streamHeight);

    void setPresentationLayout(const PlankPresentationLayout& layout);

    void refreshWaylandTabletCursorParents();

    void handleKeyEvent(SDL_KeyboardEvent* event);
#ifdef Q_OS_MACOS
    bool handleCapturedMacKeyEvent(const SDL_Event& event);
#endif

    void handleMouseButtonEvent(SDL_MouseButtonEvent* event);

    void handleMouseMotionEvent(SDL_MouseMotionEvent* event,
                                bool batchPendingEvents = true);

    void handleMouseWheelEvent(SDL_MouseWheelEvent* event);

    void handlePenEvent(const SDL_Event& event);

    void sendText(QString& string);

    void handleRawHidControl(const unsigned char* data, unsigned int length);

    bool handleRemoteCursorChunk(const unsigned char* data, unsigned int length);

    bool handleRemoteCursorPosition(const unsigned char* data, unsigned int length);

    void applyPendingRemoteCursor();

    void applyPendingRemoteCursorPosition();

    void applyPendingTabletCursorActivation();

    void beginRawHidReconnect();
    void finishRawHidReconnect();

    void resetRemoteCursorPositionEpoch();

    void raiseAllKeys();

    void notifyMouseLeave();

    void notifyFocusLost();

    void notifyFocusGained();

    bool isCaptureActive();

    void setToolbarInteractionActive(bool active);

    void setLocalToolbarAvailable(bool available);

    bool isSystemKeyCaptureActive();

    void setCaptureActive(bool active);

    bool isMouseInVideoRegion(int mouseX, int mouseY,
                              Uint32 windowId = 0,
                              int windowWidth = -1,
                              int windowHeight = -1);

    void updateKeyboardGrabState();

    void updatePointerRegionLock();

private:
#ifdef Q_OS_MACOS
    std::unique_ptr<MacQuitShortcut> m_MacQuitShortcut;
    std::unique_ptr<MacKeyboardCapture> m_MacKeyboardCapture;
    bool hasMacStreamKeyboardFocus() const;
#endif
    enum KeyCombo {
        KeyComboQuit,
        KeyComboUngrabInput,
        KeyComboToggleFullScreen,
        KeyComboToggleStatsOverlay,
        KeyComboToggleMinimize,
        KeyComboPasteText,
        KeyComboTogglePointerRegionLock,
        KeyComboToggleKeyboardGrab,
        KeyComboMax
    };

    void performSpecialKeyCombo(KeyCombo combo);

    SDL_Window* m_Window;
    PlankPresentationLayout m_PresentationLayout;
    bool m_NeedsManualCaptureOnLeave;
    bool m_MouseWasInVideoRegion;
    bool m_PendingMouseButtonsAllUpOnVideoRegionLeave;
    bool m_PointerRegionLockActive;
    bool m_PointerRegionLockToggledByUser;
    bool m_LocalToolbarAvailable;
    bool m_LocalCursorSupported;
    PlankEmbeddedCursor m_EmbeddedCursor;
    bool m_RemoteCursorVisible;
    bool m_CompositorCursorRequestedVisible;
    bool m_TabletCursorActive;
    // The sign-in screen clears the host cursor when the pen leaves. Keep the
    // local pointer shown until the host publishes a visible cursor again.
    bool m_HoldLocalPointer = false;

    QSet<short> m_KeysDown;
    bool m_FakeMouseCaptureActive;
    bool m_KeyboardCaptureActive;
    StreamingPreferences::CaptureSysKeysMode m_CaptureSystemKeysMode;
    bool m_MouseCursorCapturedVisibilityState;

    struct RemoteCursorState {
        std::uint64_t generation = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t hotspotX = 0;
        std::uint32_t hotspotY = 0;
        std::uint32_t flags = 0;
        std::uint32_t nextOffset = 0;
        std::vector<unsigned char> pixels;
    };

    std::mutex m_RemoteCursorMutex;
    RemoteCursorState m_RemoteCursorAssembly;
    RemoteCursorState m_ReadyRemoteCursor;
    RemoteCursorState m_AppliedRemoteCursor;
    bool m_RemoteCursorAssemblyActive = false;
    bool m_ReadyRemoteCursorValid = false;
    bool m_AppliedRemoteCursorValid = false;
    std::atomic_bool m_RemoteCursorUpdatePending {false};
    SDL_Cursor* m_RemoteCursor = nullptr;
    SDL_Cursor* m_BlankCursor = nullptr;

    struct RemoteCursorPosition {
        std::uint64_t sequence = 0;
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t frameWidth = 0;
        std::uint32_t frameHeight = 0;
    };

    std::mutex m_RemoteCursorPositionMutex;
    RemoteCursorPosition m_ReadyRemoteCursorPosition;
    RemoteCursorPosition m_AppliedRemoteCursorPosition;
    std::uint64_t m_HighestRemoteCursorPositionSequence = 0;
    std::uint64_t m_AppliedRemoteCursorPositionSequence = 0;
    std::uint64_t m_TabletCursorActivationSequence = 0;
    bool m_ReadyRemoteCursorPositionValid = false;
    bool m_AppliedRemoteCursorPositionValid = false;
    std::atomic_bool m_RemoteCursorPositionUpdatePending {false};
    std::atomic_bool m_TabletCursorActivationPending {false};
    struct WaylandTabletCursorOutput {
        SDL_Window* window = nullptr;
        std::unique_ptr<PlankWaylandCursor> cursor;
    };
    std::vector<WaylandTabletCursorOutput> m_WaylandTabletCursorOutputs;

    void setCursorVisible(bool visible);
    void syncCompositorCursor();
    SDL_Cursor* blankCursor();
    void activateCompositorCursor();
    PlankWaylandCursor* ensureWaylandTabletCursorAttached(
        SDL_Window* targetWindow);
    void reconcileWaylandTabletCursorOutputs();
    bool mapRemoteCursorPositionToWindow(const RemoteCursorPosition& position,
                                         SDL_Window*& window,
                                         int& x, int& y) const;
    void updateTabletCursorVisibility();
    QSize streamDimensions() const;
    bool sendAbsoluteMousePosition(SDL_Window* window,
                                   int windowX, int windowY,
                                   bool allowClampedPosition);
    bool ignorePenAsMouse(unsigned mouseId) const;
#ifdef HAVE_MAC_RAW_WACOM
    bool macRawHidAttached() const;
    void syncMacNormalizedPenActive(bool captureAndFocus);
#endif
    bool mapWindowPointToNormalized(SDL_Window* window,
                                    float windowX, float windowY,
                                    float& nx, float& ny,
                                    bool allowClamped) const;

    SDL_Window* presentationWindow(Uint32 windowId) const;
    SDL_Window* pointerPresentationWindow(SDL_Window* source,
                                          float& x, float& y) const;
    enum class PointerFocusPosition { LocalMouse, HostTablet };
    void followPointerFocus(SDL_Window* target, SDL_MouseButtonFlags eventButtons,
                            PointerFocusPosition position = PointerFocusPosition::LocalMouse);
    const PlankPresentationOutput* presentationOutput(
        SDL_Window* window) const;

    struct {
        KeyCombo keyCombo;
        SDL_Keycode keyCode;
        SDL_Scancode scanCode;
        bool enabled;
    } m_SpecialKeyCombos[KeyComboMax];

    std::atomic_uint64_t m_StreamDimensions;

#ifdef HAVE_MAC_RAW_WACOM
    std::unique_ptr<MacRawWacomInput> m_MacRawWacomInput;
    std::unique_ptr<MacNormalizedPen> m_MacNormalizedPen;
#endif

#ifdef HAVE_LIBINPUT_TABLET
    std::unique_ptr<LinuxWacomInput> m_LinuxWacomInput;
    std::unique_ptr<LinuxRawWacomInput> m_LinuxRawWacomInput;
#endif

    static const int k_ButtonMap[];
};
