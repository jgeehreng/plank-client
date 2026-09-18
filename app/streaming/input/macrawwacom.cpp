#include "macrawwacom.h"
#include "macrawwacomasync.h"
#include "macrawwacomlogic.h"
#include "macwacomvendordriver.h"
#include "linuxrawwacom.h" // shared device-family policy; no Linux dependencies
#include <Limelight.h>
#include <SDL3/SDL.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace {
using Clock = std::chrono::steady_clock;
std::atomic<unsigned> nextGeneration{0};
// A driver that never invokes a timed-out callback may retain its request
// context until process exit. Bound that storage instead of freeing memory a
// late callback could still touch.
std::atomic<unsigned> outstandingReports{0};
constexpr unsigned MaxOutstandingReports = 64;
constexpr CFTimeInterval ReportTimeoutMilliseconds = 1000;
long number(IOHIDDeviceRef device, CFStringRef key)
{
    CFTypeRef value = IOHIDDeviceGetProperty(device, key);
    long result = 0;
    if (value && CFGetTypeID(value) == CFNumberGetTypeID())
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongType, &result);
    return result;
}
void stringProperty(IOHIDDeviceRef device, CFStringRef key, char* target, std::size_t capacity)
{
    CFTypeRef value = IOHIDDeviceGetProperty(device, key);
    if (value && CFGetTypeID(value) == CFStringGetTypeID())
        CFStringGetCString(static_cast<CFStringRef>(value), target, capacity, kCFStringEncodingUTF8);
    target[capacity - 1] = 0;
}
std::uint64_t usbGroup(IOHIDDeviceRef device, long& interfaceNumber)
{
    io_registry_entry_t entry = IOHIDDeviceGetService(device);
    IOObjectRetain(entry);
    std::uint64_t group = 0;
    interfaceNumber = -1;
    for (int depth = 0; entry && depth < 16; ++depth) {
        CFTypeRef value = IORegistryEntryCreateCFProperty(entry, CFSTR("bInterfaceNumber"), kCFAllocatorDefault, 0);
        if (value) {
            if (interfaceNumber < 0 && CFGetTypeID(value) == CFNumberGetTypeID())
                CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongType, &interfaceNumber);
            CFRelease(value);
        }
        if (IOObjectConformsTo(entry, "IOUSBHostDevice")) {
            IORegistryEntryGetRegistryEntryID(entry, &group);
            break;
        }
        io_registry_entry_t parent = 0;
        IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent);
        IOObjectRelease(entry);
        entry = parent;
    }
    if (entry) IOObjectRelease(entry);
    return group;
}
int errorNumber(IOReturn result)
{
    switch (result) {
    case kIOReturnSuccess: return 0;
    case kIOReturnNotPermitted: case kIOReturnNotPrivileged: return EACCES;
    case kIOReturnExclusiveAccess: case kIOReturnBusy: return EBUSY;
    case kIOReturnNoDevice: case kIOReturnNotAttached: return ENODEV;
    case kIOReturnBadArgument: return EINVAL;
    // Replies carry Linux errno values, not Darwin's different numeric values.
    case kIOReturnUnsupported: return 95; // Linux EOPNOTSUPP
    case kIOReturnTimeout: return 110; // Linux ETIMEDOUT
    case kIOReturnNoMemory: return ENOMEM;
    default: return EIO;
    }
}
int runTool(const char* path, const std::vector<std::string>& args)
{
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(path));
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, path, &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
std::string launchdDomain()
{
    return "gui/" + std::to_string(getuid());
}
MacWacomVendorDriver::Tools vendorDriverTools()
{
    MacWacomVendorDriver::Tools tools;
    tools.domain = [] { return launchdDomain(); };
    tools.plistExists = [](std::string_view path) {
        return ::access(std::string(path).c_str(), R_OK) == 0;
    };
    tools.launchctl = [](const std::vector<std::string>& args) {
        return runTool("/bin/launchctl", args);
    };
    tools.terminateProcesses = [](const std::vector<std::string>& names) {
        for (const auto& name : names)
            runTool("/usr/bin/killall", {"-TERM", name});
    };
    return tools;
}
MacWacomVendorDriver::Hold& vendorDriverHold()
{
    static MacWacomVendorDriver::Hold hold(vendorDriverTools());
    return hold;
}
void pauseVendorDriver()
{
    auto& hold = vendorDriverHold();
    const bool first = !hold.held();
    static std::once_flag once;
    std::call_once(once, [] {
        std::atexit([] { vendorDriverHold().restore(); });
    });
    hold.pause();
    if (!first) return;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Mac Wacom: paused vendor driver for exclusive forwarding");
    if (!hold.stoppedPlists().empty())
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
}
void restoreVendorDriver()
{
    auto& hold = vendorDriverHold();
    if (!hold.held()) return;
    hold.restore();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mac Wacom: restored vendor driver");
}
}

class MacRawWacomInput::Impl : public std::enable_shared_from_this<Impl>
{
    struct ActivityGuard {
        explicit ActivityGuard(std::function<void()> callback) : callback(std::move(callback)) {}
        std::mutex mutex;
        bool enabled = true;
        std::function<void()> callback;
    };
public:
    explicit Impl(std::function<void()> activity)
        : activity(std::make_shared<ActivityGuard>(std::move(activity)))
    {
        // Constructor runs on the Client UI thread. Only the normal OS prompt
        // may grant access; never edit privacy databases or run as root.
        if (IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeUnknown)
            IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
    }
    void start()
    {
        worker = std::thread([self = shared_from_this()] { self->run(); });
    }
    ~Impl()
    {
        // The worker can own the last reference after a timed-out shutdown.
        if (worker.joinable()) worker.detach();
    }
    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(activity->mutex);
            activity->enabled = false;
            activity->callback = nullptr;
        }
        const bool released = barrier(lifecycle.stop());
        shutdownRequested = true;
        stopping = true;
        const bool exited = lifecycle.waitExited(std::chrono::seconds(2));
        if (worker.joinable()) {
            if (exited) worker.join();
            else worker.detach(); // self-owned state remains until the worker exits
        }
        if (exited) restoreVendorDriver();
        if (!released || !exited)
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Mac Wacom shutdown exceeded release deadline");
    }
    void setActive(bool value)
    {
        barrier(lifecycle.setActive(value));
    }
    void beginReconnect() { barrier(lifecycle.beginReconnect()); }
    void finishReconnect() { barrier(lifecycle.finishReconnect()); }
    void control(const unsigned char* data, unsigned length)
    {
        MacWacomWire::Control parsed;
        if (!MacWacomWire::parse(data, length, parsed)) return;
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) return;
        if (queue.size() >= 128) { overflow = true; return; }
        queue.emplace_back(data, data + length);
    }
private:
    struct Interface {
        Impl* owner;
        std::uint16_t index;
        IOHIDDeviceRef device = nullptr;
        std::array<unsigned char, PLANK_RAW_HID_MAX_REPORT_SIZE> buffer{};
        std::array<bool, 256> activityReports{};
        std::vector<unsigned char> descriptor;
    };
    struct ReportRequest {
        std::weak_ptr<MacWacomAsyncResults> results;
        MacWacomAsyncResults::Completion completion;
        IOHIDDeviceRef device;
        std::array<unsigned char, PLANK_RAW_HID_MAX_REPORT_SIZE> buffer{};
        CFIndex length = 0;
        std::size_t prefix = 0;

        ~ReportRequest()
        {
            if (device) CFRelease(device);
            --outstandingReports;
        }
    };
    std::shared_ptr<ActivityGuard> activity;
    std::shared_ptr<MacWacomAsyncResults> reportResults = std::make_shared<MacWacomAsyncResults>();
    MacWacomLifecycle lifecycle;
    std::atomic<bool> stopping{false}, shutdownRequested{false}, overflow{false};
    std::mutex mutex;
    std::deque<std::vector<unsigned char>> queue;
    std::thread worker;
    std::vector<std::unique_ptr<Interface>> interfaces;
    std::uint16_t generation = 0;
    std::uint32_t sequence = 0;
    bool pending = false, attached = false, ioFailed = false;
    Clock::time_point retry{}, deadline{};

    bool barrier(std::uint64_t ticket)
    {
        if (lifecycle.wait(ticket, std::chrono::seconds(2))) return true;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Mac Wacom release barrier timed out");
        return false;
    }
    bool send(std::uint16_t type, std::uint16_t index, std::uint32_t transaction,
              const unsigned char* payload = nullptr, std::size_t size = 0)
    {
        if (shutdownRequested || size > PLANK_RAW_HID_MAX_PAYLOAD_SIZE ||
            (size && !payload)) return false;
        PLANK_RAW_HID_WIRE_HEADER h{};
        h.magic = qToLittleEndian(std::uint32_t(PLANK_RAW_HID_WIRE_MAGIC));
        h.version = qToLittleEndian(std::uint16_t(PLANK_RAW_HID_WIRE_VERSION));
        h.type = qToLittleEndian(type); h.interfaceId = qToLittleEndian(index);
        h.generation = qToLittleEndian(generation); h.transactionId = qToLittleEndian(transaction);
        h.payloadLength = qToLittleEndian(std::uint32_t(size));
        std::vector<unsigned char> frame(sizeof(h) + size);
        std::memcpy(frame.data(), &h, sizeof(h));
        if (size) std::memcpy(frame.data() + sizeof(h), payload, size);
        return LiSendRawHidEvent(frame.data(), static_cast<unsigned>(frame.size())) == 0;
    }
    static void input(void* context, IOReturn result, void*, IOHIDReportType,
                      std::uint32_t reportId, unsigned char* bytes, CFIndex size)
    {
        auto& interface = *static_cast<Interface*>(context);
        auto& self = *interface.owner;
        if (result != kIOReturnSuccess || size <= 0 || size > PLANK_RAW_HID_MAX_REPORT_SIZE) {
            self.ioFailed = true; return;
        }
        if (!self.attached || !self.lifecycle.canForward()) return;
        if (!self.send(PLANK_RAW_HID_INPUT, interface.index, ++self.sequence, bytes, size)) {
            self.ioFailed = true; return;
        }
        // Battery/status packets must not reclaim the cursor from a real mouse.
        if (reportId < interface.activityReports.size() && interface.activityReports[reportId]) {
            std::lock_guard<std::mutex> lock(self.activity->mutex);
            if (self.activity->enabled && self.activity->callback) self.activity->callback();
        }
    }
    static void removed(void* context, IOReturn, void*)
    {
        static_cast<Interface*>(context)->owner->ioFailed = true;
    }
    static void reportCompleted(void* context, IOReturn result, void*, IOHIDReportType,
                                std::uint32_t, unsigned char* bytes, CFIndex size)
    {
        // The callback owns this request. It retains the device and buffer
        // even if the Client has already closed the physical lease.
        std::unique_ptr<ReportRequest> request(static_cast<ReportRequest*>(context));
        auto results = request->results.lock();
        if (!results) return;
        auto completion = std::move(request->completion);
        completion.result = result;
        if (completion.type == PLANK_RAW_HID_GET_REPORT && result == kIOReturnSuccess) {
            if (size <= 0 || std::size_t(size) > request->buffer.size() - request->prefix ||
                !bytes) completion.result = kIOReturnOverrun;
            else {
                if (request->prefix) completion.report.push_back(0);
                completion.report.insert(completion.report.end(), bytes, bytes + size);
            }
        }
        results->publish(std::move(completion));
    }
    void release(bool destructive)
    {
        if (pending || attached || !interfaces.empty()) reportResults->invalidate();
        if (pending || attached)
            send(destructive ? PLANK_RAW_HID_DETACH : PLANK_RAW_HID_SUSPEND, 0, 0);
        pending = attached = false;
        for (const auto& i : interfaces) {
            IOHIDDeviceRegisterInputReportCallback(i->device, i->buffer.data(), i->buffer.size(), nullptr, nullptr);
            IOHIDDeviceRegisterRemovalCallback(i->device, nullptr, nullptr);
            IOHIDDeviceUnscheduleFromRunLoop(i->device, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
            IOHIDDeviceClose(i->device, kIOHIDOptionsTypeSeizeDevice);
            CFRelease(i->device);
        }
        if (!interfaces.empty()) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mac Wacom ownership released");
        interfaces.clear();
        ioFailed = false;
    }
    bool discover()
    {
        if (IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) != kIOHIDAccessTypeGranted) return false;
        IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, 0);
        if (!manager) return false;
        const int vendor = 0x056a;
        CFNumberRef vendorValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendor);
        const void* keys[] = {CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDTransportKey)};
        const void* values[] = {vendorValue, CFSTR("USB")};
        CFDictionaryRef match = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        IOHIDManagerSetDeviceMatching(manager, match);
        CFRelease(match); CFRelease(vendorValue);
        CFSetRef devices = IOHIDManagerCopyDevices(manager);
        struct Entry { IOHIDDeviceRef device; std::uint64_t parent; long number; };
        std::vector<Entry> candidates;
        if (devices) {
            std::vector<const void*> members(CFSetGetCount(devices));
            CFSetGetValues(devices, members.data());
            for (const void* member : members) {
                IOHIDDeviceRef device = static_cast<IOHIDDeviceRef>(const_cast<void*>(member));
                long index;
                const auto parent = usbGroup(device, index);
                if (parent && index >= 0) candidates.push_back({device, parent, index});
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Entry& a, const Entry& b) {
            return a.parent == b.parent ? a.number < b.number : a.parent < b.parent;
        });
        bool success = !candidates.empty();
        if (success) {
            const auto parent = candidates.front().parent;
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                [parent](const Entry& e) { return e.parent != parent; }), candidates.end());
            success = candidates.size() <= PLANK_RAW_HID_MAX_INTERFACES;
            const auto product = number(candidates.front().device, CFSTR(kIOHIDProductIDKey));
            success &= plankWacomTransportForUsbDevice(vendor, product) == PlankWacomTransport::ExactRawHid;
            if (success && !vendorDriverHold().held()) {
                pauseVendorDriver();
                if (devices) CFRelease(devices);
                CFRelease(manager);
                return lifecycle.canForward() && discover();
            }
            for (const auto& candidate : candidates) {
                if (!success) break;
                CFTypeRef descriptor = IOHIDDeviceGetProperty(candidate.device, CFSTR(kIOHIDReportDescriptorKey));
                if (!descriptor || CFGetTypeID(descriptor) != CFDataGetTypeID() ||
                    CFDataGetLength(static_cast<CFDataRef>(descriptor)) <= 0 ||
                    CFDataGetLength(static_cast<CFDataRef>(descriptor)) > PLANK_RAW_HID_MAX_DESCRIPTOR_SIZE ||
                    number(candidate.device, CFSTR(kIOHIDProductIDKey)) != product) { success = false; break; }
                auto i = std::unique_ptr<Interface>(new Interface{});
                i->owner = this; i->index = static_cast<std::uint16_t>(interfaces.size());
                i->device = candidate.device;
                const auto data = static_cast<CFDataRef>(descriptor);
                i->descriptor.assign(CFDataGetBytePtr(data), CFDataGetBytePtr(data) + CFDataGetLength(data));
                CFArrayRef elements = IOHIDDeviceCopyMatchingElements(i->device, nullptr, 0);
                if (elements) {
                    for (CFIndex n = 0; n < CFArrayGetCount(elements); ++n) {
                        auto element = static_cast<IOHIDElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(elements, n)));
                        const auto kind = IOHIDElementGetType(element);
                        if (kind < kIOHIDElementTypeInput_Misc || kind > kIOHIDElementTypeInput_ScanCodes) continue;
                        const auto page = IOHIDElementGetUsagePage(element), usage = IOHIDElementGetUsage(element);
                        const auto id = IOHIDElementGetReportID(element);
                        const bool control = (page == 1 && (usage == 0x30 || usage == 0x31)) ||
                            page == 9 || page == 0xd || page == 0xff00 ||
                            (page == 0xff0d && (usage < 0x100 || usage == 0x130 || usage == 0x131 ||
                             usage == 0x138 || (usage >= 0x910 && usage <= 0x92f) || usage == 0x995));
                        if (control && id < i->activityReports.size()) i->activityReports[id] = true;
                    }
                    CFRelease(elements);
                }
                const IOReturn result = IOHIDDeviceOpen(i->device, kIOHIDOptionsTypeSeizeDevice);
                if (result != kIOReturnSuccess) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Mac Wacom exclusive open failed: 0x%x", unsigned(result));
                    success = false; break;
                }
                CFRetain(i->device);
                IOHIDDeviceRegisterInputReportCallback(i->device, i->buffer.data(), i->buffer.size(), input, i.get());
                IOHIDDeviceRegisterRemovalCallback(i->device, removed, i.get());
                IOHIDDeviceScheduleWithRunLoop(i->device, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
                interfaces.push_back(std::move(i));
            }
        }
        if (devices) CFRelease(devices);
        CFRelease(manager);
        if (!success) release(false);
        return success;
    }
    bool attach()
    {
        PLANK_RAW_HID_DEVICE_MESSAGE device{};
        IOHIDDeviceRef first = interfaces.front()->device;
        device.interfaceCount = qToLittleEndian(std::uint16_t(interfaces.size()));
        device.bus = qToLittleEndian(std::uint16_t(3)); // Linux BUS_USB
        device.vendor = qToLittleEndian(std::uint32_t(number(first, CFSTR(kIOHIDVendorIDKey))));
        device.product = qToLittleEndian(std::uint32_t(number(first, CFSTR(kIOHIDProductIDKey))));
        device.version = qToLittleEndian(std::uint32_t(number(first, CFSTR(kIOHIDVersionNumberKey))));
        device.country = qToLittleEndian(std::uint32_t(number(first, CFSTR(kIOHIDCountryCodeKey))));
        stringProperty(first, CFSTR(kIOHIDProductKey), device.name, sizeof(device.name));
        stringProperty(first, CFSTR(kIOHIDSerialNumberKey), device.unique, sizeof(device.unique));
        std::snprintf(device.physical, sizeof(device.physical), "plank/mac-usb/%08lx",
                      number(first, CFSTR(kIOHIDLocationIDKey)));
        do { generation = static_cast<std::uint16_t>(++nextGeneration); } while (!generation);
        sequence = 0;
        pending = true;
        deadline = Clock::now() + std::chrono::seconds(3);
        if (!send(PLANK_RAW_HID_DEVICE, 0, 0, reinterpret_cast<unsigned char*>(&device), sizeof(device))) return false;
        for (const auto& i : interfaces)
            if (!send(PLANK_RAW_HID_DESCRIPTOR, i->index, 0, i->descriptor.data(), i->descriptor.size())) return false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mac Wacom attach sent: %u interfaces, generation %u",
                    unsigned(interfaces.size()), unsigned(generation));
        return true;
    }
    void process(const std::vector<unsigned char>& bytes)
    {
        MacWacomWire::Control c;
        if (!MacWacomWire::parse(bytes.data(), bytes.size(), c) || c.generation != generation ||
            interfaces.empty() || !lifecycle.canForward()) return;
        const auto* payload = bytes.data() + sizeof(PLANK_RAW_HID_WIRE_HEADER);
        if (c.type == PLANK_RAW_HID_ATTACH_RESULT) {
            if (!pending) return;
            std::int32_t status;
            std::memcpy(&status, payload, sizeof(status));
            status = qFromLittleEndian(status);
            if (status != 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Mac Wacom host attach rejected: %d", int(status));
                release(false); retry = Clock::now() + std::chrono::seconds(1); return;
            }
            pending = false; attached = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mac Wacom attached; exclusive raw HID forwarding active");
            return;
        }
        if (c.interfaceId >= interfaces.size()) return;
        auto& i = *interfaces[c.interfaceId];
        MacWacomAsyncResults::Completion completion;
        completion.epoch = reportResults->epoch();
        completion.type = c.type;
        completion.interfaceId = c.interfaceId;
        completion.generation = c.generation;
        completion.transaction = c.transaction;
        IOReturn result = kIOReturnBadArgument;
        int type = -1;
        std::size_t prefix = 0;
        if (c.type == PLANK_RAW_HID_GET_REPORT) {
            type = MacWacomWire::ioReportType(payload[1]);
            prefix = MacWacomWire::reportPrefix(payload[0]);
        } else {
            type = MacWacomWire::ioReportType(payload[0]);
            prefix = MacWacomWire::reportPrefix(payload[1]);
        }
        if (type >= 0 && (c.type == PLANK_RAW_HID_GET_REPORT || c.size > 1 + prefix)) {
            unsigned pendingCount = outstandingReports.load();
            while (pendingCount < MaxOutstandingReports &&
                   !outstandingReports.compare_exchange_weak(pendingCount, pendingCount + 1)) {}
            if (pendingCount < MaxOutstandingReports) {
                auto* request = new ReportRequest;
                request->results = reportResults;
                request->completion = completion;
                CFRetain(i.device);
                request->device = i.device;
                request->prefix = prefix;
                if (c.type == PLANK_RAW_HID_GET_REPORT) {
                    request->buffer[0] = payload[0];
                    request->length = request->buffer.size() - prefix;
                    result = IOHIDDeviceGetReportWithCallback(i.device,
                        static_cast<IOHIDReportType>(type), payload[0],
                        request->buffer.data() + prefix, &request->length,
                        ReportTimeoutMilliseconds, reportCompleted, request);
                } else {
                    request->length = c.size - 1 - prefix;
                    std::memcpy(request->buffer.data(), payload + 1 + prefix, request->length);
                    result = IOHIDDeviceSetReportWithCallback(i.device,
                        static_cast<IOHIDReportType>(type), payload[1],
                        request->buffer.data(), request->length,
                        ReportTimeoutMilliseconds, reportCompleted, request);
                }
                if (result == kIOReturnSuccess) return; // callback now owns request
                delete request;
            } else result = kIOReturnBusy;
        }
        completion.result = result;
        completeReport(completion);
    }
    void completeReport(const MacWacomAsyncResults::Completion& completion)
    {
        if (!lifecycle.canForward() || interfaces.empty() ||
            completion.generation != generation) return;
        const auto result = static_cast<IOReturn>(completion.result);
        std::vector<unsigned char> reply(sizeof(std::int32_t));
        const auto error = qToLittleEndian(std::int32_t(errorNumber(result)));
        std::memcpy(reply.data(), &error, sizeof(error));
        if (result == kIOReturnSuccess && completion.type == PLANK_RAW_HID_GET_REPORT)
            reply.insert(reply.end(), completion.report.begin(), completion.report.end());
        if (completion.type != PLANK_RAW_HID_OUTPUT)
            send(completion.type == PLANK_RAW_HID_GET_REPORT ? PLANK_RAW_HID_GET_REPORT_REPLY : PLANK_RAW_HID_SET_REPORT_REPLY,
                 completion.interfaceId, completion.transaction, reply.data(), reply.size());
        if (result != kIOReturnSuccess)
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Mac Wacom report I/O failed: interface %u type %u result 0x%x",
                        unsigned(completion.interfaceId), unsigned(completion.type), unsigned(result));
        if (errorNumber(result) == ENODEV) ioFailed = true;
    }
    void run()
    {
        while (!stopping) {
            std::deque<std::vector<unsigned char>> controls;
            {
                std::lock_guard<std::mutex> lock(mutex);
                controls.swap(queue);
            }
            const auto barrierTicket = lifecycle.pendingTicket();
            if (barrierTicket) {
                release(false);
                controls.clear();
                lifecycle.complete(barrierTicket);
            }
            if (!lifecycle.canForward()) release(false);
            else {
                if (overflow.exchange(false)) { release(true); retry = Clock::now() + std::chrono::seconds(1); }
                for (const auto& bytes : controls) {
                    if (!lifecycle.canForward()) break;
                    process(bytes);
                }
                for (const auto& completion : reportResults->take()) completeReport(completion);
                if (ioFailed) release(true);
                if (pending && Clock::now() >= deadline) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Mac Wacom attachment timed out");
                    release(false); retry = Clock::now() + std::chrono::seconds(1);
                }
                if (interfaces.empty() && Clock::now() >= retry) {
                    // Discovery/open can outlive a focus or reconnect request.
                    // Do not advertise that lease after forwarding was revoked.
                    if (lifecycle.canForward() && discover() &&
                        (!lifecycle.canForward() || !attach())) release(false);
                    retry = Clock::now() + std::chrono::seconds(1);
                }
            }
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        release(false);
        restoreVendorDriver();
        lifecycle.markExited();
    }
};

MacRawWacomInput::MacRawWacomInput(std::function<void()> activity)
    : m_Impl(std::make_shared<Impl>(std::move(activity))) { m_Impl->start(); }
MacRawWacomInput::~MacRawWacomInput() { m_Impl->shutdown(); }
void MacRawWacomInput::setActive(bool active) { m_Impl->setActive(active); }
void MacRawWacomInput::beginReconnect() { m_Impl->beginReconnect(); }
void MacRawWacomInput::finishReconnect() { m_Impl->finishReconnect(); }
void MacRawWacomInput::handleControl(const unsigned char* data, unsigned int length) { m_Impl->control(data, length); }
