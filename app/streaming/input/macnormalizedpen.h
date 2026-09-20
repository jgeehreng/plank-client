#pragma once

#include <SDL3/SDL.h>

#include <functional>

class MacNormalizedPen
{
public:
    explicit MacNormalizedPen(std::function<void()> tabletActivity);
    ~MacNormalizedPen();

    MacNormalizedPen(const MacNormalizedPen&) = delete;
    MacNormalizedPen& operator=(const MacNormalizedPen&) = delete;

    void setActive(bool active);
    bool isActive() const { return m_Active; }

    void handleProximity(bool entered);
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
};
