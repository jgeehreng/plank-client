#pragma once

#include <functional>
#include <memory>

// Physical HID ownership and I/O run on one private CFRunLoop. Lifecycle calls
// request release with a deadline; a stalled worker retains its own state.
class MacRawWacomInput
{
public:
    explicit MacRawWacomInput(std::function<void()> tabletActivity);
    ~MacRawWacomInput();
    void setActive(bool active);
    bool isAttached() const;
    void beginReconnect();
    void finishReconnect();
    void handleControl(const unsigned char* data, unsigned int length);
private:
    class Impl;
    std::shared_ptr<Impl> m_Impl;
};
