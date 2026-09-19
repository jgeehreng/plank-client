#pragma once

#include "nvapp.h"
#include "nvaddress.h"
#include "outputtopology.h"
#include "macpreviewlaunch.h"

#include <Limelight.h>

#include <QUrl>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>
#include <functional>

class NvComputer;

class NvDisplayMode
{
public:
    bool operator==(const NvDisplayMode& other) const
    {
        return width == other.width &&
                height == other.height &&
                refreshRate == other.refreshRate;
    }

    int width;
    int height;
    int refreshRate;
};
Q_DECLARE_TYPEINFO(NvDisplayMode, Q_PRIMITIVE_TYPE);

class GfeHttpResponseException : public std::exception
{
public:
    GfeHttpResponseException(int statusCode, QString message) :
        m_StatusCode(statusCode),
        m_StatusMessage(message.toUtf8())
    {

    }

    const char* what() const throw()
    {
        return m_StatusMessage.constData();
    }

    const char* getStatusMessage() const
    {
        return m_StatusMessage.constData();
    }

    int getStatusCode() const
    {
        return m_StatusCode;
    }

    QString toQString() const
    {
        return QString::fromUtf8(m_StatusMessage) + " (Error " + QString::number(m_StatusCode) + ")";
    }

private:
    int m_StatusCode;
    QByteArray m_StatusMessage;
};

class QtNetworkReplyException : public std::exception
{
public:
    QtNetworkReplyException(QNetworkReply::NetworkError error, QString errorText) :
        m_Error(error),
        m_ErrorText(errorText.toUtf8())
    {

    }

    const char* what() const throw()
    {
        return m_ErrorText.constData();
    }

    const char* getErrorText() const
    {
        return m_ErrorText.constData();
    }

    QNetworkReply::NetworkError getError() const
    {
        return m_Error;
    }

    QString toQString() const
    {
        return QString::fromUtf8(m_ErrorText) + " (Error " + QString::number(m_Error) + ")";
    }

private:
    QNetworkReply::NetworkError m_Error;
    QByteArray m_ErrorText;
};

class NvHTTP : public QObject
{
    Q_OBJECT

public:
    enum NvLogLevel {
        NVLL_NONE,
        NVLL_ERROR,
        NVLL_VERBOSE
    };

    explicit NvHTTP(NvAddress address, QNetworkAccessManager* nam = nullptr);

    explicit NvHTTP(NvComputer* computer, QNetworkAccessManager* nam = nullptr);

    static
    int
    getCurrentGame(QString serverInfo);

    QString
    getServerInfo(NvLogLevel logLevel, bool fastFail = false);

    static
    void
    verifyResponseStatus(QString xml);

    static
    QString
    getXmlString(QString xml,
                 QString tagName);

    static
    QByteArray
    getXmlStringFromHex(QString xml,
                        QString tagName);

    QString
    openConnectionToString(QUrl baseUrl,
                           QString command,
                           QString arguments,
                           int timeoutMs,
                           NvLogLevel logLevel = NvLogLevel::NVLL_VERBOSE);

    void setAddress(NvAddress address);

    void setPlankSessionToken(QString sessionToken);

    // Used only by the session recovery worker; ordinary discovery/login has
    // no gate. False cancels, while the callback may wait for a local decision.
    void setRequestGate(std::function<bool(bool)> gate) { m_RequestGate = std::move(gate); }

    QString authenticate(QString username, QString password, bool* greeterConfirmed = nullptr,
                         bool startDesktop = true);
    bool probeWorkerReplacement(const QString& instance, const QString& certificateSha256);
    QString workerInstance() const { return m_WorkerInstance; }
    NvOutputTopology getOutputTopology(QString* certificateSha256 = nullptr);
    NvOutputTopology prepareMacDisplay(const QString& mode, const QString& encodingMode, int scale = 1);
    MacPreviewLaunch::Reply startMacPreview(const NvOutputTopology& topology,
                                           const QString& certificateSha256,
                                           int bitrateKbps, int udpPayloadSize);

    NvAddress address();

    uint16_t controlPort();

    static
    QVector<int>
    parseQuad(QString quad);

    void
    startApp(QString verb,
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
             QString& acceptedEncodingMode);

    QVector<NvApp>
    getAppList();

    QImage
    getBoxArt(int appId);

    static
    QVector<NvDisplayMode>
    getDisplayModeList(QString serverInfo);

    QUrl m_BaseUrlHttps;
private:
    void waitForRequestPermission(bool authenticating = false);
    void
    handleSslErrors(QNetworkReply* reply, const QList<QSslError>& errors);

    QNetworkReply*
    openConnection(QUrl baseUrl,
                   QString command,
                   QString arguments,
                   int timeoutMs,
                   NvLogLevel logLevel);

    QJsonObject postPlankJson(QString command, const QJsonObject& body);
    QJsonObject postPinnedMacJson(const QString& path, const QJsonObject& body,
                                 const QString& certificateSha256);

    NvAddress m_Address;
    QNetworkAccessManager* m_Nam;
    QString m_SessionToken;
    QString m_WorkerInstance;
    std::function<bool(bool)> m_RequestGate;
};
