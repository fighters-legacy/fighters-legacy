// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Internal header — not part of platform-hal. Only included by VkRenderer.cpp.
#include <vector>
#include <vulkan/vulkan.h>

struct SDL_Window;

std::vector<const char*> vk_getRequiredInstanceExtensions(SDL_Window* window);
VkSurfaceKHR vk_createSurface(VkInstance instance, SDL_Window* window);
