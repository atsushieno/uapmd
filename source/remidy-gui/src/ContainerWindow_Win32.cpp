#if defined(_WIN32)
#include <remidy-gui/remidy-gui.hpp>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <functional>

namespace remidy::gui {

static const wchar_t* kClassName = L"RemidyContainerWindow";

namespace {

SIZE clientSizeToWindowSize(HWND hwnd, int clientWidth, int clientHeight) {
    RECT rect{0, 0, clientWidth, clientHeight};
    DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    BOOL hasMenu = GetMenu(hwnd) != nullptr;
    if (!AdjustWindowRectEx(&rect, style, hasMenu, exStyle))
        return SIZE{clientWidth, clientHeight};

    return SIZE{rect.right - rect.left, rect.bottom - rect.top};
}

SIZE clientSizeToWindowSize(DWORD style, DWORD exStyle, int clientWidth, int clientHeight) {
    RECT rect{0, 0, clientWidth, clientHeight};
    if (!AdjustWindowRectEx(&rect, style, FALSE, exStyle))
        return SIZE{clientWidth, clientHeight};

    return SIZE{rect.right - rect.left, rect.bottom - rect.top};
}

} // namespace

class Win32ContainerWindow : public ContainerWindow {
public:
    explicit Win32ContainerWindow(const char* title, int w, int h, std::function<void()> closeCallback)
        : closeCallback_(std::move(closeCallback)) {
        registerClass();
        std::wstring wtitle;
        if (title) {
            size_t len = std::strlen(title); wtitle.resize(len);
            mbstowcs(&wtitle[0], title, len);
        } else {
            wtitle = L"Plugin UI";
        }
        constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        constexpr DWORD exStyle = 0;
        const SIZE initialWindowSize = clientSizeToWindowSize(style, exStyle, w, h);
        hwnd_ = CreateWindowExW(exStyle, kClassName, wtitle.c_str(), style,
                                CW_USEDEFAULT, CW_USEDEFAULT, initialWindowSize.cx, initialWindowSize.cy, nullptr, nullptr,
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
    void resize(int width, int height) override {
        if (!hwnd_) return;
        b_.width = width;
        b_.height = height;
        const SIZE windowSize = clientSizeToWindowSize(hwnd_, width, height);
        SetWindowPos(hwnd_, nullptr, 0, 0, windowSize.cx, windowSize.cy,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    Bounds getBounds() const override { return b_; }
    void* getHandle() const override { return hwnd_; }
    void setResizeCallback(std::function<void(int, int)> callback) override {
        resizeCallback_ = std::move(callback);
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

        if (msg == WM_SIZE && window && window->resizeCallback_) {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            window->b_.width = width;
            window->b_.height = height;
            window->resizeCallback_(width, height);
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
    std::function<void(int, int)> resizeCallback_;
};

std::unique_ptr<ContainerWindow> ContainerWindow::create(const char* title, int width, int height, std::function<void()> closeCallback) {
    return std::make_unique<Win32ContainerWindow>(title, width, height, std::move(closeCallback));
}

} // namespace remidy::gui

#endif

