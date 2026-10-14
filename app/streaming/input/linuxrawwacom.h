#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "linuxwacomreportworker.h"

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
    void processControl(const unsigned char* data, unsigned int length);
    void synchronizeSuspend();
    void waitForWork(std::chrono::milliseconds delay);
    void queueReport(std::uint16_t type, std::uint16_t interfaceId,
                     std::uint32_t transactionId,
                     const unsigned char* payload, std::size_t payloadLength);
    bool setGrabbed(bool grabbed);
    void suspendForFocusLoss();
    void release(bool notifyHost);

    std::atomic<bool> m_Active;
    std::atomic<bool> m_Stopping;
    std::atomic<bool> m_Reconnecting;
    std::atomic<bool> m_AttachFailed;
    std::thread m_Thread;
    // Only run() owns HID handles/attachment state. Callbacks enqueue bounded
    // control records; lifecycle callers wait for an acknowledged suspend.
    std::mutex m_Mutex;
    std::condition_variable m_Changed;
    std::deque<std::vector<unsigned char>> m_Controls;
    std::uint64_t m_SuspendRequested = 0;
    std::uint64_t m_SuspendCompleted = 0;
    bool m_Finished = false;
    bool m_ControlOverflow = false;
    std::vector<HidInterface> m_Interfaces;
    std::vector<int> m_EventFds;
    std::uint16_t m_Generation;
    std::uint32_t m_InputSequence;
    bool m_AttachPending;
    bool m_Attached;
    std::function<void()> m_TabletActivity;
    std::chrono::steady_clock::time_point m_AttachDeadline;
    LinuxWacomReportWorker m_ReportWorker;
};
