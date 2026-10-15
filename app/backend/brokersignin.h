#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVector>

class QNetworkReply;

// Uppercut: signs a person in to a facility broker the way the PCoIP client
// does with Duo. Pick a saved connection, enter a username and password, then
// choose Send Me a Push or enter a passcode. The password goes only to the
// broker (over HTTPS unless the broker is on this computer) and is kept in
// memory once, for the workstation sign-in that follows.
class BrokerSignIn : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantList connections READ connections NOTIFY connectionsChanged)
    Q_PROPERTY(int selectedConnection READ selectedConnection NOTIFY selectionChanged)
    Q_PROPERTY(QString connectionName READ connectionName NOTIFY selectionChanged)
    // "broker" (password, Duo, then the account's workstations) or
    // "workstation" (one PLANK host on the office network, password only).
    Q_PROPERTY(QString connectionKind READ connectionKind NOTIFY selectionChanged)
    Q_PROPERTY(QString connectionAddress READ connectionAddress NOTIFY selectionChanged)
    // "credentials", "signing_in", "mfa", "sending_push", "waiting_push",
    // "checking_passcode" or "signed_in".
    Q_PROPERTY(QString step READ step NOTIFY stepChanged)
    Q_PROPERTY(QString username READ username NOTIFY stepChanged)
    Q_PROPERTY(QStringList factors READ factors NOTIFY stepChanged)
    Q_PROPERTY(QString verificationCode READ verificationCode NOTIFY stepChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)
    Q_PROPERTY(bool rememberUsernameAllowed READ rememberUsernameAllowed CONSTANT)

public:
    explicit BrokerSignIn(QObject* parent = nullptr);
    ~BrokerSignIn() override;

    QVariantList connections() const;
    int selectedConnection() const { return m_Selected; }
    QString connectionName() const;
    QString connectionKind() const;
    QString connectionAddress() const;
    QString step() const { return m_Step; }
    QString username() const { return m_Username; }
    QStringList factors() const { return m_Factors; }
    QString verificationCode() const { return m_VerificationCode; }
    QString errorText() const { return m_ErrorText; }
    bool rememberUsernameAllowed() const;

    // index -1 adds a connection. kind is "broker" or "workstation".
    // Returns an error message, or "" when saved.
    Q_INVOKABLE QString saveConnection(int index, const QString& name, const QString& address,
                                       const QString& kind);
    Q_INVOKABLE void removeConnection(int index);
    // Makes this connection the active broker and returns to the password step.
    Q_INVOKABLE void selectConnection(int index);
    Q_INVOKABLE QString rememberedUsername(int index) const;
    Q_INVOKABLE void signIn(const QString& username, const QString& password, bool rememberUsername);
    Q_INVOKABLE void sendPush();
    Q_INVOKABLE void submitPasscode(const QString& passcode);
    // Leaves the Duo step without signing in.
    Q_INVOKABLE void cancel();
    // Forgets the facility session and any held password.
    Q_INVOKABLE void signOut();
    Q_INVOKABLE void clearError();

signals:
    void connectionsChanged();
    void selectionChanged();
    void stepChanged();
    void errorTextChanged();
    // The facility session is open; the workstation list can be fetched.
    void signedIn();

private:
    struct Saved {
        QString name;
        QUrl url;          // a broker
        QString host;      // a workstation: "host" or "host:port"
        QString username;
        bool workstation() const { return !host.isEmpty(); }
        QString address() const;
    };

    QVector<Saved> load() const;
    void store(const QVector<Saved>& saved);
    QUrl brokerUrl() const;
    void post(const QString& path, const QJsonObject& body,
              void (BrokerSignIn::*handler)(int, const QJsonObject&));
    void setStep(const QString& step);
    void fail(const QString& text, const QString& nextStep);
    void finishSignIn(const QJsonObject& reply);
    void rememberSignedInUsername();
    void forgetPassword();
    void stopWaiting();

    void onPasswordReply(int status, const QJsonObject& reply);
    void onPushReply(int status, const QJsonObject& reply);
    void onStatusReply(int status, const QJsonObject& reply);
    void onPasscodeReply(int status, const QJsonObject& reply);
    void pollPush();
    QString describe(int status, const QJsonObject& reply) const;

    QNetworkAccessManager m_Network;
    QPointer<QNetworkReply> m_Pending;
    QTimer m_PollTimer;
    qint64 m_PushStartedMs = 0;
    int m_Selected = -1;
    QString m_Step = QStringLiteral("credentials");
    QString m_Username;
    QString m_Password;
    bool m_RememberUsername = false;
    QString m_MfaId;
    QStringList m_Factors;
    QString m_VerificationCode;
    QString m_ErrorText;
};
