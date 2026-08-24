// Крихітна гра-макет на Vulkan — для перевірки кроку 2 у Vulkan (DOOM,
// RDR2 у Vulkan-режимі, емулятори тощо). Вікно, інстанс, поверхня, пристрій,
// свопчейн; у циклі: acquire → сабміт (очистка кадру) → present. Наш overlay.dll
// вклинюється в vkQueuePresentKHR і домальовує чат. У випуск не входить.
//
// Функції Vulkan вантажимо динамічно (як і оверлей) — без лінкування з
// vulkan-1, бо mingw імпортної бібліотеки не має.
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>
#include <vector>
#include <stdio.h>

#define L(n) PFN_##n n = nullptr
L(vkGetInstanceProcAddr); L(vkCreateInstance); L(vkGetDeviceProcAddr);
L(vkCreateWin32SurfaceKHR); L(vkEnumeratePhysicalDevices);
L(vkGetPhysicalDeviceQueueFamilyProperties); L(vkGetPhysicalDeviceSurfaceSupportKHR);
L(vkGetPhysicalDeviceSurfaceFormatsKHR); L(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
L(vkCreateDevice); L(vkGetDeviceQueue); L(vkCreateSwapchainKHR); L(vkGetSwapchainImagesKHR);
L(vkCreateCommandPool); L(vkAllocateCommandBuffers); L(vkCreateSemaphore); L(vkCreateFence);
L(vkAcquireNextImageKHR); L(vkBeginCommandBuffer); L(vkCmdPipelineBarrier);
L(vkCmdClearColorImage); L(vkEndCommandBuffer); L(vkQueueSubmit); L(vkQueuePresentKHR);
L(vkWaitForFences); L(vkResetFences); L(vkResetCommandBuffer); L(vkDeviceWaitIdle);
#undef L

static VkInstance g_inst;
static VkPhysicalDevice g_phys;
static VkDevice g_dev;
static VkQueue g_queue;
static uint32_t g_family;
static VkSurfaceKHR g_surf;
static VkSwapchainKHR g_swap;
static VkFormat g_fmt;
static VkExtent2D g_ext;
static std::vector<VkImage> g_imgs;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

template<class T> void load(T& fn, void* ctx, const char* name, bool dev) {
    fn = (T)(dev ? vkGetDeviceProcAddr((VkDevice)ctx, name)
                 : vkGetInstanceProcAddr((VkInstance)ctx, name));
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    HMODULE vk = LoadLibraryW(L"vulkan-1.dll");
    if (!vk) { MessageBoxW(0, L"no vulkan-1.dll", L"testhost", 0); return 1; }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");

    // Затримка ПЕРЕД ініціалізацією Vulkan: щоб під час перевірки встигнути
    // заінжектити оверлей до того, як гра створить інстанс/пристрій/свопчейн.
    // Саме так оверлей і має потрапляти в Vulkan-гру (інжект до старту гри).
    char delay[32] = {0};
    if (GetEnvironmentVariableA("HOMINKA_VK_DELAY_MS", delay, sizeof(delay)) > 0) {
        int ms = 0; for (char* p = delay; *p >= '0' && *p <= '9'; ++p) ms = ms*10 + (*p - '0');
        Sleep(ms);
    }
    vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(nullptr, "vkCreateInstance");

    WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc;
    wc.hInstance = inst; wc.lpszClassName = L"HominkaTestHostVK";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Hominka test host (Vulkan)",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 500,
                               NULL, NULL, inst, NULL);
    ShowWindow(hwnd, show);

    const char* iext[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai; ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = iext;
    if (vkCreateInstance(&ici, nullptr, &g_inst) != VK_SUCCESS) { MessageBoxW(0,L"no instance",L"t",0); return 1; }

    #define LI(n) load(n, g_inst, #n, false)
    LI(vkCreateWin32SurfaceKHR); LI(vkEnumeratePhysicalDevices);
    LI(vkGetPhysicalDeviceQueueFamilyProperties); LI(vkGetPhysicalDeviceSurfaceSupportKHR);
    LI(vkGetPhysicalDeviceSurfaceFormatsKHR); LI(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LI(vkCreateDevice); LI(vkGetDeviceProcAddr);
    #undef LI

    VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = inst; sci.hwnd = hwnd;
    vkCreateWin32SurfaceKHR(g_inst, &sci, nullptr, &g_surf);

    uint32_t np = 0; vkEnumeratePhysicalDevices(g_inst, &np, nullptr);
    std::vector<VkPhysicalDevice> phys(np); vkEnumeratePhysicalDevices(g_inst, &np, phys.data());
    for (auto pd : phys) {
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf.data());
        for (uint32_t i = 0; i < nq; ++i) {
            VkBool32 present = 0; vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, g_surf, &present);
            if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                g_phys = pd; g_family = i; break;
            }
        }
        if (g_phys) break;
    }
    if (!g_phys) { MessageBoxW(0,L"no gfx+present queue",L"t",0); return 1; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_family; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char* dext[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dext;
    if (vkCreateDevice(g_phys, &dci, nullptr, &g_dev) != VK_SUCCESS) { MessageBoxW(0,L"no device",L"t",0); return 1; }

    #define LD(n) load(n, g_dev, #n, true)
    LD(vkGetDeviceQueue); LD(vkCreateSwapchainKHR); LD(vkGetSwapchainImagesKHR);
    LD(vkCreateCommandPool); LD(vkAllocateCommandBuffers); LD(vkCreateSemaphore); LD(vkCreateFence);
    LD(vkAcquireNextImageKHR); LD(vkBeginCommandBuffer); LD(vkCmdPipelineBarrier);
    LD(vkCmdClearColorImage); LD(vkEndCommandBuffer); LD(vkQueueSubmit); LD(vkQueuePresentKHR);
    LD(vkWaitForFences); LD(vkResetFences); LD(vkResetCommandBuffer); LD(vkDeviceWaitIdle);
    #undef LD
    vkGetDeviceQueue(g_dev, g_family, 0, &g_queue);

    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_phys, g_surf, &caps);
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surf, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nf); vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surf, &nf, fmts.data());
    g_fmt = fmts[0].format; VkColorSpaceKHR cs = fmts[0].colorSpace;
    for (auto& f : fmts) if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { g_fmt = f.format; cs = f.colorSpace; break; }
    g_ext = caps.currentExtent.width == 0xFFFFFFFF ? VkExtent2D{800,500} : caps.currentExtent;

    VkSwapchainCreateInfoKHR swci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swci.surface = g_surf; swci.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
    swci.imageFormat = g_fmt; swci.imageColorSpace = cs; swci.imageExtent = g_ext;
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = VK_PRESENT_MODE_FIFO_KHR; swci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(g_dev, &swci, nullptr, &g_swap) != VK_SUCCESS) { MessageBoxW(0,L"no swapchain",L"t",0); return 1; }
    uint32_t ni = 0; vkGetSwapchainImagesKHR(g_dev, g_swap, &ni, nullptr);
    g_imgs.resize(ni); vkGetSwapchainImagesKHR(g_dev, g_swap, &ni, g_imgs.data());

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = g_family;
    VkCommandPool pool; vkCreateCommandPool(g_dev, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cb; vkAllocateCommandBuffers(g_dev, &cbi, &cb);
    VkSemaphoreCreateInfo sem = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquired, rendered; vkCreateSemaphore(g_dev,&sem,nullptr,&acquired); vkCreateSemaphore(g_dev,&sem,nullptr,&rendered);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence; vkCreateFence(g_dev, &fci, nullptr, &fence);

    MSG msg = {};
    int lost = 0;
    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); continue; }

        vkWaitForFences(g_dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(g_dev, 1, &fence);
        uint32_t idx = 0;
        VkResult ar = vkAcquireNextImageKHR(g_dev, g_swap, UINT64_MAX, acquired, VK_NULL_HANDLE, &idx);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { Sleep(8); vkResetFences(g_dev,0,nullptr); continue; }

        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = g_imgs[idx]; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,0,nullptr,0,nullptr,1,&b);
        VkClearColorValue col = {}; col.float32[0]=0.05f; col.float32[1]=0.07f; col.float32[2]=0.15f; col.float32[3]=1.0f;
        VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(cb, g_imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &rng);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,0,nullptr,0,nullptr,1,&b);
        vkEndCommandBuffer(cb);

        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo su = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        su.waitSemaphoreCount = 1; su.pWaitSemaphores = &acquired; su.pWaitDstStageMask = &ws;
        su.commandBufferCount = 1; su.pCommandBuffers = &cb;
        su.signalSemaphoreCount = 1; su.pSignalSemaphores = &rendered;
        vkQueueSubmit(g_queue, 1, &su, fence);

        VkPresentInfoKHR pr = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pr.waitSemaphoreCount = 1; pr.pWaitSemaphores = &rendered;
        pr.swapchainCount = 1; pr.pSwapchains = &g_swap; pr.pImageIndices = &idx;
        VkResult pres = vkQueuePresentKHR(g_queue, &pr);   // сюди вклиниться overlay
        if (pres == VK_ERROR_DEVICE_LOST) { lost++; if (lost > 3) break; }
        Sleep(8);
    }
    vkDeviceWaitIdle(g_dev);
    return 0;
}
