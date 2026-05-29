// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Concrete backend header — not a HAL interface file. Platform-specific headers
// are permitted here. Consumers hold IRenderer* and never include this directly.
#include "IRenderer.h"
#include "VkResources.h"
#include <array>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

struct SDL_Window;

static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

// Depth format: D32_SFLOAT, reverse-Z (near→1.0, far→0.0; clear = 0.0; compare = GREATER).
static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
// HDR offscreen color format.
static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// GPU-side camera UBO layout — matches set 0, binding 0 in mesh.vert.
struct CameraUBO {
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::vec4 worldOrigin{0.0f}; // xyz = origin, w = unused
};

// GPU-side light UBO layout — matches set 0, binding 1 in mesh.frag.
struct LightUBO {
    glm::vec4 sunDirection{0.0f, -1.0f, 0.0f, 0.0f}; // xyz = dir toward sun
    glm::vec4 sunColor{1.0f, 0.95f, 0.8f, 1.0f};     // xyz = color, w = intensity
    glm::vec4 ambientColor{0.1f, 0.12f, 0.15f, 0.0f};
};

// Push constant block for the forward pass — must be ≤ 128 bytes.
struct ForwardPushConstants {
    glm::mat4 model{1.0f};           // 64 bytes
    glm::vec4 baseColorFactor{1.0f}; // 16 bytes
    float metallicFactor{0.0f};
    float roughnessFactor{1.0f};
    float _pad[2]{}; // 8 bytes — total = 96
};
static_assert(sizeof(ForwardPushConstants) <= 128);

class VkRenderer : public IRenderer {
  public:
    bool init(IWindow* window) override;
    void onResize(int width, int height) override;
    void beginFrame() override;
    void endFrame() override;
    void shutdown() override;
    const char* getLastError() const override;
    const char* gpuInfo() const override;

    // ── Resource methods ───────────────────────────────────────────────────
    MeshHandle createMesh(const MeshUploadDesc& desc) override;
    TextureHandle createTexture(const TextureUploadDesc& desc) override;
    MaterialHandle createMaterial(const MaterialDesc& desc) override;
    void destroyMesh(MeshHandle h) override;
    void destroyTexture(TextureHandle h) override;
    void destroyMaterial(MaterialHandle h) override;

    // ── Scene submission ───────────────────────────────────────────────────
    void setScene(const FrameScene& scene) override;

  private:
    // ── Core Vulkan objects ────────────────────────────────────────────────
    bool createInstance();
    bool setupDebugMessenger();
    bool createSurface();
    bool pickPhysicalDevice();
    bool createLogicalDevice();

    // ── Swapchain ──────────────────────────────────────────────────────────
    bool createSwapchain(int width, int height);
    bool createImageViews();
    bool recreateSwapchain();
    void destroyImageViews();
    void cleanupSwapchain();

    // ── Attachments (depth + HDR) ──────────────────────────────────────────
    bool createDepthImage();
    bool createHdrImage();
    bool createHdrSampler();
    void destroyAttachments();

    bool createAttachmentImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                               VkImageAspectFlags aspect, VkImage& image, VkDeviceMemory& memory, VkImageView& view);
    static uint32_t findMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags props);

    // ── Tonemap descriptor ─────────────────────────────────────────────────
    bool createTonemapDescriptors();
    void updateHdrDescriptor();

    // ── Per-frame UBO descriptors ──────────────────────────────────────────
    bool createPerFrameDescriptorLayout();
    bool createMaterialDescriptorLayout();
    bool createPerFrameDescriptors();

    // Write camera + light data into the current frame's UBO buffers.
    void writeFrameUBOs(const FrameScene& scene);

    // ── Pipelines ──────────────────────────────────────────────────────────
    bool createPipelineCache();
    bool createForwardPipeline();
    bool createTonemapPipeline();

    // ── Commands + sync ────────────────────────────────────────────────────
    bool createCommandPool();
    bool allocateCommandBuffers();
    bool createSyncObjects();

    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex);

    // ── Shader / resource discovery ────────────────────────────────────────
    static std::string resolveShaderDir();

    // ── Instance / surface ────────────────────────────────────────────────
    VkInstance m_instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT m_debugMessenger{VK_NULL_HANDLE};
    VkSurfaceKHR m_surface{VK_NULL_HANDLE};

    // ── Physical / logical device ─────────────────────────────────────────
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    uint32_t m_graphicsFamily{0};
    uint32_t m_presentFamily{0};
    bool m_sameQueueFamily{false};
    VkDevice m_device{VK_NULL_HANDLE};
    VkQueue m_graphicsQueue{VK_NULL_HANDLE};
    VkQueue m_presentQueue{VK_NULL_HANDLE};

    // ── Swapchain ─────────────────────────────────────────────────────────
    VkSwapchainKHR m_swapchain{VK_NULL_HANDLE};
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;
    VkFormat m_swapchainFormat{VK_FORMAT_UNDEFINED};
    VkExtent2D m_swapchainExtent{};

    // ── Depth attachment ──────────────────────────────────────────────────
    VkImage m_depthImage{VK_NULL_HANDLE};
    VkDeviceMemory m_depthMemory{VK_NULL_HANDLE};
    VkImageView m_depthView{VK_NULL_HANDLE};

    // ── HDR offscreen attachment ──────────────────────────────────────────
    VkImage m_hdrImage{VK_NULL_HANDLE};
    VkDeviceMemory m_hdrMemory{VK_NULL_HANDLE};
    VkImageView m_hdrView{VK_NULL_HANDLE};
    VkSampler m_hdrSampler{VK_NULL_HANDLE};

    // ── Tonemap descriptor ────────────────────────────────────────────────
    VkDescriptorSetLayout m_tonemapSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_tonemapPool{VK_NULL_HANDLE};
    VkDescriptorSet m_tonemapSet{VK_NULL_HANDLE};

    // ── Per-frame descriptor set layout (set 0: camera + light UBOs) ─────
    VkDescriptorSetLayout m_perFrameSetLayout{VK_NULL_HANDLE};

    // ── Per-material descriptor set layout (set 1: base color sampler) ───
    VkDescriptorSetLayout m_matSetLayout{VK_NULL_HANDLE};

    // ── Per-frame UBO buffers + descriptor sets ───────────────────────────
    // Uses raw Vulkan memory (not VMA): small host-visible buffers that change
    // every frame don't benefit from VMA sub-allocation.
    struct PerFrameData {
        VkBuffer cameraBuffer{VK_NULL_HANDLE};
        VkDeviceMemory cameraMemory{VK_NULL_HANDLE};
        void* cameraMapped{nullptr};

        VkBuffer lightBuffer{VK_NULL_HANDLE};
        VkDeviceMemory lightMemory{VK_NULL_HANDLE};
        void* lightMapped{nullptr};

        VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    };
    std::array<PerFrameData, MAX_FRAMES_IN_FLIGHT> m_perFrame{};
    VkDescriptorPool m_perFramePool{VK_NULL_HANDLE};

    // ── Pipelines ─────────────────────────────────────────────────────────
    VkPipelineCache m_pipelineCache{VK_NULL_HANDLE};
    VkPipelineLayout m_forwardLayout{VK_NULL_HANDLE};
    VkPipeline m_forwardPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_tonemapLayout{VK_NULL_HANDLE};
    VkPipeline m_tonemapPipeline{VK_NULL_HANDLE};

    // ── Commands ──────────────────────────────────────────────────────────
    VkCommandPool m_commandPool{VK_NULL_HANDLE};
    std::array<VkCommandBuffer, MAX_FRAMES_IN_FLIGHT> m_commandBuffers{};

    // ── Synchronisation ───────────────────────────────────────────────────
    std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> m_imageAvailable{};
    std::vector<VkSemaphore> m_renderFinished;
    std::array<VkFence, MAX_FRAMES_IN_FLIGHT> m_inFlightFences{};
    std::vector<VkFence> m_imagesInFlight;

    // ── Frame state ───────────────────────────────────────────────────────
    uint32_t m_currentFrame{0};
    uint32_t m_currentImageIndex{0};
    uint64_t m_totalFrames{0};
    bool m_framebufferResized{false};
    bool m_frameAcquired{false};

    // Scene submitted this frame (set by setScene, consumed by recordCommandBuffer).
    FrameScene m_pendingScene{};

    // ── GPU resource manager ──────────────────────────────────────────────
    VkResourceManager m_resources;

    SDL_Window* m_sdlWindow{nullptr};
    std::string m_shaderDir;
    mutable std::string m_lastError;
    std::string m_gpuInfo;
};
