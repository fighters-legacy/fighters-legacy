// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include "VkWindow.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

std::vector<const char*> vk_getRequiredInstanceExtensions(SDL_Window* /*window*/) {
    Uint32 count = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&count);

    std::vector<const char*> exts(sdlExts, sdlExts + count);

#if defined(FL_VK_VALIDATION)
    exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

#if defined(__APPLE__)
    exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

    return exts;
}

VkSurfaceKHR vk_createSurface(VkInstance instance, SDL_Window* window) {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface))
        return VK_NULL_HANDLE;
    return surface;
}
