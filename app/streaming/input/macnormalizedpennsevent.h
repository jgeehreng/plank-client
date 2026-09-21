#pragma once

#include <functional>
#include <memory>

class MacNormalizedPen;

// AppKit tablet events carry the Wacom driver's pressure. SDL's macOS pen
// path is ~8-bit. This monitor does not need Input Monitoring.
class MacNormalizedPenNsEvent
{
public:
    using MapPoint = std::function<bool(float windowX, float windowY, float& nx, float& ny)>;

    MacNormalizedPenNsEvent();
    ~MacNormalizedPenNsEvent();

    MacNormalizedPenNsEvent(const MacNormalizedPenNsEvent&) = delete;
    MacNormalizedPenNsEvent& operator=(const MacNormalizedPenNsEvent&) = delete;

    void start(MacNormalizedPen* pen, MapPoint map);
    void stop();
    bool isRunning() const;

private:
    struct State;
    std::unique_ptr<State> m_State;
};
