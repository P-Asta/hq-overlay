#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>

namespace {

using Microsoft::WRL::ComPtr;

ComPtr<IDXGISwapChain> g_swap_chain;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<ID3D11RenderTargetView> g_render_target;

bool CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return false;
    return SUCCEEDED(g_device->CreateRenderTargetView(back_buffer.Get(), nullptr, &g_render_target));
}

void Resize(UINT width, UINT height) {
    if (!g_swap_chain || width == 0 || height == 0) return;
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    g_render_target.Reset();
    if (SUCCEEDED(g_swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) CreateRenderTarget();
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) Resize(LOWORD(lparam), HIWORD(lparam));
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

bool InitializeD3D11(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Width = static_cast<UINT>(client.right - client.left);
    description.BufferDesc.Height = static_cast<UINT>(client.bottom - client.top);
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION, &description, &g_swap_chain, &g_device, &selected, &g_context);
    if (FAILED(result)) {
        result = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION, &description, &g_swap_chain, &g_device, &selected, &g_context);
    }
    return SUCCEEDED(result) && CreateRenderTarget();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = L"HQOverlay.D3D11.TestHost";
    if (!RegisterClassExW(&window_class)) return 1;

    HWND window = CreateWindowExW(
        0, window_class.lpszClassName,
        L"HQ Overlay D3D11 Test Host - inject hq_overlay.dll, Insert/PageDown opens settings, Esc exits",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, instance, nullptr);
    if (window == nullptr || !InitializeD3D11(window)) return 2;
    ShowWindow(window, show_command);
    UpdateWindow(window);

    const auto started = std::chrono::steady_clock::now();
    MSG message{};
    bool running = true;
    while (running) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) running = false;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running) break;
        const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
        const float clear[] = {
            0.055F + 0.02F * std::sin(seconds * 0.7F),
            0.060F + 0.02F * std::sin(seconds * 0.9F + 1.0F),
            0.080F + 0.02F * std::sin(seconds * 1.1F + 2.0F),
            1.0F,
        };
        g_context->OMSetRenderTargets(1, g_render_target.GetAddressOf(), nullptr);
        g_context->ClearRenderTargetView(g_render_target.Get(), clear);
        g_swap_chain->Present(1, 0);
    }

    g_render_target.Reset();
    g_swap_chain.Reset();
    g_context.Reset();
    g_device.Reset();
    UnregisterClassW(window_class.lpszClassName, instance);
    return 0;
}
