#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class PlankWacomTransport
{
    ExactRawHid,
    NormalizedPen,
};

struct PlankWacomTransportDecision
{
    PlankWacomTransport transport;
    std::uint32_t vendor;
    std::uint32_t product;
};

constexpr PlankWacomTransport plankWacomTransportForUsbDevice(
        std::uint32_t vendor, std::uint32_t product)
{
    constexpr std::uint32_t WacomVendorId = 0x056a;

    if (vendor != WacomVendorId) {
        return PlankWacomTransport::ExactRawHid;
    }

    // The first-generation Intuos Pro S/M/L devices require the real USB
    // interface type during hid-wacom probing. UHID cannot reproduce that
    // metadata, so use the existing normalized core-pen transport for the
    // complete PTH-x51 generation. Newer Wacoms remain descriptor-driven.
    switch (product) {
    case 0x0314: // PTH-451
    case 0x0315: // PTH-651
    case 0x0317: // PTH-851
        return PlankWacomTransport::NormalizedPen;
    default:
        return PlankWacomTransport::ExactRawHid;
    }
}

PlankWacomTransportDecision plankWacomTransportForConnectedDevice();

// A Wacom USB device can rebind one hidraw interface after the client has
// already published the others. The host group is incomplete until every
// interface that is present now was part of the attachment.
inline bool plankRawWacomGroupIncomplete(
        const std::vector<std::string>& attachedNodes,
        const std::vector<std::string>& presentNodes)
{
    if (attachedNodes.empty()) {
        return false;
    }
    return std::any_of(
            presentNodes.cbegin(), presentNodes.cend(),
            [&attachedNodes](const std::string& node) {
                return std::find(attachedNodes.cbegin(), attachedNodes.cend(),
                                 node) == attachedNodes.cend();
            });
}

class LinuxRawWacomInput
{
public:
    explicit LinuxRawWacomInput(std::function<void()> tabletActivity);
    ~LinuxRawWacomInput();

    LinuxRawWacomInput(const LinuxRawWacomInput&) = delete;
    LinuxRawWacomInput& operator=(const LinuxRawWacomInput&) = delete;

    void setActive(bool active);
    void beginReconnect();
    void finishReconnect();
    void handleControl(const unsigned char* data, unsigned int length);

private:
    struct HidInterface {
        int fd;
        std::vector<unsigned char> descriptor;
    };

    void run();
    bool discover();
    bool sendAttach();
    bool sendFrame(std::uint16_t type, std::uint16_t interfaceId,
                   std::uint32_t transactionId,
                   const unsigned char* payload, std::size_t payloadLength);
    void handlePhysicalReports();
    void handleGetReport(std::uint16_t interfaceId, std::uint32_t transactionId,
                         const unsigned char* payload, std::size_t payloadLength);
    void handleSetReport(std::uint16_t type, std::uint16_t interfaceId,
                         std::uint32_t transactionId,
                         const unsigned char* payload, std::size_t payloadLength);
    void setGrabbed(bool grabbed);
    void suspendForFocusLoss();
    void release(bool notifyHost);
    bool rawGroupIncomplete() const;

    std::atomic<bool> m_Active;
    std::atomic<bool> m_Stopping;
    std::atomic<bool> m_Reconnecting;
    std::atomic<bool> m_AttachFailed;
    std::thread m_Thread;
    std::recursive_mutex m_Mutex;
    std::vector<HidInterface> m_Interfaces;
    std::vector<int> m_EventFds;
    std::vector<std::string> m_HidrawNodes;
    std::string m_UsbParent;
    std::uint16_t m_Generation;
    std::uint32_t m_InputSequence;
    bool m_AttachPending;
    bool m_Attached;
    bool m_Settling;
    std::chrono::steady_clock::time_point m_SettleDeadline;
    std::chrono::steady_clock::time_point m_LastSiblingCheck;
    std::function<void()> m_TabletActivity;
    std::chrono::steady_clock::time_point m_AttachDeadline;
};
