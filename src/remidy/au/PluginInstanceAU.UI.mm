#include "PluginFormatAU.hpp"
#include <priv/event-loop.hpp>
#import <Cocoa/Cocoa.h>
#import <AudioUnit/AudioUnit.h>

// Protocol for AUv2 Cocoa view factory
@protocol AUCocoaUIBase
- (NSView *)uiViewForAudioUnit:(AudioUnit)inAudioUnit withSize:(NSSize)inPreferredSize;
@end

namespace remidy {

    PluginInstanceAU::UISupport::UISupport(PluginInstanceAU* owner) : owner(owner) {
    }

    bool PluginInstanceAU::UISupport::hasUI() {
        AudioUnit au = owner->instance;
        if (!au)
            return false;

        // Check if the plugin has the CocoaUI property
        UInt32 dataSize = 0;
        Boolean writable = false;
        OSStatus result = AudioUnitGetPropertyInfo(au,
            kAudioUnitProperty_CocoaUI,
            kAudioUnitScope_Global,
            0,
            &dataSize,
            &writable);

        return (result == noErr && dataSize > 0);
    }

    bool PluginInstanceAU::UISupport::create(bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler) {
        if (created)
            return false; // Already created

        is_floating = isFloating;
        host_resize_handler = resizeHandler;
        bool success = false;

        EventLoop::runTaskOnMainThread([&] {
            @autoreleasepool {
                NSView* view = nil;

                // Try AUv3 first (requestViewController)
                if (owner->auVersion() == PluginInstanceAU::AUV3) {
                    // AUv3 uses requestViewControllerWithCompletionHandler (async)
                    // For now, we'll use the sync property approach if available
                    // Note: This is a simplification - proper async handling would be better
                    AudioUnit au = owner->instance;

                    // Try to get the view controller synchronously (some AUv3 support this)
                    UInt32 dataSize = sizeof(void*);
                    void* viewController = nullptr;
                    OSStatus result = AudioUnitGetProperty(au,
                        kAudioUnitProperty_CocoaUI,  // Try legacy property first
                        kAudioUnitScope_Global,
                        0,
                        &viewController,
                        &dataSize);

                    if (result == noErr && viewController) {
                        NSViewController* vc = (__bridge NSViewController*)viewController;
                        ns_view_controller = (__bridge void*)[vc retain];
                        view = [vc view];
                    }
                }

                // Try AUv2 Cocoa UI
                if (!view && owner->auVersion() == PluginInstanceAU::AUV2) {
                    AudioUnit au = owner->instance;
                    UInt32 dataSize = 0;
                    Boolean writable = false;

                    // Get size of property first
                    OSStatus result = AudioUnitGetPropertyInfo(au,
                        kAudioUnitProperty_CocoaUI,
                        kAudioUnitScope_Global,
                        0,
                        &dataSize,
                        &writable);

                    if (result == noErr && dataSize > 0) {
                        AudioUnitCocoaViewInfo* viewInfo = (AudioUnitCocoaViewInfo*)malloc(dataSize);

                        result = AudioUnitGetProperty(au,
                            kAudioUnitProperty_CocoaUI,
                            kAudioUnitScope_Global,
                            0,
                            viewInfo,
                            &dataSize);

                        if (result == noErr && viewInfo->mCocoaAUViewClass[0] != nullptr) {
                            // Load the bundle
                            NSURL* bundleURL = (__bridge NSURL*)viewInfo->mCocoaAUViewBundleLocation;
                            NSBundle* bundle = [NSBundle bundleWithURL:bundleURL];

                            if (bundle) {
                                ns_bundle = (__bridge void*)[bundle retain];

                                // Get the factory class name
                                NSString* className = (__bridge NSString*)viewInfo->mCocoaAUViewClass[0];
                                Class viewClass = [bundle classNamed:className];

                                if (viewClass) {
                                    // Create factory instance and get the view
                                    id<AUCocoaUIBase> factory = [[viewClass alloc] init];
                                    // Pass a large size hint - the plugin will use its preferred size
                                    NSSize sizeHint = NSMakeSize(800, 600);
                                    view = [factory uiViewForAudioUnit:au withSize:sizeHint];
                                }
                            }
                        }

                        // Cleanup: release the bundle location URL and class names
                        if (viewInfo->mCocoaAUViewBundleLocation)
                            CFRelease(viewInfo->mCocoaAUViewBundleLocation);
                        for (int i = 0; i < sizeof(viewInfo->mCocoaAUViewClass) / sizeof(CFStringRef); i++) {
                            if (viewInfo->mCocoaAUViewClass[i])
                                CFRelease(viewInfo->mCocoaAUViewClass[i]);
                        }
                        free(viewInfo);
                    }
                }

                if (view) {
                    ns_view = (__bridge void*)[view retain];

                    // If floating, create a window
                    if (is_floating) {
                        // Get the view's preferred size (as set by the plugin)
                        NSRect viewFrame = [view frame];
                        NSSize viewSize = viewFrame.size;

                        // If view has no size set, use a reasonable default
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
                        // Attach to parent if embedded
                        NSView* parentView = (__bridge NSView*)parentHandle;
                        if (!parentView) {
                            // Failed - no valid parent
                            [view release];
                            ns_view = nullptr;
                            return;
                        }

                        // Remove from current superview if any
                        if ([view superview]) {
                            [view removeFromSuperview];
                        }

                        // Add as subview to the parent
                        [parentView addSubview:view];

                        // Get the plugin view's preferred size before we modify its frame
                        NSSize preferredSize = [view frame].size;

                        // Set the view's frame to fill the parent view
                        // This ensures correct positioning and allows the parent to control sizing
                        [view setFrame:[parentView bounds]];

                        // Set autoresizing to allow the view to resize with the parent
                        [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

                        attached = true;

                        // Notify host of plugin's preferred UI size so it can resize the container
                        if (host_resize_handler && preferredSize.width > 0 && preferredSize.height > 0) {
                            uint32_t width = (uint32_t)preferredSize.width;
                            uint32_t height = (uint32_t)preferredSize.height;
                            host_resize_handler(width, height);
                        }
                    }

                    created = true;
                    visible = false;
                    success = true;
                }
            }
        });

        return success;
    }

    void PluginInstanceAU::UISupport::destroy() {
        if (!created)
            return;

        EventLoop::runTaskOnMainThread([&] {
            @autoreleasepool {
                // Close and release window if floating
                if (ns_window) {
                    NSWindow* window = (__bridge NSWindow*)ns_window;
                    [window close];
                    [window release];
                    ns_window = nullptr;
                }

                // Release view
                if (ns_view) {
                    NSView* view = (__bridge NSView*)ns_view;
                    [view removeFromSuperview];
                    [view release];
                    ns_view = nullptr;
                }

                // Release view controller (AUv3)
                if (ns_view_controller) {
                    NSViewController* vc = (__bridge NSViewController*)ns_view_controller;
                    [vc release];
                    ns_view_controller = nullptr;
                }

                // Release bundle (AUv2)
                if (ns_bundle) {
                    NSBundle* bundle = (__bridge NSBundle*)ns_bundle;
                    [bundle release];
                    ns_bundle = nullptr;
                }

                created = false;
                visible = false;
                attached = false;
                is_floating = false;
            }
        });
    }

    bool PluginInstanceAU::UISupport::show() {
        if (!created)
            return false;

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            @autoreleasepool {
                if (is_floating && ns_window) {
                    NSWindow* window = (__bridge NSWindow*)ns_window;
                    [window makeKeyAndOrderFront:nil];
                    success = true;
                } else if (ns_view) {
                    NSView* view = (__bridge NSView*)ns_view;
                    [view setHidden:NO];
                    success = true;
                }
            }
        });

        if (success)
            visible = true;

        return success;
    }

    void PluginInstanceAU::UISupport::hide() {
        if (!created)
            return;

        EventLoop::runTaskOnMainThread([&] {
            @autoreleasepool {
                if (is_floating && ns_window) {
                    NSWindow* window = (__bridge NSWindow*)ns_window;
                    [window orderOut:nil];
                } else if (ns_view) {
                    NSView* view = (__bridge NSView*)ns_view;
                    [view setHidden:YES];
                }
            }
        });

        visible = false;
    }

    void PluginInstanceAU::UISupport::setWindowTitle(std::string title) {
        if (!created || !is_floating || !ns_window)
            return;

        EventLoop::runTaskOnMainThread([&] {
            @autoreleasepool {
                NSWindow* window = (__bridge NSWindow*)ns_window;
                NSString* nsTitle = [NSString stringWithUTF8String:title.c_str()];
                [window setTitle:nsTitle];
            }
        });
    }

    bool PluginInstanceAU::UISupport::canResize() {
        if (!created)
            return false;

        // Most AU views support resizing through NSView frame manipulation
        // We'll return true by default
        return true;
    }

    bool PluginInstanceAU::UISupport::getSize(uint32_t &width, uint32_t &height) {
        if (!created)
            return false;

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            @autoreleasepool {
                NSSize size;

                if (is_floating && ns_window) {
                    NSWindow* window = (__bridge NSWindow*)ns_window;
                    // Get content size, not frame size (frame includes title bar)
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
            }
        });

        return success;
    }

    bool PluginInstanceAU::UISupport::setSize(uint32_t width, uint32_t height) {
        if (!created)
            return false;

        bool success = false;
        EventLoop::runTaskOnMainThread([&] {
            @autoreleasepool {
                if (is_floating && ns_window) {
                    NSWindow* window = (__bridge NSWindow*)ns_window;
                    // Set content size, not frame size (frame includes title bar)
                    NSSize contentSize = NSMakeSize(width, height);
                    [window setContentSize:contentSize];
                    success = true;
                } else if (ns_view) {
                    NSView* view = (__bridge NSView*)ns_view;
                    // Always position at origin (0,0) relative to parent
                    NSRect frame = NSMakeRect(0, 0, width, height);
                    [view setFrame:frame];
                    success = true;
                }
            }
        });

        return success;
    }

    bool PluginInstanceAU::UISupport::suggestSize(uint32_t &width, uint32_t &height) {
        if (!created)
            return false;

        // Try to set the size, then get the actual size
        // This allows the view to adjust if needed
        if (setSize(width, height)) {
            return getSize(width, height);
        }

        return false;
    }

    bool PluginInstanceAU::UISupport::setScale(double scale) {
        (void)scale;
        // AudioUnit doesn't have a standard scale API
        // Some plugins might respond to backing scale factor changes,
        // but there's no reliable cross-plugin way to set this
        // Return false to indicate this is not supported
        return false;
    }
}
