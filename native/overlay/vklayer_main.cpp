// hominka-vklayer — імпліцитний шар Vulkan, що малює чат у грі.
//
// Навіщо шар, а не інжект: гра ініціалізує Vulkan (пристрій, свопчейн, адреси
// функцій) на самому старті процесу — раніше, ніж встигає наш ручний інжект.
// Тому перехоплення vkGetDeviceProcAddr «ззовні» майже завжди спізнюється, і
// хуки стоять вхолосту. Офіційний механізм Khronos — імпліцитний шар: завантажувач
// Vulkan сам вставляє нас у ЛАНЦЮГ КОЖНОГО Vulkan-застосунку ДО ініціалізації.
// Так проблема «пізнього інжекту» зникає, і ми чесно стоїмо поряд з іншими шарами
// (OBS, Steam), без конфліктів стеку.
//
// Активуємося лише коли для цього процесу увімкнено чат (спільна памʼять + збіг
// target_pid — усе в OverlayVK). У чужих Vulkan-застосунках шар нічого не малює.
//
// Малювання й уся Vulkan-механіка — у overlay_vk.h (OverlayVK), той самий код, що
// й в інжект-версії. Тут — лише плюмбінг шару: узгодження інтерфейсу з
// завантажувачем, dispatch-таблиці й ланцюг викликів.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <vulkan/vk_layer.h>

#include <windows.h>
#include <unordered_map>

#include "../common/log.h"
#include "../common/inline_hook.h"
#include "overlay_vk.h"

using hominka::log;

namespace {

hominka::OverlayVK g_vk;
// Фізичнодевайсна функція, резолвлена через dispatch-цепочку шару (не з експорту
// vulkan-1.dll) — інакше виклик із нашим phys повисає. Заповнюємо в CreateInstance.
PFN_vkGetPhysicalDeviceMemoryProperties g_pdmp = nullptr;
CRITICAL_SECTION g_cs;
bool g_cs_init = false;
struct Lock {
    Lock() { EnterCriticalSection(&g_cs); }
    ~Lock() { LeaveCriticalSection(&g_cs); }
};

// Ключ dispatch — перший QWORD дороговказного обʼєкта (так робить завантажувач і
// всі шари): різні пристрої/інстанси з тим самим ключем ділять таблицю.
static void* disp_key(void* handle) { return *reinterpret_cast<void**>(handle); }

// Логгер краху — не глушить, лише пише адресу/модуль фолту (де саме валиться
// малювання слоя). Ставимо раз.
static const wchar_t* module_of(void* addr) {
    static wchar_t name[MAX_PATH];
    HMODULE mod = nullptr;
    if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)addr, &mod) && mod) {
        wchar_t path[MAX_PATH];
        if (GetModuleFileNameW(mod, path, MAX_PATH)) {
            const wchar_t* b = wcsrchr(path, L'\\');
            wcsncpy(name, b ? b + 1 : path, MAX_PATH - 1); name[MAX_PATH - 1] = 0;
            return name;
        }
    }
    return L"(поза модулями)";
}
static LONG CALLBACK crash_logger(EXCEPTION_POINTERS* ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_ILLEGAL_INSTRUCTION ||
        c == EXCEPTION_PRIV_INSTRUCTION || c == EXCEPTION_STACK_OVERFLOW) {
        void* a = ep->ExceptionRecord->ExceptionAddress;
        log("overlay(vk-шар КРАШ): code=0x%08lx addr=%p модуль=%ls",
            (unsigned long)c, a, module_of(a));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void install_crash_logger() {
    static bool once = false;
    if (!once) { once = true; AddVectoredExceptionHandler(1, crash_logger); }
}

struct InstanceData {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance destroy = nullptr;
};
struct DeviceData {
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkQueuePresentKHR present = nullptr;
    PFN_vkGetDeviceQueue get_queue = nullptr;
    PFN_vkCreateSwapchainKHR create_swap = nullptr;
    PFN_vkDestroySwapchainKHR destroy_swap = nullptr;
    PFN_vkDestroyDevice destroy = nullptr;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
};
std::unordered_map<void*, InstanceData> g_instances;
std::unordered_map<void*, DeviceData> g_devices;

DeviceData* dev_data(void* h) {
    Lock lk_;
    auto it = g_devices.find(disp_key(h));
    return it == g_devices.end() ? nullptr : &it->second;
}

// Знаходить у ланцюзі pNext ланку зв'язку шару (де лежать наступні GIPA/GDPA).
static VkLayerInstanceCreateInfo* find_inst_link(const VkInstanceCreateInfo* ci) {
    auto* p = (VkLayerInstanceCreateInfo*)ci->pNext;
    while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                  p->function == VK_LAYER_LINK_INFO))
        p = (VkLayerInstanceCreateInfo*)p->pNext;
    return p;
}
static VkLayerDeviceCreateInfo* find_dev_link(const VkDeviceCreateInfo* ci) {
    auto* p = (VkLayerDeviceCreateInfo*)ci->pNext;
    while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                  p->function == VK_LAYER_LINK_INFO))
        p = (VkLayerDeviceCreateInfo*)p->pNext;
    return p;
}

PFN_vkVoidFunction VKAPI_CALL layer_gdpa(VkDevice device, const char* name);
PFN_vkVoidFunction VKAPI_CALL layer_gipa(VkInstance instance, const char* name);

// --- скрытие від OBS: інлайн-хук справжньої vkCmdCopyImage ---
// OBS у своєму QueuePresentKHR копіює образ свопчейна через vkCmdCopyImage. Порядок
// імпліцитних шарів у Vulkan недетермінований, тож перехопити копію OBS через
// dispatch не можна надійно. Інлайн-хук ловить саму функцію драйвера — незалежно
// від порядку. Коли OBS копіює образ свопчейна й увімкнено скрытие — підміняємо
// джерело на наш чистий знімок.
typedef void (VKAPI_PTR *CmdCopyImageFn)(VkCommandBuffer, VkImage, VkImageLayout,
        VkImage, VkImageLayout, uint32_t, const VkImageCopy*);
hominka::InlineHook g_copyimg_hook;
CmdCopyImageFn g_copyimg_orig = nullptr;

void VKAPI_PTR hooked_vkCmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl,
        VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy* r) {
    if (!g_vk.recording_snapshot() && g_vk.hiding() && g_vk.is_swap_image(src)) {
        VkImage clean = g_vk.clean_for(src);
        if (clean) {
            static LONG c = 0;
            LONG k = InterlockedIncrement(&c);
            if (k == 1 || (k % 600) == 0)
                log("overlay(vk-шар): копію OBS підмінено на чистий знімок, разів=%ld", k);
            g_copyimg_orig(cb, clean, sl, dst, dl, n, r);
            return;
        }
    }
    g_copyimg_orig(cb, src, sl, dst, dl, n, r);
}

// --- перехоплені функції ---

VkResult VKAPI_CALL layer_CreateInstance(const VkInstanceCreateInfo* ci,
        const VkAllocationCallbacks* al, VkInstance* out) {
    VkLayerInstanceCreateInfo* link = find_inst_link(ci);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    // Просуваємо ланку для наступного шару в ланцюзі.
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    install_crash_logger();
    auto create = (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(ci, al, out);
    if (r != VK_SUCCESS) return r;

    InstanceData d;
    d.gipa = next_gipa;
    d.destroy = (PFN_vkDestroyInstance)next_gipa(*out, "vkDestroyInstance");
    // Резолвимо фізичнодевайсну функцію через dispatch інстансу (не з vulkan-1).
    g_pdmp = (PFN_vkGetPhysicalDeviceMemoryProperties)
             next_gipa(*out, "vkGetPhysicalDeviceMemoryProperties");
    {
        Lock lk_;
        g_instances[disp_key(*out)] = d;
    }
    log("overlay(vk-шар): CreateInstance — шар у ланцюзі");
    return VK_SUCCESS;
}

void VKAPI_CALL layer_DestroyInstance(VkInstance inst, const VkAllocationCallbacks* al) {
    PFN_vkDestroyInstance destroy = nullptr;
    {
        Lock lk_;
        auto it = g_instances.find(disp_key(inst));
        if (it != g_instances.end()) { destroy = it->second.destroy; g_instances.erase(it); }
    }
    if (destroy) destroy(inst, al);
}

VkResult VKAPI_CALL layer_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
        const VkAllocationCallbacks* al, VkDevice* out) {
    VkLayerDeviceCreateInfo* link = find_dev_link(ci);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    auto create = (PFN_vkCreateDevice)next_gipa(VK_NULL_HANDLE, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(phys, ci, al, out);
    if (r != VK_SUCCESS) return r;

    DeviceData d;
    d.gdpa = next_gdpa;
    d.present = (PFN_vkQueuePresentKHR)next_gdpa(*out, "vkQueuePresentKHR");
    d.get_queue = (PFN_vkGetDeviceQueue)next_gdpa(*out, "vkGetDeviceQueue");
    d.create_swap = (PFN_vkCreateSwapchainKHR)next_gdpa(*out, "vkCreateSwapchainKHR");
    d.destroy_swap = (PFN_vkDestroySwapchainKHR)next_gdpa(*out, "vkDestroySwapchainKHR");
    d.destroy = (PFN_vkDestroyDevice)next_gdpa(*out, "vkDestroyDevice");
    d.phys = phys;
    {
        Lock lk_;
        g_devices[disp_key(*out)] = d;
    }

    // Готуємо OverlayVK. Функції-ресурси він вантажить із vulkan-1.dll (експорти
    // завантажувача) — для НЕ present-функцій це безпечно й простіше за передачу
    // всієї dispatch-таблиці. Present-ланцюг тримаємо тут (d.present).
    HMODULE vk = GetModuleHandleW(L"vulkan-1.dll");
    if (vk) {
        Lock lk_;
        if (g_vk.load(vk)) {
            g_vk.on_device(phys, *out);
            g_vk.set_phys_mem_fn(g_pdmp);   // коректна фізичнодевайсна функція
            // Інлайн-хук справжньої vkCmdCopyImage — щоб ловити копію OBS для
            // скрытия незалежно від порядку шарів. Адресу беремо з dispatch-цепочки
            // шару (next-GDPA), а НЕ з loader-export (той не знає наш device →
            // повертав null). Ставимо раз.
            if (!g_copyimg_hook.installed()) {
                void* addr = (void*)next_gdpa(*out, "vkCmdCopyImage");
                if (addr && g_copyimg_hook.install(addr, (void*)&hooked_vkCmdCopyImage)) {
                    g_copyimg_orig = g_copyimg_hook.original<CmdCopyImageFn>();
                    log("overlay(vk-шар): інлайн-хук vkCmdCopyImage поставлено (скрытие від OBS)");
                } else {
                    log("overlay(vk-шар): інлайн-хук vkCmdCopyImage НЕ поставлено (addr=%p) — "
                        "скрытие від OBS недоступне", addr);
                }
            }
            log("overlay(vk-шар): CreateDevice — пристрій захоплено, малювання готове "
                "(pdmp=%s)", g_pdmp ? "є" : "нема");
        } else {
            log("overlay(vk-шар): OverlayVK.load не вдалося");
        }
    }
    return VK_SUCCESS;
}

void VKAPI_CALL layer_DestroyDevice(VkDevice dev, const VkAllocationCallbacks* al) {
    PFN_vkDestroyDevice destroy = nullptr;
    {
        Lock lk_;
        auto it = g_devices.find(disp_key(dev));
        if (it != g_devices.end()) { destroy = it->second.destroy; g_devices.erase(it); }
    }
    g_copyimg_hook.remove();
    { Lock lk_; g_vk.release(); }
    if (destroy) destroy(dev, al);
}

void VKAPI_CALL layer_GetDeviceQueue(VkDevice dev, uint32_t fam, uint32_t idx, VkQueue* q) {
    DeviceData* d = dev_data(dev);
    if (!d || !d->get_queue) return;
    d->get_queue(dev, fam, idx, q);
    if (q && *q) {
        Lock lk_;
        g_vk.on_queue(*q, fam);
    }
}

VkResult VKAPI_CALL layer_CreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
        const VkAllocationCallbacks* al, VkSwapchainKHR* sc) {
    DeviceData* d = dev_data(dev);
    if (!d || !d->create_swap) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->create_swap(dev, ci, al, sc);
    if (r == VK_SUCCESS && sc && *sc) {
        Lock lk_;
        g_vk.on_swapchain(*sc, ci);
    }
    return r;
}

void VKAPI_CALL layer_DestroySwapchainKHR(VkDevice dev, VkSwapchainKHR sc,
        const VkAllocationCallbacks* al) {
    DeviceData* d = dev_data(dev);
    if (sc) { Lock lk_; g_vk.on_swapchain_destroy(sc); }
    if (d && d->destroy_swap) d->destroy_swap(dev, sc, al);
}

VkResult VKAPI_CALL layer_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pi) {
    DeviceData* d = dev_data(queue);
    if (!d || !d->present) return VK_ERROR_INITIALIZATION_FAILED;

    static bool logged = false;
    if (!logged) { logged = true; log("overlay(vk-шар): перший vkQueuePresentKHR — малюємо чат"); }

    VkPresentInfoKHR local;
    std::vector<VkSemaphore> wait_store;
    { Lock lk_;
      g_vk.on_present(queue, pi, &local, &wait_store); }
    return d->present(queue, &local);
}

// --- роздача адрес (GDPA/GIPA) ---

#define IF_NAME(fn) if (!strcmp(name, "vk" #fn)) return (PFN_vkVoidFunction)&layer_##fn

PFN_vkVoidFunction VKAPI_CALL layer_gdpa(VkDevice device, const char* name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)&layer_gdpa;
    IF_NAME(DestroyDevice);
    IF_NAME(GetDeviceQueue);
    IF_NAME(CreateSwapchainKHR);
    IF_NAME(DestroySwapchainKHR);
    IF_NAME(QueuePresentKHR);
    DeviceData* d = dev_data(device);
    if (d && d->gdpa) return d->gdpa(device, name);
    return nullptr;
}

PFN_vkVoidFunction VKAPI_CALL layer_gipa(VkInstance instance, const char* name) {
    // Наш власний GIPA + функції рівня інстансу/пристрою, що ми перехоплюємо.
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)&layer_gipa;
    IF_NAME(CreateInstance);
    IF_NAME(DestroyInstance);
    IF_NAME(CreateDevice);
    // Пристроєві імена теж можуть питати через GIPA — віддаємо ті самі детури.
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)&layer_gdpa;
    IF_NAME(DestroyDevice);
    IF_NAME(GetDeviceQueue);
    IF_NAME(CreateSwapchainKHR);
    IF_NAME(DestroySwapchainKHR);
    IF_NAME(QueuePresentKHR);
    if (!instance) return nullptr;
    InstanceData* id = nullptr;
    { Lock lk_;
      auto it = g_instances.find(disp_key(instance));
      if (it != g_instances.end()) id = &it->second; }
    if (id && id->gipa) return id->gipa(instance, name);
    return nullptr;
}

#undef IF_NAME

}  // namespace

// --- узгодження інтерфейсу із завантажувачем ---
extern "C" __declspec(dllexport)
VkResult VKAPI_CALL HominkaVkNegotiate(VkNegotiateLayerInterface* v) {
    if (v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = layer_gipa;
    v->pfnGetDeviceProcAddr = layer_gdpa;
    v->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

// Резервний експорт на випадок старішого завантажувача, що шукає GIPA прямо.
extern "C" __declspec(dllexport)
PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance inst, const char* name) {
    return layer_gipa(inst, name);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH && !g_cs_init) {
        InitializeCriticalSection(&g_cs);
        g_cs_init = true;
    }
    return TRUE;
}
