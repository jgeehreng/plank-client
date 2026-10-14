#pragma once

#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <TargetConditionals.h>
#include <climits>

namespace MacDisplayMode {
struct Snapshot {
    int width = 0;
    int height = 0;
    int safeHeight = 0;
    bool native = false;
};

inline bool dimensions(CGDisplayModeRef mode, Snapshot& result)
{
    if (!mode) return false;
    const auto width = CGDisplayModeGetPixelWidth(mode);
    const auto height = CGDisplayModeGetPixelHeight(mode);
    if (!width || !height || width > INT_MAX || height > INT_MAX) return false;
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    result.safeHeight = result.height;
    return true;
}

// Presentation follows current backing pixels even when a driver advertises a
// different native panel mode. Reading this snapshot never changes display mode.
inline bool currentSnapshot(CGDirectDisplayID display, Snapshot& result)
{
    result = {};
    CGDisplayModeRef current = CGDisplayCopyDisplayMode(display);
    const bool valid = dimensions(current, result);
    if (current) CGDisplayModeRelease(current);
    return valid;
}

// Some drivers expose a valid current mode without marking any listed mode
// native. Use those current backing pixels; never guess from the largest mode
// or change the user's display configuration to obtain presentation geometry.
inline bool snapshot(CGDirectDisplayID display, Snapshot& result)
{
    result = {};
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(display, nullptr);
    const CFIndex count = modes ? CFArrayGetCount(modes) : 0;
    for (CFIndex i = 0; i < count; ++i) {
        const auto mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        if ((CGDisplayModeGetIOFlags(mode) & kDisplayModeNativeFlag) && dimensions(mode, result)) {
            result.native = true;
            break;
        }
    }
    if (!result.native) {
        if (modes) CFRelease(modes);
        return currentSnapshot(display, result);
    }
#if TARGET_CPU_ARM64
    if (CGDisplayIsBuiltin(display)) {
        for (CFIndex i = 0; i < count; ++i) {
            Snapshot candidate;
            const auto mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
            if (dimensions(mode, candidate) && result.width == candidate.width &&
                candidate.height < result.height && result.height - candidate.height <= 100)
                result.safeHeight = candidate.height;
        }
    }
#endif
    CFRelease(modes);
    return true;
}
}
