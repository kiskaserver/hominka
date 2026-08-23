// Крихітна гра-макет на DX9 — щоб перевірити крок 2 для DX9 (як L4D2, TF2,
// старий CS:GO). Вікно, пристрій, у циклі Clear→BeginScene→EndScene→Present.
// Наш overlay.dll вклинюється в EndScene. У випуск не входить.
#include <windows.h>
#include <d3d9.h>

static IDirect3D9* g_d3d = nullptr;
static IDirect3DDevice9* g_dev = nullptr;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HominkaTestHostDX9";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Hominka test host (DX9)",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               800, 500, NULL, NULL, inst, NULL);
    ShowWindow(hwnd, show);

    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) { MessageBoxW(hwnd, L"no d3d9", L"testhost", 0); return 1; }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    HRESULT hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_dev);
    if (FAILED(hr))
        hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_dev);
    if (FAILED(hr)) { MessageBoxW(hwnd, L"no device", L"testhost", 0); return 1; }

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        g_dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(13, 51, 26), 1.0f, 0);
        if (SUCCEEDED(g_dev->BeginScene())) {
            g_dev->EndScene();      // сюди вклиниться overlay.dll
        }
        g_dev->Present(NULL, NULL, NULL, NULL);
    }

    if (g_dev) g_dev->Release();
    if (g_d3d) g_d3d->Release();
    return 0;
}
