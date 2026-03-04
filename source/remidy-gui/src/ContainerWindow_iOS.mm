#if TARGET_OS_IPHONE
#include <remidy-gui/remidy-gui.hpp>
#import <UIKit/UIKit.h>

// ─── Close-button action handler ─────────────────────────────────────────────

// A lightweight ObjC helper that forwards UIBarButtonItem taps to a C++ block.
@interface IOSContainerCloseHandler : NSObject
@property (copy) void (^action)(void);
- (void)handleClose;
@end

@implementation IOSContainerCloseHandler
- (void)handleClose {
    if (self.action) self.action();
}
@end

// ─── Utility: topmost presented view controller ───────────────────────────────

static UIViewController* IOSTopViewController()
{
    UIWindowScene* activeScene = nil;
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if ([scene isKindOfClass:[UIWindowScene class]] &&
            scene.activationState == UISceneActivationStateForegroundActive) {
            activeScene = (UIWindowScene*)scene;
            break;
        }
    }
    if (!activeScene) return nil;

    UIViewController* vc = activeScene.windows.firstObject.rootViewController;
    while (vc.presentedViewController)
        vc = vc.presentedViewController;
    return vc;
}

// ─── IOSContainerWindow ──────────────────────────────────────────────────────

namespace remidy::gui {

class IOSContainerWindow : public ContainerWindow {
public:
    IOSContainerWindow(const char* title, int w, int h, std::function<void()> closeCallback)
        : closeCallback_(std::move(closeCallback))
    {
        b_ = {0, 0, w, h};
        @autoreleasepool {
            // Bare container VC — the plugin VC will be added as a child.
            UIViewController* vc = [[UIViewController alloc] init];
            vc.view.backgroundColor = [UIColor systemBackgroundColor];
            if (title)
                vc.title = [NSString stringWithUTF8String:title];
            containerVC_ = (void*)CFBridgingRetain(vc);
        }
    }

    ~IOSContainerWindow() override {
        @autoreleasepool {
            // Dismiss if currently presented.
            UIViewController* vc = (__bridge UIViewController*)containerVC_;
            UINavigationController* nav = vc.navigationController;
            UIViewController* presenter = nav ? nav.presentingViewController
                                               : vc.presentingViewController;
            if (presenter)
                [presenter dismissViewControllerAnimated:NO completion:nil];

            if (closeHandler_) { CFRelease(closeHandler_); closeHandler_ = nullptr; }
            if (containerVC_) { CFRelease(containerVC_); containerVC_ = nullptr; }
        }
    }

    void show(bool visible) override {
        @autoreleasepool {
            if (!containerVC_) return;
            UIViewController* vc = (__bridge UIViewController*)containerVC_;

            if (visible) {
                // Already on screen — nothing to do.
                if (vc.navigationController || vc.presentingViewController)
                    return;

                UIViewController* top = IOSTopViewController();
                if (!top) return;

                // Wrap in a navigation controller to host the close button.
                UINavigationController* nav =
                    [[UINavigationController alloc] initWithRootViewController:vc];

                // Close button: taps call closeCallback_ and dismiss.
                IOSContainerCloseHandler* handler = [[IOSContainerCloseHandler alloc] init];
                __weak UINavigationController* weakNav = nav;
                handler.action = ^{
                    if (closeCallback_) closeCallback_();
                    [weakNav.presentingViewController
                        dismissViewControllerAnimated:YES completion:nil];
                };
                if (closeHandler_) CFRelease(closeHandler_);
                closeHandler_ = (void*)CFBridgingRetain(handler);

                UIBarButtonItem* closeBtn = [[UIBarButtonItem alloc]
                    initWithBarButtonSystemItem:UIBarButtonSystemItemClose
                    target:handler
                    action:@selector(handleClose)];
                vc.navigationItem.leftBarButtonItem = closeBtn;

                [top presentViewController:nav animated:YES completion:nil];
            } else {
                UINavigationController* nav = vc.navigationController;
                UIViewController* presenter = nav ? nav.presentingViewController
                                                  : vc.presentingViewController;
                if (presenter)
                    [presenter dismissViewControllerAnimated:YES completion:nil];
            }
        }
    }

    void resize(int width, int height) override {
        // Modal sheets on iOS do not support programmatic resize.
        b_.width = width;
        b_.height = height;
    }

    Bounds getBounds() const override { return b_; }

    // Returns UIViewController* so plugin UI code can add its VC as a child.
    // (IDocumentProvider and AUv3 UISupport both receive this as void*.)
    void* getHandle() const override { return containerVC_; }

    void setResizeCallback(std::function<void(int, int)> callback) override {
        // iOS modal sheets are not user-resizable; store but never fire.
        (void)callback;
    }

    void setResizable(bool resizable) override {
        (void)resizable;
    }

private:
    void* containerVC_{nullptr};   // UIViewController*, retained via CFBridgingRetain
    void* closeHandler_{nullptr};  // IOSContainerCloseHandler*, retained
    Bounds b_{};
    std::function<void()> closeCallback_;
};

// ─── Factory ─────────────────────────────────────────────────────────────────

std::unique_ptr<ContainerWindow> ContainerWindow::create(
    const char* title, int width, int height, std::function<void()> closeCallback)
{
    return std::make_unique<IOSContainerWindow>(title, width, height, std::move(closeCallback));
}

} // namespace remidy::gui

#endif // TARGET_OS_IPHONE
