#include "streamingpreferences.h"
#include "backend/planknetwork.h"
#include "plankclientpolicy.h"
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QVariantMap>
#include <QTranslator>
#include <QCoreApplication>
#include <QLocale>
#include <QReadWriteLock>
#include <QtDebug>

#define SER_STREAMSETTINGS "streamsettings"
#define SER_FPS "fps"
#define SER_VSYNC "vsync"
#define SER_HOSTAUDIO "hostaudio"
#define SER_AUDIOCFG "audiocfg"
#define SER_PLANK_TOOLBAR_PINNED "planktoolbarpinned"
#define SER_WINDOWMODE "windowmode"
#define SER_MDNS "mdns"
#define SER_CONNWARNINGS "connwarnings"
#define SER_QUIC_UDP_PAYLOAD_MTU "plank-quic-udp-payload-mtu"
#define SER_PLANK_UNREACHABLE_TIMEOUT "plank-unreachable-timeout-seconds"
#define SER_PLANK_UNREACHABLE_ACTION "plank-unreachable-action"
#define SER_SHOWPERFOVERLAY "showperfoverlay"
#define SER_MUTEONFOCUSLOSS "muteonfocusloss"
#define SER_CAPTURESYSKEYS "capturesyskeys"
#define SER_KEEPAWAKE "keepawake"
#define SER_LANGUAGE "language"
#define SER_BROADCAST_MONITOR "broadcast-monitor"

static StreamingPreferences* s_GlobalPrefs;
static QReadWriteLock s_GlobalPrefsLock;

StreamingPreferences::StreamingPreferences(QQmlEngine *qmlEngine)
    : m_QmlEngine(qmlEngine)
{
    reload();
}

StreamingPreferences* StreamingPreferences::get(QQmlEngine *qmlEngine)
{
    {
        QReadLocker readGuard(&s_GlobalPrefsLock);

        // If we have a preference object and it's associated with a QML engine or
        // if the caller didn't specify a QML engine, return the existing object.
        if (s_GlobalPrefs && (s_GlobalPrefs->m_QmlEngine || !qmlEngine)) {
            // The lifetime logic here relies on the QML engine also being a singleton.
            Q_ASSERT(!qmlEngine || s_GlobalPrefs->m_QmlEngine == qmlEngine);
            return s_GlobalPrefs;
        }
    }

    {
        QWriteLocker writeGuard(&s_GlobalPrefsLock);

        // If we already have an preference object but the QML engine is now available,
        // associate the QML engine with the preferences.
        if (s_GlobalPrefs) {
            if (!s_GlobalPrefs->m_QmlEngine) {
                s_GlobalPrefs->m_QmlEngine = qmlEngine;
            }
            else {
                // We could reach this codepath if another thread raced with us
                // and created the object while we were outside the pref lock.
                Q_ASSERT(!qmlEngine || s_GlobalPrefs->m_QmlEngine == qmlEngine);
            }
        }
        else {
            s_GlobalPrefs = new StreamingPreferences(qmlEngine);
        }

        return s_GlobalPrefs;
    }
}

void StreamingPreferences::reload()
{
    QSettings settings;

    recommendedFullScreenMode = WindowMode::WM_FULLSCREEN_DESKTOP;

    fps = settings.value(SER_FPS, 60).toInt();
    identityGbrBitDepth = 10;
    plankToolbarPinned = settings.value(SER_PLANK_TOOLBAR_PINNED, false).toBool();
    enableVsync = settings.value(SER_VSYNC, true).toBool();
    playAudioOnHost = settings.value(SER_HOSTAUDIO, false).toBool();
    enableMdns = settings.value(SER_MDNS, false).toBool();
    const PlankClientPolicy systemPolicy;
    mdnsDiscoveryManaged = systemPolicy.managedBoolean(
                QStringLiteral("network/mdns_discovery"), &enableMdns);
    if (mdnsDiscoveryManaged) {
        qInfo() << "mDNS discovery is managed by"
                << PlankClientPolicy::defaultConfigPath();
    }
    connectionWarnings = settings.value(SER_CONNWARNINGS, true).toBool();
    showPerformanceOverlay = settings.value(SER_SHOWPERFOVERLAY, false).toBool();
    quicUdpPayloadMtu = settings.value(SER_QUIC_UDP_PAYLOAD_MTU, 0).toInt();
    if (quicUdpPayloadMtu != 0 &&
            (quicUdpPayloadMtu < PlankNetwork::MinimumQuicUdpPayloadMtu ||
             quicUdpPayloadMtu > PlankNetwork::MaximumQuicUdpPayloadMtu)) {
        quicUdpPayloadMtu = 0;
    }
    plankUnreachableTimeoutSeconds = qBound(
                5,
                settings.value(SER_PLANK_UNREACHABLE_TIMEOUT, 15).toInt(),
                300);
    const int unreachableAction = settings.value(
                SER_PLANK_UNREACHABLE_ACTION,
                static_cast<int>(PlankUnreachableAction::PLANK_UNREACHABLE_ASK)).toInt();
    plankUnreachableAction =
            unreachableAction == PlankUnreachableAction::PLANK_UNREACHABLE_DISCONNECT ?
                PlankUnreachableAction::PLANK_UNREACHABLE_DISCONNECT :
                PlankUnreachableAction::PLANK_UNREACHABLE_ASK;
    muteOnFocusLoss = settings.value(SER_MUTEONFOCUSLOSS, false).toBool();
    keepAwake = settings.value(SER_KEEPAWAKE, true).toBool();
    captureSysKeysMode = static_cast<CaptureSysKeysMode>(settings.value(SER_CAPTURESYSKEYS,
                                                         static_cast<int>(CaptureSysKeysMode::CSK_ALWAYS)).toInt());
    audioConfig = static_cast<AudioConfig>(settings.value(SER_AUDIOCFG,
                                                  static_cast<int>(AudioConfig::AC_STEREO)).toInt());
    broadcastMonitorGeometry = settings.value(SER_BROADCAST_MONITOR).toString();
    windowMode = static_cast<WindowMode>(settings.value(SER_WINDOWMODE,
                                                        static_cast<int>(recommendedFullScreenMode)).toInt());
    language = static_cast<Language>(settings.value(SER_LANGUAGE,
                                                    static_cast<int>(Language::LANG_AUTO)).toInt());

}

bool StreamingPreferences::retranslate()
{
    static QTranslator* translator = nullptr;

#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)
    if (m_QmlEngine != nullptr) {
        // Dynamic retranslation is not supported until Qt 5.10
        return false;
    }
#endif

    QTranslator* newTranslator = new QTranslator();
    QString languageSuffix = getSuffixFromLanguage(language);

    // Remove the old translator, even if we can't load a new one.
    // Otherwise we'll be stuck with the old translated values instead
    // of defaulting to English.
    if (translator != nullptr) {
        QCoreApplication::removeTranslator(translator);
        delete translator;
        translator = nullptr;
    }

    if (newTranslator->load(QString(":/languages/qml_") + languageSuffix)) {
        qInfo() << "Successfully loaded translation for" << languageSuffix;

        translator = newTranslator;
        QCoreApplication::installTranslator(translator);
    }
    else {
        qInfo() << "No translation available for" << languageSuffix;
        delete newTranslator;
    }

    if (m_QmlEngine != nullptr) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        // This is a dynamic retranslation from the settings page.
        // We have to kick the QML engine into reloading our text.
        m_QmlEngine->retranslate();
#else
        // Unreachable below Qt 5.10 due to the check above
        Q_ASSERT(false);
#endif
    }
    else {
        // This is a translation from a non-QML context, which means
        // it is probably app startup. There's nothing to refresh.
    }

    return true;
}

QString StreamingPreferences::getSuffixFromLanguage(StreamingPreferences::Language lang)
{
    switch (lang)
    {
    case LANG_DE:
        return "de";
    case LANG_EN:
        return "en";
    case LANG_FR:
        return "fr";
    case LANG_ZH_CN:
        return "zh_CN";
    case LANG_NB_NO:
        return "nb_NO";
    case LANG_RU:
        return "ru";
    case LANG_ES:
        return "es";
    case LANG_JA:
        return "ja";
    case LANG_VI:
        return "vi";
    case LANG_TH:
        return "th";
    case LANG_KO:
        return "ko";
    case LANG_HU:
        return "hu";
    case LANG_NL:
        return "nl";
    case LANG_SV:
        return "sv";
    case LANG_TR:
        return "tr";
    case LANG_UK:
        return "uk";
    case LANG_ZH_TW:
        return "zh_TW";
    case LANG_PT:
        return "pt";
    case LANG_PT_BR:
        return "pt_BR";
    case LANG_EL:
        return "el";
    case LANG_IT:
        return "it";
    case LANG_HI:
        return "hi";
    case LANG_PL:
        return "pl";
    case LANG_CS:
        return "cs";
    case LANG_HE:
        return "he";
    case LANG_CKB:
        return "ckb";
    case LANG_LT:
        return "lt";
    case LANG_ET:
        return "et";
    case LANG_AUTO:
    default:
        return QLocale::system().name();
    }
}

QVariantList StreamingPreferences::localMonitors() const
{
    QVariantList monitors;
    QVariantMap none;
    none.insert(QStringLiteral("text"), tr("None"));
    none.insert(QStringLiteral("geometry"), QString());
    monitors.append(none);
    const auto screens = QGuiApplication::screens();
    for (int index = 0; index < screens.size(); ++index) {
        QScreen* screen = screens.at(index);
        const QRect bounds = screen->geometry();
        QVariantMap entry;
        entry.insert(QStringLiteral("text"),
                     tr("Monitor %1 (%2x%3)")
                     .arg(index + 1)
                     .arg(bounds.width())
                     .arg(bounds.height()));
        entry.insert(QStringLiteral("geometry"),
                     QStringLiteral("%1,%2,%3,%4")
                     .arg(bounds.x()).arg(bounds.y())
                     .arg(bounds.width()).arg(bounds.height()));
        monitors.append(entry);
    }
    return monitors;
}

void StreamingPreferences::save()
{
    QSettings settings;

    settings.setValue(SER_FPS, fps);
    settings.setValue(SER_VSYNC, enableVsync);
    settings.setValue(SER_HOSTAUDIO, playAudioOnHost);
    if (!mdnsDiscoveryManaged) {
        settings.setValue(SER_MDNS, enableMdns);
    }
    settings.setValue(SER_CONNWARNINGS, connectionWarnings);
    settings.setValue(SER_QUIC_UDP_PAYLOAD_MTU, quicUdpPayloadMtu);
    settings.setValue(SER_PLANK_UNREACHABLE_TIMEOUT,
                      plankUnreachableTimeoutSeconds);
    settings.setValue(SER_PLANK_UNREACHABLE_ACTION,
                      static_cast<int>(plankUnreachableAction));
    settings.setValue(SER_SHOWPERFOVERLAY, showPerformanceOverlay);
    settings.setValue(SER_AUDIOCFG, static_cast<int>(audioConfig));
    settings.setValue(SER_PLANK_TOOLBAR_PINNED, plankToolbarPinned);
    settings.setValue(SER_WINDOWMODE, static_cast<int>(windowMode));
    settings.setValue(SER_BROADCAST_MONITOR, broadcastMonitorGeometry);
    settings.setValue(SER_LANGUAGE, static_cast<int>(language));
    settings.setValue(SER_MUTEONFOCUSLOSS, muteOnFocusLoss);
    settings.setValue(SER_CAPTURESYSKEYS, captureSysKeysMode);
    settings.setValue(SER_KEEPAWAKE, keepAwake);
}
