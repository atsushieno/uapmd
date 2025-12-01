# EventLoop Integration Guide for Linux

This documentation was entirely written by Claude Code.

## Overview

The `remidy::EventLoop` provides a framework-independent way to dispatch tasks on the GUI thread. On Linux, the default implementation now supports both X11 and Wayland, automatically detecting the display server at runtime.

**Key Features:**
1. **EventLoopLinux** (default) - Unified event loop supporting both X11 and Wayland
   - Auto-detects display server via `$WAYLAND_DISPLAY` environment variable
   - Falls back to X11 if Wayland initialization fails
   - No GTK dependency required
2. **EventLoopLinuxFD** - Bare file descriptor implementation for manual integration (legacy)
3. **EventLoopChoc** - GTK-based implementation via choc (fallback option)

## Default Implementations

- **Linux/Unix**: `EventLoopLinux` - Unified X11 + Wayland support with auto-detection
- **macOS/Windows**: `EventLoopChoc` - Uses choc message loop
- **All platforms**: Alternative implementations available via `setEventLoop()`

## Using with Auto-Detection (Recommended)

The EventLoop automatically detects whether you're running on Wayland or X11:

```cpp
#include "remidy/remidy.hpp"
#include "remidy/priv/event-loop-linux.hpp"

// Initialize - automatically detects Wayland or X11
remidy::EventLoop::initializeOnUIThread();

// Query which display server was detected
auto* linuxLoop = dynamic_cast<remidy::EventLoopLinux*>(remidy::getEventLoop());
if (linuxLoop) {
    auto type = linuxLoop->getDisplayServerType();
    if (type == remidy::DisplayServerType::Wayland) {
        std::cout << "Running on Wayland\n";
    } else if (type == remidy::DisplayServerType::X11) {
        std::cout << "Running on X11\n";
    }
}

// Run the event loop
remidy::EventLoop::start();
```

## Using with External Event Loops (Linux)

### Getting Access to the File Descriptor

```cpp
#include "remidy/remidy.hpp"
#include "remidy/priv/event-loop-linux.hpp"

// Get the Linux implementation
auto* linuxLoop = dynamic_cast<remidy::EventLoopLinux*>(remidy::getEventLoop());
if (linuxLoop) {
    int fd = linuxLoop->getEventFileDescriptor();
    // Monitor this FD in your event loop
}
```

### Integration with Qt

```cpp
#include <QSocketNotifier>
#include "remidy/remidy.hpp"
#include "remidy/priv/event-loop-linux.hpp"

// Initialize remidy event loop on the Qt GUI thread
remidy::EventLoop::initializeOnUIThread();

auto* linuxLoop = dynamic_cast<remidy::EventLoopLinuxFD*>(remidy::getEventLoop());
if (linuxLoop) {
    int fd = linuxLoop->getEventFileDescriptor();

    QSocketNotifier* notifier = new QSocketNotifier(
        fd,
        QSocketNotifier::Read,
        qApp
    );

    QObject::connect(notifier, &QSocketNotifier::activated, [linuxLoop]() {
        linuxLoop->dispatchPendingEvents();
    });
}

// Now any remidy::EventLoop::enqueueTaskOnMainThread() calls
// will be dispatched through Qt's event loop
```

### Integration with Custom poll() Loop

```cpp
#include <poll.h>
#include "remidy/remidy.hpp"
#include "remidy/priv/event-loop-linux.hpp"

remidy::EventLoop::initializeOnUIThread();

auto* linuxLoop = dynamic_cast<remidy::EventLoopLinuxFD*>(remidy::getEventLoop());
int remidyFd = linuxLoop ? linuxLoop->getEventFileDescriptor() : -1;

// Your custom event loop
pollfd fds[2];
fds[0].fd = yourAppFd;
fds[0].events = POLLIN;
fds[1].fd = remidyFd;
fds[1].events = POLLIN;

while (running) {
    int ret = poll(fds, 2, -1);

    if (fds[0].revents & POLLIN) {
        // Handle your app events
        handleAppEvents();
    }

    if (fds[1].revents & POLLIN) {
        // Dispatch remidy events
        if (linuxLoop) {
            linuxLoop->dispatchPendingEvents();
        }
    }
}
```

### Integration with GLib/GIO

```cpp
#include <glib.h>
#include "remidy/remidy.hpp"
#include "remidy/priv/event-loop-linux.hpp"

gboolean remidy_event_callback(GIOChannel* source, GIOCondition condition, gpointer data) {
    auto* linuxLoop = static_cast<remidy::EventLoopLinuxFD*>(data);
    linuxLoop->dispatchPendingEvents();
    return TRUE;
}

// Setup
remidy::EventLoop::initializeOnUIThread();

auto* linuxLoop = dynamic_cast<remidy::EventLoopLinuxFD*>(remidy::getEventLoop());
if (linuxLoop) {
    int fd = linuxLoop->getEventFileDescriptor();

    GIOChannel* channel = g_io_channel_unix_new(fd);
    g_io_add_watch(channel, G_IO_IN, remidy_event_callback, linuxLoop);
    g_io_channel_unref(channel);
}

g_main_loop_run(mainLoop);
```

## Using the Built-in X11 Event Loop (Default on Linux)

The default implementation on Linux is X11-based and works out of the box:

```cpp
#include "remidy/remidy.hpp"

int main() {
    // Initialize on main thread (opens X11 display automatically)
    remidy::EventLoop::initializeOnUIThread();

    // Post some tasks from any thread
    remidy::EventLoop::enqueueTaskOnMainThread([]() {
        std::cout << "Task executed on main thread\n";
    });

    // Run the event loop (blocks until stop() is called)
    // This processes both X11 events and remidy events
    remidy::EventLoop::start();

    return 0;
}
```

### Using with an Existing X11 Display

If you already have an X11 display connection:

```cpp
#include "remidy/remidy.hpp"
#include "remidy/priv/event-loop-x11.hpp"

int main() {
    Display* myDisplay = XOpenDisplay(nullptr);

    // Get the X11 event loop and give it your display
    auto* x11Loop = dynamic_cast<remidy::EventLoopX11*>(remidy::getEventLoop());
    if (x11Loop) {
        x11Loop->setDisplay(myDisplay);
    }

    remidy::EventLoop::initializeOnUIThread();
    remidy::EventLoop::start();

    return 0;
}
```

## Switching to GTK-based Implementation

If you need the GTK-based choc implementation on Linux:

```cpp
#include "remidy/remidy.hpp"

extern remidy::EventLoopChoc choc; // Declared in EventLoop.cpp

int main() {
    // Switch to GTK-based implementation before initialization
    remidy::setEventLoop(&choc);

    remidy::EventLoop::initializeOnUIThread();
    remidy::EventLoop::start();

    return 0;
}
```

## Thread Safety

- `enqueueTaskOnMainThread()` is thread-safe and can be called from any thread
- `dispatchPendingEvents()` should only be called from the main/GUI thread
- `getEventFileDescriptor()` can be called from any thread after initialization

## Plugin Format Support

### VST3
VST3 plugins automatically detect the display server type and use the appropriate platform type:
- **Wayland**: Uses `kPlatformTypeWaylandSurfaceID` via `IWaylandHost` and `IWaylandFrame` interfaces
- **X11**: Uses `kPlatformTypeX11EmbedWindowID` for window embedding

The host provides `IWaylandHost::openWaylandConnection()` which returns the `wl_display*` from EventLoopLinux when running on Wayland.

### LV2
LV2 plugins support native display server detection with the following priority:
- **Wayland**: Uses `ui:WaylandSurfaceUI` (`http://lv2plug.in/ns/extensions/ui#WaylandSurfaceUI`)
  - Follows the de-facto standard established by falkTX's wayland-audio-plugin-test
  - Provides `wl_display*` via `"urn:wayland:display"` feature
  - Provides parent `wl_surface*` via `LV2_UI__parent` feature
  - Plugin returns `wl_subsurface*` as the widget pointer
  - Fallback to X11UI via XWayland if WaylandSurfaceUI not available
- **X11**: Uses `LV2_UI__X11UI` for direct X11 window embedding

This implementation follows the community standard approach (see https://github.com/falkTX/wayland-audio-plugin-test) to ensure compatibility with plugins that support native Wayland, while maintaining support for pure Wayland environments without X11/XWayland.
