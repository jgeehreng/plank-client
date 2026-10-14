#include "../../app/streaming/input/linuxrawwacom.h"
#include "../../app/streaming/input/linuxwacom.h"
#include <Limelight.h>
#include <plank.h>
#include <libudev.h>
#include <cstdlib>
#include <future>
#include <iostream>

using namespace std::chrono_literals;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "check failed at line " << __LINE__ << ": " #condition "\n"; \
    std::abort(); \
} } while (false)

namespace {
std::mutex discoveryMutex;
std::condition_variable discoveryChanged;
bool holdDiscovery = true;
unsigned discoveryCount = 0;
std::atomic<unsigned> cancels{0};
}

// Exercise the real worker, control callback and focus/reconnect barriers,
// without enumerating or grabbing any physical input device.
extern "C" udev* __wrap_udev_new()
{
    std::unique_lock<std::mutex> lock(discoveryMutex);
    ++discoveryCount;
    discoveryChanged.notify_all();
    discoveryChanged.wait(lock, [] { return !holdDiscovery; });
    return nullptr;
}

extern "C" int LiSendRawHidEvent(const unsigned char*, unsigned int) { return 0; }
extern "C" int LiSendPenEvent(uint8_t event, uint8_t, uint8_t,
    float, float, float, float, float, uint16_t, uint8_t)
{
    CHECK(event == LI_TOUCH_EVENT_CANCEL_ALL);
    ++cancels;
    return 0;
}

int main()
{
    {
        LinuxRawWacomInput raw([] {});
        raw.setActive(true);
        {
            std::unique_lock<std::mutex> lock(discoveryMutex);
            CHECK(discoveryChanged.wait_for(lock, 1s, [] { return discoveryCount != 0; }));
        }
        PLANK_RAW_HID_WIRE_HEADER control{};
        control.magic = PLANK_RAW_HID_WIRE_MAGIC;
        control.version = PLANK_RAW_HID_WIRE_VERSION;
        auto callback = std::async(std::launch::async, [&] {
            raw.handleControl(reinterpret_cast<unsigned char*>(&control), sizeof(control));
        });
        // Slow device discovery must not hold up the transport control thread.
        const auto callbackResult = callback.wait_for(200ms);
        auto reconnect = std::async(std::launch::async, [&] { raw.beginReconnect(); });
        const auto reconnectResult = reconnect.wait_for(30ms);
        {
            std::lock_guard<std::mutex> lock(discoveryMutex);
            holdDiscovery = false;
            discoveryChanged.notify_all();
        }
        CHECK(callbackResult == std::future_status::ready);
        CHECK(reconnectResult == std::future_status::timeout);
        CHECK(reconnect.wait_for(500ms) == std::future_status::ready);
        reconnect.get();
        callback.get();
        // No new discovery or attach while the old stream is being replaced.
        {
            std::unique_lock<std::mutex> lock(discoveryMutex);
            const auto before = discoveryCount;
            CHECK(!discoveryChanged.wait_for(lock, 50ms, [&] { return discoveryCount != before; }));
        }
        raw.finishReconnect();
        {
            std::unique_lock<std::mutex> lock(discoveryMutex);
            CHECK(discoveryChanged.wait_for(lock, 500ms, [] { return discoveryCount >= 2; }));
        }
        for (unsigned cycle = 0; cycle < 100; ++cycle) {
            raw.setActive(false);
            raw.setActive(true);
            raw.beginReconnect();
            raw.finishReconnect();
        }
        raw.setActive(false);
    }
    {
        LinuxWacomInput normalized([] {});
        normalized.setActive(true);
        normalized.beginReconnect();
        CHECK(cancels == 1);
        normalized.finishReconnect();
        normalized.setActive(false);
        CHECK(cancels == 2);
    }
    std::cout << "Wacom worker control, focus, normalized cancel and reconnect barriers passed\n";
}
