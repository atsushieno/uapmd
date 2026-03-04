#include "PlatformBackend.hpp"

#if defined(USE_WIN32_BACKEND) || defined(USE_DIRECTX11_RENDERER)

#include <imgui.h>
#include <iostream>
#include <windows.h>
#include <windowsx.h>

#ifdef USE_WIN32_BACKEND
#include <imgui_impl_win32.h>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

#ifdef USE_DIRECTX11_RENDERER
#include <d3d11.h>
#include <dxgi.h>
#include <imgui_impl_dx11.h>
#endif

namespace uapmd {
namespace gui {

#ifdef USE_WIN32_BACKEND

// Global quit flag for Win32
static bool g_Win32_QuitRequested = false;

class Win32WindowingBackend : public WindowingBackend {
private:
    bool initialized = false;
    WindowHandle* currentWindow = nullptr;
    HINSTANCE hInstance = nullptr;
    const wchar_t* windowClassName = L"UAPMDWindowClass";

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
        // Forward messages to ImGui Win32 handler
        if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
            return true;

        switch (message) {
        case WM_SIZE:
            // Window resize handled by DirectX renderer
            return 0;
        case WM_CLOSE:
            g_Win32_QuitRequested = true;
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hWnd, message, wParam, lParam);
        }
    }

public:
    bool initialize() override {
        hInstance = GetModuleHandle(nullptr);

        // Set DPI awareness
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        // Register window class
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc = WndProc;
        wc.cbClsExtra = 0;
        wc.cbWndExtra = 0;
        wc.hInstance = hInstance;
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszMenuName = nullptr;
        wc.lpszClassName = windowClassName;
        wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

        if (!RegisterClassExW(&wc)) {
            std::cerr << "Win32 Error: Failed to register window class" << std::endl;
            return false;
        }

        initialized = true;
        return true;
    }

    WindowHandle* createWindow(const char* title, int width, int height) override {
        if (!initialized) return nullptr;

        // Convert title to wide string
        int titleLen = MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0);
        wchar_t* wideTitle = new wchar_t[titleLen];
        MultiByteToWideChar(CP_UTF8, 0, title, -1, wideTitle, titleLen);

        // Create window
        DWORD dwStyle = WS_OVERLAPPEDWINDOW;
        RECT rect = { 0, 0, width, height };
        AdjustWindowRect(&rect, dwStyle, FALSE);

        HWND hwnd = CreateWindowExW(
            0,
            windowClassName,
            wideTitle,
            dwStyle,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top,
            nullptr,
            nullptr,
            hInstance,
            nullptr
        );

        delete[] wideTitle;

        if (!hwnd) {
            std::cerr << "Win32 Error: Failed to create window" << std::endl;
            return nullptr;
        }

        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        currentWindow = new WindowHandle(hwnd);
        return currentWindow;
    }

    void destroyWindow(WindowHandle* window) override {
        if (window && window->type == WindowHandle::Win32) {
            DestroyWindow(window->hwnd);
            delete window;
            if (window == currentWindow) currentWindow = nullptr;
        }
    }

    bool shouldClose(WindowHandle* window) override {
        return g_Win32_QuitRequested || window == nullptr;
    }

    void swapBuffers(WindowHandle* window) override {
        // No-op for DirectX (handled by renderer Present())
    }

    void getDrawableSize(WindowHandle* window, int* width, int* height) override {
        if (window && window->type == WindowHandle::Win32) {
            RECT rect;
            GetClientRect(window->hwnd, &rect);
            *width = rect.right - rect.left;
            *height = rect.bottom - rect.top;
        }
    }

    void setWindowSize(WindowHandle* window, int width, int height) override {
        if (window && window->type == WindowHandle::Win32) {
            RECT rect = { 0, 0, width, height };
            AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(window->hwnd, nullptr, 0, 0,
                        rect.right - rect.left, rect.bottom - rect.top,
                        SWP_NOMOVE | SWP_NOZORDER);
        }
    }

    void makeContextCurrent(WindowHandle* window) override {
        // No-op for DirectX (no context to make current)
    }

    void shutdown() override {
        if (initialized) {
            UnregisterClassW(windowClassName, hInstance);
            initialized = false;
        }
    }

    const char* getName() const override {
        return "Win32";
    }
};

class Win32ImGuiBackend : public ImGuiPlatformBackend {
private:
    WindowHandle* window = nullptr;

public:
    bool initialize(WindowHandle* win) override {
        if (!win || win->type != WindowHandle::Win32) return false;
        window = win;
        return ImGui_ImplWin32_Init(win->hwnd);
    }

    void processEvents() override {
        // Process Win32 messages
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                g_Win32_QuitRequested = true;
            }
        }
    }

    void newFrame() override {
        ImGui_ImplWin32_NewFrame();
    }

    void shutdown() override {
        ImGui_ImplWin32_Shutdown();
    }

    const char* getName() const override {
        return "Win32 ImGui Backend";
    }
};

std::unique_ptr<WindowingBackend> createWin32WindowingBackend() {
    return std::make_unique<Win32WindowingBackend>();
}

std::unique_ptr<ImGuiPlatformBackend> createWin32ImGuiBackend() {
    return std::make_unique<Win32ImGuiBackend>();
}

#endif // USE_WIN32_BACKEND

#ifdef USE_DIRECTX11_RENDERER

class DirectX11Renderer : public ImGuiRenderer {
private:
    ID3D11Device*           g_pd3dDevice = nullptr;
    ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
    IDXGISwapChain*         g_pSwapChain = nullptr;
    ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

    bool                    g_SwapChainOccluded = false;
    UINT                    g_ResizeWidth = 0;
    UINT                    g_ResizeHeight = 0;

    void CreateRenderTarget() {
        ID3D11Texture2D* pBackBuffer = nullptr;
        g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
        if (pBackBuffer) {
            g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
            pBackBuffer->Release();
        }
    }

    void CleanupRenderTarget() {
        if (g_mainRenderTargetView) {
            g_mainRenderTargetView->Release();
            g_mainRenderTargetView = nullptr;
        }
    }

    bool CreateDeviceD3D(HWND hWnd) {
        // Setup swap chain
        DXGI_SWAP_CHAIN_DESC sd;
        ZeroMemory(&sd, sizeof(sd));
        sd.BufferCount = 2;
        sd.BufferDesc.Width = 0;
        sd.BufferDesc.Height = 0;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hWnd;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        UINT createDeviceFlags = 0;
        #ifdef _DEBUG
        createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
        #endif

        D3D_FEATURE_LEVEL featureLevel;
        const D3D_FEATURE_LEVEL featureLevelArray[2] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_0,
        };

        HRESULT res = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createDeviceFlags,
            featureLevelArray,
            2,
            D3D11_SDK_VERSION,
            &sd,
            &g_pSwapChain,
            &g_pd3dDevice,
            &featureLevel,
            &g_pd3dDeviceContext);

        // Try WARP software renderer if hardware failed
        if (res == DXGI_ERROR_UNSUPPORTED) {
            res = D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                createDeviceFlags,
                featureLevelArray,
                2,
                D3D11_SDK_VERSION,
                &sd,
                &g_pSwapChain,
                &g_pd3dDevice,
                &featureLevel,
                &g_pd3dDeviceContext);
        }

        if (res != S_OK)
            return false;

        CreateRenderTarget();
        return true;
    }

    void CleanupDeviceD3D() {
        CleanupRenderTarget();
        if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
        if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
        if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
    }

public:
    bool initialize(WindowHandle* window) override {
        if (!window || window->type != WindowHandle::Win32) {
            std::cerr << "DirectX11Renderer Error: Window is not Win32 type" << std::endl;
            return false;
        }

        HWND hWnd = window->hwnd;
        if (!hWnd) {
            std::cerr << "DirectX11Renderer Error: Invalid HWND" << std::endl;
            return false;
        }

        if (!CreateDeviceD3D(hWnd)) {
            std::cerr << "DirectX11Renderer Error: Failed to create D3D11 device" << std::endl;
            return false;
        }

        if (!ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext)) {
            std::cerr << "DirectX11Renderer Error: Failed to initialize ImGui DX11 backend" << std::endl;
            CleanupDeviceD3D();
            return false;
        }

        return true;
    }

    void newFrame() override {
        ImGui_ImplDX11_NewFrame();
    }

    void renderDrawData() override {
        // Check if swap chain needs resizing based on ImGui's display size
        ImGuiIO& io = ImGui::GetIO();
        UINT targetWidth = (UINT)io.DisplaySize.x;
        UINT targetHeight = (UINT)io.DisplaySize.y;

        if (targetWidth > 0 && targetHeight > 0) {
            // Get current swap chain dimensions
            ID3D11Texture2D* pBackBuffer = nullptr;
            g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
            D3D11_TEXTURE2D_DESC desc;
            pBackBuffer->GetDesc(&desc);
            pBackBuffer->Release();

            // Resize swap chain if dimensions don't match
            if (desc.Width != targetWidth || desc.Height != targetHeight) {
                CleanupRenderTarget();
                // Unbind all render targets and flush before ResizeBuffers;
                // any lingering OM references to swap chain buffers will cause
                // ResizeBuffers to return DXGI_ERROR_INVALID_CALL and leave the
                // swap chain broken, crashing Present on the next frame.
                g_pd3dDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
                g_pd3dDeviceContext->Flush();
                HRESULT resizeHr = g_pSwapChain->ResizeBuffers(0, targetWidth, targetHeight, DXGI_FORMAT_UNKNOWN, 0);
                if (SUCCEEDED(resizeHr))
                    CreateRenderTarget();
            }
        }

        // Get current swap chain dimensions for viewport
        ID3D11Texture2D* pBackBuffer = nullptr;
        g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
        D3D11_TEXTURE2D_DESC desc;
        pBackBuffer->GetDesc(&desc);
        pBackBuffer->Release();

        // Set viewport
        D3D11_VIEWPORT vp;
        vp.Width = (FLOAT)desc.Width;
        vp.Height = (FLOAT)desc.Height;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        vp.TopLeftX = 0;
        vp.TopLeftY = 0;
        g_pd3dDeviceContext->RSSetViewports(1, &vp);

        // Clear and render
        const float clear_color[4] = { 0.45f, 0.55f, 0.60f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);

        // Render ImGui draw data
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Present with VSync
        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    void shutdown() override {
        ImGui_ImplDX11_Shutdown();
        CleanupDeviceD3D();
    }

    const char* getName() const override {
        return "DirectX11";
    }

    void handleResize(int width, int height) {
        g_ResizeWidth = (UINT)width;
        g_ResizeHeight = (UINT)height;
    }
};

std::unique_ptr<ImGuiRenderer> createDirectX11Renderer() {
    return std::make_unique<DirectX11Renderer>();
}

#endif // USE_DIRECTX11_RENDERER

} // namespace gui
} // namespace uapmd

#endif // defined(USE_WIN32_BACKEND) || defined(USE_DIRECTX11_RENDERER)
