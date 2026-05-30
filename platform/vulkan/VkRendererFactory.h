// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Thin factory header — includes only platform-hal types.
// Consumers (game, tools) include this instead of VkRenderer.h so they are
// never exposed to Vulkan or VMA headers.
#include "IRenderer.h"
#include <memory>

std::unique_ptr<IRenderer> createVulkanRenderer();
