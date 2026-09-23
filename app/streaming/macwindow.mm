#include "macwindow.h"
#include "macdisplaygeometry.h"
#include "planktoolbarlogic.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <algorithm>
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

@interface PlankReconnectStatusView : NSView {
    NSString *_status;
    BOOL _warning;
}
- (void)setStatus:(NSString *)status warning:(BOOL)warning;
@end

@implementation PlankReconnectStatusView
- (id)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self != nil) {
        _status = [@"" retain];
        self.wantsLayer = YES;
        self.layer.contentsScale = self.window.backingScaleFactor > 0 ? self.window.backingScaleFactor : 2;
    }
    return self;
}
- (void)dealloc
{
    [_status release];
    [super dealloc];
}
- (void)setStatus:(NSString *)status warning:(BOOL)warning
{
    NSString *next = status != nil ? status : @"";
    const BOOL changed = _warning != warning || ![_status isEqualToString:next];
    if (changed) {
        [_status release];
        _status = [next copy];
        _warning = warning;
        self.needsDisplay = YES;
    }
}
- (NSView *)hitTest:(NSPoint)point { (void)point; return nil; }
- (BOOL)acceptsFirstResponder { return NO; }
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSRect bounds = self.bounds;
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(bounds, 0.5, 0.5) xRadius:8 yRadius:8];
    [[NSColor colorWithCalibratedRed:22.0 / 255.0 green:27.0 / 255.0 blue:34.0 / 255.0 alpha:1] setFill];
    [path fill];
    [[NSColor colorWithCalibratedRed:106.0 / 255.0 green:117.0 / 255.0 blue:132.0 / 255.0 alpha:1] setStroke];
    path.lineWidth = 1;
    [path stroke];
    NSMutableParagraphStyle *style = [[[NSMutableParagraphStyle alloc] init] autorelease];
    style.alignment = NSTextAlignmentCenter;
    style.lineBreakMode = NSLineBreakByWordWrapping;
    NSColor *color = _warning
        ? [NSColor colorWithCalibratedRed:239.0 / 255.0 green:88.0 / 255.0 blue:88.0 / 255.0 alpha:1]
        : [NSColor colorWithCalibratedRed:224.0 / 255.0 green:224.0 / 255.0 blue:224.0 / 255.0 alpha:1];
    NSFont *font = [NSFont systemFontOfSize:18 weight:NSFontWeightSemibold];
    NSDictionary *attributes = @{
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: color,
        NSParagraphStyleAttributeName: style,
    };
    NSRect text = NSInsetRect(bounds, 22, 18);
    NSRect measured = [_status boundingRectWithSize:text.size
        options:NSStringDrawingUsesLineFragmentOrigin attributes:attributes];
    if (NSHeight(measured) < NSHeight(text)) {
        text.origin.y += (NSHeight(text) - NSHeight(measured)) / 2.0;
        text.size.height = NSHeight(measured);
    }
    [_status drawInRect:text withAttributes:attributes];
}
- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    if (self.window.backingScaleFactor > 0) {
        self.layer.contentsScale = self.window.backingScaleFactor;
    }
}
@end

namespace {
void placeReconnectStatus(PlankReconnectStatusView *view)
{
    NSView *parent = view.superview;
    if (parent == nil) return;
    const CGFloat width = std::min<CGFloat>(460, std::max<CGFloat>(1, NSWidth(parent.bounds)));
    const CGFloat height = 156;
    const CGFloat x = std::max<CGFloat>(0, (NSWidth(parent.bounds) - width) / 2);
    const CGFloat y = std::max<CGFloat>(0, (NSHeight(parent.bounds) - height) / 2);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    view.frame = NSMakeRect(x, y, width, height);
    view.autoresizingMask = NSViewMinXMargin | NSViewMaxXMargin | NSViewMinYMargin | NSViewMaxYMargin;
    [parent addSubview:view positioned:NSWindowAbove relativeTo:nil];
    [CATransaction commit];
}

PlankReconnectStatusView *reconnectStatusView(SDL_Window *window, bool create)
{
    if (!window) return nil;
    NSWindow *native = (__bridge NSWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (native.contentView == nil) return nil;
    for (NSView *view in native.contentView.subviews) {
        if ([view isKindOfClass:[PlankReconnectStatusView class]]) {
            return (PlankReconnectStatusView *)view;
        }
    }
    if (!create) return nil;
    PlankReconnectStatusView *view = [[PlankReconnectStatusView alloc] initWithFrame:NSZeroRect];
    [native.contentView addSubview:view positioned:NSWindowAbove relativeTo:nil];
    [view release];
    return view;
}

void updateReconnectStatus(SDL_Window *window, const char *text, bool warning, bool visible)
{
    void (^update)(void) = ^{
        @autoreleasepool {
            PlankReconnectStatusView *view = reconnectStatusView(window, visible);
            if (view == nil) return;
            if (!visible) {
                view.hidden = YES;
                return;
            }
            [view setStatus:[NSString stringWithUTF8String:text != nullptr ? text : ""] warning:warning];
            placeReconnectStatus(view);
            view.hidden = NO;
        }
    };
    if ([NSThread isMainThread]) update();
    else dispatch_sync(dispatch_get_main_queue(), update);
}
}

void MacWindow::showReconnectStatus(SDL_Window *window, const char *text, bool warning)
{
    updateReconnectStatus(window, text, warning, true);
}

void MacWindow::hideReconnectStatus(SDL_Window *window)
{
    updateReconnectStatus(window, "", false, false);
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
