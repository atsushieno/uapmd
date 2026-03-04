#include <cmath>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#import <CoreAudioKit/AUViewController.h>
#else
#import <Cocoa/Cocoa.h>
#endif
#import <AudioToolbox/AUAudioUnit.h>
#include "remidy/remidy.hpp"
#include "PluginFormatAU.hpp"

namespace remidy {

    PluginInstanceAUv3::UISupport::UISupport(PluginInstanceAUv3* owner) : owner(owner) {
    }

    bool PluginInstanceAUv3::UISupport::hasUI() {
        @autoreleasepool {
            if (owner->audioUnit == nil)
                return false;

            // Check if the AU supports view controllers
            // We'll attempt to request one synchronously to check
            return [owner->audioUnit respondsToSelector:@selector(requestViewControllerWithCompletionHandler:)];
        }
    }

    bool PluginInstanceAUv3::UISupport::create(bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler) {
        if (created)
            return false;

        is_floating = isFloating;
        host_resize_handler = resizeHandler;
        bool success = false;

        EventLoop::runTaskOnMainThread([&] {
            @autoreleasepool {
                if (owner->audioUnit == nil)
                    return;

                // Use a semaphore to wait for async completion
                dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

#if TARGET_OS_IPHONE
                // On iOS, the block receives UIViewController*.
                [owner->audioUnit requestViewControllerWithCompletionHandler:^(UIViewController * _Nullable viewController) {
                    @autoreleasepool {
                        if (viewController == nil) {
                            dispatch_semaphore_signal(semaphore);
                            return;
                        }

                        ns_view_controller = (__bridge void*)[viewController retain];
                        UIView* view = viewController.view;

                        if (view == nil) {
                            [viewController release];
                            ns_view_controller = nullptr;
                            dispatch_semaphore_signal(semaphore);
                            return;
                        }

                        ns_view = (__bridge void*)[view retain];

                        // Embed as a child view controller into the container VC.
                        // parentHandle is a UIViewController* from IOSContainerWindow::getHandle().
                        if (parentHandle) {
                            UIViewController* parentVC = (__bridge UIViewController*)parentHandle;
                            [parentVC addChildViewController:viewController];
                            view.frame = parentVC.view.bounds;
                            view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
                            [parentVC.view addSubview:view];
                            [viewController didMoveToParentViewController:parentVC];

                            CGSize preferred = viewController.preferredContentSize;
                            if (host_resize_handler && preferred.width > 0 && preferred.height > 0)
                                host_resize_handler((uint32_t)preferred.width, (uint32_t)preferred.height);

                            attached = true;
                        }

                        created = true;
                        visible = false;
                        success = true;
                        dispatch_semaphore_signal(semaphore);
                    }
                }];
#else
                // On macOS, the block receives NSViewController*.
                // NOTE: some non-trivial replacement during AUv2->AUv3 migration
                [owner->audioUnit requestViewControllerWithCompletionHandler:^(NSViewController * _Nullable viewController) {
                    @autoreleasepool {
                        if (viewController == nil) {
                            dispatch_semaphore_signal(semaphore);
                            return;
                        }

                        ns_view_controller = (__bridge void*)[viewController retain];
                        NSView* view = [viewController view];

                        if (view == nil) {
                            [viewController release];
                            ns_view_controller = nullptr;
                            dispatch_semaphore_signal(semaphore);
                            return;
                        }

                        ns_view = (__bridge void*)[view retain];

                        // If floating, create a window
                        if (is_floating) {
                            NSRect viewFrame = [view frame];
                            NSSize viewSize = viewFrame.size;

                            if (viewSize.width <= 0 || viewSize.height <= 0) {
                                viewSize.width = 400;
                                viewSize.height = 300;
                            }

                            NSRect windowRect = NSMakeRect(100, 100, viewSize.width, viewSize.height);

                            NSWindow* window = [[NSWindow alloc] initWithContentRect:windowRect
                                styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable)
                                backing:NSBackingStoreBuffered
                                defer:NO];

                            [window setContentView:view];
                            ns_window = (__bridge void*)[window retain];
                        } else if (parentHandle) {
                            NSView* parentView = (__bridge NSView*)parentHandle;
                            if (!parentView) {
                                [view release];
                                [viewController release];
                                ns_view = nullptr;
                                ns_view_controller = nullptr;
                                dispatch_semaphore_signal(semaphore);
                                return;
                            }

                            if ([view superview]) {
                                [view removeFromSuperview];
                            }

                            [parentView addSubview:view];

                            NSSize preferredSize = [view frame].size;

                            ignore_view_notifications = true;
                            [view setFrame:[parentView bounds]];
                            ignore_view_notifications = false;

                            [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
                            attached = true;

                            if (host_resize_handler && preferredSize.width > 0 && preferredSize.height > 0) {
                                uint32_t width = (uint32_t)preferredSize.width;
                                uint32_t height = (uint32_t)preferredSize.height;
                                host_resize_handler(width, height);
                            }
                        }

                        created = true;
                        visible = false;
                        success = true;

                        if (ns_view)
                            startViewResizeObservation(ns_view);

                        dispatch_semaphore_signal(semaphore);
                    }
                }];
#endif

                // Wait for completion (with timeout)
                dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC);
                if (dispatch_semaphore_wait(semaphore, timeout) != 0) {
                    owner->logger()->logError("%s: UI creation timed out", owner->name.c_str());
                }

                dispatch_release(semaphore);
            }
        });

        return success;
    }

    void PluginInstanceAUv3::UISupport::destroy() {
        if (!created)
            return;

        EventLoop::runTaskOnMainThread([&] {
            @autoreleasepool {
#if TARGET_OS_IPHONE
                if (ns_view_controller) {
                    AUViewController* vc = (__bridge AUViewController*)ns_view_controller;
                    [vc willMoveToParentViewController:nil];
                    [vc.view removeFromSuperview];
                    [vc removeFromParentViewController];
                    [vc release];
                    ns_view_controller = nullptr;
                    ns_view = nullptr; // view is owned by vc; cleared above
                }
#else
                if (ns_window) {
                    NSWindow* window = (__bridge NSWindow*)ns_window;
                    [window close];
                    [window release];
                    ns_window = nullptr;
                }

                if (ns_view) {
                    stopViewResizeObservation();
                    NSView* view = (__bridge NSView*)ns_view;
                    [view removeFromSuperview];
                    [view release];
                    ns_view = nullptr;
                }

                if (ns_view_controller) {
                    // NOTE: some non-trivial replacement during AUv2->AUv3 migration
                    NSViewController* vc = (__bridge NSViewController*)ns_view_controller;
                    [vc release];
                    ns_view_controller = nullptr;
                }
#endif
                created = false;
                visible = false;
                attached = false;
                is_floating = false;
            }
        });
    }

    bool PluginInstanceAUv3::UISupport::show() {
        if (!created)
            return false;

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            @autoreleasepool {
#if TARGET_OS_IPHONE
                if (ns_view) {
                    UIView* view = (__bridge UIView*)ns_view;
                    view.hidden = NO;
                    success = true;
                }
#else
                if (is_floating && ns_window) {
                    NSWindow* window = (__bridge NSWindow*)ns_window;
                    [window makeKeyAndOrderFront:nil];
                    success = true;
                } else if (ns_view) {
                    NSView* view = (__bridge NSView*)ns_view;
                    [view setHidden:NO];
                    success = true;
                }
#endif
            }
        });

        if (success)
            visible = true;

        return success;
    }

    void PluginInstanceAUv3::UISupport::hide() {
        if (!created)
            return;

        EventLoop::runTaskOnMainThread([&] {
            @autoreleasepool {
#if TARGET_OS_IPHONE
                if (ns_view) {
                    UIView* view = (__bridge UIView*)ns_view;
                    view.hidden = YES;
                }
#else
                if (is_floating && ns_window) {
                    NSWindow* window = (__bridge NSWindow*)ns_window;
                    [window orderOut:nil];
                } else if (ns_view) {
                    NSView* view = (__bridge NSView*)ns_view;
                    [view setHidden:YES];
                }
#endif
            }
        });

        visible = false;
    }

    void PluginInstanceAUv3::UISupport::setWindowTitle(std::string title) {
#if TARGET_OS_IPHONE
        if (!created || !ns_view_controller)
            return;

        EventLoop::runTaskOnMainThread([&] {
            @autoreleasepool {
                AUViewController* vc = (__bridge AUViewController*)ns_view_controller;
                vc.title = [NSString stringWithUTF8String:title.c_str()];
            }
        });
#else
        if (!created || !is_floating || !ns_window)
            return;

        EventLoop::runTaskOnMainThread([&] {
            @autoreleasepool {
                NSWindow* window = (__bridge NSWindow*)ns_window;
                NSString* nsTitle = [NSString stringWithUTF8String:title.c_str()];
                [window setTitle:nsTitle];
            }
        });
#endif
    }

    bool PluginInstanceAUv3::UISupport::canResize() {
        return false; // Most AUv3 UIs don't support arbitrary resizing
    }

    bool PluginInstanceAUv3::UISupport::getSize(uint32_t &width, uint32_t &height) {
        if (!created)
            return false;

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            @autoreleasepool {
#if TARGET_OS_IPHONE
                if (ns_view) {
                    UIView* view = (__bridge UIView*)ns_view;
                    CGSize size = view.frame.size;
                    width = (uint32_t)size.width;
                    height = (uint32_t)size.height;
                    success = true;
                }
#else
                NSSize size;

                if (is_floating && ns_window) {
                    NSWindow* window = (__bridge NSWindow*)ns_window;
                    NSRect contentRect = [window contentRectForFrameRect:[window frame]];
                    size = contentRect.size;
                } else if (ns_view) {
                    NSView* view = (__bridge NSView*)ns_view;
                    size = [view frame].size;
                } else {
                    return;
                }

                width = (uint32_t)size.width;
                height = (uint32_t)size.height;
                success = true;
#endif
            }
        });

        return success;
    }

    bool PluginInstanceAUv3::UISupport::setSize(uint32_t width, uint32_t height) {
        if (!created)
            return false;

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            @autoreleasepool {
#if TARGET_OS_IPHONE
                if (ns_view) {
                    UIView* view = (__bridge UIView*)ns_view;
                    CGRect frame = view.frame;
                    frame.size = CGSizeMake(width, height);
                    ignore_view_notifications = true;
                    view.frame = frame;
                    ignore_view_notifications = false;
                    last_view_width = width;
                    last_view_height = height;
                    last_view_size_valid = true;
                    success = true;
                }
#else
                if (is_floating && ns_window) {
                    NSWindow* window = (__bridge NSWindow*)ns_window;
                    NSSize contentSize = NSMakeSize(width, height);
                    ignore_view_notifications = true;
                    [window setContentSize:contentSize];
                    ignore_view_notifications = false;
                    last_view_width = contentSize.width;
                    last_view_height = contentSize.height;
                    last_view_size_valid = true;
                    success = true;
                } else if (ns_view) {
                    NSView* view = (__bridge NSView*)ns_view;
                    NSRect frame = NSMakeRect(0, 0, width, height);
                    ignore_view_notifications = true;
                    [view setFrame:frame];
                    ignore_view_notifications = false;
                    last_view_width = frame.size.width;
                    last_view_height = frame.size.height;
                    last_view_size_valid = true;
                    success = true;
                }
#endif
            }
        });

        return success;
    }

    bool PluginInstanceAUv3::UISupport::suggestSize(uint32_t &width, uint32_t &height) {
        if (!created)
            return false;

        if (setSize(width, height)) {
            return getSize(width, height);
        }

        return false;
    }

#if TARGET_OS_IPHONE
    // View resize observation is not needed for modal UIViewController presentation on iOS.
    void PluginInstanceAUv3::UISupport::startViewResizeObservation(void*) {}
    void PluginInstanceAUv3::UISupport::stopViewResizeObservation() {}
    void PluginInstanceAUv3::UISupport::handleViewSizeChange() {}
#else
    void PluginInstanceAUv3::UISupport::startViewResizeObservation(void* viewHandle) {
        if (!viewHandle || view_resize_observer)
            return;

        NSView* view = (__bridge NSView*)viewHandle;
        [view setPostsFrameChangedNotifications:YES];

        __block PluginInstanceAUv3::UISupport* blockSelf = this;
        id observer = [[NSNotificationCenter defaultCenter] addObserverForName:NSViewFrameDidChangeNotification
            object:view
            queue:nil
            usingBlock:^(NSNotification* _Nonnull) {
                if (!blockSelf)
                    return;
                blockSelf->handleViewSizeChange();
            }];

        view_resize_observer = (void*)CFBridgingRetain(observer);

        NSSize currentSize = [view frame].size;
        last_view_width = currentSize.width;
        last_view_height = currentSize.height;
        last_view_size_valid = true;
    }

    void PluginInstanceAUv3::UISupport::stopViewResizeObservation() {
        if (!view_resize_observer)
            return;

        id observer = (__bridge id)view_resize_observer;
        [[NSNotificationCenter defaultCenter] removeObserver:observer];
        CFBridgingRelease(view_resize_observer);
        view_resize_observer = nullptr;
        last_view_size_valid = false;
    }

    void PluginInstanceAUv3::UISupport::handleViewSizeChange() {
        if (ignore_view_notifications || !host_resize_handler || !ns_view)
            return;

        NSView* view = (__bridge NSView*)ns_view;
        NSSize size = [view frame].size;
        if (size.width <= 0.0 || size.height <= 0.0)
            return;

        auto nearlyEqual = [](double a, double b) {
            return std::abs(a - b) < 0.5;
        };

        if (last_view_size_valid && nearlyEqual(size.width, last_view_width) && nearlyEqual(size.height, last_view_height))
            return;

        last_view_width = size.width;
        last_view_height = size.height;
        last_view_size_valid = true;

        uint32_t width = static_cast<uint32_t>(std::round(size.width));
        uint32_t height = static_cast<uint32_t>(std::round(size.height));
        host_resize_handler(width, height);
    }
#endif // !TARGET_OS_IPHONE

    bool PluginInstanceAUv3::UISupport::setScale(double scale) {
        (void)scale;
        return false; // AUv3 doesn't have standard scale API
    }
}
