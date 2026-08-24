// Малювання кадру чату поверх кадру гри для Vulkan.
//
// Найскладніший бекенд: Vulkan не має «глобального стану», малювати треба у
// власному конвеєрі поверх образу свопчейна гри. Хук — на vkQueuePresentKHR
// (плоский експорт vulkan-1.dll), плюс vkCreateSwapchainKHR, щоб знати образи
// свопчейна. Реалізація нижче.
#pragma once

// Оголошення для dllmain: справжня реалізація в overlay_vk.cpp.
namespace hominka { bool install_vk_hook_impl(); }
