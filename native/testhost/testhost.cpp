// Гра-макет на DX11: вікно, свопчейн, у циклі чистить кадр і показує його через
// Present. Слугує двом цілям: (1) автотести інжектора; (2) ЗРАЗОК ДЛЯ OBS —
// наведіть на це вікно Game Capture, заінжектьте overlay і перевіряйте
// «Ховати чат від OBS»: з галочкою чат видно на моніторі, але не в ефірі.
//
// По кадру плавають яскраві прямокутники (малюємо через ClearView по областях —
// без шейдерів), щоб з першого погляду було видно, що OBS усе захопив. У випуск
// не входить.

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>   // ID3D11DeviceContext1::ClearView — очистка ОБЛАСТІ
#include <dxgi.h>
#include <math.h>

static const int SW = 1280, SH = 720;

static IDXGISwapChain* g_swap = nullptr;
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static ID3D11DeviceContext1* g_ctx1 = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;

// «Фігурки»: розмір, швидкості/фази, колір. Позицію рахуємо з часу (пінг-понг),
// стану тримати не треба.
struct Shape { float w, h, sx, sy, px, py; float col[4]; };
static Shape g_shapes[] = {
    {190, 190, 150, 118,   0.0f, 300, {1.00f, 0.20f, 0.28f, 1}},  // червоний
    {150, 230, 100, 175,  640,   90, {0.15f, 0.95f, 1.00f, 1}},  // блакитний
    {220, 130, 195,  85,  260,  470, {1.00f, 0.85f, 0.15f, 1}},  // жовтий
    {140, 140, 132, 150,  820,  260, {0.85f, 0.25f, 1.00f, 1}},  // фіолетовий
    {170, 170, 168, 132,  420,  560, {0.25f, 1.00f, 0.45f, 1}},  // зелений
};

static float ping(float v, float mx) {
    if (mx <= 0) return 0;
    float p = fmodf(v, 2 * mx);
    if (p < 0) p += 2 * mx;
    return p > mx ? 2 * mx - p : p;
}

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
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Hominka DX11 sample (OBS test)",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               1280, 720, NULL, NULL, inst, NULL);
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

    g_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&g_ctx1);

    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    g_dev->CreateRenderTargetView(back, NULL, &g_rtv);
    back->Release();

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        float t = (float)GetTickCount() / 1000.0f;
        // Яскравий переливний фон.
        float ph = t * 1.05f;
        float bg[4] = {
            0.16f + 0.14f * (0.5f + 0.5f * (float)cos(ph)),
            0.10f + 0.12f * (0.5f + 0.5f * (float)cos(ph + 2.094f)),
            0.24f + 0.16f * (0.5f + 0.5f * (float)cos(ph + 4.188f)),
            1.0f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, NULL);
        g_ctx->ClearRenderTargetView(g_rtv, bg);
        // Плавучі фігурки — очищення по областях (ClearView є з Win8).
        if (g_ctx1) {
            for (const Shape& s : g_shapes) {
                float x = ping(t * s.sx + s.px, SW - s.w);
                float y = ping(t * s.sy + s.py, SH - s.h);
                D3D11_RECT r = {(LONG)x, (LONG)y, (LONG)(x + s.w), (LONG)(y + s.h)};
                g_ctx1->ClearView(g_rtv, s.col, &r, 1);
            }
        }
        g_swap->Present(1, 0);   // саме сюди вклиниться overlay.dll
    }

    if (g_rtv) g_rtv->Release();
    if (g_ctx1) g_ctx1->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    if (g_swap) g_swap->Release();
    return 0;
}
