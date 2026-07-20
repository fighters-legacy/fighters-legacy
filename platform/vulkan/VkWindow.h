// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Internal header — not part of platform-hal. Only included by VkRenderer.cpp.
#include <vector>
#include <vulkan/vulkan.h>

struct SDL_Window;

namespace fl {

std::vector<const char*> vk_getRequiredInstanceExtensions(SDL_Window* window);
// Instance extensions for a swapchain-free headless renderer (#913): the debug-utils / portability
// extensions only, never the WSI surface extensions (SDL video need not be initialized headless).
std::vector<const char*> vk_getHeadlessInstanceExtensions();
VkSurfaceKHR vk_createSurface(VkInstance instance, SDL_Window* window);

} // namespace fl
