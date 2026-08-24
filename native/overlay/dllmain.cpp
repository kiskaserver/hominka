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
#include <vector>
#include <cstring>
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
#include "overlay_vk.h"

using hominka::log;

namespace {

typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain*, UINT, UINT);

hominka::VtableHook g_present_hook;
PresentFn g_present_original = nullptr;
hominka::OverlayDX11 g_overlay;
volatile LONG g_frames = 0;
volatile LONG g_api = 0;   // 0 невідомо, 1 DX12, 2 DX11

// Для приховування від OBS малюємо якнайглибше — ІНЛАЙН-хуком самого тіла
// Present (5-байтна E9-латка, тож внутрішній «швидкий вхід» Present+5 цілий).
// OBS-захоплення сидить на рівні свопчейна, тож встигає зняти чистий кадр, а наш
// чат лягає вже перед показом.
hominka::InlineHook g_present_inline_hook;
PresentFn g_present_inline_orig = nullptr;
bool g_present_inline_ok = false;

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

// --- Vulkan ---
// Чіпляємось до плоских експортів vulkan-1.dll (завантажувач Vulkan проксіює
// їх для всього процесу). Інлайн-хук уже безпечний (заморозка потоків).
typedef VkResult (VKAPI_PTR *CreateDeviceFn)(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
typedef void (VKAPI_PTR *GetDeviceQueueFn)(VkDevice, uint32_t, uint32_t, VkQueue*);
typedef VkResult (VKAPI_PTR *CreateSwapchainFn)(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
typedef void (VKAPI_PTR *DestroySwapchainFn)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
typedef VkResult (VKAPI_PTR *QueuePresentFn)(VkQueue, const VkPresentInfoKHR*);
hominka::InlineHook g_vk_create_device_hook, g_vk_get_queue_hook,
    g_vk_create_swap_hook, g_vk_destroy_swap_hook, g_vk_present_hook;
CreateDeviceFn g_vk_create_device_orig = nullptr;
GetDeviceQueueFn g_vk_get_queue_orig = nullptr;
CreateSwapchainFn g_vk_create_swap_orig = nullptr;
DestroySwapchainFn g_vk_destroy_swap_orig = nullptr;
QueuePresentFn g_vk_present_orig = nullptr;
hominka::OverlayVK g_overlayvk;
volatile LONG g_vk_frames = 0;

VkResult VKAPI_PTR hooked_vkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                                         const VkAllocationCallbacks* al, VkDevice* dev) {
    VkResult r = g_vk_create_device_orig(phys, ci, al, dev);
    if (r == VK_SUCCESS && dev && *dev) g_overlayvk.on_device(phys, *dev);
    return r;
}
void VKAPI_PTR hooked_vkGetDeviceQueue(VkDevice dev, uint32_t fam, uint32_t idx, VkQueue* q) {
    g_vk_get_queue_orig(dev, fam, idx, q);
    if (q && *q) g_overlayvk.on_queue(*q, fam);
}
VkResult VKAPI_PTR hooked_vkCreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
                                               const VkAllocationCallbacks* al, VkSwapchainKHR* sc) {
    VkResult r = g_vk_create_swap_orig(dev, ci, al, sc);
    if (r == VK_SUCCESS && sc && *sc) g_overlayvk.on_swapchain(*sc, ci);
    return r;
}
void VKAPI_PTR hooked_vkDestroySwapchainKHR(VkDevice dev, VkSwapchainKHR sc, const VkAllocationCallbacks* al) {
    if (sc) g_overlayvk.on_swapchain_destroy(sc);
    g_vk_destroy_swap_orig(dev, sc, al);
}
VkResult VKAPI_PTR hooked_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR* pi) {
    LONG n = InterlockedIncrement(&g_vk_frames);
    if (n == 1) log("overlay(vk): перший перехоплений vkQueuePresentKHR — кадр наш");
    VkPresentInfoKHR local;
    std::vector<VkSemaphore> wait_store;
    g_overlayvk.on_present(q, pi, &local, &wait_store);
    return g_vk_present_orig ? g_vk_present_orig(q, &local) : VK_SUCCESS;
}

// Гра часто дістає функції Vulkan не з експортів, а через
// vkGetInstanceProcAddr/vkGetDeviceProcAddr, і завантажувач повертає для WSI
// (свопчейн/показ) ОКРЕМІ, залежні від пристрою адреси — не ті, що в експорті.
// Тому перехоплюємо саме РОЗДАЧУ адрес: на відомі імена віддаємо свій детур,
// запам'ятавши справжній вказівник. Це працює, лише якщо ми на місці ДО того,
// як гра розв'язала ці функції (тобто інжект до старту Vulkan у грі).
typedef PFN_vkVoidFunction (VKAPI_PTR *GIPAFn)(VkInstance, const char*);
typedef PFN_vkVoidFunction (VKAPI_PTR *GDPAFn)(VkDevice, const char*);
hominka::InlineHook g_vk_gipa_hook, g_vk_gdpa_hook;
GIPAFn g_vk_gipa_orig = nullptr;
GDPAFn g_vk_gdpa_orig = nullptr;
PFN_vkVoidFunction VKAPI_PTR hooked_vkGetDeviceProcAddr(VkDevice, const char*);

static PFN_vkVoidFunction vk_reroute(const char* name, PFN_vkVoidFunction real) {
    if (!real || !name) return real;
    // ВАЖЛИВО: справжні вказівники (g_vk_*_orig) беремо ЛИШЕ з експорт-хуків
    // (там за перехідником E9 — тіло функції). Тут їх НЕ чіпаємо: завантажувач
    // на ці імена повертає адресу самого експорт-перехідника, який ми вже
    // перенаправили на свій детур, — присвоїти його як «оригінал» означало б
    // нескінченну рекурсію (детур кличе сам себе). Просто віддаємо свій детур.
    #define R(n, det) if (!strcmp(name, n)) return reinterpret_cast<PFN_vkVoidFunction>(&det);
    R("vkQueuePresentKHR",     hooked_vkQueuePresentKHR);
    R("vkCreateSwapchainKHR",  hooked_vkCreateSwapchainKHR);
    R("vkDestroySwapchainKHR", hooked_vkDestroySwapchainKHR);
    R("vkGetDeviceQueue",      hooked_vkGetDeviceQueue);
    R("vkCreateDevice",        hooked_vkCreateDevice);
    #undef R
    return real;
}
PFN_vkVoidFunction VKAPI_PTR hooked_vkGetDeviceProcAddr(VkDevice dev, const char* name) {
    PFN_vkVoidFunction real = g_vk_gdpa_orig ? g_vk_gdpa_orig(dev, name) : nullptr;
    return vk_reroute(name, real);
}
PFN_vkVoidFunction VKAPI_PTR hooked_vkGetInstanceProcAddr(VkInstance inst, const char* name) {
    PFN_vkVoidFunction real = g_vk_gipa_orig ? g_vk_gipa_orig(inst, name) : nullptr;
    if (name && !strcmp(name, "vkGetDeviceProcAddr")) {
        // g_vk_gdpa_orig уже стоїть з експорт-хука (справжнє тіло); real тут —
        // це знову ж таки перенаправлений експорт, тож не переприсвоюємо.
        return reinterpret_cast<PFN_vkVoidFunction>(&hooked_vkGetDeviceProcAddr);
    }
    if (name && !strcmp(name, "vkGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(&hooked_vkGetInstanceProcAddr);
    return vk_reroute(name, real);
}

// Наш Present: спершу малюємо, потім віддаємо кадр грі. Порядок саме такий —
// інакше наш прямокутник ліг би під те, що гра намалює далі.
// Спільне малювання для обох хуків Present (зовнішнього свопчейн-vtable та
// внутрішнього інлайн). API визначаємо раз, далі — відповідний бекенд. obs_split
// вмикається, коли стоїть внутрішній хук: тоді малює рівно один шар залежно від
// прапорця hide_from_obs у кадрі (див. overlay_dx11.h).
static void draw_dxgi(IDXGISwapChain* swap, bool inner) {
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
        g_overlay12.draw(swap, inner, g_present_inline_ok);
    } else {
        g_overlay.set_swap(swap);
        g_overlay.draw(swap, inner, g_present_inline_ok);
    }
}

HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* swap, UINT interval, UINT flags) {
    LONG n = InterlockedIncrement(&g_frames);
    if (n == 1) log("overlay: перший перехоплений Present — кадр наш");
    else if ((n % 600) == 0) log("overlay: кадрів перехоплено %ld", n);

    // DXGI_PRESENT_TEST — гра лише перевіряє можливість показу; малювати не
    // треба, інакше ми псуємо саме цю перевірку.
    if (!(flags & DXGI_PRESENT_TEST)) draw_dxgi(swap, /*inner=*/false);
    return g_present_original(swap, interval, flags);
}

// Внутрішній (найглибший) хук самого тіла Present — крізь нього проходить і
// виклик «оригіналу» від OBS. Малює лише в режимі приховування (inner=true).
HRESULT STDMETHODCALLTYPE hooked_present_inline(IDXGISwapChain* swap, UINT interval, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) draw_dxgi(swap, /*inner=*/true);
    return g_present_inline_orig(swap, interval, flags);
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

        // Додатково — інлайн-хук самого тіла Present (адреса з vtable). Це
        // найглибший шар: крізь нього проходить і виклик «оригіналу» з OBS-хука,
        // тож у режимі приховування чат лягає ПІСЛЯ зняття кадру OBS. Латка — 5
        // байтів, тож внутрішній вхід Present+5 лишається цілим. Пролог не
        // піддався — не біда: тоді просто без приховування (чат видно і в OBS).
        if (g_present_inline_hook.install(reinterpret_cast<void*>(g_present_original),
                                          reinterpret_cast<void*>(&hooked_present_inline))) {
            g_present_inline_orig = g_present_inline_hook.original<PresentFn>();
            g_present_inline_ok = true;
            log("overlay: інлайн-хук Present стоїть — приховування від OBS доступне");
        } else {
            log("overlay: інлайн-хук Present не став — без приховування від OBS");
        }
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

// Vulkan: вантажимо функції й ставимо інлайн-хуки на плоскі експорти
// vulkan-1.dll. Пристрій/чергу/свопчейн ловимо на льоту, малюємо в present.
bool install_vk_hook() {
    HMODULE vk = GetModuleHandleW(L"vulkan-1.dll");
    if (!vk) return false;
    if (!g_overlayvk.load(vk)) { log("overlay(vk): не всі функції Vulkan знайдено"); return false; }

    struct { const char* name; hominka::InlineHook* hook; void* detour; void** orig; } hooks[] = {
        // Головні — перехоплення роздачі адрес (ловить і статичну лінковку через
        // експорт, і динамічну через *ProcAddr).
        {"vkGetInstanceProcAddr",&g_vk_gipa_hook,          (void*)&hooked_vkGetInstanceProcAddr,(void**)&g_vk_gipa_orig},
        {"vkGetDeviceProcAddr",  &g_vk_gdpa_hook,          (void*)&hooked_vkGetDeviceProcAddr,  (void**)&g_vk_gdpa_orig},
        // Прямі експорти — на випадок, коли гра кличе їх без *ProcAddr.
        {"vkCreateDevice",       &g_vk_create_device_hook, (void*)&hooked_vkCreateDevice,       (void**)&g_vk_create_device_orig},
        {"vkGetDeviceQueue",     &g_vk_get_queue_hook,     (void*)&hooked_vkGetDeviceQueue,     (void**)&g_vk_get_queue_orig},
        {"vkCreateSwapchainKHR", &g_vk_create_swap_hook,   (void*)&hooked_vkCreateSwapchainKHR, (void**)&g_vk_create_swap_orig},
        {"vkDestroySwapchainKHR",&g_vk_destroy_swap_hook,  (void*)&hooked_vkDestroySwapchainKHR,(void**)&g_vk_destroy_swap_orig},
        {"vkQueuePresentKHR",    &g_vk_present_hook,        (void*)&hooked_vkQueuePresentKHR,    (void**)&g_vk_present_orig},
    };
    bool gipa_ok = false, present_ok = false;
    for (auto& h : hooks) {
        void* addr = (void*)GetProcAddress(vk, h.name);
        if (!addr) continue;
        if (h.hook->install(addr, h.detour)) {
            *h.orig = h.hook->original<void*>();
            if (h.hook == &g_vk_gipa_hook) gipa_ok = true;
            if (h.hook == &g_vk_present_hook) present_ok = true;
        }
    }
    // Досить перехопити роздачу адрес АБО прямий present — тоді Vulkan наш.
    // (Малює лише за інжекту до старту Vulkan у грі — інакше пристрій уже
    //  створено без нас; тоді просто нічого не малюємо, гру не чіпаємо.)
    bool ok = gipa_ok || present_ok;
    if (ok) log("overlay(vk): хуки Vulkan поставлено (роздача=%s, present=%s)",
               gipa_ok ? "так" : "ні", present_ok ? "так" : "ні");
    return ok;
}

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

// OBS Game Capture впорскує в гру graphics-hook64/32.dll. Наявність цього
// модуля — надійна ознака, що OBS зараз захоплює саме через хук Present.
static bool obs_capture_present() {
    return GetModuleHandleW(L"graphics-hook64.dll") || GetModuleHandleW(L"graphics-hook32.dll");
}

DWORD WINAPI init_thread(LPVOID) {
    log("overlay: старт, шукаю, як гра показує кадр (DX9/DX11/DX12)");
    if (obs_capture_present())
        log("overlay: помічено graphics-hook OBS — захоплення через Present активне");

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
        // OpenGL малюємо за замовчуванням: колишній рідкісний краш був не в
        // драйвері, а в самій підміні гарячого wglSwapBuffers (гонка з потоком
        // рендера) — тепер вона робиться під заморозкою потоків, тож безпечно.
        // На контексті core-профілю чесно не малюємо (фіксований конвеєр там
        // заборонено), але й не валимо гру.
        if (!gl && gl_mod) gl = install_gl_hook();
        // Vulkan теж за замовчуванням. Малює лише коли оверлей опинився в грі ДО
        // того, як вона ініціалізувала Vulkan (пристрій/свопчейн створюються раз
        // на старті — без нас їх уже не перехопити). Якщо інжект пізніший — хуки
        // просто стоять без діла: гра нічого не помітить, чат не з'явиться.
        if (!vk && vk_mod) vk = install_vk_hook();
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
        g_present_inline_hook.remove();
        g_present_hook.remove();
        g_exec_hook.remove();
        g_reset_hook.remove();
        g_endscene_hook.remove();
        g_wglswap_hook.remove();
        g_vk_gipa_hook.remove();
        g_vk_gdpa_hook.remove();
        g_vk_present_hook.remove();
        g_vk_create_device_hook.remove();
        g_vk_get_queue_hook.remove();
        g_vk_create_swap_hook.remove();
        g_vk_destroy_swap_hook.remove();
        g_overlay.release();
        g_overlay12.release();
        g_overlay9.release();
        g_overlaygl.release();
        g_overlayvk.release();
        log("overlay: вивантаження, хуки знято");
    }
    return TRUE;
}
