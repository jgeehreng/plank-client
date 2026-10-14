#include "linuxrawwacom.h"

#include <Limelight.h>
#include <plank.h>
#include <SDL3/SDL.h>
#include <QtEndian>
#include <libudev.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <linux/uhid.h>
#include <poll.h>
#include <set>
#include <utility>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

const unsigned int kWacomVendorId = 0x056a;
std::atomic<std::uint32_t> s_NextGeneration(0);

std::uint16_t nextGeneration()
{
    std::uint16_t generation;
    do {
        generation = static_cast<std::uint16_t>(
            s_NextGeneration.fetch_add(1, std::memory_order_relaxed) + 1);
    } while (generation == 0);
    return generation;
}

std::string usbParentPath(udev_device* device)
{
    udev_device* parent = udev_device_get_parent_with_subsystem_devtype(
        device, "usb", "usb_device");
    const char* path = parent != nullptr ? udev_device_get_syspath(parent) : nullptr;
    return path != nullptr ? path : "";
}

bool isWacomUsbDevice(udev_device* device)
{
    udev_device* parent = udev_device_get_parent_with_subsystem_devtype(
        device, "usb", "usb_device");
    const char* vendor = parent != nullptr ?
        udev_device_get_sysattr_value(parent, "idVendor") : nullptr;
    if (vendor == nullptr) {
        return false;
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(vendor, &end, 16);
    return end != vendor && *end == '\0' && value == kWacomVendorId;
}

PlankWacomTransportDecision connectedWacomTransportDecision()
{
    PlankWacomTransportDecision decision = {
        PlankWacomTransport::ExactRawHid, 0, 0};
    udev* context = udev_new();
    if (context == nullptr) {
        return decision;
    }

    std::vector<std::pair<std::string, std::uint32_t> > candidates;
    udev_enumerate* enumerate = udev_enumerate_new(context);
    udev_enumerate_add_match_subsystem(enumerate, "hidraw");
    udev_enumerate_scan_devices(enumerate);
    udev_list_entry* devices = udev_enumerate_get_list_entry(enumerate);
    udev_list_entry* entry = nullptr;
    udev_list_entry_foreach(entry, devices) {
        udev_device* device = udev_device_new_from_syspath(
            context, udev_list_entry_get_name(entry));
        if (device != nullptr && isWacomUsbDevice(device)) {
            udev_device* parent = udev_device_get_parent_with_subsystem_devtype(
                device, "usb", "usb_device");
            const char* product = parent != nullptr ?
                udev_device_get_sysattr_value(parent, "idProduct") : nullptr;
            char* end = nullptr;
            const unsigned long value = product != nullptr ?
                std::strtoul(product, &end, 16) : 0;
            if (product != nullptr && end != product && *end == '\0' &&
                    value <= 0xffff) {
                candidates.push_back(std::make_pair(
                    usbParentPath(device), static_cast<std::uint32_t>(value)));
            }
        }
        if (device != nullptr) {
            udev_device_unref(device);
        }
    }
    udev_enumerate_unref(enumerate);
    udev_unref(context);

    if (candidates.empty()) {
        return decision;
    }

    std::sort(candidates.begin(), candidates.end());
    decision.vendor = kWacomVendorId;
    decision.product = candidates.front().second;
    decision.transport = plankWacomTransportForUsbDevice(
        decision.vendor, decision.product);
    return decision;
}

unsigned long getReportIoctl(std::uint8_t type, std::size_t size)
{
    switch (type) {
    case UHID_FEATURE_REPORT:
        return HIDIOCGFEATURE(size);
    case UHID_OUTPUT_REPORT:
        return HIDIOCGOUTPUT(size);
    case UHID_INPUT_REPORT:
        return HIDIOCGINPUT(size);
    default:
        return 0;
    }
}

unsigned long setReportIoctl(std::uint8_t type, std::size_t size)
{
    switch (type) {
    case UHID_FEATURE_REPORT:
        return HIDIOCSFEATURE(size);
    case UHID_OUTPUT_REPORT:
        return HIDIOCSOUTPUT(size);
    case UHID_INPUT_REPORT:
        return HIDIOCSINPUT(size);
    default:
        return 0;
    }
}

template<typename T>
T readLittle(const T& source)
{
    T wire;
    std::memcpy(&wire, &source, sizeof(wire));
    return qFromLittleEndian(wire);
}

template<typename T>
void writeLittle(T& destination, T value)
{
    value = qToLittleEndian(value);
    std::memcpy(&destination, &value, sizeof(value));
}

} // namespace

PlankWacomTransportDecision plankWacomTransportForConnectedDevice()
{
    return connectedWacomTransportDecision();
}

LinuxRawWacomInput::LinuxRawWacomInput(std::function<void()> tabletActivity)
    : m_Active(false),
      m_Stopping(false),
      m_Reconnecting(false),
      m_AttachFailed(false),
      m_Generation(0),
      m_InputSequence(0),
      m_AttachPending(false),
      m_Attached(false),
      m_TabletActivity(std::move(tabletActivity))
{
    m_Thread = std::thread(&LinuxRawWacomInput::run, this);
}

LinuxRawWacomInput::~LinuxRawWacomInput()
{
    m_Active.store(false);
    m_Stopping.store(true);
    m_Changed.notify_all();
    if (m_Thread.joinable()) {
        m_Thread.join();
    }
}

void LinuxRawWacomInput::setActive(bool active)
{
    if (!active) {
        m_Active.store(false);
        synchronizeSuspend();
        return;
    }

    if (!m_Active.exchange(true)) {
        m_AttachFailed.store(false);
    }
    m_Changed.notify_all();
}

void LinuxRawWacomInput::beginReconnect()
{
    m_Reconnecting.store(true);

    // Stop the old transport before LiStopConnection() tears down its control
    // channel. The host keeps the stable UHID endpoints for the resumed
    // session, while the reconnect barrier prevents this worker from racing
    // ahead and attaching to a partially initialized replacement channel.
    synchronizeSuspend();
    m_AttachFailed.store(false);
}

void LinuxRawWacomInput::finishReconnect()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    // beginReconnect acknowledged handle retirement before transport teardown.
    m_Controls.clear();
    m_AttachFailed.store(false);
    m_Reconnecting.store(false);
    m_Changed.notify_all();
}

void LinuxRawWacomInput::synchronizeSuspend()
{
    std::unique_lock<std::mutex> lock(m_Mutex);
    const auto ticket = ++m_SuspendRequested;
    m_Changed.notify_all();
    m_Changed.wait(lock, [&] { return m_Finished || m_SuspendCompleted >= ticket; });
}

void LinuxRawWacomInput::waitForWork(std::chrono::milliseconds delay)
{
    std::unique_lock<std::mutex> lock(m_Mutex);
    m_Changed.wait_for(lock, delay, [&] {
        return m_Stopping.load() || m_SuspendRequested != m_SuspendCompleted ||
               !m_Controls.empty();
    });
}

void LinuxRawWacomInput::run()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                "PLANK exact raw Wacom capture initialized");
    while (!m_Stopping.load()) {
        std::deque<std::vector<unsigned char>> controls;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            if (m_SuspendRequested != m_SuspendCompleted || m_ControlOverflow) {
                const auto ticket = m_SuspendRequested;
                const bool overflow = m_ControlOverflow;
                m_ControlOverflow = false;
                m_Controls.clear();
                lock.unlock();
                suspendForFocusLoss();
                if (overflow) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Wacom control queue full; restarting attachment");
                    m_AttachFailed.store(true);
                }
                lock.lock();
                m_SuspendCompleted = ticket;
                m_Changed.notify_all();
            }
            controls.swap(m_Controls);
        }
        if (!m_Active.load() || m_Reconnecting.load()) {
            if (!m_Interfaces.empty()) {
                suspendForFocusLoss();
            }
            waitForWork(std::chrono::milliseconds(10));
            continue;
        }

        for (const auto& control : controls) {
            if (!m_Active.load() || m_Reconnecting.load()) break;
            processControl(control.data(), static_cast<unsigned int>(control.size()));
        }

        bool delayRetry = false;
        {
            for (const auto& result : m_ReportWorker.take()) {
                if (!m_Active.load() || m_Reconnecting.load()) break;
                if (result.type == 0) continue; // UHID_OUTPUT has no reply.
                if (!sendFrame(result.type, result.interfaceId, result.transaction,
                               result.payload.data(), result.payload.size())) {
                    suspendForFocusLoss();
                    m_AttachFailed.store(true);
                    break;
                }
            }
            if (m_AttachPending &&
                    std::chrono::steady_clock::now() >= m_AttachDeadline) {
                SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                            "Timed out waiting for exact Wacom host attachment; retrying");
                release(false);
                delayRetry = true;
            }
            else if (m_Interfaces.empty()) {
                if (m_AttachFailed.exchange(false)) {
                    delayRetry = true;
                }
                else if (!discover() || !sendAttach()) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                                "Unable to attach exact Wacom device; will retry after checking hidraw and input permissions");
                    release(false);
                    m_AttachFailed.store(true);
                }
            }
        }

        if (delayRetry) {
            waitForWork(std::chrono::seconds(1));
            continue;
        }

        handlePhysicalReports();
    }

    // Normal stream teardown may be followed by a resume into the same host
    // application. Release the physical tablet locally, but let the host keep
    // its stable UHID/XInput endpoints.
    release(false);
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Finished = true;
    m_Changed.notify_all();
}

bool LinuxRawWacomInput::discover()
{
    udev* context = udev_new();
    if (context == nullptr) {
        return false;
    }

    std::vector<std::pair<std::string, std::string> > candidates;
    udev_enumerate* enumerate = udev_enumerate_new(context);
    udev_enumerate_add_match_subsystem(enumerate, "hidraw");
    udev_enumerate_scan_devices(enumerate);
    udev_list_entry* devices = udev_enumerate_get_list_entry(enumerate);
    udev_list_entry* entry = nullptr;
    udev_list_entry_foreach(entry, devices) {
        udev_device* device = udev_device_new_from_syspath(
            context, udev_list_entry_get_name(entry));
        const char* node = device != nullptr ? udev_device_get_devnode(device) : nullptr;
        if (device != nullptr && node != nullptr && isWacomUsbDevice(device)) {
            candidates.push_back(std::make_pair(usbParentPath(device), node));
        }
        if (device != nullptr) {
            udev_device_unref(device);
        }
    }
    udev_enumerate_unref(enumerate);

    if (candidates.empty()) {
        udev_unref(context);
        return false;
    }
    std::sort(candidates.begin(), candidates.end());
    const std::string selectedParent = candidates.front().first;
    const std::size_t selectedCount = static_cast<std::size_t>(std::count_if(
        candidates.begin(), candidates.end(),
        [&selectedParent](const std::pair<std::string, std::string>& candidate) {
            return candidate.first == selectedParent;
        }));
    if (selectedCount == 0 || selectedCount > PLANK_RAW_HID_MAX_INTERFACES) {
        udev_unref(context);
        return false;
    }

    hidraw_devinfo expectedInfo = {};
    for (const auto& candidate : candidates) {
        if (candidate.first != selectedParent) {
            continue;
        }
        const int fd = open(candidate.second.c_str(),
                            O_RDWR | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) {
            udev_unref(context);
            return false;
        }
        hidraw_devinfo info = {};
        if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0 ||
                (m_Interfaces.empty() ? false :
                 info.bustype != expectedInfo.bustype ||
                 info.vendor != expectedInfo.vendor ||
                 info.product != expectedInfo.product)) {
            close(fd);
            udev_unref(context);
            return false;
        }
        if (m_Interfaces.empty()) {
            expectedInfo = info;
        }
        int descriptorSize = 0;
        if (ioctl(fd, HIDIOCGRDESCSIZE, &descriptorSize) < 0 ||
                descriptorSize <= 0 ||
                descriptorSize > static_cast<int>(PLANK_RAW_HID_MAX_DESCRIPTOR_SIZE)) {
            close(fd);
            udev_unref(context);
            return false;
        }
        hidraw_report_descriptor descriptor = {};
        descriptor.size = descriptorSize;
        if (ioctl(fd, HIDIOCGRDESC, &descriptor) < 0) {
            close(fd);
            udev_unref(context);
            return false;
        }
        HidInterface interface;
        interface.fd = fd;
        interface.descriptor.assign(descriptor.value,
                                    descriptor.value + descriptor.size);
        m_Interfaces.push_back(interface);
    }

    bool allEventsOpened = true;
    enumerate = udev_enumerate_new(context);
    udev_enumerate_add_match_subsystem(enumerate, "input");
    udev_enumerate_scan_devices(enumerate);
    devices = udev_enumerate_get_list_entry(enumerate);
    udev_list_entry_foreach(entry, devices) {
        udev_device* device = udev_device_new_from_syspath(
            context, udev_list_entry_get_name(entry));
        const char* node = device != nullptr ? udev_device_get_devnode(device) : nullptr;
        if (device != nullptr && node != nullptr &&
                usbParentPath(device) == selectedParent &&
                std::strncmp(node, "/dev/input/event", 16) == 0) {
            const int fd = open(node, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
            if (fd >= 0) {
                m_EventFds.push_back(fd);
            } else {
                allEventsOpened = false;
            }
        }
        if (device != nullptr) {
            udev_device_unref(device);
        }
    }
    udev_enumerate_unref(enumerate);
    udev_unref(context);

    return allEventsOpened && !m_Interfaces.empty() && !m_EventFds.empty();
}

bool LinuxRawWacomInput::sendAttach()
{
    PLANK_RAW_HID_DEVICE_MESSAGE device = {};
    device.interfaceCount = qToLittleEndian(
        static_cast<std::uint16_t>(m_Interfaces.size()));

    hidraw_devinfo info = {};
    if (ioctl(m_Interfaces.front().fd, HIDIOCGRAWINFO, &info) < 0) {
        return false;
    }
    device.bus = qToLittleEndian(static_cast<std::uint16_t>(info.bustype));
    device.vendor = qToLittleEndian(static_cast<std::uint32_t>(info.vendor));
    device.product = qToLittleEndian(static_cast<std::uint32_t>(info.product));

    std::array<char, 128> name = {};
    std::array<char, 64> physical = {};
    std::array<char, 64> unique = {};
    ioctl(m_Interfaces.front().fd, HIDIOCGRAWNAME(name.size()), name.data());
    ioctl(m_Interfaces.front().fd, HIDIOCGRAWPHYS(physical.size()), physical.data());
    ioctl(m_Interfaces.front().fd, HIDIOCGRAWUNIQ(unique.size()), unique.data());
    std::memcpy(device.name, name.data(), sizeof(device.name));
    std::memcpy(device.physical, physical.data(), sizeof(device.physical));
    std::memcpy(device.unique, unique.data(), sizeof(device.unique));

    input_id inputIdentity = {};
    if (!m_EventFds.empty() &&
            ioctl(m_EventFds.front(), EVIOCGID, &inputIdentity) == 0) {
        device.version = qToLittleEndian(
            static_cast<std::uint32_t>(inputIdentity.version));
    }

    m_Generation = nextGeneration();
    m_InputSequence = 0;
    if (!sendFrame(PLANK_RAW_HID_DEVICE, 0, 0,
                   reinterpret_cast<const unsigned char*>(&device),
                   sizeof(device))) {
        return false;
    }
    for (std::size_t index = 0; index < m_Interfaces.size(); ++index) {
        const std::vector<unsigned char>& descriptor = m_Interfaces[index].descriptor;
        if (!sendFrame(PLANK_RAW_HID_DESCRIPTOR,
                       static_cast<std::uint16_t>(index), 0,
                       descriptor.data(), descriptor.size())) {
            return false;
        }
    }
    m_AttachPending = true;
    m_AttachDeadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(15);
    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                "Sent exact Wacom attach: %u interfaces, generation %u",
                static_cast<unsigned int>(m_Interfaces.size()),
                static_cast<unsigned int>(m_Generation));
    return true;
}

bool LinuxRawWacomInput::sendFrame(std::uint16_t type,
                                   std::uint16_t interfaceId,
                                   std::uint32_t transactionId,
                                   const unsigned char* payload,
                                   std::size_t payloadLength)
{
    if (payloadLength > PLANK_RAW_HID_MAX_PAYLOAD_SIZE ||
            (payloadLength != 0 && payload == nullptr)) {
        return false;
    }
    std::vector<unsigned char> frame(sizeof(PLANK_RAW_HID_WIRE_HEADER) + payloadLength);
    PLANK_RAW_HID_WIRE_HEADER header = {};
    writeLittle(header.magic, static_cast<std::uint32_t>(PLANK_RAW_HID_WIRE_MAGIC));
    writeLittle(header.version, static_cast<std::uint16_t>(PLANK_RAW_HID_WIRE_VERSION));
    writeLittle(header.type, type);
    writeLittle(header.interfaceId, interfaceId);
    writeLittle(header.generation, m_Generation);
    writeLittle(header.transactionId, transactionId);
    writeLittle(header.payloadLength, static_cast<std::uint32_t>(payloadLength));
    std::memcpy(frame.data(), &header, sizeof(header));
    if (payloadLength != 0) {
        std::memcpy(frame.data() + sizeof(header), payload, payloadLength);
    }
    return LiSendRawHidEvent(frame.data(), static_cast<unsigned int>(frame.size())) == 0;
}

void LinuxRawWacomInput::handlePhysicalReports()
{
    if (!m_Attached || m_Interfaces.empty()) {
        waitForWork(std::chrono::milliseconds(2));
        return;
    }

    std::vector<pollfd> pollFds;
    for (const HidInterface& interface : m_Interfaces) {
        pollfd descriptor = {interface.fd, POLLIN, 0};
        pollFds.push_back(descriptor);
    }
    const int result = poll(pollFds.data(), pollFds.size(), 10);
    if (result < 0 && errno != EINTR) {
        release(true);
        m_AttachFailed.store(false);
        return;
    }

    std::array<unsigned char, PLANK_RAW_HID_MAX_REPORT_SIZE> report = {};
    for (std::size_t index = 0; index < pollFds.size(); ++index) {
        if ((pollFds[index].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            release(true);
            m_AttachFailed.store(false);
            return;
        }
        if ((pollFds[index].revents & POLLIN) == 0) {
            continue;
        }
        // Drain bursts fairly instead of reading one report every 2 ms. Never
        // coalesce raw reports: a report may contain a tip/button transition.
        for (unsigned count = 0; count < 32; ++count) {
            if (!m_Active.load() || m_Reconnecting.load() || m_Stopping.load()) return;
            const ssize_t bytes = read(pollFds[index].fd, report.data(), report.size());
            if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (bytes < 0 && errno == EINTR) continue;
            if (bytes <= 0) {
                release(true);
                return;
            }
            if (!sendFrame(PLANK_RAW_HID_INPUT, static_cast<std::uint16_t>(index),
                           ++m_InputSequence, report.data(), static_cast<std::size_t>(bytes))) {
                suspendForFocusLoss();
                m_AttachFailed.store(true);
                return;
            }
            if (m_TabletActivity) m_TabletActivity();
        }
    }
}

void LinuxRawWacomInput::handleControl(const unsigned char* data,
                                       unsigned int length)
{
    if (!data || length < sizeof(PLANK_RAW_HID_WIRE_HEADER) ||
            length > sizeof(PLANK_RAW_HID_WIRE_HEADER) + PLANK_RAW_HID_MAX_PAYLOAD_SIZE) return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Finished || m_Stopping.load() || m_Reconnecting.load() || !m_Active.load()) return;
    if (m_Controls.size() == 64) {
        m_ControlOverflow = true;
    }
    else {
        m_Controls.emplace_back(data, data + length);
    }
    m_Changed.notify_all();
}

void LinuxRawWacomInput::processControl(const unsigned char* data,
                                        unsigned int length)
{
    if (data == nullptr || length < sizeof(PLANK_RAW_HID_WIRE_HEADER)) {
        return;
    }
    PLANK_RAW_HID_WIRE_HEADER header;
    std::memcpy(&header, data, sizeof(header));
    const std::uint32_t payloadLength = readLittle(header.payloadLength);
    if (readLittle(header.magic) != PLANK_RAW_HID_WIRE_MAGIC ||
            readLittle(header.version) != PLANK_RAW_HID_WIRE_VERSION ||
            payloadLength > PLANK_RAW_HID_MAX_PAYLOAD_SIZE ||
            length != sizeof(header) + payloadLength) {
        return;
    }

    const std::uint16_t type = readLittle(header.type);
    const std::uint16_t interfaceId = readLittle(header.interfaceId);
    const std::uint16_t generation = readLittle(header.generation);
    const std::uint32_t transactionId = readLittle(header.transactionId);
    const unsigned char* payload = data + sizeof(header);

    if (generation != m_Generation || m_Interfaces.empty()) {
        return;
    }
    if (type == PLANK_RAW_HID_ATTACH_RESULT && payloadLength == sizeof(std::int32_t)) {
        if (!m_AttachPending) return;
        std::int32_t result;
        std::memcpy(&result, payload, sizeof(result));
        result = qFromLittleEndian(result);
        m_AttachPending = false;
        if (result == 0 && setGrabbed(true)) {
            m_Attached = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "Exact Wacom device attached to host");
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                        "Host rejected exact Wacom attach: %s",
                        std::strerror(result));
            if (result == 0) sendFrame(PLANK_RAW_HID_SUSPEND, 0, 0, nullptr, 0);
            suspendForFocusLoss();
            m_AttachFailed.store(true);
        }
    }
    else if (type == PLANK_RAW_HID_GET_REPORT ||
             type == PLANK_RAW_HID_SET_REPORT || type == PLANK_RAW_HID_OUTPUT) {
        queueReport(type, interfaceId, transactionId, payload, payloadLength);
    }
}

void LinuxRawWacomInput::queueReport(std::uint16_t type,
                                       std::uint16_t interfaceId,
                                       std::uint32_t transactionId,
                                       const unsigned char* payload,
                                       std::size_t payloadLength)
{
    const bool get = type == PLANK_RAW_HID_GET_REPORT;
    const std::uint16_t replyType = type == PLANK_RAW_HID_OUTPUT ? 0 :
        (get ? PLANK_RAW_HID_GET_REPORT_REPLY : PLANK_RAW_HID_SET_REPORT_REPLY);
    int error = EINVAL;
    if (interfaceId < m_Interfaces.size() &&
            (get ? payloadLength == 2 : payloadLength >= 2)) {
        const int fd = fcntl(m_Interfaces[interfaceId].fd, F_DUPFD_CLOEXEC, 0);
        error = fd < 0 ? errno : EBUSY;
        if (fd >= 0) {
            // The job owns this duplicate through suspend/unplug. It never
            // locks the capture mutex or calls back into the Client object.
            std::shared_ptr<int> ownedFd(new int(fd), [](int* value) {
                close(*value);
                delete value;
            });
            std::vector<unsigned char> requestPayload(payload, payload + payloadLength);
            if (m_ReportWorker.submit([ownedFd, get, replyType, interfaceId,
                                      transactionId, requestPayload = std::move(requestPayload)] {
                std::vector<unsigned char> report(get ? PLANK_RAW_HID_MAX_REPORT_SIZE :
                                                       requestPayload.size() - 1, 0);
                if (get) report[0] = requestPayload[0];
                else std::copy(requestPayload.begin() + 1, requestPayload.end(), report.begin());
                const unsigned long request = get ?
                    getReportIoctl(requestPayload[1], report.size()) :
                    setReportIoctl(requestPayload[0], report.size());
                const int result = request ? ioctl(*ownedFd, request, report.data()) : -1;
                const std::int32_t status = qToLittleEndian<std::int32_t>(
                    result < 0 ? (request ? errno : EINVAL) :
                    (get && static_cast<std::size_t>(result) > report.size() ? EOVERFLOW : 0));
                const std::size_t size = get && status == 0 ?
                    static_cast<std::size_t>(result) : 0;
                LinuxWacomReportWorker::Result reply {replyType, interfaceId, transactionId,
                    std::vector<unsigned char>(sizeof(status) + size)};
                std::memcpy(reply.payload.data(), &status, sizeof(status));
                if (size) std::memcpy(reply.payload.data() + sizeof(status), report.data(), size);
                return reply;
            })) {
                return;
            }
        }
    }
    if (type != PLANK_RAW_HID_OUTPUT) {
        const std::int32_t status = qToLittleEndian<std::int32_t>(error);
        if (!sendFrame(replyType, interfaceId, transactionId,
                       reinterpret_cast<const unsigned char*>(&status), sizeof(status))) {
            suspendForFocusLoss();
        }
    }
}

bool LinuxRawWacomInput::setGrabbed(bool grabbed)
{
    const int value = grabbed ? 1 : 0;
    bool success = true;
    for (int fd : m_EventFds) {
        if (ioctl(fd, EVIOCGRAB, value) < 0) {
            success = false;
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                        "Unable to %s Wacom event node: %s",
                        grabbed ? "grab" : "release", std::strerror(errno));
        }
    }
    if (grabbed && !success) {
        // A partial grab would send some tablet events to the local desktop.
        for (int fd : m_EventFds) ioctl(fd, EVIOCGRAB, 0);
    }
    return success;
}

void LinuxRawWacomInput::suspendForFocusLoss()
{
    if (m_AttachPending || m_Attached) {
        if (sendFrame(PLANK_RAW_HID_SUSPEND, 0, 0, nullptr, 0)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "Suspended exact Wacom forwarding while preserving host endpoints");
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                        "Unable to suspend exact Wacom forwarding cleanly");
        }
    }
    release(false);
}

void LinuxRawWacomInput::release(bool notifyHost)
{
    m_ReportWorker.invalidate();
    if (notifyHost && (m_AttachPending || m_Attached)) {
        sendFrame(PLANK_RAW_HID_DETACH, 0, 0, nullptr, 0);
    }
    if (m_Attached) {
        setGrabbed(false);
    }
    for (int fd : m_EventFds) {
        close(fd);
    }
    for (const HidInterface& interface : m_Interfaces) {
        close(interface.fd);
    }
    m_EventFds.clear();
    m_Interfaces.clear();
    m_AttachPending = false;
    m_Attached = false;
}
