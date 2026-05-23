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

class VkRenderer : public IRenderer {
  public:
    bool init(IWindow* window) override;
    void onResize(int width, int height) override;
    void beginFrame() override;
    void endFrame() override;
    void shutdown() override;
    const char* getLastError() const override;

  private:
    bool createInstance();
    bool setupDebugMessenger();
    bool createSurface();
    bool pickPhysicalDevice();
    bool createLogicalDevice();
    bool createSwapchain(int width, int height);
    bool createImageViews();
    bool createRenderPass();
    bool createDescriptorSetLayout();
    bool createGraphicsPipeline();
    bool createFramebuffers();
    bool createCommandPool();
    bool allocateCommandBuffers();
    bool createSyncObjects();

    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex);
    bool recreateSwapchain();
    void cleanupSwapchain();
    void destroyFramebuffersAndViews();

    // Instance / surface
    VkInstance m_instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT m_debugMessenger{VK_NULL_HANDLE};
    VkSurfaceKHR m_surface{VK_NULL_HANDLE};

    // Physical / logical device
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    uint32_t m_graphicsFamily{0};
    uint32_t m_presentFamily{0};
    bool m_sameQueueFamily{false};
    VkDevice m_device{VK_NULL_HANDLE};
    VkQueue m_graphicsQueue{VK_NULL_HANDLE};
    VkQueue m_presentQueue{VK_NULL_HANDLE};

    // Swapchain
    VkSwapchainKHR m_swapchain{VK_NULL_HANDLE};
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;
    VkFormat m_swapchainFormat{VK_FORMAT_UNDEFINED};
    VkExtent2D m_swapchainExtent{};

    // Render pass / pipeline
    VkRenderPass m_renderPass{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_graphicsPipeline{VK_NULL_HANDLE};

    // Framebuffers
    std::vector<VkFramebuffer> m_framebuffers;

    // Commands
    VkCommandPool m_commandPool{VK_NULL_HANDLE};
    std::array<VkCommandBuffer, MAX_FRAMES_IN_FLIGHT> m_commandBuffers{};

    // Synchronisation
    std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> m_imageAvailable{};
    std::vector<VkSemaphore> m_renderFinished;
    std::array<VkFence, MAX_FRAMES_IN_FLIGHT> m_inFlightFences{};
    std::vector<VkFence> m_imagesInFlight;

    // Frame state
    uint32_t m_currentFrame{0};
    uint32_t m_currentImageIndex{0};
    bool m_framebufferResized{false};
    bool m_frameAcquired{false};

    SDL_Window* m_sdlWindow{nullptr};
    mutable std::string m_lastError;
};
