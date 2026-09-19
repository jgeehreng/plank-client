// Exercise the production tap callback/queue with synthetic CGEvents. These
// tests never install a system tap, request permission or post OS input.
#include "../../app/streaming/mackeyboardcapture.mm"
#include <QtTest>
#include <vector>

class TestMacKeyboardCapture : public QObject
{
    Q_OBJECT
    bool m_Focus = true;
    bool m_CaptureEnabled = true;
    int m_Releases = 0;
    std::unique_ptr<MacKeyboardCapture> m_Capture;

    MacKeyboardCapture::State& state() { return *m_Capture->m_State; }
    CGEventRef key(unsigned raw, CGEventType type = kCGEventKeyDown, CGEventFlags flags = 0)
    {
        auto event = CGEventCreateKeyboardEvent(nullptr, raw, type == kCGEventKeyDown);
        CGEventSetType(event, type);
        CGEventSetFlags(event, flags);
        return event;
    }
    bool capture(unsigned raw, CGEventType type = kCGEventKeyDown, CGEventFlags flags = 0)
    {
        auto event = key(raw, type, flags);
        bool consumed = state().handle(type, event) == nullptr;
        CFRelease(event);
        return consumed;
    }
    std::vector<SDL_KeyboardEvent> drain()
    {
        std::vector<SDL_KeyboardEvent> result;
        SDL_Event event {};
        while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, state().eventType, state().eventType) == 1) {
            m_Capture->dispatch(event, [&](SDL_KeyboardEvent* k) {
                QVERIFY(!m_Capture->suppressSdlKeyEvent());
                result.push_back(*k);
            });
        }
        return result;
    }

private slots:
    void initTestCase() { QVERIFY(SDL_Init(SDL_INIT_EVENTS)); }
    void cleanupTestCase() { SDL_Quit(); }
    void init()
    {
        m_Focus = true;
        m_CaptureEnabled = true;
        m_Releases = 0;
        m_Capture.reset(new MacKeyboardCapture([this] { return m_Focus && m_CaptureEnabled; },
                                               [this] { ++m_Releases; }, false));
        state().active = true;
        state().isoKeyboard = false;
        state().isTrusted = [] { return false; };
    }
    void cleanup() { m_Capture.reset(); }

    void disabledCaptureDoesNotPromptOrConsumeRequest()
    {
        bool prompted = false;
        QVERIFY(!accessibilityPromptNeeded(false, false, prompted));
        QVERIFY(!prompted);
        QVERIFY(accessibilityPromptNeeded(true, false, prompted));
    }
    void grantedPermissionDoesNotPrompt()
    {
        bool prompted = false;
        QVERIFY(!accessibilityPromptNeeded(true, true, prompted));
        QVERIFY(!prompted);
    }
    void launcherRequestsOnlyOncePerProcess()
    {
        bool prompted = false;
        QVERIFY(accessibilityPromptNeeded(true, false, prompted));
        QVERIFY(prompted);
        QVERIFY(!accessibilityPromptNeeded(true, false, prompted));
        QVERIFY(!accessibilityPromptNeeded(false, false, prompted));
        QVERIFY(!accessibilityPromptNeeded(true, false, prompted));
    }
    void rememberedPromptDoesNotAskAgain()
    {
        bool prompted = true;
        QVERIFY(!accessibilityPromptNeeded(true, false, prompted));
    }

    void commandTabAndSpaceAreQueuedOnce()
    {
        const auto flags = kCGEventFlagMaskCommand | NX_DEVICELCMDKEYMASK;
        QVERIFY(capture(kVK_Command, kCGEventFlagsChanged, flags));
        QVERIFY(capture(kVK_Tab, kCGEventKeyDown, flags));
        QVERIFY(capture(kVK_Tab, kCGEventKeyUp, flags));
        QVERIFY(capture(kVK_Space, kCGEventKeyDown, flags));
        QVERIFY(capture(kVK_Space, kCGEventKeyUp, flags));
        QVERIFY(capture(kVK_Command, kCGEventFlagsChanged));
        auto keys = drain();
        QCOMPARE(keys.size(), size_t(6));
        QCOMPARE(keys[0].scancode, SDL_SCANCODE_LGUI);
        QVERIFY(keys[0].down);
        QCOMPARE(keys[1].scancode, SDL_SCANCODE_TAB);
        QVERIFY(keys[1].mod & SDL_KMOD_LGUI);
        QVERIFY(!keys[2].down);
        QCOMPARE(keys[3].scancode, SDL_SCANCODE_SPACE);
        QVERIFY(!keys[4].down);
        QVERIFY(!keys[5].down);
        QVERIFY(drain().empty());
        QVERIFY(m_Capture->suppressSdlKeyEvent());
    }
    void heldAndRightModifiersAreReconciled()
    {
        const auto flags = kCGEventFlagMaskCommand | NX_DEVICERCMDKEYMASK |
                           kCGEventFlagMaskShift | NX_DEVICERSHIFTKEYMASK;
        QVERIFY(capture(kVK_Tab, kCGEventKeyDown, flags));
        auto keys = drain();
        QCOMPARE(keys.size(), size_t(3));
        QCOMPARE(keys[0].scancode, SDL_SCANCODE_RSHIFT);
        QCOMPARE(keys[1].scancode, SDL_SCANCODE_RGUI);
        QCOMPARE(keys[2].scancode, SDL_SCANCODE_TAB);
        QVERIFY(capture(kVK_RightCommand, kCGEventFlagsChanged));
        keys = drain();
        QCOMPARE(keys.size(), size_t(2));
        QVERIFY(!keys[0].down && !keys[1].down);
    }
    void aggregateModifiersAreNotDuplicated()
    {
        QVERIFY(capture(kVK_Tab, kCGEventKeyDown, kCGEventFlagMaskCommand));
        auto keys = drain();
        QCOMPARE(keys.size(), size_t(2));
        QCOMPARE(keys[0].scancode, SDL_SCANCODE_LGUI);
        QCOMPARE(keys[1].mod, SDL_Keymod(SDL_KMOD_LGUI));
    }
    void quitAndReleaseShortcutStayOnExistingInputPath()
    {
        QVERIFY(capture(kVK_ANSI_Q, kCGEventKeyDown, kCGEventFlagMaskCommand));
        auto keys = drain();
        QCOMPARE(keys.back().scancode, SDL_SCANCODE_Q);
        QVERIFY(!SDL_HasEvent(SDL_EVENT_QUIT));
        QVERIFY(capture(kVK_ANSI_Z, kCGEventKeyDown, kCGEventFlagMaskControl |
                        kCGEventFlagMaskAlternate | kCGEventFlagMaskShift));
        keys = drain();
        QCOMPARE(keys.back().scancode, SDL_SCANCODE_Z);
        QVERIFY(keys.back().mod & SDL_KMOD_CTRL);
        QVERIFY(keys.back().mod & SDL_KMOD_ALT);
        QVERIFY(keys.back().mod & SDL_KMOD_SHIFT);
    }
    void repeatsDoNotFillQueue()
    {
        QVERIFY(capture(kVK_ANSI_A));
        auto event = key(kVK_ANSI_A);
        CGEventSetIntegerValueField(event, kCGKeyboardEventAutorepeat, 1);
        for (int i = 0; i < 1000; ++i) QVERIFY(state().handle(kCGEventKeyDown, event) == nullptr);
        CFRelease(event);
        QCOMPARE(drain().size(), size_t(1));
        QVERIFY(capture(kVK_ANSI_A, kCGEventKeyUp));
        QVERIFY(!drain().back().down);
    }
    void focusLossDiscardsQueuedKeys()
    {
        QVERIFY(capture(kVK_Tab, kCGEventKeyDown, kCGEventFlagMaskCommand));
        m_Focus = false;
        QVERIFY(drain().empty());
        QCOMPARE(m_Releases, 1);
        QVERIFY(!state().active);
        QVERIFY(!capture(kVK_Space));
        QVERIFY(!m_Capture->suppressSdlKeyEvent());
    }
    void focusIsCheckedInTapBeforeQueuedSdlFocusNotification()
    {
        m_Focus = false;
        QVERIFY(!capture(kVK_ANSI_A));
        QCOMPARE(m_Releases, 1);
        QVERIFY(state().pending.empty());
    }
    void repeatedSpaceReturnsCaptureFirstShortcutWithoutRefresh()
    {
        state().isTrusted = [] { return true; };
        for (int i = 0; i < 5; ++i) {
            QVERIFY(capture(kVK_ANSI_A)); // Pending input must not cross Spaces.
            m_Focus = false;
            m_Capture->refresh();
            QVERIFY(!state().active);
            QVERIFY(drain().empty());
            QVERIFY(!capture(kVK_Tab, kCGEventKeyDown, kCGEventFlagMaskCommand));
            QVERIFY(!capture(kVK_Tab, kCGEventKeyUp, kCGEventFlagMaskCommand));
            QVERIFY(drain().empty());

            // No SDL notification, explicit refresh or timer on return.
            m_Focus = true;
            QVERIFY(capture(kVK_Tab, kCGEventKeyDown, kCGEventFlagMaskCommand));
            QVERIFY(capture(kVK_Tab, kCGEventKeyUp, kCGEventFlagMaskCommand));
            QVERIFY(capture(kVK_Command, kCGEventFlagsChanged));
            const auto keys = drain();
            QCOMPARE(keys.size(), size_t(4));
            QCOMPARE(keys[0].scancode, SDL_SCANCODE_LGUI);
            QCOMPARE(keys[1].scancode, SDL_SCANCODE_TAB);
            QVERIFY(keys[1].down && !keys[2].down && !keys[3].down);
            QVERIFY(m_Capture->suppressSdlKeyEvent());
        }
        QCOMPARE(m_Releases, 10); // One release on departure and on re-arm.
    }
    void focusReturnDoesNotOverrideExplicitCaptureRelease()
    {
        state().isTrusted = [] { return true; };
        m_CaptureEnabled = false;
        m_Capture->refresh();
        m_Focus = false;
        QVERIFY(!capture(kVK_ANSI_A));
        m_Focus = true;
        QVERIFY(!capture(kVK_Tab, kCGEventKeyDown, kCGEventFlagMaskCommand));
        QVERIFY(!state().active);
        QVERIFY(drain().empty());
        m_CaptureEnabled = true;
        QVERIFY(capture(kVK_Space, kCGEventKeyDown, kCGEventFlagMaskCommand));
        QCOMPARE(drain().back().scancode, SDL_SCANCODE_SPACE);
    }
    void focusReturnDoesNotOverridePermissionRevocation()
    {
        m_Focus = false;
        m_Capture->refresh();
        m_Focus = true;
        QVERIFY(!capture(kVK_Tab, kCGEventKeyDown, kCGEventFlagMaskCommand));
        QVERIFY(!state().active);
        QVERIFY(drain().empty());
    }
    void backgroundKeysNeverEnterTheRemoteQueue()
    {
        state().isTrusted = [] { return true; };
        m_Focus = false;
        for (int i = 0; i < 10; ++i) {
            QVERIFY(!capture(kVK_ANSI_A));
            QVERIFY(!capture(kVK_Command, kCGEventFlagsChanged, kCGEventFlagMaskCommand));
        }
        QCOMPARE(m_Releases, 1);
        QVERIFY(!state().active);
        QVERIFY(drain().empty());
        QVERIFY(!m_Capture->suppressSdlKeyEvent());
    }
    void releaseDuringDispatchDropsRemainingKeys()
    {
        QVERIFY(capture(kVK_ANSI_A));
        QVERIFY(capture(kVK_ANSI_B));
        SDL_Event event {};
        event.type = state().eventType;
        int delivered = 0;
        QVERIFY(m_Capture->dispatch(event, [&](SDL_KeyboardEvent*) {
            ++delivered;
            state().deactivate();
        }));
        QCOMPARE(delivered, 1);
        QCOMPARE(m_Releases, 1);
        QVERIFY(state().pending.empty());
    }
    void deniedOrRevokedPermissionReleasesCapture()
    {
        QVERIFY(capture(kVK_ANSI_A));
        m_Capture->refresh(); // Injected denied permission, no OS prompt.
        QCOMPARE(m_Releases, 1);
        QVERIFY(!state().active);
        QVERIFY(drain().empty());
        QVERIFY(!capture(kVK_Tab));
        m_Capture->refresh();
        QCOMPARE(m_Releases, 1);
    }
    void disabledTapReleasesBeforeRecovery()
    {
        QVERIFY(capture(kVK_ANSI_A));
        state().handle(kCGEventTapDisabledByTimeout, nullptr);
        QCOMPARE(m_Releases, 1);
        QVERIFY(state().pending.empty());
        state().active = true;
        QVERIFY(capture(kVK_Space));
        auto keys = drain();
        QCOMPARE(keys.size(), size_t(1));
        QCOMPARE(keys[0].scancode, SDL_SCANCODE_SPACE);
        state().handle(kCGEventTapDisabledByUserInput, nullptr);
        QCOMPARE(m_Releases, 2);
    }
    void overflowFailsOpenAndReleasesRemoteKeys()
    {
        for (unsigned i = 0; i < state().QueueLimit; ++i) QVERIFY(capture(kVK_ANSI_A));
        QVERIFY(!capture(kVK_ANSI_A));
        QVERIFY(!state().active);
        QVERIFY(drain().empty());
        QCOMPARE(m_Releases, 1);
    }
    void sdlQueueFailureDoesNotSwallowInput()
    {
        SDL_SetEventFilter([](void*, SDL_Event*) { return false; }, nullptr);
        const bool consumed = capture(kVK_ANSI_A);
        SDL_SetEventFilter(nullptr, nullptr);
        QVERIFY(!consumed);
        QVERIFY(!state().active);
        QCOMPARE(m_Releases, 1);
    }
    void capsLockIsABalancedPress()
    {
        QVERIFY(capture(kVK_CapsLock, kCGEventFlagsChanged, kCGEventFlagMaskAlphaShift));
        auto keys = drain();
        QCOMPARE(keys.size(), size_t(2));
        QCOMPARE(keys[0].scancode, SDL_SCANCODE_CAPSLOCK);
        QVERIFY(keys[0].down && !keys[1].down);
    }
    void isoAnsiAndJisMappingsMatchSdl()
    {
        QVERIFY(capture(10));
        QCOMPARE(drain().back().scancode, SDL_SCANCODE_NONUSBACKSLASH);
        state().isoKeyboard = true;
        QVERIFY(capture(10));
        QCOMPARE(drain().back().scancode, SDL_SCANCODE_GRAVE);
        QVERIFY(capture(50));
        QCOMPARE(drain().back().scancode, SDL_SCANCODE_NONUSBACKSLASH);
        QVERIFY(capture(93));
        QCOMPARE(drain().back().scancode, SDL_SCANCODE_INTERNATIONAL3);
    }
    void gesturesAndHardwareControlsStayLocal()
    {
        auto event = CGEventCreate(nullptr);
        QVERIFY(state().handle(kCGEventScrollWheel, event) == event);
        QVERIFY(state().handle(kCGEventMouseMoved, event) == event);
        CFRelease(event);
        QVERIFY(!capture(kVK_Function, kCGEventFlagsChanged));
        QVERIFY(!capture(kVK_VolumeUp));
        QVERIFY(drain().empty());
    }
    void teardownFlushesOnlyItsOwnEvents()
    {
        QVERIFY(capture(kVK_ANSI_A));
        const auto type = state().eventType;
        SDL_Event other {};
        other.type = SDL_EVENT_QUIT;
        QVERIFY(SDL_PushEvent(&other));
        m_Capture.reset();
        QVERIFY(!SDL_HasEvent(type));
        QVERIFY(SDL_HasEvent(SDL_EVENT_QUIT));
        SDL_FlushEvent(SDL_EVENT_QUIT);
        QCOMPARE(m_Releases, 1);
    }
    void nativeForceQuitRemainsLocal()
    {
        QVERIFY(!capture(kVK_Escape, kCGEventKeyDown,
                         kCGEventFlagMaskCommand | kCGEventFlagMaskAlternate));
        QVERIFY(!state().active);
        QCOMPARE(m_Releases, 1);
        QVERIFY(drain().empty());
    }
    void sessionControlEventsAreNotConsumed()
    {
        QVERIFY(state().eventType != SDL_EVENT_USER);
        SDL_Event event {};
        event.type = SDL_EVENT_USER;
        QVERIFY(!m_Capture->dispatch(event, [](SDL_KeyboardEvent*) { QFAIL("unexpected key"); }));
    }
};

QTEST_GUILESS_MAIN(TestMacKeyboardCapture)
#include "test_mackeyboardcapture.moc"
