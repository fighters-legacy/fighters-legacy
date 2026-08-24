// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vulkan/vulkan.h>

namespace fl {

// Vulkan boilerplate this module writes more than once (#1265). Module-internal: platform/vulkan
// only, never included from engine/ or game/ — the layering rule stands untouched.

// Pick a memory type satisfying `props` from the ones `filter` permits. Falls back to index 0 when
// nothing matches, which is what every caller has always relied on: the allocation then fails at
// vkAllocateMemory with a real Vulkan error rather than here with a silent one.
inline uint32_t findMemoryType(VkPhysicalDevice physDevice, uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if ((filter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return 0;
}

// One image memory barrier. VkRenderer.cpp and VkResources.cpp each held a file-static copy (one
// with a mipLevels parameter, one without) and the screenshot readback re-rolled a third as an
// inline lambda with the stages hardcoded to TRANSFER->TRANSFER.
//
// The queue-family indices are always IGNORED here: everything this renderer submits goes to the one
// graphics queue, so an ownership transfer would be a different function with a different contract,
// not a parameter to add later. The mipLevels/layerCount defaults are the single-level 2D case,
// which is every caller but the mipped textures and the shadow-cascade array.
inline void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                         VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                         VkPipelineStageFlags dstStage, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                         uint32_t mipLevels = 1, uint32_t layerCount = 1) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, mipLevels, 0, layerCount};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace fl
