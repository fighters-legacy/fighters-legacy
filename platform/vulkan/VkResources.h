// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Concrete backend header — not a HAL interface file. Consumers hold IRenderer*
// and never include this directly.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

// VMA must be included before vulkan.h so its typedefs win.
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "RenderTypes.h"
#include <vector>

struct MeshUploadDesc;
struct TextureUploadDesc;
struct MaterialDesc;

// ---------------------------------------------------------------------------
// GPU vertex layout — interleaved, matches mesh.vert attribute locations.
// ---------------------------------------------------------------------------
struct Vertex {
    glm::vec3 position; // location 0
    glm::vec3 normal;   // location 1
    glm::vec4 tangent;  // location 2 (w = handedness ±1)
    glm::vec2 uv;       // location 3
};
static_assert(sizeof(Vertex) == 48);

// ---------------------------------------------------------------------------
// GPU-resident resources
// ---------------------------------------------------------------------------

struct GpuMesh {
    VkBuffer vertexBuffer{VK_NULL_HANDLE};
    VmaAllocation vertexAlloc{};
    VkBuffer indexBuffer{VK_NULL_HANDLE};
    VmaAllocation indexAlloc{};
    uint32_t indexCount{0};
    bool alive{false};
};

struct GpuTexture {
    VkImage image{VK_NULL_HANDLE};
    VmaAllocation alloc{};
    VkImageView view{VK_NULL_HANDLE};
    VkSampler sampler{VK_NULL_HANDLE};
    bool alive{false};
};

struct GpuMaterial {
    TextureHandle baseColorTexture{};
    TextureHandle normalTexture{};
    TextureHandle ormTexture{};
    glm::vec4 baseColorFactor{1.0f};
    float metallicFactor{0.0f};
    float roughnessFactor{1.0f};
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    bool alphaBlend{false}; // true = alpha-blended transparent pass
    bool alive{false};
};

// ---------------------------------------------------------------------------
// VkResourceManager
//
// Owns all GPU-resident meshes, textures, and materials for the renderer.
// Upload happens synchronously on the graphics queue (staging → device-local).
// Destroyed resources are queued for deferred cleanup; call tick() every frame.
// ---------------------------------------------------------------------------
class VkResourceManager {
  public:
    bool init(VkDevice device, VkPhysicalDevice physDevice, VkInstance instance, VkCommandPool cmdPool,
              VkQueue graphicsQueue, VkDescriptorSetLayout matSetLayout);
    void shutdown();

    // Advance the frame counter and flush deferred deletions that are safe.
    void tick(uint64_t frameIndex);

    MeshHandle createMesh(const MeshUploadDesc& desc);
    TextureHandle createTexture(const TextureUploadDesc& desc);
    MaterialHandle createMaterial(const MaterialDesc& desc);

    void destroyMesh(MeshHandle h);
    void destroyTexture(TextureHandle h);
    void destroyMaterial(MaterialHandle h);

    const GpuMesh* getMesh(MeshHandle h) const;
    const GpuTexture* getTexture(TextureHandle h) const;
    const GpuMaterial* getMaterial(MaterialHandle h) const;

    // Default textures bound when a material omits a texture slot.
    TextureHandle defaultWhiteTexture() const {
        return m_defaultWhite;
    }
    TextureHandle defaultFlatNormalTexture() const {
        return m_defaultFlatNormal;
    }
    TextureHandle defaultWhiteLinearTexture() const {
        return m_defaultWhiteLinear;
    }

    // Format support queried at init; used to select Basis transcode target.
    bool supportsBC7() const {
        return m_supportsBC7;
    }
    bool supportsASTC4x4() const {
        return m_supportsASTC4x4;
    }

  private:
    // ── Upload helpers ────────────────────────────────────────────────────
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memUsage, VkBuffer& buf,
                      VmaAllocation& alloc);
    bool uploadBuffer(VkBuffer dst, const void* src, VkDeviceSize size);
    bool createGpuImage(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t mipLevels, VkFormat format,
                        GpuTexture& tex);
    bool createDefaultSampler(VkSampler& out);
    bool createDefaultWhiteTexture();
    bool createDefaultFlatNormalTexture();
    bool createDefaultWhiteLinearTexture();
    // Upload a pre-transcoded block-compressed KTX2 texture with all mip levels.
    // ktxHandle is a ktxTexture2* cast to void* to avoid exposing ktx.h in this header.
    bool createGpuImageCompressed(const uint8_t* data, VkDeviceSize dataSize, uint32_t width, uint32_t height,
                                  uint32_t numMips, VkFormat format, void* ktxHandle, GpuTexture& tex);

    // One-shot command buffer for transfers.
    VkCommandBuffer beginOneShot();
    void endOneShot(VkCommandBuffer cmd);

    // ── Deferred deletion ─────────────────────────────────────────────────
    static constexpr uint32_t kDeferFrames = 2; // MAX_FRAMES_IN_FLIGHT

    struct DeferredBuffer {
        VkBuffer buf;
        VmaAllocation alloc;
        uint64_t releaseAfterFrame;
    };
    struct DeferredImage {
        VkImage image;
        VmaAllocation alloc;
        VkImageView view;
        VkSampler sampler;
        uint64_t releaseAfterFrame;
    };

    void flushDeferred();

    // ── Members ───────────────────────────────────────────────────────────
    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physDevice{VK_NULL_HANDLE};
    VmaAllocator m_allocator{};
    VkCommandPool m_cmdPool{VK_NULL_HANDLE};
    VkQueue m_queue{VK_NULL_HANDLE};
    VkDescriptorPool m_matPool{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_matSetLayout{VK_NULL_HANDLE};
    uint64_t m_frame{0};

    TextureHandle m_defaultWhite{};
    TextureHandle m_defaultFlatNormal{};  // 1×1 UNORM {128,128,255,255} flat normal
    TextureHandle m_defaultWhiteLinear{}; // 1×1 UNORM {255,255,255,255} for ORM
    bool m_supportsBC7{false};
    bool m_supportsASTC4x4{false};

    std::vector<GpuMesh> m_meshes;
    std::vector<GpuTexture> m_textures;
    std::vector<GpuMaterial> m_materials;

    std::vector<uint32_t> m_freeMeshSlots;
    std::vector<uint32_t> m_freeTextureSlots;
    std::vector<uint32_t> m_freeMaterialSlots;

    std::vector<DeferredBuffer> m_deferredBuffers;
    std::vector<DeferredImage> m_deferredImages;
};
