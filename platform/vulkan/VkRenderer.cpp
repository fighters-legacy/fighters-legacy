// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include "VkRenderer.h"
#include "IWindow.h"
#include "VkWindow.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::vector<uint32_t> loadSpirv(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return {};
    const auto size = static_cast<std::streamsize>(file.tellg());
    file.seekg(0);
    std::vector<uint32_t> buf(static_cast<std::size_t>(size) / sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

#if defined(FL_VK_VALIDATION)
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                    void* /*userData*/) {
    const char* prefix = (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)     ? "[VK ERROR]"
                         : (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "[VK WARN] "
                                                                                         : "[VK INFO] ";
    std::fprintf(stderr, "%s %s\n", prefix, data->pMessage);
    return VK_FALSE;
}

static void fillDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& ci) {
    ci = {};
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debugCallback;
}
#endif

// ---------------------------------------------------------------------------
// IRenderer interface
// ---------------------------------------------------------------------------

bool VkRenderer::init(IWindow* window) {
    m_sdlWindow = static_cast<SDL_Window*>(window->nativeHandle());

    if (!createInstance()) {
        return false;
    }
    if (!setupDebugMessenger()) {
        return false;
    }
    if (!createSurface()) {
        return false;
    }
    if (!pickPhysicalDevice()) {
        return false;
    }
    if (!createLogicalDevice()) {
        return false;
    }
    if (!createSwapchain(window->width(), window->height())) {
        return false;
    }
    if (!createImageViews()) {
        return false;
    }
    if (!createRenderPass()) {
        return false;
    }
    if (!createDescriptorSetLayout()) {
        return false;
    }
    if (!createGraphicsPipeline()) {
        return false;
    }
    if (!createFramebuffers()) {
        return false;
    }
    if (!createCommandPool()) {
        return false;
    }
    if (!allocateCommandBuffers()) {
        return false;
    }
    if (!createSyncObjects()) {
        return false;
    }
    return true;
}

void VkRenderer::onResize(int /*width*/, int /*height*/) {
    m_framebufferResized = true;
}

void VkRenderer::beginFrame() {
    m_frameAcquired = false;

    // Poll window size every frame. On some Wayland + SDL3 configurations,
    // SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED is not fired during a live drag — SDL
    // defers it until after the client commits a buffer at the new size, creating a
    // chicken-and-egg: we wait for the event to recreate; SDL waits for a commit to
    // fire the event. Comparing the SDL-cached pixel size (updated by SDL when it
    // processes Wayland configure events, before any commit) with the current
    // swapchain extent detects the change without relying on the event.
    if (m_swapchain != VK_NULL_HANDLE) {
        int w = 0, h = 0;
        if (SDL_GetWindowSizeInPixels(m_sdlWindow, &w, &h) && w > 0 && h > 0 &&
            (static_cast<uint32_t>(w) != m_swapchainExtent.width ||
             static_cast<uint32_t>(h) != m_swapchainExtent.height)) {
            m_framebufferResized = true;
        }
    }

    if (m_framebufferResized) {
        // recreateSwapchain returns false when the window is zero-sized (minimised or
        // a Wayland configure(0,0) "choose your size" event). Re-set the flag so the
        // main loop retries next iteration after pollEvents drains pending events.
        if (!recreateSwapchain()) {
            m_framebufferResized = true;
            return;
        }
        m_framebufferResized = false;
    }

    // 100 ms timeout keeps the event loop responsive; VK_TIMEOUT → early return
    // so pollEvents() runs again before we re-enter the fence wait.
    if (vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, 100'000'000ULL) == VK_TIMEOUT)
        return;

    VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, std::numeric_limits<uint64_t>::max(),
                                            m_imageAvailable[m_currentFrame], VK_NULL_HANDLE, &m_currentImageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // Don't call recreateSwapchain() here — that would stack a second
        // vkDeviceWaitIdle in this iteration. Set the flag and let the main loop run
        // pollEvents before the next recreate attempt.
        m_framebufferResized = true;
        return;
    }
    if (result == VK_SUBOPTIMAL_KHR) {
        m_framebufferResized = true;
    }

    vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);

    if (m_imagesInFlight[m_currentImageIndex] != VK_NULL_HANDLE)
        vkWaitForFences(m_device, 1, &m_imagesInFlight[m_currentImageIndex], VK_TRUE, 100'000'000ULL);
    m_imagesInFlight[m_currentImageIndex] = m_inFlightFences[m_currentFrame];

    vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0);
    recordCommandBuffer(m_commandBuffers[m_currentFrame], m_currentImageIndex);

    m_frameAcquired = true;
}

void VkRenderer::endFrame() {
    if (!m_frameAcquired)
        return;

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &m_imageAvailable[m_currentFrame];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m_commandBuffers[m_currentFrame];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &m_renderFinished[m_currentImageIndex];
    vkQueueSubmit(m_graphicsQueue, 1, &si, m_inFlightFences[m_currentFrame]);

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &m_renderFinished[m_currentImageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &m_swapchain;
    pi.pImageIndices = &m_currentImageIndex;
    const VkResult result = vkQueuePresentKHR(m_presentQueue, &pi);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        m_framebufferResized = true;
    }

    m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VkRenderer::shutdown() {
    if (m_device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(m_device);

    cleanupSwapchain();

    if (m_graphicsPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(m_device, m_graphicsPipeline, nullptr);
    if (m_pipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_descriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
    if (m_renderPass != VK_NULL_HANDLE)
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (m_imageAvailable[i] != VK_NULL_HANDLE)
            vkDestroySemaphore(m_device, m_imageAvailable[i], nullptr);
        if (m_inFlightFences[i] != VK_NULL_HANDLE)
            vkDestroyFence(m_device, m_inFlightFences[i], nullptr);
    }
    for (auto sem : m_renderFinished)
        if (sem != VK_NULL_HANDLE)
            vkDestroySemaphore(m_device, sem, nullptr);
    m_renderFinished.clear();

    if (m_commandPool != VK_NULL_HANDLE)
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);

    if (m_device != VK_NULL_HANDLE)
        vkDestroyDevice(m_device, nullptr);

    if (m_surface != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);

#if defined(FL_VK_VALIDATION)
    if (m_debugMessenger != VK_NULL_HANDLE) {
        auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (fn)
            fn(m_instance, m_debugMessenger, nullptr);
    }
#endif

    if (m_instance != VK_NULL_HANDLE)
        vkDestroyInstance(m_instance, nullptr);

    // Reset handles so shutdown() is safe to call multiple times
    m_graphicsPipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_descriptorSetLayout = VK_NULL_HANDLE;
    m_renderPass = VK_NULL_HANDLE;
    m_commandPool = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_surface = VK_NULL_HANDLE;
    m_debugMessenger = VK_NULL_HANDLE;
    m_instance = VK_NULL_HANDLE;
}

const char* VkRenderer::getLastError() const {
    return m_lastError.empty() ? nullptr : m_lastError.c_str();
}

// ---------------------------------------------------------------------------
// createInstance
// ---------------------------------------------------------------------------

bool VkRenderer::createInstance() {
    // Check validation layer availability
    std::vector<const char*> layers;
#if defined(FL_VK_VALIDATION)
    {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> available(count);
        vkEnumerateInstanceLayerProperties(&count, available.data());
        bool found = false;
        for (const auto& l : available)
            if (std::string_view(l.layerName) == "VK_LAYER_KHRONOS_validation") {
                found = true;
                break;
            }
        if (found)
            layers.push_back("VK_LAYER_KHRONOS_validation");
        else
            std::fprintf(stderr, "[VK WARN] VK_LAYER_KHRONOS_validation not available\n");
    }
#endif

    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = SDL_GetWindowTitle(m_sdlWindow);
    ai.applicationVersion = VK_MAKE_VERSION(0, 0, 1);
    ai.pEngineName = "fighters-legacy";
    ai.engineVersion = VK_MAKE_VERSION(0, 0, 1);
    ai.apiVersion = VK_API_VERSION_1_3;

    auto exts = vk_getRequiredInstanceExtensions(m_sdlWindow);

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.empty() ? nullptr : exts.data();

#if defined(__APPLE__)
    ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

#if defined(FL_VK_VALIDATION)
    VkDebugUtilsMessengerCreateInfoEXT debugCi{};
    fillDebugMessengerCreateInfo(debugCi);
    ci.pNext = &debugCi;
#endif

    if (vkCreateInstance(&ci, nullptr, &m_instance) != VK_SUCCESS) {
        m_lastError = "vkCreateInstance failed";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// setupDebugMessenger
// ---------------------------------------------------------------------------

bool VkRenderer::setupDebugMessenger() {
#if defined(FL_VK_VALIDATION)
    VkDebugUtilsMessengerCreateInfoEXT ci{};
    fillDebugMessengerCreateInfo(ci);
    auto fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
    if (!fn || fn(m_instance, &ci, nullptr, &m_debugMessenger) != VK_SUCCESS) {
        std::fprintf(stderr, "[VK WARN] Debug messenger creation failed\n");
    }
#endif
    return true;
}

// ---------------------------------------------------------------------------
// createSurface
// ---------------------------------------------------------------------------

bool VkRenderer::createSurface() {
    m_surface = vk_createSurface(m_instance, m_sdlWindow);
    if (m_surface == VK_NULL_HANDLE) {
        m_lastError = "SDL_Vulkan_CreateSurface failed";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// pickPhysicalDevice
// ---------------------------------------------------------------------------

static bool checkDeviceExtension(VkPhysicalDevice dev, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, exts.data());
    for (const auto& e : exts)
        if (std::string_view(e.extensionName) == name)
            return true;
    return false;
}

bool VkRenderer::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) {
        m_lastError = "no Vulkan-capable GPU found";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    auto issuitable = [&](VkPhysicalDevice dev, uint32_t& gf, uint32_t& pf) {
        if (!checkDeviceExtension(dev, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            return false;
#if defined(__APPLE__)
        if (!checkDeviceExtension(dev, "VK_KHR_portability_subset"))
            return false;
#endif
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, qprops.data());

        bool foundG = false, foundP = false;
        for (uint32_t i = 0; i < qcount; ++i) {
            if (!foundG && (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                gf = i;
                foundG = true;
            }
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, m_surface, &present);
            if (!foundP && present) {
                pf = i;
                foundP = true;
            }
        }
        return foundG && foundP;
    };

    // Prefer discrete GPU, fall back to any suitable device
    VkPhysicalDevice fallback = VK_NULL_HANDLE;
    uint32_t fbGf = 0, fbPf = 0;

    for (auto dev : devices) {
        uint32_t gf = 0, pf = 0;
        if (!issuitable(dev, gf, pf))
            continue;

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            m_physicalDevice = dev;
            m_graphicsFamily = gf;
            m_presentFamily = pf;
            m_sameQueueFamily = (gf == pf);
            std::fprintf(stderr, "[VK] Selected GPU: %s\n", props.deviceName);
            return true;
        }
        if (fallback == VK_NULL_HANDLE) {
            fallback = dev;
            fbGf = gf;
            fbPf = pf;
        }
    }

    if (fallback != VK_NULL_HANDLE) {
        m_physicalDevice = fallback;
        m_graphicsFamily = fbGf;
        m_presentFamily = fbPf;
        m_sameQueueFamily = (fbGf == fbPf);
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        std::fprintf(stderr, "[VK] Selected GPU: %s\n", props.deviceName);
        return true;
    }

    m_lastError = "no suitable Vulkan GPU found";
    return false;
}

// ---------------------------------------------------------------------------
// createLogicalDevice
// ---------------------------------------------------------------------------

bool VkRenderer::createLogicalDevice() {
    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueCIs;

    auto addQueue = [&](uint32_t family) {
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        queueCIs.push_back(qci);
    };
    addQueue(m_graphicsFamily);
    if (!m_sameQueueFamily)
        addQueue(m_presentFamily);

    std::vector<const char*> exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
#if defined(__APPLE__)
    exts.push_back("VK_KHR_portability_subset");
#endif

    VkPhysicalDeviceFeatures features{};

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = static_cast<uint32_t>(queueCIs.size());
    ci.pQueueCreateInfos = queueCIs.data();
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.pEnabledFeatures = &features;

    if (vkCreateDevice(m_physicalDevice, &ci, nullptr, &m_device) != VK_SUCCESS) {
        m_lastError = "vkCreateDevice failed";
        return false;
    }

    vkGetDeviceQueue(m_device, m_graphicsFamily, 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_presentFamily, 0, &m_presentQueue);
    return true;
}

// ---------------------------------------------------------------------------
// createSwapchain
// ---------------------------------------------------------------------------

bool VkRenderer::createSwapchain(int width, int height) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &caps);

    // Surface format
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data());

    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    if (chosen.format == formats[0].format) {
        for (const auto& f : formats) {
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = f;
                break;
            }
        }
    }
    m_swapchainFormat = chosen.format;

    // Present mode
    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &modeCount, modes.data());

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto m : modes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
            presentMode = m;
            break;
        }

    // Extent
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        m_swapchainExtent = caps.currentExtent;
    } else {
        m_swapchainExtent.width =
            std::clamp(static_cast<uint32_t>(width), caps.minImageExtent.width, caps.maxImageExtent.width);
        m_swapchainExtent.height =
            std::clamp(static_cast<uint32_t>(height), caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    // Image count
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    const std::array<uint32_t, 2> queueFamilies = {m_graphicsFamily, m_presentFamily};

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = m_surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = m_swapchainExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = m_swapchain; // VK_NULL_HANDLE on first call

    if (m_sameQueueFamily) {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices = queueFamilies.data();
    }

    if (vkCreateSwapchainKHR(m_device, &ci, nullptr, &m_swapchain) != VK_SUCCESS) {
        m_lastError = "vkCreateSwapchainKHR failed";
        return false;
    }

    uint32_t imgCount = 0;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imgCount, nullptr);
    m_swapchainImages.resize(imgCount);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imgCount, m_swapchainImages.data());
    return true;
}

// ---------------------------------------------------------------------------
// createImageViews
// ---------------------------------------------------------------------------

bool VkRenderer::createImageViews() {
    m_swapchainImageViews.resize(m_swapchainImages.size());
    for (std::size_t i = 0; i < m_swapchainImages.size(); ++i) {
        VkImageViewCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image = m_swapchainImages[i];
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = m_swapchainFormat;
        ci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                         VK_COMPONENT_SWIZZLE_IDENTITY};
        ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ci.subresourceRange.baseMipLevel = 0;
        ci.subresourceRange.levelCount = 1;
        ci.subresourceRange.baseArrayLayer = 0;
        ci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(m_device, &ci, nullptr, &m_swapchainImageViews[i]) != VK_SUCCESS) {
            m_lastError = "vkCreateImageView failed";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// createRenderPass
// ---------------------------------------------------------------------------

bool VkRenderer::createRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = m_swapchainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments = &colorAttachment;
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;

    if (vkCreateRenderPass(m_device, &ci, nullptr, &m_renderPass) != VK_SUCCESS) {
        m_lastError = "vkCreateRenderPass failed";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// createDescriptorSetLayout (skeleton — 0 bindings)
// ---------------------------------------------------------------------------

bool VkRenderer::createDescriptorSetLayout() {
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = 0;
    ci.pBindings = nullptr;
    if (vkCreateDescriptorSetLayout(m_device, &ci, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        m_lastError = "vkCreateDescriptorSetLayout failed";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// createGraphicsPipeline
// ---------------------------------------------------------------------------

static VkShaderModule createShaderModule(VkDevice device, const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size() * sizeof(uint32_t);
    ci.pCode = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &mod);
    return mod;
}

bool VkRenderer::createGraphicsPipeline() {
    const std::string vertPath = std::string(FL_SHADER_DIR) + "/triangle.vert.spv";
    const std::string fragPath = std::string(FL_SHADER_DIR) + "/triangle.frag.spv";

    auto vertCode = loadSpirv(vertPath.c_str());
    auto fragCode = loadSpirv(fragPath.c_str());
    if (vertCode.empty() || fragCode.empty()) {
        m_lastError = "failed to load shader SPIR-V from: " + vertPath;
        return false;
    }

    VkShaderModule vertMod = createShaderModule(m_device, vertCode);
    VkShaderModule fragMod = createShaderModule(m_device, fragCode);

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Dynamic viewport/scissor — counts must still be 1
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    const std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.logicOpEnable = VK_FALSE;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &colorBlendAttachment;

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount = 1;
    layoutCI.pSetLayouts = &m_descriptorSetLayout;
    if (vkCreatePipelineLayout(m_device, &layoutCI, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        m_lastError = "vkCreatePipelineLayout failed";
        vkDestroyShaderModule(m_device, vertMod, nullptr);
        vkDestroyShaderModule(m_device, fragMod, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineCI{};
    pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCI.stageCount = 2;
    pipelineCI.pStages = stages.data();
    pipelineCI.pVertexInputState = &vertexInput;
    pipelineCI.pInputAssemblyState = &inputAssembly;
    pipelineCI.pViewportState = &viewportState;
    pipelineCI.pRasterizationState = &rasterizer;
    pipelineCI.pMultisampleState = &multisampling;
    pipelineCI.pColorBlendState = &colorBlend;
    pipelineCI.pDynamicState = &dynamicState;
    pipelineCI.layout = m_pipelineLayout;
    pipelineCI.renderPass = m_renderPass;
    pipelineCI.subpass = 0;

    const VkResult result =
        vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineCI, nullptr, &m_graphicsPipeline);
    vkDestroyShaderModule(m_device, vertMod, nullptr);
    vkDestroyShaderModule(m_device, fragMod, nullptr);

    if (result != VK_SUCCESS) {
        m_lastError = "vkCreateGraphicsPipelines failed";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// createFramebuffers
// ---------------------------------------------------------------------------

bool VkRenderer::createFramebuffers() {
    m_framebuffers.resize(m_swapchainImageViews.size());
    for (std::size_t i = 0; i < m_swapchainImageViews.size(); ++i) {
        VkFramebufferCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass = m_renderPass;
        ci.attachmentCount = 1;
        ci.pAttachments = &m_swapchainImageViews[i];
        ci.width = m_swapchainExtent.width;
        ci.height = m_swapchainExtent.height;
        ci.layers = 1;
        if (vkCreateFramebuffer(m_device, &ci, nullptr, &m_framebuffers[i]) != VK_SUCCESS) {
            m_lastError = "vkCreateFramebuffer failed";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// createCommandPool / allocateCommandBuffers
// ---------------------------------------------------------------------------

bool VkRenderer::createCommandPool() {
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = m_graphicsFamily;
    if (vkCreateCommandPool(m_device, &ci, nullptr, &m_commandPool) != VK_SUCCESS) {
        m_lastError = "vkCreateCommandPool failed";
        return false;
    }
    return true;
}

bool VkRenderer::allocateCommandBuffers() {
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = m_commandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
    if (vkAllocateCommandBuffers(m_device, &ai, m_commandBuffers.data()) != VK_SUCCESS) {
        m_lastError = "vkAllocateCommandBuffers failed";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// createSyncObjects
// ---------------------------------------------------------------------------

bool VkRenderer::createSyncObjects() {
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (vkCreateSemaphore(m_device, &sci, nullptr, &m_imageAvailable[i]) != VK_SUCCESS ||
            vkCreateFence(m_device, &fci, nullptr, &m_inFlightFences[i]) != VK_SUCCESS) {
            m_lastError = "sync object creation failed";
            return false;
        }
    }

    m_renderFinished.resize(m_swapchainImages.size());
    for (auto& sem : m_renderFinished) {
        if (vkCreateSemaphore(m_device, &sci, nullptr, &sem) != VK_SUCCESS) {
            m_lastError = "sync object creation failed";
            return false;
        }
    }

    m_imagesInFlight.assign(m_swapchainImages.size(), VK_NULL_HANDLE);
    return true;
}

// ---------------------------------------------------------------------------
// recordCommandBuffer
// ---------------------------------------------------------------------------

void VkRenderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex) {
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    const VkClearValue clearColor{{{0.0f, 0.0f, 0.0f, 1.0f}}};
    VkRenderPassBeginInfo rpi{};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass = m_renderPass;
    rpi.framebuffer = m_framebuffers[imageIndex];
    rpi.renderArea.offset = {0, 0};
    rpi.renderArea.extent = m_swapchainExtent;
    rpi.clearValueCount = 1;
    rpi.pClearValues = &clearColor;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphicsPipeline);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_swapchainExtent.width);
    viewport.height = static_cast<float>(m_swapchainExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, m_swapchainExtent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

// ---------------------------------------------------------------------------
// recreateSwapchain / cleanupSwapchain
// ---------------------------------------------------------------------------

void VkRenderer::destroyFramebuffersAndViews() {
    for (auto fb : m_framebuffers)
        if (fb != VK_NULL_HANDLE)
            vkDestroyFramebuffer(m_device, fb, nullptr);
    m_framebuffers.clear();

    for (auto iv : m_swapchainImageViews)
        if (iv != VK_NULL_HANDLE)
            vkDestroyImageView(m_device, iv, nullptr);
    m_swapchainImageViews.clear();
}

void VkRenderer::cleanupSwapchain() {
    destroyFramebuffersAndViews();
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

bool VkRenderer::recreateSwapchain() {
    // Check window size without blocking. Returns false when the window is zero-sized
    // (minimised, or a Wayland configure(0,0) "you choose the size" event). The caller
    // re-sets m_framebufferResized and returns to the main loop so pollEvents() can
    // drain pending Wayland events before the next attempt — avoiding the compositor
    // deadlock where we block waiting for events that won't come until we commit.
    int w = 0, h = 0;
    if (!SDL_GetWindowSizeInPixels(m_sdlWindow, &w, &h))
        SDL_GetWindowSize(m_sdlWindow, &w, &h);
    if (w == 0 || h == 0)
        return false;

    // vkDeviceWaitIdle drains both GPU command queues and the presentation engine,
    // ensuring no pending vkQueuePresentKHR is still waiting on renderFinished semaphores
    // before we destroy them. Per-fence waits alone are insufficient — they only cover
    // GPU command completion, not the presentation engine's semaphore usage.
    vkDeviceWaitIdle(m_device);

    destroyFramebuffersAndViews();

    // Keep old handle alive so createSwapchain can pass it as ci.oldSwapchain.
    VkSwapchainKHR old = m_swapchain;
    if (!createSwapchain(w, h)) {
        m_swapchain = old;
        return false;
    }
    if (old != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(m_device, old, nullptr);

    // Recreate renderFinished semaphores when image count changes (e.g. MAILBOX→FIFO
    // or surface capability change). vkDeviceWaitIdle above guarantees they are idle.
    if (m_renderFinished.size() != m_swapchainImages.size()) {
        for (auto sem : m_renderFinished)
            if (sem != VK_NULL_HANDLE)
                vkDestroySemaphore(m_device, sem, nullptr);
        m_renderFinished.clear();
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        m_renderFinished.resize(m_swapchainImages.size());
        for (auto& sem : m_renderFinished)
            if (vkCreateSemaphore(m_device, &sci, nullptr, &sem) != VK_SUCCESS)
                return false;
    }

    if (!createImageViews() || !createFramebuffers())
        return false;
    m_imagesInFlight.assign(m_swapchainImages.size(), VK_NULL_HANDLE);
    return true;
}
