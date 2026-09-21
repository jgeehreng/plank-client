#include "macnormalizedpennsevent.h"
#include "macnormalizedpen.h"

#import <Cocoa/Cocoa.h>
#include <SDL3/SDL.h>

namespace {

bool isTabletEvent(NSEvent* event)
{
    switch (event.type) {
    case NSEventTypeTabletPoint:
    case NSEventTypeTabletProximity:
        return true;
    case NSEventTypeMouseMoved:
    case NSEventTypeLeftMouseDown:
    case NSEventTypeLeftMouseUp:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDown:
    case NSEventTypeRightMouseUp:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDown:
    case NSEventTypeOtherMouseUp:
    case NSEventTypeOtherMouseDragged:
        return event.subtype == NSEventSubtypeTabletPoint ||
                event.subtype == NSEventSubtypeTabletProximity;
    default:
        return false;
    }
}

bool isProximityEvent(NSEvent* event)
{
    return event.type == NSEventTypeTabletProximity ||
            event.subtype == NSEventSubtypeTabletProximity;
}

bool isEraser(NSEvent* event)
{
    return event.pointingDeviceType == NSPointingDeviceTypeEraser;
}

} // namespace

struct MacNormalizedPenNsEvent::State {
    id monitor = nil;
    MacNormalizedPen* pen = nullptr;
    MapPoint map;
};

MacNormalizedPenNsEvent::MacNormalizedPenNsEvent()
    : m_State(std::make_unique<State>())
{
}

MacNormalizedPenNsEvent::~MacNormalizedPenNsEvent()
{
    stop();
}

bool MacNormalizedPenNsEvent::isRunning() const
{
    return m_State && m_State->monitor != nil;
}

void MacNormalizedPenNsEvent::stop()
{
    if (!m_State || m_State->monitor == nil) {
        return;
    }
    [NSEvent removeMonitor:m_State->monitor];
    m_State->monitor = nil;
    m_State->pen = nullptr;
    m_State->map = nullptr;
}

void MacNormalizedPenNsEvent::start(MacNormalizedPen* pen, MapPoint map)
{
    stop();
    if (pen == nullptr || !map) {
        return;
    }
    m_State->pen = pen;
    m_State->map = std::move(map);
    const NSEventMask mask =
            NSEventMaskTabletPoint | NSEventMaskTabletProximity |
            NSEventMaskMouseMoved | NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp |
            NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDown | NSEventMaskRightMouseUp |
            NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDown |
            NSEventMaskOtherMouseUp | NSEventMaskOtherMouseDragged;
    m_State->monitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                             handler:^NSEvent*(NSEvent* event) {
        MacNormalizedPen* active = m_State->pen;
        if (active == nullptr || !isTabletEvent(event)) {
            return event;
        }
        if (isProximityEvent(event)) {
            if (event.isEnteringProximity == NO) {
                active->handleProximity(false, isEraser(event));
                return event;
            }
            float nx = 0.0f;
            float ny = 0.0f;
            if (m_State->map((float)event.locationInWindow.x,
                             (float)event.locationInWindow.y, nx, ny)) {
                active->handleNativePoint(isEraser(event), false, 0.0f, nx, ny);
            }
            else if (active->isNear()) {
                active->handleProximity(true, isEraser(event));
            }
            return event;
        }
        float nx = 0.0f;
        float ny = 0.0f;
        if (!m_State->map((float)event.locationInWindow.x,
                          (float)event.locationInWindow.y, nx, ny)) {
            return event;
        }
        const bool tipDown =
                event.pressure > 0.0001f ||
                event.type == NSEventTypeLeftMouseDown ||
                event.type == NSEventTypeLeftMouseDragged;
        active->handleNativePoint(isEraser(event), tipDown, (float)event.pressure, nx, ny);
        return event;
    }];
    if (m_State->monitor != nil) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Mac normalized pen uses AppKit tablet events");
    }
}
