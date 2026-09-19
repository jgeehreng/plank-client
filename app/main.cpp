#include <QGuiApplication>
#include <QStyleHints>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QIcon>
#include <QQuickStyle>
#include <QMutex>
#include <QtDebug>
#include <QNetworkProxyFactory>
#include <QPalette>
#include <QFont>
#include <QCursor>
#include <QDir>
#include <QDateTime>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QRegularExpression>

#ifdef Q_OS_UNIX
#include <sys/socket.h>
#include <signal.h>
#endif

// Don't let SDL hook our main function, since Qt is already
// doing the same thing. This needs to be before any headers
// that might include SDL.h themselves.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#ifdef Q_OS_MACOS
#include "macapplication.h"
#include "streaming/mackeyboardcapture.h"
#endif

#ifdef HAVE_FFMPEG
#include "streaming/video/ffmpeg.h"
#endif

#if defined(Q_OS_WIN32)
#include "antihookingprotection.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dxgi1_6.h>
#elif defined(Q_OS_LINUX)
#include <openssl/ssl.h>
#endif

#include "cli/startstream.h"
#include "cli/commandlineparser.h"
#include "path.h"
#include "utils.h"
#include "gui/computermodel.h"
#include "backend/computermanager.h"
#include <QSslSocket>
#include "backend/systemproperties.h"
#include "streaming/session.h"
#include "settings/streamingpreferences.h"

#ifdef PLANK_TRANSPORT
#include <plank_transport.h>
#endif

#if defined(Q_OS_WIN32)
#define IS_UNSPECIFIED_HANDLE(x) ((x) == INVALID_HANDLE_VALUE || (x) == NULL)

// Log to file or console dynamically for Windows builds
#define LOG_TO_FILE
#elif defined(Q_OS_LINUX)
// Retain a private per-user log without duplicating routine output to journald
#define LOG_TO_FILE
#elif !defined(QT_DEBUG) && defined(Q_OS_DARWIN)
// Log to file for release Mac builds
#define LOG_TO_FILE
#else
// Log to console for debug Mac builds
#endif

// StreamUtils::setAsyncLogging() exposes control of this to the Session
// class to enable async logging once the stream has started.
//
// FIXME: Clean this up
QAtomicInt g_AsyncLoggingEnabled;

static QTextStream s_LoggerStream(stderr);
static QThreadPool s_LoggerThread;
static QMutex s_SyncLoggerMutex;
#ifdef LOG_TO_FILE
// Max log file size of 10 MB
#define MAX_LOG_SIZE_BYTES (10 * 1024 * 1024)
static int s_LogBytesWritten = 0;
static bool s_LogLimitReached = false;
static QFile* s_LoggerFile;
static QTextStream s_LoggerFileStream;
#endif

#ifdef HAVE_DRM_MASTER_HOOKS
extern "C" bool g_DisableDrmHooks;
#endif

static QString currentLogTimestamp()
{
    const QDateTime localTime = QDateTime::currentDateTime();
    return localTime.toOffsetFromUtc(localTime.offsetFromUtc()).toString(Qt::ISODateWithMs);
}

class LoggerTask : public QRunnable
{
public:
    LoggerTask(const QString& msg) : m_Msg(msg)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        // QTextStream is not thread-safe, so we must lock. This will generally
        // only contend in synchronous logging mode or during a transition
        // between synchronous and asynchronous. Asynchronous won't contend in
        // the common case because we only have a single logging thread.
        QMutexLocker locker(&s_SyncLoggerMutex);
        s_LoggerStream << m_Msg;
        s_LoggerStream.flush();
    }

private:
    QString m_Msg;
};

void logToLoggerStream(QString& message)
{
    QMutexLocker locker(&s_SyncLoggerMutex);

#if defined(QT_DEBUG) && defined(Q_OS_WIN32)
    // Output log messages to a debugger if attached
    if (IsDebuggerPresent()) {
        thread_local QString lineBuffer;
        lineBuffer += message;
        if (message.endsWith('\n')) {
            OutputDebugStringW(lineBuffer.toStdWString().c_str());
            lineBuffer.clear();
        }
    }
#endif

    // Strip session encryption keys and IVs from the logs

#ifdef LOG_TO_FILE
    if (s_LoggerFileStream.device() != nullptr && !s_LogLimitReached) {
        const int messageBytes = message.toUtf8().size();
        if (s_LogBytesWritten + messageBytes > MAX_LOG_SIZE_BYTES) {
            const QString limitMessage = "Log size limit reached!\n";
            if (s_LogBytesWritten + limitMessage.toUtf8().size() <= MAX_LOG_SIZE_BYTES) {
                s_LoggerFileStream << limitMessage;
                s_LoggerFileStream.flush();
            }
            s_LogLimitReached = true;
        }
        else {
            s_LogBytesWritten += messageBytes;
            s_LoggerFileStream << message;
            s_LoggerFileStream.flush();
        }
    }
#endif

#if !defined(LOG_TO_FILE)
    s_LoggerStream << message;
    s_LoggerStream.flush();
#elif defined(LOG_TO_FILE)
    // Preserve the console fallback if the file could not be opened.
    if (s_LoggerFileStream.device() == nullptr) {
        s_LoggerStream << message;
        s_LoggerStream.flush();
    }
#endif
}

void sdlLogToDiskHandler(void*, int category, SDL_LogPriority priority, const char* message)
{
    QString priorityTxt;

    switch (priority) {
    case SDL_LOG_PRIORITY_VERBOSE:
        priorityTxt = "Verbose";
        break;
    case SDL_LOG_PRIORITY_DEBUG:
        priorityTxt = "Debug";
        break;
    case SDL_LOG_PRIORITY_INFO:
        priorityTxt = "Info";
        break;
    case SDL_LOG_PRIORITY_WARN:
        priorityTxt = "Warn";
        break;
    case SDL_LOG_PRIORITY_ERROR:
        priorityTxt = "Error";
        break;
    case SDL_LOG_PRIORITY_CRITICAL:
        priorityTxt = "Critical";
        break;
    default:
        priorityTxt = "Unknown";
        break;
    }

    QString txt = QString("%1 - SDL %2 (%3): %4\n").arg(currentLogTimestamp()).arg(priorityTxt).arg(category).arg(message);

    logToLoggerStream(txt);
}

void qtLogToDiskHandler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    QString typeTxt;

    switch (type) {
    case QtDebugMsg:
        typeTxt = "Debug";
        break;
    case QtInfoMsg:
        typeTxt = "Info";
        break;
    case QtWarningMsg:
        typeTxt = "Warning";
        break;
    case QtCriticalMsg:
        typeTxt = "Critical";
        break;
    case QtFatalMsg:
        typeTxt = "Fatal";
        break;
    }

    QString txt = QString("%1 - Qt %2: %3\n").arg(currentLogTimestamp()).arg(typeTxt).arg(msg);

    logToLoggerStream(txt);
}

#ifdef HAVE_FFMPEG

void ffmpegLogToDiskHandler(void* ptr, int level, const char* fmt, va_list vl)
{
    char lineBuffer[1024];
    static int printPrefix = 1;

    if ((level & 0xFF) > av_log_get_level()) {
        return;
    }
    // We need to use the *previous* printPrefix value to determine whether to
    // print the prefix this time. av_log_format_line() will set the printPrefix
    // value to indicate whether the prefix should be printed *next time*.
    bool shouldPrefixThisMessage = printPrefix != 0;

    av_log_format_line(ptr, level, fmt, vl, lineBuffer, sizeof(lineBuffer), &printPrefix);

    if (shouldPrefixThisMessage) {
        QString txt = QString("%1 - FFmpeg: %2").arg(currentLogTimestamp()).arg(lineBuffer);
        logToLoggerStream(txt);
    }
    else {
        QString txt = QString(lineBuffer);
        logToLoggerStream(txt);
    }
}

#endif

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>

static UINT s_HitUnhandledException = 0;

LONG WINAPI UnhandledExceptionHandler(struct _EXCEPTION_POINTERS *ExceptionInfo)
{
    // Only write a dump for the first unhandled exception
    if (InterlockedCompareExchange(&s_HitUnhandledException, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    WCHAR dmpFileName[MAX_PATH];
    swprintf_s(dmpFileName, L"%ls\\PLANK-%I64u.dmp",
               (PWCHAR)QDir::toNativeSeparators(Path::getLogDir()).utf16(), QDateTime::currentSecsSinceEpoch());
    QString qDmpFileName = QString::fromUtf16((const char16_t*)dmpFileName);
    HANDLE dumpHandle = CreateFileW(dmpFileName, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dumpHandle != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info;

        info.ThreadId = GetCurrentThreadId();
        info.ExceptionPointers = ExceptionInfo;
        info.ClientPointers = FALSE;

        DWORD typeFlags = MiniDumpWithIndirectlyReferencedMemory |
                MiniDumpIgnoreInaccessibleMemory |
                MiniDumpWithUnloadedModules |
                MiniDumpWithThreadInfo;

        if (MiniDumpWriteDump(GetCurrentProcess(),
                               GetCurrentProcessId(),
                               dumpHandle,
                               (MINIDUMP_TYPE)typeFlags,
                               &info,
                               nullptr,
                               nullptr)) {
            qCritical() << "Unhandled exception! Minidump written to:" << qDmpFileName;
        }
        else {
            qCritical() << "Unhandled exception! Failed to write dump:" << GetLastError();
        }

        CloseHandle(dumpHandle);
    }
    else {
        qCritical() << "Unhandled exception! Failed to open dump file:" << qDmpFileName << "with error" << GetLastError();
    }

    // Sleep for a moment to allow the logging thread to finish up before crashing
    if (g_AsyncLoggingEnabled) {
        Sleep(500);
    }

    // Let the program crash and WER collect a dump
    return EXCEPTION_CONTINUE_SEARCH;
}

#endif

#ifdef Q_OS_UNIX

static int signalFds[2];

void handleSignal(int sig)
{
    send(signalFds[0], &sig, sizeof(sig), 0);
}

int SDLCALL signalHandlerThread(void* data)
{
    Q_UNUSED(data);

    bool requestedQuit = false;

    int sig;
    while (recv(signalFds[1], &sig, sizeof(sig), MSG_WAITALL) == sizeof(sig)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Received signal: %d", sig);

        switch (sig) {
        case SIGINT:
        case SIGTERM:
        {
            if (requestedQuit) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Exiting immediately on second signal");
                _Exit(1);
            }

            // Route termination through the normal client disconnect path.
            SDL_Event event = {};
            event.type = SDL_EVENT_QUIT;
            event.quit.timestamp = SDL_GetTicksNS();
            SDL_PushEvent(&event);
            requestedQuit = true;
            break;
        }

        default:
            Q_UNREACHABLE();
        }
    }

    return 0;
}

void configureSignalHandlers()
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, signalFds) == -1) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "socketpair() failed: %d",
                     errno);
        return;
    }

    // Create a thread to handle our signals safely outside of signal context
    SDL_Thread* thread = SDL_CreateThread(signalHandlerThread, "Signal Handler", nullptr);
    SDL_DetachThread(thread);

    struct sigaction sa = {};
    sa.sa_handler = handleSignal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

#endif

int main(int argc, char *argv[])
{
    SDL_SetMainReady();

    // Set the SDL3 application identity before any subsystem can initialize.
    // On Wayland, GNOME uses this ID to match the stream window to our desktop
    // entry and persist the user's keyboard-shortcut inhibitor decision.
    SDL_SetAppMetadata("PLANK Client",
                       PLANK_VERSION_STR,
                       "la.instinctual.Plank.Client");

    // Set the app version for the QCommandLineParser's showVersion() command
    QCoreApplication::setApplicationVersion(PLANK_VERSION_STR);

    // Set these here to allow us to use the default QSettings constructor and
    // establish the PLANK settings/cache namespace before paths are
    // initialized. There are no deployed legacy clients requiring migration.
    QCoreApplication::setOrganizationName("Instinctual");
    QCoreApplication::setOrganizationDomain("instinctual.la");
    QCoreApplication::setApplicationName("PLANK");

    if (QFile(QDir::currentPath() + "/portable.dat").exists()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QDir::currentPath());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, QDir::currentPath());

        // Initialize paths for portable mode
        Path::initialize(true);
    }
    else {
        // Initialize paths for standard installation
        Path::initialize(false);
    }

    // Override the default QML cache directory with the one we chose
    if (qEnvironmentVariableIsEmpty("QML_DISK_CACHE_PATH")) {
        qputenv("QML_DISK_CACHE_PATH", Path::getQmlCacheDir().toUtf8());
    }

#ifdef Q_OS_WIN32
    // Grab the original std handles before we potentially redirect them later
    HANDLE oldConOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE oldConErr = GetStdHandle(STD_ERROR_HANDLE);
#endif

#ifdef LOG_TO_FILE
    QDir logDir(Path::getLogDir());
    QString logNamePattern;

#if defined(Q_OS_LINUX) || defined(Q_OS_DARWIN)
    const QFileDevice::Permissions privateDirectoryPermissions =
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    bool logDirectoryReady = logDir.exists() || logDir.mkpath(".", privateDirectoryPermissions);
    if (logDirectoryReady) {
        logDirectoryReady = QFile::setPermissions(logDir.absolutePath(), privateDirectoryPermissions);
    }
    if (!logDirectoryReady) {
        QTextStream(stderr) << "Unable to create private log directory: " << logDir.absolutePath() << Qt::endl;
    }
    logNamePattern = "plank-client-*.log";
    const QString logFileName = QString("plank-client-%1-%2.log")
                                    .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz"))
                                    .arg(QCoreApplication::applicationPid());
#else
    logNamePattern = "PLANK-*.log";
    const QString logFileName = QString("PLANK-%1.log").arg(QDateTime::currentSecsSinceEpoch());
#endif

#ifdef Q_OS_WIN32
    // Only log to a file if the user didn't redirect stderr somewhere else
    if (IS_UNSPECIFIED_HANDLE(oldConErr))
#endif
    {
        s_LoggerFile = new QFile(logDir.filePath(logFileName));
#if defined(Q_OS_LINUX) || defined(Q_OS_DARWIN)
        const bool opened = logDirectoryReady &&
                            s_LoggerFile->open(QIODevice::WriteOnly | QIODevice::Text,
                                               QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#else
        const bool opened = s_LoggerFile->open(QIODevice::WriteOnly | QIODevice::Text);
#endif
        if (opened) {
            s_LoggerFileStream.setDevice(s_LoggerFile);
        }
    }
#endif

    // Serialize log messages on a single thread
    s_LoggerThread.setMaxThreadCount(1);
    // Register our logger with all libraries
    SDL_SetLogOutputFunction(sdlLogToDiskHandler, nullptr);
    qInstallMessageHandler(qtLogToDiskHandler);
#ifdef HAVE_FFMPEG
    av_log_set_callback(ffmpegLogToDiskHandler);
#endif

#if defined(Q_OS_LINUX) || defined(Q_OS_DARWIN)
    if (s_LoggerFile != nullptr && s_LoggerFile->isOpen()) {
        qInfo() << "Persistent client log:" << s_LoggerFile->fileName();
    }
#endif

#ifdef PLANK_TRANSPORT
    if (plank_transport_abi_version() != PLANK_TRANSPORT_ABI_VERSION) {
        qCritical() << "PLANK native transport ABI mismatch";
        return 9;
    }
    qInfo() << "PLANK native transport ABI"
            << plank_transport_abi_version() << "is available";
#endif

#ifdef Q_OS_WIN32
    // Create a crash dump when we crash on Windows
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);
#endif

#ifdef LOG_TO_FILE
    // Prune the oldest existing logs if there are more than 10
    QStringList existingLogNames = logDir.entryList(QStringList(logNamePattern), QDir::Files, QDir::Time);
    for (int i = 10; i < existingLogNames.size(); i++) {
        qInfo() << "Removing old log file:" << existingLogNames.at(i);
        QFile(logDir.filePath(existingLogNames.at(i))).remove();
    }
#endif

#if defined(Q_OS_WIN32)
    // Force AntiHooking.dll to be statically imported and loaded
    // by ntdll on Win32 platforms by calling a dummy function.
    AntiHookingDummyImport();
#elif defined(APP_IMAGE)
    // Force libssl.so to be directly linked to our binary, so
    // linuxdeployqt can find it and include it in our AppImage.
    // QtNetwork will pull it in via dlopen().
    SSL_free(nullptr);
#endif

    // We keep this at function scope to ensure it stays around while we're running,
    // because the Qt QPA will need to read it. Since the temporary file is only
    // created when open() is called, this doesn't do any harm for other platforms.
    QTemporaryFile eglfsConfigFile;

    // Avoid using High DPI on EGLFS. It breaks font rendering.
    // https://bugreports.qt.io/browse/QTBUG-64377
    //
    // NB: We can't use QGuiApplication::platformName() here because it is only
    // set once the QGuiApplication is created, which is too late to enable High DPI :(
    if (WMUtils::isRunningWindowManager()) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        // Enable High DPI support on Qt 5.x. It is always enabled on Qt 6.0
        QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        // Enable fractional High DPI scaling on Qt 5.14 and later
        QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
    }
    else {
#ifndef STEAM_LINK
        if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
            qInfo() << "Unable to detect Wayland or X11, so EGLFS will be used by default. Set QT_QPA_PLATFORM to override this.";
            qputenv("QT_QPA_PLATFORM", "eglfs");

            if (!qEnvironmentVariableIsSet("QT_QPA_EGLFS_ALWAYS_SET_MODE")) {
                qInfo() << "Setting display mode by default. Set QT_QPA_EGLFS_ALWAYS_SET_MODE=0 to override this.";

                // The UI doesn't appear on RetroPie without this option.
                qputenv("QT_QPA_EGLFS_ALWAYS_SET_MODE", "1");
            }

            if (!QFile("/dev/dri").exists()) {
                qWarning() << "Unable to find a KMSDRM display device!";
                qWarning() << "On the Raspberry Pi, you must enable the 'fake KMS' driver in raspi-config to use PLANK Client outside of the GUI environment.";
            }
            else if (!qEnvironmentVariableIsSet("QT_QPA_EGLFS_KMS_CONFIG")) {
                // HACK: Remove this when Qt is fixed to properly check for display support before picking a card
                QString cardOverride = WMUtils::getDrmCardOverride();
                if (!cardOverride.isEmpty()) {
                    if (eglfsConfigFile.open()) {
                        qInfo() << "Overriding default Qt EGLFS card selection to" << cardOverride;
                        QTextStream(&eglfsConfigFile) << "{ \"device\": \"" << cardOverride << "\" }";
                        qputenv("QT_QPA_EGLFS_KMS_CONFIG", eglfsConfigFile.fileName().toUtf8());
                        eglfsConfigFile.close();
                    }
                }
            }
        }

        // EGLFS uses OpenGLES 2.0, so we will too. Some embedded platforms may not
        // even have working OpenGL implementations, so GLES is the only option.
        // See https://github.com/moonlight-stream/moonlight-qt/issues/868
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
#endif
    }

    bool forceGles;
    if (!Utils::getEnvironmentVariableOverride("FORCE_QT_GLES", &forceGles)) {
        forceGles = WMUtils::isRunningNvidiaProprietaryDriverX11() ||
                    !WMUtils::supportsDesktopGLWithEGL();
    }
    if (forceGles) {
        // The Nvidia proprietary driver causes Qt to render a black window when using
        // the default Desktop GL profile with EGL. AS a workaround, we default to
        // OpenGL ES when running on Nvidia on X11.
        // https://qt-project.atlassian.net/browse/QTBUG-106065
        QSurfaceFormat fmt;
        fmt.setRenderableType(QSurfaceFormat::OpenGLES);
        QSurfaceFormat::setDefaultFormat(fmt);
    }

    // Some ARM and RISC-V embedded devices don't have working GLX which can cause
    // SDL to fail to find a working OpenGL implementation at all. Let's force EGL
    // on all platforms for both SDL and Qt. This also avoids GLX-EGL interop issues
    // when trying to use EGL on the main thread after Qt uses GLX.
    SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
    qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");

#ifdef Q_OS_WIN32
    // Let us see the true VBlank rather than DWM's approximation. We do this here
    // because this API must be called before the first swapchain (which Qt will
    // create when the window is displayed). This is supported on Win11 22H2+.
    auto fnDXGIDisableVBlankVirtualization =
        (decltype(DXGIDisableVBlankVirtualization)*)GetProcAddress(GetModuleHandleW(L"dxgi.dll"),
                                                                   "DXGIDisableVBlankVirtualization");
    if (fnDXGIDisableVBlankVirtualization) {
        fnDXGIDisableVBlankVirtualization();
    }
#endif

#ifdef Q_OS_MACOS
    // This avoids using the default keychain for SSL, which may cause
    // password prompts on macOS.
    qputenv("QT_SSL_USE_TEMPORARY_KEYCHAIN", "1");
#endif

#if defined(Q_OS_WIN32) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    if (!qEnvironmentVariableIsSet("QT_OPENGL")) {
        // On Windows, use ANGLE so we don't have to load OpenGL
        // user-mode drivers into our app. OGL drivers (especially Intel)
        // seem to crash Moonlight far more often than DirectX.
        qputenv("QT_OPENGL", "angle");
    }
#endif

#if !defined(Q_OS_WIN32) || QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Moonlight requires the non-threaded renderer because we depend
    // on being able to control the render thread by blocking in the
    // main thread (and pumping events from the main thread when needed).
    // That doesn't work with the threaded renderer which causes all
    // sorts of odd behavior depending on the platform.
    //
    // NB: Windows defaults to the "windows" non-threaded render loop on
    // Qt 5 and the threaded render loop on Qt 6.
    qputenv("QSG_RENDER_LOOP", "basic");
#endif

#if defined(Q_OS_DARWIN) && defined(QT_DEBUG) && !defined(HAVE_LIBPLACEBO_VULKAN)
    // Enable Metal valiation for debug builds without libplacebo
    //
    // The current MoltenVK driver as of Vulkan SDK 1.4.350 triggers Metal debug layer
    // violations on frame and overlay uploads like:
    // _validateReplaceRegion:252: failed assertion `Replace Region Validation
    // bytesPerRow(4803) must be a multiple of MTLPixelFormatBGRA8Unorm pixel bytes(4).
    qputenv("MTL_DEBUG_LAYER", "1");
    qputenv("MTL_SHADER_VALIDATION", "1");
#endif

    // We don't want system proxies to apply to us
    QNetworkProxyFactory::setUseSystemConfiguration(false);

    // Clear any default application proxy
    QNetworkProxy noProxy(QNetworkProxy::NoProxy);
    QNetworkProxy::setApplicationProxy(noProxy);

    // Register custom metatypes for use in signals
    qRegisterMetaType<NvApp>("NvApp");

    // Allow the display to sleep by default. We will manually use SDL_DisableScreenSaver()
    // and SDL_EnableScreenSaver() when appropriate. This hint must be set before
    // initializing the SDL video subsystem to have any effect.
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

    // We use MMAL to render on Raspberry Pi, so we do not require DRM master.
    SDL_SetHint(SDL_HINT_KMSDRM_REQUIRE_DRM_MASTER, "0");

    // Use Direct3D 9Ex to avoid a deadlock caused by the D3D device being reset when
    // the user triggers a UAC prompt. This option controls the software/SDL renderer.
    // The DXVA2 renderer uses Direct3D 9Ex itself directly.
    SDL_SetHint(SDL_HINT_WINDOWS_USE_D3D9EX, "1");

#if defined(STEAM_LINK) || defined(Q_OS_WIN32)
    // Steam Link requires that we initialize video before creating our
    // QGuiApplication in order to configure the framebuffer correctly.
    //
    // We keep the video subsystem initialized on Windows because it's
    // much more costly to reinitialize than other platforms. It hurts
    // the settings page transition performance significantly.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s",
                     SDL_GetError());
        return -1;
    }
#endif

    // Use atexit() to ensure SDL_Quit() is called. This avoids
    // racing with object destruction where SDL may be used.
    atexit(SDL_Quit);

    // Avoid the default behavior of changing the timer resolution to 1 ms.
    // We don't want this all the time that Moonlight is open. We will set
    // it manually when we start streaming.
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "0");

    // Disable minimize on focus loss by default. Users seem to want this off by default.
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

    // Disable relative mouse scaling to renderer size or logical DPI. We want to send
    // the mouse motion exactly how it was given to us.
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0");

#ifdef Q_OS_DARWIN
    // The SDL3 input handler relies on automatic capture while a button is
    // held. Without it, SDL clamps captured Cocoa drag coordinates to the
    // starting window and the pointer cannot cross presentation outputs.
    SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "1");
#else
    // Preserve the qualified non-Cocoa capture policy.
    SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "0");
#endif

    // PLANK is a Wayland desktop client. Prefer libdecor so windowed
    // streams consistently receive a title bar and resize borders on GNOME.
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR,
                            "1", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_WAYLAND_PREFER_LIBDECOR,
                            "1", SDL_HINT_OVERRIDE);

#ifdef QT_DEBUG
    // Allow thread naming using exceptions on debug builds. SDL doesn't use SEH
    // when throwing the exceptions, so we don't enable it for release builds out
    // of caution.
    SDL_SetHint(SDL_HINT_WINDOWS_DISABLE_THREAD_NAMING, "0");
#endif

    // Enable fast parameter checks on SDL 3.4.0+. We don't abuse the API by passing
    // incorrect objects, so we don't need additional expensive parameter checks.
    SDL_SetHint("SDL_INVALID_PARAM_CHECKS", "1");

    // Disable hotplug detection for SDL_GetKeyboards() and SDL_GetMice(). We don't
    // use this functionality and it can cause hangs when querying broken devices.
    SDL_SetHint("SDL_WINDOWS_DETECT_DEVICE_HOTPLUG", "0");

    // SDL3 supports offloading scaling to the Wayland compositor, which we take
    // advantage of in the GL_IS_SLOW case to help fillrate-limited GPUs. To stay
    // consistent with our own scaling logic, we need aspect ratio scaling which
    // KDE doesn't currently handle properly. As a compromise, we'll just enable
    // aspect ratio scaling in non-KDE environments.
    //
    // NB: We do not force SDL_VIDEO_WAYLAND_MODE_SCALING to "stretch" on KDE,
    // because SDL 3.6 has a workaround for KDE and switches the default to
    // "aspect" for all desktops.
    if (qgetenv("XDG_CURRENT_DESKTOP") != "KDE") {
        SDL_SetHint("SDL_VIDEO_WAYLAND_MODE_SCALING", "aspect");
    }

#ifdef Q_OS_MACOS
    MacApplication app(argc, argv);
#else
    QGuiApplication app(argc, argv);
#endif

#ifdef Q_OS_MACOS
    // Our authenticated setup requires TLS1.3. Qt's SecureTransport backend
    // cannot provide it; never silently select that backend on a clean Mac.
    if (!QSslSocket::setActiveBackend(QStringLiteral("openssl")) ||
            !QSslSocket::supportedProtocols().contains(QSsl::TlsV1_3)) {
        qCritical() << "PLANK requires the bundled OpenSSL TLS1.3 backend; available:"
                    << QSslSocket::availableBackends();
        return 10;
    }
    qInfo() << "PLANK TLS backend:" << QSslSocket::activeBackend()
            << QSslSocket::sslLibraryVersionString();
#endif
    QGuiApplication::setApplicationDisplayName("PLANK Client");

#ifdef Q_OS_DARWIN
    // macOS defaults "Keyboard navigation" to text fields and lists only, which
    // prevents Tab (and the gamepad navigation that synthesizes it) from moving
    // focus between non-text controls on the settings page. Force Tab to reach
    // all controls so keyboard and gamepad UI navigation work without requiring
    // the user to enable a system accessibility setting. Other platforms already
    // default to this behavior.
    app.styleHints()->setTabFocusBehavior(Qt::TabFocusAllControls);
#endif

#ifdef Q_OS_UNIX
    // Register signal handlers to arbitrate between SDL and Qt.
    // NB: This has to be done after the QGuiApplication is constructed to
    // ensure Qt has already installed its VT signals before we override
    // some of them with our own.
    configureSignalHandlers();
#endif

#ifdef Q_OS_WIN32
    // If we don't have stdout or stderr handles (which will normally be the case
    // since we're a /SUBSYSTEM:WINDOWS app), attach to our parent console and use
    // that for stdout and stderr.
    //
    // If we do have stdout or stderr handles, that means the user has used standard
    // handle redirection. In that case, we don't want to override those handles.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        // If we didn't have an old stdout/stderr handle, use the new CONOUT$ handle
        if (IS_UNSPECIFIED_HANDLE(oldConOut)) {
            FILE* fp;
            if (freopen_s(&fp, "CONOUT$", "w", stdout) == 0) {
                setvbuf(fp, NULL, _IONBF, 0);
            }
            else {
                freopen_s(&fp, "NUL", "w", stdout);
            }
        }
        if (IS_UNSPECIFIED_HANDLE(oldConErr)) {
            FILE* fp;
            if (freopen_s(&fp, "CONOUT$", "w", stderr) == 0) {
                setvbuf(fp, NULL, _IONBF, 0);
            }
            else {
                freopen_s(&fp, "NUL", "w", stderr);
            }
        }
    }
#endif

    GlobalCommandLineParser parser;
    GlobalCommandLineParser::ParseResult commandLineParserResult = parser.parse(app.arguments());
    const int compileVersion = SDL_VERSION;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Compiled with SDL %d.%d.%d",
                SDL_VERSIONNUM_MAJOR(compileVersion),
                SDL_VERSIONNUM_MINOR(compileVersion),
                SDL_VERSIONNUM_MICRO(compileVersion));

    const int runtimeVersion = SDL_GetVersion();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Running with SDL %d.%d.%d",
                SDL_VERSIONNUM_MAJOR(runtimeVersion),
                SDL_VERSIONNUM_MINOR(runtimeVersion),
                SDL_VERSIONNUM_MICRO(runtimeVersion));

    // SDL 3.4.0 and 3.4.2 have bugs in atomic KMSDRM support that break us,
    // so disable atomic on affected native SDL3 runtimes.
    if (runtimeVersion < SDL_VERSIONNUM(3, 4, 4)) {
#if !defined(Q_OS_WIN32) && !defined(Q_OS_DARWIN)
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Setting SDL_KMSDRM_ATOMIC=0 for older SDL3 version");
        SDL_SetHint("SDL_KMSDRM_ATOMIC", "0");
#endif
    }

    // Apply the initial translation based on user preference
    StreamingPreferences::get()->retranslate();

    // Trickily declare the translation for dialog buttons
    QCoreApplication::translate("QPlatformTheme", "&Yes");
    QCoreApplication::translate("QPlatformTheme", "&No");
    QCoreApplication::translate("QPlatformTheme", "OK");
    QCoreApplication::translate("QPlatformTheme", "Help");
    QCoreApplication::translate("QPlatformTheme", "Cancel");

    // After the QGuiApplication is created, the platform stuff will be initialized
    // and we can set the SDL video driver to match Qt.
    if (QGuiApplication::platformName() == "xcb") {
        if (WMUtils::isRunningWayland()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Detected XWayland. This will probably break hardware decoding! Try running with QT_QPA_PLATFORM=wayland or switch to X11.");
        }
        qputenv("SDL_VIDEODRIVER", "x11");
    }
    else if (QGuiApplication::platformName().startsWith("wayland")) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Detected Wayland");
        qputenv("SDL_VIDEODRIVER", "wayland");
    }
#ifndef STEAM_LINK
    // Force use of the KMSDRM backend for SDL when using Qt platform plugins
    // that directly draw to the display without a windowing system.
    else if (QGuiApplication::platformName() == "eglfs" || QGuiApplication::platformName() == "linuxfb") {
        qputenv("SDL_VIDEODRIVER", "kmsdrm");
    }
#endif

#ifdef HAVE_DRM_MASTER_HOOKS
    // Only use the Qt-SDL DRM master interoperability hooks if Qt is using KMS
    g_DisableDrmHooks = QGuiApplication::platformName() != "eglfs";
#endif

#ifdef STEAM_LINK
    // Qt 5.9 from the Steam Link SDK is not able to load any fonts
    // since the Steam Link doesn't include any of the ones it looks
    // for. We know it has NotoSans so we will explicitly ask for that.
    if (app.font().family().isEmpty()) {
        qWarning() << "SL HACK: No default font - using NotoSans";

        QFont fon("NotoSans");
        app.setFont(fon);
    }

    // Keep the system cursor out of the full-screen UI on Steam Link.
    QCursor().setPos(0xFFFF, 0xFFFF);
#endif

#ifndef Q_OS_DARWIN
    // Set the window icon except on macOS where we want to keep the
    // modified macOS 11 style rounded corner icon.
    app.setWindowIcon(QIcon(":/res/plank-logo.png"));
#endif

    // Match the PLANK desktop entry so Wayland and X11 shells group
    // both the Qt launcher and SDL stream window under the packaged identity.
    app.setDesktopFileName("la.instinctual.Plank.Client");

    // Register our C++ types for QML
    qmlRegisterType<ComputerModel>("ComputerModel", 1, 0, "ComputerModel");
    qmlRegisterUncreatableType<Session>("Session", 1, 0, "Session", "Session cannot be created from QML");
    qmlRegisterSingletonType<ComputerManager>("ComputerManager", 1, 0,
                                              "ComputerManager",
                                              [](QQmlEngine* qmlEngine, QJSEngine*) -> QObject* {
                                                  return new ComputerManager(StreamingPreferences::get(qmlEngine));
                                              });
    qmlRegisterSingletonType<SystemProperties>("SystemProperties", 1, 0,
                                               "SystemProperties",
                                               [](QQmlEngine*, QJSEngine*) -> QObject* {
                                                   return new SystemProperties();
                                               });
    qmlRegisterSingletonType<StreamingPreferences>("StreamingPreferences", 1, 0,
                                                   "StreamingPreferences",
                                                   [](QQmlEngine* qmlEngine, QJSEngine*) -> QObject* {
                                                       return StreamingPreferences::get(qmlEngine);
                                                   });

    // We require the Material theme
    QQuickStyle::setStyle("Material");

    // Our icons are styled for a dark theme, so we do not allow the user to override this
    qputenv("QT_QUICK_CONTROLS_MATERIAL_THEME", "Dark");

    // These are defaults that we allow the user to override
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_MATERIAL_ACCENT")) {
        qputenv("QT_QUICK_CONTROLS_MATERIAL_ACCENT", "#5A9CE6");
    }
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_MATERIAL_VARIANT")) {
        qputenv("QT_QUICK_CONTROLS_MATERIAL_VARIANT", "Dense");
    }
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_MATERIAL_PRIMARY")) {
        qputenv("QT_QUICK_CONTROLS_MATERIAL_PRIMARY", "#393D43");
    }

    QQmlApplicationEngine engine;
    QString initialView;
    switch (commandLineParserResult) {
    case GlobalCommandLineParser::NormalStartRequested:
        initialView = "qrc:/gui/PcView.qml";
        break;
    case GlobalCommandLineParser::StreamRequested:
        {
            initialView = "qrc:/gui/CliStartStreamSegue.qml";
            StreamingPreferences* preferences = StreamingPreferences::get();
            StreamCommandLineParser streamParser;
            streamParser.parse(app.arguments(), preferences);
            QString host    = streamParser.getHost();
            QString appName = streamParser.getAppName();
            QString plankUsername = streamParser.getPlankUsername();
            QString plankPassword = streamParser.takePlankPassword();
            auto launcher = new CliStartStream::Launcher(
                    host, appName, preferences,
                    std::move(plankUsername),
                    std::move(plankPassword), &app);
            engine.rootContext()->setContextProperty("launcher", launcher);
            break;
        }
    }

    engine.rootContext()->setContextProperty("initialView", initialView);
    engine.rootContext()->setContextProperty(
                "runConfigChecks",
                commandLineParserResult == GlobalCommandLineParser::NormalStartRequested);

#ifdef Q_OS_MACOS
    // Ask once before any stream can capture the pointer. macOS 27 repeats
    // the Device Control dialog on every launch unless we remember the ask.
    // Enabling the preference later in Settings can still request it, never
    // from a live stream.
    auto requestKeyboardPermission = [] {
        if (Session::get() == nullptr) {
            MacKeyboardCapture::requestPermissionIfNeeded(
                        StreamingPreferences::get()->captureSysKeysMode != StreamingPreferences::CSK_OFF);
        }
    };
    QObject::connect(StreamingPreferences::get(), &StreamingPreferences::captureSysKeysModeChanged,
                     &app, requestKeyboardPermission);
    // CLI autoconnect must not put a permission dialog behind its stream.
    // Provision permission once by opening the ordinary launcher first.
    if (commandLineParserResult == GlobalCommandLineParser::NormalStartRequested) {
        requestKeyboardPermission();
    }
#endif

    // Load the main.qml file
    engine.load(QUrl(QStringLiteral("qrc:/gui/main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    int err = app.exec();

    // Give worker tasks time to exit cleanly before process teardown.
    QThreadPool::globalInstance()->waitForDone(30000);

    // Restore the default logger for all libraries before shutting down ours
    SDL_SetLogOutputFunction(SDL_GetDefaultLogOutputFunction(), nullptr);
    qInstallMessageHandler(nullptr);
#ifdef HAVE_FFMPEG
    av_log_set_callback(av_log_default_callback);
#endif

    // We should not be in async logging mode anymore
    Q_ASSERT(g_AsyncLoggingEnabled == 0);

    // Wait for pending log messages to be printed
    s_LoggerThread.waitForDone();

#ifdef Q_OS_WIN32
    // Ensure redirected command-line output reaches the destination file.
    fflush(stderr);
    fflush(stdout);
#endif

    return err;
}
