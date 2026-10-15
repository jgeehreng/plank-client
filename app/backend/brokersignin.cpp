#include "brokersignin.h"

#include "brokeraddress.h"
#include "nvhttp.h"
#include "settings/plankclientpolicy.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>

namespace {

constexpr int RequestTimeoutMs = 20000;
// A Duo push stays open for about a minute.
constexpr int PushPollMs = 2000;
constexpr qint64 PushWaitMs = 90000;

const QString KeyName = QStringLiteral("name");
const QString KeyUrl = QStringLiteral("url");
const QString KeyUsername = QStringLiteral("username");
const QString KeyKind = QStringLiteral("kind");
const QString KeyHost = QStringLiteral("host");
const QString KindBroker = QStringLiteral("broker");
const QString KindWorkstation = QStringLiteral("workstation");

void wipe(QString& value)
{
    value.fill(QChar(0));
    value.clear();
}

bool usableSession(const QString& session)
{
    return !session.isEmpty() && session.size() <= 64 && !session.contains(QLatin1Char(' '));
}

}

BrokerSignIn::BrokerSignIn(QObject* parent)
    : QObject(parent)
{
    m_PollTimer.setInterval(PushPollMs);
    connect(&m_PollTimer, &QTimer::timeout, this, &BrokerSignIn::pollPush);
    if (!load().isEmpty()) {
        selectConnection(0);
    }
}

BrokerSignIn::~BrokerSignIn()
{
    forgetPassword();
}

QVector<BrokerSignIn::Saved> BrokerSignIn::load() const
{
    QVector<Saved> saved;
    QSettings settings;
    const int count = settings.beginReadArray(QStringLiteral(PLANK_BROKER_CONNECTIONS_KEY));
    for (int i = 0; i < count && i < 64; ++i) {
        settings.setArrayIndex(i);
        QString error;
        Saved entry;
        if (settings.value(KeyKind).toString() == KindWorkstation) {
            entry.host = BrokerAddress::workstation(settings.value(KeyHost).toString(), &error);
            if (entry.host.isEmpty()) {
                continue;
            }
        } else {
            entry.url = BrokerAddress::normalize(settings.value(KeyUrl).toString(), &error);
            if (!entry.url.isValid()) {
                continue;
            }
        }
        entry.name = settings.value(KeyName).toString().trimmed().left(64);
        if (entry.name.isEmpty()) {
            entry.name = entry.address();
        }
        entry.username = rememberUsernameAllowed() ? settings.value(KeyUsername).toString().left(128) : QString();
        saved.append(entry);
    }
    settings.endArray();
    return saved;
}

void BrokerSignIn::store(const QVector<Saved>& saved)
{
    QSettings settings;
    settings.remove(QStringLiteral(PLANK_BROKER_CONNECTIONS_KEY));
    settings.beginWriteArray(QStringLiteral(PLANK_BROKER_CONNECTIONS_KEY), saved.size());
    for (int i = 0; i < saved.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue(KeyName, saved[i].name);
        if (saved[i].workstation()) {
            settings.setValue(KeyKind, KindWorkstation);
            settings.setValue(KeyHost, saved[i].host);
        } else {
            settings.setValue(KeyUrl, saved[i].url.toString());
        }
        if (!saved[i].username.isEmpty() && rememberUsernameAllowed()) {
            settings.setValue(KeyUsername, saved[i].username);
        }
    }
    settings.endArray();
    emit connectionsChanged();
}

QVariantList BrokerSignIn::connections() const
{
    QVariantList list;
    for (const Saved& entry : load()) {
        QVariantMap row;
        row.insert(KeyName, entry.name);
        row.insert(QStringLiteral("address"), entry.address());
        row.insert(KeyKind, entry.workstation() ? KindWorkstation : KindBroker);
        list.append(row);
    }
    return list;
}

QString BrokerSignIn::Saved::address() const
{
    return workstation() ? host : BrokerAddress::display(url);
}

QString BrokerSignIn::connectionName() const
{
    const QVector<Saved> saved = load();
    return m_Selected >= 0 && m_Selected < saved.size() ? saved[m_Selected].name : QString();
}

QString BrokerSignIn::connectionKind() const
{
    const QVector<Saved> saved = load();
    return m_Selected >= 0 && m_Selected < saved.size() && saved[m_Selected].workstation()
            ? KindWorkstation : KindBroker;
}

QString BrokerSignIn::connectionAddress() const
{
    const QVector<Saved> saved = load();
    return m_Selected >= 0 && m_Selected < saved.size() ? saved[m_Selected].address() : QString();
}

bool BrokerSignIn::rememberUsernameAllowed() const
{
    return PlankClientPolicy().rememberUsername();
}

QString BrokerSignIn::saveConnection(int index, const QString& name, const QString& address,
                                     const QString& kind)
{
    QString error;
    const bool workstation = kind == KindWorkstation;
    QUrl url;
    QString host;
    if (workstation) {
        host = BrokerAddress::workstation(address, &error);
        if (host.isEmpty()) {
            return error;
        }
    } else {
        url = BrokerAddress::normalize(address, &error);
        if (!url.isValid()) {
            return error;
        }
    }
    QVector<Saved> saved = load();
    if (index >= saved.size()) {
        return QStringLiteral("That connection no longer exists.");
    }
    Saved entry;
    if (index >= 0) {
        entry = saved[index];
        if (entry.url != url || entry.host != host) {
            entry.username.clear();
        }
    }
    entry.url = url;
    entry.host = host;
    entry.name = name.trimmed().left(64);
    if (entry.name.isEmpty()) {
        entry.name = entry.address();
    }
    if (index >= 0) {
        saved[index] = entry;
    } else {
        saved.append(entry);
    }
    store(saved);
    if (index < 0 || index == m_Selected) {
        selectConnection(index < 0 ? saved.size() - 1 : index);
    }
    return QString();
}

void BrokerSignIn::removeConnection(int index)
{
    QVector<Saved> saved = load();
    if (index < 0 || index >= saved.size()) {
        return;
    }
    saved.removeAt(index);
    store(saved);
    if (index == m_Selected) {
        signOut();
        m_Selected = -1;
        NvHTTP::setActiveBroker(QUrl());
        emit selectionChanged();
    } else if (index < m_Selected) {
        --m_Selected;
        emit selectionChanged();
    }
}

void BrokerSignIn::selectConnection(int index)
{
    const QVector<Saved> saved = load();
    if (index < 0 || index >= saved.size()) {
        return;
    }
    cancel();
    m_Selected = index;
    // A workstation connection has no broker: no admission, no Duo.
    const QUrl url = saved[index].url;
    NvHTTP::setActiveBroker(url);
    if (saved[index].workstation()) {
        clearError();
        emit selectionChanged();
        return;
    }
    if (BrokerAddress::isLoopbackHost(url.host())) {
        m_Network.setProxy(QNetworkProxy::NoProxy);
    } else {
        m_Network.setProxy(QNetworkProxy::DefaultProxy);
    }
    clearError();
    emit selectionChanged();
}

QString BrokerSignIn::rememberedUsername(int index) const
{
    const QVector<Saved> saved = load();
    return index >= 0 && index < saved.size() ? saved[index].username : QString();
}

QUrl BrokerSignIn::brokerUrl() const
{
    const QVector<Saved> saved = load();
    return m_Selected >= 0 && m_Selected < saved.size() ? saved[m_Selected].url : QUrl();
}

void BrokerSignIn::setStep(const QString& step)
{
    if (m_Step != step) {
        m_Step = step;
    }
    emit stepChanged();
}

void BrokerSignIn::clearError()
{
    if (!m_ErrorText.isEmpty()) {
        m_ErrorText.clear();
        emit errorTextChanged();
    }
}

void BrokerSignIn::fail(const QString& text, const QString& nextStep)
{
    m_ErrorText = text;
    emit errorTextChanged();
    if (nextStep == QLatin1String("credentials")) {
        forgetPassword();
        m_MfaId.clear();
        m_Factors.clear();
        m_VerificationCode.clear();
    }
    setStep(nextStep);
}

void BrokerSignIn::forgetPassword()
{
    wipe(m_Password);
}

void BrokerSignIn::stopWaiting()
{
    m_PollTimer.stop();
    m_PushStartedMs = 0;
}

void BrokerSignIn::post(const QString& path, const QJsonObject& body,
                        void (BrokerSignIn::*handler)(int, const QJsonObject&))
{
    QUrl endpoint = brokerUrl();
    if (!endpoint.isValid()) {
        fail(tr("Choose a saved connection first."), QStringLiteral("credentials"));
        return;
    }
    endpoint.setPath(path);
    QNetworkRequest request(endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(RequestTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    if (m_Pending) {
        m_Pending->disconnect(this);
        m_Pending->abort();
        m_Pending->deleteLater();
    }
    QNetworkReply* reply = m_Network.post(request, payload);
    payload.fill('\0');
    m_Pending = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, handler]() {
        if (m_Pending == reply) {
            m_Pending = nullptr;
        }
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonObject parsed = QJsonDocument::fromJson(reply->read(65536)).object();
        if (status == 0) {
            parsed.insert(QStringLiteral("network_error"), static_cast<int>(reply->error()));
        }
        (this->*handler)(status, parsed);
    });
}

QString BrokerSignIn::describe(int status, const QJsonObject& reply) const
{
    const QString error = reply.value(QStringLiteral("error")).toString();
    if (status == 0) {
        if (reply.value(QStringLiteral("network_error")).toInt() == QNetworkReply::SslHandshakeFailedError) {
            return tr("This computer doesn't trust the broker's certificate.");
        }
        return tr("Couldn't reach %1. Check the address and your network.")
                .arg(BrokerAddress::display(brokerUrl()));
    }
    if (status == 429) {
        return tr("Too many failed sign-ins. Try again in 15 minutes.");
    }
    if (status == 410) {
        return tr("The sign-in expired. Sign in again.");
    }
    if (status == 401 && error == QLatin1String("passcode_rejected")) {
        return tr("That passcode wasn't accepted.");
    }
    if (status == 401) {
        return tr("Sign-in failed. Check your username and password.");
    }
    if (status == 503) {
        return tr("The broker couldn't reach the directory or Duo. Try again.");
    }
    return tr("The broker refused the sign-in (%1).").arg(status);
}

void BrokerSignIn::signIn(const QString& username, const QString& password, bool rememberUsername)
{
    const QString name = username.trimmed();
    if (name.isEmpty() || password.isEmpty()) {
        fail(tr("Enter your username and password."), QStringLiteral("credentials"));
        return;
    }
    if (m_Step != QLatin1String("credentials")) {
        return;
    }
    clearError();
    m_Username = name;
    m_Password = password;
    m_RememberUsername = rememberUsername && rememberUsernameAllowed();
    if (connectionKind() == KindWorkstation) {
        // No broker to ask: the workstation itself checks the password (and
        // its own PAM policy) when the client signs in to it next.
        NvHTTP::clearFacilitySignIn();
        NvHTTP::holdFacilityCredentials(m_Username, m_Password);
        forgetPassword();
        rememberSignedInUsername();
        setStep(QStringLiteral("signed_in"));
        emit signedIn();
        return;
    }
    setStep(QStringLiteral("signing_in"));
    post(QStringLiteral("/auth/ldap"),
         QJsonObject{{QStringLiteral("username"), name}, {QStringLiteral("password"), password}},
         &BrokerSignIn::onPasswordReply);
}

void BrokerSignIn::onPasswordReply(int status, const QJsonObject& reply)
{
    const QString state = reply.value(QStringLiteral("state")).toString();
    if (status == 200 && state == QLatin1String("signed_in")) {
        finishSignIn(reply);
        return;
    }
    if (status == 200 && state == QLatin1String("mfa")) {
        m_MfaId = reply.value(QStringLiteral("mfa_id")).toString().left(64);
        m_Factors.clear();
        for (const QJsonValue& factor : reply.value(QStringLiteral("factors")).toArray()) {
            const QString text = factor.toString();
            if (text == QLatin1String("push") || text == QLatin1String("passcode")) {
                m_Factors.append(text);
            }
        }
        m_VerificationCode = reply.value(QStringLiteral("verification_code")).toString().left(16);
        if (m_MfaId.isEmpty() || m_Factors.isEmpty()) {
            fail(tr("The broker sent an unusable reply."), QStringLiteral("credentials"));
            return;
        }
        setStep(QStringLiteral("mfa"));
        return;
    }
    fail(describe(status, reply), QStringLiteral("credentials"));
}

void BrokerSignIn::sendPush()
{
    if ((m_Step != QLatin1String("mfa") && m_Step != QLatin1String("waiting_push")) ||
            !m_Factors.contains(QStringLiteral("push"))) {
        return;
    }
    stopWaiting();
    clearError();
    setStep(QStringLiteral("sending_push"));
    post(QStringLiteral("/auth/duo/push"), QJsonObject{{QStringLiteral("mfa_id"), m_MfaId}},
         &BrokerSignIn::onPushReply);
}

void BrokerSignIn::onPushReply(int status, const QJsonObject& reply)
{
    if (status == 200 && reply.value(QStringLiteral("state")).toString() == QLatin1String("waiting")) {
        m_PushStartedMs = QDateTime::currentMSecsSinceEpoch();
        setStep(QStringLiteral("waiting_push"));
        m_PollTimer.start();
        return;
    }
    fail(describe(status, reply), status == 0 ? QStringLiteral("mfa") : QStringLiteral("credentials"));
}

void BrokerSignIn::pollPush()
{
    if (m_Step != QLatin1String("waiting_push")) {
        stopWaiting();
        return;
    }
    if (m_Pending) {
        return;
    }
    if (QDateTime::currentMSecsSinceEpoch() - m_PushStartedMs > PushWaitMs) {
        stopWaiting();
        fail(tr("Duo didn't get an answer. Send another push or enter a passcode."), QStringLiteral("mfa"));
        return;
    }
    post(QStringLiteral("/auth/duo/status"), QJsonObject{{QStringLiteral("mfa_id"), m_MfaId}},
         &BrokerSignIn::onStatusReply);
}

void BrokerSignIn::onStatusReply(int status, const QJsonObject& reply)
{
    if (m_Step != QLatin1String("waiting_push")) {
        return;
    }
    const QString state = reply.value(QStringLiteral("state")).toString();
    if (status == 200 && state == QLatin1String("waiting")) {
        return;
    }
    if (status == 0) {
        // A dropped poll is retried until the push times out.
        return;
    }
    stopWaiting();
    if (status == 200 && state == QLatin1String("signed_in")) {
        finishSignIn(reply);
        return;
    }
    if (status == 401) {
        fail(tr("Duo didn't approve the sign-in."), QStringLiteral("credentials"));
        return;
    }
    fail(describe(status, reply), QStringLiteral("credentials"));
}

void BrokerSignIn::submitPasscode(const QString& passcode)
{
    const QString code = passcode.trimmed();
    if ((m_Step != QLatin1String("mfa") && m_Step != QLatin1String("waiting_push")) ||
            !m_Factors.contains(QStringLiteral("passcode"))) {
        return;
    }
    static const QRegularExpression digits(QStringLiteral("^[0-9]{4,16}$"));
    if (!digits.match(code).hasMatch()) {
        m_ErrorText = tr("A passcode is a number from Duo Mobile, a text message or a token.");
        emit errorTextChanged();
        return;
    }
    stopWaiting();
    clearError();
    setStep(QStringLiteral("checking_passcode"));
    post(QStringLiteral("/auth/duo/passcode"),
         QJsonObject{{QStringLiteral("mfa_id"), m_MfaId}, {QStringLiteral("passcode"), code}},
         &BrokerSignIn::onPasscodeReply);
}

void BrokerSignIn::onPasscodeReply(int status, const QJsonObject& reply)
{
    const QString error = reply.value(QStringLiteral("error")).toString();
    if (status == 200 && reply.value(QStringLiteral("state")).toString() == QLatin1String("signed_in")) {
        finishSignIn(reply);
        return;
    }
    if (status == 401 && error == QLatin1String("passcode_rejected")) {
        fail(describe(status, reply), QStringLiteral("mfa"));
        return;
    }
    if (status == 401) {
        fail(tr("Too many wrong passcodes. Sign in again."), QStringLiteral("credentials"));
        return;
    }
    fail(describe(status, reply), status == 0 ? QStringLiteral("mfa") : QStringLiteral("credentials"));
}

void BrokerSignIn::finishSignIn(const QJsonObject& reply)
{
    const QString session = reply.value(QStringLiteral("session_id")).toString();
    if (!usableSession(session)) {
        fail(tr("The broker sent an unusable reply."), QStringLiteral("credentials"));
        return;
    }
    NvHTTP::setFacilitySession(session);
    NvHTTP::holdFacilityCredentials(m_Username, m_Password);
    forgetPassword();
    m_MfaId.clear();
    rememberSignedInUsername();
    setStep(QStringLiteral("signed_in"));
    emit signedIn();
}

void BrokerSignIn::rememberSignedInUsername()
{
    QVector<Saved> saved = load();
    if (m_Selected >= 0 && m_Selected < saved.size()) {
        const QString remembered = m_RememberUsername ? m_Username : QString();
        if (saved[m_Selected].username != remembered) {
            saved[m_Selected].username = remembered;
            store(saved);
        }
    }
}

void BrokerSignIn::cancel()
{
    stopWaiting();
    if (m_Pending) {
        m_Pending->disconnect(this);
        m_Pending->abort();
        m_Pending->deleteLater();
        m_Pending = nullptr;
    }
    if (!m_MfaId.isEmpty()) {
        QUrl endpoint = brokerUrl();
        if (endpoint.isValid()) {
            endpoint.setPath(QStringLiteral("/auth/duo/cancel"));
            QNetworkRequest request(endpoint);
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            request.setTransferTimeout(RequestTimeoutMs);
            QNetworkReply* reply = m_Network.post(request, QJsonDocument(QJsonObject{
                    {QStringLiteral("mfa_id"), m_MfaId}}).toJson(QJsonDocument::Compact));
            connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
        }
    }
    forgetPassword();
    m_MfaId.clear();
    m_Factors.clear();
    m_VerificationCode.clear();
    if (m_Step != QLatin1String("signed_in")) {
        setStep(QStringLiteral("credentials"));
    }
}

void BrokerSignIn::signOut()
{
    cancel();
    NvHTTP::clearFacilitySignIn();
    clearError();
    setStep(QStringLiteral("credentials"));
}
