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
#include "vtable_hook.h"
#include "overlay_dx11.h"

using hominka::log;

namespace {

typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain*, UINT, UINT);

hominka::VtableHook g_present_hook;
PresentFn g_present_original = nullptr;
hominka::OverlayDX11 g_overlay;
volatile LONG g_frames = 0;

// Наш Present: спершу малюємо, потім віддаємо кадр грі. Порядок саме такий —
// інакше наш прямокутник ліг би під те, що гра намалює далі.
HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* swap, UINT interval, UINT flags) {
    LONG n = InterlockedIncrement(&g_frames);
    if (n == 1) log("overlay: перший перехоплений Present — кадр наш");
    else if ((n % 600) == 0) log("overlay: кадрів перехоплено %ld", n);

    // DXGI_PRESENT_TEST — гра лише перевіряє можливість показу; малювати не
    // треба, інакше ми псуємо саме цю перевірку.
    if (!(flags & DXGI_PRESENT_TEST)) {
        g_overlay.draw_test_rectangle(swap);
    }
    return g_present_original(swap, interval, flags);
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

DWORD WINAPI init_thread(LPVOID) {
    log("overlay: старт (DX11), шукаю точку показу кадру");

    // Гра могла ще не створити свій пристрій — даємо їй час зʼявитися.
    bool ok = false;
    for (int attempt = 0; attempt < 40 && !ok; ++attempt) {
        ok = install_present_hook();
        if (!ok) Sleep(250);
    }
    if (!ok) {
        log("overlay: за 10 с не вдалося поставити хук — можливо, гра не на DX11");
        return 1;
    }
    log("overlay: хук Present стоїть, чекаю кадри");
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        // У DllMain майже нічого робити не можна (loader lock), тому вся робота
        // — в окремому потоці.
        HANDLE t = CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_present_hook.remove();
        g_overlay.release();
        log("overlay: вивантаження, хук знято");
    }
    return TRUE;
}
