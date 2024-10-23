#if !WIN32 && !__APPLE__

#include <remidy.hpp>

// The code is not verified yet, almost just what Claude AI generated.

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <pthread.h>
#include <stdlib.h>

#define CUSTOM_EVENT (LASTEvent + 1)

namespace remidy {

    typedef struct {
        void (*func)(void*);
        void* data;
    } CustomEventData;

    typedef struct {
        Display* display;
        Window window;
        pthread_t thread;
        int should_exit;
    } X11RunLoop;

    static void* x11_event_loop(void* arg) {
        X11RunLoop* runloop = (X11RunLoop*)arg;
        XEvent event;

        while (!runloop->should_exit) {
            if (XPending(runloop->display)) {
                XNextEvent(runloop->display, &event);
                if (event.type == CUSTOM_EVENT) {
                    CustomEventData* eventData = (CustomEventData*)event.xclient.data.l[0];
                    eventData->func(eventData->data);
                    free(eventData);
                }
            }
        }

        return NULL;
    }

    X11RunLoop* x11_runloop_create() {
        X11RunLoop* runloop = malloc(sizeof(X11RunLoop));
        if (!runloop) return NULL;

        runloop->display = XOpenDisplay(NULL);
        if (!runloop->display) {
            free(runloop);
            return NULL;
        }

        runloop->window = XCreateSimpleWindow(runloop->display, DefaultRootWindow(runloop->display),
                                              0, 0, 1, 1, 0, 0, 0);

        XSelectInput(runloop->display, runloop->window, StructureNotifyMask);
        XMapWindow(runloop->display, runloop->window);

        runloop->should_exit = 0;

        if (pthread_create(&runloop->thread, NULL, x11_event_loop, runloop) != 0) {
            XDestroyWindow(runloop->display, runloop->window);
            XCloseDisplay(runloop->display);
            free(runloop);
            return NULL;
        }

        return runloop;
    }

    void x11_runloop_perform(X11RunLoop* runloop, void (*func)(void*), void* data) {
        if (!runloop || !func) return;

        CustomEventData* eventData = malloc(sizeof(CustomEventData));
        if (!eventData) return;

        eventData->func = func;
        eventData->data = data;

        XEvent event = {0};
        event.type = CUSTOM_EVENT;
        event.xany.window = runloop->window;
        event.xclient.data.l[0] = (long)eventData;

        XSendEvent(runloop->display, runloop->window, false, NoEventMask, &event);
        XFlush(runloop->display);
    }

    void x11_runloop_destroy(X11RunLoop* runloop) {
        if (!runloop) return;

        runloop->should_exit = 1;
        pthread_join(runloop->thread, nullptr);

        XDestroyWindow(runloop->display, runloop->window);
        XCloseDisplay(runloop->display);
        free(runloop);
    }

    // public API implementation (and its supporting members)

    struct X11RunLoopDeleter {
        void operator delete(std::unique_ptr<X11RunLoop>::pointer ptr) {
            x11_runloop_destroy(ptr);
        }
    };

    std::unique_ptr<X11RunLoop, X11RunLoopDeleter> dispatcher{nullptr};

    void EventLoop::initializeOnUIThread() {
        if (!dispatcher)
            dispatcher.reset(x11_runloop_create());
    }

    void doInvoke(void* data) {
        auto func = static_cast<std::function<void()> *>(data);
        func();
        delete func;
    }

    void EventLoop::asyncRunOnMainThread(std::function<void()> func) {
        x11_runloop_perform(dispatcher.get(), doInvoke, std::move(&func));
    }

    void EventLoop::start() {
        // nothing to do here...?
    }
}

#endif
