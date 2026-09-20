#include "macnormalizedpen.h"
#include "macnormalizedpenlogic.h"

#include <Limelight.h>
#include <SDL3/SDL.h>

MacNormalizedPen::MacNormalizedPen(std::function<void()> tabletActivity)
    : m_Tool(MacNormalizedPenLogic::kToolPen),
      m_TabletActivity(std::move(tabletActivity))
{
}

MacNormalizedPen::~MacNormalizedPen()
{
    setActive(false);
}

void MacNormalizedPen::setActive(bool active)
{
    if (m_Active == active) {
        return;
    }
    m_Active = active;
    if (!active) {
        cancel();
    }
}

void MacNormalizedPen::cancel()
{
    if (m_Near || m_TipDown || m_Buttons != 0) {
        LiSendPenEvent(MacNormalizedPenLogic::kCancelAll,
                       MacNormalizedPenLogic::kToolUnknown, 0,
                       0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                       MacNormalizedPenLogic::kUnknownRotation,
                       MacNormalizedPenLogic::kUnknownTilt);
    }
    m_Near = false;
    m_TipDown = false;
    m_Buttons = 0;
    m_Pressure = 0.0f;
    m_Distance = 0.0f;
    m_Tool = MacNormalizedPenLogic::kToolPen;
}

void MacNormalizedPen::send(unsigned char type, float x, float y)
{
    m_X = MacNormalizedPenLogic::clamp01(x);
    m_Y = MacNormalizedPenLogic::clamp01(y);
    if (!m_Active) {
        return;
    }
    const float pressureOrDistance =
            m_TipDown ? m_Pressure : m_Distance;
    LiSendPenEvent(type, m_Tool, m_Buttons, m_X, m_Y,
                   MacNormalizedPenLogic::clamp01(pressureOrDistance),
                   0.0f, 0.0f,
                   MacNormalizedPenLogic::kUnknownRotation,
                   MacNormalizedPenLogic::kUnknownTilt);
    if (m_TabletActivity) {
        m_TabletActivity();
    }
}

void MacNormalizedPen::handleProximity(bool entered)
{
    if (entered) {
        m_Near = true;
        send(MacNormalizedPenLogic::kHover, m_X, m_Y);
        return;
    }
    send(MacNormalizedPenLogic::kHoverLeave, m_X, m_Y);
    m_Near = false;
    m_TipDown = false;
    m_Buttons = 0;
    m_Pressure = 0.0f;
}

void MacNormalizedPen::handleTip(bool down, bool eraser)
{
    handleTip(down, eraser, m_X, m_Y);
}

void MacNormalizedPen::handleTip(bool down, bool eraser, float x, float y)
{
    m_Near = true;
    m_Tool = eraser ? MacNormalizedPenLogic::kToolEraser
                    : MacNormalizedPenLogic::kToolPen;
    m_TipDown = down;
    if (!down) {
        m_Pressure = 0.0f;
    }
    send(down ? MacNormalizedPenLogic::kDown : MacNormalizedPenLogic::kUp, x, y);
}

void MacNormalizedPen::handleMotion(unsigned penState, float x, float y)
{
    m_Near = true;
    m_Tool = MacNormalizedPenLogic::tool(penState);
    m_Buttons = MacNormalizedPenLogic::buttons(penState);
    m_TipDown = (penState & MacNormalizedPenLogic::kInputDown) != 0;
    send(MacNormalizedPenLogic::motionEvent(m_TipDown), x, y);
}

void MacNormalizedPen::handleAxis(unsigned axis, float value, unsigned penState,
                                  float x, float y)
{
    if (axis == SDL_PEN_AXIS_PRESSURE) {
        m_Pressure = MacNormalizedPenLogic::clamp01(value);
    }
    else if (axis == SDL_PEN_AXIS_DISTANCE) {
        m_Distance = MacNormalizedPenLogic::clamp01(value);
    }
    else {
        return;
    }
    handleMotion(penState, x, y);
}

void MacNormalizedPen::handleButton(unsigned sdlButton, bool down)
{
    handleButton(sdlButton, down, m_X, m_Y);
}

void MacNormalizedPen::handleButton(unsigned sdlButton, bool down, float x, float y)
{
    const unsigned char mask = MacNormalizedPenLogic::buttonMask(sdlButton);
    if (mask == 0) {
        return;
    }
    m_Near = true;
    if (down) {
        m_Buttons |= mask;
    }
    else {
        m_Buttons = static_cast<unsigned char>(m_Buttons & ~mask);
    }
    send(MacNormalizedPenLogic::kButtonOnly, x, y);
}
