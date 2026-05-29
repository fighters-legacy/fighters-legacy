// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Concrete backend header — not a HAL interface file. Platform-specific headers
// are permitted here. Consumers hold IRenderer* and never include this directly.
#include "IRenderer.h"
#include <array>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

struct SDL_Window;

static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

// Depth format used throughout: D32_SFLOAT with reverse-Z
// (near→1.0, far→0.0; depth clear = 0.0; compare = GREATER).
static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

// HDR offscreen color format.
static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

class VkRenderer : public IRenderer {
  public:
    bool init(IWindow* window) override;
    void onResize(int width, int height) override;
    void beginFrame() override;
    void endFrame() override;
    void shutdown() override;
    const char* getLastError() const override;
    const char* gpuInfo() const override;

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

    // Helper: allocate a device-local image + view.
    bool createAttachmentImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                               VkImageAspectFlags aspect, VkImage& image, VkDeviceMemory& memory, VkImageView& view);

    static uint32_t findMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags props);

    // ── Tonemap descriptor ─────────────────────────────────────────────────
    bool createTonemapDescriptors();
    void updateHdrDescriptor(); // call after (re)creating HDR image

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

    // ── Depth attachment (reverse-Z, D32_SFLOAT) ──────────────────────────
    VkImage m_depthImage{VK_NULL_HANDLE};
    VkDeviceMemory m_depthMemory{VK_NULL_HANDLE};
    VkImageView m_depthView{VK_NULL_HANDLE};

    // ── HDR offscreen attachment (RGBA16F) ────────────────────────────────
    VkImage m_hdrImage{VK_NULL_HANDLE};
    VkDeviceMemory m_hdrMemory{VK_NULL_HANDLE};
    VkImageView m_hdrView{VK_NULL_HANDLE};
    VkSampler m_hdrSampler{VK_NULL_HANDLE};

    // ── Tonemap descriptor (HDR sampler → swapchain) ─────────────────────
    VkDescriptorSetLayout m_tonemapSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_tonemapPool{VK_NULL_HANDLE};
    VkDescriptorSet m_tonemapSet{VK_NULL_HANDLE};

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
    bool m_framebufferResized{false};
    bool m_frameAcquired{false};

    SDL_Window* m_sdlWindow{nullptr};
    std::string m_shaderDir; // resolved at init(); used by all shader loads
    mutable std::string m_lastError;
    std::string m_gpuInfo;
};
