#pragma once

#include <algorithm>
#include <cstdint>

// Maps SDL 3.4 pen flags onto the existing Moonlight pen wire values without
// taking a header dependency. Values must stay aligned with SDL_pen.h and
// Limelight.h. Used only when a Host advertises normalized pen and not raw HID.
namespace MacNormalizedPenLogic {

constexpr unsigned kInputDown = 1u << 0;
constexpr unsigned kInputButton1 = 1u << 1;
constexpr unsigned kInputButton2 = 1u << 2;
constexpr unsigned kInputButton3 = 1u << 3;
constexpr unsigned kInputEraser = 1u << 30;

constexpr unsigned char kHover = 0;
constexpr unsigned char kDown = 1;
constexpr unsigned char kUp = 2;
constexpr unsigned char kMove = 3;
constexpr unsigned char kCancel = 4;
constexpr unsigned char kButtonOnly = 5;
constexpr unsigned char kHoverLeave = 6;
constexpr unsigned char kCancelAll = 7;

constexpr unsigned char kToolUnknown = 0;
constexpr unsigned char kToolPen = 1;
constexpr unsigned char kToolEraser = 2;

constexpr unsigned char kButtonPrimary = 0x01;
constexpr unsigned char kButtonSecondary = 0x02;
constexpr unsigned char kButtonTertiary = 0x04;

constexpr unsigned short kUnknownRotation = 0xffff;
constexpr unsigned char kUnknownTilt = 0xff;

inline float clamp01(float value)
{
    return std::max(0.0f, std::min(1.0f, value));
}

inline unsigned char buttons(unsigned penState)
{
    unsigned char mask = 0;
    if ((penState & kInputButton1) != 0) {
        mask |= kButtonPrimary;
    }
    if ((penState & kInputButton2) != 0) {
        mask |= kButtonSecondary;
    }
    if ((penState & kInputButton3) != 0) {
        mask |= kButtonTertiary;
    }
    return mask;
}

inline unsigned char buttonMask(unsigned sdlButton)
{
    switch (sdlButton) {
    case 1:
        return kButtonPrimary;
    case 2:
        return kButtonSecondary;
    case 3:
        return kButtonTertiary;
    default:
        return 0;
    }
}

inline unsigned char tool(unsigned penState)
{
    return (penState & kInputEraser) != 0 ? kToolEraser : kToolPen;
}

inline unsigned char motionEvent(bool tipDown)
{
    return tipDown ? kMove : kHover;
}

inline bool isPenMouse(unsigned mouseId)
{
    return mouseId == static_cast<unsigned>(-2);
}

} // namespace MacNormalizedPenLogic
