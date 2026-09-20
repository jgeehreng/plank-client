#include "nvcomputer.h"
#include "nvapp.h"
#include "settings/plankclientpolicy.h"
#include "settings/streamingpreferences.h"
#include "planknetwork.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QNetworkProxy>

#define SER_NAME "hostname"
#define SER_UUID "uuid"
#define SER_LOCALADDR "localaddress"
#define SER_LOCALPORT "localport"
#define SER_REMOTEADDR "remoteaddress"
#define SER_REMOTEPORT "remoteport"
#define SER_MANUALADDR "manualaddress"
#define SER_MANUALPORT "manualport"
#define SER_IPV6ADDR "ipv6address"
#define SER_IPV6PORT "ipv6port"
#define SER_APPLIST "apps"
#define SER_CUSTOMNAME "customname"
#define SER_PLANK_SCALING_MODE "plank-scaling-mode"
#define SER_HOSTLAYOUT "plank-host-layout"
#define SER_VIRTUALMODE1 "plank-virtual-mode-1"
#define SER_VIRTUALMODE2 "plank-virtual-mode-2"
#define SER_VIDEOPROFILE "plank-video-profile"
#define SER_CAPTURESOURCE "plank-capture-source"
#define SER_PLANK_PROFILE_BITRATES "plank-profile-bitrates-kbps"
#define SER_OUTPUTTOPOLOGY "plank-output-topology"
#define SER_MANUALBOOKMARK "plank-manual-bookmark"
#define SER_SERVERUUID "plank-server-uuid"
#define SER_USERNAME "plank-username"

namespace {
QString manualBookmarkUuid(const NvAddress& address)
{
    const QByteArray bookmarkKey = address.toString().toUtf8();
    return QStringLiteral("plank-bookmark-") +
            QString::fromLatin1(QCryptographicHash::hash(
                                    bookmarkKey, QCryptographicHash::Sha256).toHex().left(32));
}
}

NvComputer::NvComputer(NvAddress address, QString nickname, int videoProfile,
                       int captureSource,
                       const QVector<int>& profileBitratesKbps)
{
    this->uuid = manualBookmarkUuid(address);
    this->name = nickname;
    this->hasCustomName = true;
    this->manualAddress = address;
    this->manualBookmark = true;
    this->plankVideoProfile = videoProfile;
    this->plankCaptureSource = captureSource;
    this->plankProfileBitratesKbps = profileBitratesKbps;
    this->plankScalingMode = NvOutputTopology::ScaledSpanMode;
    this->plankHostLayout = NvOutputTopology::MatchClientHostLayout;
    this->plankVirtualMode1 = QStringLiteral("3840x2160");
    this->plankVirtualMode2 = QStringLiteral("1280x2160");
    this->state = CS_UNKNOWN;
    this->authorizationState = AS_UNKNOWN;
    this->currentGameId = 0;
    this->plankOccupied = false;
    this->serverCodecModeSupport = 0;
    this->externalPort = address.port();
}

bool NvComputer::updateManualBookmark(NvAddress address, QString nickname,
                                      QString scalingMode,
                                      QString hostLayout, QString virtualMode1,
                                      QString virtualMode2,
                                      int videoProfile, int captureSource,
                                      const QVector<int>& profileBitratesKbps)
{
    QWriteLocker writeLocker(&lock);
    Q_ASSERT(manualBookmark);

    const bool addressChanged = manualAddress != address;
    if (addressChanged) {
        uuid = manualBookmarkUuid(address);
        manualAddress = address;
        localAddress = NvAddress();
        remoteAddress = NvAddress();
        ipv6Address = NvAddress();
        activeAddress = NvAddress();
        externalPort = address.port();
        serverUuid.clear();
        appList.clear();
        outputTopology = NvOutputTopology();
        sessionToken.clear();
        authorizationState = AS_UNKNOWN;
        state = CS_UNKNOWN;
        currentGameId = 0;
        plankOccupied = false;
        plankAuthentication = false;
        plankHostMetadataVersion = 0;
        plankHostVersion.clear();
        plankTopologyVersion = 0;
        plankFeatureFlags = 0;
        displayModes.clear();
        serverCodecModeSupport = 0;
        appVersion.clear();
    }

    name = nickname;
    hasCustomName = true;
    plankScalingMode = scalingMode;
    plankHostLayout = hostLayout;
    plankVirtualMode1 = virtualMode1;
    plankVirtualMode2 = virtualMode2;
    plankVideoProfile = videoProfile;
    plankCaptureSource = captureSource;
    plankProfileBitratesKbps = profileBitratesKbps;
    return addressChanged;
}

NvComputer::NvComputer(QSettings& settings)
{
    const quint16 defaultPort = PlankClientPolicy().networkPort();
    this->name = settings.value(SER_NAME).toString();
    this->uuid = settings.value(SER_UUID).toString();
    this->hasCustomName = settings.value(SER_CUSTOMNAME).toBool();
    this->localAddress = NvAddress(settings.value(SER_LOCALADDR).toString(),
                                   settings.value(SER_LOCALPORT, QVariant(defaultPort)).toUInt());
    this->remoteAddress = NvAddress(settings.value(SER_REMOTEADDR).toString(),
                                    settings.value(SER_REMOTEPORT, QVariant(defaultPort)).toUInt());
    this->ipv6Address = NvAddress(settings.value(SER_IPV6ADDR).toString(),
                                  settings.value(SER_IPV6PORT, QVariant(defaultPort)).toUInt());
    this->manualAddress = NvAddress(settings.value(SER_MANUALADDR).toString(),
                                    settings.value(SER_MANUALPORT, QVariant(defaultPort)).toUInt());
    this->plankScalingMode = settings.value(
                SER_PLANK_SCALING_MODE, NvOutputTopology::ScaledSpanMode).toString();
    if (this->plankScalingMode != NvOutputTopology::NativeScalingMode &&
            this->plankScalingMode != NvOutputTopology::ScaledSpanMode) {
        this->plankScalingMode = NvOutputTopology::ScaledSpanMode;
    }
    this->plankHostLayout =
            settings.value(SER_HOSTLAYOUT,
                           NvOutputTopology::MatchClientHostLayout).toString();
    if (this->plankHostLayout != NvOutputTopology::MatchClientHostLayout &&
            this->plankHostLayout != NvOutputTopology::PhysicalHostLayout &&
            this->plankHostLayout != NvOutputTopology::SingleHostLayout &&
            this->plankHostLayout != NvOutputTopology::DualHorizontalHostLayout &&
            this->plankHostLayout != QStringLiteral("fixed")) {
        this->plankHostLayout = NvOutputTopology::MatchClientHostLayout;
    }
    this->plankVirtualMode1 =
            settings.value(SER_VIRTUALMODE1, QStringLiteral("3840x2160")).toString();
    this->plankVirtualMode2 =
            settings.value(SER_VIRTUALMODE2, QStringLiteral("1280x2160")).toString();
    if (!NvOutputTopology::qualifiedVirtualModes().contains(this->plankVirtualMode1)) {
        this->plankVirtualMode1 = QStringLiteral("3840x2160");
    }
    if (!NvOutputTopology::qualifiedVirtualModes().contains(this->plankVirtualMode2)) {
        this->plankVirtualMode2 = QStringLiteral("1280x2160");
    }
    this->plankVideoProfile = qBound(
            static_cast<int>(StreamingPreferences::PLANK_PROFILE_H264_10BIT_444),
            settings.value(SER_VIDEOPROFILE,
                           static_cast<int>(StreamingPreferences::PLANK_PROFILE_H264_10BIT_444)).toInt(),
            static_cast<int>(StreamingPreferences::PLANK_PROFILE_COUNT) - 1);
    this->plankCaptureSource = qBound(
            static_cast<int>(StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT),
            settings.value(SER_CAPTURESOURCE,
                           static_cast<int>(StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT)).toInt(),
            static_cast<int>(StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT));
    if (!StreamingPreferences::isPlankProfileValidForCaptureSource(
                this->plankVideoProfile,
                this->plankCaptureSource)) {
        if (this->plankCaptureSource != StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT &&
                !StreamingPreferences::isPlankAppleProfile(this->plankVideoProfile)) {
            this->plankVideoProfile = StreamingPreferences::PLANK_PROFILE_H264_10BIT_444;
        }
    }
    if (this->plankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT &&
            this->plankHostLayout != NvOutputTopology::MatchClientHostLayout) {
        this->plankHostLayout = QStringLiteral("fixed");
    }
    else if (this->plankHostLayout == QStringLiteral("fixed")) {
        this->plankHostLayout = NvOutputTopology::MatchClientHostLayout;
    }
    if (!StreamingPreferences::plankProfileBitratesFromVariantList(
                settings.value(SER_PLANK_PROFILE_BITRATES).toList(),
                this->plankProfileBitratesKbps)) {
        this->plankProfileBitratesKbps =
                StreamingPreferences::plankDefaultProfileBitrates();
    }
    this->manualBookmark = settings.value(SER_MANUALBOOKMARK, false).toBool();
    this->serverUuid = settings.value(SER_SERVERUUID).toString();
    this->plankUsername = settings.value(SER_USERNAME).toString().trimmed();
    this->plankUsername.remove(QChar('\0'));
    if (this->plankUsername.contains(QLatin1Char('\n')) ||
            this->plankUsername.contains(QLatin1Char('\r')) ||
            this->plankUsername.size() > 256) {
        this->plankUsername.clear();
    }
    const QJsonDocument serializedTopology = QJsonDocument::fromJson(
            settings.value(SER_OUTPUTTOPOLOGY).toByteArray());
    if (serializedTopology.isObject()) {
        NvOutputTopology::fromJson(serializedTopology.object(), this->outputTopology);
    }

    int appCount = settings.beginReadArray(SER_APPLIST);
    this->appList.reserve(appCount);
    for (int i = 0; i < appCount; i++) {
        settings.setArrayIndex(i);

        NvApp app(settings);
        this->appList.append(app);
    }
    settings.endArray();
    sortAppList();

    this->currentGameId = 0;
    this->plankOccupied = false;
    this->authorizationState = AS_UNKNOWN;
    this->state = CS_UNKNOWN;
    this->appVersion = nullptr;
    this->serverCodecModeSupport = 0;
    this->externalPort = this->remoteAddress.port();
    this->plankAuthentication = false;
    this->plankHostMetadataVersion = 0;
    this->plankHostVersion.clear();
    this->plankTopologyVersion = 0;
    this->plankFeatureFlags = 0;
    this->sessionToken.clear();
}

void NvComputer::serialize(QSettings& settings, bool serializeApps) const
{
    QReadLocker lock(&this->lock);

    settings.setValue(SER_NAME, name);
    settings.setValue(SER_CUSTOMNAME, hasCustomName);
    settings.setValue(SER_UUID, uuid);
    settings.setValue(SER_LOCALADDR, localAddress.address());
    settings.setValue(SER_LOCALPORT, localAddress.port());
    settings.setValue(SER_REMOTEADDR, remoteAddress.address());
    settings.setValue(SER_REMOTEPORT, remoteAddress.port());
    settings.setValue(SER_IPV6ADDR, ipv6Address.address());
    settings.setValue(SER_IPV6PORT, ipv6Address.port());
    settings.setValue(SER_MANUALADDR, manualAddress.address());
    settings.setValue(SER_MANUALPORT, manualAddress.port());
    settings.remove("srvcert");
    settings.setValue(SER_PLANK_SCALING_MODE, plankScalingMode);
    settings.setValue(SER_HOSTLAYOUT, plankHostLayout);
    settings.setValue(SER_VIRTUALMODE1, plankVirtualMode1);
    settings.setValue(SER_VIRTUALMODE2, plankVirtualMode2);
    settings.setValue(SER_VIDEOPROFILE, plankVideoProfile);
    settings.setValue(SER_CAPTURESOURCE, plankCaptureSource);
    settings.setValue(
                SER_PLANK_PROFILE_BITRATES,
                StreamingPreferences::plankProfileBitratesToVariantList(
                    plankProfileBitratesKbps));
    settings.setValue(SER_MANUALBOOKMARK, manualBookmark);
    settings.setValue(SER_SERVERUUID, serverUuid);
    settings.remove("plank-password");
    if (plankUsername.isEmpty()) {
        settings.remove(SER_USERNAME);
    } else {
        settings.setValue(SER_USERNAME, plankUsername);
    }
    if (!outputTopology.outputs.isEmpty()) {
        settings.setValue(SER_OUTPUTTOPOLOGY,
                          QJsonDocument(outputTopology.toJson()).toJson(QJsonDocument::Compact));
    } else {
        settings.remove(SER_OUTPUTTOPOLOGY);
    }

    // Avoid deleting an existing applist if we couldn't get one
    if (!appList.isEmpty() && serializeApps) {
        settings.remove(SER_APPLIST);
        settings.beginWriteArray(SER_APPLIST);
        for (int i = 0; i < appList.count(); i++) {
            settings.setArrayIndex(i);
            appList.at(i).serialize(settings);
        }
        settings.endArray();
    }
}

bool NvComputer::isEqualSerialized(const NvComputer &that) const
{
    return this->name == that.name &&
           this->hasCustomName == that.hasCustomName &&
           this->uuid == that.uuid &&
           this->localAddress == that.localAddress &&
           this->remoteAddress == that.remoteAddress &&
           this->ipv6Address == that.ipv6Address &&
           this->manualAddress == that.manualAddress &&
           this->plankScalingMode == that.plankScalingMode &&
           this->plankHostLayout == that.plankHostLayout &&
           this->plankVirtualMode1 == that.plankVirtualMode1 &&
           this->plankVirtualMode2 == that.plankVirtualMode2 &&
           this->plankVideoProfile == that.plankVideoProfile &&
           this->plankCaptureSource == that.plankCaptureSource &&
           this->plankProfileBitratesKbps ==
               that.plankProfileBitratesKbps &&
           this->manualBookmark == that.manualBookmark &&
           this->serverUuid == that.serverUuid &&
           this->plankUsername == that.plankUsername &&
           this->outputTopology.toJson() == that.outputTopology.toJson() &&
           this->appList == that.appList;
}

void NvComputer::sortAppList()
{
    std::stable_sort(appList.begin(), appList.end(), [](const NvApp& app1, const NvApp& app2) {
       return app1.name.toLower() < app2.name.toLower();
    });
}

NvComputer::NvComputer(NvHTTP& http, QString serverInfo)
{
    this->manualBookmark = false;
    this->plankScalingMode = NvOutputTopology::ScaledSpanMode;

    this->hasCustomName = false;
    this->name = NvHTTP::getXmlString(serverInfo, "hostname");
    if (this->name.isEmpty()) {
        this->name = "UNKNOWN";
    }

    this->uuid = NvHTTP::getXmlString(serverInfo, "uniqueid");
    QString codecSupport = NvHTTP::getXmlString(serverInfo, "ServerCodecModeSupport");
    if (!codecSupport.isEmpty()) {
        this->serverCodecModeSupport = codecSupport.toInt();
    }
    else {
        // Assume H.264 is always supported
        this->serverCodecModeSupport = SCM_H264;
    }

    this->displayModes = NvHTTP::getDisplayModeList(serverInfo);
    std::stable_sort(this->displayModes.begin(), this->displayModes.end(),
                     [](const NvDisplayMode& mode1, const NvDisplayMode& mode2) {
        return (uint64_t)mode1.width * mode1.height * mode1.refreshRate <
                (uint64_t)mode2.width * mode2.height * mode2.refreshRate;
    });

    // We can get an IPv4 loopback address if we're using the GS IPv6 Forwarder
    this->localAddress = NvAddress(NvHTTP::getXmlString(serverInfo, "LocalIP"), http.controlPort());
    if (this->localAddress.address().startsWith("127.")) {
        this->localAddress = NvAddress();
    }

    const QString advertisedControlPort = NvHTTP::getXmlString(serverInfo, "HttpsPort");
    if (!advertisedControlPort.isEmpty() &&
            advertisedControlPort.toUShort() != http.controlPort()) {
        throw GfeHttpResponseException(
                    400, "Host advertised a different PLANK control port");
    }

    // This is an extension which is not present in GFE. It is present for Sunshine to be able
    // to support dynamic HTTP WAN ports without requiring the user to manually enter the port.
    QString remotePortStr = NvHTTP::getXmlString(serverInfo, "ExternalPort");
    if (remotePortStr.isEmpty() || (this->externalPort = remotePortStr.toUShort()) == 0) {
        this->externalPort = http.controlPort();
    }

    QString remoteAddress = NvHTTP::getXmlString(serverInfo, "ExternalIP");
    if (!remoteAddress.isEmpty()) {
        this->remoteAddress = NvAddress(remoteAddress, this->externalPort);
    }
    else {
        this->remoteAddress = NvAddress();
    }

    this->plankAuthentication =
            NvHTTP::getXmlString(serverInfo, "PlankAuth") == "1";
    this->plankHostMetadataVersion =
            NvHTTP::getXmlString(serverInfo, "PlankHostMetadataVersion").toInt();
    this->plankHostVersion =
            NvHTTP::getXmlString(serverInfo, "PlankHostVersion");
    this->plankTopologyVersion =
            NvHTTP::getXmlString(serverInfo, "PlankTopologyVersion").toInt();
    this->plankFeatureFlags =
            NvHTTP::getXmlString(serverInfo, "PlankFeatureFlags").toInt();
    this->authorizationState = NvHTTP::getXmlString(serverInfo, "PairStatus") == "1" ?
                AS_AUTHORIZED : AS_UNAUTHORIZED;
    this->currentGameId = NvHTTP::getCurrentGame(serverInfo);
    this->plankOccupied = NvHTTP::getPlankOccupied(serverInfo);
    this->appVersion = NvHTTP::getXmlString(serverInfo, "appversion");
    this->activeAddress = http.address();
    this->state = NvComputer::CS_ONLINE;
}

NvComputer::ReachabilityType NvComputer::getActiveAddressReachability(
        quint32* interfaceMtu, bool* isIpv6) const
{
    if (interfaceMtu != nullptr) {
        *interfaceMtu = 0;
    }
    if (isIpv6 != nullptr) {
        *isIpv6 = false;
    }

    NvAddress copyOfActiveAddress;

    {
        QReadLocker readLocker(&lock);

        if (activeAddress.isNull()) {
            return ReachabilityType::RI_UNKNOWN;
        }

        // Grab a copy of the active address to avoid having to hold
        // the computer lock while doing socket operations
        copyOfActiveAddress = activeAddress;
    }

    QTcpSocket s;
    s.setProxy(QNetworkProxy::NoProxy);
    s.connectToHost(copyOfActiveAddress.address(), copyOfActiveAddress.port());
    if (s.waitForConnected(3000)) {
        Q_ASSERT(!s.localAddress().isNull());
        Q_ASSERT(!s.peerAddress().isNull());

        const auto allInterfaces = QNetworkInterface::allInterfaces();
        for (const QNetworkInterface& nic : allInterfaces) {
            // Ensure the interface is up
            if ((nic.flags() & QNetworkInterface::IsUp) == 0) {
                continue;
            }

            const auto allInterfaceAddresses = nic.addressEntries();
            for (const QNetworkAddressEntry& addr : allInterfaceAddresses) {
                if (addr.ip() == s.localAddress()) {
                    if (interfaceMtu != nullptr) {
                        *interfaceMtu = qMax(0, nic.maximumTransmissionUnit());
                    }
                    if (isIpv6 != nullptr) {
                        *isIpv6 = s.localAddress().protocol() ==
                                QAbstractSocket::IPv6Protocol;
                    }
                    qInfo() << "Found matching interface:" << nic.humanReadableName() << nic.hardwareAddress() << nic.flags();

                    if (PlankNetwork::isZeroTierInterface(nic.name(),
                                                                   nic.humanReadableName())) {
                        // Identify ZeroTier before broader virtual/VPN
                        // heuristics so its qualified encapsulation budget is
                        // available to the native QUIC transport.
                        return ReachabilityType::RI_ZEROTIER;
                    }

#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
                    qInfo() << "Interface Type:" << nic.type();
                    qInfo() << "Interface MTU:" << nic.maximumTransmissionUnit();

                    if (nic.type() == QNetworkInterface::Virtual ||
                            nic.type() == QNetworkInterface::Ppp) {
                        // Treat PPP and virtual interfaces as likely VPNs
                        return ReachabilityType::RI_VPN;
                    }

                    if (nic.maximumTransmissionUnit() != 0 && nic.maximumTransmissionUnit() < 1500) {
                        // Treat MTUs under 1500 as likely VPNs
                        return ReachabilityType::RI_VPN;
                    }
#endif

                    if (nic.flags() & QNetworkInterface::IsPointToPoint) {
                        // Treat point-to-point links as likely VPNs.
                        // This check detects OpenVPN on Unix-like OSes.
                        return ReachabilityType::RI_VPN;
                    }

#ifdef Q_OS_WINDOWS
                    if (nic.name().startsWith("iftype53_") || nic.name().startsWith("iftype131_")) {
                        // Match by NDIS interface type. These values are Microsoft's recommended values for VPN connections:
                        // https://learn.microsoft.com/en-US/troubleshoot/windows-client/networking/windows-connection-manager-disconnects-wlan#more-information
                        //
                        // The following VPNs use IF_TYPE_PROP_VIRTUAL under Windows:
                        //  - WireguardNT VPNs
                        //  - All WinTun-based VPNs (such as Slack Nebula)
                        //  - OpenVPN with tap-windows6
                        return ReachabilityType::RI_VPN;
                    }
#endif

                    if (nic.hardwareAddress().startsWith("00:FF", Qt::CaseInsensitive)) {
                        // OpenVPN TAP interfaces have a MAC address starting with 00:FF on Windows
                        return ReachabilityType::RI_VPN;
                    }

                    if (nic.humanReadableName().contains("VPN")) {
                        // This one is just a final VPN heuristic if all else fails
                        return ReachabilityType::RI_VPN;
                    }

                    // Didn't meet any of our VPN heuristics. Let's see if the peer address is on-link.
                    Q_ASSERT(addr.prefixLength() >= 0);
                    if (addr.prefixLength() >= 0 && s.localAddress().isInSubnet(s.peerAddress(), addr.prefixLength())) {
                        return ReachabilityType::RI_LAN;
                    }

                    // Default to unknown if nothing else matched
                    return ReachabilityType::RI_UNKNOWN;
                }
            }
        }

        qWarning() << "No match found for address:" << s.localAddress();
        return ReachabilityType::RI_UNKNOWN;
    }
    else {
        // If we fail to connect, just pretend that it's not a VPN
        qWarning() << "Unable to check for reachability within 3 seconds";
        return ReachabilityType::RI_UNKNOWN;
    }
}

bool NvComputer::updateAppList(QVector<NvApp> newAppList) {
    if (appList == newAppList) {
        return false;
    }

    // Propagate client-side attributes to the new app list
    for (const NvApp& existingApp : std::as_const(appList)) {
        for (NvApp& newApp : newAppList) {
            if (existingApp.id == newApp.id) {
                newApp.hidden = existingApp.hidden;
                newApp.directLaunch = existingApp.directLaunch;
            }
        }
    }

    appList = newAppList;
    sortAppList();
    return true;
}

QVector<NvAddress> NvComputer::uniqueAddresses() const
{
    QReadLocker readLocker(&lock);
    QVector<NvAddress> uniqueAddressList;

    // Start with addresses correctly ordered
    uniqueAddressList.append(activeAddress);
    uniqueAddressList.append(localAddress);
    uniqueAddressList.append(remoteAddress);
    uniqueAddressList.append(ipv6Address);
    uniqueAddressList.append(manualAddress);

    // Prune duplicates (always giving precedence to the first)
    for (int i = 0; i < uniqueAddressList.count(); i++) {
        if (uniqueAddressList[i].isNull()) {
            uniqueAddressList.remove(i);
            i--;
            continue;
        }
        for (int j = i + 1; j < uniqueAddressList.count(); j++) {
            if (uniqueAddressList[i] == uniqueAddressList[j]) {
                // Always remove the later occurrence
                uniqueAddressList.remove(j);
                j--;
            }
        }
    }

    // We must have at least 1 address
    Q_ASSERT(!uniqueAddressList.isEmpty());

    return uniqueAddressList;
}

bool NvComputer::update(const NvComputer& that, NvAddress expectedAddress)
{
    bool changed = false;

    // Lock us for write and them for read
    QWriteLocker thisLock(&this->lock);
    QReadLocker thatLock(&that.lock);

    if (!expectedAddress.isNull() && expectedAddress != activeAddress &&
            expectedAddress != localAddress && expectedAddress != remoteAddress &&
            expectedAddress != ipv6Address && expectedAddress != manualAddress) {
        return false;
    }

    // A manual bookmark has a stable local UUID before its server identity is
    // known. Bind it to the first server that successfully answers, then reject
    // any different identity at that saved address.
    if (manualBookmark) {
        Q_ASSERT(serverUuid.isEmpty() || serverUuid == that.uuid);
        if (serverUuid.isEmpty()) {
            serverUuid = that.uuid;
            changed = true;
        }
    }
    else {
        Q_ASSERT(this->uuid == that.uuid);
    }

#define ASSIGN_IF_CHANGED(field)       \
    if (this->field != that.field) {   \
        this->field = that.field;      \
        changed = true;                \
    }

#define ASSIGN_IF_CHANGED_AND_NONEMPTY(field) \
    if (!that.field.isEmpty() &&              \
        this->field != that.field) {          \
        this->field = that.field;             \
        changed = true;                       \
    }

#define ASSIGN_IF_CHANGED_AND_NONNULL(field)  \
    if (!that.field.isNull() &&               \
        this->field != that.field) {          \
        this->field = that.field;             \
        changed = true;                       \
    }

    if (!hasCustomName) {
        // Only overwrite the name if it's not custom
        ASSIGN_IF_CHANGED(name);
    }
    ASSIGN_IF_CHANGED_AND_NONNULL(localAddress);
    ASSIGN_IF_CHANGED_AND_NONNULL(remoteAddress);
    ASSIGN_IF_CHANGED_AND_NONNULL(ipv6Address);
    ASSIGN_IF_CHANGED_AND_NONNULL(manualAddress);
    ASSIGN_IF_CHANGED(externalPort);
    ASSIGN_IF_CHANGED(plankAuthentication);
    ASSIGN_IF_CHANGED(plankHostMetadataVersion);
    ASSIGN_IF_CHANGED(plankHostVersion);
    ASSIGN_IF_CHANGED(plankTopologyVersion);
    ASSIGN_IF_CHANGED(plankFeatureFlags);
    if (plankAuthentication && sessionToken.isEmpty()) {
        if (authorizationState != AS_UNAUTHORIZED) {
            authorizationState = AS_UNAUTHORIZED;
            changed = true;
        }
    }
    else {
        ASSIGN_IF_CHANGED(authorizationState);
    }
    ASSIGN_IF_CHANGED(serverCodecModeSupport);
    ASSIGN_IF_CHANGED(currentGameId);
    ASSIGN_IF_CHANGED(plankOccupied);
    ASSIGN_IF_CHANGED(activeAddress);
    ASSIGN_IF_CHANGED(state);
    ASSIGN_IF_CHANGED(appVersion);
    ASSIGN_IF_CHANGED_AND_NONEMPTY(displayModes);

    if (!that.appList.isEmpty()) {
        // updateAppList() handles merging client-side attributes
        updateAppList(that.appList);
    }

    return changed;
}

bool NvComputer::acceptsServerUuid(const QString& candidateUuid) const
{
    QReadLocker readLocker(&lock);
    if (manualBookmark) {
        return serverUuid.isEmpty() || serverUuid == candidateUuid;
    }
    return uuid == candidateUuid;
}
