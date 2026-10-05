#pragma once

#include "nvcomputer.h"
#include "settings/streamingpreferences.h"

#include <qmdnsengine/server.h>
#include <qmdnsengine/cache.h>
#include <qmdnsengine/browser.h>
#include <qmdnsengine/service.h>
#include <qmdnsengine/resolver.h>

#include <QThread>
#include <QReadWriteLock>
#include <QMutex>
#include <QSettings>
#include <QRunnable>
#include <QTimer>
#include <QWaitCondition>

class ComputerManager;

class DelayedFlushThread : public QThread
{
    Q_OBJECT

public:
    DelayedFlushThread(ComputerManager* cm)
        : m_ComputerManager(cm)
    {
        setObjectName("CM Delayed Flush Thread");
    }

    void run();

private:
    ComputerManager* m_ComputerManager;
};

class MdnsPendingComputer : public QObject
{
    Q_OBJECT

public:
    explicit MdnsPendingComputer(const QSharedPointer<QMdnsEngine::Server> server,
                                 const QMdnsEngine::Service& service)
        : m_Hostname(service.hostname()),
          m_Port(service.port()),
          m_ServerWeak(server),
          m_Resolver(nullptr)
    {
        // Start resolving
        resolve();
    }

    virtual ~MdnsPendingComputer()
    {
        delete m_Resolver;
    }

    QString hostname()
    {
        return m_Hostname;
    }

    uint16_t port()
    {
        return m_Port;
    }

private slots:
    void handleResolvedTimeout()
    {
        if (m_Addresses.isEmpty()) {
            if (m_Retries-- > 0) {
                // Try again
                qInfo() << "Resolving" << hostname() << "timed out. Retrying...";
                resolve();
            }
            else {
                qWarning() << "Giving up on resolving" << hostname() << "after repeated failures";
                cleanup();
            }
        }
        else {
            Q_ASSERT(!m_Addresses.isEmpty());
            emit resolvedHost(this, m_Addresses);
        }
    }

    void handleResolvedAddress(const QHostAddress& address)
    {
        m_Addresses.push_back(address);
    }

signals:
    void resolvedHost(MdnsPendingComputer*,QVector<QHostAddress>&);

private:
    void cleanup()
    {
        // Delete our resolver, so we're guaranteed that nothing is referencing m_Server.
        delete m_Resolver;
        m_Resolver = nullptr;

        // Now delete our strong reference that we held on behalf of m_Resolver.
        // The server may be destroyed after we make this call.
        m_Server.reset();
    }

    void resolve()
    {
        // Clean up any existing resolver object and server references
        cleanup();

        // Re-acquire a strong reference if the server still exists.
        m_Server = m_ServerWeak.toStrongRef();
        if (!m_Server) {
            return;
        }

        m_Resolver = new QMdnsEngine::Resolver(m_Server.data(), m_Hostname);
        connect(m_Resolver, &QMdnsEngine::Resolver::resolved,
                this, &MdnsPendingComputer::handleResolvedAddress);
        QTimer::singleShot(2000, this, &MdnsPendingComputer::handleResolvedTimeout);
    }

    QByteArray m_Hostname;
    uint16_t m_Port;
    QWeakPointer<QMdnsEngine::Server> m_ServerWeak;
    QSharedPointer<QMdnsEngine::Server> m_Server;
    QMdnsEngine::Resolver* m_Resolver;
    QVector<QHostAddress> m_Addresses;
    int m_Retries = 10;
};

class ComputerPollingEntry
{
public:
    ComputerPollingEntry()
        : m_ActiveThread(nullptr)
    {

    }

    virtual ~ComputerPollingEntry()
    {
        interrupt();

        // interrupt() should have taken care of this
        Q_ASSERT(m_ActiveThread == nullptr);

        for (QThread* thread : std::as_const(m_InactiveList)) {
            thread->wait();
            delete thread;
        }
    }

    bool isActive()
    {
        cleanInactiveList();

        return m_ActiveThread != nullptr;
    }

    void setActiveThread(QThread* thread)
    {
        cleanInactiveList();

        Q_ASSERT(!isActive());
        m_ActiveThread = thread;
    }

    void interrupt()
    {
        cleanInactiveList();

        if (m_ActiveThread != nullptr) {
            // Interrupt the active thread
            m_ActiveThread->requestInterruption();

            // Place it on the inactive list awaiting death
            m_InactiveList.append(m_ActiveThread);

            m_ActiveThread = nullptr;
        }
    }

private:
    void cleanInactiveList()
    {
        QMutableListIterator<QThread*> i(m_InactiveList);

        // Reap any threads that have finished
        while (i.hasNext()) {
            i.next();

            QThread* thread = i.value();
            if (thread->isFinished()) {
                delete thread;
                i.remove();
            }
        }
    }

    QThread* m_ActiveThread;
    QList<QThread*> m_InactiveList;
};

class ComputerManager : public QObject
{
    Q_OBJECT

    friend class DeferredHostDeletionTask;
    friend class PendingAddTask;
    friend class PendingAuthenticationTask;
    friend class BrokerWorkstationRefreshTask;
    friend class DelayedFlushThread;

public:
    explicit ComputerManager(StreamingPreferences* prefs);

    virtual ~ComputerManager();

    Q_INVOKABLE void startPolling();

    Q_INVOKABLE void stopPollingAsync();

    Q_INVOKABLE QStringList plankVirtualModeChoices() const;
    Q_INVOKABLE int probeHostPlatform(QString address);

    Q_INVOKABLE void addNewHostManually(QString address, QString nickname = QString(),
                                        int hostLayout = 0, int virtualMode1 = 9,
                                        int virtualMode2 = 1,
                                        int scaling = 1,
                                        int videoProfile = StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444,
                                        int captureSource = 0,
                                        QVariantList profileBitratesKbps = QVariantList());

    bool editManualBookmark(NvComputer* computer, QString address, QString nickname,
                            QString scalingMode,
                            QString hostLayout, QString virtualMode1,
                            QString virtualMode2,
                            int videoProfile, int captureSource,
                            const QVariantList& profileBitratesKbps);

    void addNewHost(NvAddress address, bool mdns, QString name = QString(), NvAddress mdnsIpv6Address = NvAddress());

    void authenticateHost(NvComputer* computer, QString username, QString password);

    bool takePlankReconnectCredentials(NvComputer* computer,
                                                QString& username,
                                                QString& password);

    QVector<NvComputer*> getComputers();

    // computer is deleted inside this call
    void deleteHost(NvComputer* computer);

    void renameHost(NvComputer* computer, QString name);

    void clientSideAttributeUpdated(NvComputer* computer);

    // Replaces m_KnownHosts with the broker's per-subject workstation list.
    // Only meaningful when NvHTTP::brokerConfigured() is true; this is a
    // no-op (besides refreshing an empty list) otherwise.
    Q_INVOKABLE void refreshBrokerWorkstations();

signals:
    void hostPlatformDetected(int requestId, QString address, int platform);
    void computerStateChanged(NvComputer* computer);

    void authenticationCompleted(NvComputer* computer, QString error);

    void computerAddCompleted(QVariant success);

    // Emitted when the broker rejects (or lacks) the facility session while
    // fetching the workstation list. The sign-in screen should keep waiting
    // for a successful browser-based sign-in.
    void brokerSignInRequired();

    // Emitted when a broker workstation list refresh fails for a reason
    // other than a missing/expired session (e.g. the broker is unreachable).
    void brokerRefreshFailed(QString error);

private slots:
    void handleAboutToQuit();

    void handleComputerStateChanged(NvComputer* computer);

    void handleMdnsServiceResolved(MdnsPendingComputer* computer, QVector<QHostAddress>& addresses);

private:
    void saveHosts();

    void saveHost(NvComputer* computer);

    QHostAddress getBestGlobalAddressV6(QVector<QHostAddress>& addresses);

    void startPollingComputer(NvComputer* computer);

    void rememberPlankReconnectCredentials(NvComputer* computer,
                                                     QString username,
                                                     QString password);

    StreamingPreferences* m_Prefs;
    bool m_HostPlatformProbePending = false;
    int m_HostPlatformProbeSequence = 0;
    int m_PollingRef;
    QReadWriteLock m_Lock;
    QMap<QString, NvComputer*> m_KnownHosts;
    QMutex m_ReconnectCredentialLock;
    QMap<NvComputer*, QPair<QString, QString>> m_ReconnectCredentials;
    QMap<QString, ComputerPollingEntry*> m_PollEntries;
    QHash<QString, NvComputer> m_LastSerializedHosts; // Protected by m_DelayedFlushMutex
    QSharedPointer<QMdnsEngine::Server> m_MdnsServer;
    QMdnsEngine::Browser* m_MdnsBrowser;
    QVector<MdnsPendingComputer*> m_PendingResolution;
    DelayedFlushThread* m_DelayedFlushThread;
    QMutex m_DelayedFlushMutex; // Lock ordering: Must never be acquired while holding NvComputer lock
    QWaitCondition m_DelayedFlushCondition;
    bool m_NeedsDelayedFlush;
};
