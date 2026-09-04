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
#include <stdint.h>
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
hominka::InlineHook g_present_inline;   // альтернатива vtable-хуку: коли тіло вже
                                        // під чужим inline-хуком (OBS/Steam), стаємо
                                        // ланкою inline-ланцюга, не чіпаючи vtable
PresentFn g_present_original = nullptr;
// «Чистий flip» — шлях у справжній показ кадру В ОБХІД чужого inline-хука тіла
// Present (Steam-оверлей патчить перший байт тіла на E9 → свій детур). Будуємо
// його з непропатчених байтів dxgi.dll на диску; кличемо лише на реентрантному
// вході, коли Steam зсередини свого детура знову викликає swap->Present(), — так
// рветься нескінченна рекурсія Steam↔ми (див. build_clean_flip і hooked_present).
PresentFn g_clean_flip = nullptr;
// Захист від реентрантної рекурсії Present. ГЛОБАЛЬНИЙ атомік, а не thread_local:
// у інжектнутій mingw-DLL thread_local для потоків гри, що виникли ДО інжекту (а
// саме на потоці рендера все й крутиться), не ініціалізований і завжди читає 0 —
// тобто thread_local-вартовий мертвий. Атомік працює завжди й для всіх потоків.
volatile LONG g_present_busy = 0;
hominka::OverlayDX11 g_overlay;
volatile LONG g_frames = 0;
volatile LONG g_api = 0;   // 0 невідомо, 1 DX12, 2 DX11

// Приховування від OBS у DX12 (див. overlay_dx12.h): не через порядок хуків
// Present — у DX12 надійного порядку немає, — а через чергу команд. OBS копіює
// бекбуфер своїм D3D11On12 і його .Flush() шле ExecuteCommandLists на чергу гри,
// яку ми теж перехоплюємо. Тому: коли треба сховати чат, при Present НЕ малюємо,
// а чекаємо копію OBS у hooked_execute й кладемо чат ОДРАЗУ ПІСЛЯ неї, на ту саму
// чергу — на GPU він виконається пізніше копії.
volatile LONG g_in_present = 0;        // ми всередині ланцюга Present (там копіює OBS)
bool g_obs_drew_this_present = false;  // цього Present ми вже доклали чат після копії
bool g_submitting_overlay = false;     // ми самі шлемо ECL — не сплутати з копією OBS
IDXGISwapChain* g_last_swap = nullptr; // свопчейн із Present — щоб малювати з ECL

// --- DX12 ---
typedef void (STDMETHODCALLTYPE *ExecFn)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
hominka::VtableHook g_exec_hook;
ExecFn g_exec_original = nullptr;
hominka::OverlayDX12 g_overlay12;

// --- DX11: приховування від OBS через хук GetBuffer ---
// OBS Game Capture у СВОЄМУ Present-детурі щокадру бере бекбуфер через
// swap->GetBuffer(0) і копіює його. Тож замість перехоплювати саму копію на
// гарячому immediate-контексті (тисячі викликів рендеру → просадка FPS), ми
// хукаємо GetBuffer на спільній DXGI-vtable: коли бекбуфер просять УСЕРЕДИНІ
// Present у режимі приховування (це OBS), віддаємо ЧИСТИЙ знімок кадру замість
// справжнього бекбуфера — OBS зніме без чату, а на моніторі лишиться бекбуфер із
// чатом. 0 хуків на копію, ~1 підміна за кадр. Наші/ігрові GetBuffer (поза
// Present) не чіпаємо — гра рендерить у справжній бекбуфер.
typedef HRESULT (STDMETHODCALLTYPE *GetBufferFn)(IDXGISwapChain*, UINT, REFIID, void**);
hominka::VtableHook g_getbuffer_hook;
GetBufferFn g_getbuffer_orig = nullptr;

// --- DX9 ---
typedef HRESULT (STDMETHODCALLTYPE *EndSceneFn)(IDirect3DDevice9*);
typedef HRESULT (STDMETHODCALLTYPE *ResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
// Приховування від OBS у DX9: OBS копіює бекбуфер у своєму хуку Present через
// StretchRect (shtex) або GetRenderTargetData (shmem). Хукаємо обидва, ловимо
// копію саме бекбуфера й обгортаємо її (чистий кадр → копія → назад із чатом).
typedef HRESULT (STDMETHODCALLTYPE *StretchRectFn)(IDirect3DDevice9*, IDirect3DSurface9*,
        const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
typedef HRESULT (STDMETHODCALLTYPE *GetRTDataFn)(IDirect3DDevice9*, IDirect3DSurface9*,
        IDirect3DSurface9*);

hominka::InlineHook g_endscene_hook;
hominka::InlineHook g_reset_hook;
hominka::InlineHook g_d9_stretch_hook;
hominka::InlineHook g_d9_getrtdata_hook;
EndSceneFn g_endscene_original = nullptr;
ResetFn g_reset_original = nullptr;
StretchRectFn g_d9_stretch_orig = nullptr;
GetRTDataFn g_d9_getrtdata_orig = nullptr;
volatile LONG g_d9_wrapping = 0;   // ми самі свопаємо бекбуфер — не сплутати з копією OBS
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

static bool obs_capture_present();   // визначено нижче, біля init_thread

// Малювання чату при Present. API (DX11/DX12) визначаємо раз, далі — відповідний
// бекенд. DX12 у режимі приховування ще й знімає чистий кадр (обгортання копії
// OBS — у hooked_execute); DX11 просто малює при Present.
static void dxgi_present(IDXGISwapChain* swap) {
    if (!swap) return;
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
    bool obs = obs_capture_present();
    if (g_api == 1) {
        g_overlay12.set_swap(swap);
        g_overlay12.present_draw(swap, obs);
    } else {
        g_overlay.set_swap(swap);
        // Знімок чистого кадру + чат у бекбуфер (g_in_present ще 0, тож наш власний
        // GetBuffer у знімку/малюванні отримує СПРАВЖНІЙ бекбуфер). Підміну для
        // OBS робить хук GetBuffer уже в межах Present (g_in_present=1).
        g_overlay.draw(swap, obs);
    }
}

HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* swap, UINT interval, UINT flags) {
    // Реентрантний вхід: ми вже в цьому виклику Present (глобальний прапорець уже
    // піднято). Так буває з чужими оверлеями (Steam), які ЗСЕРЕДИНИ свого детура
    // знову кличуть swap->Present() і повертають керування сюди. Якщо тут знову
    // піти в g_present_original (тіло → E9 → Steam), утвориться нескінченна
    // рекурсія і STACK_OVERFLOW. Тому показуємо кадр обхідним трампліном у
    // справжній dxgi (повз чужий детур) і виходимо — ланцюг стає скінченним.
    if (InterlockedCompareExchange(&g_present_busy, 1, 0) != 0) {
        static LONG once = 0;
        if (InterlockedCompareExchange(&once, 1, 0) == 0)
            log("overlay: РЕЕНТРАНТ Present спіймано — обхід через %s",
                g_clean_flip ? "clean_flip" : "g_present_original");
        return g_clean_flip ? g_clean_flip(swap, interval, flags)
                            : g_present_original(swap, interval, flags);
    }

    LONG n = InterlockedIncrement(&g_frames);
    if (n == 1) log("overlay: перший перехоплений Present — кадр наш");
    else if ((n % 600) == 0) log("overlay: кадрів перехоплено %ld", n);

    // DXGI_PRESENT_TEST — гра лише перевіряє можливість показу; нічого не робимо.
    if (flags & DXGI_PRESENT_TEST) {
        InterlockedExchange(&g_present_busy, 0);
        return g_present_original(swap, interval, flags);
    }

    g_last_swap = swap;
    // Малюємо чат при КОЖНОМУ Present (моник завжди з чатом). У режимі приховування
    // це ще й знімає чистий кадр — а копію OBS нижче обгорнемо в hooked_execute.
    dxgi_present(swap);

    // OBS робить свою копію бекбуфера саме в межах цього виклику (D3D11On12.Flush
    // → ExecuteCommandLists на черзі гри) — позначаємо вікно для hooked_execute.
    g_obs_drew_this_present = false;
    InterlockedExchange(&g_in_present, 1);
    // Звичайний показ: кличемо оригінал (у грі зі Steam-оверлеєм це його детур —
    // хай малює свій оверлей). Якщо Steam зсередини знову покличе Present, його
    // спіймає реентрантний вартовий вище й покаже кадр обхідним flip — без петлі.
    HRESULT r = g_present_original(swap, interval, flags);
    InterlockedExchange(&g_in_present, 0);
    InterlockedExchange(&g_present_busy, 0);
    return r;
}

// --- DX12: перехоплення черги команд ---
void STDMETHODCALLTYPE hooked_execute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l) {
    g_overlay12.capture_queue(q);

    // Це той самий виклик, яким OBS кладе копію бекбуфера (D3D11On12.Flush під час
    // Present). Якщо ми ховаємо чат — обгортаємо копію: ПЕРЕД нею повертаємо в
    // бекбуфер чистий знімок (OBS зніме чисте), а ПІСЛЯ домальовуємо чат назад для
    // показу. Усе на ту саму чергу q — на GPU строго в цьому порядку.
    if (g_in_present && !g_submitting_overlay && !g_obs_drew_this_present
        && g_overlay12.hide_armed()) {
        g_submitting_overlay = true;
        g_overlay12.obs_wrap_before(q);
        g_exec_original(q, n, l);
        g_overlay12.obs_wrap_after(q);
        g_submitting_overlay = false;
        g_obs_drew_this_present = true;
        return;
    }
    g_exec_original(q, n, l);
}

// Пробний пристрій DX12 + черга → спільна vtable ID3D12CommandQueue, у якій
// підміняємо ExecuteCommandLists (індекс 10). Так дізнаємось про чергу гри.
bool install_d3d12_queue_hook() {
    HMODULE d12mod = GetModuleHandleW(L"d3d12.dll");
    if (!d12mod) return false;
    typedef HRESULT (WINAPI *CreateDevFn)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    CreateDevFn create = (CreateDevFn)(void*)GetProcAddress(d12mod, "D3D12CreateDevice");
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

// Хук GetBuffer на спільній DXGI-vtable. Коли бекбуфер (індекс 0) просять
// УСЕРЕДИНІ Present у режимі приховування — це OBS у своєму детурі бере кадр для
// копії; віддаємо йому ЧИСТИЙ знімок замість справжнього бекбуфера. Поза Present
// (рендер гри, наші знімок/малювання) — віддаємо справжній бекбуфер, тож гра
// малює куди слід, а на моніторі лишається чат.
HRESULT STDMETHODCALLTYPE hooked_getbuffer(IDXGISwapChain* swap, UINT idx,
        REFIID riid, void** out) {
    if (idx == 0 && g_in_present && g_overlay.wants_hide() && out) {
        ID3D11Resource* clean = g_overlay.clean_resource();
        // Віддаємо чисту текстуру в ТОМУ інтерфейсі, який просить OBS (він бере
        // бекбуфер як IDXGIResource через IID_PPV_ARGS, не як ID3D11Texture2D).
        // QueryInterface сам робить AddRef — OBS зробить Release після копії.
        if (clean && SUCCEEDED(clean->QueryInterface(riid, out))) {
            static LONG w = 0;
            LONG c = InterlockedIncrement(&w);
            if (c == 1 || (c % 600) == 0)
                log("overlay(dx11): бекбуфер підмінено для OBS, разів=%ld", c);
            return S_OK;
        }
    }
    return g_getbuffer_orig(swap, idx, riid, out);
}

// GetBuffer — індекс 9 у спільній vtable IDXGISwapChain (Present=8). Ставимо через
// пробний свопчейн: vtable у DXGI спільна на процес, тож підміна діє й для гри.
bool install_getbuffer_hook(IDXGISwapChain* probe) {
    if (g_getbuffer_hook.installed()) return true;
    if (g_getbuffer_hook.install(probe, 9, reinterpret_cast<void*>(&hooked_getbuffer))) {
        g_getbuffer_orig = g_getbuffer_hook.original<GetBufferFn>();
        log("overlay(dx11): GetBuffer перехоплено (для приховування від OBS)");
        return true;
    }
    return false;
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

static bool obs_capture_present();   // визначено нижче, біля init_thread

// Імʼя модуля, якому належить адреса (напр. dxgi.dll чи graphics-hook64.dll).
static const wchar_t* module_of(void* addr) {
    static wchar_t name[MAX_PATH];
    HMODULE mod = nullptr;
    if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)addr, &mod) && mod) {
        wchar_t path[MAX_PATH];
        if (GetModuleFileNameW(mod, path, MAX_PATH)) {
            const wchar_t* base = wcsrchr(path, L'\\');
            wcsncpy(name, base ? base + 1 : path, MAX_PATH - 1);
            name[MAX_PATH - 1] = 0;
            return name;
        }
    }
    return L"(поза модулями)";
}

// Логгер краху: не глушить виняток (повертає CONTINUE_SEARCH — гра падає як
// падала), а лише пише в лог точну адресу фолту, її модуль і скільки кадрів ми
// вже намалювали. Так у ОДНОМУ повторі краху видно, ДЕ саме валиться: у dxgi, у
// Steam-оверлеї (gameoverlayrenderer64), у нас чи це переповнення стека
// (STACK_OVERFLOW = таки нескінченна рекурсія).
static LONG CALLBACK crash_logger(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW ||
        code == EXCEPTION_IN_PAGE_ERROR) {
        void* addr = ep->ExceptionRecord->ExceptionAddress;
        log("overlay(КРАШ): code=0x%08lx addr=%p модуль=%ls кадрів=%ld in_present=%ld busy=%ld",
            (unsigned long)code, addr, module_of(addr),
            g_frames, g_in_present, g_present_busy);
        // Стек виклику в момент краху — показує повторюваний цикл рекурсії
        // (які функції/модулі чергуються). Знімаємо раз, щоб не залити лог.
        static LONG once_bt = 0;
        if (InterlockedCompareExchange(&once_bt, 1, 0) == 0) {
            void* bt[30];
            USHORT k = RtlCaptureStackBackTrace(0, 30, bt, nullptr);
            for (USHORT i = 0; i < k; ++i)
                log("overlay(КРАШ-стек) #%02u %p %ls", i, bt[i], module_of(bt[i]));
        }
        if (code == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2) {
            log("overlay(КРАШ): AV %s за адресою %p",
                ep->ExceptionRecord->ExceptionInformation[0] ? "запис" : "читання",
                (void*)ep->ExceptionRecord->ExceptionInformation[1]);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Діагностика: як ПРЯМО ЗАРАЗ перехоплено Present (можливо, іншим оверлеєм —
// OBS). Нічого не змінює — лише пише в лог. За цим ми точно дізнаємось спосіб
// хука конкретної версії OBS: чи це підміна покажчика у vtable[8], чи інлайн-
// стрибок на початку тіла Present, і КУДИ він веде (у graphics-hook OBS?).
// Саме цього факту бракує, щоб полагодити порядок «OBS знімає чистий кадр →
// ми малюємо після» не наосліп.
static void diagnose_present(IDXGISwapChain* swap) {
    void** vt = *reinterpret_cast<void***>(swap);
    void* present = vt[8];
    log("overlay(діаг): OBS graphics-hook у процесі: %s",
        obs_capture_present() ? "так" : "ні");
    log("overlay(діаг): vtable[8] Present веде в %ls", module_of(present));
    uint8_t* p = reinterpret_cast<uint8_t*>(present);
    log("overlay(діаг): байти тіла Present: %02x %02x %02x %02x %02x %02x %02x %02x",
        p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
    if (p[0] == 0xE9) {
        int32_t rel = *reinterpret_cast<int32_t*>(p + 1);
        void* tgt = p + 5 + rel;
        log("overlay(діаг): на початку тіла інлайн-стрибок E9 -> %ls", module_of(tgt));
    } else if (p[0] == 0xFF && p[1] == 0x25) {
        int32_t disp = *reinterpret_cast<int32_t*>(p + 2);
        void* tgt = *reinterpret_cast<void**>(p + 6 + disp);
        log("overlay(діаг): на початку тіла інлайн-стрибок FF25 -> %ls", module_of(tgt));
    } else {
        log("overlay(діаг): тіло Present без інлайн-стрибка на початку "
            "(отже OBS хукає не інлайном — імовірно vtable)");
    }
}

// Виділяє блок ПОРУЧ із target (±2 ГБ), щоб перерахований RIP-операдний зсув у
// скопійованому пролозі дотягнувся до своєї цілі (на x64 disp32 обмежений ±2 ГБ).
static uint8_t* alloc_near_body(uint8_t* target) {
    const uint64_t GB2 = 0x60000000ULL;
    const uint64_t step = 0x10000ULL;
    uint64_t base = (uint64_t)target;
    for (uint64_t off = step; off < GB2; off += step) {
        for (int dir = 0; dir < 2; ++dir) {
            uint64_t addr = dir ? base + off : base - off;
            void* p = VirtualAlloc((void*)(addr & ~(step - 1)), 64,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) return (uint8_t*)p;
        }
    }
    return nullptr;
}

// Будує «чистий flip» — трамплін у справжній показ кадру В ОБХІД чужого inline-
// хука тіла Present. Чужий хук (Steam-оверлей) затирає перші байти тіла 5-байтним
// E9 на свій детур; оригінальні байти в памʼяті вже втрачені, але у ФАЙЛІ
// dxgi.dll на диску вони цілі. Читаємо пролог із файлу, копіюємо ЦІЛІ інструкції,
// поки не накриємо 5 байтів (та сама межа, куди чужий хук поклав свій E9), і
// будуємо [оригінальний пролог][далекий стрибок на тіло+N] — точну копію
// трампліна, який тримає для себе сам чужий хук. Виклик цього трампліна показує
// кадр, не заходячи в чужий детур, тож рекурсія Steam↔ми не виникає.
static PresentFn build_clean_flip(uint8_t* body) {
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) return nullptr;
    uint64_t rva = (uint64_t)body - (uint64_t)dxgi;

    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(dxgi, path, MAX_PATH)) return nullptr;
    HANDLE fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return nullptr;
    HANDLE mp = CreateFileMappingW(fh, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mp) { CloseHandle(fh); return nullptr; }
    uint8_t* file = reinterpret_cast<uint8_t*>(MapViewOfFile(mp, FILE_MAP_READ, 0, 0, 0));
    PresentFn result = nullptr;

    if (file) {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(file + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                // RVA → файловий зсув через таблицю секцій.
                IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
                uint32_t foff = 0;
                for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
                    uint32_t va = sec[i].VirtualAddress;
                    uint32_t vsz = sec[i].Misc.VirtualSize;
                    if (rva >= va && rva < va + vsz) {
                        foff = sec[i].PointerToRawData + (uint32_t)(rva - va);
                        break;
                    }
                }
                if (foff) {
                    const uint8_t* clean = file + foff;
                    int copied = 0, rip_at[8], rip_n = 0;
                    bool ok = true;
                    while (copied < 5) {
                        int off = -1;
                        int n = hominka::insn_len(clean + copied, &off);
                        if (n <= 0) { ok = false; break; }
                        if (off >= 0 && rip_n < 8) rip_at[rip_n++] = copied + off;
                        copied += n;
                    }
                    if (ok && copied <= 24) {
                        uint8_t* stub = alloc_near_body(body);
                        if (stub) {
                            memcpy(stub, clean, copied);
                            // Перерахунок RIP-відносних disp32: пролог тепер не за
                            // адресою body, а в stub — зсуваємо на різницю.
                            int64_t delta = (int64_t)body - (int64_t)stub;
                            bool reloc_ok = true;
                            for (int j = 0; j < rip_n; ++j) {
                                int32_t* d = reinterpret_cast<int32_t*>(stub + rip_at[j]);
                                int64_t nd = (int64_t)*d + delta;
                                if (nd < INT32_MIN || nd > INT32_MAX) { reloc_ok = false; break; }
                                *d = (int32_t)nd;
                            }
                            if (reloc_ok) {
                                // Далекий абсолютний стрибок на тіло+copied (за чужим хуком).
                                stub[copied] = 0xFF; stub[copied + 1] = 0x25;
                                *reinterpret_cast<uint32_t*>(stub + copied + 2) = 0;
                                *reinterpret_cast<uint64_t*>(stub + copied + 6) =
                                    reinterpret_cast<uint64_t>(body + copied);
                                FlushInstructionCache(GetCurrentProcess(), stub, copied + 14);
                                result = reinterpret_cast<PresentFn>(stub);
                            } else {
                                VirtualFree(stub, 0, MEM_RELEASE);
                            }
                        }
                    }
                }
            }
        }
        UnmapViewOfFile(file);
    }
    CloseHandle(mp);
    CloseHandle(fh);
    return result;
}

bool install_present_hook() {
    HWND hwnd = nullptr;
    IDXGISwapChain* swap = make_probe_swapchain(&hwnd, L"HominkaHookProbe");
    if (!swap) return false;

    // ПЕРЕД тим як щось чіпати — знімок того, як Present перехоплено зараз.
    diagnose_present(swap);

    bool ok = false;
    {
        void** vt = *reinterpret_cast<void***>(swap);
        uint8_t* body = reinterpret_cast<uint8_t*>(vt[8]);
        if (body && body[0] == 0xE9) {
            // Тіло Present уже під чужим inline-хуком (OBS graphics-hook і/або
            // Steam-оверлей). НЕ чіпаємо vtable: якби ми підмінили vtable[8] на
            // себе, то swap->Present() інших оверлеїв резолвився б у нас, і Steam
            // ішов би в нескінченну рекурсію (краш), а зняття чужого хука ламає
            // захоплення OBS. Замість цього стаємо ЛАНКОЮ inline-ланцюга просто на
            // тілі: гра→тіло→[ми→наступний детур→…]→dxgi. vtable лишається = тіло,
            // тож чужі swap->Present() йдуть звичним ЛІНІЙНИМ ланцюгом — без
            // рекурсії, а оверлей Steam і захоплення OBS лишаються робочими.
            g_clean_flip = build_clean_flip(body);   // страховка для реентранту
            if (g_present_inline.install(body, reinterpret_cast<void*>(&hooked_present))) {
                g_present_original = g_present_inline.original<PresentFn>();
                ok = true;
                log("overlay: тіло Present під чужим хуком — стаю ланкою inline-"
                    "ланцюга, vtable не чіпаю (обхідний flip %s)",
                    g_clean_flip ? "є" : "нема");
            } else {
                log("overlay: inline-хук тіла Present не вдався — відкат на vtable");
            }
        }
    }
    if (!ok) {
        ok = g_present_hook.install(swap, 8, reinterpret_cast<void*>(&hooked_present));
        if (ok) {
            g_present_original = g_present_hook.original<PresentFn>();
            log("overlay: адресу Present підмінено у спільній vtable");
            // Приховування від OBS у DX12 більше НЕ спирається на інлайн-хук тіла
            // Present (у DX12 порядок хуків Present ненадійний). Замість цього чат
            // кладеться після копії OBS через хук ExecuteCommandLists — див.
            // hooked_execute та overlay_dx12.h.
        }
    }

    // Хук GetBuffer на тій самій спільній DXGI-vtable — для приховування від OBS.
    install_getbuffer_hook(swap);

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

    bool draw = g_overlay9.prepare(device, obs_capture_present());
    if (draw && g_overlay9.wants_hide()) {
        // Ховаємо від OBS — усе БЕЗ своєї сцени (див. overlay_dx9.h):
        //  А) знімок чистого кадру (RT на мить убік → StretchRect → назад);
        //  Б) чат у сцену ГРИ (як звичайний показ);
        //  В) original EndScene; Г) знімок кадру з чатом (поза сценою).
        // Копію OBS обгорнемо в hooked_d9_stretchrect/getrtdata.
        InterlockedExchange(&g_d9_wrapping, 1);   // наші StretchRect'и — не копія OBS
        g_overlay9.hide_snapshot_clean(device);
        InterlockedExchange(&g_d9_wrapping, 0);
        g_overlay9.draw(device);                  // чат у сцену гри
        HRESULT r = g_endscene_original(device);
        InterlockedExchange(&g_d9_wrapping, 1);
        g_overlay9.hide_snapshot_dirty(device);
        InterlockedExchange(&g_d9_wrapping, 0);
        return r;
    }
    if (draw) g_overlay9.draw(device);            // показ: чат у сцені гри
    return g_endscene_original(device);
}

// OBS копіює бекбуфер (джерело == бекбуфер) — обгортаємо: чистий → копія → з чатом.
static bool d9_is_obs_copy(IDirect3DSurface9* src) {
    return src && !g_d9_wrapping && g_overlay9.wants_hide()
        && src == g_overlay9.obs_backbuffer();
}

HRESULT STDMETHODCALLTYPE hooked_d9_stretchrect(IDirect3DDevice9* dev,
        IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst,
        const RECT* dr, D3DTEXTUREFILTERTYPE filter) {
    if (d9_is_obs_copy(src)) {
        InterlockedExchange(&g_d9_wrapping, 1);
        g_overlay9.obs_copy_before(dev, src);
        HRESULT r = g_d9_stretch_orig(dev, src, sr, dst, dr, filter);
        g_overlay9.obs_copy_after(dev, src);
        InterlockedExchange(&g_d9_wrapping, 0);
        return r;
    }
    return g_d9_stretch_orig(dev, src, sr, dst, dr, filter);
}

HRESULT STDMETHODCALLTYPE hooked_d9_getrtdata(IDirect3DDevice9* dev,
        IDirect3DSurface9* src, IDirect3DSurface9* dst) {
    if (d9_is_obs_copy(src)) {
        InterlockedExchange(&g_d9_wrapping, 1);
        g_overlay9.obs_copy_before(dev, src);
        HRESULT r = g_d9_getrtdata_orig(dev, src, dst);
        g_overlay9.obs_copy_after(dev, src);
        InterlockedExchange(&g_d9_wrapping, 0);
        return r;
    }
    return g_d9_getrtdata_orig(dev, src, dst);
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
    CreateFn create = (CreateFn)(void*)GetProcAddress(d3d9, "Direct3DCreate9");
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
        // Для приховування від OBS: StretchRect — індекс 34, GetRenderTargetData —
        // 32. Пролог не піддався — не біда: цей шлях копії OBS не обгортається.
        if (g_d9_stretch_hook.install(vt[34], reinterpret_cast<void*>(&hooked_d9_stretchrect)))
            g_d9_stretch_orig = g_d9_stretch_hook.original<StretchRectFn>();
        if (g_d9_getrtdata_hook.install(vt[32], reinterpret_cast<void*>(&hooked_d9_getrtdata)))
            g_d9_getrtdata_orig = g_d9_getrtdata_hook.original<GetRTDataFn>();
        log("overlay(dx9): EndScene перехоплено (приховування OBS: StretchRect=%s GetRTData=%s)",
            g_d9_stretch_orig ? "так" : "ні", g_d9_getrtdata_orig ? "так" : "ні");
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
    // Якщо в процесі вже активний наш Vulkan-шар (hominka-vklayer), то це
    // Vulkan-гра, і чат у ній малює ШАР. Інжект-хуки тут не потрібні й НЕБЕЗПЕЧНІ:
    // два оверлеї в одному процесі конфліктують і роняли гру. Тож нічого не
    // чіпаємо — хай малює шар (Hominka все одно ввімкне продюсера й поставить
    // target_pid при інжекті, а шар його підхопить).
    if (GetModuleHandleW(L"hominka-vklayer-x64.dll") ||
        GetModuleHandleW(L"hominka-vklayer-x86.dll")) {
        log("overlay: у процесі активний Vulkan-шар Hominka — інжект-хуки НЕ ставлю "
            "(малює шар; уникаємо конфлікту й краху)");
        return 0;
    }
    // Логгер краху ставимо ПЕРШИМ — щоб зловити навіть падіння під час установки
    // хуків. Він лише пише в лог і пропускає виняток далі.
    AddVectoredExceptionHandler(1, crash_logger);
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
            // install_present_hook сам знешкодить графічний хук Steam на тілі
            // Present (якщо є), перш ніж ставити свій, — інакше детур Steam-оверлея
            // на DX11 йде в нескінченну рекурсію й гра падає.
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
        g_present_hook.remove();
        g_present_inline.remove();
        g_getbuffer_hook.remove();
        g_exec_hook.remove();
        g_d9_stretch_hook.remove();
        g_d9_getrtdata_hook.remove();
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
