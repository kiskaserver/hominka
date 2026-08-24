// overlay.dll — те, що живе всередині гри і малює поверх її кадру.
//
// КРОК 1 (цей файл): довести, що кадр наш. Перехопити виклик, яким гра показує
// готовий кадр, і намалювати в ньому тестовий прямокутник. Без прямокутника ми
// ніколи не будемо певні, що хук стоїть у правильному місці, а не просто
// «нічого не впало».
//
// Як показується кадр, залежить від API:
//     DX11/DX10  IDXGISwapChain::Present   (vtable #8)   ← зроблено тут
//     DX9        IDirect3DDevice9::EndScene              ← крок 2
//     DX12       IDXGISwapChain::Present + черга команд  ← крок 2
//     Vulkan     vkQueuePresentKHR                       ← крок 2
//
// Хук — це підміна ОДНОГО запису у vtable свопчейна (vtable_hook.h). Ключове
// спостереження: vtable у DXGI одна на весь процес. Тобто якщо ми створимо свій
// тимчасовий свопчейн і підмінимо в його таблиці метод Present, ця сама підміна
// діятиме й для свопчейна ГРИ — вони дивляться в одну таблицю. Тому свій обʼєкт
// можна одразу відпустити: підміна лишається в памʼяті таблиці, а не в обʼєкті.

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "../common/log.h"
#include <d3d9.h>
#include <d3d12.h>

#include "vtable_hook.h"
#include "../common/inline_hook.h"
#include "overlay_dx11.h"
#include "overlay_dx12.h"
#include "overlay_dx9.h"
#include "overlay_gl.h"

using hominka::log;

namespace {

typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain*, UINT, UINT);

hominka::VtableHook g_present_hook;
PresentFn g_present_original = nullptr;
hominka::OverlayDX11 g_overlay;
volatile LONG g_frames = 0;
volatile LONG g_api = 0;   // 0 невідомо, 1 DX12, 2 DX11

// --- DX12 ---
typedef void (STDMETHODCALLTYPE *ExecFn)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
hominka::VtableHook g_exec_hook;
ExecFn g_exec_original = nullptr;
hominka::OverlayDX12 g_overlay12;

// --- DX9 ---
typedef HRESULT (STDMETHODCALLTYPE *EndSceneFn)(IDirect3DDevice9*);
typedef HRESULT (STDMETHODCALLTYPE *ResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

hominka::InlineHook g_endscene_hook;
hominka::InlineHook g_reset_hook;
EndSceneFn g_endscene_original = nullptr;
ResetFn g_reset_original = nullptr;
hominka::OverlayDX9 g_overlay9;

// --- OpenGL ---
typedef BOOL (WINAPI *SwapBuffersFn)(HDC);
hominka::InlineHook g_wglswap_hook;
SwapBuffersFn g_wglswap_original = nullptr;
hominka::OverlayGL g_overlaygl;
volatile LONG g_gl_frames = 0;
volatile LONG g_dx9_frames = 0;

// Наш Present: спершу малюємо, потім віддаємо кадр грі. Порядок саме такий —
// інакше наш прямокутник ліг би під те, що гра намалює далі.
HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* swap, UINT interval, UINT flags) {
    LONG n = InterlockedIncrement(&g_frames);
    if (n == 1) log("overlay: перший перехоплений Present — кадр наш");
    else if ((n % 600) == 0) log("overlay: кадрів перехоплено %ld", n);

    // DXGI_PRESENT_TEST — гра лише перевіряє можливість показу; малювати не
    // треба, інакше ми псуємо саме цю перевірку.
    if (!(flags & DXGI_PRESENT_TEST)) {
        // Той самий Present у DX11 і DX12 — розрізняємо за пристроєм свопчейна
        // (раз) і далі малюємо відповідним бекендом.
        if (g_api == 0) {
            ID3D12Device* d12 = nullptr;
            if (SUCCEEDED(swap->GetDevice(__uuidof(ID3D12Device), (void**)&d12)) && d12) {
                g_api = 1; d12->Release();
                log("overlay: гра на DX12");
            } else {
                g_api = 2;
                log("overlay: гра на DX11");
            }
        }
        if (g_api == 1) {
            g_overlay12.set_swap(swap);
            g_overlay12.draw(swap);
        } else {
            g_overlay.set_swap(swap);
            g_overlay.draw(swap);
        }
    }
    return g_present_original(swap, interval, flags);
}

// --- DX12: перехоплення черги команд ---
void STDMETHODCALLTYPE hooked_execute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l) {
    g_overlay12.capture_queue(q);
    g_exec_original(q, n, l);
}

// Пробний пристрій DX12 + черга → спільна vtable ID3D12CommandQueue, у якій
// підміняємо ExecuteCommandLists (індекс 10). Так дізнаємось про чергу гри.
bool install_d3d12_queue_hook() {
    HMODULE d12mod = GetModuleHandleW(L"d3d12.dll");
    if (!d12mod) return false;
    typedef HRESULT (WINAPI *CreateDevFn)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    CreateDevFn create = (CreateDevFn)GetProcAddress(d12mod, "D3D12CreateDevice");
    if (!create) return false;

    ID3D12Device* dev = nullptr;
    if (FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev)) || !dev)
        return false;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr;
    HRESULT hr = dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q);
    bool ok = false;
    if (SUCCEEDED(hr) && q) {
        // ExecuteCommandLists — індекс 10 у vtable ID3D12CommandQueue.
        if (g_exec_hook.install(q, 10, reinterpret_cast<void*>(&hooked_execute))) {
            g_exec_original = g_exec_hook.original<ExecFn>();
            log("overlay(dx12): ExecuteCommandLists перехоплено");
            ok = true;
        }
        q->Release();
    }
    dev->Release();
    return ok;
}

// Створює тимчасовий свопчейн на прихованому вікні. Через нього ми дістаємося
// спільної vtable. Повертає свопчейн (його треба відпустити) або nullptr.
IDXGISwapChain* make_probe_swapchain(HWND* out_hwnd, const wchar_t* cls) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, cls, L"", WS_OVERLAPPEDWINDOW,
                               0, 0, 8, 8, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        UnregisterClassW(cls, wc.hInstance);
        return nullptr;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = 8;
    desc.BufferDesc.Height = 8;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL got;
    IDXGISwapChain* swap = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, 2, D3D11_SDK_VERSION,
        &desc, &swap, &device, &got, &ctx);
    if (FAILED(hr)) {
        // WARP — програмний растеризатор: на машині без апаратного DX11 проба
        // інакше не пройшла б, а vtable у WARP та сама, що потрібно.
        hr = D3D11CreateDeviceAndSwapChain(
            NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, levels, 2, D3D11_SDK_VERSION,
            &desc, &swap, &device, &got, &ctx);
    }

    if (ctx) ctx->Release();
    if (device) device->Release();

    if (FAILED(hr) || !swap) {
        log("overlay: проба свопчейна не вдалася, hr=0x%lx", (unsigned long)hr);
        DestroyWindow(hwnd);
        UnregisterClassW(cls, wc.hInstance);
        return nullptr;
    }
    *out_hwnd = hwnd;
    return swap;
}

bool install_present_hook() {
    HWND hwnd = nullptr;
    IDXGISwapChain* swap = make_probe_swapchain(&hwnd, L"HominkaHookProbe");
    if (!swap) return false;

    bool ok = g_present_hook.install(swap, 8, reinterpret_cast<void*>(&hooked_present));
    if (ok) {
        g_present_original = g_present_hook.original<PresentFn>();
        log("overlay: адресу Present підмінено у спільній vtable");
    }

    // Обʼєкт більше не потрібен: підміна лишилася в памʼяті vtable, спільної для
    // всього процесу.
    swap->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(L"HominkaHookProbe", GetModuleHandleW(NULL));
    return ok;
}

// --- DX9 ---
HRESULT STDMETHODCALLTYPE hooked_endscene(IDirect3DDevice9* device) {
    LONG n = InterlockedIncrement(&g_dx9_frames);
    if (n == 1) log("overlay(dx9): перший перехоплений EndScene — кадр наш");
    g_overlay9.draw(device);
    return g_endscene_original(device);
}

HRESULT STDMETHODCALLTYPE hooked_reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
    // Ресурси D3DPOOL_DEFAULT треба звільнити ДО Reset, інакше він не вдасться.
    g_overlay9.on_lost();
    return g_reset_original(device, pp);
}

// --- OpenGL ---
BOOL WINAPI hooked_wglswap(HDC hdc) {
    LONG n = InterlockedIncrement(&g_gl_frames);
    if (n == 1) log("overlay(gl): перший перехоплений wglSwapBuffers — кадр наш");
    g_overlaygl.draw();
    return g_wglswap_original(hdc);
}

// wglSwapBuffers — плоский експорт opengl32.dll (не COM), тож інлайн-хук.
bool install_gl_hook() {
    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    if (!gl) return false;   // гра не на OpenGL
    void* addr = (void*)GetProcAddress(gl, "wglSwapBuffers");
    if (!addr) return false;
    if (!g_wglswap_hook.install(addr, reinterpret_cast<void*>(&hooked_wglswap)))
        return false;
    g_wglswap_original = g_wglswap_hook.original<SwapBuffersFn>();
    log("overlay(gl): wglSwapBuffers перехоплено інлайн-хуком");
    return true;
}

// Vulkan поки не реалізовано — заглушка, щоб решта збиралася й працювала.
bool install_vk_hook() { return false; }

// Створює тимчасовий пристрій DX9 і крізь нього — доступ до спільної vtable
// IDirect3DDevice9. Підміна в ній діє для пристрою гри так само, як з DXGI.
bool install_d3d9_hook() {
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9) return false;   // гра не на DX9
    typedef IDirect3D9* (WINAPI *CreateFn)(UINT);
    CreateFn create = (CreateFn)GetProcAddress(d3d9, "Direct3DCreate9");
    if (!create) { log("overlay(dx9): немає Direct3DCreate9"); return false; }
    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    if (!d3d) { log("overlay(dx9): Direct3DCreate9 повернув null"); return false; }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"HominkaD9Probe";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                               0, 0, 8, 8, NULL, NULL, wc.hInstance, NULL);

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 8;
    pp.BackBufferHeight = 8;
    pp.BackBufferCount = 1;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.MultiSampleType = D3DMULTISAMPLE_NONE;
    pp.EnableAutoDepthStencil = FALSE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.hDeviceWindow = hwnd;

    IDirect3DDevice9* device = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, &device);
    if (FAILED(hr) || !device) {
        log("overlay(dx9): CreateDevice не вдалося, hr=0x%lx", (unsigned long)hr);
        if (d3d) d3d->Release();
        if (hwnd) DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    // IDirect3DDevice9::EndScene — індекс 42, Reset — 16 (Windows SDK vtable).
    // Кожен пристрій DX9 має власну КОПІЮ vtable, тож підміна в таблиці проби
    // гру не зачепить. Зате самі функції — спільні на процес, і їх ми
    // перехоплюємо інлайн-хуком за адресою (див. inline_hook.h).
    void** vt = *reinterpret_cast<void***>(device);
    bool ok = g_endscene_hook.install(vt[42], reinterpret_cast<void*>(&hooked_endscene));
    if (ok) {
        g_endscene_original = g_endscene_hook.original<EndSceneFn>();
        if (g_reset_hook.install(vt[16], reinterpret_cast<void*>(&hooked_reset)))
            g_reset_original = g_reset_hook.original<ResetFn>();
        log("overlay(dx9): EndScene перехоплено інлайн-хуком");
    }

    device->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

DWORD WINAPI init_thread(LPVOID) {
    log("overlay: старт, шукаю, як гра показує кадр (DX9/DX11/DX12)");

    // Ставимо ОБИДВА застосовні хуки, а не перший-ліпший. Модуль не каже, який
    // саме API в ділі: сучасний d3d9.dll сам тягне dxgi.dll, тож «dxgi
    // завантажено» ще не означає DX11. Тому вішаємо хук і на DXGI Present, і на
    // DX9 EndScene, а малює той, чий виклик гра справді робить, — інший просто
    // ніколи не спрацює. Проба DXGI створює власний пристрій DX11 і «вдається»
    // всюди, тому покладатися на її успіх не можна — покладаємось на виклик.
    bool dxgi = false, d9 = false, gl = false, vk = false;
    for (int attempt = 0; attempt < 80; ++attempt) {
        bool dxgi_mod = GetModuleHandleW(L"dxgi.dll") != nullptr;
        bool d9_mod = GetModuleHandleW(L"d3d9.dll") != nullptr;
        bool gl_mod = GetModuleHandleW(L"opengl32.dll") != nullptr;
        bool vk_mod = GetModuleHandleW(L"vulkan-1.dll") != nullptr;
        if (!dxgi && dxgi_mod) {
            dxgi = install_present_hook();                     // DX11 та DX12
            // Для DX12 ще й перехоплюємо чергу команд (одноразово).
            if (dxgi && GetModuleHandleW(L"d3d12.dll")) install_d3d12_queue_hook();
        }
        if (!d9 && d9_mod) d9 = install_d3d9_hook();           // DX9
        // OpenGL і Vulkan поки не малюємо за замовчуванням: рендер готовий, але
        // в OpenGL лишається рідкісний нестабільний краш у драйвері, і виставляти
        // його стрімерам не можна. Вмикається для випробувань змінною HOMINKA_GL.
        if (!gl && gl_mod && getenv("HOMINKA_GL")) gl = install_gl_hook();
        if (!vk && vk_mod && getenv("HOMINKA_VK")) vk = install_vk_hook();
        // Досить, коли все застосовне поставлено і хоч один хук стоїть.
        bool done = (dxgi || !dxgi_mod) && (d9 || !d9_mod) &&
                    (gl || !gl_mod) && (vk || !vk_mod);
        if ((dxgi || d9 || gl || vk) && done) break;
        Sleep(250);
    }

    if (!dxgi && !d9 && !gl && !vk) {
        if (GetModuleHandleW(L"opengl32.dll")) log("overlay: гра на OpenGL — малювання буде далі");
        else if (GetModuleHandleW(L"vulkan-1.dll")) log("overlay: гра на Vulkan — малювання буде далі");
        else log("overlay: не впізнав графічний API гри — нічого не намалюю");
        return 1;
    }
    log("overlay: хуки стоять (DXGI/DX11:%s DX9:%s GL:%s Vulkan:%s), чекаю кадри",
        dxgi ? "так" : "ні", d9 ? "так" : "ні", gl ? "так" : "ні", vk ? "так" : "ні");
    return 0;
}

}  // namespace

// Підпис «це наша бібліотека». Інжектор перевіряє його ПЕРЕД тим, як щось
// вкладати: так наш інжектор не можна нацькувати на чужу (наприклад, чит-) DLL —
// він вантажить лише файл із цим маркером. Рядок унікальний навмисно.
extern "C" __declspec(dllexport) const char* HominkaOverlayMarker() {
    return "HOMINKA-OVERLAY-D7A1F3E9-b2c4-4a6e-9f10-chat-in-game";
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        // У DllMain майже нічого робити не можна (loader lock), тому вся робота
        // — в окремому потоці.
        HANDLE t = CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_present_hook.remove();
        g_exec_hook.remove();
        g_reset_hook.remove();
        g_endscene_hook.remove();
        g_wglswap_hook.remove();
        g_overlay.release();
        g_overlay12.release();
        g_overlay9.release();
        g_overlaygl.release();
        log("overlay: вивантаження, хуки знято");
    }
    return TRUE;
}
