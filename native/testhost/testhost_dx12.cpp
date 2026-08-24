// Крихітна гра-макет на DX12 — для перевірки кроку 2 у DX12 (сучасні ігри).
// Вікно, пристрій, черга, свопчейн на 2 буфери, у циклі: чистить задній буфер і
// Present. Наш overlay.dll вклинюється в Present + захоплює чергу через
// ExecuteCommandLists. У випуск не входить.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <math.h>

static const UINT N = 2;
static ID3D12Device* g_dev;
static ID3D12CommandQueue* g_queue;
static IDXGISwapChain3* g_swap;
static ID3D12DescriptorHeap* g_rtvHeap;
static ID3D12Resource* g_back[N];
static D3D12_CPU_DESCRIPTOR_HANDLE g_rtv[N];
static ID3D12CommandAllocator* g_alloc[N];
static ID3D12GraphicsCommandList* g_cl;
static ID3D12Fence* g_fence;
static UINT64 g_fenceVal[N];
static UINT64 g_fenceCounter;
static HANDLE g_fenceEvent;
static UINT g_rtvStep;

static int SW = 1280, SH = 720;   // фактичний розмір кадру (весь екран)

// «Фігурки»: розмір, швидкості/фази, колір. Позицію рахуємо з часу (пінг-понг).
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
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { PostQuitMessage(0); return 0; }  // Esc — вихід
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc; wc.hInstance = inst;
    wc.lpszClassName = L"HominkaTestHostDX12";
    RegisterClassExW(&wc);
    // Безрамкове на весь екран, як у справжніх ігор (borderless fullscreen):
    // саме такий кадр ловить OBS Game Capture. WS_EX_APPWINDOW — щоб вікно було
    // в панелі завдань і у списку вікон OBS; робимо його активним переднім.
    // Esc — вихід.
    SW = GetSystemMetrics(SM_CXSCREEN);
    SH = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Hominka DX12 sample (OBS test)",
                               WS_POPUP, 0, 0, SW, SH, NULL, NULL, inst, NULL);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&g_dev))) {
        MessageBoxW(hwnd, L"no d3d12", L"testhost", 0); return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&g_queue);

    IDXGIFactory4* fac = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&fac);
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.BufferCount = N; sd.Width = SW; sd.Height = SH;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;
    IDXGISwapChain1* sc1 = nullptr;
    fac->CreateSwapChainForHwnd(g_queue, hwnd, &sd, nullptr, nullptr, &sc1);
    sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&g_swap);
    sc1->Release(); fac->Release();

    D3D12_DESCRIPTOR_HEAP_DESC rh = {};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = N;
    g_dev->CreateDescriptorHeap(&rh, __uuidof(ID3D12DescriptorHeap), (void**)&g_rtvHeap);
    g_rtvStep = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < N; ++i) {
        g_swap->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&g_back[i]);
        g_dev->CreateRenderTargetView(g_back[i], nullptr, h);
        g_rtv[i] = h; h.ptr += g_rtvStep;
        g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&g_alloc[i]);
    }
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&g_cl);
    g_cl->Close();
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fence);
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); continue; }
        UINT i = g_swap->GetCurrentBackBufferIndex();
        if (g_fenceVal[i] && g_fence->GetCompletedValue() < g_fenceVal[i]) {
            g_fence->SetEventOnCompletion(g_fenceVal[i], g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, 100);
        }
        g_alloc[i]->Reset();
        g_cl->Reset(g_alloc[i], nullptr);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_back[i];
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_cl->ResourceBarrier(1, &b);
        float t = (float)GetTickCount() / 1000.0f;
        float ph = t * 1.05f;
        const float bg[4] = {
            0.16f + 0.14f * (0.5f + 0.5f * (float)cos(ph)),
            0.10f + 0.12f * (0.5f + 0.5f * (float)cos(ph + 2.094f)),
            0.24f + 0.16f * (0.5f + 0.5f * (float)cos(ph + 4.188f)),
            1.0f };
        g_cl->ClearRenderTargetView(g_rtv[i], bg, 0, nullptr);
        // Плавучі яскраві фігурки — очищення по областях (rects у DX12 очистці).
        for (const Shape& s : g_shapes) {
            float x = ping(t * s.sx + s.px, SW - s.w);
            float y = ping(t * s.sy + s.py, SH - s.h);
            D3D12_RECT r = {(LONG)x, (LONG)y, (LONG)(x + s.w), (LONG)(y + s.h)};
            g_cl->ClearRenderTargetView(g_rtv[i], s.col, 1, &r);
        }
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_cl->ResourceBarrier(1, &b);
        g_cl->Close();
        ID3D12CommandList* lists[] = { g_cl };
        g_queue->ExecuteCommandLists(1, lists);   // сюди вклиниться overlay (захопить чергу)
        g_fenceVal[i] = ++g_fenceCounter;
        g_queue->Signal(g_fence, g_fenceVal[i]);
        g_swap->Present(1, 0);                     // і сюди (малювання)
        Sleep(8);
    }
    return 0;
}
