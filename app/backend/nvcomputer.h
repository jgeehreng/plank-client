#pragma once

#include "nvhttp.h"
#include "nvaddress.h"
#include "outputtopology.h"
#include "settings/streamingpreferences.h"
#include "settings/plankclientpolicy.h"

#include <QThread>
#include <QReadWriteLock>
#include <QSettings>
#include <QRunnable>

class CopySafeReadWriteLock : public QReadWriteLock
{
public:
    CopySafeReadWriteLock() = default;

    // Don't actually copy the QReadWriteLock
    CopySafeReadWriteLock(const CopySafeReadWriteLock&) : QReadWriteLock() {}
    CopySafeReadWriteLock& operator=(const CopySafeReadWriteLock &) { return *this; }
};

class NvComputer
{
    friend class PcMonitorThread;
    friend class ComputerManager;
    friend class PendingAuthenticationTask;
    friend class Session;

private:
    void sortAppList();

    bool updateAppList(QVector<NvApp> newAppList);

public:
    NvComputer() = default;

    // Caller is responsible for synchronizing read access to the other host
    NvComputer(const NvComputer&) = default;

    // Caller is responsible for synchronizing read access to the other host
    NvComputer& operator=(const NvComputer &) = default;

    explicit NvComputer(NvHTTP& http, QString serverInfo);

    explicit NvComputer(QSettings& settings,
                        const PlankClientPolicy& policy = PlankClientPolicy());

    NvComputer(NvAddress manualAddress, QString nickname, int videoProfile,
               int captureSource,
               const QVector<int>& profileBitratesKbps);

    bool
    updateManualBookmark(NvAddress manualAddress, QString nickname,
                         QString scalingMode,
                         QString hostLayout, QString virtualMode1,
                         QString virtualMode2,
                         int videoProfile, int captureSource,
                         const QVector<int>& profileBitratesKbps);

    bool
    update(const NvComputer& that, NvAddress expectedAddress = NvAddress());

    bool
    acceptsServerUuid(const QString& candidateUuid) const;

    enum ReachabilityType
    {
        RI_UNKNOWN,
        RI_LAN,
        RI_VPN,
        RI_ZEROTIER,
    };

    static bool isVpnReachability(ReachabilityType reachability)
    {
        return reachability == RI_VPN || reachability == RI_ZEROTIER;
    }

    ReachabilityType
    getActiveAddressReachability(quint32* interfaceMtu = nullptr,
                                 bool* isIpv6 = nullptr) const;

    QVector<NvAddress>
    uniqueAddresses() const;

    void
    serialize(QSettings& settings, bool serializeApps,
              const PlankClientPolicy& policy = PlankClientPolicy()) const;

    // Local opt-in prefill only. Never populated by Host discovery metadata.
    QString rememberedUsername(const PlankClientPolicy& policy = PlankClientPolicy()) const;
    void rememberAuthenticatedUsername(const QString& username, const NvAddress& expectedAddress,
                                       const PlankClientPolicy& policy = PlankClientPolicy());

    // Purge every entry in a bookmark array, including orphaned backup entries.
    static void forgetSavedUsernames(QSettings& settings, const QString& array);

    // Caller is responsible for synchronizing read access to both hosts
    bool
    isEqualSerialized(const NvComputer& that) const;

    // Copy host capability fields from /serverinfo. Leaves the dialed address
    // alone so a broker relay port is not replaced by the host's own port.
    void
    applyServerMetadata(const QString& serverInfo);

    enum AuthorizationState
    {
        AS_UNKNOWN,
        AS_AUTHORIZED,
        AS_UNAUTHORIZED
    };

    enum ComputerState
    {
        CS_UNKNOWN,
        CS_ONLINE,
        CS_OFFLINE
    };

    // Ephemeral traits
    ComputerState state;
    AuthorizationState authorizationState;
    NvAddress activeAddress;
    int currentGameId;
    bool plankOccupied = false;
    bool broadcastSource = false;
    QString plankSessionUser;
    QString appVersion;
    QVector<NvDisplayMode> displayModes;
    int serverCodecModeSupport;
    bool plankAuthentication = false;
    int plankHostMetadataVersion = 0;
    QString plankHostVersion;
    QString sessionToken;
    // In-memory only; bind the bearer token to the authenticated Host identity.
    QByteArray sessionIdentityKey;
    int plankTopologyVersion = 0;
    int plankFeatureFlags = 0;
    NvOutputTopology outputTopology;

    // Persisted traits
    NvAddress localAddress;
    NvAddress remoteAddress;
    NvAddress ipv6Address;
    NvAddress manualAddress;
    QString name;
    bool hasCustomName;
    QString uuid;
    QVector<NvApp> appList;
    QString plankScalingMode;
    QString plankHostLayout;
    QString plankVirtualMode1;
    QString plankVirtualMode2;
    int plankVideoProfile = 0;
    int plankCaptureSource = 0;
    QVector<int> plankProfileBitratesKbps =
            StreamingPreferences::plankDefaultProfileBitrates();
    bool manualBookmark = false;
    // Sourced from the broker's per-subject workstation list rather than a
    // local bookmark or mDNS discovery. Never persisted to QSettings.
    bool brokerManaged = false;
    QString serverUuid;
    // Remember to update isEqualSerialized() when adding fields here!

    // Synchronization
    mutable CopySafeReadWriteLock lock;

private:
    static void forgetSavedUsername(QSettings& settings);
    uint16_t externalPort;
    // Optional persisted local state; intentionally absent from update(serverinfo).
    QString m_RememberedUsername;
};
