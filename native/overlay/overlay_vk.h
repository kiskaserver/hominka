// Малювання кадру чату поверх кадру гри для Vulkan.
//
// Vulkan не має «одного Present у vtable» як DXGI — гра сама керує чергою,
// свопчейном і синхронізацією. Тому чіпляємось до ПЛОСКИХ експортів
// vulkan-1.dll (їх усі проксіює завантажувач Vulkan): vkCreateDevice —
// запам'ятати пристрій, vkGetDeviceQueue — родину черги, vkCreateSwapchainKHR
// — формат/розмір/образи свопчейна, vkQueuePresentKHR — сам показ, де ми і
// домальовуємо чат.
//
// Найтонше — синхронізація. Present чекає на семафори, які подала гра після
// свого рендера. Ми вставляємо СВІЙ сабміт МІЖ рендером гри й показом: наш
// сабміт чекає ті самі семафори, а показу підсовуємо чекати вже НАШ семафор.
// Так кадр чату гарантовано лягає поверх готового кадру гри.
//
// Функції Vulkan НЕ лінкуємо (VK_NO_PROTOTYPES) — вантажимо в рантаймі з
// vulkan-1.dll, бо DLL має працювати і там, де Vulkan SDK немає.
#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include <windows.h>
#include <vector>
#include <unordered_map>

#include "../common/log.h"
#include "shared_frame_reader.h"
#include "vk_vert_spv.h"
#include "vk_frag_spv.h"

namespace hominka {

class OverlayVK {
public:
    // Завантажує потрібні функції з vulkan-1.dll. Викликається раз при встановленні
    // хука. Повертає false, якщо чогось критичного немає.
    bool load(HMODULE vk) {
        gipa_ = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(vk, "vkGetInstanceProcAddr");
        if (!gipa_) return false;
        #define VKL(name) name##_ = (PFN_##name)(void*)GetProcAddress(vk, #name)
        VKL(vkGetDeviceProcAddr);
        VKL(vkGetPhysicalDeviceMemoryProperties);
        VKL(vkGetSwapchainImagesKHR);
        VKL(vkCreateImageView); VKL(vkDestroyImageView);
        VKL(vkCreateFramebuffer); VKL(vkDestroyFramebuffer);
        VKL(vkCreateRenderPass); VKL(vkDestroyRenderPass);
        VKL(vkCreateShaderModule); VKL(vkDestroyShaderModule);
        VKL(vkCreatePipelineLayout); VKL(vkDestroyPipelineLayout);
        VKL(vkCreateGraphicsPipelines); VKL(vkDestroyPipeline);
        VKL(vkCreateDescriptorSetLayout); VKL(vkDestroyDescriptorSetLayout);
        VKL(vkCreateDescriptorPool); VKL(vkDestroyDescriptorPool);
        VKL(vkAllocateDescriptorSets); VKL(vkUpdateDescriptorSets);
        VKL(vkCreateSampler); VKL(vkDestroySampler);
        VKL(vkCreateImage); VKL(vkDestroyImage);
        VKL(vkAllocateMemory); VKL(vkFreeMemory);
        VKL(vkBindImageMemory); VKL(vkGetImageMemoryRequirements);
        VKL(vkCreateBuffer); VKL(vkDestroyBuffer);
        VKL(vkGetBufferMemoryRequirements); VKL(vkBindBufferMemory);
        VKL(vkMapMemory); VKL(vkUnmapMemory);
        VKL(vkCreateCommandPool); VKL(vkDestroyCommandPool);
        VKL(vkAllocateCommandBuffers); VKL(vkFreeCommandBuffers);
        VKL(vkBeginCommandBuffer); VKL(vkEndCommandBuffer);
        VKL(vkCmdBeginRenderPass); VKL(vkCmdEndRenderPass);
        VKL(vkCmdBindPipeline); VKL(vkCmdBindDescriptorSets);
        VKL(vkCmdSetViewport); VKL(vkCmdSetScissor);
        VKL(vkCmdPushConstants); VKL(vkCmdDraw);
        VKL(vkCmdPipelineBarrier); VKL(vkCmdCopyBufferToImage);
        VKL(vkCreateSemaphore); VKL(vkDestroySemaphore);
        VKL(vkCreateFence); VKL(vkDestroyFence);
        VKL(vkWaitForFences); VKL(vkResetFences);
        VKL(vkResetCommandBuffer); VKL(vkQueueSubmit);
        VKL(vkDeviceWaitIdle);
        #undef VKL
        return vkGetDeviceProcAddr_ && vkCreateRenderPass_ && vkQueueSubmit_ &&
               vkCreateGraphicsPipelines_ && vkCmdDraw_;
    }

    // --- хуки віддають сюди захоплений стан ---

    void on_device(VkPhysicalDevice phys, VkDevice dev) {
        phys_ = phys; dev_ = dev;
    }
    void on_queue(VkQueue q, uint32_t family) {
        queue_family_[q] = family;
    }
    void on_swapchain(VkSwapchainKHR sc, const VkSwapchainCreateInfoKHR* ci) {
        SwapData s;
        s.format = ci->imageFormat;
        s.extent = ci->imageExtent;
        swaps_[sc] = s;   // образи/каркаси створимо лінькувато при показі
    }
    void on_swapchain_destroy(VkSwapchainKHR sc) {
        auto it = swaps_.find(sc);
        if (it != swaps_.end()) { destroy_swap(it->second); swaps_.erase(it); }
    }

    // Головне: домалювати чат і, за потреби, перенаправити семафори показу.
    // pi — оригінальний PresentInfo; out — копія, яку віддамо справжньому
    // vkQueuePresentKHR (у ній можемо підмінити семафори очікування).
    void on_present(VkQueue q, const VkPresentInfoKHR* pi, VkPresentInfoKHR* out,
                    std::vector<VkSemaphore>* wait_store) {
        *out = *pi;
        if (!dev_ || !ensure_common(q)) return;
        if (!read_frame()) return;          // нема кадру / вимкнено / не наш процес
        if (pi->swapchainCount == 0) return;

        // Малюємо на кожному свопчейні, який показують. Наш сабміт чекає ті
        // семафори, що й показ, і сигналить власний; показ перенаправляємо на
        // наш семафор. Для звичайної гри свопчейн один.
        VkSemaphore signalled = VK_NULL_HANDLE;
        for (uint32_t i = 0; i < pi->swapchainCount; ++i) {
            VkSwapchainKHR sc = pi->pSwapchains[i];
            uint32_t idx = pi->pImageIndices[i];
            auto it = swaps_.find(sc);
            if (it == swaps_.end()) continue;
            if (!ensure_swap(it->second, sc)) continue;
            // Наш сабміт для першого свопчейна чекає семафори показу; далі —
            // ланцюжком через наш семафор.
            const VkSemaphore* waits = (i == 0) ? pi->pWaitSemaphores : &signalled;
            uint32_t waitCount = (i == 0) ? pi->waitSemaphoreCount : (signalled ? 1u : 0u);
            VkSemaphore sig = draw_swap(q, it->second, idx, waits, waitCount);
            if (sig) signalled = sig;
        }
        if (signalled) {
            wait_store->assign(1, signalled);
            out->waitSemaphoreCount = 1;
            out->pWaitSemaphores = wait_store->data();
        }
    }

    void release() {
        if (!dev_) return;
        if (vkDeviceWaitIdle_) vkDeviceWaitIdle_(dev_);
        for (auto& kv : swaps_) destroy_swap(kv.second);
        swaps_.clear();
        destroy_common();
        dev_ = VK_NULL_HANDLE;
    }

private:
    struct SwapData {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent = {0, 0};
        VkRenderPass rpass = VK_NULL_HANDLE;
        std::vector<VkImageView> views;
        std::vector<VkFramebuffer> fbs;
        std::vector<VkCommandBuffer> cmds;
        std::vector<VkSemaphore> done;
        std::vector<VkFence> fences;
        bool ready = false;
    };

    // --- читання кадру чату зі спільної памʼяті ---
    bool read_frame() {
        if (!reader_.ensure_open()) return false;
        FrameView f;
        if (!reader_.read(&f)) return have_tex_ && enabled_;
        if (f.target_pid && f.target_pid != GetCurrentProcessId()) return false;
        if (!logged_) { logged_ = true;
            log("overlay(vk): кадр — enabled=%u target=%u ми=%u розмір=%ux%u",
                (unsigned)f.enabled, f.target_pid, (unsigned)GetCurrentProcessId(),
                f.width, f.height); }
        enabled_ = f.enabled != 0;
        if (!enabled_) return false;
        frame_ = f;
        if (f.seq != tex_seq_ || f.width != tex_w_ || f.height != tex_h_ || !have_tex_)
            need_upload_ = true;
        return true;
    }

    uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties_(phys_, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
                return i;
        return UINT32_MAX;
    }

    // Спільні на весь пристрій ресурси: пул команд, дескриптори, конвеєр,
    // семплер. Створюємо раз, коли знаємо родину черги показу.
    bool ensure_common(VkQueue q) {
        if (common_ready_) return true;
        auto it = queue_family_.find(q);
        if (it == queue_family_.end()) return false;   // не знаємо родину — ще ні
        present_family_ = it->second;

        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = present_family_;
        if (vkCreateCommandPool_(dev_, &pci, nullptr, &pool_) != VK_SUCCESS) return false;

        VkDescriptorSetLayoutBinding b = {};
        b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = 1; dl.pBindings = &b;
        if (vkCreateDescriptorSetLayout_(dev_, &dl, nullptr, &set_layout_) != VK_SUCCESS) return false;

        VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 1; dp.poolSizeCount = 1; dp.pPoolSizes = &ps;
        if (vkCreateDescriptorPool_(dev_, &dp, nullptr, &desc_pool_) != VK_SUCCESS) return false;
        VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = desc_pool_; da.descriptorSetCount = 1; da.pSetLayouts = &set_layout_;
        if (vkAllocateDescriptorSets_(dev_, &da, &desc_set_) != VK_SUCCESS) return false;

        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.0f;
        if (vkCreateSampler_(dev_, &si, nullptr, &sampler_) != VK_SUCCESS) return false;

        VkPushConstantRange pc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float)};
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1; pl.pSetLayouts = &set_layout_;
        pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pc;
        if (vkCreatePipelineLayout_(dev_, &pl, nullptr, &pipe_layout_) != VK_SUCCESS) return false;

        VkShaderModuleCreateInfo vs = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        vs.codeSize = sizeof(g_vk_vert_spv); vs.pCode = g_vk_vert_spv;
        VkShaderModuleCreateInfo fs = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        fs.codeSize = sizeof(g_vk_frag_spv); fs.pCode = g_vk_frag_spv;
        if (vkCreateShaderModule_(dev_, &vs, nullptr, &vs_) != VK_SUCCESS) return false;
        if (vkCreateShaderModule_(dev_, &fs, nullptr, &fs_) != VK_SUCCESS) return false;

        common_ready_ = true;
        log("overlay(vk): спільні ресурси готові (родина черги=%u)", present_family_);
        return true;
    }

    // Конвеєр залежить від формату свопчейна (render pass), тож будуємо його
    // при першому свопчейні й переробляємо, якщо формат інший.
    bool ensure_pipeline(VkFormat fmt, VkRenderPass rpass) {
        if (pipe_ && pipe_fmt_ == fmt) return true;
        if (pipe_) { vkDestroyPipeline_(dev_, pipe_, nullptr); pipe_ = VK_NULL_HANDLE; }

        VkPipelineShaderStageCreateInfo st[2] = {};
        st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs_; st[0].pName = "main";
        st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs_; st[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1; vp.scissorCount = 1;   // динамічні
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba = {};
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = st;
        gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
        gp.pDynamicState = &ds; gp.layout = pipe_layout_;
        gp.renderPass = rpass; gp.subpass = 0;
        if (vkCreateGraphicsPipelines_(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe_) != VK_SUCCESS) {
            log("overlay(vk): конвеєр не створено"); return false;
        }
        pipe_fmt_ = fmt;
        return true;
    }

    bool ensure_texture() {
        if (have_tex_ && tex_w_ == frame_.width && tex_h_ == frame_.height) return true;
        // старий звільняємо
        destroy_texture();
        uint32_t w = frame_.width, h = frame_.height;

        VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D; ic.format = VK_FORMAT_B8G8R8A8_UNORM;
        ic.extent = {w, h, 1}; ic.mipLevels = 1; ic.arrayLayers = 1;
        ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage_(dev_, &ic, nullptr, &tex_img_) != VK_SUCCESS) return false;
        VkMemoryRequirements mr; vkGetImageMemoryRequirements_(dev_, tex_img_, &mr);
        VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ma.allocationSize = mr.size;
        ma.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ma.memoryTypeIndex == UINT32_MAX) return false;
        if (vkAllocateMemory_(dev_, &ma, nullptr, &tex_mem_) != VK_SUCCESS) return false;
        vkBindImageMemory_(dev_, tex_img_, tex_mem_, 0);

        VkImageViewCreateInfo iv = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        iv.image = tex_img_; iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = VK_FORMAT_B8G8R8A8_UNORM;
        iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView_(dev_, &iv, nullptr, &tex_view_) != VK_SUCCESS) return false;

        // Стейджинг-буфер під заливку пікселів (host-visible).
        VkDeviceSize sz = (VkDeviceSize)w * h * 4;
        VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bc.size = sz; bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer_(dev_, &bc, nullptr, &stage_buf_) != VK_SUCCESS) return false;
        VkMemoryRequirements br; vkGetBufferMemoryRequirements_(dev_, stage_buf_, &br);
        VkMemoryAllocateInfo bam = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        bam.allocationSize = br.size;
        bam.memoryTypeIndex = mem_type(br.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (bam.memoryTypeIndex == UINT32_MAX) return false;
        if (vkAllocateMemory_(dev_, &bam, nullptr, &stage_mem_) != VK_SUCCESS) return false;
        vkBindBufferMemory_(dev_, stage_buf_, stage_mem_, 0);
        stage_size_ = sz;

        VkDescriptorImageInfo dii = {sampler_, tex_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = desc_set_; wr.dstBinding = 0; wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &dii;
        vkUpdateDescriptorSets_(dev_, 1, &wr, 0, nullptr);

        have_tex_ = true; tex_w_ = w; tex_h_ = h; tex_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }

    // Створює render pass, образи-в'юхи, каркаси, командні буфери й синхронізацію
    // для конкретного свопчейна. Раз на свопчейн.
    bool ensure_swap(SwapData& s, VkSwapchainKHR sc) {
        if (s.ready) return ensure_pipeline(s.format, s.rpass);

        uint32_t n = 0;
        if (vkGetSwapchainImagesKHR_(dev_, sc, &n, nullptr) != VK_SUCCESS || n == 0) return false;
        std::vector<VkImage> imgs(n);
        vkGetSwapchainImagesKHR_(dev_, sc, &n, imgs.data());

        // Render pass: вантажимо наявний вміст (кадр гри) і домальовуємо поверх.
        VkAttachmentDescription at = {};
        at.format = s.format; at.samples = VK_SAMPLE_COUNT_1_BIT;
        at.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;       // зберегти кадр гри
        at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        at.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub = {};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        VkSubpassDependency dep = {};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1; rp.pAttachments = &at;
        rp.subpassCount = 1; rp.pSubpasses = &sub;
        rp.dependencyCount = 1; rp.pDependencies = &dep;
        if (vkCreateRenderPass_(dev_, &rp, nullptr, &s.rpass) != VK_SUCCESS) return false;

        s.views.resize(n); s.fbs.resize(n); s.cmds.resize(n);
        s.done.resize(n); s.fences.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            VkImageViewCreateInfo iv = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            iv.image = imgs[i]; iv.viewType = VK_IMAGE_VIEW_TYPE_2D; iv.format = s.format;
            iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView_(dev_, &iv, nullptr, &s.views[i]) != VK_SUCCESS) {
                destroy_swap(s); return false; }
            VkFramebufferCreateInfo fb = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb.renderPass = s.rpass; fb.attachmentCount = 1; fb.pAttachments = &s.views[i];
            fb.width = s.extent.width; fb.height = s.extent.height; fb.layers = 1;
            if (vkCreateFramebuffer_(dev_, &fb, nullptr, &s.fbs[i]) != VK_SUCCESS) {
                destroy_swap(s); return false; }
            VkSemaphoreCreateInfo se = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            vkCreateSemaphore_(dev_, &se, nullptr, &s.done[i]);
            VkFenceCreateInfo fe = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fe.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence_(dev_, &fe, nullptr, &s.fences[i]);
        }
        VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool_; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = n;
        if (vkAllocateCommandBuffers_(dev_, &ca, s.cmds.data()) != VK_SUCCESS) {
            destroy_swap(s); return false; }

        s.ready = true;
        log("overlay(vk): свопчейн готовий (образів=%u, %ux%u)", n, s.extent.width, s.extent.height);
        return ensure_pipeline(s.format, s.rpass);
    }

    VkSemaphore draw_swap(VkQueue q, SwapData& s, uint32_t idx,
                          const VkSemaphore* waits, uint32_t waitCount) {
        if (idx >= s.cmds.size()) return VK_NULL_HANDLE;
        if (!ensure_texture()) return VK_NULL_HANDLE;

        // Чекаємо, поки цей командний буфер звільниться (кадр тому).
        vkWaitForFences_(dev_, 1, &s.fences[idx], VK_TRUE, UINT64_MAX);
        vkResetFences_(dev_, 1, &s.fences[idx]);

        VkCommandBuffer cb = s.cmds[idx];
        vkResetCommandBuffer_(cb, 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer_(cb, &bi);

        // Заливка нової текстури (за потреби) через стейджинг.
        if (need_upload_) {
            upload_texture(cb);
            need_upload_ = false;
            tex_seq_ = frame_.seq;
        } else if (tex_layout_ != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier(cb, tex_layout_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            tex_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        // Рамка чату всередині кадру — частки кадру, масштабуємо під гру.
        float x, y, ow, oh;
        frame_.rect((float)s.extent.width, (float)s.extent.height, &x, &y, &ow, &oh);

        VkClearValue noClear = {};
        VkRenderPassBeginInfo rpb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpb.renderPass = s.rpass; rpb.framebuffer = s.fbs[idx];
        rpb.renderArea.extent = s.extent;
        rpb.clearValueCount = 0; rpb.pClearValues = &noClear;
        vkCmdBeginRenderPass_(cb, &rpb, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vpt = {x, y, ow, oh, 0.0f, 1.0f};
        VkRect2D sci = {{(int32_t)x, (int32_t)y}, {(uint32_t)ow, (uint32_t)oh}};
        vkCmdSetViewport_(cb, 0, 1, &vpt);
        vkCmdSetScissor_(cb, 0, 1, &sci);
        vkCmdBindPipeline_(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_);
        vkCmdBindDescriptorSets_(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_, 0, 1, &desc_set_, 0, nullptr);
        float op = frame_.opacity / 255.0f;
        vkCmdPushConstants_(cb, pipe_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float), &op);
        vkCmdDraw_(cb, 4, 1, 0, 0);
        vkCmdEndRenderPass_(cb);
        vkEndCommandBuffer_(cb);

        // Наш сабміт: чекаємо семафори показу, сигналимо власний.
        std::vector<VkPipelineStageFlags> stages(waitCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = waitCount; si.pWaitSemaphores = waits;
        si.pWaitDstStageMask = stages.data();
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &s.done[idx];
        if (vkQueueSubmit_(q, 1, &si, s.fences[idx]) != VK_SUCCESS) return VK_NULL_HANDLE;
        return s.done[idx];
    }

    void upload_texture(VkCommandBuffer cb) {
        void* p = nullptr;
        if (vkMapMemory_(dev_, stage_mem_, 0, stage_size_, 0, &p) != VK_SUCCESS) return;
        // Кадр у спільній памʼяті щільний (stride = width*4), тож копіюємо цілим.
        memcpy(p, frame_.pixels, (size_t)frame_.width * frame_.height * 4);
        vkUnmapMemory_(dev_, stage_mem_);

        barrier(cb, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy bic = {};
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageExtent = {frame_.width, frame_.height, 1};
        vkCmdCopyBufferToImage_(cb, stage_buf_, tex_img_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        barrier(cb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        tex_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    void barrier(VkCommandBuffer cb, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = tex_img_;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkPipelineStageFlags src = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags dst = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        if (to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            src = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; dst = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (to == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            src = VK_PIPELINE_STAGE_TRANSFER_BIT; dst = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        vkCmdPipelineBarrier_(cb, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    void destroy_texture() {
        if (tex_view_) vkDestroyImageView_(dev_, tex_view_, nullptr);
        if (tex_img_) vkDestroyImage_(dev_, tex_img_, nullptr);
        if (tex_mem_) vkFreeMemory_(dev_, tex_mem_, nullptr);
        if (stage_buf_) vkDestroyBuffer_(dev_, stage_buf_, nullptr);
        if (stage_mem_) vkFreeMemory_(dev_, stage_mem_, nullptr);
        tex_view_ = VK_NULL_HANDLE; tex_img_ = VK_NULL_HANDLE; tex_mem_ = VK_NULL_HANDLE;
        stage_buf_ = VK_NULL_HANDLE; stage_mem_ = VK_NULL_HANDLE;
        have_tex_ = false;
    }

    void destroy_swap(SwapData& s) {
        if (!dev_) return;
        for (auto v : s.views) if (v) vkDestroyImageView_(dev_, v, nullptr);
        for (auto f : s.fbs) if (f) vkDestroyFramebuffer_(dev_, f, nullptr);
        for (auto se : s.done) if (se) vkDestroySemaphore_(dev_, se, nullptr);
        for (auto fe : s.fences) if (fe) vkDestroyFence_(dev_, fe, nullptr);
        if (!s.cmds.empty()) vkFreeCommandBuffers_(dev_, pool_, (uint32_t)s.cmds.size(), s.cmds.data());
        if (s.rpass) vkDestroyRenderPass_(dev_, s.rpass, nullptr);
        s = SwapData{};
    }

    void destroy_common() {
        destroy_texture();
        if (pipe_) vkDestroyPipeline_(dev_, pipe_, nullptr);
        if (vs_) vkDestroyShaderModule_(dev_, vs_, nullptr);
        if (fs_) vkDestroyShaderModule_(dev_, fs_, nullptr);
        if (pipe_layout_) vkDestroyPipelineLayout_(dev_, pipe_layout_, nullptr);
        if (sampler_) vkDestroySampler_(dev_, sampler_, nullptr);
        if (desc_pool_) vkDestroyDescriptorPool_(dev_, desc_pool_, nullptr);
        if (set_layout_) vkDestroyDescriptorSetLayout_(dev_, set_layout_, nullptr);
        if (pool_) vkDestroyCommandPool_(dev_, pool_, nullptr);
        pipe_ = VK_NULL_HANDLE; vs_ = fs_ = VK_NULL_HANDLE; pipe_layout_ = VK_NULL_HANDLE;
        sampler_ = VK_NULL_HANDLE; desc_pool_ = VK_NULL_HANDLE; set_layout_ = VK_NULL_HANDLE;
        pool_ = VK_NULL_HANDLE; common_ready_ = false;
    }

    // --- завантажені функції ---
    PFN_vkGetInstanceProcAddr gipa_ = nullptr;
    #define VKF(name) PFN_##name name##_ = nullptr
    VKF(vkGetDeviceProcAddr);
    VKF(vkGetPhysicalDeviceMemoryProperties);
    VKF(vkGetSwapchainImagesKHR);
    VKF(vkCreateImageView); VKF(vkDestroyImageView);
    VKF(vkCreateFramebuffer); VKF(vkDestroyFramebuffer);
    VKF(vkCreateRenderPass); VKF(vkDestroyRenderPass);
    VKF(vkCreateShaderModule); VKF(vkDestroyShaderModule);
    VKF(vkCreatePipelineLayout); VKF(vkDestroyPipelineLayout);
    VKF(vkCreateGraphicsPipelines); VKF(vkDestroyPipeline);
    VKF(vkCreateDescriptorSetLayout); VKF(vkDestroyDescriptorSetLayout);
    VKF(vkCreateDescriptorPool); VKF(vkDestroyDescriptorPool);
    VKF(vkAllocateDescriptorSets); VKF(vkUpdateDescriptorSets);
    VKF(vkCreateSampler); VKF(vkDestroySampler);
    VKF(vkCreateImage); VKF(vkDestroyImage);
    VKF(vkAllocateMemory); VKF(vkFreeMemory);
    VKF(vkBindImageMemory); VKF(vkGetImageMemoryRequirements);
    VKF(vkCreateBuffer); VKF(vkDestroyBuffer);
    VKF(vkGetBufferMemoryRequirements); VKF(vkBindBufferMemory);
    VKF(vkMapMemory); VKF(vkUnmapMemory);
    VKF(vkCreateCommandPool); VKF(vkDestroyCommandPool);
    VKF(vkAllocateCommandBuffers); VKF(vkFreeCommandBuffers);
    VKF(vkBeginCommandBuffer); VKF(vkEndCommandBuffer);
    VKF(vkCmdBeginRenderPass); VKF(vkCmdEndRenderPass);
    VKF(vkCmdBindPipeline); VKF(vkCmdBindDescriptorSets);
    VKF(vkCmdSetViewport); VKF(vkCmdSetScissor);
    VKF(vkCmdPushConstants); VKF(vkCmdDraw);
    VKF(vkCmdPipelineBarrier); VKF(vkCmdCopyBufferToImage);
    VKF(vkCreateSemaphore); VKF(vkDestroySemaphore);
    VKF(vkCreateFence); VKF(vkDestroyFence);
    VKF(vkWaitForFences); VKF(vkResetFences);
    VKF(vkResetCommandBuffer); VKF(vkQueueSubmit);
    VKF(vkDeviceWaitIdle);
    #undef VKF

    // --- захоплений стан ---
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    std::unordered_map<VkQueue, uint32_t> queue_family_;
    std::unordered_map<VkSwapchainKHR, SwapData> swaps_;
    uint32_t present_family_ = 0;

    // --- спільні ресурси ---
    bool common_ready_ = false;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE, fs_ = VK_NULL_HANDLE;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    VkFormat pipe_fmt_ = VK_FORMAT_UNDEFINED;

    // --- текстура чату ---
    VkImage tex_img_ = VK_NULL_HANDLE;
    VkDeviceMemory tex_mem_ = VK_NULL_HANDLE;
    VkImageView tex_view_ = VK_NULL_HANDLE;
    VkBuffer stage_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory stage_mem_ = VK_NULL_HANDLE;
    VkDeviceSize stage_size_ = 0;
    VkImageLayout tex_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    bool have_tex_ = false, need_upload_ = false;
    uint32_t tex_w_ = 0, tex_h_ = 0, tex_seq_ = 0;

    // --- кадр ---
    SharedFrameReader reader_;
    FrameView frame_;
    bool enabled_ = false, logged_ = false;
};

}  // namespace hominka
