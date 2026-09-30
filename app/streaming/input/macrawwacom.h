#pragma once

#include <functional>
#include <memory>

// Physical HID ownership and I/O run on one private CFRunLoop. Lifecycle calls
// request release with a deadline; a stalled worker retains its own state.
class MacRawWacomInput
{
public:
    // Interactive launcher only; registry inspection does not open/seize HID.
    static void requestPermissionIfNeeded();
    // Read-only registry enumeration: -1 unavailable, 0 absent, 1 attached.
    static int supportedTabletPresence();
    using PositionHint = std::function<void(int x, int y, int maxX, int maxY)>;
    explicit MacRawWacomInput(std::function<void()> tabletActivity,
                              PositionHint positionHint = {});
    ~MacRawWacomInput();
    void setActive(bool active);
    void beginReconnect();
    void finishReconnect();
    void handleControl(const unsigned char* data, unsigned int length);
private:
    class Impl;
    std::shared_ptr<Impl> m_Impl;
};
