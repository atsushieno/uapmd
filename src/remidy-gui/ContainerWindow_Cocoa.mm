#if defined(__APPLE__)
#include <remidy-gui/remidy-gui.hpp>
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
    CocoaContainerWindow(const char* title, int w, int h, std::function<void()> closeCallback) : closeCallback_(std::move(closeCallback)) {
        @autoreleasepool {
            NSRect rect = NSMakeRect(100, 100, w, h);
            NSWindow* window = [[NSWindow alloc] initWithContentRect:rect
                                                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable)
                                                    backing:NSBackingStoreBuffered defer:NO];
            NSString* t = title ? [NSString stringWithUTF8String:title] : @"Plugin UI";
            [window setTitle:t];
            b_.width = w; b_.height = h;

            // Create and set the delegate
            ContainerWindowDelegate* delegate = [[ContainerWindowDelegate alloc] init];
            [window setDelegate:delegate];

            // Set close callback
            if (closeCallback_) {
                delegate.closeCallback = ^{
                    closeCallback_();
                };
            }

            // Store as retained void* to avoid ARC management of C++ members
            window_ = (void*)CFBridgingRetain(window);
            delegate_ = (void*)CFBridgingRetain(delegate);
        }
    }
    ~CocoaContainerWindow() override {
        @autoreleasepool {
            // Bridge back from void* to Objective-C objects
            NSWindow* window = (__bridge_transfer NSWindow*)window_;
            ContainerWindowDelegate* delegate = (__bridge_transfer ContainerWindowDelegate*)delegate_;

            // Critical: Clear the delegate's callback FIRST before any operations
            // This prevents calling into destroyed C++ objects
            if (delegate) {
                delegate.closeCallback = nil;
            }

            // Detach delegate from window
            if (window) {
                [window setDelegate:nil];
                // Note: Do NOT call [window close] here - it may trigger callbacks
                // Just let ARC deallocate the window naturally when it goes out of scope
            }

            // window and delegate will be released by ARC when exiting this scope
            // because we used __bridge_transfer
            window_ = nullptr;
            delegate_ = nullptr;
        }
    }
    void show(bool visible) override {
        @autoreleasepool {
            if (!window_) return;
            NSWindow* window = (__bridge NSWindow*)window_;
            if (visible) [window makeKeyAndOrderFront:nil]; else [window orderOut:nil];
        }
    }
    void resize(int width, int height) override {
        @autoreleasepool {
            if (!window_) return;
            NSWindow* window = (__bridge NSWindow*)window_;
            b_.width = width;
            b_.height = height;
            NSSize contentSize = NSMakeSize(width, height);
            [window setContentSize:contentSize];

            // Update all subviews to fill the content view
            // This ensures plugin views are properly positioned and sized
            NSView* contentView = [window contentView];
            for (NSView* subview in [contentView subviews]) {
                [subview setFrame:[contentView bounds]];
            }
        }
    }
    Bounds getBounds() const override { return b_; }
    void* getHandle() const override {
        // CLAP expects NSView* for cocoa
        NSWindow* window = (__bridge NSWindow*)window_;
        return (__bridge void*)[window contentView];
    }
private:
    void setResizable(bool resizable) {
        @autoreleasepool {
            if (!window_) return;
            NSWindow* window = (__bridge NSWindow*)window_;
            NSWindowStyleMask currentStyle = [window styleMask];
            if (resizable) {
                [window setStyleMask:currentStyle | NSWindowStyleMaskResizable];
            } else {
                [window setStyleMask:currentStyle & ~NSWindowStyleMaskResizable];
            }
        }
    }

    // Use void* instead of direct ObjC pointers to avoid ARC managing them as C++ members
    // We manually manage the lifetime with CFBridgingRetain/Release
    void* window_{nullptr};
    void* delegate_{nullptr};
    Bounds b_{};
    std::function<void()> closeCallback_;
};

std::unique_ptr<ContainerWindow> ContainerWindow::create(const char* title, int width, int height, std::function<void()> closeCallback) {
    return std::make_unique<CocoaContainerWindow>(title, width, height, std::move(closeCallback));
}

} // namespace remidy::gui

#endif

