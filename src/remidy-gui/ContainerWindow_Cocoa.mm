#if defined(__APPLE__)
#include <remidy-gui/ContainerWindow.hpp>
#import <Cocoa/Cocoa.h>

// Window delegate to handle close events
@interface ContainerWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, copy) void (^closeCallback)(void);
@end

@implementation ContainerWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    // Don't actually close the window - just hide it and notify via callback
    if (self.closeCallback) {
        self.closeCallback();
    }
    [sender orderOut:nil];
    return NO; // Prevent actual window close
}
@end

namespace remidy::gui {

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

            // Create and set the delegate
            delegate_ = [[ContainerWindowDelegate alloc] init];
            [window_ setDelegate:delegate_];
        }
    }
    ~CocoaContainerWindow() override {
        @autoreleasepool {
            if (window_) {
                [window_ setDelegate:nil];
                [window_ close];
            }
        }
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
            // Set content size, not frame size (frame includes title bar)
            // First set the content size
            NSSize contentSize = NSMakeSize(b.width, b.height);
            [window_ setContentSize:contentSize];

            // Then set the position (top-left of frame)
            NSRect frame = [window_ frame];
            frame.origin.x = b.x;
            frame.origin.y = b.y;
            [window_ setFrameOrigin:frame.origin];
        }
    }
    Bounds getBounds() const override { return b_; }
    void* getHandle() const override {
        // CLAP expects NSView* for cocoa
        return (__bridge void*)[window_ contentView];
    }
    void setCloseCallback(std::function<void()> callback) override {
        @autoreleasepool {
            if (!delegate_) return;
            if (callback) {
                // Capture the callback in a block
                delegate_.closeCallback = ^{
                    callback();
                };
            } else {
                delegate_.closeCallback = nil;
            }
        }
    }
private:
    NSWindow* window_{nil};
    ContainerWindowDelegate* delegate_{nil};
    Bounds b_{};
};

std::unique_ptr<ContainerWindow> ContainerWindow::create(const char* title, int width, int height) {
    return std::make_unique<CocoaContainerWindow>(title, width, height);
}

} // namespace remidy::gui

#endif

