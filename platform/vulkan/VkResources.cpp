// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

// ---------------------------------------------------------------------------
// VMA implementation — exactly one TU.
// Dynamic Vulkan function loading: VMA calls vkGetInstanceProcAddr /
// vkGetDeviceProcAddr at runtime rather than static-linking Vulkan entry pts.
// ---------------------------------------------------------------------------
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

// ---------------------------------------------------------------------------
// tinygltf + stb_image implementation — exactly one TU in this library.
// TINYGLTF_HEADER_ONLY suppresses tinygltf's own compiled target; we define
// TINYGLTF_IMPLEMENTATION here to pull in all definitions. stb_image is
// included via this path (as a system header → warnings suppressed).
// ---------------------------------------------------------------------------
#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>

// ---------------------------------------------------------------------------
// KTX2 / Basis Universal
// ---------------------------------------------------------------------------
#include <ktx.h>

#include "RenderTypes.h"
#include "VkResources.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void imageBarrierSimple(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                               VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                               VkPipelineStageFlags dstStage, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                               uint32_t mipLevels = 1) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, mipLevels, 0, 1};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static bool checkFormat(VkPhysicalDevice physDevice, VkFormat format) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physDevice, format, &props);
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

// ---------------------------------------------------------------------------
// VkResourceManager — init / shutdown
// ---------------------------------------------------------------------------

bool VkResourceManager::init(VkDevice device, VkPhysicalDevice physDevice, VkInstance instance, VkCommandPool cmdPool,
                             VkQueue graphicsQueue, VkDescriptorSetLayout matSetLayout) {
    m_device = device;
    m_physDevice = physDevice;
    m_cmdPool = cmdPool;
    m_queue = graphicsQueue;
    m_matSetLayout = matSetLayout;

    // Texture format support for Basis Universal transcode target selection.
    m_supportsBC7 = checkFormat(physDevice, VK_FORMAT_BC7_UNORM_BLOCK);
    m_supportsASTC4x4 = checkFormat(physDevice, VK_FORMAT_ASTC_4x4_UNORM_BLOCK);

    // Create VMA allocator with dynamic Vulkan function loading.
    VmaVulkanFunctions vkFns{};
    vkFns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vkFns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocCI{};
    allocCI.vulkanApiVersion = VK_API_VERSION_1_3;
    allocCI.physicalDevice = physDevice;
    allocCI.device = device;
    allocCI.instance = instance;
    allocCI.pVulkanFunctions = &vkFns;

    if (vmaCreateAllocator(&allocCI, &m_allocator) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkResources] vmaCreateAllocator failed\n");
        return false;
    }

    // Descriptor pool for per-material descriptor sets.
    // 3 combined image samplers per material (base color, normal, ORM).
    constexpr uint32_t kMaxMaterials = 256;
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxMaterials * 3};
    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCI.maxSets = kMaxMaterials;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &poolCI, nullptr, &m_matPool) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkResources] material descriptor pool creation failed\n");
        return false;
    }

    if (!createDefaultWhiteTexture())
        return false;
    if (!createDefaultFlatNormalTexture())
        return false;
    if (!createDefaultWhiteLinearTexture())
        return false;

    return true;
}

void VkResourceManager::shutdown() {
    if (m_device == VK_NULL_HANDLE)
        return;

    vkDeviceWaitIdle(m_device);

    // Flush all deferred deletions.
    m_frame = std::numeric_limits<uint64_t>::max();
    flushDeferred();

    // Destroy all alive materials.
    for (auto& mat : m_materials) {
        if (!mat.alive)
            continue;
        if (mat.descriptorSet != VK_NULL_HANDLE)
            vkFreeDescriptorSets(m_device, m_matPool, 1, &mat.descriptorSet);
    }
    m_materials.clear();

    // Destroy all alive textures.
    for (auto& tex : m_textures) {
        if (!tex.alive)
            continue;
        if (tex.sampler != VK_NULL_HANDLE)
            vkDestroySampler(m_device, tex.sampler, nullptr);
        if (tex.view != VK_NULL_HANDLE)
            vkDestroyImageView(m_device, tex.view, nullptr);
        if (tex.image != VK_NULL_HANDLE)
            vmaDestroyImage(m_allocator, tex.image, tex.alloc);
    }
    m_textures.clear();

    // Destroy all alive meshes.
    for (auto& mesh : m_meshes) {
        if (!mesh.alive)
            continue;
        if (mesh.indexBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_allocator, mesh.indexBuffer, mesh.indexAlloc);
        if (mesh.vertexBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_allocator, mesh.vertexBuffer, mesh.vertexAlloc);
    }
    m_meshes.clear();

    if (m_matPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_matPool, nullptr);
        m_matPool = VK_NULL_HANDLE;
    }
    if (m_allocator) {
        vmaDestroyAllocator(m_allocator);
        m_allocator = {};
    }
}

// ---------------------------------------------------------------------------
// tick — advance frame counter, flush safe deferred deletions
// ---------------------------------------------------------------------------

void VkResourceManager::tick(uint64_t frameIndex) {
    m_frame = frameIndex;
    flushDeferred();
}

void VkResourceManager::flushDeferred() {
    auto shouldFlush = [this](uint64_t releaseAfter) { return m_frame >= releaseAfter; };

    m_deferredBuffers.erase(std::remove_if(m_deferredBuffers.begin(), m_deferredBuffers.end(),
                                           [&](const DeferredBuffer& d) {
                                               if (!shouldFlush(d.releaseAfterFrame))
                                                   return false;
                                               vmaDestroyBuffer(m_allocator, d.buf, d.alloc);
                                               return true;
                                           }),
                            m_deferredBuffers.end());

    m_deferredImages.erase(std::remove_if(m_deferredImages.begin(), m_deferredImages.end(),
                                          [&](const DeferredImage& d) {
                                              if (!shouldFlush(d.releaseAfterFrame))
                                                  return false;
                                              if (d.sampler != VK_NULL_HANDLE)
                                                  vkDestroySampler(m_device, d.sampler, nullptr);
                                              if (d.view != VK_NULL_HANDLE)
                                                  vkDestroyImageView(m_device, d.view, nullptr);
                                              if (d.image != VK_NULL_HANDLE)
                                                  vmaDestroyImage(m_allocator, d.image, d.alloc);
                                              return true;
                                          }),
                           m_deferredImages.end());
}

// ---------------------------------------------------------------------------
// One-shot command buffer helpers
// ---------------------------------------------------------------------------

VkCommandBuffer VkResourceManager::beginOneShot() {
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = m_cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &ai, &cmd);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void VkResourceManager::endOneShot(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_queue);
    vkFreeCommandBuffers(m_device, m_cmdPool, 1, &cmd);
}

// ---------------------------------------------------------------------------
// Buffer + image helpers
// ---------------------------------------------------------------------------

bool VkResourceManager::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memUsage,
                                     VkBuffer& buf, VmaAllocation& alloc) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo aci{};
    aci.usage = memUsage;
    if (memUsage == VMA_MEMORY_USAGE_CPU_TO_GPU)
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    return vmaCreateBuffer(m_allocator, &bci, &aci, &buf, &alloc, nullptr) == VK_SUCCESS;
}

bool VkResourceManager::uploadBuffer(VkBuffer dst, const void* src, VkDeviceSize size) {
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc{};
    if (!createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, staging, stagingAlloc))
        return false;

    void* mapped = nullptr;
    vmaMapMemory(m_allocator, stagingAlloc, &mapped);
    std::memcpy(mapped, src, static_cast<std::size_t>(size));
    vmaUnmapMemory(m_allocator, stagingAlloc);

    VkCommandBuffer cmd = beginOneShot();
    VkBufferCopy region{0, 0, size};
    vkCmdCopyBuffer(cmd, staging, dst, 1, &region);
    endOneShot(cmd);

    vmaDestroyBuffer(m_allocator, staging, stagingAlloc);
    return true;
}

bool VkResourceManager::createGpuImage(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t mipLevels,
                                       VkFormat format, GpuTexture& tex) {
    const VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;

    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc{};
    if (!createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, staging, stagingAlloc))
        return false;

    void* mapped = nullptr;
    vmaMapMemory(m_allocator, stagingAlloc, &mapped);
    std::memcpy(mapped, pixels, static_cast<std::size_t>(size));
    vmaUnmapMemory(m_allocator, stagingAlloc);

    VkImageCreateInfo imageCI{};
    imageCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCI.imageType = VK_IMAGE_TYPE_2D;
    imageCI.extent = {width, height, 1};
    imageCI.mipLevels = mipLevels;
    imageCI.arrayLayers = 1;
    imageCI.format = format;
    imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCI.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (mipLevels > 1)
        imageCI.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(m_allocator, &imageCI, &aci, &tex.image, &tex.alloc, nullptr) != VK_SUCCESS) {
        vmaDestroyBuffer(m_allocator, staging, stagingAlloc);
        return false;
    }

    VkCommandBuffer cmd = beginOneShot();

    imageBarrierSimple(cmd, tex.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    imageBarrierSimple(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);

    endOneShot(cmd);
    vmaDestroyBuffer(m_allocator, staging, stagingAlloc);

    VkImageViewCreateInfo viewCI{};
    viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image = tex.image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format = format;
    viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.levelCount = mipLevels;
    viewCI.subresourceRange.layerCount = 1;
    if (vkCreateImageView(m_device, &viewCI, nullptr, &tex.view) != VK_SUCCESS)
        return false;

    return createDefaultSampler(tex.sampler);
}

bool VkResourceManager::createDefaultSampler(VkSampler& out) {
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    sci.anisotropyEnable = VK_FALSE;
    return vkCreateSampler(m_device, &sci, nullptr, &out) == VK_SUCCESS;
}

bool VkResourceManager::createDefaultWhiteTexture() {
    const uint8_t white[4] = {255, 255, 255, 255};
    GpuTexture tex{};
    if (!createGpuImage(white, 1, 1, 1, VK_FORMAT_R8G8B8A8_SRGB, tex))
        return false;
    tex.alive = true;
    m_textures.push_back(tex);
    m_defaultWhite = TextureHandle{static_cast<uint32_t>(m_textures.size())};
    return true;
}

bool VkResourceManager::createDefaultFlatNormalTexture() {
    // Tangent-space flat normal: (0,0,+1) encoded as RGB={128,128,255}.
    const uint8_t flatNormal[4] = {128, 128, 255, 255};
    GpuTexture tex{};
    if (!createGpuImage(flatNormal, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, tex))
        return false;
    tex.alive = true;
    m_textures.push_back(tex);
    m_defaultFlatNormal = TextureHandle{static_cast<uint32_t>(m_textures.size())};
    return true;
}

bool VkResourceManager::createDefaultWhiteLinearTexture() {
    // All-ones ORM: occlusion=1, roughness factor passes through, metallic=0 via factor.
    const uint8_t white[4] = {255, 255, 255, 255};
    GpuTexture tex{};
    if (!createGpuImage(white, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, tex))
        return false;
    tex.alive = true;
    m_textures.push_back(tex);
    m_defaultWhiteLinear = TextureHandle{static_cast<uint32_t>(m_textures.size())};
    return true;
}

// ---------------------------------------------------------------------------
// createGpuImageCompressed — upload BC7/ASTC KTX2 data with all mip levels
// ---------------------------------------------------------------------------
bool VkResourceManager::createGpuImageCompressed(const uint8_t* data, VkDeviceSize dataSize, uint32_t width,
                                                 uint32_t height, uint32_t numMips, VkFormat format, void* ktxHandle,
                                                 GpuTexture& tex) {
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc{};
    if (!createBuffer(dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, staging, stagingAlloc))
        return false;

    void* mapped = nullptr;
    vmaMapMemory(m_allocator, stagingAlloc, &mapped);
    std::memcpy(mapped, data, static_cast<std::size_t>(dataSize));
    vmaUnmapMemory(m_allocator, stagingAlloc);

    VkImageCreateInfo imageCI{};
    imageCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCI.imageType = VK_IMAGE_TYPE_2D;
    imageCI.extent = {width, height, 1};
    imageCI.mipLevels = numMips;
    imageCI.arrayLayers = 1;
    imageCI.format = format;
    imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCI.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(m_allocator, &imageCI, &aci, &tex.image, &tex.alloc, nullptr) != VK_SUCCESS) {
        vmaDestroyBuffer(m_allocator, staging, stagingAlloc);
        return false;
    }

    VkCommandBuffer cmd = beginOneShot();

    imageBarrierSimple(cmd, tex.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, numMips);

    // One copy region per mip level; offsets come from the KTX2 container.
    std::vector<VkBufferImageCopy> regions(numMips);
    auto* ktx = reinterpret_cast<ktxTexture*>(ktxHandle);
    for (uint32_t mip = 0; mip < numMips; ++mip) {
        ktx_size_t offset = 0;
        ktxTexture_GetImageOffset(ktx, mip, 0, 0, &offset);

        const uint32_t mipW = std::max(width >> mip, 1u);
        const uint32_t mipH = std::max(height >> mip, 1u);

        VkBufferImageCopy& r = regions[mip];
        r = {};
        r.bufferOffset = static_cast<VkDeviceSize>(offset);
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.mipLevel = mip;
        r.imageSubresource.layerCount = 1;
        r.imageExtent = {mipW, mipH, 1};
    }
    vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());

    imageBarrierSimple(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_IMAGE_ASPECT_COLOR_BIT, numMips);

    endOneShot(cmd);
    vmaDestroyBuffer(m_allocator, staging, stagingAlloc);

    VkImageViewCreateInfo viewCI{};
    viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image = tex.image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format = format;
    viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.levelCount = numMips;
    viewCI.subresourceRange.layerCount = 1;
    if (vkCreateImageView(m_device, &viewCI, nullptr, &tex.view) != VK_SUCCESS)
        return false;

    return createDefaultSampler(tex.sampler);
}

// ---------------------------------------------------------------------------
// createMesh — parse glTF .glb, extract first primitive, upload to GPU
// ---------------------------------------------------------------------------

MeshHandle VkResourceManager::createMesh(const MeshUploadDesc& desc) {
    if (desc.bytes.empty())
        return {};

    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;

    const bool ok =
        loader.LoadBinaryFromMemory(&model, &err, &warn, desc.bytes.data(), static_cast<uint32_t>(desc.bytes.size()));
    if (!warn.empty())
        std::fprintf(stderr, "[VkResources] glTF warn (%.*s): %s\n", static_cast<int>(desc.name.size()),
                     desc.name.data(), warn.c_str());
    if (!ok) {
        std::fprintf(stderr, "[VkResources] glTF load failed (%.*s): %s\n", static_cast<int>(desc.name.size()),
                     desc.name.data(), err.c_str());
        return {};
    }

    if (model.meshes.empty())
        return {};
    const auto& prim = model.meshes[0].primitives[0];
    if (prim.attributes.find("POSITION") == prim.attributes.end())
        return {};

    // ── Extract vertex data ──────────────────────────────────────────────
    auto getAccessorData = [&](const std::string& attr, int& accIdx) -> const uint8_t* {
        auto it = prim.attributes.find(attr);
        if (it == prim.attributes.end()) {
            accIdx = -1;
            return nullptr;
        }
        accIdx = it->second;
        const auto& acc = model.accessors[accIdx];
        const auto& bv = model.bufferViews[acc.bufferView];
        return model.buffers[bv.buffer].data.data() + bv.byteOffset + acc.byteOffset;
    };

    int posIdx = -1, nrmIdx = -1, tanIdx = -1, uvIdx = -1;
    const uint8_t* posPtr = getAccessorData("POSITION", posIdx);
    const uint8_t* nrmPtr = getAccessorData("NORMAL", nrmIdx);
    const uint8_t* tanPtr = getAccessorData("TANGENT", tanIdx);
    const uint8_t* uvPtr = getAccessorData("TEXCOORD_0", uvIdx);

    if (!posPtr)
        return {};

    const uint32_t vertexCount = static_cast<uint32_t>(model.accessors[posIdx].count);

    auto stridedVec3 = [&](const uint8_t* base, int accI, uint32_t i) -> glm::vec3 {
        if (!base)
            return {};
        const auto& acc = model.accessors[accI];
        const auto& bv = model.bufferViews[acc.bufferView];
        const uint32_t stride = bv.byteStride ? static_cast<uint32_t>(bv.byteStride) : sizeof(glm::vec3);
        glm::vec3 v{};
        std::memcpy(&v, base + i * stride, sizeof(v));
        return v;
    };
    auto stridedVec4 = [&](const uint8_t* base, int accI, uint32_t i) -> glm::vec4 {
        if (!base)
            return glm::vec4(1, 0, 0, 1);
        const auto& acc = model.accessors[accI];
        const auto& bv = model.bufferViews[acc.bufferView];
        const uint32_t stride = bv.byteStride ? static_cast<uint32_t>(bv.byteStride) : sizeof(glm::vec4);
        glm::vec4 v{};
        std::memcpy(&v, base + i * stride, sizeof(v));
        return v;
    };
    auto stridedVec2 = [&](const uint8_t* base, int accI, uint32_t i) -> glm::vec2 {
        if (!base)
            return {};
        const auto& acc = model.accessors[accI];
        const auto& bv = model.bufferViews[acc.bufferView];
        const uint32_t stride = bv.byteStride ? static_cast<uint32_t>(bv.byteStride) : sizeof(glm::vec2);
        glm::vec2 v{};
        std::memcpy(&v, base + i * stride, sizeof(v));
        return v;
    };

    std::vector<Vertex> vertices(vertexCount);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        vertices[i].position = stridedVec3(posPtr, posIdx, i);
        vertices[i].normal = nrmPtr ? stridedVec3(nrmPtr, nrmIdx, i) : glm::vec3(0, 1, 0);
        vertices[i].tangent = tanPtr ? stridedVec4(tanPtr, tanIdx, i) : glm::vec4(1, 0, 0, 1);
        vertices[i].uv = uvPtr ? stridedVec2(uvPtr, uvIdx, i) : glm::vec2(0, 0);
    }

    // ── Extract index data ───────────────────────────────────────────────
    std::vector<uint32_t> indices;
    if (prim.indices >= 0) {
        const auto& acc = model.accessors[prim.indices];
        const auto& bv = model.bufferViews[acc.bufferView];
        const uint8_t* data = model.buffers[bv.buffer].data.data() + bv.byteOffset + acc.byteOffset;
        indices.reserve(acc.count);
        for (std::size_t i = 0; i < acc.count; ++i) {
            uint32_t idx = 0;
            if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                uint16_t v16 = 0;
                std::memcpy(&v16, data + i * sizeof(uint16_t), sizeof(uint16_t));
                idx = v16;
            } else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                std::memcpy(&idx, data + i * sizeof(uint32_t), sizeof(uint32_t));
            } else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                idx = data[i];
            }
            indices.push_back(idx);
        }
    } else {
        // No index buffer: generate sequential indices.
        indices.resize(vertexCount);
        for (uint32_t i = 0; i < vertexCount; ++i)
            indices[i] = i;
    }

    // ── Upload to GPU ────────────────────────────────────────────────────
    GpuMesh mesh{};
    const VkDeviceSize vbSize = vertices.size() * sizeof(Vertex);
    const VkDeviceSize ibSize = indices.size() * sizeof(uint32_t);

    if (!createBuffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VMA_MEMORY_USAGE_GPU_ONLY, mesh.vertexBuffer, mesh.vertexAlloc))
        return {};
    if (!uploadBuffer(mesh.vertexBuffer, vertices.data(), vbSize)) {
        vmaDestroyBuffer(m_allocator, mesh.vertexBuffer, mesh.vertexAlloc);
        return {};
    }

    if (!createBuffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VMA_MEMORY_USAGE_GPU_ONLY, mesh.indexBuffer, mesh.indexAlloc)) {
        vmaDestroyBuffer(m_allocator, mesh.vertexBuffer, mesh.vertexAlloc);
        return {};
    }
    if (!uploadBuffer(mesh.indexBuffer, indices.data(), ibSize)) {
        vmaDestroyBuffer(m_allocator, mesh.indexBuffer, mesh.indexAlloc);
        vmaDestroyBuffer(m_allocator, mesh.vertexBuffer, mesh.vertexAlloc);
        return {};
    }

    mesh.indexCount = static_cast<uint32_t>(indices.size());
    mesh.alive = true;

    // ── Slot allocation ──────────────────────────────────────────────────
    uint32_t slot = 0;
    if (!m_freeMeshSlots.empty()) {
        slot = m_freeMeshSlots.back();
        m_freeMeshSlots.pop_back();
        m_meshes[slot] = mesh;
    } else {
        slot = static_cast<uint32_t>(m_meshes.size());
        m_meshes.push_back(mesh);
    }
    return MeshHandle{slot + 1};
}

// ---------------------------------------------------------------------------
// createTexture — detect KTX2 vs PNG, decode, upload
// ---------------------------------------------------------------------------

TextureHandle VkResourceManager::createTexture(const TextureUploadDesc& desc) {
    if (desc.bytes.empty())
        return m_defaultWhite;

    GpuTexture tex{};
    bool ok = false;

    // KTX2 magic: 0xAB 0x4B 0x54 0x58 0x20 0x32 0x30 0xBB 0x0D 0x0A 0x1A 0x0A
    const bool isKtx2 = desc.bytes.size() >= 12 && desc.bytes[0] == 0xAB && desc.bytes[1] == 0x4B &&
                        desc.bytes[2] == 0x54 && desc.bytes[3] == 0x58 && desc.bytes[4] == 0x20 &&
                        desc.bytes[5] == 0x32;

    if (isKtx2) {
        ktxTexture2* ktx = nullptr;
        KTX_error_code result = ktxTexture2_CreateFromMemory(desc.bytes.data(), desc.bytes.size(),
                                                             KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktx);

        if (result != KTX_SUCCESS) {
            std::fprintf(stderr, "[VkResources] KTX2 load failed (%.*s): %d\n", static_cast<int>(desc.name.size()),
                         desc.name.data(), static_cast<int>(result));
            return m_defaultWhite;
        }

        if (ktxTexture2_NeedsTranscoding(ktx)) {
            // Select the best GPU-native format for this device:
            //   BC7  — desktop (Windows / Linux), broad GPU coverage
            //   ASTC — Apple Silicon (M-series has no BC support)
            //   RGBA32 — universal fallback for older / software renderers
            ktx_transcode_fmt_e transcodeTarget;
            if (m_supportsBC7)
                transcodeTarget = KTX_TTF_BC7_RGBA;
            else if (m_supportsASTC4x4)
                transcodeTarget = KTX_TTF_ASTC_4x4_RGBA;
            else
                transcodeTarget = KTX_TTF_RGBA32;

            if (ktxTexture2_TranscodeBasis(ktx, transcodeTarget, 0) != KTX_SUCCESS) {
                std::fprintf(stderr, "[VkResources] KTX2 transcode failed (%.*s)\n", static_cast<int>(desc.name.size()),
                             desc.name.data());
                ktxTexture_Destroy(reinterpret_cast<ktxTexture*>(ktx));
                return m_defaultWhite;
            }
        }

        const uint32_t w = ktx->baseWidth;
        const uint32_t h = ktx->baseHeight;
        const uint32_t numMips = ktx->numLevels;
        const uint8_t* data = ktxTexture_GetData(reinterpret_cast<ktxTexture*>(ktx));
        const VkDeviceSize size = ktxTexture_GetDataSize(reinterpret_cast<ktxTexture*>(ktx));

        VkFormat fmt;
        bool compressed;
        if (m_supportsBC7) {
            fmt = desc.srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
            compressed = true;
        } else if (m_supportsASTC4x4) {
            fmt = desc.srgb ? VK_FORMAT_ASTC_4x4_SRGB_BLOCK : VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
            compressed = true;
        } else {
            fmt = desc.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            compressed = false;
        }

        if (compressed)
            ok = createGpuImageCompressed(data, size, w, h, numMips, fmt, ktx, tex);
        else
            ok = createGpuImage(data, w, h, 1, fmt, tex);

        ktxTexture_Destroy(reinterpret_cast<ktxTexture*>(ktx));

    } else {
        // PNG / JPEG / BMP fallback via stb_image (bundled through tinygltf).
        int w = 0, h = 0, ch = 0;
        uint8_t* pixels = stbi_load_from_memory(desc.bytes.data(), static_cast<int>(desc.bytes.size()), &w, &h, &ch, 4);
        if (!pixels) {
            std::fprintf(stderr, "[VkResources] stb_image load failed (%.*s)\n", static_cast<int>(desc.name.size()),
                         desc.name.data());
            return m_defaultWhite;
        }
        const VkFormat fmt = desc.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        ok = createGpuImage(pixels, static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1, fmt, tex);
        stbi_image_free(pixels);
    }

    if (!ok)
        return m_defaultWhite;

    tex.alive = true;

    uint32_t slot = 0;
    if (!m_freeTextureSlots.empty()) {
        slot = m_freeTextureSlots.back();
        m_freeTextureSlots.pop_back();
        m_textures[slot] = tex;
    } else {
        slot = static_cast<uint32_t>(m_textures.size());
        m_textures.push_back(tex);
    }
    return TextureHandle{slot + 1};
}

// ---------------------------------------------------------------------------
// createMaterial — allocate descriptor set, write base color binding
// ---------------------------------------------------------------------------

MaterialHandle VkResourceManager::createMaterial(const MaterialDesc& desc) {
    GpuMaterial mat{};
    mat.baseColorTexture = desc.baseColorTexture;
    mat.normalTexture = desc.normalTexture;
    mat.ormTexture = desc.ormTexture;
    mat.baseColorFactor = desc.baseColorFactor;
    mat.metallicFactor = desc.metallicFactor;
    mat.roughnessFactor = desc.roughnessFactor;
    mat.alphaBlend = desc.alphaBlend;

    VkDescriptorSetAllocateInfo allocCI{};
    allocCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocCI.descriptorPool = m_matPool;
    allocCI.descriptorSetCount = 1;
    allocCI.pSetLayouts = &m_matSetLayout;
    if (vkAllocateDescriptorSets(m_device, &allocCI, &mat.descriptorSet) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkResources] material descriptor set allocation failed\n");
        return {};
    }

    auto resolveOrDefault = [&](TextureHandle h, TextureHandle def) -> const GpuTexture* {
        const GpuTexture* t = getTexture(h.valid() ? h : def);
        return t ? t : getTexture(def);
    };

    const GpuTexture* baseColorGpu = resolveOrDefault(desc.baseColorTexture, m_defaultWhite);
    const GpuTexture* normalGpu = resolveOrDefault(desc.normalTexture, m_defaultFlatNormal);
    const GpuTexture* ormGpu = resolveOrDefault(desc.ormTexture, m_defaultWhiteLinear);

    auto makeImageInfo = [](const GpuTexture* t) {
        VkDescriptorImageInfo info{};
        info.sampler = t->sampler;
        info.imageView = t->view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return info;
    };

    const VkDescriptorImageInfo imgInfos[3] = {
        makeImageInfo(baseColorGpu),
        makeImageInfo(normalGpu),
        makeImageInfo(ormGpu),
    };

    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = mat.descriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imgInfos[i];
    }
    vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);

    mat.alive = true;

    uint32_t slot = 0;
    if (!m_freeMaterialSlots.empty()) {
        slot = m_freeMaterialSlots.back();
        m_freeMaterialSlots.pop_back();
        m_materials[slot] = mat;
    } else {
        slot = static_cast<uint32_t>(m_materials.size());
        m_materials.push_back(mat);
    }
    return MaterialHandle{slot + 1};
}

// ---------------------------------------------------------------------------
// Destroy methods — deferred cleanup
// ---------------------------------------------------------------------------

void VkResourceManager::destroyMesh(MeshHandle h) {
    if (!h.valid() || h.id > m_meshes.size())
        return;
    GpuMesh& mesh = m_meshes[h.id - 1];
    if (!mesh.alive)
        return;
    mesh.alive = false;

    const uint64_t release = m_frame + kDeferFrames;
    if (mesh.vertexBuffer != VK_NULL_HANDLE)
        m_deferredBuffers.push_back({mesh.vertexBuffer, mesh.vertexAlloc, release});
    if (mesh.indexBuffer != VK_NULL_HANDLE)
        m_deferredBuffers.push_back({mesh.indexBuffer, mesh.indexAlloc, release});
    mesh = {};

    m_freeMeshSlots.push_back(h.id - 1);
}

void VkResourceManager::destroyTexture(TextureHandle h) {
    if (!h.valid() || h.id > m_textures.size())
        return;
    if (h.id == m_defaultWhite.id || h.id == m_defaultFlatNormal.id || h.id == m_defaultWhiteLinear.id)
        return; // never destroy default textures
    GpuTexture& tex = m_textures[h.id - 1];
    if (!tex.alive)
        return;
    tex.alive = false;

    m_deferredImages.push_back({tex.image, tex.alloc, tex.view, tex.sampler, m_frame + kDeferFrames});
    tex = {};

    m_freeTextureSlots.push_back(h.id - 1);
}

void VkResourceManager::destroyMaterial(MaterialHandle h) {
    if (!h.valid() || h.id > m_materials.size())
        return;
    GpuMaterial& mat = m_materials[h.id - 1];
    if (!mat.alive)
        return;
    mat.alive = false;

    if (mat.descriptorSet != VK_NULL_HANDLE)
        vkFreeDescriptorSets(m_device, m_matPool, 1, &mat.descriptorSet);
    mat = {};

    m_freeMaterialSlots.push_back(h.id - 1);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

const GpuMesh* VkResourceManager::getMesh(MeshHandle h) const {
    if (!h.valid() || h.id > m_meshes.size())
        return nullptr;
    const GpuMesh& m = m_meshes[h.id - 1];
    return m.alive ? &m : nullptr;
}

const GpuTexture* VkResourceManager::getTexture(TextureHandle h) const {
    if (!h.valid() || h.id > m_textures.size())
        return nullptr;
    const GpuTexture& t = m_textures[h.id - 1];
    return t.alive ? &t : nullptr;
}

const GpuMaterial* VkResourceManager::getMaterial(MaterialHandle h) const {
    if (!h.valid() || h.id > m_materials.size())
        return nullptr;
    const GpuMaterial& m = m_materials[h.id - 1];
    return m.alive ? &m : nullptr;
}
