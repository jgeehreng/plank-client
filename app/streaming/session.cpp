#include "session.h"
#include "streaming/clientframeflowtrace.h"
#include "backend/hostrecovery.h"
#include "backend/planknetwork.h"
#include "settings/streamingpreferences.h"
#include "streaming/avsynccontroller.h"
#include "streaming/plankdisplaymode.h"
#include "streaming/planktoolbar.h"
#include "streaming/streamutils.h"
#include "streaming/input/plankmousemotion.h"
#ifdef Q_OS_MACOS
#include "macclipboardsync.h"
#endif
#include "backend/computermanager.h"
#include "backend/nvaddress.h"
#ifdef Q_OS_MACOS
#include "macapplication.h"
#endif
#ifdef Q_OS_DARWIN
#include "streaming/macwindow.h"
#include "streaming/macdisplaygeometry.h"
#endif

#include <Limelight.h>
#include <SDL3/SDL.h>
#include "utils.h"

#ifdef HAVE_FFMPEG
#include "video/ffmpeg.h"
#endif

#ifdef HAVE_SLVIDEO
#include "video/slvid.h"
#endif

#ifdef Q_OS_WIN32
// Scaling the icon down on Win32 looks dreadful, so render at lower res
#define ICON_SIZE 32
#else
#define ICON_SIZE 64
#endif

// HACK: Remove once proper Dark Mode support lands in SDL
#ifdef Q_OS_WIN32
#include <SDL3/SDL_system.h>
#include <dwmapi.h>
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE_OLD
#define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#endif


#define SDL_CODE_FLUSH_WINDOW_EVENT_BARRIER 100
#define SDL_CODE_PLANK_RECONNECT 105
#define SDL_CODE_PLANK_BITRATE_APPLIED 106
#define SDL_CODE_PLANK_CURSOR 107
#define SDL_CODE_PLANK_TABLET_CURSOR 108
#define SDL_CODE_PLANK_CURSOR_POSITION 109
#define SDL_CODE_PLANK_REPLANK_COMPLETE 110
#define SDL_CODE_PLANK_CLIPBOARD 111
#define SDL_CODE_PLANK_CLIPBOARD_POLL 112

#include <QtEndian>
#include <QCoreApplication>
#include <QThreadPool>
#include <QImage>
#include <QGuiApplication>
#include <QCursor>
#include <QWindow>
#include <QScreen>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#ifdef PLANK_TRANSPORT
#include "plank_transport.h"
#include "plank_transport_control.h"
#include "plank_transport_event.h"
#include "plank_transport_input.h"
#include "plank_transport_setup.h"
#endif

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

CONNECTION_LISTENER_CALLBACKS Session::k_ConnCallbacks = {
    Session::clStageStarting,
    nullptr,
    Session::clStageFailed,
    nullptr,
    Session::clConnectionTerminated,
    Session::clLogMessage,
    nullptr,
    Session::clConnectionStatusUpdate,
    Session::clSetHdrMode,
    nullptr,
    nullptr,
    nullptr,
    Session::clRawHidControl,
    Session::clVideoBitrateApplied,
    Session::clCursorChunk,
    Session::clCursorPosition,
    nullptr, // Native transport publishes the paired FEC counters directly.
};

Session* Session::s_ActiveSession;
QSemaphore Session::s_ActiveSessionSemaphore(1);

void Session::clStageStarting(int stage)
{
    // We know this is called on the same thread as LiStartConnection()
    // which happens to be the main thread, so it's cool to interact
    // with the GUI in these callbacks.
    emit s_ActiveSession->stageStarting(QString::fromLocal8Bit(LiGetStageName(stage)));
}

void Session::clStageFailed(int stage, int errorCode)
{
    if (s_ActiveSession->m_Reconnecting.load()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK reconnect stage failed: %s (%d)",
                    LiGetStageName(stage), errorCode);
        return;
    }

    QString failingEndpoint;
    if (stage == STAGE_SESSION_NEGOTIATION || stage == STAGE_CONTROL_STREAM_START) {
        failingEndpoint = QStringLiteral("UDP %1").arg(
                    s_ActiveSession->m_Computer->activeAddress.port());
    }

    emit s_ActiveSession->stageFailed(QString::fromLocal8Bit(LiGetStageName(stage)),
                                      errorCode,
                                      failingEndpoint);
}

void Session::clConnectionTerminated(int errorCode)
{
    if (static_cast<std::uint32_t>(errorCode) ==
            PLANK_TRANSPORT_TERMINATION_SESSION_TAKEN_OVER) {
        s_ActiveSession->m_CanReconnect.store(false);
        s_ActiveSession->m_ReconnectCancelled.store(true);
        s_ActiveSession->m_UnexpectedTermination = false;
        emit s_ActiveSession->displayLaunchError(
                    tr("This PLANK session was transferred to another client."));

        SDL_Event event = {};
        event.type = SDL_EVENT_QUIT;
        event.quit.timestamp = SDL_GetTicks();
        SDL_PushEvent(&event);
        return;
    }

    if (s_ActiveSession->m_Computer->plankAuthentication &&
            s_ActiveSession->m_CanReconnect.load() &&
            !s_ActiveSession->m_Reconnecting.load() &&
            !s_ActiveSession->m_ReconnectRequested.exchange(true)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK transport ended (%d); starting responsive reconnect",
                    errorCode);
        SDL_Event event = {};
        event.type = SDL_EVENT_USER;
        event.user.code = SDL_CODE_PLANK_RECONNECT;
        SDL_PushEvent(&event);
        return;
    }

    // The early worker probe and transport termination can arrive together.
    // A queued reconnect is already owned by the SDL thread, just like a
    // running one; duplicate callbacks must not enqueue a fatal quit.
    if (s_ActiveSession->m_ReconnectRequested.load() || s_ActiveSession->m_Reconnecting.load()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK reconnect attempt ended: %d", errorCode);
        return;
    }

    // Display the termination dialog if this was not intended
    switch (errorCode) {
    case ML_ERROR_GRACEFUL_TERMINATION:
        break;

    case ML_ERROR_NO_VIDEO_TRAFFIC:
        s_ActiveSession->m_UnexpectedTermination = true;

        emit s_ActiveSession->displayLaunchError(tr("No video received from host.") + "\n\n"+
                                                 tr("Check your firewall rules for UDP port %1.")
                                                 .arg(s_ActiveSession->m_Computer->activeAddress.port()));
        break;

    case ML_ERROR_NO_VIDEO_FRAME:
        s_ActiveSession->m_UnexpectedTermination = true;
        emit s_ActiveSession->displayLaunchError(tr("Your network connection isn't performing well. Reduce your video bitrate setting or try a faster connection."));
        break;

    case ML_ERROR_PROTECTED_CONTENT:
    case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
        s_ActiveSession->m_UnexpectedTermination = true;
        emit s_ActiveSession->displayLaunchError(tr("Something went wrong on your host PC when starting the stream.") + "\n\n" +
                                                 tr("Make sure you don't have any DRM-protected content open on your host PC. You can also try restarting your host PC."));
        break;

    case ML_ERROR_FRAME_CONVERSION:
        s_ActiveSession->m_UnexpectedTermination = true;
        emit s_ActiveSession->displayLaunchError(tr("The host PC reported a fatal video encoding error.") + "\n\n" +
                                                 tr("Try disabling HDR mode, changing the streaming resolution, or changing your host PC's display resolution."));
        break;

    default:
        s_ActiveSession->m_UnexpectedTermination = true;

        // We'll assume large errors are hex values
        bool hexError = qAbs(errorCode) > 1000;
        emit s_ActiveSession->displayLaunchError(tr("Connection terminated") + "\n\n" +
                                                 tr("Error code: %1").arg(errorCode, hexError ? 8 : 0, hexError ? 16 : 10, QChar('0')));
        break;
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "Connection terminated: %d",
                 errorCode);

    // Push a quit event to the main loop
    SDL_Event event;
    event.type = SDL_EVENT_QUIT;
    event.quit.timestamp = SDL_GetTicks();
    SDL_PushEvent(&event);
}

void Session::clLogMessage(const char* format, ...)
{
    va_list ap;

    va_start(ap, format);
    SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION,
                    SDL_LOG_PRIORITY_INFO,
                    format,
                    ap);
    va_end(ap);
}

void Session::clConnectionStatusUpdate(int connectionStatus)
{
    if (s_ActiveSession->m_Reconnecting.load()) return;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Connection status update: %d",
                connectionStatus);

    if (!s_ActiveSession->m_Preferences->connectionWarnings) {
        return;
    }

    switch (connectionStatus)
    {
    case CONN_STATUS_POOR:
        s_ActiveSession->m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate,
                                                            s_ActiveSession->m_StreamConfig.bitrate > 5000 ?
                                                                "Slow connection to PC\nReduce your bitrate" : "Poor connection to PC");
        s_ActiveSession->m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);
        break;
    case CONN_STATUS_OKAY:
        s_ActiveSession->m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, false);
        break;
    }
}

void Session::clSetHdrMode(bool enabled)
{
    // If we're in the process of recreating our decoder when we get
    // this callback, we'll drop it. The main thread will make the
    // callback when it finishes creating the new decoder.
    if (SDL_TryLockSpinlock(&s_ActiveSession->m_DecoderLock)) {
        IVideoDecoder* decoder = s_ActiveSession->m_VideoDecoder;
        if (decoder != nullptr) {
            decoder->setHdrMode(enabled);
        }
        SDL_UnlockSpinlock(&s_ActiveSession->m_DecoderLock);
    }
}

void Session::clRawHidControl(const unsigned char* data, unsigned int length)
{
    if (s_ActiveSession != nullptr && s_ActiveSession->m_InputHandler != nullptr) {
        s_ActiveSession->m_InputHandler->handleRawHidControl(data, length);
    }
}

void Session::clVideoBitrateApplied(
        uint32_t requestedKbps, uint32_t appliedKbps, uint32_t peakKbps)
{
    if (s_ActiveSession == nullptr) {
        return;
    }

    s_ActiveSession->m_ConfirmedBitrateRequestKbps.store(
                static_cast<int>(requestedKbps), std::memory_order_relaxed);
    s_ActiveSession->m_ConfirmedBitrateAppliedKbps.store(
                static_cast<int>(appliedKbps), std::memory_order_relaxed);
    s_ActiveSession->m_ConfirmedBitratePeakKbps.store(
                static_cast<int>(peakKbps), std::memory_order_relaxed);

    SDL_Event event = {};
    event.type = SDL_EVENT_USER;
    event.user.code = SDL_CODE_PLANK_BITRATE_APPLIED;
    SDL_PushEvent(&event);
}

void Session::clCursorChunk(const unsigned char* data, unsigned int length)
{
    if (s_ActiveSession == nullptr || s_ActiveSession->m_InputHandler == nullptr) {
        return;
    }
    if (s_ActiveSession->m_InputHandler->handleRemoteCursorChunk(data, length)) {
        SDL_Event event = {};
        event.type = SDL_EVENT_USER;
        event.user.code = SDL_CODE_PLANK_CURSOR;
        SDL_PushEvent(&event);
    }
}

void Session::clCursorPosition(const unsigned char* data, unsigned int length)
{
    if (s_ActiveSession == nullptr || s_ActiveSession->m_InputHandler == nullptr) {
        return;
    }
    if (s_ActiveSession->m_InputHandler->handleRemoteCursorPosition(data, length)) {
        SDL_Event event = {};
        event.type = SDL_EVENT_USER;
        event.user.code = SDL_CODE_PLANK_CURSOR_POSITION;
        SDL_PushEvent(&event);
    }
}

void Session::postTabletCursorActivationEvent()
{
    SDL_Event event = {};
    event.type = SDL_EVENT_USER;
    event.user.code = SDL_CODE_PLANK_TABLET_CURSOR;
    SDL_PushEvent(&event);
}

void Session::updateVideoFecLoss(VideoFecLossPercent loss)
{
    Session* session = s_ActiveSession;
    if (session == nullptr) {
        return;
    }

    const Uint64 now = SDL_GetTicks();
    std::lock_guard<std::mutex> lock(session->m_VideoPacketLossSamplesLock);
    // Publish both ten-second peaks together. Toolbar and overlay share this
    // snapshot; frame drops and decoder/render queues are separate metrics.
    session->m_CurrentVideoFecLoss = {
        session->m_VideoPacketLossPeakWindow.addSample(now, loss.before),
        session->m_VideoPacketLossAfterFecPeakWindow.addSample(now, loss.after)
    };
}

bool Session::chooseDecoder(DecoderSelectionMode selectionMode,
                            SDL_Window* window, int videoFormat, int width, int height,
                            int frameRate, bool enableVsync, bool testOnly,
                            IVideoDecoder*& chosenDecoder, bool enableIdentityGbr,
                            DecoderCaptureSource captureSource,
                            DecoderEncoderBackend encoderBackend)
{
    DECODER_PARAMETERS params = {};

    // We should never have vsync enabled for test-mode.
    // It introduces unnecessary delay for renderers that may
    // block while waiting for a backbuffer swap.
    SDL_assert(!enableVsync || !testOnly);

    params.width = width;
    params.height = height;
    params.frameRate = frameRate;
    params.videoFormat = videoFormat;
    params.window = window;
    params.enableVsync = enableVsync;
    // PLANK is a latency-first Wayland workstation client. Keep the
    // optional software pacer disabled; renderers may still force pacing when
    // their backend requires it for correctness.
    params.enableFramePacing = false;
    params.enableIdentityGbr = enableIdentityGbr;
    params.testOnly = testOnly;
    params.selectionMode = selectionMode;
    params.captureSource = captureSource;
    params.encoderBackend = encoderBackend;
    params.presentationLayout = !testOnly && s_ActiveSession != nullptr ?
                &s_ActiveSession->m_PresentationLayout : nullptr;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "V-sync %s",
                enableVsync ? "enabled" : "disabled");

#ifdef HAVE_SLVIDEO
    chosenDecoder = new SLVideoDecoder(testOnly);
    if (chosenDecoder->initialize(&params)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SLVideo video decoder chosen");
        return true;
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to load SLVideo decoder");
        delete chosenDecoder;
        chosenDecoder = nullptr;
    }
#endif

#ifdef HAVE_FFMPEG
    chosenDecoder = new FFmpegVideoDecoder(testOnly);
    if (chosenDecoder->initialize(&params)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "FFmpeg-based video decoder chosen");
        return true;
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to load FFmpeg decoder");
        delete chosenDecoder;
        chosenDecoder = nullptr;
    }
#endif

#if !defined(HAVE_FFMPEG) && !defined(HAVE_SLVIDEO)
#error No video decoding libraries available!
#endif

    // If we reach this, we didn't initialize any decoders successfully
    return false;
}

bool Session::isIdentityGbrEnabledForFormat(int videoFormat) const
{
    if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) return false;
    return (videoFormat == VIDEO_FORMAT_H264_HIGH8_444 ||
            videoFormat == VIDEO_FORMAT_H264_HIGH10_444 ||
            videoFormat == VIDEO_FORMAT_H265_REXT8_444 ||
            videoFormat == VIDEO_FORMAT_H265_REXT10_444) &&
           (m_Computer->serverCodecModeSupport & SCM_IDENTITY_GBR_444);
}

int Session::drSetup(int videoFormat, int width, int height, int frameRate, void *, int)
{
    s_ActiveSession->m_ActiveVideoFormat = videoFormat;
    s_ActiveSession->m_ActiveVideoWidth = width;
    s_ActiveSession->m_ActiveVideoHeight = height;
    s_ActiveSession->m_ActiveVideoFrameRate = frameRate;

    // Defer decoder setup until we've started streaming so we
    // don't have to hide and show the SDL window (which seems to
    // cause pointer hiding to break on Windows).

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Video stream is %dx%dx%d (format 0x%x)",
                width, height, frameRate, videoFormat);
    if (s_ActiveSession->isIdentityGbrEnabledForFormat(videoFormat)) {
        if (videoFormat == VIDEO_FORMAT_H264_HIGH8_444) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Video precision: native 8-bit RGB -> "
                        "8-bit H.264 4:4:4 -> 8-bit RGB identity presentation");
        }
        else if (videoFormat == VIDEO_FORMAT_H265_REXT8_444) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Video precision: native 8-bit NvFBC source -> "
                        "8-bit HEVC 4:4:4 -> 8-bit RGB identity presentation");
        }
        else {
            const bool native10 = s_ActiveSession->m_PlankCaptureSource ==
                    StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Video precision: %s -> 10-bit %s 4:4:4 -> "
                        "10-bit RGB identity presentation",
                        native10 ? "native 10-bit X11/XShm" :
                                   "8-bit NvFBC source/up-converted",
                        videoFormat == VIDEO_FORMAT_H264_HIGH10_444 ? "H.264" : "HEVC");
        }
    }
    else if (videoFormat == VIDEO_FORMAT_H264_HIGH8_422 ||
             videoFormat == VIDEO_FORMAT_H264_HIGH10_422) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Video precision: 8-bit source -> %s H.264 4:2:2 -> "
                    "BT.709 full-range YCbCr presentation",
                    videoFormat == VIDEO_FORMAT_H264_HIGH10_422 ? "10-bit" : "8-bit");
    }

    return 0;
}

int Session::drSubmitDecodeUnit(PDECODE_UNIT du)
{
    // Use a lock since we'll be yanking this decoder out
    // from underneath the session when we initiate destruction.
    // We need to destroy the decoder on the main thread to satisfy
    // some API constraints (like DXVA2). If we can't acquire it,
    // that means the decoder is about to be destroyed, so we can
    // safely return DR_OK and wait for the IDR frame request by
    // the decoder reinitialization code.

    if (SDL_TryLockSpinlock(&s_ActiveSession->m_DecoderLock)) {
        IVideoDecoder* decoder = s_ActiveSession->m_VideoDecoder;
        if (decoder != nullptr) {
            int ret = decoder->submitDecodeUnit(du);
            SDL_UnlockSpinlock(&s_ActiveSession->m_DecoderLock);
            return ret;
        }
        else {
            SDL_UnlockSpinlock(&s_ActiveSession->m_DecoderLock);
            return DR_OK;
        }
    }
    else {
        // Decoder is going away. Ignore anything coming in until
        // the lock is released.
        return DR_OK;
    }
}

void Session::getDecoderInfo(SDL_Window* window,
                             bool& isHardwareAccelerated, bool& isFullScreenOnly,
                             QSize& maxResolution)
{
    IVideoDecoder* decoder;

    // Since AV1 support on the host side is in its infancy, let's not consider
    // _only_ a working AV1 decoder to be acceptable and still show the warning
    // dialog indicating lack of hardware decoding support.

    // Try a regular hardware accelerated HEVC decoder now
    if (chooseDecoder(DecoderSelectionMode::ExactHardwareOnly,
                      window, VIDEO_FORMAT_H265, 1920, 1080, 60,
                      false, true, decoder)) {
        isHardwareAccelerated = decoder->isHardwareAccelerated();
        isFullScreenOnly = decoder->isAlwaysFullScreen();
        maxResolution = decoder->getDecoderMaxResolution();
        delete decoder;

        return;
    }


#if 0 // See AV1 comment at the top of this function
    if (chooseDecoder(DecoderSelectionMode::ExactHardwareOnly,
                      window, VIDEO_FORMAT_AV1_MAIN8, 1920, 1080, 60,
                      false, true, decoder)) {
        isHardwareAccelerated = decoder->isHardwareAccelerated();
        isFullScreenOnly = decoder->isAlwaysFullScreen();
        maxResolution = decoder->getDecoderMaxResolution();
        delete decoder;

        return;
    }
#endif

    // If we still didn't find a hardware decoder, try H.264 now.
    // This will fall back to software decoding, so it should always work.
    if (chooseDecoder(DecoderSelectionMode::PreferExactHardwareThenSoftware,
                      window, VIDEO_FORMAT_H264, 1920, 1080, 60,
                      false, true, decoder)) {
        isHardwareAccelerated = decoder->isHardwareAccelerated();
        isFullScreenOnly = decoder->isAlwaysFullScreen();
        maxResolution = decoder->getDecoderMaxResolution();
        delete decoder;

        return;
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "Failed to find ANY working H.264 or HEVC decoder!");
}

Session::DecoderAvailability
Session::getDecoderAvailability(SDL_Window* window,
                                int videoFormat, int width, int height, int frameRate,
                                bool enableIdentityGbr)
{
    IVideoDecoder* decoder;

    if (!chooseDecoder(DecoderSelectionMode::PreferExactHardwareThenSoftware,
                       window, videoFormat, width, height, frameRate,
                       false, true, decoder, enableIdentityGbr,
                       decoderCaptureSource(), decoderEncoderBackend())) {
        return DecoderAvailability::None;
    }

    bool hw = decoder->isHardwareAccelerated();

    delete decoder;

    return hw ? DecoderAvailability::Hardware : DecoderAvailability::Software;
}

DecoderCaptureSource Session::decoderCaptureSource() const
{
    if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) {
        return DecoderCaptureSource::ScreenCaptureKit;
    }
    return m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10 ?
                DecoderCaptureSource::NativeX11_10Bit : DecoderCaptureSource::Nvfbc8Bit;
}

DecoderEncoderBackend Session::decoderEncoderBackend() const
{
    if (StreamingPreferences::isPlankAppleProfile(m_PlankVideoProfile)) {
        return DecoderEncoderBackend::VideoToolbox;
    }
    return StreamingPreferences::isPlankNvencProfile(m_PlankVideoProfile) ?
                DecoderEncoderBackend::NvencDirect : DecoderEncoderBackend::SoftwareCuda;
}

bool Session::populateDecoderProperties(SDL_Window* window)
{
    IVideoDecoder* decoder;

    if (!chooseDecoder(DecoderSelectionMode::PreferExactHardwareThenSoftware,
                       window,
                       m_SupportedVideoFormats.first(),
                       m_StreamConfig.width,
                       m_StreamConfig.height,
                       m_StreamConfig.fps,
                       false, true, decoder,
                       isIdentityGbrEnabledForFormat(m_SupportedVideoFormats.first()),
                       decoderCaptureSource(), decoderEncoderBackend())) {
        return false;
    }

    m_VideoCallbacks.capabilities = decoder->getDecoderCapabilities();
    if (m_VideoCallbacks.capabilities & CAPABILITY_PULL_RENDERER) {
        // It is an error to pass a push callback when in pull mode
        m_VideoCallbacks.submitDecodeUnit = nullptr;
    }
    else {
        m_VideoCallbacks.submitDecodeUnit = drSubmitDecodeUnit;
    }

    if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) {
        // This profile has an exact, negotiated color contract. An environment
        // override must not reinterpret its YCbCr samples as full-range or RGB.
        m_StreamConfig.colorSpace = COLORSPACE_REC_709;
        m_StreamConfig.colorRange = COLOR_RANGE_LIMITED;
    }
    else {
        bool ok;

        m_StreamConfig.colorSpace = qEnvironmentVariableIntValue("COLOR_SPACE_OVERRIDE", &ok);
        if (ok) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Using colorspace override: %d",
                        m_StreamConfig.colorSpace);
        }
        else {
            m_StreamConfig.colorSpace = decoder->getDecoderColorspace();
        }

        m_StreamConfig.colorRange = qEnvironmentVariableIntValue("COLOR_RANGE_OVERRIDE", &ok);
        if (ok) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Using color range override: %d",
                        m_StreamConfig.colorRange);
        }
        else {
            m_StreamConfig.colorRange = decoder->getDecoderColorRange();
        }
    }

    if (decoder->isAlwaysFullScreen()) {
        m_IsFullScreen = true;
    }

    delete decoder;

    return true;
}

Session::Session(NvComputer* computer, NvApp& app,
                 StreamingPreferences *preferences,
                 ComputerManager *computerManager)
    : m_Preferences(preferences ? preferences : StreamingPreferences::get()),
      m_IsFullScreen(m_Preferences->windowMode != StreamingPreferences::WM_WINDOWED || !WMUtils::isRunningDesktopEnvironment()),
      m_Computer(computer),
      m_PlankVideoProfile(static_cast<StreamingPreferences::PlankVideoProfile>(
              qBound(static_cast<int>(StreamingPreferences::PLANK_PROFILE_H264_10BIT_444),
                     computer->plankVideoProfile,
                     static_cast<int>(StreamingPreferences::PLANK_PROFILE_COUNT) - 1))),
      m_PlankCaptureSource(static_cast<StreamingPreferences::PlankCaptureSource>(
              qBound(static_cast<int>(StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT),
                     computer->plankCaptureSource,
                     static_cast<int>(StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT)))),
      m_PlankBitrateKbps(
              StreamingPreferences::plankBitrateForProfile(
                  computer->plankProfileBitratesKbps,
                  computer->plankVideoProfile)),
      m_ComputerManager(computerManager),
      m_App(app),
      m_Window(nullptr),
      m_VideoDecoder(nullptr),
      m_DecoderLock(0),
      m_AudioMuted(false),
      m_FullScreenFlag(SDL_WINDOW_FULLSCREEN),
      m_QtWindow(nullptr),
      m_UnexpectedTermination(true), // Failure prior to streaming is unexpected
      m_ReconnectRequested(false),
      m_Reconnecting(false),
      m_ReconnectCancelled(false),
      m_CanReconnect(false),
      m_ConnectionStartCancelled(false),
      m_WaitingForSessionCleanup(false),
      m_InputHandler(nullptr),
      m_FlushingWindowEventsRef(0),
      m_AsyncConnectionSuccess(false),
      m_OpusDecoder(nullptr),
      m_AudioRenderer(nullptr),
      m_AudioSampleCount(0),
      m_DropAudioEndTime(0),
      m_AudioMediaFramesReceived(0),
      m_AvSyncTelemetryEnabled(qEnvironmentVariableIntValue("PLANK_AV_SYNC_TELEMETRY") > 0),
      m_LastAudioTelemetryTime(0),
      m_CurrentRenderedFps(0.0f),
      m_CurrentVideoMbps(0.0f),
      m_CurrentNetworkRttMs(0)
{
    if (m_Computer->plankAuthentication) {
        if (m_ComputerManager != nullptr) {
            m_ComputerManager->takePlankReconnectCredentials(
                        m_Computer, m_PlankUsername,
                        m_PlankPassword);
            m_CanReconnect.store(!m_PlankUsername.isEmpty() &&
                                 !m_PlankPassword.isEmpty());
        }
        PlankAvSync::resetVideoClock();

        // PLANK is a qualified workstation protocol, not a generic
        // game-streaming profile. Its stream size is selected after SDL video
        // initialization from the target client display or explicit override.
        // The bookmark owns one startup encoder target for each exact encoding
        // profile. The selected value is a session-local copy, and the toolbar
        // never writes changes back to the bookmark.
        m_Preferences->fps = 60;
        m_Preferences->identityGbrBitDepth =
                (m_PlankVideoProfile ==
                     StreamingPreferences::PLANK_PROFILE_H264_8BIT_422 ||
                 m_PlankVideoProfile ==
                     StreamingPreferences::PLANK_PROFILE_H264_8BIT_444 ||
                 m_PlankVideoProfile ==
                     StreamingPreferences::PLANK_PROFILE_NVENC_H264_8BIT_444 ||
                 m_PlankVideoProfile ==
                     StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444) ? 8 : 10;
        // Decoder selection is internal and exact-profile constrained. Hardware
        // is accepted only after a test frame proves the requested bit depth,
        // chroma sampling, and identity mapping; otherwise the same profile
        // falls back to FFmpeg software decoding without changing formats.
    }
}

Session::~Session()
{
    stopPlankTransportDataPlane();
    clearPlankReconnectCredentials();
}

bool Session::startPlankTransportDataPlane(quint16 port,
                                      const QString& certificateSha256,
                                      const QString& token,
                                      quint16 quicUdpPayloadMtu)
{
#ifndef PLANK_TRANSPORT
    Q_UNUSED(port)
    Q_UNUSED(certificateSha256)
    Q_UNUSED(token)
    Q_UNUSED(quicUdpPayloadMtu)
    return false;
#else
    stopPlankTransportDataPlane();

    QHostAddress remoteHost(m_Computer->activeAddress.address());
    if (remoteHost.isNull()) {
        const QHostInfo resolved = QHostInfo::fromName(
                    m_Computer->activeAddress.address());
        for (const QHostAddress& candidate : resolved.addresses()) {
            if (candidate.protocol() == QAbstractSocket::IPv4Protocol) {
                remoteHost = candidate;
                break;
            }
        }
        if (remoteHost.isNull() && !resolved.addresses().isEmpty()) {
            remoteHost = resolved.addresses().first();
        }
    }
    if (remoteHost.isNull()) {
        qWarning() << "Unable to resolve the PLANK transport endpoint";
        return false;
    }

    const QString remoteAddress = remoteHost.protocol() == QAbstractSocket::IPv6Protocol ?
                QStringLiteral("[%1]:%2").arg(remoteHost.toString()).arg(port) :
                QStringLiteral("%1:%2").arg(remoteHost.toString()).arg(port);
    const QByteArray remoteAddressUtf8 = remoteAddress.toUtf8();
    const QByteArray serverNameUtf8("plank");
    const QByteArray certificateUtf8 = certificateSha256.toLatin1();
    const QByteArray tokenUtf8 = token.toLatin1();

    PlankTransportConfig config {};
    config.struct_size = sizeof(config);
    config.abi_version = PLANK_TRANSPORT_ABI_VERSION;
    config.mode = PLANK_TRANSPORT_MODE_CLIENT;
    config.handshake_timeout_ms = 10000;
    config.idle_timeout_ms = 30000;
    config.keep_alive_interval_ms = 5000;
    config.max_udp_payload_size = quicUdpPayloadMtu;
    qInfo() << "Using the negotiated fixed maximum QUIC UDP payload:"
            << config.max_udp_payload_size << "bytes";
    config.remote_address = remoteAddressUtf8.constData();
    config.server_name = serverNameUtf8.constData();
    config.certificate_sha256 = certificateUtf8.constData();
    config.session_token = tokenUtf8.constData();

    PlankTransportNativeEndpoint* endpoint = nullptr;
    int result = plank_transport_native_endpoint_create(&config, &endpoint);
    if (result == PLANK_TRANSPORT_OK) {
        result = plank_transport_native_endpoint_start(endpoint);
    }
    if (result == PLANK_TRANSPORT_OK) {
        result = plank_transport_native_endpoint_wait_ready(endpoint, 12000);
    }
    if (result != PLANK_TRANSPORT_OK) {
        QByteArray error(512, '\0');
        if (endpoint != nullptr) {
            plank_transport_native_endpoint_last_error(
                        endpoint, error.data(), static_cast<size_t>(error.size()));
            plank_transport_native_endpoint_stop(endpoint);
            plank_transport_native_endpoint_destroy(endpoint);
        }
        qWarning() << "Experimental plank_transport handshake failed:" << error.constData();
        return false;
    }

    m_PlankTransportEndpoint = endpoint;
    LiSetPlankNativeControlSender(
                &Session::plankTransportNativeControlSender, endpoint);
    LiSetPlankNativeInputSender(
                &Session::plankTransportNativeInputSender, endpoint);
    qInfo() << "Experimental native KyProto media, input, and data protocols are ready on UDP"
            << port;
    return true;
#endif
}

bool Session::negotiatePlankTransportSession(quint16 sessionPort, QString& errorMessage)
{
#ifndef PLANK_TRANSPORT
    Q_UNUSED(sessionPort)
    errorMessage = tr("Native PLANK session negotiation is unavailable.");
    return false;
#else
    if (m_PlankTransportEndpoint == nullptr) {
        errorMessage = tr("The native PLANK transport is not connected.");
        return false;
    }

    const int negotiatedVideoFormat = m_StreamConfig.supportedVideoFormats;
    int codec = 0;
    int chroma = 1;
    bool tenBit = false;
    switch (negotiatedVideoFormat) {
    case VIDEO_FORMAT_H264_HIGH8_422:
        chroma = 2;
        break;
    case VIDEO_FORMAT_H264_HIGH8_444:
        break;
    case VIDEO_FORMAT_H264_HIGH10_422:
        chroma = 2;
        tenBit = true;
        break;
    case VIDEO_FORMAT_H264_HIGH10_444:
        tenBit = true;
        break;
    case VIDEO_FORMAT_H265_REXT8_444:
        codec = 1;
        break;
    case VIDEO_FORMAT_H265_REXT10_444:
        codec = 1;
        tenBit = true;
        break;
    case VIDEO_FORMAT_H265_MAIN10:
        codec = 1;
        chroma = 0;
        tenBit = true;
        break;
    default:
        errorMessage = tr("The selected PLANK video profile has no native negotiation mapping.");
        return false;
    }

    int slicesPerFrame = (m_VideoCallbacks.capabilities >> 24) & 0xFF;
    if (slicesPerFrame == 0) {
        slicesPerFrame = 1;
    }
    const bool supportsReferenceFrameInvalidation = codec == 0 ?
                (m_VideoCallbacks.capabilities &
                 CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC) != 0 :
                (m_VideoCallbacks.capabilities &
                 CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC) != 0;
    const int referenceFrames = supportsReferenceFrameInvalidation ? 0 : 1;
    const int audioChannels = CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(
                m_StreamConfig.audioConfiguration);
    const int audioChannelMask = CHANNEL_MASK_FROM_AUDIO_CONFIGURATION(
                m_StreamConfig.audioConfiguration);
    const bool highQualityAudio =
            m_StreamConfig.bitrate >= 15000 &&
            (m_AudioCallbacks.capabilities & CAPABILITY_SLOW_OPUS_DECODER) == 0 &&
            (audioChannels <= 2 ||
             !NvComputer::isVpnReachability(
                 m_Computer->getActiveAddressReachability()));
    const int packetDurationMs =
            (m_AudioCallbacks.capabilities & CAPABILITY_SLOW_OPUS_DECODER) != 0 ?
                10 : 5;

    const QJsonObject request {
        {QStringLiteral("video"), QJsonObject {
            {QStringLiteral("width"), m_StreamConfig.width},
            {QStringLiteral("height"), m_StreamConfig.height},
            {QStringLiteral("fps"), m_StreamConfig.fps},
            {QStringLiteral("fps_x100"), m_StreamConfig.clientRefreshRateX100},
            {QStringLiteral("slices_per_frame"), slicesPerFrame},
            {QStringLiteral("reference_frames"), referenceFrames},
            {QStringLiteral("encoder_csc_mode"),
             (m_StreamConfig.colorSpace << 1) | m_StreamConfig.colorRange},
            {QStringLiteral("codec"), codec},
            {QStringLiteral("ten_bit"), tenBit},
            {QStringLiteral("chroma"), chroma},
            {QStringLiteral("intra_refresh"), 0},
            {QStringLiteral("encoder_target_kbps"), m_StreamConfig.bitrate},
            {QStringLiteral("negotiated_format"), negotiatedVideoFormat},
        }},
        {QStringLiteral("audio"), QJsonObject {
            {QStringLiteral("channels"), audioChannels},
            {QStringLiteral("channel_mask"), audioChannelMask},
            {QStringLiteral("packet_duration_ms"), packetDurationMs},
            {QStringLiteral("high_quality"), highQualityAudio},
        }},
    };
    const QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    std::vector<unsigned char> packet(
                PLANK_TRANSPORT_SETUP_HEADER_SIZE + static_cast<size_t>(payload.size()));
    size_t packetSize = 0;
    constexpr uint32_t RequestId = 1;
    if (plank_transport_setup_encode(
                PLANK_TRANSPORT_SETUP_LAUNCH_REQUEST, 0,
                PLANK_TRANSPORT_SETUP_STATUS_OK, RequestId,
                reinterpret_cast<const uint8_t*>(payload.constData()),
                static_cast<size_t>(payload.size()),
                packet.data(), packet.size(), &packetSize) != 0 ||
            plank_transport_native_data_send(
                m_PlankTransportEndpoint, packet.data(), packetSize) !=
                PLANK_TRANSPORT_OK) {
        errorMessage = tr("The native PLANK launch request could not be sent.");
        return false;
    }

    packet.resize(PLANK_TRANSPORT_SETUP_MAX_PACKET_SIZE);
    packetSize = 0;
    if (plank_transport_native_data_receive(
                m_PlankTransportEndpoint, packet.data(), packet.size(),
                &packetSize, 12000) != PLANK_TRANSPORT_OK) {
        errorMessage = tr("The host did not complete native session negotiation.");
        return false;
    }

    PlankTransportSetupPacket responsePacket {};
    if (plank_transport_setup_decode(packet.data(), packetSize, &responsePacket) != 0 ||
            responsePacket.request_id != RequestId ||
            (responsePacket.flags & PLANK_TRANSPORT_SETUP_FLAG_RESPONSE) == 0 ||
            (responsePacket.type != PLANK_TRANSPORT_SETUP_LAUNCH_RESPONSE &&
             responsePacket.type != PLANK_TRANSPORT_SETUP_ERROR)) {
        errorMessage = tr("The host returned an invalid native session response.");
        return false;
    }

    QJsonParseError parseError {};
    const QJsonDocument responseDocument = QJsonDocument::fromJson(
                QByteArray(reinterpret_cast<const char*>(responsePacket.payload),
                           static_cast<int>(responsePacket.payload_size)),
                &parseError);
    if (parseError.error != QJsonParseError::NoError ||
            !responseDocument.isObject()) {
        errorMessage = tr("The host returned malformed native session values.");
        return false;
    }
    const QJsonObject response = responseDocument.object();
    if (responsePacket.status != PLANK_TRANSPORT_SETUP_STATUS_OK ||
            responsePacket.type == PLANK_TRANSPORT_SETUP_ERROR) {
        const QString hostMessage = response.value(QStringLiteral("message")).toString();
        errorMessage = hostMessage.isEmpty() ?
                    tr("The host rejected native session negotiation.") : hostMessage;
        return false;
    }

    const QJsonObject audio = response.value(QStringLiteral("audio")).toObject();
    const QJsonArray mapping = audio.value(QStringLiteral("mapping")).toArray();
    const int responseVideoFormat = response.value(
                QStringLiteral("video_format")).toInt();
    const int sampleRate = audio.value(QStringLiteral("sample_rate")).toInt();
    const int responseChannels = audio.value(QStringLiteral("channels")).toInt();
    const int streams = audio.value(QStringLiteral("streams")).toInt();
    const int coupledStreams = audio.value(QStringLiteral("coupled_streams")).toInt();
    const int responsePacketDuration = audio.value(
                QStringLiteral("packet_duration_ms")).toInt();
    const int hostFeatureFlags = response.value(
                QStringLiteral("host_feature_flags")).toInt();
    const int referenceFrameInvalidation = response.value(
                QStringLiteral("reference_frame_invalidation")).toInt(-1);
    if (responseVideoFormat != negotiatedVideoFormat || sampleRate != 48000 ||
            responseChannels != audioChannels || responseChannels <= 0 ||
            responseChannels > AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT ||
            streams <= 0 || streams > responseChannels ||
            coupledStreams < 0 || coupledStreams > streams ||
            responsePacketDuration <= 0 || responsePacketDuration > 120 ||
            (hostFeatureFlags & LI_FF_LOCAL_CURSOR) == 0 ||
            (referenceFrameInvalidation != 0 && referenceFrameInvalidation != 1) ||
            mapping.size() != responseChannels) {
        errorMessage = tr("The host returned unsupported native audio or video values.");
        return false;
    }

    PLANK_NATIVE_SESSION_CONFIGURATION nativeConfiguration {};
    nativeConfiguration.structSize = sizeof(nativeConfiguration);
    nativeConfiguration.serviceFlags = PLANK_NATIVE_SERVICE_AUDIO |
            PLANK_NATIVE_SERVICE_INPUT | PLANK_NATIVE_SERVICE_LOCAL_CURSOR;
    nativeConfiguration.negotiatedVideoFormat = responseVideoFormat;
    nativeConfiguration.hostFeatureFlags =
            static_cast<unsigned int>(hostFeatureFlags);
    nativeConfiguration.audioPacketDurationMs = responsePacketDuration;
    nativeConfiguration.referenceFrameInvalidationSupported =
            static_cast<unsigned int>(referenceFrameInvalidation);
    nativeConfiguration.sessionPort = sessionPort;
    nativeConfiguration.opusConfiguration.sampleRate = sampleRate;
    nativeConfiguration.opusConfiguration.channelCount = responseChannels;
    nativeConfiguration.opusConfiguration.streams = streams;
    nativeConfiguration.opusConfiguration.coupledStreams = coupledStreams;
    for (int index = 0; index < responseChannels; ++index) {
        const int channel = mapping.at(index).toInt(-1);
        if (channel < 0 || channel >= responseChannels) {
            errorMessage = tr("The host returned an invalid native audio channel map.");
            return false;
        }
        nativeConfiguration.opusConfiguration.mapping[index] =
                static_cast<unsigned char>(channel);
    }
    if (LiSetPlankNativeSessionConfiguration(&nativeConfiguration) != 0) {
        errorMessage = tr("The native PLANK session values were rejected locally.");
        return false;
    }

    qInfo() << "Native QUIC session negotiated: video format"
            << QString::number(responseVideoFormat, 16)
            << "host features" << QString::number(hostFeatureFlags, 16)
            << "audio channels" << responseChannels
            << "packet duration" << responsePacketDuration << "ms";
    return true;
#endif
}

void Session::stopPlankTransportDataPlane()
{
#ifdef PLANK_TRANSPORT
    if (m_PlankTransportEndpoint != nullptr) {
        unsigned char packet[PLANK_TRANSPORT_CONTROL_MAX_PACKET_SIZE];
        size_t packetSize = 0;
        if (plank_transport_control_encode(
                    PLANK_TRANSPORT_CONTROL_CLIENT_DISCONNECT, nullptr, 0,
                    packet, sizeof(packet), &packetSize) == 0) {
            plank_transport_native_data_send(m_PlankTransportEndpoint, packet, packetSize);
        }
    }
    stopPlankTransportMediaReceivers();
    LiSetPlankNativeControlSender(nullptr, nullptr);
    LiSetPlankNativeInputSender(nullptr, nullptr);
    if (m_PlankTransportEndpoint != nullptr) {
        PlankTransportNativeStats stats {};
        stats.struct_size = sizeof(stats);
        if (plank_transport_native_endpoint_stats(m_PlankTransportEndpoint, &stats) ==
                PLANK_TRANSPORT_OK) {
            qInfo() << "PlankTransport native transport: video-frames="
                    << stats.video_frames_received
                    << "video-bytes=" << stats.video_bytes_received
                    << "video-receive-drops=" << stats.video_receive_drops
                    << "audio-packets=" << stats.audio_packets_received
                    << "audio-bytes=" << stats.audio_bytes_received
                    << "audio-receive-drops=" << stats.audio_receive_drops
                    << "input-sent=" << stats.input_packets_sent
                    << "data-sent=" << stats.data_packets_sent
                    << "data-received=" << stats.data_packets_received
                    << "QUIC-lost=" << stats.quic_packets_lost
                    << "QUIC-RTT-us=" << stats.quic_rtt_us
                    << "KyProto-drops=" << stats.kyproto_packets_dropped
                    << "video-FEC-source-symbols="
                    << stats.video_fec_source_symbols
                    << "video-FEC-source-symbols-missing="
                    << stats.video_fec_source_symbols_missing
                    << "video-FEC-source-symbols-unrecovered="
                    << stats.video_fec_source_symbols_unrecovered;
        }
        QByteArray lastError(512, '\0');
        plank_transport_native_endpoint_last_error(
                    m_PlankTransportEndpoint, lastError.data(),
                    static_cast<size_t>(lastError.size()));
        if (!lastError.isEmpty() && lastError.constData()[0] != '\0') {
            qWarning() << "PlankTransport native endpoint ended:"
                       << lastError.constData();
        }
        plank_transport_native_endpoint_stop(m_PlankTransportEndpoint);
        plank_transport_native_endpoint_destroy(m_PlankTransportEndpoint);
        m_PlankTransportEndpoint = nullptr;
        qInfo() << "Experimental native KyProto connection stopped";
    }
#endif
}

#ifdef PLANK_TRANSPORT
void Session::startPlankTransportMediaReceivers()
{
    stopPlankTransportMediaReceivers();
    if (m_PlankTransportEndpoint == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_VideoPacketLossSamplesLock);
        m_VideoPacketLossPeakWindow.reset();
        m_VideoPacketLossAfterFecPeakWindow.reset();
        m_CurrentVideoFecLoss = {};
    }
    m_CurrentNetworkRttMs.store(0, std::memory_order_relaxed);
    m_LastPlankVideoReceived.store(0);
    m_PlankTransportReceiversStopping.store(false);
#ifdef Q_OS_MACOS
    startClipboardSync();
#endif
    m_PlankTransportVideoThread = std::thread([this]() {
        plankTransportVideoReceiveLoop();
    });
    if (LiGetPlankNativeServiceFlags() & PLANK_NATIVE_SERVICE_AUDIO) {
        m_PlankTransportAudioThread = std::thread([this]() {
            plankTransportAudioReceiveLoop();
        });
    }
    m_PlankTransportDataThread = std::thread([this]() {
        plankTransportDataReceiveLoop();
    });
}

void Session::stopPlankTransportMediaReceivers()
{
#ifdef Q_OS_MACOS
    stopClipboardPollTimer();
#endif
    m_PlankTransportReceiversStopping.store(true);
    if (m_PlankTransportVideoThread.joinable()) {
        m_PlankTransportVideoThread.join();
    }
    if (m_PlankTransportAudioThread.joinable()) {
        m_PlankTransportAudioThread.join();
    }
    if (m_PlankTransportDataThread.joinable()) {
        m_PlankTransportDataThread.join();
    }
#ifdef Q_OS_MACOS
    stopClipboardSync();
#endif
}

void Session::plankTransportVideoReceiveLoop()
{
    ClientFrameFlowTrace frameFlow("receive");
    constexpr size_t InitialFrameCapacity = 1024 * 1024;
    constexpr size_t MaximumFrameCapacity = 64 * 1024 * 1024;
    std::vector<unsigned char> frame(InitialFrameCapacity);
    VideoPacketLossInterval packetLossInterval;
    Uint64 nextPacketLossSample = SDL_GetTicks();

    const auto sampleTransportTelemetry = [this, &packetLossInterval,
                                           &nextPacketLossSample]() {
        const Uint64 now = SDL_GetTicks();
        if (now < nextPacketLossSample) {
            return;
        }
        nextPacketLossSample = now + 1000;

        PlankTransportNativeStats stats {};
        stats.struct_size = sizeof(stats);
        if (plank_transport_native_endpoint_stats(m_PlankTransportEndpoint, &stats) !=
                PLANK_TRANSPORT_OK) {
            return;
        }

        const std::uint64_t roundedRttMs =
                (stats.quic_rtt_us + 500) / 1000;
        m_CurrentNetworkRttMs.store(
                    static_cast<std::uint32_t>(std::min<std::uint64_t>(
                        roundedRttMs,
                        std::numeric_limits<std::uint32_t>::max())),
                    std::memory_order_relaxed);

        const auto packetLossPercent = packetLossInterval.addCumulative(
                    stats.video_fec_source_symbols,
                    stats.video_fec_source_symbols_missing,
                    stats.video_fec_source_symbols_unrecovered);
        if (packetLossPercent.has_value()) {
            updateVideoFecLoss(*packetLossPercent);
        }
    };

    while (!m_PlankTransportReceiversStopping.load()) {
        sampleTransportTelemetry();
        PlankTransportNativeVideoFrameInfo info {};
        info.struct_size = sizeof(info);
        size_t frameSize = 0;
        const int result = plank_transport_native_video_receive(
                    m_PlankTransportEndpoint, &info, frame.data(), frame.size(),
                    &frameSize, 50);
        if (result == PLANK_TRANSPORT_TIMEOUT) {
            continue;
        }
        if (result == PLANK_TRANSPORT_ERROR_BUFFER_TOO_SMALL &&
                frameSize > frame.size() &&
                frameSize <= MaximumFrameCapacity) {
            frame.resize(frameSize);
            continue;
        }
        if (result != PLANK_TRANSPORT_OK) {
            if (!m_PlankTransportReceiversStopping.load()) {
                qWarning() << "Native KyProto video receive failed:" << result;
            }
            return;
        }
        if (frameSize > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                info.frame_number > std::numeric_limits<uint32_t>::max()) {
            qWarning() << "Native KyProto video frame metadata is out of range";
            LiRequestIdrFrame();
            continue;
        }

        m_LastPlankVideoReceived.store(SDL_GetTicks());
        m_ReachedUserDesktop.store(true);
        frameFlow.record(ClientFrameFlowTrace::Receive,
                         (info.pts / 90000) * 1000000 + (info.pts % 90000) * 1000000 / 90000,
                         info.frame_number,
                         (info.flags & PLANK_TRANSPORT_NATIVE_VIDEO_FLAG_KEY) != 0,
                         frameSize);
        const uint32_t flags =
                (info.flags & PLANK_TRANSPORT_NATIVE_VIDEO_FLAG_KEY) != 0 ?
                    PLANK_VIDEO_FRAME_FLAG_KEY : 0;
        const int submitResult = LiSubmitPlankVideoFrame(
                    frame.data(), static_cast<int>(frameSize),
                    static_cast<uint32_t>(info.frame_number), flags,
                    info.pts, info.host_processing_latency);
        if (submitResult < 0) {
            qWarning() << "Native KyProto video frame submission failed:"
                       << submitResult << "frame" << info.frame_number;
            LiRequestIdrFrame();
        }
    }
}

void Session::plankTransportAudioReceiveLoop()
{
    constexpr size_t MaximumAudioPacketSize = 64 * 1024;
    std::vector<unsigned char> packet(MaximumAudioPacketSize);

    while (!m_PlankTransportReceiversStopping.load()) {
        PlankTransportNativeAudioPacketInfo info {};
        info.struct_size = sizeof(info);
        size_t packetSize = 0;
        const int result = plank_transport_native_audio_receive(
                    m_PlankTransportEndpoint, &info, packet.data(), packet.size(),
                    &packetSize, 50);
        if (result == PLANK_TRANSPORT_TIMEOUT) {
            continue;
        }
        if (result != PLANK_TRANSPORT_OK ||
                packetSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
            if (!m_PlankTransportReceiversStopping.load()) {
                qWarning() << "Native KyProto audio receive failed:" << result;
            }
            return;
        }

        const int submitResult = LiSubmitPlankAudioPacket(
                    packetSize == 0 ? nullptr : packet.data(),
                    static_cast<int>(packetSize), info.frame_samples,
                    info.missing_samples);
        if (submitResult < 0) {
            qWarning() << "Native KyProto audio packet submission failed:"
                       << submitResult;
        }
    }
}

void Session::plankTransportDataReceiveLoop()
{
    std::vector<unsigned char> packet(PLANK_TRANSPORT_EVENT_MAX_PACKET_SIZE);
    while (!m_PlankTransportReceiversStopping.load()) {
        size_t packetSize = 0;
        const int result = plank_transport_native_data_receive(
                    m_PlankTransportEndpoint, packet.data(), packet.size(),
                    &packetSize, 50);
        if (result == PLANK_TRANSPORT_TIMEOUT) {
            continue;
        }
        if (result != PLANK_TRANSPORT_OK) {
            if (!m_PlankTransportReceiversStopping.load()) {
                qWarning() << "Native KyProto control receive failed:" << result;
                // This receiver owns termination for the shared QUIC connection.
                // Drain queued control records first (notably session takeover),
                // then use the existing duplicate-safe reconnect callback. A
                // closed receive path must not wait for a mouse/key send failure.
                clConnectionTerminated(result);
            }
            return;
        }

        if (packetSize < sizeof(uint32_t)) {
            qWarning() << "Rejected undersized native KyProto data record";
            LiNotifyPlankHostTermination(-1);
            return;
        }
        const uint32_t magic = plank_transport_event_read_u32(packet.data());
        if (magic == PLANK_TRANSPORT_CONTROL_MAGIC) {
            PlankTransportControlPacket control {};
            if (plank_transport_control_decode(
                        packet.data(), packetSize, &control) != 0) {
                qWarning() << "Rejected malformed native KyProto control packet";
                LiNotifyPlankHostTermination(-1);
                return;
            }
            switch (control.type) {
            case PLANK_TRANSPORT_CONTROL_HOST_DESKTOP_HANDOFF:
                if (control.payload_size != 0 ||
                        !(m_Computer->plankFeatureFlags & NvOutputTopology::DesktopHandoffNoticeFeature)) {
                    LiNotifyPlankHostTermination(-1);
                    return;
                }
                // Advisory only. Do not reconnect until the transport actually
                // closes; a stale notice must not relabel an unrelated outage.
                m_DesktopHandoffNoticeDeadline.store(SDL_GetTicks() + 5000);
                qInfo() << "Host announced GDM-to-desktop handoff";
                break;
            case PLANK_TRANSPORT_CONTROL_VIDEO_BITRATE_APPLIED:
                if (control.payload_size != 3 * sizeof(uint32_t)) {
                    LiNotifyPlankHostTermination(-1);
                    return;
                }
                LiNotifyPlankVideoBitrateApplied(
                            plank_transport_control_read_u32(control.payload),
                            plank_transport_control_read_u32(control.payload + 4),
                            plank_transport_control_read_u32(control.payload + 8));
                break;
            case PLANK_TRANSPORT_CONTROL_HOST_TERMINATE:
                if (control.payload_size != sizeof(uint32_t)) {
                    LiNotifyPlankHostTermination(-1);
                    return;
                }
                LiNotifyPlankHostTermination(
                            plank_transport_control_read_u32(control.payload));
                return;
            default:
                qWarning() << "Rejected unexpected native KyProto control type"
                           << control.type;
                LiNotifyPlankHostTermination(-1);
                return;
            }
            continue;
        }

        PlankTransportEventPacket event {};
        if (magic != PLANK_TRANSPORT_EVENT_MAGIC ||
                plank_transport_event_decode(
                    packet.data(), packetSize, &event) != 0) {
            qWarning() << "Rejected malformed native KyProto event record";
            LiNotifyPlankHostTermination(-1);
            return;
        }
        switch (event.type) {
        case PLANK_TRANSPORT_EVENT_HDR_MODE: {
            if (event.payload_size != PLANK_TRANSPORT_EVENT_HDR_MODE_SIZE ||
                    event.payload[1] != 0) {
                LiNotifyPlankHostTermination(-1);
                return;
            }
            SS_HDR_METADATA metadata {};
            size_t offset = 2;
            for (int index = 0; index < 3; ++index) {
                metadata.displayPrimaries[index].x =
                        plank_transport_event_read_u16(event.payload + offset);
                metadata.displayPrimaries[index].y =
                        plank_transport_event_read_u16(event.payload + offset + 2);
                offset += 4;
            }
            metadata.whitePoint.x = plank_transport_event_read_u16(event.payload + offset);
            metadata.whitePoint.y = plank_transport_event_read_u16(event.payload + offset + 2);
            offset += 4;
            metadata.maxDisplayLuminance = plank_transport_event_read_u16(event.payload + offset);
            metadata.minDisplayLuminance = plank_transport_event_read_u16(event.payload + offset + 2);
            metadata.maxContentLightLevel = plank_transport_event_read_u16(event.payload + offset + 4);
            metadata.maxFrameAverageLightLevel = plank_transport_event_read_u16(event.payload + offset + 6);
            metadata.maxFullFrameLuminance = plank_transport_event_read_u16(event.payload + offset + 8);
            LiNotifyPlankHdrMode(event.payload[0] != 0, &metadata);
            break;
        }
        case PLANK_TRANSPORT_EVENT_RAW_HID_WACOM:
            if (event.payload_size < sizeof(PLANK_RAW_HID_WIRE_HEADER) ||
                    event.payload_size > sizeof(PLANK_RAW_HID_WIRE_HEADER) +
                        PLANK_RAW_HID_MAX_PAYLOAD_SIZE) {
                LiNotifyPlankHostTermination(-1);
                return;
            }
            LiNotifyPlankRawHidControl(
                        event.payload, event.payload_size);
            break;
        case PLANK_TRANSPORT_EVENT_CURSOR_SHAPE:
            if (event.payload_size < sizeof(PLANK_CURSOR_WIRE_HEADER) ||
                    event.payload_size > sizeof(PLANK_CURSOR_WIRE_HEADER) +
                        PLANK_CURSOR_MAX_CHUNK_SIZE) {
                LiNotifyPlankHostTermination(-1);
                return;
            }
            LiNotifyPlankCursorChunk(
                        event.payload, event.payload_size);
            break;
        case PLANK_TRANSPORT_EVENT_CURSOR_POSITION:
            if (event.payload_size != sizeof(PLANK_CURSOR_POSITION_WIRE_MESSAGE)) {
                LiNotifyPlankHostTermination(-1);
                return;
            }
            LiNotifyPlankCursorPosition(
                        event.payload, event.payload_size);
            break;
        case PLANK_TRANSPORT_EVENT_CLIPBOARD_OFFER:
            if (event.payload_size < sizeof(PLANK_CLIPBOARD_WIRE_HEADER) ||
                    event.payload_size > sizeof(PLANK_CLIPBOARD_WIRE_HEADER) +
                        PLANK_CLIPBOARD_MAX_EVENT_CHUNK_SIZE) {
                LiNotifyPlankHostTermination(-1);
                return;
            }
#ifdef Q_OS_MACOS
            if (m_ClipboardSync && clipboardSyncEnabled() &&
                    !m_ClipboardSync->handleHostOffer(event.payload, event.payload_size)) {
                LiNotifyPlankHostTermination(-1);
                return;
            }
#endif
            break;
        default:
            qWarning() << "Rejected unexpected native KyProto event type"
                       << event.type;
            LiNotifyPlankHostTermination(-1);
            return;
        }
    }
}

int Session::plankTransportNativeControlSender(void* context, uint32_t type,
                                          uint32_t value1, uint32_t value2)
{
    if (context == nullptr) {
        return PLANK_TRANSPORT_ERROR_INVALID_ARGUMENT;
    }
    uint16_t wireType = 0;
    uint32_t values[2] {value1, value2};
    size_t valueCount = 0;
    switch (type) {
    case LI_SC_NATIVE_CONTROL_REQUEST_IDR:
        wireType = PLANK_TRANSPORT_CONTROL_REQUEST_IDR;
        break;
    case LI_SC_NATIVE_CONTROL_INVALIDATE_REFERENCE_FRAMES:
        wireType = PLANK_TRANSPORT_CONTROL_INVALIDATE_REFERENCE_FRAMES;
        valueCount = 2;
        break;
    case LI_SC_NATIVE_CONTROL_SET_VIDEO_BITRATE:
        wireType = PLANK_TRANSPORT_CONTROL_SET_VIDEO_BITRATE;
        valueCount = 1;
        break;
    default:
        return PLANK_TRANSPORT_ERROR_INVALID_ARGUMENT;
    }

    unsigned char packet[PLANK_TRANSPORT_CONTROL_MAX_PACKET_SIZE];
    size_t packetSize = 0;
    if (plank_transport_control_encode(
                wireType, values, valueCount, packet, sizeof(packet),
                &packetSize) != 0) {
        return PLANK_TRANSPORT_ERROR_INVALID_ARGUMENT;
    }
    return plank_transport_native_data_send(
                static_cast<PlankTransportNativeEndpoint*>(context), packet,
                packetSize);
}

int Session::plankTransportNativeInputSender(void* context, uint8_t type,
                                        const unsigned char* payload,
                                        size_t payloadLength)
{
    if (context == nullptr || payload == nullptr || payloadLength == 0) {
        return PLANK_TRANSPORT_ERROR_INVALID_ARGUMENT;
    }
    return plank_transport_native_input_send(
                static_cast<PlankTransportNativeEndpoint*>(context), type,
                payload, payloadLength);
}
#endif

#ifdef Q_OS_MACOS
bool Session::clipboardSyncEnabled() const
{
    return m_Computer != nullptr &&
            (!(m_Computer->plankFeatureFlags & NvOutputTopology::FixedCaptureFeature) || m_MacClipboardNegotiated) &&
            (m_Computer->plankFeatureFlags & NvOutputTopology::ClipboardSyncFeature) != 0;
}

void Session::startClipboardSync()
{
#ifdef PLANK_TRANSPORT
    if (!clipboardSyncEnabled() || m_PlankTransportEndpoint == nullptr) {
        return;
    }
    if (!m_ClipboardSync) {
        m_ClipboardSync = std::make_unique<MacClipboardSync>(
                    [this](const std::uint8_t* payload, std::size_t size) {
                        return plankTransportNativeInputSender(
                                   m_PlankTransportEndpoint,
                                   PLANK_TRANSPORT_INPUT_CLIPBOARD_OFFER,
                                   payload,
                                   size) == PLANK_TRANSPORT_OK;
                    },
                    [this] { return anyPresentationWindowFocused(); },
                    [this] { return clipboardSyncEnabled(); },
                    [] {
                        SDL_Event event {};
                        event.type = SDL_EVENT_USER;
                        event.user.code = SDL_CODE_PLANK_CLIPBOARD;
                        event.user.timestamp = SDL_GetTicks();
                        if (!SDL_PushEvent(&event)) {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                         "Unable to queue host clipboard offer: %s",
                                         SDL_GetError());
                            return false;
                        }
                        return true;
                    });
    }
    m_ClipboardSync->start();
    qInfo() << "Started PLANK clipboard sync";
#endif
}

void Session::stopClipboardSync()
{
    stopClipboardPollTimer();
    if (m_ClipboardSync) {
        m_ClipboardSync->stop();
    }
}

void Session::queueClipboardPollEvent()
{
    ClipboardPollTimer::queue(SDL_CODE_PLANK_CLIPBOARD_POLL);
}

void Session::startClipboardPollTimer()
{
    if (m_ClipboardSync != nullptr &&
            !m_ClipboardPollTimer.start(SDL_CODE_PLANK_CLIPBOARD_POLL)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to start clipboard poll timer: %s",
                    SDL_GetError());
    }
}

void Session::stopClipboardPollTimer()
{
    m_ClipboardPollTimer.stop();
}

#endif

void Session::clearPlankReconnectCredentials()
{
    m_CanReconnect.store(false);
    m_PlankPassword.fill(QChar('\0'));
    m_PlankPassword.clear();
    m_PlankUsername.clear();
}

bool Session::initialize()
{
#ifdef Q_OS_DARWIN
    // SDL 3.4.2 caches allow_spaces in Cocoa_VideoInit, so use CoreGraphics
    // before SDL video initialization. Setting the hint before window creation
    // alone is too late. Match Client and the presenter share this policy.
    const int macDisplayCount = MacWindow::activeDisplayCount();
    if (macDisplayCount <= 0) {
        emit displayLaunchError(tr("Unable to discover active Mac displays."));
        return false;
    }
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES,
                MacDisplayGeometry::useNativeFullscreen(macDisplayCount) ? "1" : "0");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK Mac fullscreen policy: native Spaces, active displays=%d",
                macDisplayCount);
#endif
    if (!StreamingPreferences::isPlankProfileValidForCaptureSource(
                m_PlankVideoProfile,
                m_PlankCaptureSource)) {
        const QString error = tr("The selected capture source and encoding profile are not compatible.");
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", qPrintable(error));
        emit displayLaunchError(error);
        return false;
    }
    if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT &&
            m_PlankVideoProfile ==
                StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444 &&
            (m_Computer->plankFeatureFlags &
             NvOutputTopology::NvfbcHevc10NvencFeature) == 0) {
        const QString error = tr("The host does not support NvFBC with the HEVC 10-bit NVENC profile.");
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", qPrintable(error));
        emit displayLaunchError(error);
        return false;
    }

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s",
                     SDL_GetError());
        return false;
    }

    if (!snapshotClientDisplays()) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }

#ifdef Q_OS_DARWIN
    if (m_ClientDisplays.size() != macDisplayCount) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        emit displayLaunchError(tr("The Mac display layout changed during setup. Please connect again."));
        return false;
    }
#endif

    LiInitializeStreamConfiguration(&m_StreamConfig);
    if (!configurePlankLaunchGeometry()) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }

    int x, y, width, height;
    getWindowDimensions(x, y, width, height);

    // Create a hidden window to use for decoder initialization tests
    SDL_Window* testWindow = SDL_CreateWindow("", width, height,
                                              SDL_WINDOW_HIDDEN | StreamUtils::getPlatformWindowFlags());
    if (!testWindow) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to create test window with platform flags: %s",
                    SDL_GetError());

        testWindow = SDL_CreateWindow("", width, height, SDL_WINDOW_HIDDEN);
        if (!testWindow) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to create window for hardware decode test: %s",
                         SDL_GetError());
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }
    }

    qInfo() << "PLANK host version:"
            << m_Computer->plankHostVersion;

    LiInitializeVideoCallbacks(&m_VideoCallbacks);
    m_VideoCallbacks.setup = drSetup;

    m_StreamConfig.fps = m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT ?
                60 : m_Preferences->fps;
    m_StreamConfig.bitrate = m_PlankBitrateKbps;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Video bitrate: %d kbps",
                m_StreamConfig.bitrate);

    switch (m_Preferences->audioConfig)
    {
    case StreamingPreferences::AC_STEREO:
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
        break;
    case StreamingPreferences::AC_51_SURROUND:
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_51_SURROUND;
        break;
    case StreamingPreferences::AC_71_SURROUND:
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_71_SURROUND;
        break;
    }
    if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) {
        // The authenticated Mac contract is system audio, stereo 48 kHz.
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    }

    LiInitializeAudioCallbacks(&m_AudioCallbacks);
    m_AudioCallbacks.init = arInit;
    m_AudioCallbacks.cleanup = arCleanup;
    m_AudioCallbacks.decodeAndPlaySample = arDecodeAndPlaySample;
    m_AudioCallbacks.capabilities = getAudioRendererCapabilities(m_StreamConfig.audioConfiguration);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Audio channel count: %d",
                CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(m_StreamConfig.audioConfiguration));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Audio channel mask: %X",
                CHANNEL_MASK_FROM_AUDIO_CONFIGURATION(m_StreamConfig.audioConfiguration));

    // PLANK advertises exactly the selected profile. Do not silently
    // substitute another bit depth or chroma format when probing fails.
    int selectedVideoFormat = VIDEO_FORMAT_H264_HIGH10_444;
    switch (m_PlankVideoProfile) {
    case StreamingPreferences::PLANK_PROFILE_H264_8BIT_422:
        selectedVideoFormat = VIDEO_FORMAT_H264_HIGH8_422;
        break;
    case StreamingPreferences::PLANK_PROFILE_H264_8BIT_444:
        selectedVideoFormat = VIDEO_FORMAT_H264_HIGH8_444;
        break;
    case StreamingPreferences::PLANK_PROFILE_H264_10BIT_422:
        selectedVideoFormat = VIDEO_FORMAT_H264_HIGH10_422;
        break;
    case StreamingPreferences::PLANK_PROFILE_H264_10BIT_444:
        break;
    case StreamingPreferences::PLANK_PROFILE_NVENC_H264_8BIT_444:
        selectedVideoFormat = VIDEO_FORMAT_H264_HIGH8_444;
        break;
    case StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444:
        selectedVideoFormat = VIDEO_FORMAT_H265_REXT8_444;
        break;
    case StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444:
        selectedVideoFormat = VIDEO_FORMAT_H265_REXT10_444;
        break;
    case StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_420:
        selectedVideoFormat = VIDEO_FORMAT_H265_MAIN10;
        break;
    case StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_444:
        selectedVideoFormat = VIDEO_FORMAT_H265_REXT10_444;
        break;
    default:
        emit displayLaunchError(tr("The bookmark contains an invalid encoding profile."));
        SDL_DestroyWindow(testWindow);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }
    if (!(selectedVideoFormat & VIDEO_FORMAT_MASK_YUV444) ||
            isIdentityGbrEnabledForFormat(selectedVideoFormat) ||
            StreamingPreferences::isPlankAppleProfile(m_PlankVideoProfile)) {
        m_SupportedVideoFormats.append(selectedVideoFormat);
    }

    SDL_assert(m_SupportedVideoFormats.size() == 1);

    // Check for validation errors/warnings and emit
    // signals for them, if appropriate
    bool ret = validateLaunch(testWindow);

    if (ret) {
        // Video format is now locked in
        m_StreamConfig.supportedVideoFormats = m_SupportedVideoFormats.front();

        // Populate decoder-dependent properties.
        // Must be done after validateLaunch() since m_StreamConfig is finalized.
        ret = populateDecoderProperties(testWindow);
    }

    SDL_DestroyWindow(testWindow);

    if (!ret) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }

    return true;
}

void Session::emitLaunchWarning(QString text)
{
    // Emit the warning to the UI
    emit displayLaunchWarning(text);

    // Wait a little bit so the user can actually read what we just said.
    // This wait is a little longer than the actual toast timeout (3 seconds)
    // to allow it to transition off the screen before continuing.
    const Uint64 start = SDL_GetTicks();
    while (SDL_GetTicks() < start + 3500) {
        SDL_Delay(5);

        if (!m_ThreadedExec) {
            // Pump the UI loop while we wait if we're on the main thread
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            QCoreApplication::sendPostedEvents();
        }
    }
}

bool Session::validateLaunch(SDL_Window* testWindow)
{
    m_SupportedVideoFormats.removeByMask(
                ~m_SupportedVideoFormats.maskByServerCodecModes(m_Computer->serverCodecModeSupport));
    if (m_SupportedVideoFormats.isEmpty()) {
        emit displayLaunchError(tr("The selected PLANK encoding profile is not supported by both this host and client."));
        return false;
    }

    // Internal exact-profile decoder selection may legitimately choose
    // software when no hardware path reproduces the selected profile. That is
    // an expected capability result, so do not interrupt each connection with
    // a warning.
    while (!m_SupportedVideoFormats.isEmpty()) {
        const auto availability = getDecoderAvailability(
                    testWindow,
                    m_SupportedVideoFormats.front(),
                    m_StreamConfig.width,
                    m_StreamConfig.height,
                    m_StreamConfig.fps,
                    isIdentityGbrEnabledForFormat(m_SupportedVideoFormats.front()));
        if (availability == DecoderAvailability::None) {
            m_SupportedVideoFormats.removeFirst();
        }
        else {
            break;
        }
    }
    if (m_SupportedVideoFormats.isEmpty()) {
        emit displayLaunchError(tr("This client cannot decode the selected PLANK encoding profile."));
        return false;
    }

    // Test if audio works at the specified audio configuration
    bool audioTestPassed = testAudio(m_StreamConfig.audioConfiguration);

    // Gracefully degrade to stereo if surround sound doesn't work
    if (!audioTestPassed && CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(m_StreamConfig.audioConfiguration) > 2) {
        audioTestPassed = testAudio(AUDIO_CONFIGURATION_STEREO);
        if (audioTestPassed) {
            m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
            emitLaunchWarning(tr("Your selected surround sound setting is not supported by the current audio device."));
        }
    }

    // If nothing worked, warn the user that audio will not work
    if (!audioTestPassed) {
        emitLaunchWarning(tr("Failed to open audio device. Audio will be unavailable during this session."));
    }

    return true;
}


class DeferredSessionCleanupTask : public QRunnable
{
public:
    DeferredSessionCleanupTask(Session* session) :
        m_Session(session) {}

private:
    virtual ~DeferredSessionCleanupTask() override
    {
        // Allow another session to start now that we're cleaned up
        Session::s_ActiveSession = nullptr;
        Session::s_ActiveSessionSemaphore.release();

        // Notify that the session is ready to be cleaned up
        emit m_Session->readyForDeletion();
    }

    void run() override
    {
#ifdef Q_OS_MACOS
        if (!m_Session->m_ApplicationExitRequested.load())
#endif
        emit m_Session->sessionFinished();

        // The video decoder must already be destroyed, since it could
        // try to interact with APIs that can only be called between
        // LiStartConnection() and LiStopConnection().
        SDL_assert(m_Session->m_VideoDecoder == nullptr);

        // Finish cleanup of the connection state
#ifdef PLANK_TRANSPORT
        m_Session->stopPlankTransportMediaReceivers();
#endif
        LiStopConnection();
        m_Session->stopPlankTransportDataPlane();

    }

    Session* m_Session;
};

int Session::getTargetDisplayIndex() const
{
    int displayIndex = 0;

    if (m_Window != nullptr) {
        displayIndex = StreamUtils::getDisplayIndex(SDL_GetDisplayForWindow(m_Window));
        if (displayIndex < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SDL_GetDisplayForWindow() failed: %s",
                        SDL_GetError());
            displayIndex = 0;
        }
    }
    // Create our window on the same display that Qt's UI
    // was being displayed on.
    else {
        if (m_QtWindow != nullptr) {
            QScreen* screen = m_QtWindow->screen();
            if (screen != nullptr) {
                QRect displayRect = screen->geometry();

                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Qt UI screen is at (%d,%d)",
                            displayRect.x(), displayRect.y());
                for (int i = 0; i < StreamUtils::getDisplayCount(); i++) {
                    SDL_Rect displayBounds;

                    if (SDL_GetDisplayBounds(StreamUtils::getDisplayId(i), &displayBounds)) {
                        if (displayBounds.x == displayRect.x() &&
                            displayBounds.y == displayRect.y()) {
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                        "SDL found matching display %d",
                                        i);
                            displayIndex = i;
                            break;
                        }
                    }
                    else {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                    "SDL_GetDisplayBounds(%d) failed: %s",
                                    i, SDL_GetError());
                    }
                }
            }
            else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Qt window is not associated with a QScreen!");
            }
        }
    }

    return displayIndex;
}

bool Session::snapshotClientDisplays()
{
    m_ClientDisplays.clear();
#ifdef Q_OS_DARWIN
    bool matchMacDesktop;
    {
        QReadLocker lock(&m_Computer->lock);
        matchMacDesktop = m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT &&
            m_Computer->plankHostLayout == NvOutputTopology::MatchClientHostLayout;
    }
#endif
    const int targetIndex = getTargetDisplayIndex();
    m_TargetDisplayId = StreamUtils::getDisplayId(targetIndex);
    const int displayCount = StreamUtils::getDisplayCount();
    for (int index = 0; index < displayCount; ++index) {
        ClientDisplaySnapshot snapshot;
        snapshot.displayId = StreamUtils::getDisplayId(index);
        SDL_DisplayMode nativeMode;
        SDL_Rect safeArea;
        if (snapshot.displayId == 0 ||
                !SDL_GetDisplayBounds(snapshot.displayId,
                                      &snapshot.logicalBounds) ||
                !StreamUtils::getNativeDesktopMode(index, &nativeMode,
                                                   &safeArea)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Unable to snapshot client display %d: %s",
                         index, SDL_GetError());
            return false;
        }
        snapshot.nativeSize = QSize(nativeMode.w, nativeMode.h);
#ifdef Q_OS_DARWIN
        if (matchMacDesktop) {
            SDL_DisplayMode currentMode;
            SDL_Rect matchedBounds;
            if (!StreamUtils::getMacCurrentDisplayModeForBounds(snapshot.logicalBounds,
                    &currentMode, &matchedBounds, m_IsFullScreen &&
                    MacDisplayGeometry::useNativeFullscreen(displayCount))) return false;
            snapshot.macMatchedBounds = QRect(matchedBounds.x, matchedBounds.y,
                                             matchedBounds.w, matchedBounds.h);
            snapshot.macBackingSize = QSize(currentMode.w, currentMode.h);
            // Presentation tiles must share the matched backing-pixel canvas,
            // not mix differently scaled panel-native pixel dimensions.
            snapshot.nativeSize = snapshot.macBackingSize;
        }
#endif
        m_ClientDisplays.append(snapshot);
    }

    std::sort(m_ClientDisplays.begin(), m_ClientDisplays.end(),
              [](const auto& left, const auto& right) {
        return std::make_tuple(left.logicalBounds.x, left.logicalBounds.y) <
                std::make_tuple(right.logicalBounds.x,
                                right.logicalBounds.y);
    });
    // Remember multi-output capability even when the session starts windowed.
    // Fullscreen may be entered later without reconnecting; only the active
    // presentation layout, not display discovery, depends on that state.
    m_MultiDisplayPresentationAvailable =
            (strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0 ||
             strcmp(SDL_GetCurrentVideoDriver(), "cocoa") == 0) &&
            m_ClientDisplays.size() == 2;
    if (m_MultiDisplayPresentationAvailable) {
        const auto& left = m_ClientDisplays.at(0).logicalBounds;
        const auto& right = m_ClientDisplays.at(1).logicalBounds;
        const bool horizontal = left.x + left.w <= right.x;
        const bool overlapsVertically = left.y < right.y + right.h &&
                right.y < left.y + left.h;
        if (!horizontal || !overlapsVertically) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Two-output presentation requires client monitors arranged left to right; using the target output only");
            m_MultiDisplayPresentationAvailable = false;
        }
    }

    int canvasX = 0;
    int canvasHeight = 0;
    for (auto& display : m_ClientDisplays) {
        display.canvasRect = QRect(canvasX, 0,
                                   display.nativeSize.width(),
                                   display.nativeSize.height());
        canvasX += display.nativeSize.width();
        canvasHeight = qMax(canvasHeight, display.nativeSize.height());
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK client output %u: logical=%dx%d%+d%+d native=%dx%d canvas=%dx%d%+d%+d%s",
                    display.displayId,
                    display.logicalBounds.w, display.logicalBounds.h,
                    display.logicalBounds.x, display.logicalBounds.y,
                    display.nativeSize.width(), display.nativeSize.height(),
                    display.canvasRect.width(), display.canvasRect.height(),
                    display.canvasRect.x(), display.canvasRect.y(),
                    display.displayId == m_TargetDisplayId ? " primary" : "");
    }
    if (m_ClientDisplays.isEmpty()) {
        return false;
    }
    QSize targetNativeSize;
    for (const auto& display : std::as_const(m_ClientDisplays)) {
        if (display.displayId == m_TargetDisplayId) {
            targetNativeSize = display.nativeSize;
            break;
        }
    }
    if (!targetNativeSize.isValid()) {
        targetNativeSize = m_ClientDisplays.first().nativeSize;
        m_TargetDisplayId = m_ClientDisplays.first().displayId;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK client presentation capability: outputs=%lld canvas=%dx%d mode=%s",
                static_cast<long long>(
                    m_MultiDisplayPresentationAvailable ? m_ClientDisplays.size() : 1),
                m_MultiDisplayPresentationAvailable ? canvasX : targetNativeSize.width(),
                m_MultiDisplayPresentationAvailable ? canvasHeight : targetNativeSize.height(),
                m_MultiDisplayPresentationAvailable ? "multi-output" : "single-output");
    return true;
}

void Session::rebuildPresentationLayout()
{
    m_PresentationLayout = {};
    if (m_Window == nullptr) {
        return;
    }

    const bool multiOutputActive = m_UseMultiDisplayPresentation &&
            m_PresentationFullscreen && !m_SecondaryWindows.isEmpty();
    if (multiOutputActive) {
        int canvasWidth = 0;
        int canvasHeight = 0;
        int secondaryIndex = 0;
        for (const auto& display : std::as_const(m_ClientDisplays)) {
            SDL_Window* window = display.displayId == m_TargetDisplayId ?
                        m_Window : nullptr;
            if (window == nullptr &&
                    secondaryIndex < m_SecondaryWindows.size()) {
                // Secondary windows are created in the same stable display
                // order as this snapshot. Keep intended presentation geometry
                // independent of asynchronous compositor placement state.
                window = m_SecondaryWindows.at(secondaryIndex++);
            }
            if (window == nullptr) {
                continue;
            }
            m_PresentationLayout.outputs.append(
                {window, display.canvasRect, window == m_Window});
            canvasWidth = qMax(canvasWidth, display.canvasRect.right() + 1);
            canvasHeight = qMax(canvasHeight, display.canvasRect.bottom() + 1);
        }
        m_PresentationLayout.canvasSize = QSize(canvasWidth, canvasHeight);
    }
    else {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(m_Window, &width, &height);
        m_PresentationLayout.canvasSize = QSize(qMax(1, width), qMax(1, height));
        m_PresentationLayout.outputs.append(
            {m_Window, QRect(QPoint(0, 0), m_PresentationLayout.canvasSize), true});
    }

    if (m_InputHandler != nullptr) {
        m_InputHandler->setPresentationLayout(m_PresentationLayout);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK render presentation: outputs=%lld canvas=%dx%d fullscreen=%s",
                static_cast<long long>(m_PresentationLayout.outputs.size()),
                m_PresentationLayout.canvasSize.width(),
                m_PresentationLayout.canvasSize.height(),
                m_PresentationFullscreen ? "yes" : "no");
}

bool Session::placeFullscreenWindowOnDisplay(SDL_Window* window,
                                             SDL_DisplayID displayId)
{
    if (!SDL_SyncWindow(window)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Timed out synchronizing new fullscreen surface before output placement: %s",
                    SDL_GetError());
    }

    const int centered = SDL_WINDOWPOS_CENTERED_DISPLAY(displayId);
    if (!SDL_SetWindowPosition(window, centered, centered)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to request fullscreen surface placement on output %u: %s",
                     displayId, SDL_GetError());
        return false;
    }
    if (!SDL_SyncWindow(window)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Timed out synchronizing fullscreen surface placement on output %u: %s",
                    displayId, SDL_GetError());
    }

    SDL_DisplayID actualDisplay = SDL_GetDisplayForWindow(window);
    if (actualDisplay == displayId) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Fullscreen surface assigned to requested client output %u",
                    displayId);
        return true;
    }

    // Some Wayland compositors retain the first fullscreen assignment until
    // the toplevel has completed one windowed configure. Re-seed SDL's target
    // display while windowed, then enter fullscreen again.
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Fullscreen surface landed on output %u instead of %u; retrying through a synchronized windowed configure",
                actualDisplay, displayId);
    if (!SDL_SetWindowFullscreen(window, false) ||
            !SDL_SyncWindow(window) ||
            !SDL_SetWindowPosition(window, centered, centered) ||
            !SDL_SyncWindow(window) ||
            !SDL_SetWindowFullscreen(window, true) ||
            !SDL_SyncWindow(window)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to reassign fullscreen surface to output %u: %s",
                     displayId, SDL_GetError());
        return false;
    }

    actualDisplay = SDL_GetDisplayForWindow(window);
    if (actualDisplay != displayId) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Window system kept fullscreen surface on output %u instead of requested output %u",
                     actualDisplay, displayId);
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Fullscreen surface assigned to requested client output %u after synchronized retry",
                displayId);
    return true;
}

SDL_Window* Session::windowForEvent(Uint32 windowId) const
{
    SDL_Window* window = SDL_GetWindowFromID(windowId);
    if (window == m_Window || m_SecondaryWindows.contains(window)) {
        return window;
    }
    return nullptr;
}

bool Session::anyPresentationWindowFocused() const
{
    if (m_Window != nullptr &&
            (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_INPUT_FOCUS)) {
        return true;
    }
    for (SDL_Window* window : m_SecondaryWindows) {
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) {
            return true;
        }
    }
    return false;
}

void Session::setPresentationWindowsFullscreen(bool fullscreen)
{
    m_PresentationFullscreen = fullscreen;
    if (!SDL_SetWindowFullscreen(m_Window,
                                 fullscreen ? m_FullScreenFlag : 0)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to set presentation fullscreen state: %s",
                    SDL_GetError());
    }
    if ((strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0 ||
         strcmp(SDL_GetCurrentVideoDriver(), "cocoa") == 0) &&
            !SDL_SyncWindow(m_Window)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Timed out synchronizing presentation fullscreen state: %s",
                    SDL_GetError());
    }
    if (!fullscreen && !m_HasWindowedPresentationGeometry) {
        int x, y, width, height;
        getWindowDimensions(x, y, width, height);
        Q_UNUSED(x);
        Q_UNUSED(y);

        // The initial Wayland toplevel uses the complete output dimensions so
        // fullscreen absolute-pointer coordinates are correct from its first
        // configure. SDL also caches that size as the window's initial
        // floating geometry. Replace it on the first fullscreen exit so
        // libdecor can commit a complete, usable decoration frame immediately.
        if (!SDL_SetWindowSize(m_Window, width, height)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to establish initial windowed presentation geometry: %s",
                        SDL_GetError());
        }
        else {
            m_HasWindowedPresentationGeometry = true;
            if (strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0 &&
                    !SDL_SyncWindow(m_Window)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Timed out synchronizing initial windowed presentation geometry: %s",
                            SDL_GetError());
            }
        }
    }
    for (SDL_Window* window : m_SecondaryWindows) {
        if (!PlankPresentation::setSecondaryFullscreen(window, fullscreen)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to transition secondary presentation window: %s",
                        SDL_GetError());
        }
    }
    rebuildPresentationLayout();
}

void Session::minimizePresentationWindows()
{
    SDL_MinimizeWindow(m_Window);
    for (SDL_Window* window : m_SecondaryWindows) {
        SDL_MinimizeWindow(window);
    }
}

bool Session::configurePlankHostLayout()
{
    QString layoutPolicy;
    QString scalingMode;
    QString virtualMode1;
    QString virtualMode2;
    QSize authenticatedDesktopSize;
    QSizeF authenticatedLogicalSize;
    bool hostRejectsRequestedLayout = false;
    {
        QReadLocker lock(&m_Computer->lock);
        layoutPolicy = m_Computer->plankHostLayout;
        scalingMode = m_Computer->plankScalingMode;
        virtualMode1 = m_Computer->plankVirtualMode1;
        virtualMode2 = m_Computer->plankVirtualMode2;
        authenticatedDesktopSize = QSize(m_Computer->outputTopology.desktopWidth,
                                         m_Computer->outputTopology.desktopHeight);
        authenticatedLogicalSize = m_Computer->outputTopology.captureLogicalBounds.size();
        const bool hostPolicyKnown = m_Computer->outputTopology.displayPolicyKnown();
        hostRejectsRequestedLayout = hostPolicyKnown &&
                !m_Computer->outputTopology.allowsBookmarkHostLayout(layoutPolicy);
    }

    if (hostRejectsRequestedLayout) {
        const QString error = tr("This workstation does not support the display layout selected by the bookmark.");
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", qPrintable(error));
        emit displayLaunchError(error);
        return false;
    }

    m_ResolvedHostLayout.clear();
    m_ResolvedVirtualModes.clear();
    if (scalingMode != NvOutputTopology::NativeScalingMode &&
            scalingMode != NvOutputTopology::ScaledSpanMode) {
        const QString error = tr("The bookmark contains an unsupported client scaling mode.");
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", qPrintable(error));
        emit displayLaunchError(error);
        return false;
    }
    m_ResolvedScalingMode = scalingMode;
    if (layoutPolicy == NvOutputTopology::MatchClientHostLayout) {
        QVector<NvClientDisplay> displays;
        for (const auto& display : std::as_const(m_ClientDisplays)) {
            displays.append({display.macMatchedBounds.isValid() ? display.macMatchedBounds : QRect(display.logicalBounds.x,
                                   display.logicalBounds.y,
                                   display.logicalBounds.w,
                                   display.logicalBounds.h),
                             display.nativeSize, display.macBackingSize});
        }

        QString error;
        if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) {
            int scale = 1;
            const QString mode = NvOutputTopology::resolveMacClientDisplayMode(displays, &error, &scale);
            if (mode.isEmpty()) {
                emit displayLaunchError(error);
                return false;
            }
            if (NvOutputTopology::macDisplayModeSize(mode) != authenticatedDesktopSize ||
                    authenticatedLogicalSize !=
                    QSizeF(authenticatedDesktopSize.width() / scale, authenticatedDesktopSize.height() / scale)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "PLANK Mac Match Client requested %s scale=%d; host kept %dx%d",
                            qPrintable(mode), scale,
                            authenticatedDesktopSize.width(), authenticatedDesktopSize.height());
            }
            m_ResolvedHostLayout = QStringLiteral("fixed");
        }
        else if (!NvOutputTopology::resolveClientDisplayLayout(
                    displays, m_ResolvedHostLayout, m_ResolvedVirtualModes, &error)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", qPrintable(error));
            emit displayLaunchError(error);
            return false;
        }
    }
    else if (layoutPolicy == QStringLiteral("fixed") &&
             m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT &&
             m_Computer->outputTopology.featureFlags == NvOutputTopology::FixedCaptureFlags) {
        m_ResolvedHostLayout = layoutPolicy;
    }
    else if (layoutPolicy == NvOutputTopology::PhysicalHostLayout) {
        m_ResolvedHostLayout = NvOutputTopology::PhysicalHostLayout;
    }
    else if (layoutPolicy == NvOutputTopology::SingleHostLayout ||
             layoutPolicy == NvOutputTopology::DualHorizontalHostLayout) {
        if (!NvOutputTopology::qualifiedVirtualModes().contains(virtualMode1) ||
                (layoutPolicy == NvOutputTopology::DualHorizontalHostLayout &&
                 !NvOutputTopology::qualifiedVirtualModes().contains(virtualMode2))) {
            const QString error = tr("The bookmark contains an unsupported virtual monitor resolution.");
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", qPrintable(error));
            emit displayLaunchError(error);
            return false;
        }
        m_ResolvedHostLayout = layoutPolicy;
        m_ResolvedVirtualModes.append(virtualMode1);
        if (layoutPolicy == NvOutputTopology::DualHorizontalHostLayout) {
            m_ResolvedVirtualModes.append(virtualMode2);
        }
    }
    else {
        const QString error = tr("The bookmark contains an unsupported host display layout.");
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", qPrintable(error));
        emit displayLaunchError(error);
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK host layout: policy=%s resolved=%s modes=%s scaling=%s",
                qPrintable(layoutPolicy), qPrintable(m_ResolvedHostLayout),
                qPrintable(m_ResolvedVirtualModes.join(',')),
                qPrintable(m_ResolvedScalingMode));
    return true;
}

QSize Session::configurePlankDisplayMode()
{
    QSize detectedResolution;

    if (m_UseMultiDisplayPresentation) {
        int width = 0;
        int height = 0;
        for (const auto& display : std::as_const(m_ClientDisplays)) {
            width += display.nativeSize.width();
            height = qMax(height, display.nativeSize.height());
        }
        detectedResolution = QSize(width, height);
    }
    else {
        for (const auto& display : std::as_const(m_ClientDisplays)) {
            if (display.displayId == m_TargetDisplayId) {
                detectedResolution = display.nativeSize;
                break;
            }
        }
    }

    QSize nativeCanvasResolution;
    {
        QReadLocker lock(&m_Computer->lock);
        if (m_ResolvedHostLayout == NvOutputTopology::PhysicalHostLayout ||
                m_ResolvedHostLayout == QStringLiteral("fixed")) {
            nativeCanvasResolution = QSize(m_Computer->outputTopology.desktopWidth,
                                           m_Computer->outputTopology.desktopHeight);
        }
    }
    if (m_ResolvedHostLayout != NvOutputTopology::PhysicalHostLayout &&
            m_ResolvedHostLayout != QStringLiteral("fixed")) {
        nativeCanvasResolution = NvOutputTopology::virtualCanvasSize(
                    m_ResolvedHostLayout, m_ResolvedVirtualModes);
    }

    QSize selectedResolution;
    if (m_ResolvedScalingMode == NvOutputTopology::NativeScalingMode ||
            m_ResolvedHostLayout == QStringLiteral("fixed")) {
        if (!nativeCanvasResolution.isValid()) {
            const QString error = tr("Native scaling requires a valid host desktop pixel size.");
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", qPrintable(error));
            emit displayLaunchError(error);
            return QSize();
        }
        selectedResolution = nativeCanvasResolution;
    }
    else {
        selectedResolution = PlankDisplayMode::resolve(
            detectedResolution, nativeCanvasResolution);
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK client physical resolution: detected=%dx%d host-native=%dx%d selected=%dx%d scaling=%s resolution-policy=%s",
                detectedResolution.width(), detectedResolution.height(),
                nativeCanvasResolution.width(), nativeCanvasResolution.height(),
                selectedResolution.width(), selectedResolution.height(),
                qPrintable(m_ResolvedScalingMode),
                m_ResolvedScalingMode == NvOutputTopology::NativeScalingMode ?
                    "host-native" : "client-native");
    return selectedResolution;
}

bool Session::configurePlankLaunchGeometry()
{
    if (m_Computer->plankAuthentication &&
            !configurePlankHostLayout()) {
        return false;
    }

    {
        QReadLocker lock(&m_Computer->lock);
        const int hostOutputs = m_Computer->outputTopology.outputCountForLayout(m_ResolvedHostLayout);
        m_UseMultiDisplayPresentation = m_MultiDisplayPresentationAvailable;
#ifdef Q_OS_DARWIN
        // The Mac single-output policy is qualified independently of Wayland.
        m_UseMultiDisplayPresentation = m_UseMultiDisplayPresentation && hostOutputs > 1;
#endif
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK presentation selection: host-outputs=%d client-multi=%d selected-outputs=%d",
                    hostOutputs, m_MultiDisplayPresentationAvailable,
                    m_UseMultiDisplayPresentation ? 2 : 1);
    }

    const QSize resolution = configurePlankDisplayMode();
    if (!resolution.isValid()) {
        return false;
    }

    const QSize previousResolution(m_StreamConfig.width,
                                   m_StreamConfig.height);
    m_StreamConfig.width = resolution.width();
    m_StreamConfig.height = resolution.height();
    if (m_InputHandler != nullptr) {
        m_InputHandler->setStreamDimensions(resolution.width(),
                                            resolution.height());
    }

    if (previousResolution.isValid() && previousResolution != resolution) {
        qInfo() << "PLANK launch geometry changed from"
                << previousResolution << "to" << resolution;
    }
    return true;
}

void Session::getWindowDimensions(int& x, int& y,
                                  int& width, int& height)
{
    const int displayIndex = getTargetDisplayIndex();

    SDL_Rect usableBounds;
    if (SDL_GetDisplayUsableBounds(StreamUtils::getDisplayId(displayIndex), &usableBounds)) {
        // Don't use more than 80% of the display to leave room for system UI
        // and ensure the target size is not odd (otherwise one of the sides
        // of the image will have a one-pixel black bar next to it).
        SDL_Rect src, dst;
        src.x = src.y = dst.x = dst.y = 0;
        src.w = m_StreamConfig.width;
        src.h = m_StreamConfig.height;
        dst.w = ((int)SDL_ceilf(usableBounds.w * 0.80f) & ~0x1);
        dst.h = ((int)SDL_ceilf(usableBounds.h * 0.80f) & ~0x1);

        // Scale the window size while preserving aspect ratio
        StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

        // If the stream window can fit within the usable drawing area with 1:1
        // scaling, do that rather than filling the screen.
        if (m_StreamConfig.width < dst.w && m_StreamConfig.height < dst.h) {
            width = m_StreamConfig.width;
            height = m_StreamConfig.height;
        }
        else {
            width = dst.w;
            height = dst.h;
        }
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_GetDisplayUsableBounds() failed: %s",
                     SDL_GetError());

        width = m_StreamConfig.width;
        height = m_StreamConfig.height;
    }

    x = y = SDL_WINDOWPOS_CENTERED_DISPLAY(StreamUtils::getDisplayId(displayIndex));
}

void Session::updateOptimalWindowDisplayMode()
{
    bool preserveDesktopMode = strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0;
#ifdef Q_OS_DARWIN
    preserveDesktopMode = true;
#endif
    // A PLANK Wayland session is a desktop surface, not a monitor
    // mode switch. Let the compositor size the fullscreen surface and keep
    // SDL's window and pointer coordinates in the same space. SDL 3.4.2 can
    // otherwise retain the pre-fullscreen viewport for pointer events while
    // exposing the fullscreen mode dimensions through SDL_GetWindowSize().
    // Our renderer already performs the required aspect scaling and
    // letterboxing, so an exclusive Wayland display mode adds no value.
    // Mac fullscreen also preserves the current desktop mode; exclusive-mode
    // selection must not change the user's Retina scale, including manual
    // remote resolutions and connections to Linux hosts.
    if (preserveDesktopMode) {
        if (!SDL_SetWindowFullscreenMode(m_Window, nullptr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to select desktop fullscreen mode: %s",
                        SDL_GetError());
        }
        else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Using compositor-native desktop fullscreen mode");
        }
        return;
    }

    SDL_DisplayMode desktopMode, bestMode, mode;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(m_Window);
    const int displayIndex = StreamUtils::getDisplayIndex(display);

    // Try the current display mode first. On macOS, this will be the normal
    // scaled desktop resolution setting.
    if (const SDL_DisplayMode* queriedDesktopMode = SDL_GetDesktopDisplayMode(display)) {
        desktopMode = *queriedDesktopMode;
        // If this doesn't fit the selected resolution, use the native
        // resolution of the panel (unscaled).
        if (desktopMode.w < m_ActiveVideoWidth || desktopMode.h < m_ActiveVideoHeight) {
            SDL_Rect safeArea;
            if (!StreamUtils::getNativeDesktopMode(displayIndex, &desktopMode, &safeArea)) {
                return;
            }
        }
    }
    else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_GetDesktopDisplayMode() failed: %s",
                    SDL_GetError());
        return;
    }

    // Start with the native desktop resolution and try to find
    // the highest refresh rate that our stream FPS evenly divides.
    bestMode = desktopMode;
    bestMode.refresh_rate = 0;
    for (int i = 0; i < StreamUtils::getDisplayModeCount(displayIndex); i++) {
        if (StreamUtils::getDisplayMode(displayIndex, i, &mode)) {
            if (mode.w == desktopMode.w && mode.h == desktopMode.h &&
                    qRound(mode.refresh_rate) % m_StreamConfig.fps == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Found display mode with desktop resolution: %dx%dx%d",
                            mode.w, mode.h, qRound(mode.refresh_rate));
                if (mode.refresh_rate > bestMode.refresh_rate) {
                    bestMode = mode;
                }
            }
        }
    }

    // If we didn't find a mode that matched the current resolution and
    // had a high enough refresh rate, start looking for lower resolution
    // modes that can meet the required refresh rate and minimum video
    // resolution. We will also try to pick a display mode that matches
    // aspect ratio closest to the video stream.
    if (bestMode.refresh_rate == 0) {
        float bestModeAspectRatio = 0;
        float videoAspectRatio = (float)m_ActiveVideoWidth / (float)m_ActiveVideoHeight;
        for (int i = 0; i < StreamUtils::getDisplayModeCount(displayIndex); i++) {
            if (StreamUtils::getDisplayMode(displayIndex, i, &mode)) {
                float modeAspectRatio = (float)mode.w / (float)mode.h;
                if (mode.w >= m_ActiveVideoWidth && mode.h >= m_ActiveVideoHeight &&
                        qRound(mode.refresh_rate) % m_StreamConfig.fps == 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Found display mode with video resolution: %dx%dx%d",
                                mode.w, mode.h, qRound(mode.refresh_rate));
                    if (mode.refresh_rate >= bestMode.refresh_rate &&
                            (bestModeAspectRatio == 0 || fabs(videoAspectRatio - modeAspectRatio) <= fabs(videoAspectRatio - bestModeAspectRatio))) {
                        bestMode = mode;
                        bestModeAspectRatio = modeAspectRatio;
                    }
                }
            }
        }
    }

    if (bestMode.refresh_rate == 0) {
        // We may find no match if the user has moved a 120 FPS
        // stream onto a 60 Hz monitor (since no refresh rate can
        // divide our FPS setting). We'll stick to the default in
        // this case.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "No matching display mode found; using desktop mode");
        bestMode = desktopMode;
    }

    if (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_FULLSCREEN) {
        // Only print when the window is actually in full-screen exclusive mode,
        // otherwise we're not actually using the mode we've set here
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Chosen best display mode: %dx%dx%d",
                    bestMode.w, bestMode.h, qRound(bestMode.refresh_rate));
    }

    SDL_SetWindowFullscreenMode(m_Window, &bestMode);
}

void Session::toggleFullscreen()
{
    bool fullScreen = !(SDL_GetWindowFlags(m_Window) & m_FullScreenFlag);

    if (m_UseMultiDisplayPresentation) {
        SDL_LockSpinlock(&m_DecoderLock);
        delete m_VideoDecoder;
        m_VideoDecoder = nullptr;
        SDL_UnlockSpinlock(&m_DecoderLock);
    }

#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN)
    // Destroy the video decoder before toggling full-screen because D3D9 can try
    // to put the window back into full-screen before we've managed to destroy
    // the renderer. This leads to excessive flickering and can cause the window
    // decorations to get messed up as SDL and D3D9 fight over the window style.
    //
    // On Apple Silicon Macs, the AVSampleBufferDisplayLayer may cause WindowServer
    // to deadlock when transitioning out of fullscreen. Destroy the decoder before
    // exiting fullscreen as a workaround. See issue #973.
    SDL_LockSpinlock(&m_DecoderLock);
    delete m_VideoDecoder;
    m_VideoDecoder = nullptr;
    SDL_UnlockSpinlock(&m_DecoderLock);
#endif

    // Actually enter/leave fullscreen
    setPresentationWindowsFullscreen(fullScreen);

    // Input handler might need to start/stop keyboard grab after changing modes
    m_InputHandler->updateKeyboardGrabState();

    // Input handler might need stop/stop mouse grab after changing modes
    m_InputHandler->updatePointerRegionLock();

    if (m_UseMultiDisplayPresentation) {
        SDL_Event resetEvent = {};
        resetEvent.type = SDL_EVENT_RENDER_DEVICE_RESET;
        SDL_PushEvent(&resetEvent);
    }
}

class AsyncConnectionStartThread : public QThread
{
public:
    AsyncConnectionStartThread(Session* session) :
        QThread(nullptr),
        m_Session(session)
    {
        setObjectName("Async Conn Start");
    }

    void run() override
    {
        m_Session->m_AsyncConnectionSuccess = m_Session->startConnectionAsync();
    }

    Session* m_Session;
};

// Called in a non-main thread
bool Session::startConnectionAsync(bool reconnecting,
                                   bool takeOverActiveSession)
{
    // Wait 1.5 seconds before connecting to let the user
    // have time to read any messages present on the segue
    if (!reconnecting && !takeOverActiveSession) {
        SDL_Delay(1500);
    }

    // PLANK never terminates a host application remotely. Only resume
    // the already-running Desktop application or launch it from an idle host.
    Q_ASSERT(m_Computer->currentGameId == 0 ||
             m_Computer->currentGameId == m_App.id);

    quint16 plankTransportPort = 0;
    QString plankTransportCertificateSha256;
    QString plankTransportToken;
    QString acceptedCaptureSource;
    QString acceptedEncoderBackend;
    QString acceptedEncodingMode;
    const bool macCapture = m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT;
    MacPreviewLaunch::Reply macLaunch;
    quint32 routeInterfaceMtu = 0;
    bool routeIsIpv6 = false;
    const NvComputer::ReachabilityType routeReachability =
            m_Computer->getActiveAddressReachability(&routeInterfaceMtu, &routeIsIpv6);
    const int configuredMtu = m_Preferences->quicUdpPayloadMtu;
    const quint16 quicUdpPayloadMtu = PlankNetwork::quicUdpPayloadMtuForRoute(
                configuredMtu, routeReachability == NvComputer::RI_ZEROTIER,
                routeInterfaceMtu, routeIsIpv6);
    if (quicUdpPayloadMtu == 0) {
        const quint32 overhead = routeIsIpv6 ? PlankNetwork::InnerIpv6UdpOverhead :
                                              PlankNetwork::InnerIpv4UdpOverhead;
        const quint32 minimumMtu = PlankNetwork::MinimumQuicUdpPayloadMtu +
                overhead + PlankNetwork::AutomaticPathSafetyMargin;
        if (routeInterfaceMtu && routeInterfaceMtu < minimumMtu) {
            emit displayLaunchError(tr("The network interface MTU is %1 bytes. PLANK requires at least %2 bytes for %3, including protocol headers and its safety margin.")
                                    .arg(routeInterfaceMtu).arg(minimumMtu)
                                    .arg(routeIsIpv6 ? QStringLiteral("IPv6") : QStringLiteral("IPv4")));
        }
        else {
            emit displayLaunchError(tr("The configured QUIC UDP payload (%1 bytes) is invalid or exceeds the network interface's safe payload limit. Choose Automatic or a smaller value of at least 1200 bytes.")
                                    .arg(configuredMtu));
        }
        qWarning() << "Rejected QUIC UDP payload: configured=" << configuredMtu
                   << "interface MTU=" << routeInterfaceMtu
                   << (routeIsIpv6 ? "IPv6" : "IPv4");
        return false;
    }
    qInfo() << (configuredMtu ? "Manual fixed QUIC UDP payload ceiling:" :
                              "Automatically resolved fixed QUIC UDP payload ceiling:")
            << quicUdpPayloadMtu << "bytes from interface MTU" << routeInterfaceMtu
            << (routeIsIpv6 ? "(IPv6)" : "(IPv4)")
            << "ZeroTier=" << (routeReachability == NvComputer::RI_ZEROTIER);

    try {
        std::unique_ptr<NvHTTP> http = std::make_unique<NvHTTP>(m_Computer);
        if (reconnecting) http->setRequestGate([this](bool auth) { return waitForPlankReconnectRequest(auth); });
        const QString captureSource =
                m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT ?
                    QStringLiteral("screencapturekit") :
                m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10 ?
                    QStringLiteral("x11-native10") : QStringLiteral("nvfbc");
        const QString encoderBackend =
                StreamingPreferences::isPlankAppleProfile(m_PlankVideoProfile) ?
                    QStringLiteral("videotoolbox") :
                StreamingPreferences::isPlankNvencProfile(
                    m_PlankVideoProfile) ?
                    QStringLiteral("nvenc-direct") : QStringLiteral("software-cuda");
        QString encodingMode;
        switch (m_PlankVideoProfile) {
        case StreamingPreferences::PLANK_PROFILE_H264_8BIT_422:
            encodingMode = QStringLiteral("h264-8-422-software");
            break;
        case StreamingPreferences::PLANK_PROFILE_H264_8BIT_444:
            encodingMode = QStringLiteral("h264-8-444-software");
            break;
        case StreamingPreferences::PLANK_PROFILE_H264_10BIT_422:
            encodingMode = QStringLiteral("h264-10-422-software");
            break;
        case StreamingPreferences::PLANK_PROFILE_NVENC_H264_8BIT_444:
            encodingMode = QStringLiteral("h264-8-444-nvenc");
            break;
        case StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444:
            encodingMode = QStringLiteral("hevc-8-444-nvenc");
            break;
        case StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444:
            encodingMode = QStringLiteral("hevc-10-444-nvenc");
            break;
        case StreamingPreferences::PLANK_PROFILE_H264_10BIT_444:
            encodingMode = QStringLiteral("h264-10-444-software");
            break;
        case StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_420:
            encodingMode = QStringLiteral("hevc-10-420-videotoolbox");
            break;
        case StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_444:
            encodingMode = QStringLiteral("hevc-10-444-videotoolbox");
            break;
        default:
            emit displayLaunchError(tr("The bookmark contains an invalid encoding profile."));
            return false;
        }
        const auto startApp = [&]() {
            if (macCapture) {
                QString pin;
                const NvOutputTopology topology = http->getOutputTopology(&pin);
                if (topology.toJson() != m_Computer->outputTopology.toJson() ||
                        topology.desktopWidth != m_StreamConfig.width ||
                        topology.desktopHeight != m_StreamConfig.height) {
                    throw GfeHttpResponseException(409, "The Mac display changed. Reconnect to refresh its capture geometry.");
                }
                // Launch is one-shot, including an ambiguous network timeout.
                // Readiness failures above keep the setup context instead.
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->sessionToken.fill(QChar('\0'));
                    m_Computer->sessionToken.clear();
                    m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
                }
                macLaunch = http->startMacPreview(topology, pin, m_StreamConfig.bitrate, quicUdpPayloadMtu);
                m_MacClipboardNegotiated = macLaunch.clipboard;
                plankTransportPort = http->controlPort();
                plankTransportCertificateSha256 = pin;
                plankTransportToken = QString::fromLatin1(macLaunch.transportToken);
                macLaunch.transportToken.fill('\0');
                macLaunch.transportToken.clear();
                acceptedCaptureSource = captureSource;
                acceptedEncoderBackend = encoderBackend;
                acceptedEncodingMode = encodingMode;
                return;
            }
            http->startApp(m_Computer->currentGameId != 0 ? "resume" : "launch",
                          m_App.id, &m_StreamConfig,
                          m_Preferences->playAudioOnHost,
                          0,
                          false,
                          NvOutputTopology::ScaledSpanMode,
                          m_Computer->outputTopology.generation,
                          m_Computer->plankTopologyVersion,
                          m_Computer->plankFeatureFlags &
                              NvOutputTopology::SupportedFeatureFlags,
                          takeOverActiveSession,
                          m_ResolvedHostLayout,
                          m_ResolvedVirtualModes.value(0),
                          m_ResolvedVirtualModes.value(1),
                          captureSource,
                          encoderBackend,
                          encodingMode,
                          quicUdpPayloadMtu,
                          plankTransportPort,
                          plankTransportCertificateSha256,
                          plankTransportToken,
                          acceptedCaptureSource,
                          acceptedEncoderBackend,
                          acceptedEncodingMode);
        };
        try {
            startApp();
        } catch (const GfeHttpResponseException& e) {
            const QString statusMessage = QString::fromUtf8(e.getStatusMessage());
            const bool displayTransitionStarted =
                    m_Computer->plankAuthentication &&
                    ((e.getStatusCode() == 425 &&
                      statusMessage ==
                          QStringLiteral("PLANK host display transition started")) ||
                     (takeOverActiveSession &&
                      e.getStatusCode() == 503 &&
                      statusMessage ==
                          QStringLiteral("Host display layout transition is currently unavailable")));
            const bool activeSessionConflict =
                    m_Computer->plankAuthentication &&
                    e.getStatusCode() == 409 &&
                    QString::fromUtf8(e.getStatusMessage()) ==
                        QStringLiteral("PLANK workstation session is active");
            if (displayTransitionStarted) {
                constexpr int RetryIntervalMs = 500;
                constexpr int MaximumWaitMs = 45000;
                constexpr int CancellationPollMs = 50;
                bool started = false;

                if (m_PlankUsername.isEmpty() ||
                        m_PlankPassword.isEmpty()) {
                    throw;
                }
                m_WaitingForSessionCleanup.store(true);
                emit sessionCleanupWaitChanged(
                            true,
                            tr("Applying workstation display layout..."));
                qInfo() << "PLANK host display transition started; waiting up to"
                        << MaximumWaitMs << "ms";
                bool authenticationRefreshRequired = false;

                for (int elapsedMs = 0;
                     elapsedMs < MaximumWaitMs && !started;
                     elapsedMs += RetryIntervalMs) {
                    for (int delayMs = 0;
                         delayMs < RetryIntervalMs;
                         delayMs += CancellationPollMs) {
                        if (m_ConnectionStartCancelled.load()) break;
                        SDL_Delay(CancellationPollMs);
                    }
                    if (m_ConnectionStartCancelled.load()) break;

                    if (authenticationRefreshRequired) {
                        try {
                            {
                                QWriteLocker lock(&m_Computer->lock);
                                m_Computer->sessionToken.fill(QChar('\0'));
                                m_Computer->sessionToken.clear();
                                m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
                                m_Computer->currentGameId = 0;
                            }
                            http = std::make_unique<NvHTTP>(m_Computer);
                            if (reconnecting) http->setRequestGate([this](bool auth) { return waitForPlankReconnectRequest(auth); });
                            const QString token = http->authenticate(
                                        m_PlankUsername,
                                        m_PlankPassword);
                            {
                                QWriteLocker lock(&m_Computer->lock);
                                m_Computer->sessionToken = token;
                                m_Computer->authorizationState = NvComputer::AS_AUTHORIZED;
                            }
                            authenticationRefreshRequired = false;
                            qInfo() << "PLANK authenticated to the replacement display worker";
                        } catch (const GfeHttpResponseException& retryError) {
                            if (reconnecting && PlankReconnectPolicy::terminalStatus(retryError.getStatusCode(), true))
                                m_ReconnectCancelled.store(true);
                            throw;
                        } catch (const QtNetworkReplyException& retryError) {
                            if (retryError.getError() == QNetworkReply::SslHandshakeFailedError) throw;
                            qInfo() << "PLANK replacement display worker is not ready for authentication:"
                                    << retryError.toQString();
                            continue;
                        }
                    }

                    try {
                        const NvOutputTopology topology = http->getOutputTopology();
                        {
                            QWriteLocker lock(&m_Computer->lock);
                            m_Computer->outputTopology = topology;
                        }
                        if (m_ComputerManager != nullptr) {
                            m_ComputerManager->clientSideAttributeUpdated(m_Computer);
                        }
                        if (!configurePlankLaunchGeometry()) {
                            m_WaitingForSessionCleanup.store(false);
                            emit sessionCleanupWaitChanged(false, QString());
                            return false;
                        }
                        if (!topology.matchesRequestedHostLayout(
                                    m_ResolvedHostLayout,
                                    m_ResolvedVirtualModes)) {
                            qInfo() << "PLANK display transition is still pending:"
                                    << topology.layoutKind << topology.virtualModes;
                            continue;
                        }
                        startApp();
                        started = true;
                    } catch (const GfeHttpResponseException& retryError) {
                        if (retryError.getStatusCode() == 423) {
                            m_WaitingForSessionCleanup.store(false);
                            emit sessionCleanupWaitChanged(false, QString());
                            throw;
                        }
                        if (retryError.getStatusCode() == 401) {
                            authenticationRefreshRequired = true;
                            qInfo() << "PLANK display worker changed; authentication will be refreshed once";
                            continue;
                        }
                        if (retryError.getStatusCode() != 409 &&
                                retryError.getStatusCode() != 425 &&
                                retryError.getStatusCode() != 503) {
                            m_WaitingForSessionCleanup.store(false);
                            emit sessionCleanupWaitChanged(false, QString());
                            throw;
                        }
                        qInfo() << "PLANK display transition wait attempt failed:"
                                << retryError.toQString();
                    } catch (const QtNetworkReplyException& retryError) {
                        if (retryError.getError() == QNetworkReply::SslHandshakeFailedError) throw;
                        qInfo() << "PLANK display transition worker is not ready:"
                                << retryError.toQString();
                    }
                }

                m_WaitingForSessionCleanup.store(false);
                emit sessionCleanupWaitChanged(false, QString());
                if (!started && m_ConnectionStartCancelled.load()) {
                    qInfo() << "PLANK connection cancelled during display transition";
                    return false;
                }
                if (!started) {
                    if (!reconnecting) {
                        emit displayLaunchError(
                                    tr("The workstation display layout did not become ready within 45 seconds."));
                    }
                    return false;
                }
                qInfo() << "PLANK display transition completed; launch succeeded";
            }
            else if (activeSessionConflict) {
                if (reconnecting) {
                    m_CanReconnect.store(false);
                    m_ReconnectCancelled.store(true);
                    emit displayLaunchError(
                                tr("This PLANK session was transferred to another client."));
                    qInfo() << "PLANK reconnect stopped because another client owns the active session";
                    return false;
                }
                if (takeOverActiveSession ||
                        (m_Computer->plankFeatureFlags &
                         NvOutputTopology::SessionTakeoverFeature) == 0) {
                    emit displayLaunchError(
                                tr("The workstation has an active PLANK session that cannot be transferred."));
                    return false;
                }

                constexpr int DecisionPollMs = 50;
                m_ActiveSessionTakeoverDecision.store(0);
                m_WaitingForActiveSessionTakeoverDecision.store(true);
                emit activeSessionTakeoverRequested(
                            tr("This workstation already has an active PLANK session. Disconnect the existing client and continue?"));
                while (m_ActiveSessionTakeoverDecision.load() == 0 &&
                       !m_ConnectionStartCancelled.load()) {
                    SDL_Delay(DecisionPollMs);
                }
                m_WaitingForActiveSessionTakeoverDecision.store(false);
                if (m_ActiveSessionTakeoverDecision.exchange(0) != 1 ||
                        m_ConnectionStartCancelled.load()) {
                    qInfo() << "PLANK active-session takeover was cancelled";
                    return false;
                }

                m_WaitingForSessionCleanup.store(true);
                emit sessionCleanupWaitChanged(
                            true, tr("Transferring workstation session..."));
                const bool transferred = startConnectionAsync(false, true);
                m_WaitingForSessionCleanup.store(false);
                emit sessionCleanupWaitChanged(false, QString());
                return transferred;
            }
            else if (reconnecting && m_Computer->plankAuthentication &&
                    m_Computer->currentGameId == 0 &&
                    e.getStatusCode() == 400) {
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->currentGameId = m_App.id;
                }
                qInfo() << "PLANK worker already has an active Desktop stream; resuming it";
                startApp();
            }
            else if (reconnecting && m_Computer->plankAuthentication &&
                    m_Computer->currentGameId != 0 &&
                    e.getStatusCode() == 503) {
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->currentGameId = 0;
                }
                qInfo() << "PLANK replacement worker has no app to resume; "
                           "launching a fresh Desktop stream";
                startApp();
            }
            else {
                const bool generationBinding =
                        (m_Computer->plankFeatureFlags &
                         NvOutputTopology::TopologyGenerationFeature) != 0;
                if (!m_Computer->plankAuthentication || !generationBinding ||
                        e.getStatusCode() != 409) {
                    throw;
                }

                const NvOutputTopology topology = http->getOutputTopology();
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->outputTopology = topology;
                }
                if (m_ComputerManager != nullptr) {
                    m_ComputerManager->clientSideAttributeUpdated(m_Computer);
                }
                if (!configurePlankLaunchGeometry()) {
                    return false;
                }
                qInfo() << "PLANK refreshed stale topology and launch geometry; retrying launch:"
                        << topology.generation << m_StreamConfig.width
                        << m_StreamConfig.height;
                startApp();
            }
        }

        // Record the successful launch immediately. If low-level transport
        // setup fails afterward, the next bounded attempt must resume this
        // app instead of issuing a second launch request.
        if (m_Computer->plankAuthentication) {
            m_PlankWorkerInstance = (m_Computer->plankFeatureFlags & NvOutputTopology::WorkerInstanceFeature) ?
                        http->workerInstance() : QString();
            m_PlankHostCertificateSha256 = plankTransportCertificateSha256;
            QWriteLocker lock(&m_Computer->lock);
            m_Computer->currentGameId = m_App.id;
        }

        if (m_Computer->plankAuthentication) {
            {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->sessionToken.fill(QChar('\0'));
                m_Computer->sessionToken.clear();
                m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
            }
            if (m_ComputerManager != nullptr) {
                m_ComputerManager->clientSideAttributeUpdated(m_Computer);
            }
            qInfo() << "PLANK authentication token consumed after launch";
        }
    } catch (const GfeHttpResponseException& e) {
        if (reconnecting && PlankReconnectPolicy::terminalStatus(e.getStatusCode(), false)) {
            // Operator consent cannot recover through automatic reauthentication.
            m_ReconnectCancelled.store(true);
            emit displayLaunchError(e.toQString());
        }
        m_WaitingForSessionCleanup.store(false);
        emit sessionCleanupWaitChanged(false, QString());
        if (reconnecting && e.getStatusCode() == 401) {
            QWriteLocker lock(&m_Computer->lock);
            m_Computer->sessionToken.fill(QChar('\0'));
            m_Computer->sessionToken.clear();
            m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
        }
        if (!reconnecting) {
            emit displayLaunchError(tr("Host returned error: %1").arg(e.toQString()));
        } else {
            qWarning() << "PLANK reconnect launch failed:" << e.toQString();
        }
        return false;
    } catch (const QtNetworkReplyException& e) {
        if (reconnecting && e.getError() == QNetworkReply::SslHandshakeFailedError) {
            m_ReconnectCancelled.store(true);
            emit displayLaunchError(e.toQString());
        }
        m_WaitingForSessionCleanup.store(false);
        emit sessionCleanupWaitChanged(false, QString());
        if (!reconnecting) {
            emit displayLaunchError(e.toQString());
        } else {
            qWarning() << "PLANK reconnect transport setup failed:"
                       << e.toQString();
        }
        return false;
    }

#ifdef PLANK_TRANSPORT
    LiSetPlankNativeControlSender(nullptr, nullptr);
    LiSetPlankNativeInputSender(nullptr, nullptr);
#endif
    if (!startPlankTransportDataPlane(plankTransportPort,
                                 plankTransportCertificateSha256,
                                 plankTransportToken,
                                 quicUdpPayloadMtu)) {
        plankTransportToken.fill(QChar('\0'));
        if (!reconnecting) {
            emit displayLaunchError(
                        tr("The experimental PLANK data plane could not be established."));
        }
        return false;
    }
    plankTransportToken.fill(QChar('\0'));

    QString nativeNegotiationError;
    const bool negotiated = macCapture ?
                LiSetPlankNativeSessionConfiguration(&macLaunch.configuration) == 0 :
                negotiatePlankTransportSession(plankTransportPort, nativeNegotiationError);
    if (!negotiated) {
        if (macCapture) nativeNegotiationError = tr("The Mac returned an invalid native media configuration.");
        stopPlankTransportDataPlane();
        if (!reconnecting) {
            emit displayLaunchError(nativeNegotiationError);
        }
        else {
            qWarning() << "PLANK native session negotiation failed:"
                       << nativeNegotiationError;
        }
        return false;
    }

    QByteArray hostnameStr = m_Computer->activeAddress.address().toLatin1();
    QByteArray siAppVersion = m_Computer->appVersion.toLatin1();

    SERVER_INFORMATION hostInfo;
    LiInitializeServerInformation(&hostInfo);
    hostInfo.address = hostnameStr.data();
    hostInfo.serverInfoAppVersion = siAppVersion.data();
    hostInfo.serverCodecModeSupport = m_Computer->serverCodecModeSupport;

    // moonlight-common-c fills missing callbacks in the caller-owned table.
    // Restore the pull/push decoder contract before every reuse of this
    // Session for a PLANK desktop handoff.
    m_VideoCallbacks.submitDecodeUnit =
            (m_VideoCallbacks.capabilities & CAPABILITY_PULL_RENDERER) ?
                nullptr : drSubmitDecodeUnit;

    int err = LiStartConnection(&hostInfo, &m_StreamConfig, &k_ConnCallbacks,
                                &m_VideoCallbacks, &m_AudioCallbacks,
                                NULL, 0, NULL, 0);
    if (err != 0) {
        stopPlankTransportDataPlane();
        // We already displayed an error dialog in the stage failure
        // listener.
        return false;
    }

#ifdef PLANK_TRANSPORT
    startPlankTransportMediaReceivers();
#endif

    if (!macCapture && (LiGetHostFeatureFlags() & LI_FF_LOCAL_CURSOR) == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Host does not advertise required PLANK local cursor transport");
        stopPlankTransportMediaReceivers();
        LiStopConnection();
        stopPlankTransportDataPlane();
        emit displayLaunchError(
                    tr("This workstation does not support the required PLANK local cursor protocol."));
        return false;
    }

    emit connectionStarted();
    return true;
}

void Session::cancelConnectionStart()
{
    m_ConnectionStartCancelled.store(true);
    m_ActiveSessionTakeoverDecision.store(-1);
}

void Session::respondToActiveSessionTakeover(bool takeOver)
{
    int expected = 0;
    if (m_ActiveSessionTakeoverDecision.compare_exchange_strong(
                expected, takeOver ? 1 : -1)) {
        qInfo() << "PLANK active-session takeover decision:"
                << (takeOver ? "take over" : "cancel");
    }
}

void Session::setPlankReconnectStatus(const char* text, bool warning)
{
    const bool nativeStatus = m_PlankToolbar &&
            m_PlankToolbar->setReconnectStatus(QString::fromUtf8(text), warning);
    // Reconnect pauses video, so an overlay alone cannot reliably repaint.
    // Use the native local surface on Wayland; keep the existing fallback for
    // other presentation platforms without duplicating the message.
    m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, false);
    if (!nativeStatus && text[0] != '\0') {
        m_OverlayManager.setOverlayColor(Overlay::OverlayStatusUpdate,
                    warning ? SDL_Color{0xCC, 0x00, 0x00, 0xFF} : SDL_Color{0xE0, 0xE0, 0xE0, 0xFF});
        m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate, text);
        m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);
    }
    if (text[0] != '\0') {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PLANK reconnect status (%s): %s",
                    nativeStatus ? "native local surface" : "video overlay fallback", text);
    }
}

bool Session::beginPlankReconnect(
        PlankReconnectState& state)
{
    if (m_PlankUsername.isEmpty() ||
            m_PlankPassword.isEmpty()) {
        return false;
    }

    m_Reconnecting.store(true);
    m_ReconnectGreeterConfirmed.store(false);
    m_LogoutReturnedToLogin.store(false);
    const bool openingDesktop = m_DesktopHandoffNoticeDeadline.exchange(0) > SDL_GetTicks();
    setPlankReconnectStatus(
                openingDesktop ? "Opening your desktop..." : "Waiting for workstation...", false);

    m_InputHandler->raiseAllKeys();
    state = {};
    state.inputCaptureWasActive = m_InputHandler->isCaptureActive();
    // The remote cursor transport stops with the host connection. Return
    // pointer ownership to the local compositor immediately so the user can
    // still see and navigate the whole client window while reconnecting. The
    // native reconnect prompt has its own Wayland surface, but cursor
    // visibility must not depend on entering that surface first.
    m_InputHandler->setCaptureActive(false);
    m_InputHandler->beginRawHidReconnect();
    state.videoFormat = m_ActiveVideoFormat;
    state.videoWidth = m_ActiveVideoWidth;
    state.videoHeight = m_ActiveVideoHeight;
    state.videoFrameRate = m_ActiveVideoFrameRate;
    SDL_LockSpinlock(&m_DecoderLock);
    if (m_VideoDecoder != nullptr) {
        state.retainedRenderer = m_VideoDecoder->suspendForReconnect();
        if (!state.retainedRenderer) {
            delete m_VideoDecoder;
            m_VideoDecoder = nullptr;
        }
    }
    SDL_UnlockSpinlock(&m_DecoderLock);
#ifdef PLANK_TRANSPORT
    stopPlankTransportMediaReceivers();
#endif
    LiStopConnection();
    stopPlankTransportDataPlane();
    m_InputHandler->resetRemoteCursorPositionEpoch();
    m_ReconnectCancelled.store(false);
    m_ConnectionStartCancelled.store(false);
    return true;
}

bool Session::waitForPlankReconnectRequest(bool restartAuthenticationAfterWait)
{
    bool waited = false;
    while (!m_ReconnectCancelled.load() && !m_ConnectionStartCancelled.load()) {
        if (m_ReconnectPolicy.allowsRequest(SDL_GetTicks())) {
            // A password conversation may have expired while Ask was open.
            // Restart it after explicit consent, rather than submitting a stale
            // challenge response and treating its rejection as a bad password.
            return !(waited && restartAuthenticationAfterWait);
        }
        waited = true;
        SDL_Delay(50);
    }
    return false;
}

bool Session::runPlankReconnect()
{
    if (m_PlankUsername.isEmpty() ||
            m_PlankPassword.isEmpty()) {
        return false;
    }

    {
        QWriteLocker lock(&m_Computer->lock);
        m_Computer->sessionToken.fill(QChar('\0'));
        m_Computer->sessionToken.clear();
        m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
        m_Computer->currentGameId = 0;
    }
    for (int attempt = 1; waitForPlankReconnectRequest(); ++attempt) {
        bool authenticating = false;
        try {
            QString token;
            {
                QReadLocker lock(&m_Computer->lock);
                token = m_Computer->sessionToken;
            }
            NvHTTP http(m_Computer);
            http.setRequestGate([this](bool auth) { return waitForPlankReconnectRequest(auth); });
            if (token.isEmpty()) {
                authenticating = true;
                bool greeterConfirmed = false;
                token = http.authenticate(m_PlankUsername, m_PlankPassword, &greeterConfirmed,
                                          !m_ReachedUserDesktop.load());
                authenticating = false;
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->sessionToken = token;
                    m_Computer->authorizationState = NvComputer::AS_AUTHORIZED;
                }
                if (greeterConfirmed && m_ReachedUserDesktop.load()) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Authenticated Host is at the sign-in screen after logout");
                    m_LogoutReturnedToLogin.store(true);
                    m_ReconnectCancelled.store(true);
                    return false;
                }
                if (greeterConfirmed &&
                        ((m_Computer->plankFeatureFlags & NvOutputTopology::AuthenticatedDesktopStageFeature) ||
                         m_Computer->plankFeatureFlags == NvOutputTopology::FixedCaptureFlags)) {
                    m_ReconnectGreeterConfirmed.store(true);
                }
            }

            NvOutputTopology topology;
            bool topologySupported;
            bool macDesktop;
            QString desktopMode;
            int desktopScale = 1;
            {
                QReadLocker lock(&m_Computer->lock);
                topologySupported = NvOutputTopology::supportsDescription(
                            m_Computer->plankTopologyVersion, m_Computer->plankFeatureFlags);
                macDesktop = m_Computer->plankFeatureFlags == NvOutputTopology::FixedCaptureFlags;
                desktopMode = m_Computer->plankVirtualMode1;
                if (macDesktop && m_Computer->plankHostLayout == NvOutputTopology::MatchClientHostLayout) {
                    QVector<NvClientDisplay> displays;
                    for (const auto& display : std::as_const(m_ClientDisplays)) {
                        displays.append({display.macMatchedBounds.isValid() ? display.macMatchedBounds : QRect(display.logicalBounds.x, display.logicalBounds.y,
                                               display.logicalBounds.w, display.logicalBounds.h), display.nativeSize, display.macBackingSize});
                    }
                    desktopMode = NvOutputTopology::resolveMacClientDisplayMode(displays, nullptr, &desktopScale);
                    if (desktopMode.isEmpty()) return false;
                }
            }
            if (topologySupported) {
                topology = macDesktop ? http.prepareMacDisplay(desktopMode,
                    StreamingPreferences::plankAppleEncodingMode(m_PlankVideoProfile), desktopScale) : http.getOutputTopology();
            }
            const QVector<NvApp> apps = http.getAppList();
            {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->sessionToken = token;
                m_Computer->authorizationState = NvComputer::AS_AUTHORIZED;
                if (topologySupported) {
                    m_Computer->outputTopology = topology;
                }
                m_Computer->updateAppList(apps);
            }

            if (waitForPlankReconnectRequest() && startConnectionAsync(true) &&
                    !m_ReconnectCancelled.load() && m_ReconnectPolicy.allowsRequest(SDL_GetTicks())) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "PLANK reconnect transport completed on attempt %d",
                            attempt);
                return true;
            }
        } catch (const GfeHttpResponseException& error) {
            qWarning() << "PLANK reconnect attempt" << attempt
                       << "failed:" << error.toQString();
            if (PlankReconnectPolicy::terminalStatus(error.getStatusCode(), authenticating)) {
                m_ReconnectCancelled.store(true);
                emit displayLaunchError(error.toQString());
            }
            if (error.getStatusCode() == 401) {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->sessionToken.fill(QChar('\0'));
                m_Computer->sessionToken.clear();
                m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
            }
        } catch (const QtNetworkReplyException& error) {
            if (error.getError() == QNetworkReply::SslHandshakeFailedError) {
                m_ReconnectCancelled.store(true);
                emit displayLaunchError(error.toQString());
            }
            qWarning() << "PLANK reconnect attempt" << attempt
                       << "was deferred or failed:" << error.toQString();
        }

#ifdef PLANK_TRANSPORT
        // If setup finished after the Ask deadline, discard that transport
        // before pausing. Do not keep a hidden connection alive (or accept a
        // dead one) while the operator is away from the unanswered prompt.
        stopPlankTransportMediaReceivers();
#endif
        LiStopConnection();
        stopPlankTransportDataPlane();
        if (!m_ReconnectCancelled.load()) {
            constexpr int RetryDelayMs = 1000;
            constexpr int CancellationPollMs = 50;
            for (int elapsedMs = 0;
                 elapsedMs < RetryDelayMs && !m_ReconnectCancelled.load();
                 elapsedMs += CancellationPollMs) {
                SDL_Delay(CancellationPollMs);
            }
        }
    }

    return false;
}

bool Session::finishPlankReconnect(
        bool success,
        const PlankReconnectState& state)
{
    bool resumedRenderer = false;
    if (success) {
        const bool streamConfigurationUnchanged =
                state.videoFormat == m_ActiveVideoFormat &&
                state.videoWidth == m_ActiveVideoWidth &&
                state.videoHeight == m_ActiveVideoHeight &&
                state.videoFrameRate == m_ActiveVideoFrameRate;
        if (state.retainedRenderer && streamConfigurationUnchanged) {
            SDL_LockSpinlock(&m_DecoderLock);
            resumedRenderer = m_VideoDecoder != nullptr &&
                    m_VideoDecoder->resumeAfterReconnect();
            SDL_UnlockSpinlock(&m_DecoderLock);
        }
        m_InputHandler->finishRawHidReconnect();
        m_UnexpectedTermination = false;
    } else {
        m_UnexpectedTermination = true;
    }

    m_Reconnecting.store(false);
    m_ReconnectGreeterConfirmed.store(false);
    m_DesktopHandoffNoticeDeadline.store(0);
    setPlankReconnectStatus("", false);
    m_ReconnectRequested = false;
    m_ReconnectCancelled.store(false);
    m_ConnectionStartCancelled.store(false);
    m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, false);

    m_OverlayManager.setOverlayColor(Overlay::OverlayStatusUpdate, {0xCC, 0x00, 0x00, 0xFF});

    if (!success) {
        return false;
    }

    if (!resumedRenderer) {
        SDL_Event resetEvent = {};
        resetEvent.type = SDL_EVENT_RENDER_DEVICE_RESET;
        SDL_PushEvent(&resetEvent);
    } else {
        LiRequestIdrFrame();
    }
    if (state.inputCaptureWasActive) {
        m_InputHandler->setCaptureActive(true);
    }
#ifdef Q_OS_MACOS
    // Receiver teardown removes this timer on every reconnect attempt. Resume
    // periodic polling only after success, back on the SDL/AppKit main thread.
    startClipboardPollTimer();
#endif
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK reconnect completed (%s renderer)",
                resumedRenderer ? "retained" : "recreated");
    return true;
}

// At most one bounded, credential-free probe is outstanding. It owns its
// address/identity snapshots and never reads or changes Session state.
class PlankWorkerProbeThread : public QThread
{
public:
    PlankWorkerProbeThread(NvAddress address, QString instance, QString certificate) :
        m_Address(address), m_Instance(instance), m_Certificate(certificate) { }

    bool replacement() const { return m_Replacement; }
    const QString& instance() const { return m_Instance; }

    void run() override
    {
        try {
            NvHTTP http(m_Address);
            m_Replacement = http.probeWorkerReplacement(m_Instance, m_Certificate);
        } catch (const GfeHttpResponseException&) {
            // Failure is not proof of a replacement; preserve the live stream.
        } catch (const QtNetworkReplyException&) {
        }
    }

private:
    NvAddress m_Address;
    QString m_Instance;
    QString m_Certificate;
    bool m_Replacement = false;
};

class PlankReconnectThread : public QThread
{
public:
    explicit PlankReconnectThread(Session* session) :
        QThread(nullptr),
        m_Session(session),
        m_Success(false),
        m_CompletionPosted(false)
    {
        setObjectName("PLANK Reconnect");
    }

    bool succeeded() const
    {
        return m_Success;
    }

    bool completionPosted() const
    {
        return m_CompletionPosted.load();
    }

    void run() override
    {
        m_Success = m_Session->runPlankReconnect();

        SDL_Event event = {};
        event.type = SDL_EVENT_USER;
        event.user.code = SDL_CODE_PLANK_REPLANK_COMPLETE;
        const bool posted = SDL_PushEvent(&event);
        m_CompletionPosted.store(posted);
        if (!posted) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to post PLANK reconnect completion: %s",
                         SDL_GetError());
        }
    }

private:
    Session* m_Session;
    bool m_Success;
    std::atomic_bool m_CompletionPosted;
};

void Session::flushWindowEvents()
{
    // Pump events to ensure all pending OS events are posted
    SDL_PumpEvents();

    // SDL_CreateRenderer() callers use this barrier after renderer setup
    // because SDL may have recreated one or more native windows. Refresh every
    // Wayland Wacom subsurface explicitly; native wl_surface proxy addresses
    // alone cannot identify a replacement reliably.
    if (m_InputHandler != nullptr) {
        m_InputHandler->refreshWaylandTabletCursorParents();
    }

    // Insert a barrier to discard any additional window events.
    // We don't use SDL_FlushEvent() here because it could cause
    // important events to be lost.
    m_FlushingWindowEventsRef++;

    // This event will cause us to set m_FlushingWindowEvents back to false.
    SDL_Event flushEvent = {};
    flushEvent.type = SDL_EVENT_USER;
    flushEvent.user.code = SDL_CODE_FLUSH_WINDOW_EVENT_BARRIER;
    SDL_PushEvent(&flushEvent);
}

class ExecThread : public QThread
{
public:
    ExecThread(Session* session) :
        QThread(nullptr),
        m_Session(session)
    {
        setObjectName("Session Exec");
    }

    void run() override
    {
        m_Session->execInternal();
    }

    Session* m_Session;
};

void Session::exec(QWindow* qtWindow)
{
#ifdef Q_OS_MACOS
    auto* application = static_cast<MacApplication*>(QCoreApplication::instance());
    if (!application->beginSession()) {
        emit readyForDeletion();
        return;
    }
    const auto exitConnection = connect(application, &MacApplication::exitRequested,
                                       this, [this] {
        m_ApplicationExitRequested.store(true);
        cancelConnectionStart();
        m_ReconnectCancelled.store(true);
        m_CanReconnect.store(false);
    });
    // sessionFinished precedes asynchronous transport cleanup. Only this
    // later signal releases application ownership. Queue onto Qt's thread;
    // the Session itself may already have been garbage-collected by QML.
    connect(this, &Session::readyForDeletion, application,
            [application, exitConnection] {
        QObject::disconnect(exitConnection);
        application->endSession();
    }, Qt::QueuedConnection);
#endif
    m_QtWindow = qtWindow;

    // Use a separate thread for the streaming session on X11 or Wayland
    // to ensure we don't stomp on Qt's GL context. This breaks when using
    // the Qt EGLFS backend, so we will restrict this to X11
    m_ThreadedExec = WMUtils::isRunningX11() || WMUtils::isRunningWayland();

    if (m_ThreadedExec) {
        // Run the streaming session on a separate thread for Linux/BSD
        ExecThread execThread(this);
        execThread.start();

        // Until the SDL streaming window is created, we should continue
        // to update the Qt UI to allow warning messages to display and
        // make sure that the Qt window can hide itself.
        while (!execThread.wait(10) && m_Window == nullptr) {
            const bool allowUserInput =
                    m_WaitingForSessionCleanup.load() ||
                    m_WaitingForActiveSessionTakeoverDecision.load();
            QCoreApplication::processEvents(
                        allowUserInput ?
                            QEventLoop::AllEvents :
                            QEventLoop::ExcludeUserInputEvents);
            QCoreApplication::sendPostedEvents();
        }
        const bool allowUserInput =
                m_WaitingForSessionCleanup.load() ||
                m_WaitingForActiveSessionTakeoverDecision.load();
        QCoreApplication::processEvents(
                    allowUserInput ?
                        QEventLoop::AllEvents :
                        QEventLoop::ExcludeUserInputEvents);
        QCoreApplication::sendPostedEvents();

        // SDL is in charge now. Wait until the streaming thread exits
        // to further update the Qt window.
        execThread.wait();
    }
    else {
        // Run the streaming session on the main thread for Windows and macOS
        execInternal();
    }
}

void Session::execInternal()
{
    // Complete initialization in this deferred context to avoid
    // calling expensive functions in the constructor (during the
    // process of loading the StreamSegue).
    //
    // NB: This initializes the SDL video subsystem, so it must be
    // called on the main thread.
    const bool initialized = initialize();
    if (!initialized
#ifdef Q_OS_MACOS
            || m_ApplicationExitRequested.load()
#endif
            ) {
#ifdef Q_OS_MACOS
        if (initialized) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
        if (!m_ApplicationExitRequested.load())
#endif
        emit sessionFinished();
        emit readyForDeletion();
        return;
    }

    // Wait for any old session to finish cleanup
    s_ActiveSessionSemaphore.acquire();

    // We're now active
    s_ActiveSession = this;

    // Initialize input before starting the connection.
    // PLANK is a remote-desktop product. Like RGS desktop mode, use
    // authoritative absolute coordinates and reserve relative capture for a
    // distinct game-mode path. This also gives receiver UI exact hit testing.
    m_InputHandler = new SdlInputHandler(*m_Preferences,
                                         m_StreamConfig.width,
                                         m_StreamConfig.height);

    m_ConnectionStartCancelled.store(false);
    AsyncConnectionStartThread asyncConnThread(this);
    if (!m_ThreadedExec) {
        // Kick off the async connection thread while we sit here and pump the event loop
        asyncConnThread.start();
        while (!asyncConnThread.wait(10)) {
            const bool allowUserInput =
                    m_WaitingForSessionCleanup.load() ||
                    m_WaitingForActiveSessionTakeoverDecision.load();
            QCoreApplication::processEvents(
                        allowUserInput ?
                            QEventLoop::AllEvents :
                            QEventLoop::ExcludeUserInputEvents);
            QCoreApplication::sendPostedEvents();
        }

        // Pump the event loop one last time to ensure we pick up any events from
        // the thread that happened while it was in the final successful QThread::wait().
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        QCoreApplication::sendPostedEvents();
    }
    else {
        // We're already in a separate thread so run the connection operations
        // synchronously and don't pump the event loop. The main thread is already
        // pumping the event loop for us.
        asyncConnThread.run();
    }

    // If the connection failed, clean up and abort the connection.
    if (!m_AsyncConnectionSuccess) {
        delete m_InputHandler;
        m_InputHandler = nullptr;
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        QThreadPool::globalInstance()->start(new DeferredSessionCleanupTask(this));
        return;
    }

#ifdef Q_OS_MACOS
    startClipboardPollTimer();
#endif

    int x, y, width, height;
    getWindowDimensions(x, y, width, height);

    const bool createWaylandFullscreen =
            m_IsFullScreen &&
            strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0;
    bool presentationMappingDeferred =
            createWaylandFullscreen && !m_UseMultiDisplayPresentation;
    if (createWaylandFullscreen) {
        SDL_Rect displayBounds;
        const SDL_DisplayID display = StreamUtils::getDisplayId(
                    getTargetDisplayIndex());
        if (SDL_GetDisplayBounds(display, &displayBounds)) {
            // SDL 3.4.2 can retain the initially requested Wayland viewport
            // as its absolute-pointer coordinate range after a later
            // fullscreen configure. Request the compositor's complete logical
            // output size from the first surface configure so there is no
            // coordinate-space transition to retain.
            width = displayBounds.w;
            height = displayBounds.h;
            x = y = SDL_WINDOWPOS_CENTERED_DISPLAY(display);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Creating Wayland fullscreen surface at %dx%d",
                        width, height);
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to query Wayland fullscreen bounds: %s",
                        SDL_GetError());
        }
    }

#ifdef STEAM_LINK
    // We need a little delay before creating the window or we will trigger some kind
    // of graphics driver bug on Steam Link that causes a jagged overlay to appear in
    // the top right corner randomly.
    SDL_Delay(500);
#endif

    // Request at least 8 bits per color for GL
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);

    // We always want a resizable window with High DPI enabled
    Uint32 defaultWindowFlags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE;
    if (createWaylandFullscreen && !presentationMappingDeferred) {
        // Enter compositor-native fullscreen on the initial configure. This
        // prevents SDL from binding pointer input to an intermediate windowed
        // viewport before the fullscreen surface exists.
        defaultWindowFlags |= m_FullScreenFlag;
    }
    if (presentationMappingDeferred) {
        // Decoder selection can switch the SDL window between OpenGL and
        // Vulkan. SDL implements that switch by recreating the native Wayland
        // toplevel. Keep those intermediate surfaces unmapped so GNOME sees
        // one stable PLANK window instead of a sequence of short-lived
        // fullscreen windows.
        defaultWindowFlags |= SDL_WINDOW_HIDDEN;
    }

    // We use only the computer name on macOS to match Apple conventions where the
    // app name is featured in the menu bar and the document name is in the title bar.
#ifdef Q_OS_DARWIN
    std::string windowName = QString(m_Computer->name).toStdString();
#else
    std::string windowName = QString(m_Computer->name + " - PLANK").toStdString();
#endif

    m_Window = SDL_CreateWindow(windowName.c_str(),
                                width,
                                height,
                                defaultWindowFlags | StreamUtils::getPlatformWindowFlags());
    if (!m_Window) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_CreateWindow() failed with platform flags: %s",
                    SDL_GetError());

        m_Window = SDL_CreateWindow(windowName.c_str(),
                                    width,
                                    height,
                                    defaultWindowFlags);
        if (!m_Window) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_CreateWindow() failed: %s",
                         SDL_GetError());

            delete m_InputHandler;
            m_InputHandler = nullptr;
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            QThreadPool::globalInstance()->start(new DeferredSessionCleanupTask(this));
            return;
        }
    }

    SDL_SetWindowPosition(m_Window, x, y);

    if (m_UseMultiDisplayPresentation) {
        for (const auto& display : std::as_const(m_ClientDisplays)) {
            if (display.displayId == m_TargetDisplayId) {
                continue;
            }

            SDL_PropertiesID properties = SDL_CreateProperties();
            const Uint32 flags = defaultWindowFlags |
                    StreamUtils::getPlatformWindowFlags() |
                    (m_IsFullScreen ? 0 : SDL_WINDOW_HIDDEN);
            SDL_SetStringProperty(properties,
                                  SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                                  windowName.c_str());
            SDL_SetNumberProperty(properties,
                                  SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER,
                                  display.logicalBounds.w);
            SDL_SetNumberProperty(properties,
                                  SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER,
                                  display.logicalBounds.h);
            SDL_SetNumberProperty(properties,
                                  SDL_PROP_WINDOW_CREATE_X_NUMBER,
                                  SDL_WINDOWPOS_CENTERED_DISPLAY(display.displayId));
            SDL_SetNumberProperty(properties,
                                  SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                                  SDL_WINDOWPOS_CENTERED_DISPLAY(display.displayId));
            SDL_SetNumberProperty(properties,
                                  SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                                  flags);
            SDL_SetBooleanProperty(properties,
                                   SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN,
                                   m_IsFullScreen);
            SDL_Window* secondary = SDL_CreateWindowWithProperties(properties);
            SDL_DestroyProperties(properties);
            if (secondary == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Failed to create PLANK fullscreen surface for client output %u: %s",
                             display.displayId, SDL_GetError());
                emit displayLaunchError(
                    tr("Unable to create a fullscreen surface for the second client monitor."));
                for (SDL_Window* window : std::as_const(m_SecondaryWindows)) {
                    SDL_DestroyWindow(window);
                }
                m_SecondaryWindows.clear();
                SDL_DestroyWindow(m_Window);
                m_Window = nullptr;
                delete m_InputHandler;
                m_InputHandler = nullptr;
                SDL_QuitSubSystem(SDL_INIT_VIDEO);
                QThreadPool::globalInstance()->start(
                            new DeferredSessionCleanupTask(this));
                return;
            }
            SDL_SetWindowFullscreenMode(secondary, nullptr);
            if (m_IsFullScreen &&
                    !placeFullscreenWindowOnDisplay(secondary,
                                                    display.displayId)) {
                SDL_DestroyWindow(secondary);
                emit displayLaunchError(
                    tr("Unable to place the second fullscreen surface on its client monitor."));
                for (SDL_Window* window : std::as_const(m_SecondaryWindows)) {
                    SDL_DestroyWindow(window);
                }
                m_SecondaryWindows.clear();
                SDL_DestroyWindow(m_Window);
                m_Window = nullptr;
                delete m_InputHandler;
                m_InputHandler = nullptr;
                SDL_QuitSubSystem(SDL_INIT_VIDEO);
                QThreadPool::globalInstance()->start(
                            new DeferredSessionCleanupTask(this));
                return;
            }
            m_SecondaryWindows.append(secondary);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Created PLANK presentation surface for output %u (initially %s)",
                        display.displayId, m_IsFullScreen ? "fullscreen" : "hidden");
        }
    }

    if (!m_IsFullScreen) {
        // Windowed means a normal compositor-managed desktop window. Do not
        // inherit a maximized launcher state that can make it indistinguishable
        // from borderless mode on Wayland.
        SDL_SetWindowFullscreen(m_Window, 0);
        SDL_RestoreWindow(m_Window);
        SDL_SetWindowBordered(m_Window, true);
        SDL_SetWindowResizable(m_Window, true);
        m_HasWindowedPresentationGeometry = true;
    }

    // HACK: Remove once proper Dark Mode support lands in SDL
#ifdef Q_OS_WIN32
    if (m_QtWindow != nullptr) {
        BOOL darkModeEnabled;

        // Query whether dark mode is enabled for our Qt window (which tracks the OS dark mode state)
        if (FAILED(DwmGetWindowAttribute((HWND)m_QtWindow->winId(), DWMWA_USE_IMMERSIVE_DARK_MODE, &darkModeEnabled, sizeof(darkModeEnabled))) &&
            FAILED(DwmGetWindowAttribute((HWND)m_QtWindow->winId(), DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &darkModeEnabled, sizeof(darkModeEnabled)))) {
            darkModeEnabled = FALSE;
        }

        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);

        if (SDL_GetWindowWMInfo(m_Window, &info) && info.subsystem == SDL_SYSWM_WINDOWS) {
            // If dark mode is enabled, propagate that to our SDL window
            if (darkModeEnabled) {
                if (FAILED(DwmSetWindowAttribute(info.info.win.window, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkModeEnabled, sizeof(darkModeEnabled)))) {
                    DwmSetWindowAttribute(info.info.win.window, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &darkModeEnabled, sizeof(darkModeEnabled));
                }

                // Toggle non-client rendering off and back on to ensure dark mode takes effect on Windows 10.
                // DWM doesn't seem to correctly invalidate the non-client area after enabling dark mode.
                DWMNCRENDERINGPOLICY ncPolicy = DWMNCRP_DISABLED;
                DwmSetWindowAttribute(info.info.win.window, DWMWA_NCRENDERING_POLICY, &ncPolicy, sizeof(ncPolicy));
                ncPolicy = DWMNCRP_ENABLED;
                DwmSetWindowAttribute(info.info.win.window, DWMWA_NCRENDERING_POLICY, &ncPolicy, sizeof(ncPolicy));
            }
        }
    }
#endif

    m_InputHandler->setWindow(m_Window);
    rebuildPresentationLayout();

    QImage iconImage(":/res/plank-logo.png");
    iconImage = iconImage.scaled(ICON_SIZE,
                                 ICON_SIZE,
                                 Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation)
                    .convertToFormat(QImage::Format_RGBA8888);
    SDL_Surface* iconSurface = SDL_CreateSurfaceFrom(iconImage.width(),
                                                     iconImage.height(),
                                                     SDL_PIXELFORMAT_RGBA32,
                                                     (void*)iconImage.constBits(),
                                                     4 * iconImage.width());
#ifndef Q_OS_DARWIN
    // Other platforms seem to preserve our Qt icon when creating a new window.
    if (iconSurface != nullptr) {
        // This must be called before entering full-screen mode on Windows
        // or our icon will not persist when toggling to windowed mode
        SDL_SetWindowIcon(m_Window, iconSurface);
        for (SDL_Window* window : std::as_const(m_SecondaryWindows)) {
            SDL_SetWindowIcon(window, iconSurface);
        }
    }
#endif

    // Update the window display mode based on our current monitor
    // for if/when we enter full-screen mode.
    updateOptimalWindowDisplayMode();

    // Enter full screen if requested
    if (m_IsFullScreen) {
        if (presentationMappingDeferred) {
            // Keep the initial window normally sized and hidden through all
            // graphics-backend probes. Fullscreen is queued immediately before
            // the first map, after the final native surface exists. This avoids
            // SDL 3.4.2 sending zero-sized xdg_surface geometry while recreating
            // a hidden fullscreen Wayland window.
            m_PresentationFullscreen = true;
            rebuildPresentationLayout();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Deferring initial Wayland presentation map until decoder selection completes");
        } else {
            setPresentationWindowsFullscreen(true);
        }
    }

    bool needsFirstEnterCapture = false;
    bool needsPostDecoderCreationCapture = false;

    // HACK: For Wayland, we wait until we get the first SDL_EVENT_WINDOW_MOUSE_ENTER
    // event where it seems to work consistently on GNOME. For other platforms,
    // especially where SDL may call SDL_RecreateWindow(), we must only capture
    // after the decoder is created.
    if (strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        // Native Wayland: Capture on SDL_EVENT_WINDOW_MOUSE_ENTER
        needsFirstEnterCapture = true;
    }
    else {
        // X11/XWayland: Capture after decoder creation
        needsPostDecoderCreationCapture = true;
    }

    // Stop text input. SDL enables it by default
    // when we initialize the video subsystem, but this
    // causes an IME popup when certain keys are held down
    // on macOS.
    SDL_StopTextInput(m_Window);
    for (SDL_Window* window : std::as_const(m_SecondaryWindows)) {
        SDL_StopTextInput(window);
    }

    // Disable the screen saver if requested
    if (m_Preferences->keepAwake) {
        SDL_DisableScreenSaver();
    }

    // Hide Qt's fake mouse cursor on EGLFS systems
    if (QGuiApplication::platformName() == "eglfs") {
        QGuiApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
    }

    // Set timer resolution to 1 ms on Windows for greater
    // sleep precision and more accurate callback timing.
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "1");

    SDL_DisplayID currentDisplayId = SDL_GetDisplayForWindow(m_Window);

    // Now that we're about to stream, any SDL_EVENT_QUIT event is expected
    // unless it comes from the connection termination callback where
    // (m_UnexpectedTermination is set back to true).
    m_UnexpectedTermination = false;

    // Toggle the stats overlay if requested by the user
    m_OverlayManager.setOverlayState(Overlay::OverlayDebug, m_Preferences->showPerformanceOverlay);

    const auto initializePlankToolbar = [this]() {
        if (m_Computer->plankAuthentication && !m_PlankToolbar) {
            m_PlankToolbar.reset(new PlankToolbar(
                        m_Window, m_OverlayManager, *m_InputHandler,
                        *m_Preferences, m_PlankBitrateKbps));
        }
    };
    if (!presentationMappingDeferred) {
        initializePlankToolbar();
    }

    // Hijack this thread to be the SDL main thread. We have to do this
    // because we want to suspend all Qt processing until the stream is over.
    PlankReconnectThread* reconnectThread = nullptr;
    PlankWorkerProbeThread* workerProbe = nullptr;
    Uint64 nextWorkerProbe = 0;
    bool earlyWaitingVisible = false;
    PlankReconnectState reconnectState;
    Uint64 reconnectDecisionDeadline = 0;
    const auto handlePlankLocalUserEvent = [this](const SDL_UserEvent& userEvent) {
        switch (userEvent.code) {
        case SDL_CODE_PLANK_BITRATE_APPLIED:
            if (m_PlankToolbar) {
                m_PlankToolbar->setAppliedBitrate(
                            m_ConfirmedBitrateRequestKbps.load(std::memory_order_relaxed),
                            m_ConfirmedBitrateAppliedKbps.load(std::memory_order_relaxed),
                            m_ConfirmedBitratePeakKbps.load(std::memory_order_relaxed));
            }
            return true;
        case SDL_CODE_PLANK_CURSOR:
            if (m_InputHandler != nullptr) {
                m_InputHandler->applyPendingRemoteCursor();
            }
            return true;
        case SDL_CODE_PLANK_TABLET_CURSOR:
            if (m_InputHandler != nullptr) {
                m_InputHandler->applyPendingTabletCursorActivation();
            }
            return true;
        case SDL_CODE_PLANK_CURSOR_POSITION:
            if (m_InputHandler != nullptr) {
                m_InputHandler->applyPendingRemoteCursorPosition();
            }
            return true;
        case SDL_CODE_PLANK_CLIPBOARD:
#ifdef Q_OS_MACOS
            if (m_ClipboardSync != nullptr) {
                m_ClipboardSync->applyPendingHostTextOnMainThread();
            }
#endif
            return true;
        case SDL_CODE_PLANK_CLIPBOARD_POLL:
#ifdef Q_OS_MACOS
            if (m_ClipboardSync != nullptr) {
                m_ClipboardSync->pollLocalClipboardOnMainThread();
            }
#endif
            return true;
        default:
            return false;
        }
    };
    if (presentationMappingDeferred) {
        SDL_Event initializeRendererEvent = {};
        initializeRendererEvent.type = SDL_EVENT_RENDER_DEVICE_RESET;
        if (!SDL_PushEvent(&initializeRendererEvent)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to queue deferred Wayland renderer initialization: %s",
                        SDL_GetError());
            SDL_ShowWindow(m_Window);
            presentationMappingDeferred = false;
            initializePlankToolbar();
        }
    }
    SDL_Event event;
    for (;;) {
#ifdef Q_OS_MACOS
        if (m_ApplicationExitRequested.load()) {
            goto DispatchDeferredCleanup;
        }
#endif
        const Uint64 now = SDL_GetTicks();
        const bool videoSilent = PlankHostRecovery::videoSilent(now, m_LastPlankVideoReceived.load());
        if (workerProbe != nullptr && workerProbe->isFinished()) {
            workerProbe->wait();
            if (!m_Reconnecting.load() && !m_ReconnectRequested.load() && m_CanReconnect.load() &&
                    videoSilent && workerProbe->instance() == m_PlankWorkerInstance && workerProbe->replacement()) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Confirmed replacement Host worker during video silence; reconnecting before transport timeout");
                clConnectionTerminated(-2);
            }
            delete workerProbe;
            workerProbe = nullptr;
            nextWorkerProbe = now + PlankHostRecovery::ProbeIntervalMs;
        }
        if (!m_Reconnecting.load() && !m_ReconnectRequested.load()) {
            if (videoSilent && m_CanReconnect.load() && !m_PlankWorkerInstance.isEmpty()) {
                if (!earlyWaitingVisible) {
                    setPlankReconnectStatus("Waiting for workstation...", false);
                    earlyWaitingVisible = true;
                }
                if (workerProbe == nullptr && now >= nextWorkerProbe) {
                    workerProbe = new PlankWorkerProbeThread(m_Computer->activeAddress,
                                    m_PlankWorkerInstance, m_PlankHostCertificateSha256);
                    workerProbe->start();
                }
            } else if (earlyWaitingVisible) {
                setPlankReconnectStatus("", false);
                earlyWaitingVisible = false;
            }
        } else {
            earlyWaitingVisible = false;
        }
        if (m_PlankToolbar) {
            m_PlankToolbar->setRenderedStats(
                        m_CurrentRenderedFps.load(std::memory_order_relaxed),
                        m_CurrentVideoMbps.load(std::memory_order_relaxed),
                        currentVideoFecLoss().before,
                        currentNetworkRttMs());
            const auto action = m_PlankToolbar->update(
                        SDL_GetTicks(), !m_Reconnecting.load());
            if (action == PlankToolbar::Action::Disconnect) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "PLANK toolbar disconnect requested");
                goto DispatchDeferredCleanup;
            }
            if (action == PlankToolbar::Action::KeepWaiting) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "PLANK unreachable-host prompt requested continued retries");
                m_PlankToolbar->hideReconnectPrompt();
                reconnectDecisionDeadline = SDL_GetTicks() +
                        static_cast<Uint64>(m_Preferences->plankUnreachableTimeoutSeconds) * 1000;
                m_ReconnectPolicy.allowUntil(reconnectDecisionDeadline);
                setPlankReconnectStatus("Waiting for workstation...", false);
            }
            if (action == PlankToolbar::Action::ToggleFullscreen) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "PLANK toolbar fullscreen toggle requested");
                toggleFullscreen();
                m_PlankToolbar->notifyWindowChanged();
            } else if (action == PlankToolbar::Action::Minimize) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "PLANK toolbar minimize requested");
                minimizePresentationWindows();
            }
        }

        // The old desktop worker may lose Xorg before it can send a logout
        // notice. Use the replacement worker's authenticated stage instead.
        // Apply only on the SDL thread and never override an expired timeout.
        if (m_ReconnectGreeterConfirmed.exchange(false) && m_Reconnecting.load() &&
                reconnectDecisionDeadline != 0 && SDL_GetTicks() < reconnectDecisionDeadline) {
            setPlankReconnectStatus("Returning to the sign-in screen...", false);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Authenticated Host confirmed return to the sign-in screen");
        }

        if (m_Reconnecting.load() && reconnectDecisionDeadline != 0 &&
                (reconnectThread == nullptr || !reconnectThread->isFinished()) &&
                SDL_GetTicks() >= reconnectDecisionDeadline) {
            setPlankReconnectStatus("Workstation is taking longer to respond...", true);
            if (m_Preferences->plankUnreachableAction ==
                    StreamingPreferences::PLANK_UNREACHABLE_DISCONNECT) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "PLANK host remained unreachable for %d seconds; disconnecting automatically",
                            m_Preferences->plankUnreachableTimeoutSeconds);
                goto DispatchDeferredCleanup;
            }

            m_PlankToolbar->showReconnectPrompt(
                        m_Preferences->plankUnreachableTimeoutSeconds);
            reconnectDecisionDeadline = 0;
        }
        int eventWaitTimeout = m_Reconnecting.load() ? 50 :
                    (m_PlankToolbar ?
                         m_PlankToolbar->eventWaitTimeout() : 1000);
#ifdef Q_OS_MACOS
        // Native Quit changes explicit session state, not the SDL event queue.
        // Bound the wait even when a stalled Host produces no events.
        eventWaitTimeout = std::min(eventWaitTimeout, 50);
#endif
        if (!SDL_WaitEventTimeout(&event, eventWaitTimeout)) {
            if (reconnectThread != nullptr &&
                    reconnectThread->isFinished() &&
                    !reconnectThread->completionPosted()) {
                event = {};
                event.type = SDL_EVENT_USER;
                event.user.code = SDL_CODE_PLANK_REPLANK_COMPLETE;
            } else {
                continue;
            }
        }

#ifdef Q_OS_MACOS
        if (m_ApplicationExitRequested.load()) {
            goto DispatchDeferredCleanup;
        }
#endif
        const bool reconnectCompletion =
                event.type == SDL_EVENT_USER &&
                event.user.code == SDL_CODE_PLANK_REPLANK_COMPLETE;
#ifdef Q_OS_MACOS
        if (m_InputHandler->handleCapturedMacKeyEvent(event)) {
            continue;
        }
#endif
        if (m_Reconnecting.load() &&
                event.type != SDL_EVENT_QUIT && !reconnectCompletion) {
            // Cursor shapes, host-authoritative Wacom positions, and toolbar
            // bitrate confirmation are local presentation events. The new
            // transport can deliver them before its reconnect worker posts
            // completion. Apply them now so their one-shot pending latches do
            // not remain set after this event is consumed.
            if (event.type == SDL_EVENT_USER &&
                    handlePlankLocalUserEvent(event.user)) {
                continue;
            }

            // Keep the local window, hotkeys, and toolbar alive while the
            // transport worker retries. Never forward these events to a host
            // whose input connection has already stopped.
            switch (event.type) {
            case SDL_EVENT_MOUSE_MOTION:
                if (m_PlankToolbar &&
                        event.motion.windowID == SDL_GetWindowID(m_Window)) {
                    m_PlankToolbar->observeMouseMotion(event.motion);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (m_PlankToolbar &&
                        event.button.windowID == SDL_GetWindowID(m_Window)) {
                    const auto action =
                            m_PlankToolbar->handleMouseButton(event.button);
                    if (action == PlankToolbar::Action::Disconnect) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "PLANK toolbar disconnect requested during reconnect");
                        goto DispatchDeferredCleanup;
                    }
                    if (action == PlankToolbar::Action::ToggleFullscreen) {
                        toggleFullscreen();
                        m_PlankToolbar->notifyWindowChanged();
                    } else if (action == PlankToolbar::Action::Minimize) {
                        minimizePresentationWindows();
                    }
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (m_PlankToolbar &&
                        event.wheel.windowID == SDL_GetWindowID(m_Window)) {
                    m_PlankToolbar->handleMouseWheel(event.wheel);
                }
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                // Retain PLANK's local Ctrl+Alt+Shift hotkeys. Any
                // ordinary key events are harmless because LiStopConnection()
                // has already closed the remote input channel.
                m_InputHandler->handleKeyEvent(&event.key);
                break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                if (m_PlankToolbar) {
                    m_PlankToolbar->notifyWindowChanged();
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                if (!anyPresentationWindowFocused()) {
                    if (m_PlankToolbar) {
                        m_PlankToolbar->notifyFocusLost();
                    }
                    m_InputHandler->notifyFocusLost();
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                m_InputHandler->notifyFocusGained();
#ifdef Q_OS_MACOS
                queueClipboardPollEvent();
#endif
                break;
            default:
                break;
            }
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_QUIT:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Quit event received");
            goto DispatchDeferredCleanup;

        case SDL_EVENT_USER:
            if (handlePlankLocalUserEvent(event.user)) {
                break;
            }
            switch (event.user.code) {
            case SDL_CODE_PLANK_RECONNECT:
                if (reconnectThread != nullptr ||
                        !beginPlankReconnect(reconnectState)) {
                    emit displayLaunchError(
                                tr("The workstation desktop changed, but the client could not start reconnecting."));
                    goto DispatchDeferredCleanup;
                }
                reconnectDecisionDeadline = SDL_GetTicks() +
                        static_cast<Uint64>(m_Preferences->plankUnreachableTimeoutSeconds) * 1000;
                m_ReconnectPolicy.allowUntil(reconnectDecisionDeadline);
                reconnectThread = new PlankReconnectThread(this);
                reconnectThread->start();
                break;
            case SDL_CODE_PLANK_REPLANK_COMPLETE:
            {
                if (reconnectThread == nullptr) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Ignoring reconnect completion without an active worker");
                    break;
                }
                reconnectThread->wait();
                const bool reconnectSucceeded = reconnectThread->succeeded();
                delete reconnectThread;
                reconnectThread = nullptr;
                reconnectDecisionDeadline = 0;
                if (m_PlankToolbar) {
                    m_PlankToolbar->hideReconnectPrompt();
                }
                if (!finishPlankReconnect(
                            reconnectSucceeded, reconnectState)) {
                    if (m_LogoutReturnedToLogin.load()) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "Returning to the sign-in screen after logout");
                        m_UnexpectedTermination = false;
                        emit displayLaunchError(
                                    tr("You have logged out of the workstation."));
                    } else {
                        emit displayLaunchError(
                                    tr("The workstation desktop changed, but the client could not reconnect."));
                    }
                    goto DispatchDeferredCleanup;
                }
                break;
            }
            case SDL_CODE_FRAME_READY:
                if (m_VideoDecoder != nullptr) {
                    m_VideoDecoder->renderFrameOnMainThread();
                }
                break;
            case SDL_CODE_FLUSH_WINDOW_EVENT_BARRIER:
                m_FlushingWindowEventsRef--;
                break;
            default:
                SDL_assert(false);
            }
            break;

        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            if (m_PlankToolbar) {
                m_PlankToolbar->notifyWindowChanged();
            }
            break;

#ifdef Q_OS_DARWIN
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            if (SDL_Window* window = windowForEvent(event.window.windowID)) {
                MacWindow::logGeometry(window);
                m_InputHandler->updateKeyboardGrabState();
                if (m_PlankToolbar) {
                    m_PlankToolbar->notifyWindowChanged();
                }
            }
            break;
#endif

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
        {
            SDL_Window* eventWindow = windowForEvent(event.window.windowID);
            if (eventWindow == nullptr) {
                break;
            }
#ifdef Q_OS_DARWIN
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                MacWindow::logGeometry(eventWindow);
            }
#endif
            if (m_PlankToolbar && eventWindow == m_Window &&
                    event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                m_PlankToolbar->notifyWindowChanged();
            }
            // Early handling of some events
            switch (event.type) {
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                if (!anyPresentationWindowFocused()) {
                    if (m_PlankToolbar) {
                        m_PlankToolbar->notifyFocusLost();
                    }
                    if (m_Preferences->muteOnFocusLoss) {
                        m_AudioMuted = true;
                    }
                    m_InputHandler->notifyFocusLost();
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                if (m_Preferences->muteOnFocusLoss) {
                    m_AudioMuted = false;
                }
                m_InputHandler->notifyFocusGained();
#ifdef Q_OS_MACOS
                queueClipboardPollEvent();
#endif
                break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                m_InputHandler->notifyMouseLeave();
                break;
            }


            // Capture the mouse on SDL_EVENT_WINDOW_MOUSE_ENTER if needed
            if (needsFirstEnterCapture && event.type == SDL_EVENT_WINDOW_MOUSE_ENTER) {
                m_InputHandler->setCaptureActive(true);
                needsFirstEnterCapture = false;
            }

            // Secondary Vulkan swapchains resize themselves during the next
            // frame. Only the primary window participates in decoder and
            // toolbar lifecycle decisions.
            if (eventWindow != m_Window) {
                break;
            }

            // We want to recreate the decoder for resizes (full-screen toggles) and the initial shown event.
            // We use SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED rather than SDL_EVENT_WINDOW_RESIZED because the latter doesn't
            // seem to fire when switching from windowed to full-screen on X11.
            if (event.type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
                (event.type != SDL_EVENT_WINDOW_SHOWN || m_VideoDecoder != nullptr)) {
                // Check that the window display hasn't changed. If it has, we want
                // to recreate the decoder to allow it to adapt to the new display.
                // This will allow Pacer to pull the new display refresh rate.
                if (event.type != SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
                    break;
                }
            }
#ifdef Q_OS_WIN32
            // We can get a resize event after being minimized. Recreating the renderer at that time can cause
            // us to start drawing on the screen even while our window is minimized. Minimizing on Windows also
            // moves the window to -32000, -32000 which can cause a false window display index change. Avoid
            // that whole mess by never recreating the decoder if we're minimized.
            else if (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MINIMIZED) {
                break;
            }
#endif

            if (m_FlushingWindowEventsRef > 0) {
                // Ignore window events for renderer reset if flushing
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Dropping window event during flush: %d (%d %d)",
                            event.type,
                            event.window.data1,
                            event.window.data2);
                break;
            }

            // Allow the renderer to handle the state change without being recreated
            if (m_VideoDecoder) {
                bool forceRecreation = false;

                WINDOW_STATE_CHANGE_INFO windowChangeInfo = {};
                windowChangeInfo.window = m_Window;

                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                    windowChangeInfo.stateChangeFlags |= WINDOW_STATE_CHANGE_SIZE;

                    windowChangeInfo.width = event.window.data1;
                    windowChangeInfo.height = event.window.data2;
                }

                const SDL_DisplayID newDisplayId = SDL_GetDisplayForWindow(m_Window);
                if (newDisplayId != currentDisplayId) {
                    windowChangeInfo.stateChangeFlags |= WINDOW_STATE_CHANGE_DISPLAY;

                    windowChangeInfo.displayId = newDisplayId;

                    // If the refresh rates have changed, we will need to go through the full
                    // decoder recreation path to ensure Pacer is switched to the new display
                    // and that we apply any V-Sync disablement rules that may be needed for
                    // this display.
                    const SDL_DisplayMode* oldMode = SDL_GetCurrentDisplayMode(currentDisplayId);
                    const SDL_DisplayMode* newMode = SDL_GetCurrentDisplayMode(newDisplayId);
                    if (oldMode == nullptr || newMode == nullptr ||
                            oldMode->refresh_rate != newMode->refresh_rate) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "Forcing renderer recreation due to refresh rate change between displays");
                        forceRecreation = true;
                    }
                }

                if (!forceRecreation && m_VideoDecoder->notifyWindowChanged(&windowChangeInfo)) {
                    // Update the window display mode based on our current monitor
                    // NB: Avoid a useless modeset by only doing this if it changed.
                    if (newDisplayId != currentDisplayId) {
                        currentDisplayId = newDisplayId;
                        updateOptimalWindowDisplayMode();
                    }

                    break;
                }
            }

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Recreating renderer for window event: %d (%d %d)",
                        event.type,
                        event.window.data1,
                        event.window.data2);
            SDL_Event resetEvent = {};
            resetEvent.type = SDL_EVENT_RENDER_DEVICE_RESET;
            SDL_PushEvent(&resetEvent);
            break;
        }

        case SDL_EVENT_RENDER_DEVICE_RESET:
            if (presentationMappingDeferred && m_VideoDecoder == nullptr) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Initializing renderer before first Wayland presentation map");
            } else if (event.type == SDL_EVENT_RENDER_DEVICE_RESET) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Recreating renderer by internal request: %d",
                            event.type);
            }

            SDL_LockSpinlock(&m_DecoderLock);

            // Destroy the old decoder
            delete m_VideoDecoder;

            // Insert a barrier to discard any additional window events
            // that could cause the renderer to be and recreated again.
            // We don't use SDL_FlushEvent() here because it could cause
            // important events to be lost.
            flushWindowEvents();

            // Update the window display mode based on our current monitor
            // NB: Avoid a useless modeset by only doing this if it changed.
            if (currentDisplayId != SDL_GetDisplayForWindow(m_Window)) {
                currentDisplayId = SDL_GetDisplayForWindow(m_Window);
                updateOptimalWindowDisplayMode();
            }

            // Now that the old decoder is dead, flush any events it may
            // have queued to reset itself (if this reset was the result
            // of state loss).
            SDL_PumpEvents();
            SDL_FlushEvent(SDL_EVENT_RENDER_DEVICE_RESET);

            {
                // If the stream exceeds the display refresh rate (plus some slack),
                // forcefully disable V-sync to allow the stream to render faster
                // than the display.
                int displayHz = StreamUtils::getDisplayRefreshRate(m_Window);
                bool enableVsync = m_Preferences->enableVsync;
                if (displayHz + 5 < m_StreamConfig.fps) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Disabling V-sync because refresh rate limit exceeded");
                    enableVsync = false;
                }

                // Choose a new decoder (hopefully the same one, but possibly
                // not if a GPU was removed or something).
                if (!chooseDecoder(DecoderSelectionMode::PreferExactHardwareThenSoftware,
                                   m_Window, m_ActiveVideoFormat, m_ActiveVideoWidth,
                                   m_ActiveVideoHeight, m_ActiveVideoFrameRate,
                                   enableVsync, false,
                                   s_ActiveSession->m_VideoDecoder,
                                   isIdentityGbrEnabledForFormat(m_ActiveVideoFormat),
                                   decoderCaptureSource(), decoderEncoderBackend())) {
                    SDL_UnlockSpinlock(&m_DecoderLock);
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "Failed to recreate decoder after reset");
                    emit displayLaunchError(tr("Unable to initialize video decoder. Please check your streaming settings and try again."));
                    goto DispatchDeferredCleanup;
                }

                // As of SDL 2.0.12, SDL_RecreateWindow() doesn't carry over mouse capture
                // or mouse hiding state to the new window. By capturing after the decoder
                // is set up, this ensures the window re-creation is already done.
                if (needsPostDecoderCreationCapture) {
                    m_InputHandler->setCaptureActive(true);
                    needsPostDecoderCreationCapture = false;
                }
            }

            if (presentationMappingDeferred) {
                int logicalWidth = 0;
                int logicalHeight = 0;
                SDL_GetWindowSize(m_Window, &logicalWidth, &logicalHeight);
                if (logicalWidth <= 0 || logicalHeight <= 0) {
                    SDL_UnlockSpinlock(&m_DecoderLock);
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "Refusing to map invalid Wayland presentation geometry: %dx%d",
                                 logicalWidth, logicalHeight);
                    emit displayLaunchError(
                                tr("Unable to display the streaming window with valid dimensions."));
                    goto DispatchDeferredCleanup;
                }
                if (!SDL_SetWindowFullscreenMode(m_Window, nullptr) ||
                        !SDL_SetWindowFullscreen(m_Window, true)) {
                    SDL_UnlockSpinlock(&m_DecoderLock);
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "Failed to queue initial Wayland fullscreen state: %s",
                                 SDL_GetError());
                    emit displayLaunchError(
                                tr("Unable to enter fullscreen mode for the streaming window."));
                    goto DispatchDeferredCleanup;
                }
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Mapping initialized Wayland presentation at %dx%d with fullscreen pending",
                            logicalWidth, logicalHeight);
                if (!SDL_ShowWindow(m_Window)) {
                    SDL_UnlockSpinlock(&m_DecoderLock);
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "Failed to map initialized Wayland presentation: %s",
                                 SDL_GetError());
                    emit displayLaunchError(
                                tr("Unable to display the initialized streaming window."));
                    goto DispatchDeferredCleanup;
                }
                if (!SDL_SyncWindow(m_Window)) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Timed out synchronizing initial Wayland presentation map: %s",
                                SDL_GetError());
                }
                presentationMappingDeferred = false;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Mapped initialized Wayland presentation surface");
                initializePlankToolbar();
            }

            // Request an IDR frame to complete the reset
            LiRequestIdrFrame();

            // Set HDR mode. We may miss the callback if we're in the middle
            // of recreating our decoder at the time the HDR transition happens.
            m_VideoDecoder->setHdrMode(LiGetCurrentHostDisplayHdrMode());

            // The replacement renderer has no copy of the prior overlay
            // texture, so publish the toolbar surface again after recreation.
            if (m_PlankToolbar) {
                m_PlankToolbar->notifyWindowChanged();
            }

            // After a window resize, we need to reset the pointer lock region
            m_InputHandler->updatePointerRegionLock();

            SDL_UnlockSpinlock(&m_DecoderLock);
            break;

        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_KEY_DOWN:
            m_InputHandler->handleKeyEvent(&event.key);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (m_PlankToolbar &&
                    event.button.windowID == SDL_GetWindowID(m_Window)) {
                const auto action = m_PlankToolbar->handleMouseButton(event.button);
                if (action == PlankToolbar::Action::Disconnect) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "PLANK toolbar disconnect requested");
                    goto DispatchDeferredCleanup;
                }
                if (action == PlankToolbar::Action::ToggleFullscreen) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "PLANK toolbar fullscreen toggle requested");
                    toggleFullscreen();
                    m_PlankToolbar->notifyWindowChanged();
                    break;
                }
                if (action == PlankToolbar::Action::Minimize) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "PLANK toolbar minimize requested");
                    minimizePresentationWindows();
                    break;
                }
                if (action == PlankToolbar::Action::Consumed) {
                    break;
                }
            }
            m_InputHandler->handleMouseButtonEvent(&event.button);
            break;
        case SDL_EVENT_MOUSE_MOTION:
        {
            bool toolbarConsumedMotion = false;
            if (m_PlankToolbar &&
                    event.motion.windowID == SDL_GetWindowID(m_Window)) {
                // The ordinary input path batches queued motion for efficient
                // transport. Aggregate it here when the toolbar is present so
                // the toolbar tracker and host receive the identical delta.
                if (event.motion.which != SDL_TOUCH_MOUSEID) {
                    PlankMouseMotion::coalescePending(event.motion);
                }
                // The single-window toolbar observes the same authoritative
                // coordinates, but motion always remains remote-desktop input.
                // Only toolbar button and wheel events have exclusive local
                // ownership.
                toolbarConsumedMotion =
                        m_PlankToolbar->observeMouseMotion(event.motion);
            }
            if (!toolbarConsumedMotion) {
                m_InputHandler->handleMouseMotionEvent(
                            &event.motion, !m_PlankToolbar);
            }
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            if (m_PlankToolbar &&
                    event.wheel.windowID == SDL_GetWindowID(m_Window) &&
                    m_PlankToolbar->handleMouseWheel(event.wheel)) {
                break;
            }
            m_InputHandler->handleMouseWheelEvent(&event.wheel);
            break;
        case SDL_EVENT_PEN_PROXIMITY_IN:
        case SDL_EVENT_PEN_PROXIMITY_OUT:
        case SDL_EVENT_PEN_DOWN:
        case SDL_EVENT_PEN_UP:
        case SDL_EVENT_PEN_BUTTON_DOWN:
        case SDL_EVENT_PEN_BUTTON_UP:
        case SDL_EVENT_PEN_MOTION:
        case SDL_EVENT_PEN_AXIS:
            m_InputHandler->handlePenEvent(event);
            break;
        }
    }

DispatchDeferredCleanup:
    if (workerProbe != nullptr) {
        // The probe has a one-second HTTP deadline and owns no Session state.
        SDL_HideWindow(m_Window);
        workerProbe->wait();
        delete workerProbe;
        workerProbe = nullptr;
    }
    // Uncapture the mouse and hide the window immediately,
    // so we can return to the Qt GUI ASAP.
    if (reconnectThread != nullptr) {
        SDL_HideWindow(m_Window);
        for (SDL_Window* window : std::as_const(m_SecondaryWindows)) {
            SDL_HideWindow(window);
        }
        m_ReconnectCancelled.store(true);
        m_ConnectionStartCancelled.store(true);
        reconnectThread->wait();
        delete reconnectThread;
        reconnectThread = nullptr;
        m_Reconnecting.store(false);
        m_ReconnectRequested.store(false);
    }
    m_InputHandler->setCaptureActive(false);
    SDL_EnableScreenSaver();
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "0");
    if (QGuiApplication::platformName() == "eglfs") {
        QGuiApplication::restoreOverrideCursor();
    }

    // Raise any keys that are still down
    m_InputHandler->raiseAllKeys();

    // The toolbar owns a reference to the input handler, so destroy it before
    // releasing the handler itself.
    m_PlankToolbar.reset();

    // Destroy the input handler now. This must be destroyed
    // before allowing the UI to continue execution.
    delete m_InputHandler;
    m_InputHandler = nullptr;
    clearPlankReconnectCredentials();
    {
        QWriteLocker lock(&m_Computer->lock);
        m_Computer->sessionToken.fill(QChar('\0'));
        m_Computer->sessionToken.clear();
        m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
    }

#ifdef PLANK_TRANSPORT
    // Native media threads call directly into the active decoder and audio
    // renderer. Quiesce them before either renderer can be destroyed.
    stopPlankTransportMediaReceivers();
#endif

    // Destroy the decoder, since this must be done on the main thread
    // NB: This must happen before LiStopConnection() for pull-based
    // decoders.
    SDL_LockSpinlock(&m_DecoderLock);
    delete m_VideoDecoder;
    m_VideoDecoder = nullptr;
    SDL_UnlockSpinlock(&m_DecoderLock);

    // Propagate state changes from the SDL window back to the Qt window
    //
    // NB: We're making a conscious decision not to propagate the maximized
    // or normal state of the window here. The thinking is that users may
    // routinely maximize the streaming window simply to view the stream
    // in a larger window, but they don't necessarily want the UI in such
    // a large window.
    if (!m_IsFullScreen && m_QtWindow != nullptr && m_Window != nullptr) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        if (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MINIMIZED) {
            m_QtWindow->setWindowStates(m_QtWindow->windowStates() | Qt::WindowMinimized);
        }
        else if (m_QtWindow->windowStates() & Qt::WindowMinimized) {
            m_QtWindow->setWindowStates(m_QtWindow->windowStates() & ~Qt::WindowMinimized);
        }
#else
        if (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MINIMIZED) {
            m_QtWindow->setWindowState(Qt::WindowMinimized);
        }
        else if (m_QtWindow->windowState() & Qt::WindowMinimized) {
            m_QtWindow->setWindowState(Qt::WindowNoState);
        }
#endif
    }

    // This must be called after the decoder is deleted, because
    // the renderer may want to interact with the window
    for (SDL_Window* window : std::as_const(m_SecondaryWindows)) {
        SDL_DestroyWindow(window);
    }
    m_SecondaryWindows.clear();
    SDL_DestroyWindow(m_Window);
    m_Window = nullptr;

    if (iconSurface != nullptr) {
        SDL_DestroySurface(iconSurface);
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);

    // Cleanup can take a while, so dispatch it to a worker thread.
    // When it is complete, it will release our s_ActiveSessionSemaphore
    // reference.
    QThreadPool::globalInstance()->start(new DeferredSessionCleanupTask(this));
}
