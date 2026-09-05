// Оверлей чату через DirectComposition — щоб чат було видно поверх гри навіть у
// безрамковому повноекранному (Independent Flip), де звичайне «слоєне» вікно
// (WA_TranslucentBackground) зникає.
//
// Чому саме так (ресерч). У безрамковому повноекранному DWM переводить гру в
// Independent Flip / MPO: кадр гри сканується прямо на екран, ОМИНАЮЧИ композицію
// робочого столу. Legacy-слоєне вікно живе лише в тій композиції, тож зникає.
// Вікно ж, чий вміст малює DirectComposition через flip-swapchain, DWM може
// покласти на окрему АПАРАТНУ overlay-площину поверх гри — і воно лишається
// видимим (саме так робить конкурент на Flutter). WDA_EXCLUDEFROMCAPTURE при
// цьому й далі ховає його від OBS: це різні речі.
//
// ЦЕ КРОК 1: вікно малює лише тестовий прямокутник. Мета — переконатися, що воно
// (а) видно поверх Hunt: Showdown у 3D і (б) невидиме для OBS. Якщо так —
// наступним кроком підставимо справжній кадр чату зі спільної памʼяті.
//
// Запуск: hominka-dcomp.exe  (окремий процес, у гру НІЧОГО не вкладає — безпечно
// для античитів). Клік-крізь, без фокуса, завжди зверху.

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x11
#endif
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

static ID3D11Device*        g_dev    = nullptr;
static ID3D11DeviceContext* g_ctx    = nullptr;
static IDXGISwapChain1*      g_swap   = nullptr;
static ID3D11RenderTargetView* g_rtv  = nullptr;
static IDCompositionDevice*  g_dcomp  = nullptr;
static IDCompositionTarget*  g_target = nullptr;
static IDCompositionVisual*  g_visual = nullptr;

static const int W = 480, H = 320;   // крок 1: фіксований розмір тестового вікна

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

// D3D11 + flip-swapchain для композиції + прив'язка до вікна через DirectComposition.
static bool init_gfx(HWND hwnd) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;   // потрібно для DComp
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx)))
        return false;

    IDXGIDevice* dxdev = nullptr;
    if (FAILED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev))) return false;
    IDXGIAdapter* adapter = nullptr;
    dxdev->GetAdapter(&adapter);
    IDXGIFactory2* factory = nullptr;
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = W;
    sd.Height = H;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;   // flip-модель — обов'язково для DComp
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;       // прозорість над грою
    if (FAILED(factory->CreateSwapChainForComposition(g_dev, &sd, nullptr, &g_swap)))
        return false;

    ID3D11Texture2D* bb = nullptr;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();

    if (FAILED(DCompositionCreateDevice(dxdev, __uuidof(IDCompositionDevice), (void**)&g_dcomp)))
        return false;
    if (FAILED(g_dcomp->CreateTargetForHwnd(hwnd, TRUE, &g_target))) return false;
    g_dcomp->CreateVisual(&g_visual);
    g_visual->SetContent(g_swap);
    g_target->SetRoot(g_visual);
    g_dcomp->Commit();

    factory->Release();
    adapter->Release();
    dxdev->Release();
    return true;
}

static void render() {
    // Крок 1: усе вікно (480×320) — напівпрозора магента. Значення ПРЕМНОЖЕНІ на
    // альфу (swapchain premultiplied): 40% магента straight (1,0,1,0.4) →
    // (0.4,0,0.4,0.4). Це видимий тест: є магента над грою — DComp працює.
    const float mag[4] = {0.4f, 0.0f, 0.4f, 0.4f};
    g_ctx->ClearRenderTargetView(g_rtv, mag);
    g_swap->Present(1, 0);
    g_dcomp->Commit();
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"HominkaDCompOverlay";
    RegisterClassExW(&wc);

    // WS_EX_NOREDIRECTIONBITMAP — обов'язково для вікна, вміст якого дає
    // DirectComposition (без GDI-поверхні перенаправлення). Клік-крізь
    // (TRANSPARENT), без активації, службове, завжди зверху.
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT
            | WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, L"Hominka DComp overlay",
        WS_POPUP, 120, 120, W, H, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    // Ховаємо від захоплення (OBS/Discord його не бачать) — і водночас, завдяки
    // DComp, воно лишається видимим поверх гри.
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);

    if (!init_gfx(hwnd)) return 2;
    ShowWindow(hwnd, SW_SHOWNA);   // показати без активації

    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        render();
        // FSO-гра сидить у вищому z-band — тримаємось зверху щокадру.
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Sleep(16);
    }
}
