#include "macwindow.h"
#include "macdisplaygeometry.h"
#include "planktoolbarlogic.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <cmath>

@interface PlankTabletCursorView : NSView
@property(nonatomic) std::uint64_t cursorGeneration;
@end
@implementation PlankTabletCursorView
- (NSView*)hitTest:(NSPoint)point { (void)point; return nil; }
- (BOOL)acceptsFirstResponder { return NO; }
@end

namespace {
PlankTabletCursorView* tabletView(SDL_Window* window, bool create)
{
    if (!window) return nil;
    NSWindow* native = (__bridge NSWindow*)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    for (NSView* view in native.contentView.subviews)
        if ([view isKindOfClass:[PlankTabletCursorView class]])
            return (PlankTabletCursorView*)view;
    if (!create || !native.contentView) return nil;
    PlankTabletCursorView* view = [[PlankTabletCursorView alloc] initWithFrame:NSZeroRect];
    view.wantsLayer = YES;
    view.hidden = YES;
    [native.contentView addSubview:view positioned:NSWindowAbove relativeTo:nil];
    [view release]; // the native content view owns its cursor's lifetime
    return view;
}
void releaseCursorPixels(void*, const void* data, size_t) { free(const_cast<void*>(data)); }
}

void MacWindow::tabletCursor(SDL_Window* window, const unsigned char* pixels, unsigned width,
                            unsigned height, unsigned hotX, unsigned hotY, std::uint64_t generation,
                            int x, int y, bool visible)
{
    @autoreleasepool {
        PlankTabletCursorView* view = tabletView(window, visible);
        if (!view) return;
        if (!visible) { view.hidden = YES; return; }
        if (!pixels || !width || !height || width > 512 || height > 512) return;
        if (view.cursorGeneration != generation || !view.layer.contents) {
            const auto size = std::size_t(width) * height * 4;
            void* copy = malloc(size);
            if (!copy) return;
            memcpy(copy, pixels, size);
            CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, copy, size, releaseCursorPixels);
            if (!provider) { free(copy); return; }
            CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
            CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, space,
                kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst,
                provider, nullptr, false, kCGRenderingIntentDefault);
            view.layer.contents = (__bridge id)image;
            view.layer.contentsGravity = kCAGravityResize;
            view.cursorGeneration = generation;
            if (image) CGImageRelease(image);
            CGColorSpaceRelease(space);
            CGDataProviderRelease(provider);
        }
        const CGFloat top = y - static_cast<int>(hotY);
        const CGFloat nativeY = view.superview.isFlipped ? top : NSHeight(view.superview.bounds) - top - height;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        view.frame = NSMakeRect(x - static_cast<int>(hotX), nativeY, width, height);
        // Metal/toolbar surfaces may have been replaced since the last frame.
        [view.superview addSubview:view positioned:NSWindowAbove relativeTo:nil];
        view.hidden = NO;
        [CATransaction commit];
    }
}

void MacWindow::hideTabletCursor(SDL_Window* window)
{
    @autoreleasepool { tabletView(window, false).hidden = YES; }
}

int MacWindow::activeDisplayCount()
{
    uint32_t count = 0;
    return CGGetActiveDisplayList(0, nullptr, &count) == kCGErrorSuccess ?
                static_cast<int>(count) : 0;
}

bool MacWindow::hasKeyboardFocus(SDL_Window* window)
{
    NSWindow* nativeWindow = (__bridge NSWindow*)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    return nativeWindow && NSApp.isActive && NSApp.keyWindow == nativeWindow &&
           nativeWindow.isOnActiveSpace;
}

bool MacWindow::fullscreenTopInset(Uint32 displayId, int* top)
{
    @autoreleasepool {
        for (NSScreen* screen in NSScreen.screens) {
            if ([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == displayId) {
                // visibleFrame also excludes the Dock/menu bar: that is NOT
                // the native fullscreen viewport. Only reserve the camera area.
                *top = MacDisplayGeometry::nativeFullscreenTopInset(
                    static_cast<int>(std::ceil(screen.safeAreaInsets.top)),
                    static_cast<int>(NSProcessInfo.processInfo.operatingSystemVersion.majorVersion));
                return true;
            }
        }
        return false;
    }
}

void MacWindow::logGeometry(SDL_Window* window)
{
    @autoreleasepool {
        NSWindow* nativeWindow = (__bridge NSWindow*)SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        if (!nativeWindow || !nativeWindow.screen)
            return;
        const NSRect panel = nativeWindow.screen.frame;
        const NSRect frame = nativeWindow.frame;
        const NSRect content = nativeWindow.contentView.bounds;
        int pixelWidth = 0, pixelHeight = 0;
        SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK Mac presentation: native-fullscreen=%d panel=%.0fx%.0f frame=%.0fx%.0f content=%.0fx%.0f drawable=%dx%d scale=%.2f",
                    (nativeWindow.styleMask & NSWindowStyleMaskFullScreen) != 0,
                    (double)panel.size.width, (double)panel.size.height,
                    (double)frame.size.width, (double)frame.size.height,
                    (double)content.size.width, (double)content.size.height,
                    pixelWidth, pixelHeight, (double)nativeWindow.backingScaleFactor);
    }
}

int MacWindow::unobscuredToolbarLeft(SDL_Window* window, int currentLeft, int toolbarWidth)
{
    @autoreleasepool {
        if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN))
            return currentLeft;
        NSWindow* nativeWindow = (__bridge NSWindow*)SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        NSScreen* screen = nativeWindow.screen;
        if (!screen || screen.safeAreaInsets.top <= 0)
            return currentLeft;

        // AppKit exposes the actual unobscured areas: do not assume a specific
        // laptop model, notch width, desktop scale or global screen origin.
        const NSRect left = screen.auxiliaryTopLeftArea;
        const NSRect right = screen.auxiliaryTopRightArea;
        const CGFloat origin = nativeWindow.frame.origin.x;
        int width = 0;
        if (!SDL_GetWindowSize(window, &width, nullptr))
            return currentLeft;
        return PlankToolbarLogic::unobscuredToolbarLeft(
            currentLeft, toolbarWidth, width,
            static_cast<int>(std::floor(NSMaxX(left) - origin)),
            static_cast<int>(std::ceil(NSMinX(right) - origin)));
    }
}

void MacWindow::openInputMonitoringSettings()
{
    void (^open)(void) = ^{
        NSURL* url = [NSURL URLWithString:
            @"x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent"];
        if (url != nil) {
            [[NSWorkspace sharedWorkspace] openURL:url];
        }
    };
    if ([NSThread isMainThread]) {
        open();
        return;
    }
    dispatch_async(dispatch_get_main_queue(), open);
}
