#pragma once

#include <remidy/priv/event-loop.hpp>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <cstring>

#if defined(__linux__) || defined(__unix__)
#include <unistd.h>
#include <sys/socket.h>
#include <X11/Xlib.h>
#include <poll.h>
#ifdef HAVE_WAYLAND
#include <wayland-client.h>
#endif
#endif

namespace remidy {

#if defined(__linux__) || defined(__unix__)

    enum class DisplayServerType {
        Unknown,
        X11,
        Wayland
    };

    // Linux EventLoop implementation supporting both X11 and Wayland.
    // Auto-detects the display server type using the WAYLAND_DISPLAY environment variable.
    //
    // This implementation:
    // - Automatically detects X11 or Wayland at initialization
    // - Falls back to X11 if Wayland init fails
    // - Processes display server events alongside remidy events
    // - Uses file descriptors for cross-thread communication
    // - Works standalone or can be integrated with existing apps
    class EventLoopLinux : public EventLoop {
    protected:
        DisplayServerType detectedType = DisplayServerType::Unknown;

        // X11 members
        Display* x11_display = nullptr;
        bool owns_x11_display = false;

        // Wayland members
#ifdef HAVE_WAYLAND
        wl_display* wl_display_connection = nullptr;
        bool owns_wl_display = false;
#endif

        // Shared event loop infrastructure
        int msgpipe[2] = {-1, -1};
        std::queue<std::function<void()>> pendingTasks;
        std::mutex taskMutex;
        std::thread::id mainThreadId;
        std::atomic<bool> shouldStop{false};

        DisplayServerType detectDisplayServer() {
#ifdef HAVE_WAYLAND
            const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
            if (wayland_display && wayland_display[0] != '\0') {
                return DisplayServerType::Wayland;
            }
#endif

            const char* x11_display_env = std::getenv("DISPLAY");
            if (x11_display_env && x11_display_env[0] != '\0') {
                return DisplayServerType::X11;
            }

            // Default to X11 if nothing detected
            return DisplayServerType::X11;
        }

        bool initializeX11() {
            x11_display = XOpenDisplay(nullptr);
            if (!x11_display) {
                return false;
            }
            owns_x11_display = true;
            detectedType = DisplayServerType::X11;
            return true;
        }

#ifdef HAVE_WAYLAND
        bool initializeWayland() {
            wl_display_connection = wl_display_connect(nullptr);
            if (!wl_display_connection) {
                return false;
            }
            owns_wl_display = true;
            detectedType = DisplayServerType::Wayland;
            return true;
        }
#endif

        void runX11Loop() {
            int x11Fd = ConnectionNumber(x11_display);

            while (!shouldStop.load()) {
                // Check for pending X11 events first (non-blocking)
                while (XPending(x11_display) > 0) {
                    XEvent event;
                    XNextEvent(x11_display, &event);
                    // X11 events would be dispatched to windows here
                    // For now, we just drain them
                }

                // Poll both X11 FD and our message pipe
                pollfd fds[2];
                fds[0].fd = x11Fd;
                fds[0].events = POLLIN;
                fds[0].revents = 0;
                fds[1].fd = msgpipe[1];
                fds[1].events = POLLIN;
                fds[1].revents = 0;

                int result = ::poll(fds, 2, 100); // 100ms timeout

                if (result < 0) {
                    // Error in poll
                    break;
                }

                // Check X11 events
                if (fds[0].revents & POLLIN) {
                    // X11 has events, will be processed in next iteration
                }

                // Check our message pipe
                if (fds[1].revents & POLLIN) {
                    dispatchPendingEvents();
                }
            }
        }

#ifdef HAVE_WAYLAND
        void runWaylandLoop() {
            int wl_fd = wl_display_get_fd(wl_display_connection);

            while (!shouldStop.load()) {
                // Prepare Wayland display for reading
                while (wl_display_prepare_read(wl_display_connection) != 0) {
                    wl_display_dispatch_pending(wl_display_connection);
                }

                wl_display_flush(wl_display_connection);

                // Poll both Wayland FD and our message pipe
                pollfd fds[2];
                fds[0].fd = wl_fd;
                fds[0].events = POLLIN;
                fds[0].revents = 0;
                fds[1].fd = msgpipe[1];
                fds[1].events = POLLIN;
                fds[1].revents = 0;

                int result = ::poll(fds, 2, 100); // 100ms timeout

                // Handle Wayland events
                if (result == -1 || fds[0].revents == 0) {
                    wl_display_cancel_read(wl_display_connection);
                } else if (fds[0].revents & POLLIN) {
                    wl_display_read_events(wl_display_connection);
                }

                wl_display_dispatch_pending(wl_display_connection);

                // Handle our pending tasks
                if (fds[1].revents & POLLIN) {
                    dispatchPendingEvents();
                }
            }
        }
#endif

        void initializeOnUIThreadImpl() override {
            mainThreadId = std::this_thread::get_id();

            // Create socketpair for cross-thread communication
            if (msgpipe[0] == -1) {
                if (::socketpair(AF_LOCAL, SOCK_STREAM, 0, msgpipe) != 0) {
                    throw std::runtime_error("Failed to create socketpair for EventLoop");
                }
            }

            // Detect and initialize appropriate display server
            DisplayServerType preferred = detectDisplayServer();

            bool initialized = false;

#ifdef HAVE_WAYLAND
            if (preferred == DisplayServerType::Wayland) {
                initialized = initializeWayland();
                if (!initialized) {
                    // Wayland detection failed, fallback to X11
                    initialized = initializeX11();
                }
            }
#endif

            if (!initialized) {
                if (!initializeX11()) {
                    throw std::runtime_error("Failed to initialize any display server");
                }
            }
        }

        bool runningOnMainThreadImpl() override {
            return std::this_thread::get_id() == mainThreadId;
        }

        void enqueueTaskOnMainThreadImpl(std::function<void()>&& func) override {
            {
                std::lock_guard<std::mutex> lock(taskMutex);
                pendingTasks.push(std::move(func));
            }

            // Signal the event loop by writing a byte
            if (msgpipe[0] != -1) {
                uint8_t signal = 0xff;
                [[maybe_unused]] auto written = ::write(msgpipe[0], &signal, 1);
            }
        }

        void startImpl() override {
            shouldStop = false;

            if (detectedType == DisplayServerType::X11) {
                if (!x11_display) {
                    throw std::runtime_error("EventLoop not initialized - call initializeOnUIThread() first");
                }
                runX11Loop();
            }
#ifdef HAVE_WAYLAND
            else if (detectedType == DisplayServerType::Wayland) {
                if (!wl_display_connection) {
                    throw std::runtime_error("EventLoop not initialized - call initializeOnUIThread() first");
                }
                runWaylandLoop();
            }
#endif
        }

        void stopImpl() override {
            shouldStop = true;
            // Wake up the event loop
            enqueueTaskOnMainThreadImpl([](){});
        }

    public:
        EventLoopLinux() = default;

        // Constructor that accepts an existing X11 display
        explicit EventLoopLinux(Display* existingX11Display)
            : x11_display(existingX11Display), owns_x11_display(false) {
            detectedType = DisplayServerType::X11;
        }

#ifdef HAVE_WAYLAND
        // Constructor that accepts an existing Wayland display
        explicit EventLoopLinux(wl_display* existingWlDisplay)
            : wl_display_connection(existingWlDisplay), owns_wl_display(false) {
            detectedType = DisplayServerType::Wayland;
        }
#endif

        ~EventLoopLinux() override {
            if (msgpipe[0] != -1) {
                ::close(msgpipe[0]);
                ::close(msgpipe[1]);
            }

            if (x11_display && owns_x11_display) {
                XCloseDisplay(x11_display);
            }

#ifdef HAVE_WAYLAND
            if (wl_display_connection && owns_wl_display) {
                wl_display_disconnect(wl_display_connection);
            }
#endif
        }

        // Query methods
        DisplayServerType getDisplayServerType() const {
            return detectedType;
        }

        Display* getX11Display() const {
            return x11_display;
        }

#ifdef HAVE_WAYLAND
        wl_display* getWaylandDisplay() const {
            return wl_display_connection;
        }
#endif

        // Set an external X11 display (must be called before initializeOnUIThread)
        void setX11Display(Display* externalDisplay) {
            if (x11_display && owns_x11_display) {
                XCloseDisplay(x11_display);
            }
            x11_display = externalDisplay;
            owns_x11_display = false;
            detectedType = DisplayServerType::X11;
        }

#ifdef HAVE_WAYLAND
        // Set an external Wayland display (must be called before initializeOnUIThread)
        void setWaylandDisplay(wl_display* externalDisplay) {
            if (wl_display_connection && owns_wl_display) {
                wl_display_disconnect(wl_display_connection);
            }
            wl_display_connection = externalDisplay;
            owns_wl_display = false;
            detectedType = DisplayServerType::Wayland;
        }
#endif

        // Get the file descriptor to monitor for external event loops
        int getEventFileDescriptor() const {
            return msgpipe[1]; // Read end
        }

        // Dispatch all pending events (call when FD becomes readable)
        void dispatchPendingEvents() {
            // Drain the pipe
            uint8_t buffer[256];
            [[maybe_unused]] auto bytesRead = ::read(msgpipe[1], buffer, sizeof(buffer));

            // Execute all pending tasks
            std::queue<std::function<void()>> tasksToExecute;
            {
                std::lock_guard<std::mutex> lock(taskMutex);
                tasksToExecute.swap(pendingTasks);
            }

            while (!tasksToExecute.empty()) {
                try {
                    tasksToExecute.front()();
                } catch (const std::exception& e) {
                    // Log or handle exception - don't let it escape to event loop
                    // In production, you might want to log this
                } catch (...) {
                    // Prevent unknown exceptions from escaping
                }
                tasksToExecute.pop();
            }
        }
    };

#endif // __linux__ || __unix__

}
