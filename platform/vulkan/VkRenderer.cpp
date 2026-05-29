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

static std::vector<uint32_t> loadSpirv(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return {};
    const auto size = static_cast<std::streamsize>(file.tellg());
    file.seekg(0);
    std::vector<uint32_t> buf(static_cast<std::size_t>(size) / sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

static VkShaderModule createShaderModule(VkDevice device, const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size() * sizeof(uint32_t);
    ci.pCode = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &mod);
    return mod;
}

// Insert a simple image layout transition barrier.
static void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                         VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                         VkPipelineStageFlags dstStage, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
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
// Shader / resource path discovery
//
// Priority:
//   1. SDL_GetBasePath() + "shaders/"         (exe-relative, installed layout)
//   2. SDL_GetBasePath() + "../Resources/shaders/"  (macOS .app bundle)
//   3. SDL_GetBasePath() + "../share/fighters-legacy/shaders/"  (Linux system)
//   4. FL_SHADER_DIR + "/"                    (build-tree fallback)
// ---------------------------------------------------------------------------
std::string VkRenderer::resolveShaderDir() {
    // SDL3: SDL_GetBasePath() returns a const char* owned by SDL (no free needed).
    const char* sdlBase = SDL_GetBasePath();
    if (sdlBase) {
        std::string base(sdlBase);
        const char* candidates[] = {
            "shaders/",
            "../Resources/shaders/",
            "../share/fighters-legacy/shaders/",
        };
        for (const char* rel : candidates) {
            std::string dir = base + rel;
            if (!loadSpirv(dir + "tonemap.vert.spv").empty())
                return dir;
        }
    }
    return std::string(FL_SHADER_DIR) + "/";
}

// ---------------------------------------------------------------------------
// IRenderer interface
// ---------------------------------------------------------------------------

bool VkRenderer::init(IWindow* window) {
    m_sdlWindow = static_cast<SDL_Window*>(window->nativeHandle());
    m_shaderDir = resolveShaderDir();

    if (!createInstance())
        return false;
    if (!setupDebugMessenger())
        return false;
    if (!createSurface())
        return false;
    if (!pickPhysicalDevice())
        return false;

    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        uint32_t drv = props.driverVersion;
        m_gpuInfo = std::string(props.deviceName) + " (Vulkan driver " + std::to_string(VK_VERSION_MAJOR(drv)) + "." +
                    std::to_string(VK_VERSION_MINOR(drv)) + "." + std::to_string(VK_VERSION_PATCH(drv)) + ")";
    }

    if (!createLogicalDevice())
        return false;
    if (!createPipelineCache())
        return false;

    if (!createSwapchain(window->width(), window->height()))
        return false;
    if (!createImageViews())
        return false;
    if (!createDepthImage())
        return false;
    if (!createHdrImage())
        return false;
    if (!createHdrSampler())
        return false;
    if (!createTonemapDescriptors())
        return false;
    if (!createForwardPipeline())
        return false;
    if (!createTonemapPipeline())
        return false;
    if (!createCommandPool())
        return false;
    if (!allocateCommandBuffers())
        return false;
    if (!createSyncObjects())
        return false;
    return true;
}

void VkRenderer::onResize(int /*width*/, int /*height*/) {
    m_framebufferResized = true;
}

void VkRenderer::beginFrame() {
    m_frameAcquired = false;

    // Poll window size every frame. On some Wayland + SDL3 configurations,
    // SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED is not fired during a live drag.
    if (m_swapchain != VK_NULL_HANDLE) {
        int w = 0, h = 0;
        if (SDL_GetWindowSizeInPixels(m_sdlWindow, &w, &h) && w > 0 && h > 0 &&
            (static_cast<uint32_t>(w) != m_swapchainExtent.width ||
             static_cast<uint32_t>(h) != m_swapchainExtent.height)) {
            m_framebufferResized = true;
        }
    }

    if (m_framebufferResized) {
        if (!recreateSwapchain()) {
            m_framebufferResized = true;
            return;
        }
        m_framebufferResized = false;
    }

    // 100 ms timeout keeps the event loop responsive.
    if (vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, 100'000'000ULL) == VK_TIMEOUT)
        return;

    VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, std::numeric_limits<uint64_t>::max(),
                                            m_imageAvailable[m_currentFrame], VK_NULL_HANDLE, &m_currentImageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        m_framebufferResized = true;
        return;
    }
    if (result == VK_SUBOPTIMAL_KHR)
        m_framebufferResized = true;

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

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        m_framebufferResized = true;

    m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VkRenderer::shutdown() {
    if (m_device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(m_device);

    destroyAttachments();

    if (m_hdrSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_hdrSampler, nullptr);
        m_hdrSampler = VK_NULL_HANDLE;
    }
    if (m_tonemapPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_tonemapPool, nullptr);
        m_tonemapPool = VK_NULL_HANDLE;
    }
    if (m_tonemapSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_tonemapSetLayout, nullptr);
        m_tonemapSetLayout = VK_NULL_HANDLE;
    }

    cleanupSwapchain();

    if (m_forwardPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_forwardPipeline, nullptr);
        m_forwardPipeline = VK_NULL_HANDLE;
    }
    if (m_forwardLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_forwardLayout, nullptr);
        m_forwardLayout = VK_NULL_HANDLE;
    }
    if (m_tonemapPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_tonemapPipeline, nullptr);
        m_tonemapPipeline = VK_NULL_HANDLE;
    }
    if (m_tonemapLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_tonemapLayout, nullptr);
        m_tonemapLayout = VK_NULL_HANDLE;
    }
    if (m_pipelineCache != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
        m_pipelineCache = VK_NULL_HANDLE;
    }

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

    if (m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
    if (m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
#if defined(FL_VK_VALIDATION)
    if (m_debugMessenger != VK_NULL_HANDLE) {
        auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (fn)
            fn(m_instance, m_debugMessenger, nullptr);
        m_debugMessenger = VK_NULL_HANDLE;
    }
#endif
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}

const char* VkRenderer::getLastError() const {
    return m_lastError.empty() ? nullptr : m_lastError.c_str();
}

const char* VkRenderer::gpuInfo() const {
    return m_gpuInfo.c_str();
}

// ---------------------------------------------------------------------------
// createInstance
// ---------------------------------------------------------------------------

bool VkRenderer::createInstance() {
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
    if (!fn || fn(m_instance, &ci, nullptr, &m_debugMessenger) != VK_SUCCESS)
        std::fprintf(stderr, "[VK WARN] Debug messenger creation failed\n");
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

    auto isSuitable = [&](VkPhysicalDevice dev, uint32_t& gf, uint32_t& pf) -> bool {
        if (!checkDeviceExtension(dev, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            return false;
#if defined(__APPLE__)
        if (!checkDeviceExtension(dev, "VK_KHR_portability_subset"))
            return false;
#endif
        // Require Vulkan 1.3 dynamic rendering support.
        VkPhysicalDeviceVulkan13Features vk13{};
        vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceFeatures2 feats{};
        feats.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        feats.pNext = &vk13;
        vkGetPhysicalDeviceFeatures2(dev, &feats);
        if (!vk13.dynamicRendering)
            return false;

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

    VkPhysicalDevice fallback = VK_NULL_HANDLE;
    uint32_t fbGf = 0, fbPf = 0;

    for (auto dev : devices) {
        uint32_t gf = 0, pf = 0;
        if (!isSuitable(dev, gf, pf))
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

    m_lastError = "no suitable Vulkan 1.3 GPU with dynamic rendering found";
    return false;
}

// ---------------------------------------------------------------------------
// createLogicalDevice — enables dynamic rendering (Vulkan 1.3 core)
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

    // Enable Vulkan 1.3 features required by this renderer.
    VkPhysicalDeviceVulkan13Features vk13{};
    vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vk13.dynamicRendering = VK_TRUE;
    vk13.synchronization2 = VK_TRUE;

    // Chain through VkPhysicalDeviceFeatures2; pEnabledFeatures must be null.
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &vk13;

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext = &features2;
    ci.queueCreateInfoCount = static_cast<uint32_t>(queueCIs.size());
    ci.pQueueCreateInfos = queueCIs.data();
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    // pEnabledFeatures must be null when using VkPhysicalDeviceFeatures2 in pNext.

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

    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        m_swapchainExtent = caps.currentExtent;
    } else {
        m_swapchainExtent.width =
            std::clamp(static_cast<uint32_t>(width), caps.minImageExtent.width, caps.maxImageExtent.width);
        m_swapchainExtent.height =
            std::clamp(static_cast<uint32_t>(height), caps.minImageExtent.height, caps.maxImageExtent.height);
    }

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
    ci.oldSwapchain = m_swapchain;

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
// Attachments (depth + HDR)
// ---------------------------------------------------------------------------

uint32_t VkRenderer::findMemoryType(VkPhysicalDevice physDevice, uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if ((filter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return 0; // unreachable on any compliant GPU
}

bool VkRenderer::createAttachmentImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                                       VkImageAspectFlags aspect, VkImage& image, VkDeviceMemory& memory,
                                       VkImageView& view) {
    VkImageCreateInfo imageCI{};
    imageCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCI.imageType = VK_IMAGE_TYPE_2D;
    imageCI.extent = {width, height, 1};
    imageCI.mipLevels = 1;
    imageCI.arrayLayers = 1;
    imageCI.format = format;
    imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCI.usage = usage;
    imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(m_device, &imageCI, nullptr, &image) != VK_SUCCESS) {
        m_lastError = "vkCreateImage failed";
        return false;
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(m_device, image, &memReq);

    VkMemoryAllocateInfo allocCI{};
    allocCI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocCI.allocationSize = memReq.size;
    allocCI.memoryTypeIndex =
        findMemoryType(m_physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &allocCI, nullptr, &memory) != VK_SUCCESS) {
        m_lastError = "vkAllocateMemory failed";
        return false;
    }
    vkBindImageMemory(m_device, image, memory, 0);

    VkImageViewCreateInfo viewCI{};
    viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image = image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format = format;
    viewCI.subresourceRange.aspectMask = aspect;
    viewCI.subresourceRange.levelCount = 1;
    viewCI.subresourceRange.layerCount = 1;
    if (vkCreateImageView(m_device, &viewCI, nullptr, &view) != VK_SUCCESS) {
        m_lastError = "vkCreateImageView (attachment) failed";
        return false;
    }
    return true;
}

bool VkRenderer::createDepthImage() {
    return createAttachmentImage(m_swapchainExtent.width, m_swapchainExtent.height, kDepthFormat,
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, m_depthImage,
                                 m_depthMemory, m_depthView);
}

bool VkRenderer::createHdrImage() {
    return createAttachmentImage(m_swapchainExtent.width, m_swapchainExtent.height, kHdrFormat,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT, m_hdrImage, m_hdrMemory, m_hdrView);
}

bool VkRenderer::createHdrSampler() {
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = VK_FILTER_LINEAR;
    ci.minFilter = VK_FILTER_LINEAR;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(m_device, &ci, nullptr, &m_hdrSampler) != VK_SUCCESS) {
        m_lastError = "vkCreateSampler (hdr) failed";
        return false;
    }
    return true;
}

void VkRenderer::destroyAttachments() {
    if (m_depthView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_depthView, nullptr);
        m_depthView = VK_NULL_HANDLE;
    }
    if (m_depthImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_depthImage, nullptr);
        m_depthImage = VK_NULL_HANDLE;
    }
    if (m_depthMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_depthMemory, nullptr);
        m_depthMemory = VK_NULL_HANDLE;
    }
    if (m_hdrView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_hdrView, nullptr);
        m_hdrView = VK_NULL_HANDLE;
    }
    if (m_hdrImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_hdrImage, nullptr);
        m_hdrImage = VK_NULL_HANDLE;
    }
    if (m_hdrMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_hdrMemory, nullptr);
        m_hdrMemory = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Tonemap descriptor
// ---------------------------------------------------------------------------

bool VkRenderer::createTonemapDescriptors() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 1;
    layoutCI.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(m_device, &layoutCI, nullptr, &m_tonemapSetLayout) != VK_SUCCESS) {
        m_lastError = "vkCreateDescriptorSetLayout (tonemap) failed";
        return false;
    }

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(m_device, &poolCI, nullptr, &m_tonemapPool) != VK_SUCCESS) {
        m_lastError = "vkCreateDescriptorPool (tonemap) failed";
        return false;
    }

    VkDescriptorSetAllocateInfo allocCI{};
    allocCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocCI.descriptorPool = m_tonemapPool;
    allocCI.descriptorSetCount = 1;
    allocCI.pSetLayouts = &m_tonemapSetLayout;
    if (vkAllocateDescriptorSets(m_device, &allocCI, &m_tonemapSet) != VK_SUCCESS) {
        m_lastError = "vkAllocateDescriptorSets (tonemap) failed";
        return false;
    }

    updateHdrDescriptor();
    return true;
}

void VkRenderer::updateHdrDescriptor() {
    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = m_hdrSampler;
    imageInfo.imageView = m_hdrView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_tonemapSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
}

// ---------------------------------------------------------------------------
// Pipeline cache
// ---------------------------------------------------------------------------

bool VkRenderer::createPipelineCache() {
    VkPipelineCacheCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (vkCreatePipelineCache(m_device, &ci, nullptr, &m_pipelineCache) != VK_SUCCESS) {
        m_lastError = "vkCreatePipelineCache failed";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// createForwardPipeline — renders geometry into the HDR target
// ---------------------------------------------------------------------------

bool VkRenderer::createForwardPipeline() {
    auto vertCode = loadSpirv(m_shaderDir + "mesh.vert.spv");
    auto fragCode = loadSpirv(m_shaderDir + "mesh.frag.spv");
    if (vertCode.empty() || fragCode.empty()) {
        m_lastError = "failed to load mesh shader SPIR-V from: " + m_shaderDir;
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

    // Depth state: reverse-Z (GREATER compare); test disabled for the placeholder
    // triangle since it has no depth output. Enabled in PR 2 with real geometry.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER; // reverse-Z

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
    if (vkCreatePipelineLayout(m_device, &layoutCI, nullptr, &m_forwardLayout) != VK_SUCCESS) {
        m_lastError = "vkCreatePipelineLayout (forward) failed";
        vkDestroyShaderModule(m_device, vertMod, nullptr);
        vkDestroyShaderModule(m_device, fragMod, nullptr);
        return false;
    }

    // Dynamic rendering: declare attachment formats instead of a VkRenderPass.
    VkFormat hdrFmt = kHdrFormat;
    VkPipelineRenderingCreateInfo renderingCI{};
    renderingCI.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingCI.colorAttachmentCount = 1;
    renderingCI.pColorAttachmentFormats = &hdrFmt;
    renderingCI.depthAttachmentFormat = kDepthFormat;

    VkGraphicsPipelineCreateInfo pipelineCI{};
    pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCI.pNext = &renderingCI;
    pipelineCI.stageCount = 2;
    pipelineCI.pStages = stages.data();
    pipelineCI.pVertexInputState = &vertexInput;
    pipelineCI.pInputAssemblyState = &inputAssembly;
    pipelineCI.pViewportState = &viewportState;
    pipelineCI.pRasterizationState = &rasterizer;
    pipelineCI.pMultisampleState = &multisampling;
    pipelineCI.pDepthStencilState = &depthStencil;
    pipelineCI.pColorBlendState = &colorBlend;
    pipelineCI.pDynamicState = &dynamicState;
    pipelineCI.layout = m_forwardLayout;
    pipelineCI.renderPass = VK_NULL_HANDLE; // dynamic rendering

    const VkResult result =
        vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineCI, nullptr, &m_forwardPipeline);
    vkDestroyShaderModule(m_device, vertMod, nullptr);
    vkDestroyShaderModule(m_device, fragMod, nullptr);

    if (result != VK_SUCCESS) {
        m_lastError = "vkCreateGraphicsPipelines (forward) failed";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// createTonemapPipeline — fullscreen HDR→LDR conversion into swapchain
// ---------------------------------------------------------------------------

bool VkRenderer::createTonemapPipeline() {
    auto vertCode = loadSpirv(m_shaderDir + "tonemap.vert.spv");
    auto fragCode = loadSpirv(m_shaderDir + "tonemap.frag.spv");
    if (vertCode.empty() || fragCode.empty()) {
        m_lastError = "failed to load tonemap shader SPIR-V from: " + m_shaderDir;
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
    rasterizer.cullMode = VK_CULL_MODE_NONE; // fullscreen triangle, no back-face cull
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo noDepth{};
    noDepth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &colorBlendAttachment;

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount = 1;
    layoutCI.pSetLayouts = &m_tonemapSetLayout;
    if (vkCreatePipelineLayout(m_device, &layoutCI, nullptr, &m_tonemapLayout) != VK_SUCCESS) {
        m_lastError = "vkCreatePipelineLayout (tonemap) failed";
        vkDestroyShaderModule(m_device, vertMod, nullptr);
        vkDestroyShaderModule(m_device, fragMod, nullptr);
        return false;
    }

    VkPipelineRenderingCreateInfo renderingCI{};
    renderingCI.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingCI.colorAttachmentCount = 1;
    renderingCI.pColorAttachmentFormats = &m_swapchainFormat;

    VkGraphicsPipelineCreateInfo pipelineCI{};
    pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCI.pNext = &renderingCI;
    pipelineCI.stageCount = 2;
    pipelineCI.pStages = stages.data();
    pipelineCI.pVertexInputState = &vertexInput;
    pipelineCI.pInputAssemblyState = &inputAssembly;
    pipelineCI.pViewportState = &viewportState;
    pipelineCI.pRasterizationState = &rasterizer;
    pipelineCI.pMultisampleState = &multisampling;
    pipelineCI.pDepthStencilState = &noDepth;
    pipelineCI.pColorBlendState = &colorBlend;
    pipelineCI.pDynamicState = &dynamicState;
    pipelineCI.layout = m_tonemapLayout;
    pipelineCI.renderPass = VK_NULL_HANDLE;

    const VkResult result =
        vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineCI, nullptr, &m_tonemapPipeline);
    vkDestroyShaderModule(m_device, vertMod, nullptr);
    vkDestroyShaderModule(m_device, fragMod, nullptr);

    if (result != VK_SUCCESS) {
        m_lastError = "vkCreateGraphicsPipelines (tonemap) failed";
        return false;
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
//
// Passes:
//   1. Forward  — placeholder triangle into HDR RGBA16F + D32 depth
//   2. Tonemap  — fullscreen HDR→LDR into swapchain image
// ---------------------------------------------------------------------------

void VkRenderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex) {
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    // ── Transition HDR image: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL ──────────
    imageBarrier(cmd, m_hdrImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    // ── Transition depth image: UNDEFINED → DEPTH_STENCIL_ATTACHMENT_OPTIMAL ─
    imageBarrier(cmd, m_depthImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT);

    // ── Forward pass ─────────────────────────────────────────────────────────
    {
        VkRenderingAttachmentInfo colorAtt{};
        colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAtt.imageView = m_hdrView;
        colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.clearValue.color = {{0.05f, 0.10f, 0.18f, 1.0f}}; // deep blue horizon

        VkRenderingAttachmentInfo depthAtt{};
        depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAtt.imageView = m_depthView;
        depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAtt.clearValue.depthStencil = {0.0f, 0}; // reverse-Z: far plane = 0

        VkRenderingInfo renderInfo{};
        renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderInfo.renderArea = {{0, 0}, m_swapchainExtent};
        renderInfo.layerCount = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments = &colorAtt;
        renderInfo.pDepthAttachment = &depthAtt;

        vkCmdBeginRendering(cmd, &renderInfo);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_forwardPipeline);

        VkViewport viewport{
            0.0f, 0.0f, static_cast<float>(m_swapchainExtent.width), static_cast<float>(m_swapchainExtent.height),
            0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, m_swapchainExtent};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdDraw(cmd, 3, 1, 0, 0); // placeholder triangle; replaced in PR 2

        vkCmdEndRendering(cmd);
    }

    // ── Transition HDR: COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL ─
    imageBarrier(cmd, m_hdrImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // ── Transition swapchain image: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL ────
    imageBarrier(cmd, m_swapchainImages[imageIndex], VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    // ── Tonemap pass: HDR sampler → swapchain ────────────────────────────────
    {
        VkRenderingAttachmentInfo colorAtt{};
        colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAtt.imageView = m_swapchainImageViews[imageIndex];
        colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo renderInfo{};
        renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderInfo.renderArea = {{0, 0}, m_swapchainExtent};
        renderInfo.layerCount = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments = &colorAtt;

        vkCmdBeginRendering(cmd, &renderInfo);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapLayout, 0, 1, &m_tonemapSet, 0, nullptr);

        VkViewport viewport{
            0.0f, 0.0f, static_cast<float>(m_swapchainExtent.width), static_cast<float>(m_swapchainExtent.height),
            0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, m_swapchainExtent};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdDraw(cmd, 3, 1, 0, 0); // fullscreen triangle

        vkCmdEndRendering(cmd);
    }

    // ── Transition swapchain: COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR ────
    imageBarrier(cmd, m_swapchainImages[imageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    vkEndCommandBuffer(cmd);
}

// ---------------------------------------------------------------------------
// recreateSwapchain / cleanupSwapchain
// ---------------------------------------------------------------------------

void VkRenderer::destroyImageViews() {
    for (auto iv : m_swapchainImageViews)
        if (iv != VK_NULL_HANDLE)
            vkDestroyImageView(m_device, iv, nullptr);
    m_swapchainImageViews.clear();
}

void VkRenderer::cleanupSwapchain() {
    destroyImageViews();
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

bool VkRenderer::recreateSwapchain() {
    // Returns false when the window is zero-sized (minimised or a Wayland
    // configure(0,0) event). Caller re-sets m_framebufferResized and returns
    // to the main loop so pollEvents() can drain pending events before the
    // next attempt — avoiding the compositor deadlock.
    int w = 0, h = 0;
    if (!SDL_GetWindowSizeInPixels(m_sdlWindow, &w, &h))
        SDL_GetWindowSize(m_sdlWindow, &w, &h);
    if (w == 0 || h == 0)
        return false;

    // vkDeviceWaitIdle drains both GPU queues and the presentation engine,
    // ensuring no pending present is still waiting on renderFinished semaphores.
    vkDeviceWaitIdle(m_device);

    destroyImageViews();
    destroyAttachments();

    VkSwapchainKHR old = m_swapchain;
    if (!createSwapchain(w, h)) {
        m_swapchain = old;
        return false;
    }
    if (old != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(m_device, old, nullptr);

    // Recreate renderFinished semaphores when image count changes.
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

    if (!createImageViews())
        return false;
    if (!createDepthImage())
        return false;
    if (!createHdrImage())
        return false;
    updateHdrDescriptor(); // point descriptor to the new HDR image view

    m_imagesInFlight.assign(m_swapchainImages.size(), VK_NULL_HANDLE);
    return true;
}
