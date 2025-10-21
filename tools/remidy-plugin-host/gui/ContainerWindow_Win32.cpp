#if defined(_WIN32)
#include "ContainerWindow.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <functional>

namespace uapmd::gui {

static const wchar_t* kClassName = L"RemidyContainerWindow";

class Win32ContainerWindow : public ContainerWindow {
public:
    explicit Win32ContainerWindow(const char* title, int w, int h) {
        registerClass();
        std::wstring wtitle;
        if (title) {
            size_t len = std::strlen(title); wtitle.resize(len);
            mbstowcs(&wtitle[0], title, len);
        } else {
            wtitle = L"Plugin UI";
        }
        hwnd_ = CreateWindowExW(0, kClassName, wtitle.c_str(), WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr,
                                GetModuleHandleW(nullptr), this);
        // Store 'this' pointer for window proc
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        b_.width = w; b_.height = h;
    }
    ~Win32ContainerWindow() override {
        if (hwnd_) {
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            DestroyWindow(hwnd_);
        }
    }
    void show(bool visible) override {
        if (!hwnd_) return;
        ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
        UpdateWindow(hwnd_);
    }
    void setResizable(bool resizable) override {
        if (!hwnd_) return;
        LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        if (resizable) {
            style |= (WS_THICKFRAME | WS_MAXIMIZEBOX);
        } else {
            style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        }
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
        // Force window to redraw with new style
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    void setBounds(const Bounds& b) override {
        if (!hwnd_) return;
        b_ = b;
        MoveWindow(hwnd_, b.x, b.y, b.width, b.height, TRUE);
    }
    Bounds getBounds() const override { return b_; }
    void* getHandle() const override { return hwnd_; }
    void setCloseCallback(std::function<void()> callback) override {
        closeCallback_ = std::move(callback);
    }

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        Win32ContainerWindow* window = reinterpret_cast<Win32ContainerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (msg == WM_CLOSE && window) {
            // Don't actually close/destroy the window - just hide it
            if (window->closeCallback_) {
                window->closeCallback_();
            }
            ShowWindow(hwnd, SW_HIDE);
            return 0; // Prevent default close behavior
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    static void registerClass() {
        static bool registered = false; if (registered) return;
        WNDCLASSW wc{}; wc.lpfnWndProc = windowProc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName; RegisterClassW(&wc); registered = true;
    }
    HWND hwnd_{};
    Bounds b_{};
    std::function<void()> closeCallback_;
};

std::unique_ptr<ContainerWindow> ContainerWindow::create(const char* title, int width, int height) {
    return std::make_unique<Win32ContainerWindow>(title, width, height);
}

} // namespace uapmd::gui

#endif

