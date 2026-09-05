// Оверлей чату через DirectComposition — щоб чат було видно поверх гри навіть у
// безрамковому повноекранному (Independent Flip), де звичайне «слоєне» вікно
// (WA_TranslucentBackground) зникає. Окремий процес; у гру НІЧОГО не вкладає —
// безпечно для античитів (Hunt: Showdown / EAC).
//
// Чому працює (ресерч, підтверджено на Hunt): вміст цього вікна малює
// DirectComposition через flip-swapchain — таке вікно DWM може покласти на
// окрему АПАРАТНУ overlay-площину поверх гри, тоді як legacy-слоєне вікно живе
// лише в композиції робочого столу й зникає в незалежному flip. WDA_EXCLUDEFROMCAPTURE
// і далі ховає його від OBS — це різні речі (крок 1 це підтвердив).
//
// КРОК 2: беремо готовий кадр чату зі СПІЛЬНОЇ ПАМʼЯТІ (той самий продюсер, що й
// для інжект-оверлея — hominka/gameoverlay.py пише BGRA), і показуємо його.
// Розкладка: РОЗМІР вікна задає сам кадр (width×height), ПОЗИЦІЮ (лівий-верхній
// кут на екрані) ставить Python через SetWindowPos — так вікно збігається з
// вікном чату на робочому столі.

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <vector>
#include <stdio.h>
#include <stdarg.h>

#include "../overlay/shared_frame_reader.h"

// --- журнал (той самий файл, що й у решти оверлея) --------------------------
static void dlog(const char* fmt, ...) {
    char msg[512];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    char path[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (!n || n > MAX_PATH - 24) return;
    lstrcatA(path, "hominka-overlay.log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] dcomp: %s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    fclose(f);
}

// Ловимо збої (AV тощо) і пишемо модуль+адресу — інакше окремий процес падав би
// без жодного сліду. Не перехоплюємо (CONTINUE_SEARCH) — лише лишаємо запис.
static LONG CALLBACK dcomp_veh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0xC0000005 || code == 0xC0000409) {
        void* addr = ep->ExceptionRecord->ExceptionAddress;
        char name[MAX_PATH] = "?";
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)addr, &mod) && mod)
            GetModuleFileNameA(mod, name, sizeof name);
        dlog("КРАШ code=0x%lX addr=%p модуль=%s", code, addr, name);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x11
#endif
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

static ID3D11Device*        g_dev    = nullptr;
static ID3D11DeviceContext* g_ctx    = nullptr;
static IDXGIFactory2*       g_factory = nullptr;
static IDXGISwapChain1*      g_swap   = nullptr;
static ID3D11RenderTargetView* g_rtv  = nullptr;
static ID3D11Texture2D*      g_upload = nullptr;   // сюди кладемо BGRA кадру
static IDCompositionDevice*  g_dcomp  = nullptr;
static IDCompositionTarget*  g_target = nullptr;
static IDCompositionVisual*  g_visual = nullptr;

static int g_w = 0, g_h = 0;       // поточний розмір свопчейна/вікна
static bool g_shown = false;
static uint32_t g_last_seq = 0xFFFFFFFFu;  // seq останнього НАМАЛЬОВАНОГО кадру
static bool g_blanked = false;             // прозорий кадр уже показано (не мигаємо)
static hominka::SharedFrameReader g_reader;
static std::vector<uint8_t> g_buf; // буфер під премножені пікселі

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

// D3D11 + DirectComposition. Свопчейн створюємо/переростимо під розмір кадру.
static bool init_gfx(HWND hwnd) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx)))
        return false;
    IDXGIDevice* dxdev = nullptr;
    if (FAILED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev))) return false;
    IDXGIAdapter* adapter = nullptr; dxdev->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&g_factory);
    adapter->Release();

    if (FAILED(DCompositionCreateDevice(dxdev, __uuidof(IDCompositionDevice), (void**)&g_dcomp))) {
        dxdev->Release(); return false;
    }
    dxdev->Release();
    if (FAILED(g_dcomp->CreateTargetForHwnd(hwnd, TRUE, &g_target))) return false;
    g_dcomp->CreateVisual(&g_visual);
    g_target->SetRoot(g_visual);
    return true;
}

// Готує свопчейн і текстуру-приймач під розмір w×h. Розмір ВІКНА теж підганяємо
// (позицію лишаємо Python). Свопчейн створюється раз, далі — ResizeBuffers.
static bool ensure_size(HWND hwnd, int w, int h) {
    if (g_swap && g_w == w && g_h == h) return true;
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }

    if (!g_swap) {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = w; sd.Height = h;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(g_factory->CreateSwapChainForComposition(g_dev, &sd, nullptr, &g_swap)))
            return false;
        g_visual->SetContent(g_swap);
        g_dcomp->Commit();
    } else if (FAILED(g_swap->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0))) {
        return false;
    }

    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
        g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
        bb->Release();
    }

    if (g_upload) { g_upload->Release(); g_upload = nullptr; }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    g_dev->CreateTexture2D(&td, nullptr, &g_upload);

    g_w = w; g_h = h;
    // РОЗМІР вікна = розмір кадру; позицію не чіпаємо (її ставить Python).
    SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    return g_swap && g_upload;
}

static void present_transparent() {
    if (!g_swap || !g_rtv) return;
    const float clear[4] = {0, 0, 0, 0};
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    g_swap->Present(1, 0);
    g_dcomp->Commit();
}

static void render(HWND hwnd) {
    // Немає продюсера / чат вимкнено / кадру ще нема: чистимо ОДИН раз і далі
    // GPU не чіпаємо. Раніше тут щокадру (60 к/с) йшов Present — і навіть без
    // чату оверлей молотив відеокарту.
    if (!g_reader.ensure_open()) { return; }
    hominka::FrameView f;
    if (!g_reader.read(&f) || !f.enabled || f.width == 0 || f.height == 0) {
        if (g_shown && !g_blanked) { present_transparent(); g_blanked = true; }
        g_last_seq = 0xFFFFFFFFu;
        return;
    }
    g_blanked = false;
    const bool resized = ((int)f.width != g_w || (int)f.height != g_h);
    if (!ensure_size(hwnd, (int)f.width, (int)f.height)) {
        dlog("ensure_size %ux%u НЕ вдалося", f.width, f.height);
        return;
    }
    if (!g_upload) return;
    // Кадр не змінився (той самий seq) і вже показаний — DirectComposition сама
    // тримає попередній на екрані, копіювати й презентувати НЕМАЄ ЧОГО. Саме
    // безумовний Present щокадру відбирав GPU в гри й давав лаги/розростання.
    if (!resized && f.seq == g_last_seq && g_shown) return;
    g_last_seq = f.seq;
    static bool first = false;
    if (!first) { first = true; dlog("перший кадр %ux%u — малюю", f.width, f.height); }

    // Свопчейн premultiplied — премножуємо RGB на альфу (і на загальну opacity).
    const size_t n = (size_t)f.width * f.height;
    g_buf.resize(n * 4);
    const uint8_t* s = f.pixels;
    const float op = (f.opacity > 255 ? 255 : f.opacity) / 255.0f;
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = s[i*4+0], g = s[i*4+1], r = s[i*4+2], a = s[i*4+3];
        float fa = (a / 255.0f) * op;
        g_buf[i*4+0] = (uint8_t)(b * fa);
        g_buf[i*4+1] = (uint8_t)(g * fa);
        g_buf[i*4+2] = (uint8_t)(r * fa);
        g_buf[i*4+3] = (uint8_t)(a * op);
    }
    g_ctx->UpdateSubresource(g_upload, 0, nullptr, g_buf.data(), f.width * 4, 0);

    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
        g_ctx->CopyResource(bb, g_upload);
        bb->Release();
        g_swap->Present(1, 0);
        g_dcomp->Commit();
    }

    if (!g_shown) { ShowWindow(hwnd, SW_SHOWNA); g_shown = true; }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmd, int) {
    AddVectoredExceptionHandler(1, dcomp_veh);
    dlog("старт");
    // Аргумент — PID Hominka. Стежимо за ним і виходимо, коли вона зникла (навіть
    // якщо впала): щоб оверлей ніколи не лишався сиротою на екрані.
    HANDLE parent = nullptr;
    if (cmd && *cmd) {
        DWORD ppid = (DWORD)_wtoi(cmd);
        if (ppid) parent = OpenProcess(SYNCHRONIZE, FALSE, ppid);
    }
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"HominkaDCompOverlay";   // Python шукає вікно за цим класом
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT
            | WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, L"Hominka DComp overlay",
        WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);   // OBS не бачить
    if (!init_gfx(hwnd)) { dlog("init_gfx НЕ вдалося — вихід"); return 2; }
    dlog("init_gfx ок");

    MSG msg;
    unsigned tick = 0;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0)
            return 0;   // Hominka зникла — виходимо
        render(hwnd);
        // FSO-гра сидить у вищому z-band — тримаємось зверху, але НЕ щокадру: раз
        // на ~250 мс досить, а 60 разів/с лише засмічувало чергу вікон (позицію й
        // розмір не чіпаємо: позицію веде Python, розмір — кадр).
        if ((tick++ % 16) == 0 && g_shown)
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Sleep(16);
    }
}
