// Крихітна гра-макет на DX11: вікно, свопчейн, у циклі чистить кадр і показує
// його через Present. Потрібна лише для перевірки кроку 1 — щоб було в що
// інжектити overlay.dll і на власні очі побачити наш прямокутник поверх її
// кадру. У випуск не входить.
//
// Фон навмисно темно-зелений: на ньому фіолетовий прямокутник оверлея видно
// одразу, і сплутати «намалювали ми» з «намалювала гра» неможливо.

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

static IDXGISwapChain* g_swap = nullptr;
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HominkaTestHost";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Hominka test host (DX11)",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               800, 500, NULL, NULL, inst, NULL);
    ShowWindow(hwnd, show);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 2, D3D11_SDK_VERSION,
        &sd, &g_swap, &g_dev, &fl, &g_ctx);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(
            NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, want, 2, D3D11_SDK_VERSION,
            &sd, &g_swap, &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) { MessageBoxW(hwnd, L"no DX11", L"testhost", 0); return 1; }

    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    g_dev->CreateRenderTargetView(back, NULL, &g_rtv);
    back->Release();

    MSG msg = {};
    const float green[4] = {0.05f, 0.20f, 0.10f, 1.0f};
    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        g_ctx->OMSetRenderTargets(1, &g_rtv, NULL);
        g_ctx->ClearRenderTargetView(g_rtv, green);
        g_swap->Present(1, 0);   // саме сюди вклиниться overlay.dll
    }

    if (g_rtv) g_rtv->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    if (g_swap) g_swap->Release();
    return 0;
}
