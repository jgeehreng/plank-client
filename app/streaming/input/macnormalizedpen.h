#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <memory>

class MacNormalizedPenNsEvent;

class MacNormalizedPen
{
public:
    using MapPoint = std::function<bool(float windowX, float windowY, float& nx, float& ny)>;

    explicit MacNormalizedPen(std::function<void()> tabletActivity);
    ~MacNormalizedPen();

    MacNormalizedPen(const MacNormalizedPen&) = delete;
    MacNormalizedPen& operator=(const MacNormalizedPen&) = delete;

    void setActive(bool active);
    bool isActive() const { return m_Active; }
    bool isNear() const { return m_Near; }
    bool nativeEventsActive() const;
    void setNativeMapper(MapPoint map);

    void handleProximity(bool entered);
    void handleProximity(bool entered, bool eraser);
    void handleNativePoint(bool eraser, bool tipDown, float pressure, float x, float y);
    void handleTip(bool down, bool eraser);
    void handleTip(bool down, bool eraser, float x, float y);
    void handleMotion(unsigned penState, float x, float y);
    void handleAxis(unsigned axis, float value, unsigned penState, float x, float y);
    void handleButton(unsigned sdlButton, bool down);
    void handleButton(unsigned sdlButton, bool down, float x, float y);

private:
    void cancel();
    void send(unsigned char type, float x, float y);

    bool m_Active = false;
    bool m_Near = false;
    bool m_TipDown = false;
    unsigned char m_Tool = 0;
    unsigned char m_Buttons = 0;
    float m_X = 0.0f;
    float m_Y = 0.0f;
    float m_Pressure = 0.0f;
    float m_Distance = 0.0f;
    std::function<void()> m_TabletActivity;
    MapPoint m_Map;
    std::unique_ptr<MacNormalizedPenNsEvent> m_NativeEvents;
};
