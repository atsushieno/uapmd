#if defined(__linux__) && !defined(__APPLE__)
#include <remidy-gui/remidy-gui.hpp>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>

namespace remidy::gui {

class X11ContainerWindow : public ContainerWindow {
public:
    explicit X11ContainerWindow(const char* title, int w, int h, std::function<void()> closeCallback)
        : closeCallback_(std::move(closeCallback)) {
        static std::once_flag x_init_once;
        std::call_once(x_init_once, [](){ XInitThreads(); });
        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) return;
        int screen = DefaultScreen(dpy_);
        Window root = RootWindow(dpy_, screen);

        XSetWindowAttributes swa{};
        swa.event_mask = StructureNotifyMask | SubstructureNotifyMask;
        swa.backing_store = NotUseful;
        swa.background_pixmap = None;

        wnd_ = XCreateWindow(dpy_, root, b_.x, b_.y, (unsigned) w, (unsigned) h,
                             0, CopyFromParent, InputOutput, CopyFromParent,
                             CWEventMask | CWBackPixmap | CWBackingStore, &swa);
        if (!wnd_) return;
        b_.width = w; b_.height = h;

        // Set WM_DELETE_WINDOW
        wmDelete_ = XInternAtom(dpy_, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(dpy_, wnd_, &wmDelete_, 1);

        // Create an inner holder window that will act as the XEmbed socket/parent
        // We don't subscribe to input events on the holder - let the child window handle those directly
        XSetWindowAttributes hwa{};
        hwa.event_mask = StructureNotifyMask | SubstructureNotifyMask;
        hwa.backing_store = NotUseful;
        hwa.background_pixmap = None;
        holder_ = XCreateWindow(dpy_, wnd_, 0, 0, (unsigned) w, (unsigned) h, 0,
                                CopyFromParent, InputOutput, CopyFromParent,
                                CWEventMask | CWBackPixmap | CWBackingStore, &hwa);
        if (!holder_) return;

        // XEmbed atoms and info
        xembed_atom_ = XInternAtom(dpy_, "_XEMBED", False);
        Atom xembedInfo = XInternAtom(dpy_, "_XEMBED_INFO", False);
        if (xembedInfo != None && holder_) {
            long info[2] = { 0, 0 }; // version=0, flags=0
            XChangeProperty(dpy_, holder_, xembedInfo, xembedInfo, 32, PropModeReplace,
                            reinterpret_cast<unsigned char*>(info), 2);
        }

        // Set title
        XStoreName(dpy_, wnd_, title ? title : "Plugin UI");

        // Minimal event pump so window processes expose/close events
        running_.store(true);
        pump_ = std::thread([this]{ eventPump(); });
    }

    ~X11ContainerWindow() override {
        running_.store(false);
        if (pump_.joinable()) pump_.join();
        if (dpy_ && wnd_) {
            XUnmapWindow(dpy_, wnd_);
            if (holder_) XDestroyWindow(dpy_, holder_);
            XDestroyWindow(dpy_, wnd_);
            XFlush(dpy_);
        }
        if (dpy_) XCloseDisplay(dpy_);
    }

    void show(bool visible) override {
        if (!dpy_ || !wnd_) return;
        if (visible) {
            XMapRaised(dpy_, wnd_);
            if (holder_) XMapWindow(dpy_, holder_);
            XFlush(dpy_);
            // Notify child of activation if already embedded
            if (child_) sendXEmbed(child_, 1 /*XEMBED_WINDOW_ACTIVATE*/, 0, 0, 0);
        } else {
            if (child_) sendXEmbed(child_, 2 /*XEMBED_WINDOW_DEACTIVATE*/, 0, 0, 0);
            if (holder_) XUnmapWindow(dpy_, holder_);
            XUnmapWindow(dpy_, wnd_);
        }
        XFlush(dpy_);
    }

    void resize(int width, int height) override {
        if (!dpy_ || !wnd_) return;
        // X11 doesn't allow 0 dimensions - reject invalid resize requests
        if (width <= 0 || height <= 0) {
            fprintf(stderr, "ContainerWindow: Rejected invalid resize request: %dx%d\n", width, height);
            return;
        }
        b_.width = width;
        b_.height = height;
        XResizeWindow(dpy_, wnd_, (unsigned)width, (unsigned)height);
        if (holder_) XResizeWindow(dpy_, holder_, (unsigned)width, (unsigned)height);
        XFlush(dpy_);
    }

    Bounds getBounds() const override { return b_; }

    void* getHandle() const override {
        // Pass the holder (socket) XID to plugins
        return reinterpret_cast<void*>(static_cast<uintptr_t>(holder_ ? holder_ : wnd_));
    }
    void setResizeCallback(std::function<void(int, int)> callback) override {
        resizeCallback_ = std::move(callback);
    }
    void setResizable(bool resizable) override {
        if (!dpy_ || !wnd_) return;
        XSizeHints hints{};
        hints.flags = PMinSize | PMaxSize;
        if (resizable) {
            // Allow resizing - set very large max size
            hints.min_width = 1;
            hints.min_height = 1;
            hints.max_width = 16384;
            hints.max_height = 16384;
        } else {
            // Fixed size - set min and max to current size
            hints.min_width = b_.width;
            hints.min_height = b_.height;
            hints.max_width = b_.width;
            hints.max_height = b_.height;
        }
        XSetWMNormalHints(dpy_, wnd_, &hints);
        XFlush(dpy_);
    }

    Display* dpy_{nullptr};
    Window wnd_{};
    Bounds b_{};
    Window holder_{}; // socket window inside top-level
    Atom wmDelete_{};
    Atom xembed_atom_{};
    std::thread pump_{};
    std::atomic<bool> running_{false};
    Window child_{};
    std::function<void()> closeCallback_;
    std::function<void(int, int)> resizeCallback_;

    void eventPump() {
        // Only handle events targeted to our container window; don't drain the connection-wide queue.
        const long mask = StructureNotifyMask | SubstructureNotifyMask;
        while (running_.load()) {
            if (!dpy_) break;
            XEvent ev{};
            bool handled = false;
            // Use XCheckWindowEvent to limit to events for wnd_
            while (XCheckWindowEvent(dpy_, wnd_, mask, &ev) || XCheckTypedWindowEvent(dpy_, wnd_, ClientMessage, &ev)) {
                handled = true;
                switch (ev.type) {
                    case ClientMessage:
                        if (ev.xclient.message_type == XInternAtom(dpy_, "WM_PROTOCOLS", False)
                            && static_cast<Atom>(ev.xclient.data.l[0]) == wmDelete_) {
                            // Don't actually close the window - just hide it
                            if (closeCallback_) {
                                closeCallback_();
                            }
                            XUnmapWindow(dpy_, wnd_);
                            if (holder_) XUnmapWindow(dpy_, holder_);
                            XFlush(dpy_);
                        }
                        break;
                    case ConfigureNotify:
                        if (ev.xconfigure.window == wnd_) {
                            int newWidth = ev.xconfigure.width;
                            int newHeight = ev.xconfigure.height;
                            // Only notify if size actually changed
                            if (newWidth != b_.width || newHeight != b_.height) {
                                b_.width = newWidth;
                                b_.height = newHeight;
                                // Resize holder to match
                                if (holder_) XResizeWindow(dpy_, holder_, (unsigned)newWidth, (unsigned)newHeight);
                                // Notify via callback
                                if (resizeCallback_) {
                                    resizeCallback_(newWidth, newHeight);
                                }
                            }
                        }
                        break;
                    case ReparentNotify:
                        if (holder_ && ev.xreparent.parent == holder_) {
                            child_ = ev.xreparent.window;
                            // Send embedded notify on reparent
                            sendXEmbed(child_, 0 /*XEMBED_EMBEDDED_NOTIFY*/, 0 /*version*/, holder_, 0);
                        }
                        break;
                    case MapNotify:
                        if (holder_ && ev.xmap.event == holder_) {
                            // Child mapped inside our container
                            if (!child_) child_ = ev.xmap.window;
                            if (child_) {
                                // Ensure child fills the holder window and is positioned at origin
                                XMoveResizeWindow(dpy_, child_, 0, 0, (unsigned) b_.width, (unsigned) b_.height);
                                sendXEmbed(child_, 1 /*XEMBED_WINDOW_ACTIVATE*/, 0, 0, 0);
                            }
                        }
                        break;
                    default:
                        break;
                }
            }
            if (handled) XFlush(dpy_);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void sendXEmbed(Window target, long message, long detail, long data1, long data2) {
        if (!dpy_ || !target || !xembed_atom_) return;
        XClientMessageEvent ce{};
        ce.type = ClientMessage;
        ce.window = target;
        ce.message_type = xembed_atom_;
        ce.format = 32;
        ce.data.l[0] = CurrentTime;
        ce.data.l[1] = message;
        ce.data.l[2] = detail;
        ce.data.l[3] = data1;
        ce.data.l[4] = data2;
        XSendEvent(dpy_, target, False, NoEventMask, reinterpret_cast<XEvent*>(&ce));
    }
};

std::unique_ptr<ContainerWindow> ContainerWindow::create(const char* title, int width, int height, std::function<void()> closeCallback) {
    return std::make_unique<X11ContainerWindow>(title, width, height, std::move(closeCallback));
}

} // namespace remidy::gui

#endif
