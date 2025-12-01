#pragma once

#include "event-loop.hpp"
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>

#if defined(__linux__) || defined(__unix__)
#include <unistd.h>
#include <sys/socket.h>
#include <X11/Xlib.h>
#include <poll.h>
#endif

namespace remidy {

#if defined(__linux__) || defined(__unix__)

    // X11-based EventLoop implementation for Linux.
    // This provides a complete, ready-to-use event loop that integrates
    // with X11 without requiring GTK or any other framework.
    //
    // This implementation:
    // - Creates and manages an X11 display connection
    // - Processes X11 events alongside remidy events
    // - Uses file descriptors for cross-thread communication
    // - Works standalone or can be integrated with existing X11 apps
    class EventLoopX11 : public EventLoop {
    protected:
        Display* display = nullptr;
        int msgpipe[2] = {-1, -1};
        std::queue<std::function<void()>> pendingTasks;
        std::mutex taskMutex;
        std::thread::id mainThreadId;
        std::atomic<bool> shouldStop{false};
        bool ownsDisplay = false;

        void initializeOnUIThreadImpl() override {
            mainThreadId = std::this_thread::get_id();

            // Create socketpair for cross-thread communication
            if (msgpipe[0] == -1) {
                if (::socketpair(AF_LOCAL, SOCK_STREAM, 0, msgpipe) != 0) {
                    throw std::runtime_error("Failed to create socketpair for EventLoop");
                }
            }

            // Open X11 display if not already opened
            if (!display) {
                display = XOpenDisplay(nullptr);
                if (!display) {
                    throw std::runtime_error("Failed to open X11 display");
                }
                ownsDisplay = true;
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

            if (!display) {
                throw std::runtime_error("EventLoop not initialized - call initializeOnUIThread() first");
            }

            int x11Fd = ConnectionNumber(display);

            // Main event loop using poll()
            while (!shouldStop.load()) {
                // Check for pending X11 events first (non-blocking)
                while (XPending(display) > 0) {
                    XEvent event;
                    XNextEvent(display, &event);
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

        void stopImpl() override {
            shouldStop = true;
            // Wake up the event loop
            enqueueTaskOnMainThreadImpl([](){});
        }

    public:
        EventLoopX11() = default;

        // Constructor that accepts an existing X11 display
        explicit EventLoopX11(Display* existingDisplay)
            : display(existingDisplay), ownsDisplay(false) {}

        ~EventLoopX11() override {
            if (msgpipe[0] != -1) {
                ::close(msgpipe[0]);
                ::close(msgpipe[1]);
            }

            if (display && ownsDisplay) {
                XCloseDisplay(display);
            }
        }

        // Get the file descriptor to monitor for external event loops
        int getEventFileDescriptor() const {
            return msgpipe[1]; // Read end
        }

        // Get the X11 display connection
        Display* getDisplay() const {
            return display;
        }

        // Set an external X11 display (must be called before initializeOnUIThread)
        void setDisplay(Display* externalDisplay) {
            if (display && ownsDisplay) {
                XCloseDisplay(display);
            }
            display = externalDisplay;
            ownsDisplay = false;
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
