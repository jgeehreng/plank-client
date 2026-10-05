#include "nvcomputer.h"
#include "desktopstage.h"
#include "hostrecovery.h"
#include <QCryptographicHash>
#include <QScopedPointer>
#include <Limelight.h>

#include <utility>

#include <QDebug>
#include <QDateTime>
#include <QtNetwork/QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QXmlStreamReader>
#include <QSslKey>
#include <QSslCipher>
#include <QImageReader>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QUuid>

#define FAST_FAIL_TIMEOUT_MS 2000
#define REQUEST_TIMEOUT_MS 5000
#define LAUNCH_TIMEOUT_MS 120000
#define RESUME_TIMEOUT_MS 30000

namespace {
struct AdmissionMemory {
    bool set = false;
    QJsonObject wrapper;
    QString uniqueId;
    QString certificateSha256;
    // Broker ticket already presented to the Host. The next authenticate()
    // releases this reservation and dials dialedAddress, which is the
    // workstation address from before the relay rewrite.
    QString admissionId;
    QString dialedAddress;
};

AdmissionMemory g_admission;

bool sameWorkstationUuid(const QString& left, const QString& right)
{
    if (left.size() != 36 || right.size() != 36) return false;
    for (int i = 0; i < 36; ++i) {
        const bool hyphen = i == 8 || i == 13 || i == 18 || i == 23;
        const QChar a = left.at(i);
        const QChar b = right.at(i);
        if (hyphen) {
            if (a != QLatin1Char('-') || b != QLatin1Char('-')) return false;
            continue;
        }
        const char ca = a.toLower().toLatin1();
        const char cb = b.toLower().toLatin1();
        const bool hexA = (ca >= '0' && ca <= '9') || (ca >= 'a' && ca <= 'f');
        const bool hexB = (cb >= '0' && cb <= '9') || (cb >= 'a' && cb <= 'f');
        if (!hexA || !hexB || ca != cb) return false;
    }
    return true;
}

void loadAdmissionFromEnvironment()
{
    if (g_admission.set) return;
    const QByteArray path = qgetenv("PLANK_ADMISSION_BUNDLE");
    if (path.isEmpty()) return;
    QFile file(QString::fromLocal8Bit(path));
    if (!file.open(QIODevice::ReadOnly)) {
        throw GfeHttpResponseException(400, "PLANK admission bundle is unavailable");
    }
    const QByteArray bytes = file.readAll();
    file.close();
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    const QJsonObject root = document.object();
    const QJsonObject admission = root.value(QStringLiteral("admission")).toObject();
    const QString uniqueId = root.value(QStringLiteral("workstation_uniqueid")).toString();
    if (!document.isObject() || admission.value(QStringLiteral("v")).toInt() != 1 ||
            !admission.value(QStringLiteral("payload")).isString() ||
            !admission.value(QStringLiteral("sig")).isString() || uniqueId.size() != 36) {
        throw GfeHttpResponseException(400, "PLANK admission bundle is unavailable");
    }
    g_admission.set = true;
    g_admission.wrapper = admission;
    g_admission.uniqueId = uniqueId;
    g_admission.certificateSha256 = root.value(QStringLiteral("certificate_sha256")).toString().toLower();
}

class SecureStringGuard
{
public:
    explicit SecureStringGuard(QString& value) : m_Value(value) {}
    ~SecureStringGuard()
    {
        m_Value.fill(QChar('\0'));
        m_Value.clear();
    }

private:
    QString& m_Value;
};

bool isPlankCertificate(const QSslCertificate& certificate)
{
    const auto alternativeNames = certificate.subjectAlternativeNames();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    return !certificate.isNull() && certificate.isSelfSigned() &&
            certificate.publicKey().algorithm() == QSsl::Rsa &&
            certificate.publicKey().length() >= 3072 &&
            !alternativeNames.values(QSsl::DnsEntry).isEmpty() &&
            alternativeNames.values(QSsl::IpAddressEntry).isEmpty() &&
            certificate.effectiveDate() <= now && certificate.expiryDate() > now;
}

QSslConfiguration plankSslConfiguration()
{
    QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
    configuration.setProtocol(QSsl::TlsV1_3OrLater);
    return configuration;
}

// Qt can clear the live socket's negotiated TLS details when a close-delimited
// HTTP response finishes. Preserve the completed handshake on that reply, not
// globally on the manager (which may connect to another certificate later).
QMetaObject::Connection rememberPlankTls(QNetworkAccessManager* manager, QObject* context)
{
    return QObject::connect(manager, &QNetworkAccessManager::encrypted, context,
                            [](QNetworkReply* reply) {
        reply->setProperty("plankNegotiatedTls", QVariant::fromValue(reply->sslConfiguration()));
    });
}

QSslConfiguration negotiatedPlankTls(QNetworkReply* reply)
{
    const QVariant saved = reply->property("plankNegotiatedTls");
    return saved.isValid() ? saved.value<QSslConfiguration>() : reply->sslConfiguration();
}
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#define XML_NAME_EQUALS(x, y) ((x) == (y))
#else
#define XML_NAME_EQUALS(x, y) ((x) == (u##y))
#endif

NvHTTP::NvHTTP(NvAddress address, QNetworkAccessManager* nam) :
    m_Nam(nam ? nam : new QNetworkAccessManager(this))
{
    m_BaseUrlHttps.setScheme("https");

    setAddress(address);

    // Never use a proxy server
    QNetworkProxy noProxy(QNetworkProxy::NoProxy);
    m_Nam->setProxy(noProxy);
}

NvHTTP::NvHTTP(NvComputer* computer, QNetworkAccessManager* nam) :
    NvHTTP(computer->activeAddress, nam)
{
    setPlankSessionToken(computer->sessionToken);
}

void NvHTTP::setAddress(NvAddress address)
{
    Q_ASSERT(!address.isNull());

    m_Address = address;

    m_BaseUrlHttps.setHost(address.address());
    m_BaseUrlHttps.setPort(address.port());
}

void NvHTTP::setPlankSessionToken(QString sessionToken)
{
    m_SessionToken = std::move(sessionToken);
}

NvAddress NvHTTP::address()
{
    return m_Address;
}

uint16_t NvHTTP::controlPort()
{
    return m_BaseUrlHttps.port();
}

QVector<int>
NvHTTP::parseQuad(QString quad)
{
    QVector<int> ret;

    // Return an empty vector for old GFE versions
    // that were missing GfeVersion.
    if (quad.isEmpty()) {
        return ret;
    }

    QStringList parts = quad.split(".");
    ret.reserve(parts.length());
    for (int i = 0; i < parts.length(); i++)
    {
        ret.append(parts.at(i).toInt());
    }

    return ret;
}

int
NvHTTP::getCurrentGame(QString serverInfo)
{
    // GFE 2.8 started keeping currentgame set to the last game played. As a result, it no longer
    // has the semantics that its name would indicate. To contain the effects of this change as much
    // as possible, we'll force the current game to zero if the server isn't in a streaming session.
    QString serverState = getXmlString(serverInfo, "state");
    if (serverState.endsWith("_SERVER_BUSY"))
    {
        return getXmlString(serverInfo, "currentgame").toInt();
    }
    else
    {
        return 0;
    }
}

bool
NvHTTP::getPlankOccupied(QString serverInfo)
{
    return getXmlString(serverInfo, "PlankOccupied") == QLatin1String("1");
}

QString
NvHTTP::getPlankSessionUser(QString serverInfo)
{
    const QString name = getXmlString(serverInfo, "PlankSessionUser");
    if (name.size() < 1 || name.size() > 64) {
        return QString();
    }
    for (const QChar character : name) {
        const char16_t code = character.unicode();
        const bool letter = (code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z');
        const bool digit = code >= '0' && code <= '9';
        if (!letter && !digit && code != '.' && code != '_' && code != '-') {
            return QString();
        }
    }
    return name;
}

QString
NvHTTP::getServerInfo(NvLogLevel logLevel, bool fastFail)
{
    const QString serverInfo = openConnectionToString(
                m_BaseUrlHttps,
                "serverinfo",
                nullptr,
                fastFail ? FAST_FAIL_TIMEOUT_MS : REQUEST_TIMEOUT_MS,
                logLevel);
    verifyResponseStatus(serverInfo);
    return serverInfo;
}

void
NvHTTP::startApp(QString verb,
                 int appId,
                 PSTREAM_CONFIGURATION streamConfig,
                 bool localAudio,
                 int gamepadMask,
                 bool persistGameControllersOnDisconnect,
                 QString captureDisplayMode,
                 QString topologyGeneration,
                 int plankProtocolVersion,
                 int plankFeatureFlags,
                 bool takeOverActiveSession,
                 QString hostLayout,
                 QString virtualMode1,
                 QString virtualMode2,
                 QString captureSource,
                 QString encoderBackend,
                 QString encodingMode,
                 quint16 quicUdpPayloadMtu,
                 quint16& plankTransportPort,
                 QString& plankTransportCertificateSha256,
                 QString& plankTransportToken,
                 QString& acceptedCaptureSource,
                 QString& acceptedEncoderBackend,
                 QString& acceptedEncodingMode)
{
    QString plankOutputArguments;
    if (!captureDisplayMode.isEmpty()) {
        plankOutputArguments =
                "&plankProtocolVersion=" + QString::number(plankProtocolVersion) +
                "&plankFeatureFlags=" + QString::number(plankFeatureFlags) +
                "&plankDisplayMode=" + QString::fromLatin1(QUrl::toPercentEncoding(captureDisplayMode));
        if (takeOverActiveSession &&
                (plankFeatureFlags & NvOutputTopology::SessionTakeoverFeature) != 0) {
            plankOutputArguments += "&plankTakeover=1";
        }
        plankOutputArguments +=
                "&plankCaptureSource=" +
                QString::fromLatin1(QUrl::toPercentEncoding(captureSource));
        plankOutputArguments +=
                "&plankEncoderBackend=" +
                QString::fromLatin1(QUrl::toPercentEncoding(encoderBackend));
        plankOutputArguments +=
                "&plankEncodingMode=" +
                QString::fromLatin1(QUrl::toPercentEncoding(encodingMode));
        if ((plankFeatureFlags &
             NvOutputTopology::FixedTransportMtuFeature) != 0) {
            plankOutputArguments +=
                    "&plankQuicUdpPayloadMtu=" +
                    QString::number(quicUdpPayloadMtu);
        }
        if ((plankFeatureFlags & NvOutputTopology::HostLayoutBindingFeature) != 0 &&
                !hostLayout.isEmpty()) {
            plankOutputArguments +=
                    "&plankHostLayout=" +
                    QString::fromLatin1(QUrl::toPercentEncoding(hostLayout));
            if ((plankFeatureFlags &
                    NvOutputTopology::IndependentVirtualModesFeature) != 0) {
                if (!virtualMode1.isEmpty()) {
                    plankOutputArguments +=
                            "&plankVirtualMode1=" +
                            QString::fromLatin1(QUrl::toPercentEncoding(virtualMode1));
                }
                if (!virtualMode2.isEmpty()) {
                    plankOutputArguments +=
                            "&plankVirtualMode2=" +
                            QString::fromLatin1(QUrl::toPercentEncoding(virtualMode2));
                }
            }
        }
        if ((plankFeatureFlags & NvOutputTopology::TopologyGenerationFeature) != 0 &&
                !topologyGeneration.isEmpty()) {
            plankOutputArguments +=
                    "&plankTopologyGeneration=" +
                    QString::fromLatin1(QUrl::toPercentEncoding(topologyGeneration));
        }
    }

    QString response =
            openConnectionToString(m_BaseUrlHttps,
                                   verb,
                                   "appid="+QString::number(appId)+
                                   "&mode="+QString::number(streamConfig->width)+"x"+
                                   QString::number(streamConfig->height)+"x"+
                                   QString::number(streamConfig->fps)+
                                   "&additionalStates=1"+
                                   ((streamConfig->supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) ?
                                       "&hdrMode=1&clientHdrCapVersion=0&clientHdrCapSupportedFlagsInUint32=0&clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1&clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0" :
                                        "")+
                                   "&localAudioPlayMode="+QString::number(localAudio ? 1 : 0)+
                                   "&surroundAudioInfo="+QString::number(SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(streamConfig->audioConfiguration))+
                                   "&remoteControllersBitmap="+QString::number(gamepadMask)+
                                   "&gcmap="+QString::number(gamepadMask)+
                                   "&gcpersist="+QString::number(persistGameControllersOnDisconnect ? 1 : 0)+
                                   plankOutputArguments,
                                   LAUNCH_TIMEOUT_MS);

    qInfo() << "PLANK launch response received";

    // Throws if the request failed
    verifyResponseStatus(response);

    m_WorkerInstance = PlankHostRecovery::canonicalInstance(getXmlString(response, "PlankWorkerInstance"));
    if ((plankFeatureFlags & NvOutputTopology::WorkerInstanceFeature) && m_WorkerInstance.isEmpty()) {
        throw GfeHttpResponseException(400, "Host returned an invalid media-worker identity");
    }

    plankTransportPort = getXmlString(response, "PlankTransportPort").toUShort();
    plankTransportCertificateSha256 =
            getXmlString(response, "PlankTransportCertificateSha256");
    plankTransportToken = getXmlString(response, "PlankTransportToken");
    const quint16 acceptedQuicUdpPayloadMtu =
            getXmlString(response, "PlankQuicUdpPayloadMtu").toUShort();
    acceptedCaptureSource = getXmlString(response, "PlankCaptureSource");
    acceptedEncoderBackend = getXmlString(response, "PlankEncoderBackend");
    acceptedEncodingMode = getXmlString(response, "PlankEncodingMode");
    const auto isCanonicalSha256Hex = [](const QString& value) {
        const QByteArray encoded = value.toLatin1();
        const QByteArray decoded = QByteArray::fromHex(encoded);
        return encoded.size() == 64 && decoded.size() == 32 &&
                decoded.toHex() == encoded.toLower();
    };
    if (plankTransportPort == 0 ||
            !isCanonicalSha256Hex(plankTransportCertificateSha256) ||
            !isCanonicalSha256Hex(plankTransportToken)) {
        throw GfeHttpResponseException(
                    400, "Host returned invalid plank_transport launch credentials");
    }
    if (acceptedQuicUdpPayloadMtu != quicUdpPayloadMtu) {
        throw GfeHttpResponseException(
                    400, "Host did not accept the fixed QUIC UDP payload ceiling");
    }
    if (acceptedCaptureSource.isEmpty() || acceptedCaptureSource != captureSource) {
        throw GfeHttpResponseException(
                    400, "Host did not accept the requested capture source");
    }
    if (acceptedEncoderBackend.isEmpty() || acceptedEncoderBackend != encoderBackend) {
        throw GfeHttpResponseException(
                    400, "Host did not accept the requested encoder backend");
    }
    if (acceptedEncodingMode.isEmpty() || acceptedEncodingMode != encodingMode) {
        throw GfeHttpResponseException(
                    400, "Host did not accept the requested encoding mode");
    }
}

QVector<NvDisplayMode>
NvHTTP::getDisplayModeList(QString serverInfo)
{
    QXmlStreamReader xmlReader(serverInfo);
    QVector<NvDisplayMode> modes;

    while (!xmlReader.atEnd()) {
        while (xmlReader.readNextStartElement()) {
            auto name = xmlReader.name();
            if (XML_NAME_EQUALS(name, "DisplayMode")) {
                modes.append(NvDisplayMode());
            }
            else if (!modes.isEmpty()) {
                if (XML_NAME_EQUALS(name, "Width")) {
                    modes.last().width = xmlReader.readElementText().toInt();
                }
                else if (XML_NAME_EQUALS(name, "Height")) {
                    modes.last().height = xmlReader.readElementText().toInt();
                }
                else if (XML_NAME_EQUALS(name, "RefreshRate")) {
                    modes.last().refreshRate = xmlReader.readElementText().toInt();
                }
            }
        }
    }

    return modes;
}

QVector<NvApp>
NvHTTP::getAppList()
{
    QString appxml = openConnectionToString(m_BaseUrlHttps,
                                            "applist",
                                            nullptr,
                                            REQUEST_TIMEOUT_MS,
                                            NvLogLevel::NVLL_ERROR);
    verifyResponseStatus(appxml);

    QXmlStreamReader xmlReader(appxml);
    QVector<NvApp> apps;
    while (!xmlReader.atEnd()) {
        while (xmlReader.readNextStartElement()) {
            auto name = xmlReader.name();
            if (XML_NAME_EQUALS(name, "App")) {
                // We must have a valid app before advancing to the next one
                if (!apps.isEmpty() && !apps.last().isInitialized()) {
                    qWarning() << "Invalid applist XML";
                    throw std::runtime_error("Invalid applist XML");
                }
                apps.append(NvApp());
            }
            else if (!apps.isEmpty()) {
                if (XML_NAME_EQUALS(name, "AppTitle")) {
                    // If an app has no name, Sunshine may send us <AppTitle/>,
                    // which readElementText() returns as a null QString.
                    // We want to treat this as an empty QString instead, so we
                    // will explicitly convert it. An empty string will satisfy
                    // NvApp's isInitialized() check.
                    QString name = xmlReader.readElementText();
                    if (name.isNull()) {
                        name = "";
                    }
                    apps.last().name = name;
                }
                else if (XML_NAME_EQUALS(name, "ID")) {
                    apps.last().id = xmlReader.readElementText().toInt();
                }
                else if (XML_NAME_EQUALS(name, "IsHdrSupported")) {
                    apps.last().hdrSupported = xmlReader.readElementText() == "1";
                }
                else if (XML_NAME_EQUALS(name, "IsAppCollectorGame")) {
                    apps.last().isAppCollectorGame = xmlReader.readElementText() == "1";
                }
            }
        }
    }

    return apps;
}

void
NvHTTP::verifyResponseStatus(QString xml)
{
    QXmlStreamReader xmlReader(xml);

    while (xmlReader.readNextStartElement())
    {
        if (XML_NAME_EQUALS(xmlReader.name(), "root"))
        {
            // Status code can be 0xFFFFFFFF in some rare cases on GFE 3.20.3, and
            // QString::toInt() will fail in that case, so use QString::toUInt()
            // and cast the result to an int instead.
            int statusCode = (int)xmlReader.attributes().value("status_code").toUInt();
            if (statusCode == 200)
            {
                // Successful
                return;
            }
            else
            {
                QString statusMessage = xmlReader.attributes().value("status_message").toString();
                if (statusCode != 401) {
                    // 401 is expected before PAM authorization establishes a bearer session.
                    qWarning() << "Request failed:" << statusCode << statusMessage;
                }
                if (statusCode == -1 && statusMessage == "Invalid") {
                    // Special case handling an audio capture error which GFE doesn't
                    // provide any useful status message for.
                    statusCode = 418;
                    statusMessage = tr("Missing audio capture device. Reinstalling GeForce Experience should resolve this error.");
                }
                throw GfeHttpResponseException(statusCode, statusMessage);
            }
        }
    }

    throw GfeHttpResponseException(-1, "Malformed XML (missing root element)");
}

QImage
NvHTTP::getBoxArt(int appId)
{
    QNetworkReply* reply = openConnection(m_BaseUrlHttps,
                                          "appasset",
                                          "appid="+QString::number(appId)+
                                          "&AssetType=2&AssetIdx=0",
                                          REQUEST_TIMEOUT_MS,
                                          NvLogLevel::NVLL_VERBOSE);
    QImage image = QImageReader(reply).read();
    delete reply;

    return image;
}

QByteArray
NvHTTP::getXmlStringFromHex(QString xml,
                            QString tagName)
{
    return QByteArray::fromHex(getXmlString(xml, tagName).toUtf8());
}

QString
NvHTTP::getXmlString(QString xml,
                     QString tagName)
{
    QXmlStreamReader xmlReader(xml);

    while (!xmlReader.atEnd())
    {
        if (xmlReader.readNext() != QXmlStreamReader::StartElement)
        {
            continue;
        }

        if (xmlReader.name() == tagName)
        {
            return xmlReader.readElementText();
        }
    }

    return QString();
}

void NvHTTP::handleSslErrors(QNetworkReply* reply, const QList<QSslError>& errors)
{
    const QSslCertificate certificate = reply->sslConfiguration().peerCertificate();
    if (!isPlankCertificate(certificate)) {
        const auto alternativeNames = certificate.subjectAlternativeNames();
        qWarning() << "Rejecting a TLS certificate outside the PLANK profile"
                   << "null" << certificate.isNull()
                   << "selfSigned" << certificate.isSelfSigned()
                   << "keyAlgorithm" << certificate.publicKey().algorithm()
                   << "keyBits" << certificate.publicKey().length()
                   << "dnsSans" << alternativeNames.values(QSsl::DnsEntry).size()
                   << "ipSans" << alternativeNames.values(QSsl::IpAddressEntry).size()
                   << "effective" << certificate.effectiveDate()
                   << "expiry" << certificate.expiryDate();
        return;
    }
    for (const QSslError& error : errors) {
        switch (error.error()) {
        case QSslError::SelfSignedCertificate:
        case QSslError::CertificateUntrusted:
        case QSslError::UnableToGetLocalIssuerCertificate:
        case QSslError::UnableToVerifyFirstCertificate:
        case QSslError::HostNameMismatch:
            break;
        default:
            return;
        }
    }
    reply->ignoreSslErrors(errors);
}

QString
NvHTTP::openConnectionToString(QUrl baseUrl,
                               QString command,
                               QString arguments,
                               int timeoutMs,
                               NvLogLevel logLevel)
{
    QNetworkReply* reply = openConnection(baseUrl, command, arguments, timeoutMs, logLevel);
    QString ret;

    QTextStream stream(reply);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    stream.setEncoding(QStringConverter::Utf8);
#else
    stream.setCodec("UTF-8");
#endif

    ret = stream.readAll();
    delete reply;

    return ret;
}

QJsonObject NvHTTP::postPlankJson(QString command, const QJsonObject& body)
{
    waitForRequestPermission(true);
    if (!m_SessionToken.isEmpty()) {
        throw GfeHttpResponseException(400, "Invalid PLANK authentication state");
    }

    // /serverinfo leaves a TLS connection the Host has already closed. Reusing
    // it makes this POST fail before the Host reads the admission.
    m_Nam->clearAccessCache();
    QUrl url(m_BaseUrlHttps);
    url.setPath("/plank/auth/" + command);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setSslConfiguration(plankSslConfiguration());
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif

    const auto sslErrorsConnection = connect(
        m_Nam, &QNetworkAccessManager::sslErrors,
        this, &NvHTTP::handleSslErrors);
    const auto encryptedConnection = rememberPlankTls(m_Nam, this);
    QNetworkReply* reply = m_Nam->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            &loop, &QEventLoop::quit);
    QTimer::singleShot(REQUEST_TIMEOUT_MS, &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (!reply->isFinished()) {
        reply->abort();
    }
    m_Nam->clearAccessCache();
    disconnect(sslErrorsConnection);
    disconnect(encryptedConnection);
    if (reply->error() != QNetworkReply::NoError) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        const QString message = reply->errorString();
        delete reply;
        if (status >= 400) throw GfeHttpResponseException(status, "PLANK authentication request rejected");
        throw QtNetworkReplyException(error, message);
    }
    const QSslConfiguration negotiatedSsl = negotiatedPlankTls(reply);
    if (!isPlankCertificate(negotiatedSsl.peerCertificate()) ||
            negotiatedSsl.sessionProtocol() != QSsl::TlsV1_3) {
        delete reply;
        throw QtNetworkReplyException(QNetworkReply::SslHandshakeFailedError,
                                      "PLANK TLS validation failed");
    }
    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
    delete reply;
    if (!document.isObject()) {
        throw GfeHttpResponseException(400, "Malformed PLANK authentication response");
    }
    return document.object();
}

bool NvHTTP::probeWorkerReplacement(const QString& instance, const QString& certificateSha256)
{
    // Use an address-only NvHTTP, never a bearer token or PAM credentials.
    if (!m_SessionToken.isEmpty()) return false;
    QScopedPointer<QNetworkReply> reply(openConnection(m_BaseUrlHttps, "serverinfo", nullptr,
                                                      1000, NvLogLevel::NVLL_NONE));
    const QByteArray certificate = reply->sslConfiguration().peerCertificate().digest(QCryptographicHash::Sha256);
    const QString response = QString::fromUtf8(reply->readAll());
    verifyResponseStatus(response);
    return PlankHostRecovery::replacementConfirmed(instance,
                getXmlString(response, "PlankWorkerInstance"),
                QByteArray::fromHex(certificateSha256.toLatin1()), certificate);
}

namespace {

bool loopbackBroker(const QUrl& url)
{
    if (url.scheme() != QLatin1String("http") || !url.isValid()) {
        return false;
    }
    const QString host = url.host().toLower();
    return host == QLatin1String("127.0.0.1") ||
            host == QLatin1String("localhost") ||
            host == QLatin1String("::1");
}

QString sessionFilePath()
{
    return QDir::homePath() + QStringLiteral("/.local/share/plank-broker/client.session");
}

bool ownerOnlyFile(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return false;
    }
    const QFile::Permissions permissions = info.permissions();
    const QFile::Permissions others = QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
            QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
    return (permissions & others) == QFile::Permissions() && info.size() <= 80;
}

QUrl configuredBrokerUrl()
{
    const QByteArray configured = qgetenv("PLANK_BROKER_URL");
    if (!configured.isEmpty()) {
        const QUrl url(QString::fromUtf8(configured));
        if (!loopbackBroker(url)) {
            throw GfeHttpResponseException(400, "PLANK broker address must stay on this computer");
        }
        return url;
    }
    if (!qgetenv("PLANK_FACILITY_SESSION").isEmpty() || ownerOnlyFile(sessionFilePath())) {
        return QUrl(QStringLiteral("http://127.0.0.1:8765"));
    }
    return QUrl();
}

QString facilityBearer()
{
    const QByteArray configured = qgetenv("PLANK_FACILITY_SESSION");
    if (!configured.isEmpty()) {
        const QString session = QString::fromUtf8(configured).trimmed();
        if (session.isEmpty() || session.size() > 64 || session.contains(QLatin1Char(' '))) {
            throw GfeHttpResponseException(401, "PLANK facility sign-in is not valid");
        }
        return session;
    }
    const QString path = sessionFilePath();
    if (!ownerOnlyFile(path)) {
        return QString();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    const QString session = QString::fromUtf8(file.readAll()).trimmed();
    file.close();
    if (session.isEmpty() || session.size() > 64 || session.contains(QLatin1Char(' '))) {
        throw GfeHttpResponseException(401, "PLANK facility sign-in is not valid");
    }
    return session;
}

QByteArray postBroker(const QUrl& url, const QByteArray& body, const QString& session, int* status)
{
    QNetworkAccessManager manager;
    manager.setProxy(QNetworkProxy::NoProxy);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + session.toUtf8());
    QNetworkReply* reply = manager.post(request, body);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(REQUEST_TIMEOUT_MS);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (!reply->isFinished()) {
        reply->abort();
        reply->deleteLater();
        throw GfeHttpResponseException(503, "PLANK broker did not respond");
    }
    *status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray payload = reply->readAll();
    reply->deleteLater();
    return payload;
}

void throwBrokerStatus(int status)
{
    if (status == 401) {
        throw GfeHttpResponseException(401, "Facility sign-in expired. Sign in again.");
    }
    if (status == 403) {
        throw GfeHttpResponseException(403, "This account is not assigned to that workstation");
    }
    if (status == 404) {
        throw GfeHttpResponseException(404, "That workstation is not available");
    }
    if (status == 409) {
        throw GfeHttpResponseException(409, "That workstation is already reserved");
    }
    throw GfeHttpResponseException(status == 0 ? 503 : status, "PLANK broker denied the connection request");
}

struct BrokerHold {
    BrokerHold() = default;
    BrokerHold(const BrokerHold&) = delete;
    BrokerHold& operator=(const BrokerHold&) = delete;

    QString admissionId;
    QUrl broker;
    QString session;

    void keep()
    {
        admissionId.clear();
    }

    ~BrokerHold()
    {
        if (admissionId.isEmpty()) {
            return;
        }
        try {
            QUrl endpoint(broker);
            endpoint.setPath(QStringLiteral("/release"));
            endpoint.setQuery(QString());
            const QByteArray body = QJsonDocument(QJsonObject{
                {QStringLiteral("admission_id"), admissionId},
            }).toJson(QJsonDocument::Compact);
            int status = 0;
            postBroker(endpoint, body, session, &status);
        } catch (...) {
        }
    }
};

void releaseConsumedAdmission()
{
    if (g_admission.admissionId.size() != 36) {
        return;
    }
    const QUrl broker = configuredBrokerUrl();
    if (!broker.isValid()) {
        return;
    }
    const QString session = facilityBearer();
    if (session.isEmpty()) {
        return;
    }
    QUrl endpoint(broker);
    endpoint.setPath(QStringLiteral("/release"));
    endpoint.setQuery(QString());
    const QByteArray body = QJsonDocument(QJsonObject{
        {QStringLiteral("admission_id"), g_admission.admissionId},
    }).toJson(QJsonDocument::Compact);
    int status = 0;
    postBroker(endpoint, body, session, &status);
    if (status != 200) {
        throwBrokerStatus(status);
    }
}

void retirePresentedAdmission()
{
    if (g_admission.admissionId.isEmpty()) {
        return;
    }
    g_admission.set = false;
    g_admission.wrapper = QJsonObject();
}

}

void NvHTTP::setAdmissionBundle(const QJsonObject& admission, const QString& workstationUniqueId,
                                 const QString& certificateSha256)
{
    g_admission.set = true;
    g_admission.wrapper = admission;
    g_admission.uniqueId = workstationUniqueId;
    g_admission.certificateSha256 = certificateSha256.toLower();
}

bool NvHTTP::brokerConfigured()
{
    try {
        return configuredBrokerUrl().isValid();
    } catch (const GfeHttpResponseException&) {
        return false;
    }
}

QString NvHTTP::authenticate(QString username, QString password, bool* greeterConfirmed,
                             bool startDesktop)
{
    if (greeterConfirmed != nullptr) *greeterConfirmed = false;
    SecureStringGuard passwordGuard(password);
    if (!m_SessionToken.isEmpty() || username.isEmpty()) {
        throw GfeHttpResponseException(400, "Invalid PLANK authentication state");
    }
    loadAdmissionFromEnvironment();
    BrokerHold reservation;
    QString consumedDial;
    if (!g_admission.set && g_admission.admissionId.size() == 36) {
        // The Host consumes an admission on auth/start, including when the
        // display worker is replaced and this process must sign in again.
        consumedDial = g_admission.dialedAddress;
        releaseConsumedAdmission();
        g_admission = {};
        qInfo() << "PLANK requested a fresh admission after the host consumed the previous ticket";
    }
    if (!g_admission.set) {
        const QUrl broker = configuredBrokerUrl();
        if (broker.isValid()) {
            const QString session = facilityBearer();
            if (session.isEmpty()) {
                throw GfeHttpResponseException(401, "Sign in to the PLANK broker before connecting");
            }
            const uint16_t dialPort = m_Address.port() == 0 ? 28989 : m_Address.port();
            const QString dialed = consumedDial.isEmpty()
                    ? m_Address.address() + QLatin1Char(':') + QString::number(dialPort)
                    : consumedDial;
            const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
            const QByteArray requestBody = QJsonDocument(QJsonObject{
                {QStringLiteral("address"), dialed},
                {QStringLiteral("request_id"), requestId},
            }).toJson(QJsonDocument::Compact);
            QUrl endpoint(broker);
            endpoint.setPath(QStringLiteral("/request"));
            endpoint.setQuery(QString());
            int status = 0;
            const QByteArray response = postBroker(endpoint, requestBody, session, &status);
            if (status != 200) {
                throwBrokerStatus(status);
            }
            const QJsonDocument document = QJsonDocument::fromJson(response);
            const QJsonObject root = document.object();
            const QString admissionId = root.value(QStringLiteral("admission_id")).toString();
            if (admissionId.size() == 36) {
                reservation.admissionId = admissionId;
                reservation.broker = broker;
                reservation.session = session;
            }
            const QJsonObject bundle = root.value(QStringLiteral("bundle")).toObject();
            const QJsonObject admission = bundle.value(QStringLiteral("admission")).toObject();
            const QString bundleId = bundle.value(QStringLiteral("workstation_uniqueid")).toString();
            if (!document.isObject() || bundle.contains(QStringLiteral("request_id")) ||
                    admission.value(QStringLiteral("v")).toInt() != 1 ||
                    !admission.value(QStringLiteral("payload")).isString() ||
                    !admission.value(QStringLiteral("sig")).isString() ||
                    bundleId.size() != 36 || reservation.admissionId.isEmpty()) {
                throw GfeHttpResponseException(502, "PLANK broker returned an unusable admission");
            }
            const QString routing = bundle.value(QStringLiteral("address")).toString();
            const int colon = routing.lastIndexOf(QLatin1Char(':'));
            bool portOk = false;
            const int routingPort = colon > 0 ? routing.mid(colon + 1).toInt(&portOk) : 0;
            const NvAddress routed(colon > 0 ? routing.left(colon) : QString(),
                                   portOk ? static_cast<uint16_t>(routingPort) : 0);
            if (routed.isNull() || !portOk || routingPort < 1 || routingPort > 65535) {
                throw GfeHttpResponseException(502, "PLANK broker returned an unusable admission");
            }
            setAdmissionBundle(admission, bundleId,
                               bundle.value(QStringLiteral("certificate_sha256")).toString());
            g_admission.admissionId = admissionId;
            g_admission.dialedAddress = dialed;
            setAddress(routed);
            // The identity check dials the address the broker returned.
            m_Nam->clearAccessCache();
        }
    }

    QJsonObject startBody {
        {"username", username},
        {"start_desktop", startDesktop},
    };
    if (g_admission.set) {
        QNetworkReply* reply = openConnection(m_BaseUrlHttps, "serverinfo", nullptr,
                                              REQUEST_TIMEOUT_MS, NvLogLevel::NVLL_NONE);
        const QString certificate = QString::fromLatin1(reply->sslConfiguration().peerCertificate()
                .digest(QCryptographicHash::Sha256).toHex());
        QString serverInfo;
        {
            QTextStream stream(reply);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            stream.setEncoding(QStringConverter::Utf8);
#else
            stream.setCodec("UTF-8");
#endif
            serverInfo = stream.readAll();
        }
        delete reply;
        verifyResponseStatus(serverInfo);
        if (!sameWorkstationUuid(getXmlString(serverInfo, QStringLiteral("uniqueid")),
                                  g_admission.uniqueId) ||
                (!g_admission.certificateSha256.isEmpty() &&
                 certificate.toLower() != g_admission.certificateSha256)) {
            throw GfeHttpResponseException(401, "PLANK host identity does not match the admission");
        }
        startBody.insert(QStringLiteral("admission"), g_admission.wrapper);
    }

    QJsonObject result;
    try {
        result = postPlankJson("start", startBody);
    } catch (const GfeHttpResponseException&) {
        retirePresentedAdmission();
        throw;
    }
    for (int round = 0; round < 16; ++round) {
        const QString state = result.value("state").toString();
        if (state == "authenticated") {
            m_SessionToken = result.value("session_token").toString();
            if (m_SessionToken.isEmpty()) {
                throw GfeHttpResponseException(401, "Authentication returned no session token");
            }
            if (greeterConfirmed != nullptr) {
                *greeterConfirmed = plankAuthenticatedGreeter(result);
            }
            retirePresentedAdmission();
            reservation.keep();
            return m_SessionToken;
        }
        if (state == "denied") {
            retirePresentedAdmission();
            throw GfeHttpResponseException(401, "Operating-system authentication failed");
        }
        if (state == "admission_rejected") {
            retirePresentedAdmission();
            throw GfeHttpResponseException(401, "PLANK admission rejected");
        }
        if (state == "busy") {
            throw GfeHttpResponseException(503, "Host authentication is busy. Please try again shortly.");
        }
        if (state != "challenge" || !result.value("messages").isArray()) {
            throw GfeHttpResponseException(400, "Invalid PAM conversation response");
        }

        QJsonArray responses;
        const QJsonArray messages = result.value("messages").toArray();
        for (const QJsonValue& value : messages) {
            const QJsonObject message = value.toObject();
            switch (message.value("style").toInt()) {
            case 1: // PAM_PROMPT_ECHO_OFF
                responses.append(password);
                break;
            case 2: // PAM_PROMPT_ECHO_ON
                responses.append(username);
                break;
            case 3: // PAM_ERROR_MSG
            case 4: // PAM_TEXT_INFO
                responses.append(QString());
                break;
            default:
                throw GfeHttpResponseException(400, "Unsupported PAM prompt style");
            }
        }
        // The Host already consumed this admission when it accepted auth/start.
        retirePresentedAdmission();
        result = postPlankJson("respond", {
            {"conversation_id", result.value("conversation_id").toString()},
            {"responses", responses},
            {"start_desktop", startDesktop},
        });
    }

    throw GfeHttpResponseException(400, "PAM conversation exceeded the round limit");
}

NvOutputTopology NvHTTP::getOutputTopology(QString* certificateSha256)
{
    if (certificateSha256 != nullptr) certificateSha256->clear();
    if (m_SessionToken.isEmpty()) {
        throw GfeHttpResponseException(400, "Invalid PLANK topology state");
    }
    QScopedPointer<QNetworkReply> reply(openConnection(
                m_BaseUrlHttps, "plank/topology", nullptr,
                REQUEST_TIMEOUT_MS, NvLogLevel::NVLL_VERBOSE));
    const QString response = QString::fromUtf8(reply->readAll());
    const QJsonDocument document = QJsonDocument::fromJson(response.toUtf8());
    if (!document.isObject() && response.trimmed().startsWith(QLatin1Char('<'))) {
        // GameStream authorization failures use an XML status envelope even
        // for this PLANK JSON endpoint. This is expected after a
        // display transition replaces the media worker and its in-memory
        // bearer sessions. Preserve the 401 so the bounded transition loop
        // can authenticate once to the replacement worker.
        verifyResponseStatus(response);
    }
    NvOutputTopology topology;
    QString error;
    if (!document.isObject() ||
            !NvOutputTopology::fromJson(document.object(), topology, &error)) {
        throw GfeHttpResponseException(400,
                                       error.isEmpty() ?
                                           "Malformed PLANK topology response" : error);
    }
    if (certificateSha256 != nullptr) {
        *certificateSha256 = QString::fromLatin1(negotiatedPlankTls(reply.data())
                .peerCertificate().digest(QCryptographicHash::Sha256).toHex());
    }
    return topology;
}

MacPreviewLaunch::Reply NvHTTP::startMacPreview(const NvOutputTopology& topology,
                                              const QString& certificateSha256,
                                              int bitrateKbps, int udpPayloadSize)
{
    const auto body = MacPreviewLaunch::request(topology, bitrateKbps, udpPayloadSize);
    // One-shot launch: even an ambiguous timeout must require fresh auth.
    SecureStringGuard tokenGuard(m_SessionToken);
    const auto object = postPinnedMacJson(QStringLiteral("/plank/launch"), body, certificateSha256);
    MacPreviewLaunch::Reply parsed;
    if (!MacPreviewLaunch::parseReply(object, topology, controlPort(), udpPayloadSize, parsed)) {
        throw GfeHttpResponseException(400, "Invalid Mac preview launch response");
    }
    return parsed;
}

NvOutputTopology NvHTTP::prepareMacDisplay(const QString& mode, const QString& encodingMode, int scale)
{
    const QSize size = NvOutputTopology::macDisplayModeSize(mode);
    const auto request = NvOutputTopology::macDisplayRequest(mode, encodingMode, scale);
    if (request.isEmpty()) {
        throw GfeHttpResponseException(400, "Unsupported Mac desktop resolution");
    }
    QString pin;
    const auto current = getOutputTopology(&pin);
    if (current.featureFlags != NvOutputTopology::FixedCaptureFlags) {
        throw GfeHttpResponseException(400, "Host does not support Mac desktop preparation");
    }
    const auto object = postPinnedMacJson(QStringLiteral("/plank/display"), request, pin);
    NvOutputTopology result;
    if (!NvOutputTopology::fromJson(object, result) ||
            result.featureFlags != NvOutputTopology::FixedCaptureFlags ||
            result.appleEncodingMode != encodingMode) {
        throw GfeHttpResponseException(400, "Mac desktop did not reach the requested resolution");
    }
    if (result.desktopWidth != size.width() || result.desktopHeight != size.height() ||
            result.captureLogicalBounds.size() != QSizeF(size.width() / scale, size.height() / scale)) {
        // Host kept the current Aqua framebuffer (often Jump Desktop on a
        // headless Mini). Stream that geometry instead of inventing another size.
        qWarning() << "Mac desktop stayed" << result.desktopWidth << "x" << result.desktopHeight
                   << "at logical" << result.captureLogicalBounds.size()
                   << "; requested" << size << "scale" << scale << "was not applied";
    }
    return result;
}

QJsonObject NvHTTP::postPinnedMacJson(const QString& path, const QJsonObject& body,
                                    const QString& certificateSha256)
{
    waitForRequestPermission();
    const QByteArray pin = QByteArray::fromHex(certificateSha256.toLatin1());
    if ((path != QLatin1String("/plank/launch") && path != QLatin1String("/plank/display")) ||
            body.isEmpty() || pin.size() != 32 ||
            QString::fromLatin1(pin.toHex()) != certificateSha256 ||
            m_SessionToken.isEmpty() || m_SessionToken.size() > 512 ||
            m_BaseUrlHttps.scheme() != QLatin1String("https") ||
            !m_BaseUrlHttps.userInfo().isEmpty() || m_BaseUrlHttps.port(0) == 0) {
        throw GfeHttpResponseException(400, "Invalid Mac preview launch state");
    }
    for (const QChar character : m_SessionToken) {
        if (character.unicode() < 33 || character.unicode() > 126) {
            throw GfeHttpResponseException(400, "Invalid Mac preview authorization");
        }
    }

    QUrl url(m_BaseUrlHttps);
    url.setPath(path);
    url.setQuery(QString());
    url.setFragment(QString());
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", "Bearer " + m_SessionToken.toLatin1());
    request.setSslConfiguration(plankSslConfiguration());
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

    // A fresh manager guarantees the TLS encrypted signal before sending data;
    // reused connections are not guaranteed to emit it. Never send the bearer
    // token to a replacement certificate merely because it has PLANK's shape.
    QNetworkAccessManager manager;
    manager.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    rememberPlankTls(&manager, &manager);
    bool certificateChecked = false;
    bool certificateRejected = false;
    auto matchesPin = [&pin](QNetworkReply* reply) {
        const auto ssl = negotiatedPlankTls(reply);
        return isPlankCertificate(ssl.peerCertificate()) &&
                ssl.sessionProtocol() == QSsl::TlsV1_3 &&
                ssl.peerCertificate().digest(QCryptographicHash::Sha256) == pin;
    };
    connect(&manager, &QNetworkAccessManager::sslErrors, &manager,
            [this, &pin](QNetworkReply* reply, const QList<QSslError>& errors) {
        if (reply->sslConfiguration().peerCertificate().digest(QCryptographicHash::Sha256) == pin) {
            handleSslErrors(reply, errors);
        }
    });
    connect(&manager, &QNetworkAccessManager::encrypted, &manager, [&](QNetworkReply* reply) {
        certificateChecked = matchesPin(reply);
        if (!certificateChecked) { certificateRejected = true; reply->abort(); }
    });
    QScopedPointer<QNetworkReply> reply(manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact)));
    constexpr qint64 MaximumReplyBytes = 32768;
    reply->setReadBufferSize(MaximumReplyBytes + 1);
    QByteArray response;
    bool oversized = false;
    auto drain = [&]() {
        response += reply->read(MaximumReplyBytes + 1 - response.size());
        if (response.size() > MaximumReplyBytes) {
            oversized = true;
            reply->abort();
        }
    };
    QEventLoop loop;
    connect(reply.data(), &QNetworkReply::readyRead, &loop, drain);
    connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &loop, &QEventLoop::quit);
    QTimer::singleShot(path == QLatin1String("/plank/display") ? 10000 : REQUEST_TIMEOUT_MS,
                      &loop, &QEventLoop::quit);
    if (!reply->isFinished()) loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (!reply->isFinished()) reply->abort();
    if (!oversized) drain();
    if (!certificateChecked && !certificateRejected &&
            reply->error() != QNetworkReply::NoError &&
            reply->error() != QNetworkReply::SslHandshakeFailedError) {
        throw QtNetworkReplyException(reply->error(), "Mac control connection failed or timed out");
    }
    if (!certificateChecked || !matchesPin(reply.data())) {
        throw QtNetworkReplyException(QNetworkReply::SslHandshakeFailedError,
                                      "Mac preview TLS certificate changed or was rejected");
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (oversized) throw GfeHttpResponseException(400, "Mac preview response exceeded its size limit");
    if (status != 200 && status != 0) {
        // Do not expose arbitrary server text, redirect URLs, or response tokens.
        const auto failure = QJsonDocument::fromJson(response).object();
        response.fill('\0');
        if (status == 403 && failure.value(QStringLiteral("state")) == QLatin1String("denied") &&
                failure.value(QStringLiteral("error")) == QLatin1String("host_permissions_required")) {
            throw GfeHttpResponseException(status,
                "PLANK Host requires macOS permissions. On the Mac, open PLANK Host in Applications "
                "and approve Screen Recording and Accessibility in System Settings > Privacy & Security. "
                "If already enabled, the permissions may belong to an earlier signed build.");
        }
        throw GfeHttpResponseException(status, path == QLatin1String("/plank/display") ?
            "Mac desktop resolution change was not accepted" : "Mac stream launch was not accepted");
    }
    if (reply->error() != QNetworkReply::NoError) {
        throw QtNetworkReplyException(reply->error(), "Mac preview launch failed or timed out");
    }
    QJsonParseError parseError {};
    const auto document = QJsonDocument::fromJson(response, &parseError);
    response.fill('\0');
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        throw GfeHttpResponseException(400, "Invalid Mac control response");
    }
    return document.object();
}

void NvHTTP::waitForRequestPermission(bool authenticating)
{
    if (m_RequestGate && !m_RequestGate(authenticating)) {
        throw QtNetworkReplyException(QNetworkReply::OperationCanceledError, "PLANK reconnect cancelled");
    }
}

QNetworkReply*
NvHTTP::openConnection(QUrl baseUrl,
                       QString command,
                       QString arguments,
                       int timeoutMs,
                       NvLogLevel logLevel)
{
    waitForRequestPermission();
    // Port must be set
    Q_ASSERT(baseUrl.port(0) != 0);

    // Build a URL for the request
    QUrl url(baseUrl);
    url.setPath("/" + command);

    // Only operation parameters belong in the query. PLANK authorization is
    // carried separately; discovery and topology need no client ID/cache nonce.
    url.setQuery(arguments);

    QNetworkRequest request(url);

    if (baseUrl.scheme() == "https") {
        request.setSslConfiguration(plankSslConfiguration());
        if (!m_SessionToken.isEmpty()) {
            request.setRawHeader("Authorization", "Bearer " + m_SessionToken.toUtf8());
        }
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Disable HTTP/2 (GFE 3.22 doesn't like it) and Qt 6 enables it by default
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    // Use fine-grained idle timeouts to avoid calling QNetworkAccessManager::clearAccessCache(),
    // which tears down the NAM's global thread each time. We must not keep persistent connections
    // or GFE will puke.
    request.setAttribute(QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute, 0);
#endif

    auto sslErrorsConnection = connect(m_Nam, &QNetworkAccessManager::sslErrors, this, &NvHTTP::handleSslErrors);
    const auto encryptedConnection = rememberPlankTls(m_Nam, this);
    QNetworkReply* reply = m_Nam->get(request);

    // Run the request with a timeout if requested
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &loop, &QEventLoop::quit);
    if (timeoutMs) {
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    }
    if (logLevel >= NvLogLevel::NVLL_VERBOSE) {
        qInfo() << "Executing request:" << url.toString();
    }
    loop.exec(QEventLoop::ExcludeUserInputEvents);

    // Abort the request if it timed out
    if (!reply->isFinished())
    {
        if (logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << "Aborting timed out request for" << url.toString();
        }
        reply->abort();
    }

#if QT_VERSION < QT_VERSION_CHECK(6, 3, 0)
    // If we couldn't use fine-grained connection idle timeouts, kill them all now
    m_Nam->clearAccessCache();
#endif
    disconnect(sslErrorsConnection);

    disconnect(encryptedConnection);

    // Handle error
    if (reply->error() != QNetworkReply::NoError)
    {
        if (logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << command << "request failed with error:" << reply->error()
                       << reply->errorString();
        }

        if (reply->error() == QNetworkReply::SslHandshakeFailedError) {
            QtNetworkReplyException exception(QNetworkReply::SslHandshakeFailedError, "PLANK TLS validation failed");
            delete reply;
            throw exception;
        }
        else if (reply->error() == QNetworkReply::OperationCanceledError) {
            QtNetworkReplyException exception(QNetworkReply::TimeoutError, "Request timed out");
            delete reply;
            throw exception;
        }
        else {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status >= 400 && (command == QLatin1String("plank/topology") ||
                                 command == QLatin1String("applist"))) {
                delete reply;
                throw GfeHttpResponseException(status, "PLANK desktop readiness request rejected");
            }
            QtNetworkReplyException exception(reply->error(), reply->errorString());
            delete reply;
            throw exception;
        }
    }

    const bool plankTls = baseUrl.scheme() == "https";
    const bool approvedCertificate = !plankTls ||
            isPlankCertificate(negotiatedPlankTls(reply).peerCertificate());
    const bool approvedProtocol = !plankTls ||
            negotiatedPlankTls(reply).sessionProtocol() == QSsl::TlsV1_3;
    if (!approvedCertificate || !approvedProtocol) {
        qWarning() << "Rejecting PLANK TLS session"
                   << "certificate" << approvedCertificate
                   << "tls13" << approvedProtocol
                   << "protocol" << reply->sslConfiguration().sessionProtocol()
                   << "cipherProtocol" << reply->sslConfiguration().sessionCipher().protocol();
        QtNetworkReplyException exception(QNetworkReply::SslHandshakeFailedError, "Invalid PLANK TLS session");
        delete reply;
        throw exception;
    }

    return reply;
}
