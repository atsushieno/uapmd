#if WIN32

#include <functional>
#include <queue>
#include <mutex>
#include <windows.h>
#include <remidy.hpp>

namespace remidy {
    // from https://learn.microsoft.com/en-us/windows/win32/winmsg/using-messages-and-message-queues :
    // > Because the system directs messages to individual windows in an application, a thread must create at least one window before starting its message loop.

    // Not verified to work, it's based on what Claude AI generated (after various corrections from me)
    class WindowsRunLoop {
    private:
        static const UINT WM_EXECUTE = WM_USER + 1;
        HWND hwnd;
        std::queue<std::function<void()>> queue;
        std::mutex mutex;

        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
            if (uMsg == WM_EXECUTE) {
                auto runLoop = reinterpret_cast<WindowsRunLoop*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
                if (runLoop)
                    runLoop->ExecuteNextFunction();
                return 0;
            }
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }

        void ExecuteNextFunction() {
            std::function<void()> func;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!queue.empty()) {
                    func = std::move(queue.front());
                    queue.pop();
                }
            }
            if (func)
                func();
        }

    public:
        WindowsRunLoop() {
            WNDCLASS wc{};
            wc.lpfnWndProc = WindowProc;
            wc.hInstance = GetModuleHandle(nullptr);
            wc.lpszClassName = L"RemidyRunLoopClass";
            RegisterClass(&wc);

            hwnd = CreateWindowEx(0, L"RemidyRunLoopClass", L"RemidyRunLoop", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandle(NULL), NULL);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        }

        ~WindowsRunLoop() {
            DestroyWindow(hwnd);
        }

        void PerformBlock(std::function<void()> block) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                queue.push(std::move(block));
            }
            PostMessage(hwnd, WM_EXECUTE, 0, 0);
        }

        void Run() {
            MSG msg;
            while (GetMessage(&msg, NULL, 0, 0)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
    };

    // public API implementation (and its supporting members)

    std::unique_ptr<WindowsRunLoop> dispatcher{nullptr};

    void EventLoop::initializeOnUIThread() {
        if (!dispatcher)
            dispatcher = std::make_unique<WindowsRunLoop>();
    }

    void EventLoop::asyncRunOnMainThread(std::function<void()> func) {
        dispatcher->PerformBlock(func);
    }

    void EventLoop::start() {
        dispatcher->Run();
    }
}

#endif
