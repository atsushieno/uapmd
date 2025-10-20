#if defined(__APPLE__)
#include "ContainerWindow.hpp"
#import <Cocoa/Cocoa.h>

namespace uapmd::gui {

class CocoaContainerWindow : public ContainerWindow {
public:
    CocoaContainerWindow(const char* title, int w, int h) {
        @autoreleasepool {
            NSRect rect = NSMakeRect(100, 100, w, h);
            window_ = [[NSWindow alloc] initWithContentRect:rect
                                                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable)
                                                    backing:NSBackingStoreBuffered defer:NO];
            NSString* t = title ? [NSString stringWithUTF8String:title] : @"Plugin UI";
            [window_ setTitle:t];
            b_.width = w; b_.height = h;
        }
    }
    ~CocoaContainerWindow() override {
        @autoreleasepool { if (window_) [window_ close]; }
    }
    void show(bool visible) override {
        @autoreleasepool {
            if (!window_) return;
            if (visible) [window_ makeKeyAndOrderFront:nil]; else [window_ orderOut:nil];
        }
    }
    void setResizable(bool resizable) override {
        @autoreleasepool {
            if (!window_) return;
            NSWindowStyleMask currentStyle = [window_ styleMask];
            if (resizable) {
                [window_ setStyleMask:currentStyle | NSWindowStyleMaskResizable];
            } else {
                [window_ setStyleMask:currentStyle & ~NSWindowStyleMaskResizable];
            }
        }
    }
    void setBounds(const Bounds& b) override {
        @autoreleasepool {
            if (!window_) return; b_ = b;
            NSRect frame = [window_ frame];
            frame.origin.x = b.x; frame.origin.y = b.y; frame.size.width = b.width; frame.size.height = b.height;
            [window_ setFrame:frame display:YES];
        }
    }
    Bounds getBounds() const override { return b_; }
    void* getHandle() const override {
        // CLAP expects NSView* for cocoa
        return (__bridge void*)[window_ contentView];
    }
private:
    NSWindow* window_{nil}; Bounds b_{};
};

std::unique_ptr<ContainerWindow> ContainerWindow::create(const char* title, int width, int height) {
    return std::make_unique<CocoaContainerWindow>(title, width, height);
}

} // namespace uapmd::gui

#endif

