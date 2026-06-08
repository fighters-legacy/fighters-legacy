// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include "VkRenderer.h"
#include "IWindow.h"
#include "UnifontBitmap.h"
#include "Utf8Decode.h"
#include "VkWindow.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <glm/ext/matrix_clip_space.hpp> // glm::orthoZO
#include <glm/gtc/matrix_transform.hpp>  // glm::lookAt, glm::inverse
#include <limits>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
#include "VkRendererFactory.h"
std::unique_ptr<IRenderer> createVulkanRenderer() {
    return std::make_unique<VkRenderer>();
}

// ---------------------------------------------------------------------------
// Static helpers
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

static void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                         VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                         VkPipelineStageFlags dstStage, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                         uint32_t layerCount = 1) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, layerCount};
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
// Host-visible buffer helper (for small per-frame UBOs)
// ---------------------------------------------------------------------------
static bool createHostBuffer(VkDevice device, VkPhysicalDevice physDevice, VkDeviceSize size, VkBufferUsageFlags usage,
                             VkBuffer& buf, VkDeviceMemory& mem, void*& mapped) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bci, nullptr, &buf) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buf, &req);

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);

    const VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t memTypeIdx = 0;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & flags) == flags) {
            memTypeIdx = i;
            break;
        }
    }

    VkMemoryAllocateInfo allocCI{};
    allocCI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocCI.allocationSize = req.size;
    allocCI.memoryTypeIndex = memTypeIdx;
    if (vkAllocateMemory(device, &allocCI, nullptr, &mem) != VK_SUCCESS)
        return false;

    vkBindBufferMemory(device, buf, mem, 0);
    vkMapMemory(device, mem, 0, size, 0, &mapped);
    return true;
}

// ---------------------------------------------------------------------------
// Shadow resources — 2D array depth image for kNumCascades CSM cascades
// ---------------------------------------------------------------------------
bool VkRenderer::createShadowResources() {
    VkImageCreateInfo imageCI{};
    imageCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCI.imageType = VK_IMAGE_TYPE_2D;
    imageCI.extent = {kShadowRes, kShadowRes, 1};
    imageCI.mipLevels = 1;
    imageCI.arrayLayers = kNumCascades;
    imageCI.format = kDepthFormat;
    imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCI.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(m_device, &imageCI, nullptr, &m_shadowImage) != VK_SUCCESS) {
        m_lastError = "vkCreateImage (shadow) failed";
        return false;
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(m_device, m_shadowImage, &memReq);
    VkMemoryAllocateInfo allocCI{};
    allocCI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocCI.allocationSize = memReq.size;
    allocCI.memoryTypeIndex =
        findMemoryType(m_physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &allocCI, nullptr, &m_shadowMemory) != VK_SUCCESS) {
        m_lastError = "vkAllocateMemory (shadow) failed";
        return false;
    }
    vkBindImageMemory(m_device, m_shadowImage, m_shadowMemory, 0);

    // Array view for sampling in the forward/fragment pass.
    VkImageViewCreateInfo arrayViewCI{};
    arrayViewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    arrayViewCI.image = m_shadowImage;
    arrayViewCI.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    arrayViewCI.format = kDepthFormat;
    arrayViewCI.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kNumCascades};
    if (vkCreateImageView(m_device, &arrayViewCI, nullptr, &m_shadowArrayView) != VK_SUCCESS) {
        m_lastError = "vkCreateImageView (shadow array) failed";
        return false;
    }

    // Per-cascade layer views for rendering.
    for (uint32_t i = 0; i < kNumCascades; ++i) {
        VkImageViewCreateInfo layerCI{};
        layerCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        layerCI.image = m_shadowImage;
        layerCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
        layerCI.format = kDepthFormat;
        layerCI.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1};
        if (vkCreateImageView(m_device, &layerCI, nullptr, &m_shadowLayerViews[i]) != VK_SUCCESS) {
            m_lastError = "vkCreateImageView (shadow layer) failed";
            return false;
        }
    }

    // PCF comparison sampler: border=white (depth=1 → not in shadow outside map).
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL; // lit when receiver ≤ shadow depth
    if (vkCreateSampler(m_device, &sci, nullptr, &m_shadowSampler) != VK_SUCCESS) {
        m_lastError = "vkCreateSampler (shadow) failed";
        return false;
    }
    return true;
}

void VkRenderer::destroyShadowResources() {
    if (m_shadowSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_shadowSampler, nullptr);
        m_shadowSampler = VK_NULL_HANDLE;
    }
    for (auto& v : m_shadowLayerViews) {
        if (v != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, v, nullptr);
            v = VK_NULL_HANDLE;
        }
    }
    if (m_shadowArrayView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_shadowArrayView, nullptr);
        m_shadowArrayView = VK_NULL_HANDLE;
    }
    if (m_shadowImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_shadowImage, nullptr);
        m_shadowImage = VK_NULL_HANDLE;
    }
    if (m_shadowMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_shadowMemory, nullptr);
        m_shadowMemory = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// applySettings — store settings; vsync takes effect on next swapchain recreate
// ---------------------------------------------------------------------------
void VkRenderer::applySettings(const RendererSettings& settings) {
    m_settings = settings;
    m_framebufferResized = true; // trigger swapchain recreate on next beginFrame
}

// ---------------------------------------------------------------------------
// IRenderer — init
// ---------------------------------------------------------------------------
bool VkRenderer::init(IWindow* window) {
    m_sdlWindow = static_cast<SDL_Window*>(window->nativeHandle());
    m_iWindow = window;
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
        m_timestampPeriod = props.limits.timestampPeriod;

        // Check timestamp support on the graphics queue family.
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &qcount, qprops.data());
        if (m_graphicsFamily < qcount && qprops[m_graphicsFamily].timestampValidBits > 0)
            m_timestampSupported = true;
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
    if (!createBloomImages())
        return false;
    if (!createHdrSampler())
        return false;
    if (!createShadowResources())
        return false;
    if (!createPerFrameDescriptorLayout())
        return false;
    if (!createShadowDescriptorLayout())
        return false;
    if (!createMaterialDescriptorLayout())
        return false;
    if (!createCommandPool())
        return false;
    if (!m_resources.init(m_device, m_physicalDevice, m_instance, m_commandPool, m_graphicsQueue, m_matSetLayout))
        return false;
    if (!createTonemapDescriptors())
        return false;
    if (!createBloomDescriptors())
        return false;
    if (!createForwardPipeline())
        return false;
    if (!createForwardAlphaPipeline())
        return false;
    if (!createTonemapPipeline())
        return false;
    if (!createShadowPipeline())
        return false;
    if (!createSkyPipeline())
        return false;
    if (!createBloomPipelines())
        return false;
    if (!allocateCommandBuffers())
        return false;
    if (!createPerFrameDescriptors())
        return false;
    // createParticleResources writes the camera UBO handle into the particle render
    // descriptor sets — must come after createPerFrameDescriptors allocates those buffers.
    if (!createParticleResources())
        return false;
    if (!createParticleComputePipeline())
        return false;
    if (!createParticleRenderPipelines())
        return false;
    if (!createSyncObjects())
        return false;

    // Create timestamp query pool if supported (2 slots per in-flight frame).
    if (m_timestampSupported) {
        VkQueryPoolCreateInfo qpci{};
        qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = 2 * MAX_FRAMES_IN_FLIGHT;
        if (vkCreateQueryPool(m_device, &qpci, nullptr, &m_timestampPool) != VK_SUCCESS) {
            // Non-fatal: GPU timing will show 0 ms.
            m_timestampPool = VK_NULL_HANDLE;
            m_timestampSupported = false;
        }
    }

    // Lazy overlay pipeline — created on first setOverlayLines() call with non-empty span.
    // No failure here; overlay is cosmetic and non-critical.

    return true;
}

void VkRenderer::onResize(int /*width*/, int /*height*/) {
    m_framebufferResized = true;
}

// ---------------------------------------------------------------------------
// beginFrame — acquire image, reset + begin command buffer recording
// ---------------------------------------------------------------------------
void VkRenderer::beginFrame() {
    m_frameAcquired = false;
    m_pendingScene = {};

    // Track wall-clock frame dt for particle simulation (capped at 50 ms).
    const uint64_t nowNs = SDL_GetTicksNS();
    if (m_lastFrameNs > 0) {
        m_frameDt = std::min(float(nowNs - m_lastFrameNs) * 1e-9f, 0.05f);
        m_frameStats.frameDtMs = m_frameDt * 1000.0f;
    }
    m_lastFrameNs = nowNs;

    // Read back GPU timestamps from the completed frame (currentFrame is the slot
    // that just finished presenting — its fence was signaled before this call).
    if (m_timestampSupported && m_timestampPool != VK_NULL_HANDLE) {
        uint64_t ts[2] = {0, 0};
        if (vkGetQueryPoolResults(m_device, m_timestampPool, m_currentFrame * 2, 2, sizeof(ts), ts, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            if (ts[1] >= ts[0])
                m_frameStats.gpuDtMs = static_cast<float>((ts[1] - ts[0]) * m_timestampPeriod) * 1e-6f;
        }
    }

    // Query device-local heap usage + budget via VK_EXT_memory_budget (pNext chain).
    // heapBudget/heapUsage are zero on devices that don't expose the extension;
    // fall back to reporting heap capacity only in that case.
    {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budgetExt{};
        budgetExt.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        VkPhysicalDeviceMemoryProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        props2.pNext = &budgetExt;
        vkGetPhysicalDeviceMemoryProperties2(m_physicalDevice, &props2);

        auto& heaps = props2.memoryProperties;
        uint64_t usedBytes = 0, budgetBytes = 0;
        for (uint32_t i = 0; i < heaps.memoryHeapCount; ++i) {
            if (heaps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                usedBytes = std::max(usedBytes, budgetExt.heapUsage[i]);
                const uint64_t b = budgetExt.heapBudget[i] > 0 ? budgetExt.heapBudget[i] : heaps.memoryHeaps[i].size;
                budgetBytes = std::max(budgetBytes, b);
            }
        }
        m_frameStats.gpuMemUsedBytes = usedBytes;
        m_frameStats.gpuMemBudgetBytes = budgetBytes;
    }

    m_frameStats.drawCalls = m_drawCallCount;
    m_drawCallCount = 0;

    // Advance resource manager deferred deletion.
    m_resources.tick(m_totalFrames);

    if (m_swapchain != VK_NULL_HANDLE) {
        const int w = m_iWindow->width();
        const int h = m_iWindow->height();
        if (w > 0 && h > 0 &&
            (static_cast<uint32_t>(w) != m_swapchainExtent.width ||
             static_cast<uint32_t>(h) != m_swapchainExtent.height))
            m_framebufferResized = true;
    }
    if (m_framebufferResized) {
        if (!recreateSwapchain()) {
            m_framebufferResized = true;
            return;
        }
        m_framebufferResized = false;
    }

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
    m_frameAcquired = true;
}

// ---------------------------------------------------------------------------
// setScene — store scene and upload per-frame UBO data
// ---------------------------------------------------------------------------
void VkRenderer::setScene(const FrameScene& scene) {
    if (!m_frameAcquired)
        return;
    m_pendingScene = scene;
    writeFrameUBOs(scene);
}

void VkRenderer::writeFrameUBOs(const FrameScene& scene) {
    auto& pf = m_perFrame[m_currentFrame];

    CameraUBO cam{};
    cam.view = scene.camera.view;
    cam.proj = scene.camera.proj;
    cam.worldOrigin = glm::vec4(glm::vec3(scene.camera.worldOrigin), 0.0f);
    std::memcpy(pf.cameraMapped, &cam, sizeof(cam));

    LightUBO light{};
    light.sunDirection = glm::vec4(scene.environment.sunDirection, 0.0f);
    light.sunColor = glm::vec4(scene.environment.sunColor, 1.0f);
    light.ambientColor = glm::vec4(scene.environment.ambientColor, 0.0f);
    light.fogParams =
        glm::vec4(scene.environment.fogDensity, scene.environment.fogStartDist, scene.environment.timeOfDay, 0.0f);
    std::memcpy(pf.lightMapped, &light, sizeof(light));

    ShadowUBO shadowUBO{};
    computeCascades(scene, shadowUBO);
    std::memcpy(pf.shadowMapped, &shadowUBO, sizeof(shadowUBO));
}

// ---------------------------------------------------------------------------
// computeCascades — PSSM cascade split + tight bounding-sphere light VP
// ---------------------------------------------------------------------------
void VkRenderer::computeCascades(const FrameScene& scene, ShadowUBO& out) {
    static constexpr float kShadowFar = 20000.0f; // 20 km shadow range
    static constexpr float kShadowNear = 0.1f;
    static constexpr float kLambda = 0.5f; // blend log vs uniform splits

    const glm::mat4& view = scene.camera.view;
    const glm::mat4& proj = scene.camera.proj;
    const glm::mat4 invVP = glm::inverse(proj * view);

    const glm::vec3 sunDir = glm::normalize(scene.environment.sunDirection);
    const glm::vec3 lightDir = -sunDir; // from sun toward scene
    const glm::vec3 worldUp = (std::abs(lightDir.y) > 0.99f) ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);

    // PSSM split distances (view-space positive distances from camera).
    float splits[kNumCascades + 1];
    splits[0] = kShadowNear;
    splits[kNumCascades] = kShadowFar;
    for (uint32_t i = 1; i < kNumCascades; ++i) {
        const float p = float(i) / float(kNumCascades);
        const float lg = kShadowNear * std::pow(kShadowFar / kShadowNear, p);
        const float uni = kShadowNear + (kShadowFar - kShadowNear) * p;
        splits[i] = kLambda * lg + (1.0f - kLambda) * uni;
    }
    // Store cascade end distances for the fragment shader.
    out.splitDepths = glm::vec4(splits[1], splits[2], splits[3], kShadowFar);

    // proj[3][2] = camera near plane (for infinite reverse-Z perspective).
    const float nearVal = proj[3][2];

    for (uint32_t c = 0; c < kNumCascades; ++c) {
        const float nearDist = splits[c];
        const float farDist = splits[c + 1];

        // NDC Z values for the cascade sub-frustum (reverse-Z: near→1, far→0).
        const float ndcZNear = (nearDist > 0.001f) ? glm::clamp(nearVal / nearDist, 0.0f, 1.0f) : 1.0f;
        const float ndcZFar = (farDist > 0.001f) ? glm::clamp(nearVal / farDist, 0.0f, 1.0f) : 0.0f;

        // Unproject 8 frustum corners → camera-relative world space → absolute.
        glm::vec3 corners[8];
        int idx = 0;
        for (float z : {ndcZNear, ndcZFar}) {
            for (float x : {-1.0f, 1.0f}) {
                for (float y : {-1.0f, 1.0f}) {
                    glm::vec4 ndc(x, y, z, 1.0f);
                    glm::vec4 world = invVP * ndc;
                    corners[idx++] = glm::vec3(world / world.w) + glm::vec3(scene.camera.worldOrigin);
                }
            }
        }

        // Bounding sphere centre + radius.
        glm::vec3 center(0.0f);
        for (const auto& v : corners)
            center += v;
        center /= 8.0f;

        float radius = 0.0f;
        for (const auto& v : corners)
            radius = glm::max(radius, glm::length(v - center));
        // Snap to texel increments for stability (prevents shadow swimming).
        const float texelSize = (2.0f * radius) / float(kShadowRes);
        radius = std::ceil(radius / texelSize) * texelSize;

        const glm::vec3 eye = center - lightDir * (radius + 1.0f);
        const glm::mat4 lightView = glm::lookAt(eye, center, worldUp);

        // Orthographic projection — forward-Z (near=0, far=2*(radius+1)).
        glm::mat4 lightProj = glm::orthoZO(-radius, radius, -radius, radius, 0.0f, 2.0f * (radius + 1.0f));
        lightProj[1][1] *= -1.0f; // Vulkan Y-flip

        out.lightViewProj[c] = lightProj * lightView;
    }
}

// ---------------------------------------------------------------------------
// endFrame — record commands, submit, present
// ---------------------------------------------------------------------------
void VkRenderer::endFrame() {
    if (!m_frameAcquired)
        return;

    recordCommandBuffer(m_commandBuffers[m_currentFrame], m_currentImageIndex);

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
    ++m_totalFrames;
}

// ---------------------------------------------------------------------------
// Resource method delegation
// ---------------------------------------------------------------------------
MeshHandle VkRenderer::createMesh(const MeshUploadDesc& d) {
    return m_resources.createMesh(d);
}
TextureHandle VkRenderer::createTexture(const TextureUploadDesc& d) {
    return m_resources.createTexture(d);
}
MaterialHandle VkRenderer::createMaterial(const MaterialDesc& d) {
    return m_resources.createMaterial(d);
}
void VkRenderer::destroyMesh(MeshHandle h) {
    m_resources.destroyMesh(h);
}
void VkRenderer::destroyTexture(TextureHandle h) {
    m_resources.destroyTexture(h);
}
void VkRenderer::destroyMaterial(MaterialHandle h) {
    m_resources.destroyMaterial(h);
}

// ---------------------------------------------------------------------------
// shutdown
// ---------------------------------------------------------------------------
void VkRenderer::shutdown() {
    if (m_device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(m_device);

    destroyOverlayResources();
    destroyParticleResources();
    m_resources.shutdown();

    if (m_timestampPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(m_device, m_timestampPool, nullptr);
        m_timestampPool = VK_NULL_HANDLE;
    }

    // Per-frame UBO buffers
    for (auto& pf : m_perFrame) {
        if (pf.cameraMapped && pf.cameraMemory != VK_NULL_HANDLE)
            vkUnmapMemory(m_device, pf.cameraMemory);
        if (pf.cameraBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(m_device, pf.cameraBuffer, nullptr);
        if (pf.cameraMemory != VK_NULL_HANDLE)
            vkFreeMemory(m_device, pf.cameraMemory, nullptr);

        if (pf.lightMapped && pf.lightMemory != VK_NULL_HANDLE)
            vkUnmapMemory(m_device, pf.lightMemory);
        if (pf.lightBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(m_device, pf.lightBuffer, nullptr);
        if (pf.lightMemory != VK_NULL_HANDLE)
            vkFreeMemory(m_device, pf.lightMemory, nullptr);

        if (pf.shadowMapped && pf.shadowMemory != VK_NULL_HANDLE)
            vkUnmapMemory(m_device, pf.shadowMemory);
        if (pf.shadowBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(m_device, pf.shadowBuffer, nullptr);
        if (pf.shadowMemory != VK_NULL_HANDLE)
            vkFreeMemory(m_device, pf.shadowMemory, nullptr);
    }
    if (m_perFramePool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_perFramePool, nullptr);
        m_perFramePool = VK_NULL_HANDLE;
    }
    if (m_shadowPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_shadowPool, nullptr);
        m_shadowPool = VK_NULL_HANDLE;
    }
    if (m_perFrameSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_perFrameSetLayout, nullptr);
        m_perFrameSetLayout = VK_NULL_HANDLE;
    }
    if (m_shadowSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_shadowSetLayout, nullptr);
        m_shadowSetLayout = VK_NULL_HANDLE;
    }
    if (m_matSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_matSetLayout, nullptr);
        m_matSetLayout = VK_NULL_HANDLE;
    }

    destroyAttachments();
    destroyShadowResources();

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

    destroyBloomResources();

    if (m_forwardPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_forwardPipeline, nullptr);
        m_forwardPipeline = VK_NULL_HANDLE;
    }
    if (m_forwardAlphaPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_forwardAlphaPipeline, nullptr);
        m_forwardAlphaPipeline = VK_NULL_HANDLE;
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
    if (m_shadowPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_shadowPipeline, nullptr);
        m_shadowPipeline = VK_NULL_HANDLE;
    }
    if (m_shadowLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_shadowLayout, nullptr);
        m_shadowLayout = VK_NULL_HANDLE;
    }
    if (m_skyPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_skyPipeline, nullptr);
        m_skyPipeline = VK_NULL_HANDLE;
    }
    if (m_skyLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_skyLayout, nullptr);
        m_skyLayout = VK_NULL_HANDLE;
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

    VkPhysicalDeviceVulkan13Features vk13{};
    vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vk13.dynamicRendering = VK_TRUE;
    vk13.synchronization2 = VK_TRUE;

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
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    if (chosen.format == formats[0].format)
        for (const auto& f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = f;
                break;
            }
    m_swapchainFormat = chosen.format;

    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &modeCount, modes.data());

    // Select present mode based on vsync setting.
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR; // always available
    auto hasMode = [&](VkPresentModeKHR want) {
        for (auto m : modes)
            if (m == want)
                return true;
        return false;
    };
    if (m_settings.vsync == RendererVsyncMode::Off) {
        if (hasMode(VK_PRESENT_MODE_IMMEDIATE_KHR))
            presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (hasMode(VK_PRESENT_MODE_MAILBOX_KHR))
            presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (m_settings.vsync == RendererVsyncMode::Adaptive) {
        if (hasMode(VK_PRESENT_MODE_FIFO_RELAXED_KHR))
            presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }
    // RendererVsyncMode::On stays with FIFO_KHR (guaranteed vsync)

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
// Attachments
// ---------------------------------------------------------------------------
uint32_t VkRenderer::findMemoryType(VkPhysicalDevice physDevice, uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if ((filter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return 0;
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
    auto destroy = [this](VkImageView& v, VkImage& i, VkDeviceMemory& m) {
        if (v != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, v, nullptr);
            v = VK_NULL_HANDLE;
        }
        if (i != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, i, nullptr);
            i = VK_NULL_HANDLE;
        }
        if (m != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m, nullptr);
            m = VK_NULL_HANDLE;
        }
    };
    destroy(m_depthView, m_depthImage, m_depthMemory);
    destroy(m_hdrView, m_hdrImage, m_hdrMemory);
    destroy(m_bloomView, m_bloomImage, m_bloomMemory);
    destroy(m_bloomAuxView, m_bloomAuxImage, m_bloomAuxMemory);
}

// ---------------------------------------------------------------------------
// Tonemap descriptor
// ---------------------------------------------------------------------------
bool VkRenderer::createTonemapDescriptors() {
    // Binding 0: HDR buffer; binding 1: bloom buffer (for composite in tonemap shader).
    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    }};

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutCI.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &layoutCI, nullptr, &m_tonemapSetLayout) != VK_SUCCESS) {
        m_lastError = "vkCreateDescriptorSetLayout (tonemap) failed";
        return false;
    }

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
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
    auto makeInfo = [this](VkImageView view) {
        VkDescriptorImageInfo info{};
        info.sampler = m_hdrSampler;
        info.imageView = view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return info;
    };

    // Binding 0: HDR; binding 1: bloom (points to bloom image when available, else HDR as dummy).
    const VkDescriptorImageInfo hdrInfo = makeInfo(m_hdrView);
    const VkDescriptorImageInfo bloomInfo = makeInfo(m_bloomView != VK_NULL_HANDLE ? m_bloomView : m_hdrView);

    const std::array<VkWriteDescriptorSet, 2> writes{{
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_tonemapSet, 0, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &hdrInfo, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_tonemapSet, 1, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &bloomInfo, nullptr, nullptr},
    }};
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

// ---------------------------------------------------------------------------
// Per-frame descriptor layout (set 0: camera UBO + light UBO + shadow UBO + shadow map)
// ---------------------------------------------------------------------------
bool VkRenderer::createPerFrameDescriptorLayout() {
    const std::array<VkDescriptorSetLayoutBinding, 4> bindings{{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    }};
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = static_cast<uint32_t>(bindings.size());
    ci.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &ci, nullptr, &m_perFrameSetLayout) != VK_SUCCESS) {
        m_lastError = "vkCreateDescriptorSetLayout (per-frame) failed";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Shadow pipeline descriptor layout (set 0: ShadowUBO only, vertex stage)
// ---------------------------------------------------------------------------
bool VkRenderer::createShadowDescriptorLayout() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 1;
    layoutCI.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(m_device, &layoutCI, nullptr, &m_shadowSetLayout) != VK_SUCCESS) {
        m_lastError = "vkCreateDescriptorSetLayout (shadow) failed";
        return false;
    }

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = MAX_FRAMES_IN_FLIGHT;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(m_device, &poolCI, nullptr, &m_shadowPool) != VK_SUCCESS) {
        m_lastError = "vkCreateDescriptorPool (shadow) failed";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Per-material descriptor layout (set 1: base color + normal + ORM samplers)
// ---------------------------------------------------------------------------
bool VkRenderer::createMaterialDescriptorLayout() {
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings{{
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    }};
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = static_cast<uint32_t>(bindings.size());
    ci.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &ci, nullptr, &m_matSetLayout) != VK_SUCCESS) {
        m_lastError = "vkCreateDescriptorSetLayout (material) failed";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Per-frame UBO buffers + descriptor sets
// ---------------------------------------------------------------------------
bool VkRenderer::createPerFrameDescriptors() {
    // Pool: 2 frames × (3 UBOs + 1 shadow-map sampler).
    const std::array<VkDescriptorPoolSize, 2> poolSizes{{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT) * 3},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT)},
    }};
    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = MAX_FRAMES_IN_FLIGHT;
    poolCI.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolCI.pPoolSizes = poolSizes.data();
    if (vkCreateDescriptorPool(m_device, &poolCI, nullptr, &m_perFramePool) != VK_SUCCESS) {
        m_lastError = "vkCreateDescriptorPool (per-frame) failed";
        return false;
    }

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        auto& pf = m_perFrame[i];

        if (!createHostBuffer(m_device, m_physicalDevice, sizeof(CameraUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              pf.cameraBuffer, pf.cameraMemory, pf.cameraMapped) ||
            !createHostBuffer(m_device, m_physicalDevice, sizeof(LightUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              pf.lightBuffer, pf.lightMemory, pf.lightMapped) ||
            !createHostBuffer(m_device, m_physicalDevice, sizeof(ShadowUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              pf.shadowBuffer, pf.shadowMemory, pf.shadowMapped)) {
            m_lastError = "per-frame UBO buffer creation failed";
            return false;
        }

        // ── Forward pass descriptor set (set 0) ───────────────────────────
        VkDescriptorSetAllocateInfo allocCI{};
        allocCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocCI.descriptorPool = m_perFramePool;
        allocCI.descriptorSetCount = 1;
        allocCI.pSetLayouts = &m_perFrameSetLayout;
        if (vkAllocateDescriptorSets(m_device, &allocCI, &pf.descriptorSet) != VK_SUCCESS) {
            m_lastError = "vkAllocateDescriptorSets (per-frame) failed";
            return false;
        }

        VkDescriptorBufferInfo camInfo{pf.cameraBuffer, 0, sizeof(CameraUBO)};
        VkDescriptorBufferInfo lgtInfo{pf.lightBuffer, 0, sizeof(LightUBO)};
        VkDescriptorBufferInfo shadowInfo{pf.shadowBuffer, 0, sizeof(ShadowUBO)};

        VkDescriptorImageInfo shadowImgInfo{};
        shadowImgInfo.sampler = m_shadowSampler;
        shadowImgInfo.imageView = m_shadowArrayView;
        shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        const std::array<VkWriteDescriptorSet, 4> writes{{
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, pf.descriptorSet, 0, 0, 1,
             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &camInfo, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, pf.descriptorSet, 1, 0, 1,
             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &lgtInfo, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, pf.descriptorSet, 2, 0, 1,
             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &shadowInfo, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, pf.descriptorSet, 3, 0, 1,
             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shadowImgInfo, nullptr, nullptr},
        }};
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        // ── Shadow pipeline descriptor set (set 0) ────────────────────────
        VkDescriptorSetAllocateInfo shadowAllocCI{};
        shadowAllocCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        shadowAllocCI.descriptorPool = m_shadowPool;
        shadowAllocCI.descriptorSetCount = 1;
        shadowAllocCI.pSetLayouts = &m_shadowSetLayout;
        if (vkAllocateDescriptorSets(m_device, &shadowAllocCI, &pf.shadowDescriptorSet) != VK_SUCCESS) {
            m_lastError = "vkAllocateDescriptorSets (shadow pipeline) failed";
            return false;
        }

        VkWriteDescriptorSet shadowWrite{};
        shadowWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        shadowWrite.dstSet = pf.shadowDescriptorSet;
        shadowWrite.dstBinding = 0;
        shadowWrite.descriptorCount = 1;
        shadowWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        shadowWrite.pBufferInfo = &shadowInfo;
        vkUpdateDescriptorSets(m_device, 1, &shadowWrite, 0, nullptr);
    }
    return true;
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

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertMod, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragMod, "main",
         nullptr},
    }};

    // Vertex input: single interleaved binding matching struct Vertex (48 bytes).
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    const std::array<VkVertexInputAttributeDescription, 4> attrs{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)},
        {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
    }};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vertexInput.pVertexAttributeDescriptions = attrs.data();

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

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER; // reverse-Z

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &colorBlendAttachment;

    // Push constant range: ForwardPushConstants (96 bytes).
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(ForwardPushConstants);

    // Descriptor set layouts: set 0 = per-frame, set 1 = per-material.
    const std::array<VkDescriptorSetLayout, 2> setLayouts{m_perFrameSetLayout, m_matSetLayout};

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutCI.pSetLayouts = setLayouts.data();
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_device, &layoutCI, nullptr, &m_forwardLayout) != VK_SUCCESS) {
        m_lastError = "vkCreatePipelineLayout (forward) failed";
        vkDestroyShaderModule(m_device, vertMod, nullptr);
        vkDestroyShaderModule(m_device, fragMod, nullptr);
        return false;
    }

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
    pipelineCI.renderPass = VK_NULL_HANDLE;

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
// createForwardAlphaPipeline — same shaders as forward but with alpha blend,
// depth write OFF, no face culling (for canopy glass and similar double-sided
// transparent surfaces).
// ---------------------------------------------------------------------------
bool VkRenderer::createForwardAlphaPipeline() {
    auto vertCode = loadSpirv(m_shaderDir + "mesh.vert.spv");
    auto fragCode = loadSpirv(m_shaderDir + "mesh.frag.spv");
    if (vertCode.empty() || fragCode.empty()) {
        m_lastError = "failed to load mesh shader SPIR-V (alpha pipeline): " + m_shaderDir;
        return false;
    }

    VkShaderModule vertMod = createShaderModule(m_device, vertCode);
    VkShaderModule fragMod = createShaderModule(m_device, fragCode);

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertMod, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragMod, "main",
         nullptr},
    }};

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    const std::array<VkVertexInputAttributeDescription, 4> attrs{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)},
        {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
    }};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vertexInput.pVertexAttributeDescriptions = attrs.data();

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
    rasterizer.cullMode = VK_CULL_MODE_NONE; // transparent surfaces rendered from both sides
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;            // transparent — don't occlude opaques behind
    depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER; // reverse-Z

    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAtt.blendEnable = VK_TRUE;
    blendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAtt.colorBlendOp = VK_BLEND_OP_ADD;
    blendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAtt.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAtt;

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
    pipelineCI.layout = m_forwardLayout; // same layout as opaque forward
    pipelineCI.renderPass = VK_NULL_HANDLE;

    const VkResult result =
        vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineCI, nullptr, &m_forwardAlphaPipeline);
    vkDestroyShaderModule(m_device, vertMod, nullptr);
    vkDestroyShaderModule(m_device, fragMod, nullptr);

    if (result != VK_SUCCESS) {
        m_lastError = "vkCreateGraphicsPipelines (forward alpha) failed";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// createTonemapPipeline — fullscreen HDR→LDR into swapchain
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

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertMod, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragMod, "main",
         nullptr},
    }};

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
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo noDepth{};
    noDepth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &colorBlendAttachment;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(TonemapPush);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount = 1;
    layoutCI.pSetLayouts = &m_tonemapSetLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pushRange;
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
// createShadowPipeline — depth-only pipeline for CSM cascade rendering
// ---------------------------------------------------------------------------
bool VkRenderer::createShadowPipeline() {
    auto vertCode = loadSpirv(m_shaderDir + "shadow.vert.spv");
    if (vertCode.empty()) {
        m_lastError = "failed to load shadow.vert.spv from: " + m_shaderDir;
        return false;
    }
    VkShaderModule vertMod = createShaderModule(m_device, vertCode);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vertMod;
    stage.pName = "main";

    // Vertex input: position only from the interleaved buffer (stride = sizeof(Vertex)).
    VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription posAttr{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions = &posAttr;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    const std::array<VkDynamicState, 2> dynStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
    dynamicState.pDynamicStates = dynStates.data();

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT; // front-face culling reduces peter-panning
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS; // forward-Z shadow maps

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(ShadowPushConstants);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount = 1;
    layoutCI.pSetLayouts = &m_shadowSetLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_device, &layoutCI, nullptr, &m_shadowLayout) != VK_SUCCESS) {
        m_lastError = "vkCreatePipelineLayout (shadow) failed";
        vkDestroyShaderModule(m_device, vertMod, nullptr);
        return false;
    }

    VkPipelineRenderingCreateInfo renderingCI{};
    renderingCI.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingCI.depthAttachmentFormat = kDepthFormat;

    VkGraphicsPipelineCreateInfo pipelineCI{};
    pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCI.pNext = &renderingCI;
    pipelineCI.stageCount = 1;
    pipelineCI.pStages = &stage;
    pipelineCI.pVertexInputState = &vertexInput;
    pipelineCI.pInputAssemblyState = &inputAssembly;
    pipelineCI.pViewportState = &viewportState;
    pipelineCI.pRasterizationState = &rasterizer;
    pipelineCI.pMultisampleState = &multisampling;
    pipelineCI.pDepthStencilState = &depthStencil;
    pipelineCI.pDynamicState = &dynamicState;
    pipelineCI.layout = m_shadowLayout;
    pipelineCI.renderPass = VK_NULL_HANDLE;

    const VkResult result =
        vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineCI, nullptr, &m_shadowPipeline);
    vkDestroyShaderModule(m_device, vertMod, nullptr);

    if (result != VK_SUCCESS) {
        m_lastError = "vkCreateGraphicsPipelines (shadow) failed";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// createSkyPipeline — sky gradient rendered where depth == 0 (reverse-Z far)
// ---------------------------------------------------------------------------
bool VkRenderer::createSkyPipeline() {
    auto vertCode = loadSpirv(m_shaderDir + "tonemap.vert.spv"); // fullscreen triangle
    auto fragCode = loadSpirv(m_shaderDir + "sky.frag.spv");
    if (vertCode.empty() || fragCode.empty()) {
        m_lastError = "failed to load sky shader SPIR-V from: " + m_shaderDir;
        return false;
    }
    VkShaderModule vertMod = createShaderModule(m_device, vertCode);
    VkShaderModule fragMod = createShaderModule(m_device, fragCode);

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertMod, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragMod, "main",
         nullptr},
    }};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    const std::array<VkDynamicState, 2> dynStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
    dynamicState.pDynamicStates = dynStates.data();

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth test GREATER_OR_EQUAL: sky fragment depth = 0 (far plane in reverse-Z),
    // passes only where nothing was drawn (stored depth == 0.0 = clear value).
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

    VkPipelineColorBlendAttachmentState colorBlend{};
    colorBlend.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blendState{};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments = &colorBlend;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(SkyPushConstants);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount = 0;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_device, &layoutCI, nullptr, &m_skyLayout) != VK_SUCCESS) {
        m_lastError = "vkCreatePipelineLayout (sky) failed";
        vkDestroyShaderModule(m_device, vertMod, nullptr);
        vkDestroyShaderModule(m_device, fragMod, nullptr);
        return false;
    }

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
    pipelineCI.pColorBlendState = &blendState;
    pipelineCI.pDynamicState = &dynamicState;
    pipelineCI.layout = m_skyLayout;
    pipelineCI.renderPass = VK_NULL_HANDLE;

    const VkResult result =
        vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineCI, nullptr, &m_skyPipeline);
    vkDestroyShaderModule(m_device, vertMod, nullptr);
    vkDestroyShaderModule(m_device, fragMod, nullptr);

    if (result != VK_SUCCESS) {
        m_lastError = "vkCreateGraphicsPipelines (sky) failed";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Particle system — GPU compute simulation + instanced billboard rendering
// ---------------------------------------------------------------------------

// Simple LCG for per-particle random velocity directions (no stdlib rand dependency).
static float lcgFloat(uint32_t& seed) {
    seed = seed * 1664525u + 1013904223u;
    return float(seed >> 8) * (1.0f / float(1u << 24));
}

bool VkRenderer::createParticleResources() {
    const VkDeviceSize poolSize = kMaxParticles * sizeof(GpuParticle);

    // ── Particle pool SSBO (device-local) ────────────────────────────────
    {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = poolSize;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &bci, nullptr, &m_particlePoolBuf) != VK_SUCCESS) {
            m_lastError = "vkCreateBuffer (particle pool) failed";
            return false;
        }

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_device, m_particlePoolBuf, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemoryType(m_physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_device, &ai, nullptr, &m_particlePoolMemory) != VK_SUCCESS) {
            m_lastError = "vkAllocateMemory (particle pool) failed";
            return false;
        }
        vkBindBufferMemory(m_device, m_particlePoolBuf, m_particlePoolMemory, 0);
    }

    // ── Per-frame host-visible spawn staging buffers ──────────────────────
    const VkDeviceSize spawnSize = kMaxSpawnPerFrame * sizeof(GpuParticle);
    for (auto& spf : m_particleSpawn) {
        VkDeviceMemory mem;
        if (!createHostBuffer(m_device, m_physicalDevice, spawnSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, spf.buf, mem,
                              spf.mapped)) {
            m_lastError = "createHostBuffer (particle spawn) failed";
            return false;
        }
        spf.mem = mem;
    }

    // ── Zero-initialise the particle pool (all age fields = 0 = inactive) ─
    {
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = m_commandPool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer initCmd;
        vkAllocateCommandBuffers(m_device, &cai, &initCmd);

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(initCmd, &bi);
        vkCmdFillBuffer(initCmd, m_particlePoolBuf, 0, poolSize, 0u);
        vkEndCommandBuffer(initCmd);

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &initCmd;
        vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &initCmd);
    }

    // ── Compute descriptor set layout (set 0: particle pool SSBO RW) ─────
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 1;
        lci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(m_device, &lci, nullptr, &m_particleComputeSetLayout) != VK_SUCCESS) {
            m_lastError = "vkCreateDescriptorSetLayout (particle compute) failed";
            return false;
        }
    }

    // ── Render descriptor set layout (set 0: particle SSBO RO + camera UBO)
    // particle.vert: binding 0 = particle pool SSBO, binding 1 = camera UBO.
    {
        const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        }};
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = static_cast<uint32_t>(bindings.size());
        lci.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(m_device, &lci, nullptr, &m_particleRenderSetLayout) != VK_SUCCESS) {
            m_lastError = "vkCreateDescriptorSetLayout (particle render) failed";
            return false;
        }
    }

    // ── Compute descriptor pool + set ─────────────────────────────────────
    {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo poolCI{};
        poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolCI.maxSets = 1;
        poolCI.poolSizeCount = 1;
        poolCI.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(m_device, &poolCI, nullptr, &m_particleComputePool) != VK_SUCCESS) {
            m_lastError = "vkCreateDescriptorPool (particle compute) failed";
            return false;
        }

        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_particleComputePool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &m_particleComputeSetLayout;
        if (vkAllocateDescriptorSets(m_device, &ai, &m_particleComputeSet) != VK_SUCCESS) {
            m_lastError = "vkAllocateDescriptorSets (particle compute) failed";
            return false;
        }

        VkDescriptorBufferInfo poolBuf{m_particlePoolBuf, 0, kMaxParticles * sizeof(GpuParticle)};
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = m_particleComputeSet;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &poolBuf;
        vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
    }

    // ── Render descriptor pool + per-frame sets ───────────────────────────
    {
        const std::array<VkDescriptorPoolSize, 2> ps{{
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES_IN_FLIGHT},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES_IN_FLIGHT},
        }};
        VkDescriptorPoolCreateInfo poolCI{};
        poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolCI.maxSets = MAX_FRAMES_IN_FLIGHT;
        poolCI.poolSizeCount = static_cast<uint32_t>(ps.size());
        poolCI.pPoolSizes = ps.data();
        if (vkCreateDescriptorPool(m_device, &poolCI, nullptr, &m_particleRenderPool) != VK_SUCCESS) {
            m_lastError = "vkCreateDescriptorPool (particle render) failed";
            return false;
        }

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = m_particleRenderPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &m_particleRenderSetLayout;
            if (vkAllocateDescriptorSets(m_device, &ai, &m_particleRenderSets[i]) != VK_SUCCESS) {
                m_lastError = "vkAllocateDescriptorSets (particle render) failed";
                return false;
            }

            VkDescriptorBufferInfo poolBuf{m_particlePoolBuf, 0, kMaxParticles * sizeof(GpuParticle)};
            VkDescriptorBufferInfo camInfo{m_perFrame[i].cameraBuffer, 0, sizeof(CameraUBO)};
            const std::array<VkWriteDescriptorSet, 2> writes{{
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_particleRenderSets[i], 0, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &poolBuf, nullptr},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_particleRenderSets[i], 1, 0, 1,
                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &camInfo, nullptr},
            }};
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }

    return true;
}

bool VkRenderer::createParticleComputePipeline() {
    auto spv = loadSpirv(m_shaderDir + "particle_sim.comp.spv");
    if (spv.empty()) {
        m_lastError = "failed to load particle_sim.comp.spv";
        return false;
    }
    VkShaderModule mod = createShaderModule(m_device, spv);

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(ParticleSimPush);

    VkPipelineLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &m_particleComputeSetLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(m_device, &lci, nullptr, &m_particleComputeLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, mod, nullptr);
        m_lastError = "vkCreatePipelineLayout (particle compute) failed";
        return false;
    }

    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = mod;
    ci.stage.pName = "main";
    ci.layout = m_particleComputeLayout;
    VkResult result = vkCreateComputePipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &m_particleComputePipeline);
    vkDestroyShaderModule(m_device, mod, nullptr);
    if (result != VK_SUCCESS) {
        m_lastError = "vkCreateComputePipelines (particle) failed";
        return false;
    }
    return true;
}

bool VkRenderer::createParticleRenderPipelines() {
    auto vertSpv = loadSpirv(m_shaderDir + "particle.vert.spv");
    auto fragSpv = loadSpirv(m_shaderDir + "particle.frag.spv");
    if (vertSpv.empty() || fragSpv.empty()) {
        m_lastError = "failed to load particle.vert.spv or particle.frag.spv";
        return false;
    }

    VkShaderModule vertMod = createShaderModule(m_device, vertSpv);
    VkShaderModule fragMod = createShaderModule(m_device, fragSpv);

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertMod, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragMod, "main",
         nullptr},
    }};

    // No vertex input — all data comes from SSBO.
    VkPipelineVertexInputStateCreateInfo viState{};
    viState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo iaState{};
    iaState.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    iaState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpState{};
    vpState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpState.viewportCount = 1;
    vpState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterState{};
    rasterState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterState.polygonMode = VK_POLYGON_MODE_FILL;
    rasterState.cullMode = VK_CULL_MODE_NONE; // billboards face both ways
    rasterState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterState.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo msState{};
    msState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth test ON (reverse-Z GREATER), depth write OFF (transparent).
    VkPipelineDepthStencilStateCreateInfo dsState{};
    dsState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dsState.depthTestEnable = VK_TRUE;
    dsState.depthWriteEnable = VK_FALSE;
    dsState.depthCompareOp = VK_COMPARE_OP_GREATER;

    const std::array<VkDynamicState, 2> dynStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
    dynState.pDynamicStates = dynStates.data();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(uint32_t); // renderAdditive flag

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount = 1;
    layoutCI.pSetLayouts = &m_particleRenderSetLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(m_device, &layoutCI, nullptr, &m_particleRenderLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertMod, nullptr);
        vkDestroyShaderModule(m_device, fragMod, nullptr);
        m_lastError = "vkCreatePipelineLayout (particle render) failed";
        return false;
    }

    // Colour blend state is the only difference between additive and alpha pipelines.
    auto makeBlendAttachment = [](bool additive) {
        VkPipelineColorBlendAttachmentState att{};
        att.blendEnable = VK_TRUE;
        if (additive) {
            att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            att.colorBlendOp = VK_BLEND_OP_ADD;
            att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.alphaBlendOp = VK_BLEND_OP_ADD;
        } else {
            att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            att.colorBlendOp = VK_BLEND_OP_ADD;
            att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            att.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        att.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        return att;
    };

    const VkFormat hdrFmt = kHdrFormat;
    VkPipelineRenderingCreateInfo renderCI{};
    renderCI.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderCI.colorAttachmentCount = 1;
    renderCI.pColorAttachmentFormats = &hdrFmt;
    renderCI.depthAttachmentFormat = kDepthFormat;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext = &renderCI;
    ci.stageCount = static_cast<uint32_t>(stages.size());
    ci.pStages = stages.data();
    ci.pVertexInputState = &viState;
    ci.pInputAssemblyState = &iaState;
    ci.pViewportState = &vpState;
    ci.pRasterizationState = &rasterState;
    ci.pMultisampleState = &msState;
    ci.pDepthStencilState = &dsState;
    ci.pDynamicState = &dynState;
    ci.layout = m_particleRenderLayout;

    // Additive pipeline
    auto additAtt = makeBlendAttachment(true);
    VkPipelineColorBlendStateCreateInfo additBlend{};
    additBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    additBlend.attachmentCount = 1;
    additBlend.pAttachments = &additAtt;
    ci.pColorBlendState = &additBlend;
    VkResult result = vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &m_particleAdditPipeline);

    // Alpha pipeline
    auto alphaAtt = makeBlendAttachment(false);
    VkPipelineColorBlendStateCreateInfo alphaBlend{};
    alphaBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    alphaBlend.attachmentCount = 1;
    alphaBlend.pAttachments = &alphaAtt;
    ci.pColorBlendState = &alphaBlend;
    VkResult result2 = vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &m_particleAlphaPipeline);

    vkDestroyShaderModule(m_device, vertMod, nullptr);
    vkDestroyShaderModule(m_device, fragMod, nullptr);

    if (result != VK_SUCCESS || result2 != VK_SUCCESS) {
        m_lastError = "vkCreateGraphicsPipelines (particle) failed";
        return false;
    }
    return true;
}

void VkRenderer::destroyParticleResources() {
    if (m_particleAdditPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_particleAdditPipeline, nullptr);
        m_particleAdditPipeline = VK_NULL_HANDLE;
    }
    if (m_particleAlphaPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_particleAlphaPipeline, nullptr);
        m_particleAlphaPipeline = VK_NULL_HANDLE;
    }
    if (m_particleRenderLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_particleRenderLayout, nullptr);
        m_particleRenderLayout = VK_NULL_HANDLE;
    }
    if (m_particleComputePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_particleComputePipeline, nullptr);
        m_particleComputePipeline = VK_NULL_HANDLE;
    }
    if (m_particleComputeLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_particleComputeLayout, nullptr);
        m_particleComputeLayout = VK_NULL_HANDLE;
    }
    if (m_particleRenderPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_particleRenderPool, nullptr);
        m_particleRenderPool = VK_NULL_HANDLE;
    }
    if (m_particleComputePool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_particleComputePool, nullptr);
        m_particleComputePool = VK_NULL_HANDLE;
    }
    if (m_particleRenderSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_particleRenderSetLayout, nullptr);
        m_particleRenderSetLayout = VK_NULL_HANDLE;
    }
    if (m_particleComputeSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_particleComputeSetLayout, nullptr);
        m_particleComputeSetLayout = VK_NULL_HANDLE;
    }
    for (auto& spf : m_particleSpawn) {
        if (spf.mapped && spf.mem != VK_NULL_HANDLE)
            vkUnmapMemory(m_device, spf.mem);
        if (spf.buf != VK_NULL_HANDLE)
            vkDestroyBuffer(m_device, spf.buf, nullptr);
        if (spf.mem != VK_NULL_HANDLE)
            vkFreeMemory(m_device, spf.mem, nullptr);
        spf = {};
    }
    if (m_particlePoolBuf != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_particlePoolBuf, nullptr);
        m_particlePoolBuf = VK_NULL_HANDLE;
    }
    if (m_particlePoolMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_particlePoolMemory, nullptr);
        m_particlePoolMemory = VK_NULL_HANDLE;
    }
}

void VkRenderer::recordParticleCompute(VkCommandBuffer cmd, float dt) {
    // Build spawn list from the current frame's emitters.
    auto* spawnBuf = static_cast<GpuParticle*>(m_particleSpawn[m_currentFrame].mapped);
    uint32_t spawnCount = 0;

    for (const auto& emitter : m_pendingScene.particleEmitters) {
        if (!emitter.effectName || emitter.intensity <= 0.0f)
            continue;
        const uint32_t toSpawn = static_cast<uint32_t>(emitter.spawnRate * dt * emitter.intensity);

        // Tangent frame for hemisphere centred on emitDirection.
        const glm::vec3 dir = glm::length(emitter.emitDirection) > 0.5f ? glm::normalize(emitter.emitDirection)
                                                                        : glm::vec3{0.0f, 1.0f, 0.0f};
        const glm::vec3 ref = std::abs(dir.x) > 0.9f ? glm::vec3{0.0f, 1.0f, 0.0f} : glm::vec3{1.0f, 0.0f, 0.0f};
        const glm::vec3 tan = glm::normalize(glm::cross(dir, ref));
        const glm::vec3 bitan = glm::cross(tan, dir);

        for (uint32_t s = 0; s < toSpawn && spawnCount < kMaxSpawnPerFrame; ++s, ++spawnCount) {
            // LCG-based random velocity in hemisphere centred on emitDirection.
            uint32_t seed = m_nextParticleSlot * 2654435761u ^ (s * 2246822519u) ^
                            static_cast<uint32_t>(m_totalFrames * 0x9e3779b97f4a7c15ULL);
            const float theta = lcgFloat(seed) * 6.28318530f;
            const float phi = lcgFloat(seed) * 1.57079632f; // [0, pi/2] half-angle
            const float speed = emitter.initialSpeed * (0.5f + lcgFloat(seed));

            GpuParticle p{};
            p.pos = emitter.position;
            p.age = emitter.particleLifetime;
            const glm::vec3 hemVec{std::cos(theta) * std::sin(phi), std::cos(phi), std::sin(theta) * std::sin(phi)};
            p.vel = (tan * hemVec.x + dir * hemVec.y + bitan * hemVec.z) * speed;
            p.maxAge = emitter.particleLifetime;
            p.colorStart = glm::vec4(emitter.colorStart, 1.0f);
            p.colorEnd = glm::vec4(emitter.colorEnd, emitter.additive ? 0.0f : 1.0f);
            p.sizeStart = emitter.sizeStart;
            p.sizeEnd = emitter.sizeEnd;
            p.additive = emitter.additive ? 1.0f : 0.0f;
            p._pad = 0.0f;

            spawnBuf[spawnCount] = p;
            m_nextParticleSlot = (m_nextParticleSlot + 1) % kMaxParticles;
        }
    }

    if (spawnCount > 0) {
        // Barrier: HOST_WRITE → TRANSFER so the GPU sees the CPU-written spawn data.
        VkBufferMemoryBarrier hostToTransfer{};
        hostToTransfer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        hostToTransfer.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        hostToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        hostToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostToTransfer.buffer = m_particleSpawn[m_currentFrame].buf;
        hostToTransfer.offset = 0;
        hostToTransfer.size = spawnCount * sizeof(GpuParticle);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                             &hostToTransfer, 0, nullptr);

        // Copy spawned particles into the pool using the ring-buffer pointer.
        // Handle wrap-around with up to two copy regions.
        const uint32_t startSlot = (m_nextParticleSlot + kMaxParticles - spawnCount) % kMaxParticles;
        const uint32_t endSlot = startSlot + spawnCount;

        if (endSlot <= kMaxParticles) {
            VkBufferCopy region{0, startSlot * sizeof(GpuParticle), spawnCount * sizeof(GpuParticle)};
            vkCmdCopyBuffer(cmd, m_particleSpawn[m_currentFrame].buf, m_particlePoolBuf, 1, &region);
        } else {
            // Split: copy tail of ring then wrap to front.
            const uint32_t tailCount = kMaxParticles - startSlot;
            const uint32_t headCount = spawnCount - tailCount;
            const std::array<VkBufferCopy, 2> regions{{
                {0, startSlot * sizeof(GpuParticle), tailCount * sizeof(GpuParticle)},
                {tailCount * sizeof(GpuParticle), 0, headCount * sizeof(GpuParticle)},
            }};
            vkCmdCopyBuffer(cmd, m_particleSpawn[m_currentFrame].buf, m_particlePoolBuf,
                            static_cast<uint32_t>(regions.size()), regions.data());
        }

        // Barrier: TRANSFER_WRITE → COMPUTE_SHADER_READ/WRITE.
        VkBufferMemoryBarrier transferToCompute{};
        transferToCompute.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        transferToCompute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        transferToCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        transferToCompute.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        transferToCompute.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        transferToCompute.buffer = m_particlePoolBuf;
        transferToCompute.offset = 0;
        transferToCompute.size = kMaxParticles * sizeof(GpuParticle);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             1, &transferToCompute, 0, nullptr);
    }

    // Dispatch compute to integrate all active particles.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_particleComputePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_particleComputeLayout, 0, 1, &m_particleComputeSet,
                            0, nullptr);

    ParticleSimPush push{};
    push.dt = dt;
    push.count = kMaxParticles;
    push.gravity = -2.0f; // slow upward drift for smoke, negligible for fast-moving fire
    push._pad = 0.0f;
    vkCmdPushConstants(cmd, m_particleComputeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ParticleSimPush), &push);

    const uint32_t groups = (kMaxParticles + 63u) / 64u;
    vkCmdDispatch(cmd, groups, 1, 1);

    // Barrier: COMPUTE_SHADER_WRITE → VERTEX_SHADER_READ (for particle.vert).
    VkBufferMemoryBarrier computeToVertex{};
    computeToVertex.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    computeToVertex.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    computeToVertex.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    computeToVertex.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    computeToVertex.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    computeToVertex.buffer = m_particlePoolBuf;
    computeToVertex.offset = 0;
    computeToVertex.size = kMaxParticles * sizeof(GpuParticle);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0, nullptr,
                         1, &computeToVertex, 0, nullptr);
}

void VkRenderer::recordParticleDraw(VkCommandBuffer cmd) {
    VkViewport vp{0.0f, 0.0f, static_cast<float>(m_swapchainExtent.width), static_cast<float>(m_swapchainExtent.height),
                  0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{{0, 0}, m_swapchainExtent};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_particleRenderLayout, 0, 1,
                            &m_particleRenderSets[m_currentFrame], 0, nullptr);

    // Additive-blend pass (fire, explosion): renderAdditive=1 — vertex shader clips alpha particles.
    uint32_t renderAdditive = 1u;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_particleAdditPipeline);
    vkCmdPushConstants(cmd, m_particleRenderLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t), &renderAdditive);
    vkCmdDraw(cmd, 6, kMaxParticles, 0, 0);

    // Alpha-blend pass (smoke): renderAdditive=0 — vertex shader clips additive particles.
    renderAdditive = 0u;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_particleAlphaPipeline);
    vkCmdPushConstants(cmd, m_particleRenderLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t), &renderAdditive);
    vkCmdDraw(cmd, 6, kMaxParticles, 0, 0);
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
    for (auto& sem : m_renderFinished)
        if (vkCreateSemaphore(m_device, &sci, nullptr, &sem) != VK_SUCCESS) {
            m_lastError = "sync object creation failed";
            return false;
        }

    m_imagesInFlight.assign(m_swapchainImages.size(), VK_NULL_HANDLE);
    return true;
}

// ---------------------------------------------------------------------------
// Bloom images (half-resolution RGBA16F) — created alongside HDR + depth,
// destroyed in destroyAttachments() on swapchain resize.
// ---------------------------------------------------------------------------
bool VkRenderer::createBloomImages() {
    const uint32_t w = std::max(1u, m_swapchainExtent.width / 2);
    const uint32_t h = std::max(1u, m_swapchainExtent.height / 2);
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!createAttachmentImage(w, h, kHdrFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT, m_bloomImage, m_bloomMemory,
                               m_bloomView)) {
        m_lastError = "createAttachmentImage (bloom) failed";
        return false;
    }
    if (!createAttachmentImage(w, h, kHdrFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT, m_bloomAuxImage, m_bloomAuxMemory,
                               m_bloomAuxView)) {
        m_lastError = "createAttachmentImage (bloomAux) failed";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Bloom descriptor layout + pool + 3 sets (threshold in, blur-H in, blur-V in).
// Called once at init; updateBloomDescriptors() re-writes views after resize.
// ---------------------------------------------------------------------------
bool VkRenderer::createBloomDescriptors() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 1;
    layoutCI.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(m_device, &layoutCI, nullptr, &m_bloomSetLayout) != VK_SUCCESS) {
        m_lastError = "vkCreateDescriptorSetLayout (bloom) failed";
        return false;
    }

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = 3;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(m_device, &poolCI, nullptr, &m_bloomPool) != VK_SUCCESS) {
        m_lastError = "vkCreateDescriptorPool (bloom) failed";
        return false;
    }

    const std::array<VkDescriptorSetLayout, 3> layouts{m_bloomSetLayout, m_bloomSetLayout, m_bloomSetLayout};
    VkDescriptorSetAllocateInfo allocCI{};
    allocCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocCI.descriptorPool = m_bloomPool;
    allocCI.descriptorSetCount = 3;
    allocCI.pSetLayouts = layouts.data();
    VkDescriptorSet sets[3]{};
    if (vkAllocateDescriptorSets(m_device, &allocCI, sets) != VK_SUCCESS) {
        m_lastError = "vkAllocateDescriptorSets (bloom) failed";
        return false;
    }
    m_bloomThresholdSet = sets[0];
    m_bloomBlurHSet = sets[1];
    m_bloomBlurVSet = sets[2];

    updateBloomDescriptors();
    return true;
}

void VkRenderer::updateBloomDescriptors() {
    auto write = [&](VkDescriptorSet set, VkImageView view) {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler = m_hdrSampler; // linear, clamp-to-edge — same as tonemap
        imgInfo.imageView = view;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
    };
    write(m_bloomThresholdSet, m_hdrView);  // threshold reads HDR
    write(m_bloomBlurHSet, m_bloomView);    // blur H reads bloom → writes aux
    write(m_bloomBlurVSet, m_bloomAuxView); // blur V reads aux → writes bloom
}

// ---------------------------------------------------------------------------
// Bloom pipelines (threshold + separable blur).  Both use tonemap.vert
// (fullscreen triangle) + their respective frag shaders.
// ---------------------------------------------------------------------------
bool VkRenderer::createBloomPipelines() {
    auto vertCode = loadSpirv(m_shaderDir + "tonemap.vert.spv");
    auto threshCode = loadSpirv(m_shaderDir + "bloom_threshold.frag.spv");
    auto blurCode = loadSpirv(m_shaderDir + "bloom_blur.frag.spv");
    if (vertCode.empty() || threshCode.empty() || blurCode.empty()) {
        m_lastError = "failed to load bloom shader SPIR-V from: " + m_shaderDir;
        return false;
    }

    VkShaderModule vertMod = createShaderModule(m_device, vertCode);
    VkShaderModule threshMod = createShaderModule(m_device, threshCode);
    VkShaderModule blurMod = createShaderModule(m_device, blurCode);

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(BloomPush);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount = 1;
    layoutCI.pSetLayouts = &m_bloomSetLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_device, &layoutCI, nullptr, &m_bloomLayout) != VK_SUCCESS) {
        m_lastError = "vkCreatePipelineLayout (bloom) failed";
        vkDestroyShaderModule(m_device, vertMod, nullptr);
        vkDestroyShaderModule(m_device, threshMod, nullptr);
        vkDestroyShaderModule(m_device, blurMod, nullptr);
        return false;
    }

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
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo noDepth{};
    noDepth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAtt;

    VkFormat hdrFmt = kHdrFormat;
    VkPipelineRenderingCreateInfo renderingCI{};
    renderingCI.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingCI.colorAttachmentCount = 1;
    renderingCI.pColorAttachmentFormats = &hdrFmt;

    auto makeStages = [](VkShaderModule v, VkShaderModule f) {
        return std::array<VkPipelineShaderStageCreateInfo, 2>{{
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, v, "main",
             nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, f, "main",
             nullptr},
        }};
    };

    VkGraphicsPipelineCreateInfo pipelineCI{};
    pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCI.pNext = &renderingCI;
    pipelineCI.pVertexInputState = &vertexInput;
    pipelineCI.pInputAssemblyState = &inputAssembly;
    pipelineCI.pViewportState = &viewportState;
    pipelineCI.pRasterizationState = &rasterizer;
    pipelineCI.pMultisampleState = &multisampling;
    pipelineCI.pDepthStencilState = &noDepth;
    pipelineCI.pColorBlendState = &colorBlend;
    pipelineCI.pDynamicState = &dynamicState;
    pipelineCI.layout = m_bloomLayout;
    pipelineCI.renderPass = VK_NULL_HANDLE;

    auto threshStages = makeStages(vertMod, threshMod);
    pipelineCI.stageCount = 2;
    pipelineCI.pStages = threshStages.data();
    if (vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineCI, nullptr, &m_bloomThresholdPipeline) !=
        VK_SUCCESS) {
        m_lastError = "vkCreateGraphicsPipelines (bloom threshold) failed";
        vkDestroyShaderModule(m_device, vertMod, nullptr);
        vkDestroyShaderModule(m_device, threshMod, nullptr);
        vkDestroyShaderModule(m_device, blurMod, nullptr);
        return false;
    }

    auto blurStages = makeStages(vertMod, blurMod);
    pipelineCI.pStages = blurStages.data();
    if (vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineCI, nullptr, &m_bloomBlurPipeline) !=
        VK_SUCCESS) {
        m_lastError = "vkCreateGraphicsPipelines (bloom blur) failed";
        vkDestroyShaderModule(m_device, vertMod, nullptr);
        vkDestroyShaderModule(m_device, threshMod, nullptr);
        vkDestroyShaderModule(m_device, blurMod, nullptr);
        return false;
    }

    vkDestroyShaderModule(m_device, vertMod, nullptr);
    vkDestroyShaderModule(m_device, threshMod, nullptr);
    vkDestroyShaderModule(m_device, blurMod, nullptr);
    return true;
}

void VkRenderer::destroyBloomResources() {
    auto destroyPipeline = [this](VkPipeline& p) {
        if (p != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, p, nullptr);
            p = VK_NULL_HANDLE;
        }
    };
    destroyPipeline(m_bloomThresholdPipeline);
    destroyPipeline(m_bloomBlurPipeline);
    if (m_bloomLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_bloomLayout, nullptr);
        m_bloomLayout = VK_NULL_HANDLE;
    }
    if (m_bloomPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_bloomPool, nullptr);
        m_bloomPool = VK_NULL_HANDLE;
        m_bloomThresholdSet = VK_NULL_HANDLE;
        m_bloomBlurHSet = VK_NULL_HANDLE;
        m_bloomBlurVSet = VK_NULL_HANDLE;
    }
    if (m_bloomSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_bloomSetLayout, nullptr);
        m_bloomSetLayout = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// recordBloomPasses — brightness threshold + separable Gaussian blur.
// On entry: HDR is SHADER_READ_ONLY (transitioned before this call).
// On exit:  m_bloomImage is SHADER_READ_ONLY (ready for tonemap composite).
// ---------------------------------------------------------------------------
void VkRenderer::recordBloomPasses(VkCommandBuffer cmd) {
    const uint32_t bw = std::max(1u, m_swapchainExtent.width / 2);
    const uint32_t bh = std::max(1u, m_swapchainExtent.height / 2);
    const VkExtent2D bloomExtent{bw, bh};
    const VkViewport bloomVP{0.0f, 0.0f, float(bw), float(bh), 0.0f, 1.0f};
    const VkRect2D bloomScissor{{0, 0}, bloomExtent};

    BloomPush pc{};
    pc.texelSizeX = 1.0f / float(bw);
    pc.texelSizeY = 1.0f / float(bh);

    auto beginBloomPass = [&](VkCommandBuffer c, VkImageView target) {
        VkRenderingAttachmentInfo att{};
        att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView = target;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = {{0, 0}, bloomExtent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(c, &ri);
        vkCmdSetViewport(c, 0, 1, &bloomVP);
        vkCmdSetScissor(c, 0, 1, &bloomScissor);
    };

    // ── Threshold: HDR (SHADER_READ_ONLY) → bloom (UNDEFINED → COLOR_ATT) ─
    imageBarrier(cmd, m_bloomImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    beginBloomPass(cmd, m_bloomView);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomThresholdPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomLayout, 0, 1, &m_bloomThresholdSet, 0,
                            nullptr);
    pc.isVertical = 0u;
    vkCmdPushConstants(cmd, m_bloomLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BloomPush), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    // ── Blur H: bloom (COLOR_ATT → SHADER_READ) → aux (UNDEF → COLOR_ATT) ─
    imageBarrier(cmd, m_bloomImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    imageBarrier(cmd, m_bloomAuxImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    beginBloomPass(cmd, m_bloomAuxView);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomBlurPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomLayout, 0, 1, &m_bloomBlurHSet, 0, nullptr);
    pc.isVertical = 0u;
    vkCmdPushConstants(cmd, m_bloomLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BloomPush), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    // ── Blur V: aux (COLOR_ATT → SHADER_READ) → bloom (SHADER_READ → COLOR_ATT → SHADER_READ) ─
    imageBarrier(cmd, m_bloomAuxImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    imageBarrier(cmd, m_bloomImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    beginBloomPass(cmd, m_bloomView);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomLayout, 0, 1, &m_bloomBlurVSet, 0, nullptr);
    pc.isVertical = 1u;
    vkCmdPushConstants(cmd, m_bloomLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BloomPush), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    // ── bloom (COLOR_ATT) → SHADER_READ_ONLY for tonemap composite ────────
    imageBarrier(cmd, m_bloomImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// ---------------------------------------------------------------------------
// recordCommandBuffer
// ---------------------------------------------------------------------------
void VkRenderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex) {
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    // Timestamp: reset slots for this frame and write the "begin" timestamp.
    if (m_timestampSupported && m_timestampPool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd, m_timestampPool, m_currentFrame * 2, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_timestampPool, m_currentFrame * 2);
    }

    m_drawCallCount = 0;

    // ── Particle compute (simulate + spawn new particles) ─────────────────
    recordParticleCompute(cmd, m_frameDt);

    // ── Shadow map (all cascades) → DEPTH_ATTACHMENT ─────────────────────
    imageBarrier(cmd, m_shadowImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT, kNumCascades);

    // ── Shadow passes — one per cascade ──────────────────────────────────
    for (uint32_t c = 0; c < kNumCascades; ++c) {
        VkRenderingAttachmentInfo depthAtt{};
        depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAtt.imageView = m_shadowLayerViews[c];
        depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAtt.clearValue.depthStencil = {1.0f, 0}; // forward-Z: far = 1.0

        VkRenderingInfo renderInfo{};
        renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderInfo.renderArea = {{0, 0}, {kShadowRes, kShadowRes}};
        renderInfo.layerCount = 1;
        renderInfo.pDepthAttachment = &depthAtt;

        vkCmdBeginRendering(cmd, &renderInfo);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);

        VkViewport vp{0.0f, 0.0f, float(kShadowRes), float(kShadowRes), 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D sc{{0, 0}, {kShadowRes, kShadowRes}};
        vkCmdSetScissor(cmd, 0, 1, &sc);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowLayout, 0, 1,
                                &m_perFrame[m_currentFrame].shadowDescriptorSet, 0, nullptr);

        for (const auto& item : m_pendingScene.renderItems) {
            const GpuMesh* mesh = m_resources.getMesh(item.mesh);
            if (!mesh)
                continue;
            ShadowPushConstants pc{};
            pc.model = item.transform;
            pc.cascadeIdx = c;
            vkCmdPushConstants(cmd, m_shadowLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ShadowPushConstants), &pc);
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, mesh->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, mesh->indexCount, 1, 0, 0, 0);
        }

        vkCmdEndRendering(cmd);
    }

    // ── Shadow map → SHADER_READ_ONLY ────────────────────────────────────
    imageBarrier(cmd, m_shadowImage, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, kNumCascades);

    // ── HDR → COLOR_ATTACHMENT_OPTIMAL ───────────────────────────────────
    imageBarrier(cmd, m_hdrImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    // ── depth → DEPTH_STENCIL_ATTACHMENT_OPTIMAL ─────────────────────────
    imageBarrier(cmd, m_depthImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT);

    // ── Forward pass ──────────────────────────────────────────────────────
    {
        VkRenderingAttachmentInfo colorAtt{};
        colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAtt.imageView = m_hdrView;
        colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.clearValue.color = {{0.05f, 0.10f, 0.18f, 1.0f}};

        VkRenderingAttachmentInfo depthAtt{};
        depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAtt.imageView = m_depthView;
        depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE; // kept for sky+particle depth test
        depthAtt.clearValue.depthStencil = {0.0f, 0};    // reverse-Z: far = 0

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

        // Bind per-frame descriptor set (set 0: camera + light UBOs).
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_forwardLayout, 0, 1,
                                &m_perFrame[m_currentFrame].descriptorSet, 0, nullptr);

        // Draw each submitted RenderItem.
        for (const auto& item : m_pendingScene.renderItems) {
            const GpuMesh* mesh = m_resources.getMesh(item.mesh);
            if (!mesh)
                continue;

            // Resolve material; fall back to a default if invalid.
            const GpuMaterial* mat = m_resources.getMaterial(item.material);

            // Bind per-material descriptor set (set 1: base color texture).
            if (mat && mat->descriptorSet != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_forwardLayout, 1, 1,
                                        &mat->descriptorSet, 0, nullptr);
            }

            // Push per-object constants.
            ForwardPushConstants pc{};
            pc.model = item.transform;
            pc.baseColorFactor = mat ? mat->baseColorFactor : glm::vec4(1.0f);
            pc.metallicFactor = mat ? mat->metallicFactor : 0.0f;
            pc.roughnessFactor = mat ? mat->roughnessFactor : 1.0f;
            vkCmdPushConstants(cmd, m_forwardLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(pc), &pc);

            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, mesh->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, mesh->indexCount, 1, 0, 0, 0);
        }

        vkCmdEndRendering(cmd);
    }

    // ── Sky pass — fills pixels where depth == 0 (reverse-Z far, nothing drawn) ──
    {
        VkRenderingAttachmentInfo colorAtt{};
        colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAtt.imageView = m_hdrView;
        colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingAttachmentInfo depthAtt{};
        depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAtt.imageView = m_depthView;
        depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

        VkRenderingInfo renderInfo{};
        renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderInfo.renderArea = {{0, 0}, m_swapchainExtent};
        renderInfo.layerCount = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments = &colorAtt;
        renderInfo.pDepthAttachment = &depthAtt;

        vkCmdBeginRendering(cmd, &renderInfo);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyPipeline);

        VkViewport viewport{
            0.0f, 0.0f, static_cast<float>(m_swapchainExtent.width), static_cast<float>(m_swapchainExtent.height),
            0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, m_swapchainExtent};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        SkyPushConstants skyPC{};
        skyPC.invViewProj = glm::inverse(m_pendingScene.camera.proj * m_pendingScene.camera.view);
        skyPC.sunDirection = glm::vec4(m_pendingScene.environment.sunDirection, 0.0f);
        skyPC.sunColor = glm::vec4(m_pendingScene.environment.sunColor, 1.0f);
        {
            // Derive horizon color and cloud coverage from environment state.
            const auto& env = m_pendingScene.environment;
            float cloudCov = env.cloudCoverage;
            // Warmth driven by sun elevation, not sun color: warm near horizon (dawn/dusk),
            // cool blue at high elevation (midday). avoids tan sky at moderate elevation.
            float elevation = env.sunDirection.y; // [-1,1]; 1=noon, 0=horizon, <0=night
            float warmth = 1.0f - glm::clamp(elevation / 0.3f, 0.0f, 1.0f);
            glm::vec3 horizonDay{0.40f, 0.55f, 0.75f};
            glm::vec3 horizonDusk{0.85f, 0.55f, 0.25f};
            glm::vec3 horizonStorm{0.25f, 0.28f, 0.32f};
            glm::vec3 horizon = glm::mix(glm::mix(horizonDay, horizonDusk, warmth), horizonStorm, cloudCov);
            skyPC.skyParams = glm::vec4(horizon, cloudCov);
            skyPC.fogParams = glm::vec4(env.fogDensity,
                                        env.fogStartDist / 1000.0f, // metres → km for sky shader
                                        env.timeOfDay, 0.0f);
        }
        vkCmdPushConstants(cmd, m_skyLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SkyPushConstants), &skyPC);

        vkCmdDraw(cmd, 3, 1, 0, 0); // fullscreen triangle

        // ── Particle billboard draw (shares the HDR+depth scope with sky) ─
        recordParticleDraw(cmd);

        vkCmdEndRendering(cmd);
    }

    // ── Transparent pass — alpha-blended items sorted back-to-front ───────
    {
        // Collect transparent render items (material.alphaBlend == true).
        std::vector<uint32_t> transIndices;
        const auto& items = m_pendingScene.renderItems;
        for (uint32_t i = 0; i < static_cast<uint32_t>(items.size()); ++i) {
            const GpuMaterial* mat = m_resources.getMaterial(items[i].material);
            if (mat && mat->alphaBlend)
                transIndices.push_back(i);
        }

        if (!transIndices.empty()) {
            // Sort back-to-front (descending squared distance) for correct blending.
            std::sort(transIndices.begin(), transIndices.end(), [&](uint32_t a, uint32_t b) {
                const glm::vec3 ta(items[a].transform[3]);
                const glm::vec3 tb(items[b].transform[3]);
                return glm::dot(ta, ta) > glm::dot(tb, tb);
            });

            VkRenderingAttachmentInfo colorAtt{};
            colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAtt.imageView = m_hdrView;
            colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingAttachmentInfo depthAtt{};
            depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAtt.imageView = m_depthView;
            depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

            VkRenderingInfo renderInfo{};
            renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderInfo.renderArea = {{0, 0}, m_swapchainExtent};
            renderInfo.layerCount = 1;
            renderInfo.colorAttachmentCount = 1;
            renderInfo.pColorAttachments = &colorAtt;
            renderInfo.pDepthAttachment = &depthAtt;

            vkCmdBeginRendering(cmd, &renderInfo);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_forwardAlphaPipeline);

            VkViewport viewport{
                0.0f, 0.0f, static_cast<float>(m_swapchainExtent.width), static_cast<float>(m_swapchainExtent.height),
                0.0f, 1.0f};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            VkRect2D scissor{{0, 0}, m_swapchainExtent};
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_forwardLayout, 0, 1,
                                    &m_perFrame[m_currentFrame].descriptorSet, 0, nullptr);

            for (uint32_t idx : transIndices) {
                const RenderItem& item = items[idx];
                const GpuMesh* mesh = m_resources.getMesh(item.mesh);
                if (!mesh)
                    continue;
                const GpuMaterial* mat = m_resources.getMaterial(item.material);
                if (mat && mat->descriptorSet != VK_NULL_HANDLE)
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_forwardLayout, 1, 1,
                                            &mat->descriptorSet, 0, nullptr);
                ForwardPushConstants pc{};
                pc.model = item.transform;
                pc.baseColorFactor = mat ? mat->baseColorFactor : glm::vec4(1.0f);
                pc.metallicFactor = mat ? mat->metallicFactor : 0.0f;
                pc.roughnessFactor = mat ? mat->roughnessFactor : 1.0f;
                vkCmdPushConstants(cmd, m_forwardLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(pc), &pc);
                const VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertexBuffer, &offset);
                vkCmdBindIndexBuffer(cmd, mesh->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, mesh->indexCount, 1, 0, 0, 0);
            }

            vkCmdEndRendering(cmd);
        }
    }

    // ── HDR → SHADER_READ_ONLY ───────────────────────────────────────────
    imageBarrier(cmd, m_hdrImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // ── swapchain → COLOR_ATTACHMENT_OPTIMAL ──────────────────────────────
    imageBarrier(cmd, m_swapchainImages[imageIndex], VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    // ── Bloom passes (threshold → blur H → blur V) ────────────────────────
    if (m_settings.bloom)
        recordBloomPasses(cmd);

    // ── Tonemap pass (Khronos PBR Neutral + optional FXAA + bloom blend) ─
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

        TonemapPush pc{};
        pc.texelSizeX = 1.0f / static_cast<float>(m_swapchainExtent.width);
        pc.texelSizeY = 1.0f / static_cast<float>(m_swapchainExtent.height);
        pc.enableFxaa = m_settings.antiAliasing ? 1u : 0u;
        pc.bloomStrength = m_settings.bloom ? 0.04f : 0.0f;
        vkCmdPushConstants(cmd, m_tonemapLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

        vkCmdDraw(cmd, 3, 1, 0, 0); // fullscreen triangle
        vkCmdEndRendering(cmd);
    }

    // Accumulate draw-call count: entity passes + sky + 2 particle passes + tonemap + bloom (3).
    m_drawCallCount = static_cast<uint32_t>(m_pendingScene.renderItems.size()) + 7u;

    // ── Overlay pass (debug text — after tonemap, before PRESENT transition) ─
    if (!m_overlayLines.empty()) {
        if (!m_overlayReady)
            createOverlayPipeline();
        if (m_overlayReady)
            recordOverlayPass(cmd);
    }

    // ── swapchain → PRESENT_SRC_KHR ───────────────────────────────────────
    imageBarrier(cmd, m_swapchainImages[imageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    // Timestamp: write "end" timestamp.
    if (m_timestampSupported && m_timestampPool != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_timestampPool, m_currentFrame * 2 + 1);

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
    const int w = m_iWindow->width();
    const int h = m_iWindow->height();
    if (w == 0 || h == 0)
        return false;

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
    if (!createBloomImages())
        return false;
    updateHdrDescriptor();
    updateBloomDescriptors();

    m_imagesInFlight.assign(m_swapchainImages.size(), VK_NULL_HANDLE);
    return true;
}

// ---------------------------------------------------------------------------
// getFrameStats / setOverlayLines
// ---------------------------------------------------------------------------

FrameStats VkRenderer::getFrameStats() const {
    return m_frameStats;
}

void VkRenderer::setOverlayLines(std::span<const std::string_view> lines) {
    m_overlayLines.assign(lines.begin(), lines.end());
}

void VkRenderer::submitHudElements(std::span<const HudElement> elements) {
    m_hudElements = elements;
}

// ---------------------------------------------------------------------------
// Overlay pipeline — created lazily on first non-empty setOverlayLines call
// ---------------------------------------------------------------------------

// Vertex layout: pos(2×f32) uv(2×f32) color(4×f32) = 32 bytes
struct OverlayVertex {
    float pos[2];
    float uv[2];
    float color[4];
};
static constexpr uint32_t kOverlayVertexSize = sizeof(OverlayVertex); // 32

bool VkRenderer::createOverlayPipeline() {
    // ── Font texture (R8_UNORM 4096×2048 expanded from 1bpp kUnifontBitmap) ──
    // Created directly via raw Vulkan — VkResources::createTexture() expects KTX2/PNG,
    // not raw pixel data, so bypassing it avoids silent fallback to the white default.
    // Atlas layout: 512 glyph columns × 128 glyph rows (each glyph 8×16 px).
    {
        constexpr uint32_t kFontW = 4096; // 512 cols × 8 px
        constexpr uint32_t kFontH = 2048; // 128 rows × 16 px
        std::vector<uint8_t> atlas(kFontW * kFontH, 0u);
        for (int cp = 0; cp < 65536; ++cp) {
            const int col = cp % 512;
            const int row = cp / 512;
            for (int y = 0; y < 16; ++y) {
                const uint8_t bits = kUnifontBitmap[cp * 16 + y];
                for (int x = 0; x < 8; ++x) {
                    const int px = col * 8 + x;
                    const int py = row * 16 + y;
                    atlas[static_cast<size_t>(py) * kFontW + static_cast<size_t>(px)] =
                        (bits & (0x80u >> x)) ? 0xFFu : 0x00u;
                }
            }
        }
        // Reserve codepoint U+FFFF (col=511, row=127) as a solid-white block for
        // line/rect HUD fills. Sampling any UV within this glyph returns 1.0.
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 8; ++x)
                atlas[static_cast<size_t>(127 * 16 + y) * kFontW + static_cast<size_t>(511 * 8 + x)] = 0xFFu;

        // Staging buffer (host-visible, one-shot upload).
        VkBuffer stagingBuf{VK_NULL_HANDLE};
        VkDeviceMemory stagingMem{VK_NULL_HANDLE};
        {
            VkBufferCreateInfo bci{};
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size = atlas.size();
            bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            vkCreateBuffer(m_device, &bci, nullptr, &stagingBuf);
            VkMemoryRequirements mr{};
            vkGetBufferMemoryRequirements(m_device, stagingBuf, &mr);
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex =
                findMemoryType(m_physicalDevice, mr.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(m_device, &ai, nullptr, &stagingMem);
            vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);
            void* mapped = nullptr;
            vkMapMemory(m_device, stagingMem, 0, atlas.size(), 0, &mapped);
            std::memcpy(mapped, atlas.data(), atlas.size());
            vkUnmapMemory(m_device, stagingMem);
        }

        // Device-local image.
        {
            VkImageCreateInfo ici{};
            ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = VK_FORMAT_R8_UNORM;
            ici.extent = {kFontW, kFontH, 1};
            ici.mipLevels = 1;
            ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_device, &ici, nullptr, &m_fontImage) != VK_SUCCESS) {
                m_lastError = "overlay: vkCreateImage (font) failed";
                vkDestroyBuffer(m_device, stagingBuf, nullptr);
                vkFreeMemory(m_device, stagingMem, nullptr);
                return false;
            }
            VkMemoryRequirements mr{};
            vkGetImageMemoryRequirements(m_device, m_fontImage, &mr);
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex =
                findMemoryType(m_physicalDevice, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            vkAllocateMemory(m_device, &ai, nullptr, &m_fontImageMemory);
            vkBindImageMemory(m_device, m_fontImage, m_fontImageMemory, 0);
        }

        // One-time command buffer: transition → copy → transition.
        {
            VkCommandBufferAllocateInfo cbai{};
            cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbai.commandPool = m_commandPool;
            cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = 1;
            VkCommandBuffer cmd{VK_NULL_HANDLE};
            vkAllocateCommandBuffers(m_device, &cbai, &cmd);
            VkCommandBufferBeginInfo cbbi{};
            cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &cbbi);

            imageBarrier(cmd, m_fontImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);

            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {kFontW, kFontH, 1};
            vkCmdCopyBufferToImage(cmd, stagingBuf, m_fontImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            imageBarrier(cmd, m_fontImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

            vkEndCommandBuffer(cmd);
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
            vkQueueWaitIdle(m_graphicsQueue);
            vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
        }

        vkDestroyBuffer(m_device, stagingBuf, nullptr);
        vkFreeMemory(m_device, stagingMem, nullptr);

        // Image view.
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = m_fontImage;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(m_device, &vci, nullptr, &m_fontImageView) != VK_SUCCESS) {
            m_lastError = "overlay: vkCreateImageView (font) failed";
            return false;
        }
    }

    // ── Font sampler (nearest, no mip) ────────────────────────────────────
    {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_device, &sci, nullptr, &m_overlayFontSampler) != VK_SUCCESS) {
            m_lastError = "overlay: vkCreateSampler failed";
            return false;
        }
    }

    // ── Descriptor set layout: binding 0 = combined image sampler ─────────
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dsLayoutCI{};
        dsLayoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsLayoutCI.bindingCount = 1;
        dsLayoutCI.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(m_device, &dsLayoutCI, nullptr, &m_overlayDsLayout) != VK_SUCCESS) {
            m_lastError = "overlay: vkCreateDescriptorSetLayout failed";
            return false;
        }

        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo dpCI{};
        dpCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpCI.maxSets = 1;
        dpCI.poolSizeCount = 1;
        dpCI.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(m_device, &dpCI, nullptr, &m_overlayDsPool) != VK_SUCCESS) {
            m_lastError = "overlay: vkCreateDescriptorPool failed";
            return false;
        }

        VkDescriptorSetAllocateInfo dsAlloc{};
        dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsAlloc.descriptorPool = m_overlayDsPool;
        dsAlloc.descriptorSetCount = 1;
        dsAlloc.pSetLayouts = &m_overlayDsLayout;
        if (vkAllocateDescriptorSets(m_device, &dsAlloc, &m_overlayDs) != VK_SUCCESS) {
            m_lastError = "overlay: vkAllocateDescriptorSets failed";
            return false;
        }

        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler = m_overlayFontSampler;
        imgInfo.imageView = m_fontImageView;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet wr{};
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = m_overlayDs;
        wr.dstBinding = 0;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(m_device, 1, &wr, 0, nullptr);
    }

    // ── Vertex buffer (host-visible, overlay text + 2D HUD elements) ──────
    {
        const VkDeviceSize vbSize = (kMaxOverlayChars + kMaxHudVerts) * 6u * kOverlayVertexSize;
        VkBufferCreateInfo vbCI{};
        vbCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        vbCI.size = vbSize;
        vbCI.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        vbCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &vbCI, nullptr, &m_overlayVB) != VK_SUCCESS) {
            m_lastError = "overlay: vkCreateBuffer failed";
            return false;
        }
        VkMemoryRequirements vbMR{};
        vkGetBufferMemoryRequirements(m_device, m_overlayVB, &vbMR);
        VkMemoryAllocateInfo vbAI{};
        vbAI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        vbAI.allocationSize = vbMR.size;
        vbAI.memoryTypeIndex =
            findMemoryType(m_physicalDevice, vbMR.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &vbAI, nullptr, &m_overlayVBMemory) != VK_SUCCESS) {
            m_lastError = "overlay: vkAllocateMemory failed";
            return false;
        }
        vkBindBufferMemory(m_device, m_overlayVB, m_overlayVBMemory, 0);
        vkMapMemory(m_device, m_overlayVBMemory, 0, vbSize, 0, &m_overlayVBMapped);
    }

    // ── Pipeline layout ────────────────────────────────────────────────────
    {
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pcRange.offset = 0;
        pcRange.size = 8; // vec2 screenSize
        VkPipelineLayoutCreateInfo plCI{};
        plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plCI.setLayoutCount = 1;
        plCI.pSetLayouts = &m_overlayDsLayout;
        plCI.pushConstantRangeCount = 1;
        plCI.pPushConstantRanges = &pcRange;
        if (vkCreatePipelineLayout(m_device, &plCI, nullptr, &m_overlayPipelineLayout) != VK_SUCCESS) {
            m_lastError = "overlay: vkCreatePipelineLayout failed";
            return false;
        }
    }

    // ── Shaders ────────────────────────────────────────────────────────────
    auto vertCode = loadSpirv(m_shaderDir + "overlay.vert.spv");
    auto fragCode = loadSpirv(m_shaderDir + "overlay.frag.spv");
    if (vertCode.empty() || fragCode.empty()) {
        m_lastError = "overlay: failed to load shader SPIR-V from: " + m_shaderDir;
        return false;
    }
    VkShaderModule vertMod = createShaderModule(m_device, vertCode);
    VkShaderModule fragMod = createShaderModule(m_device, fragCode);

    // ── Pipeline ───────────────────────────────────────────────────────────
    {
        VkVertexInputBindingDescription vbd{};
        vbd.binding = 0;
        vbd.stride = kOverlayVertexSize;
        vbd.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription vattrs[3]{};
        vattrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};        // pos
        vattrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 8};        // uv
        vattrs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16}; // color

        VkPipelineVertexInputStateCreateInfo vis{};
        vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vis.vertexBindingDescriptionCount = 1;
        vis.pVertexBindingDescriptions = &vbd;
        vis.vertexAttributeDescriptionCount = 3;
        vis.pVertexAttributeDescriptions = vattrs;

        VkPipelineInputAssemblyStateCreateInfo ias{};
        ias.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vss{};
        vss.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vss.viewportCount = 1;
        vss.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rast{};
        rast.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rast.polygonMode = VK_POLYGON_MODE_FILL;
        rast.cullMode = VK_CULL_MODE_NONE;
        rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rast.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo msaa{};
        msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthSt{};
        depthSt.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo cbs{};
        cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbs.attachmentCount = 1;
        cbs.pAttachments = &cba;

        const VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynSt{};
        dynSt.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynSt.dynamicStateCount = 2;
        dynSt.pDynamicStates = dynStates;

        const VkFormat swapFmt = m_swapchainFormat;
        VkPipelineRenderingCreateInfo prc{};
        prc.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        prc.colorAttachmentCount = 1;
        prc.pColorAttachmentFormats = &swapFmt;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertMod;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragMod;
        stages[1].pName = "main";

        VkGraphicsPipelineCreateInfo pipeCI{};
        pipeCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeCI.pNext = &prc;
        pipeCI.stageCount = 2;
        pipeCI.pStages = stages;
        pipeCI.pVertexInputState = &vis;
        pipeCI.pInputAssemblyState = &ias;
        pipeCI.pViewportState = &vss;
        pipeCI.pRasterizationState = &rast;
        pipeCI.pMultisampleState = &msaa;
        pipeCI.pDepthStencilState = &depthSt;
        pipeCI.pColorBlendState = &cbs;
        pipeCI.pDynamicState = &dynSt;
        pipeCI.layout = m_overlayPipelineLayout;

        const VkResult r =
            vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipeCI, nullptr, &m_overlayPipeline);
        vkDestroyShaderModule(m_device, vertMod, nullptr);
        vkDestroyShaderModule(m_device, fragMod, nullptr);

        if (r != VK_SUCCESS) {
            m_lastError = "overlay: vkCreateGraphicsPipelines failed";
            return false;
        }
    }

    m_overlayReady = true;
    return true;
}

void VkRenderer::recordOverlayPass(VkCommandBuffer cmd) {
    constexpr float kCharW = 8.0f;
    constexpr float kCharH = 16.0f;
    constexpr float kAtlasW = 4096.0f; // 512 cols × 8 px
    constexpr float kAtlasH = 2048.0f; // 128 rows × 16 px
    constexpr float kMarginX = 8.0f;
    constexpr float kMarginY = 8.0f;
    constexpr float kLineGap = 2.0f;

    auto* verts = static_cast<OverlayVertex*>(m_overlayVBMapped);
    uint32_t vertCount = 0;
    float cy = kMarginY;

    for (const std::string_view& line : m_overlayLines) {
        float cx = kMarginX;
        const char* p = line.data();
        const char* pEnd = p + line.size();
        while (p < pEnd) {
            if (vertCount + 6 > kMaxOverlayChars * 6u)
                break;

            const uint32_t cp = fl::nextUtf8Codepoint(p, pEnd);
            const uint32_t glyphCol = cp % 512u;
            const uint32_t glyphRow = cp / 512u;
            const float u0 = static_cast<float>(glyphCol) * kCharW / kAtlasW;
            const float u1 = u0 + kCharW / kAtlasW;
            const float v0 = static_cast<float>(glyphRow) * kCharH / kAtlasH;
            const float v1 = v0 + kCharH / kAtlasH;
            const float x0 = cx, x1 = cx + kCharW;
            const float y0 = cy, y1 = cy + kCharH;
            constexpr float kW = 1.0f;

            verts[vertCount++] = {{x0, y0}, {u0, v0}, {kW, kW, kW, kW}};
            verts[vertCount++] = {{x1, y0}, {u1, v0}, {kW, kW, kW, kW}};
            verts[vertCount++] = {{x1, y1}, {u1, v1}, {kW, kW, kW, kW}};
            verts[vertCount++] = {{x0, y0}, {u0, v0}, {kW, kW, kW, kW}};
            verts[vertCount++] = {{x1, y1}, {u1, v1}, {kW, kW, kW, kW}};
            verts[vertCount++] = {{x0, y1}, {u0, v1}, {kW, kW, kW, kW}};
            cx += kCharW;
        }
        cy += kCharH + kLineGap;
    }

    // ── 2D HUD elements ────────────────────────────────────────────────────
    // UV center of U+FFFF (col=511, row=127) — solid-white block reserved in atlas.
    constexpr float kWhiteU = (511.f * kCharW + kCharW * 0.5f) / kAtlasW;
    constexpr float kWhiteV = (127.f * kCharH + kCharH * 0.5f) / kAtlasH;

    const float vpW = static_cast<float>(m_swapchainExtent.width);
    const float vpH = static_cast<float>(m_swapchainExtent.height);
    const uint32_t kTotalBudget = (kMaxOverlayChars + kMaxHudVerts) * 6u;

    for (const HudElement& el : m_hudElements) {
        switch (el.type) {
        case HudElement::Type::Text: {
            float cx = el.x * vpW;
            float baseY = el.y * vpH;
            float cw = kCharW * el.scale;
            float ch = kCharH * el.scale;
            {
                const char* tp = el.text.data();
                const char* tEnd = tp + el.text.size();
                while (tp < tEnd) {
                    if (vertCount + 6 > kTotalBudget)
                        break;
                    const uint32_t cp = fl::nextUtf8Codepoint(tp, tEnd);
                    const uint32_t glyphCol = cp % 512u;
                    const uint32_t glyphRow = cp / 512u;
                    const float u0 = static_cast<float>(glyphCol) * kCharW / kAtlasW;
                    const float u1 = u0 + kCharW / kAtlasW;
                    const float v0 = static_cast<float>(glyphRow) * kCharH / kAtlasH;
                    const float v1 = v0 + kCharH / kAtlasH;
                    const float x0 = cx, x1 = cx + cw;
                    const float y0 = baseY, y1 = baseY + ch;
                    verts[vertCount++] = {{x0, y0}, {u0, v0}, {el.r, el.g, el.b, el.a}};
                    verts[vertCount++] = {{x1, y0}, {u1, v0}, {el.r, el.g, el.b, el.a}};
                    verts[vertCount++] = {{x1, y1}, {u1, v1}, {el.r, el.g, el.b, el.a}};
                    verts[vertCount++] = {{x0, y0}, {u0, v0}, {el.r, el.g, el.b, el.a}};
                    verts[vertCount++] = {{x1, y1}, {u1, v1}, {el.r, el.g, el.b, el.a}};
                    verts[vertCount++] = {{x0, y1}, {u0, v1}, {el.r, el.g, el.b, el.a}};
                    cx += cw;
                }
            }
            break;
        }
        case HudElement::Type::Rect: {
            if (vertCount + 6 > kTotalBudget)
                break;
            const float x0 = el.x * vpW, y0 = el.y * vpH;
            const float x1 = el.x2 * vpW, y1 = el.y2 * vpH;
            verts[vertCount++] = {{x0, y0}, {kWhiteU, kWhiteV}, {el.r, el.g, el.b, el.a}};
            verts[vertCount++] = {{x1, y0}, {kWhiteU, kWhiteV}, {el.r, el.g, el.b, el.a}};
            verts[vertCount++] = {{x1, y1}, {kWhiteU, kWhiteV}, {el.r, el.g, el.b, el.a}};
            verts[vertCount++] = {{x0, y0}, {kWhiteU, kWhiteV}, {el.r, el.g, el.b, el.a}};
            verts[vertCount++] = {{x1, y1}, {kWhiteU, kWhiteV}, {el.r, el.g, el.b, el.a}};
            verts[vertCount++] = {{x0, y1}, {kWhiteU, kWhiteV}, {el.r, el.g, el.b, el.a}};
            break;
        }
        case HudElement::Type::Line: {
            if (vertCount + 6 > kTotalBudget)
                break;
            const float sx = el.x * vpW, sy = el.y * vpH;
            const float ex = el.x2 * vpW, ey = el.y2 * vpH;
            const float dx = ex - sx, dy = ey - sy;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-4f)
                break; // degenerate line — skip to avoid NaN
            const float nx = -dy / len * el.strokeWidth * 0.5f;
            const float ny = dx / len * el.strokeWidth * 0.5f;
            // Four corners of the thick line quad
            float ax = sx + nx, ay = sy + ny;
            float bx = sx - nx, by = sy - ny;
            float cx2 = ex + nx, cy2 = ey + ny;
            float dx2 = ex - nx, dy2 = ey - ny;
            verts[vertCount++] = {{ax, ay}, {kWhiteU, kWhiteV}, {el.r, el.g, el.b, el.a}};
            verts[vertCount++] = {{cx2, cy2}, {kWhiteU, kWhiteV}, {el.r, el.g, el.b, el.a}};
            verts[vertCount++] = {{dx2, dy2}, {kWhiteU, kWhiteV}, {el.r, el.g, el.b, el.a}};
            verts[vertCount++] = {{ax, ay}, {kWhiteU, kWhiteV}, {el.r, el.g, el.b, el.a}};
            verts[vertCount++] = {{dx2, dy2}, {kWhiteU, kWhiteV}, {el.r, el.g, el.b, el.a}};
            verts[vertCount++] = {{bx, by}, {kWhiteU, kWhiteV}, {el.r, el.g, el.b, el.a}};
            break;
        }
        }
    }
    m_hudElements = {}; // clear after consuming

    if (vertCount == 0)
        return;

    VkRenderingAttachmentInfo colorAtt{};
    colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAtt.imageView = m_swapchainImageViews[m_currentImageIndex];
    colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, m_swapchainExtent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colorAtt;

    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_overlayPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_overlayPipelineLayout, 0, 1, &m_overlayDs, 0,
                            nullptr);

    VkViewport vp{0.0f, 0.0f, static_cast<float>(m_swapchainExtent.width), static_cast<float>(m_swapchainExtent.height),
                  0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, m_swapchainExtent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const float screenSize[2] = {static_cast<float>(m_swapchainExtent.width),
                                 static_cast<float>(m_swapchainExtent.height)};
    vkCmdPushConstants(cmd, m_overlayPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(screenSize), screenSize);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_overlayVB, &offset);
    vkCmdDraw(cmd, vertCount, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void VkRenderer::destroyOverlayResources() {
    if (m_device == VK_NULL_HANDLE)
        return;
    if (m_overlayVBMapped && m_overlayVBMemory != VK_NULL_HANDLE)
        vkUnmapMemory(m_device, m_overlayVBMemory);
    if (m_overlayVB != VK_NULL_HANDLE)
        vkDestroyBuffer(m_device, m_overlayVB, nullptr);
    if (m_overlayVBMemory != VK_NULL_HANDLE)
        vkFreeMemory(m_device, m_overlayVBMemory, nullptr);
    if (m_overlayPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(m_device, m_overlayPipeline, nullptr);
    if (m_overlayPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(m_device, m_overlayPipelineLayout, nullptr);
    if (m_overlayDsPool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(m_device, m_overlayDsPool, nullptr);
    if (m_overlayDsLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(m_device, m_overlayDsLayout, nullptr);
    if (m_overlayFontSampler != VK_NULL_HANDLE)
        vkDestroySampler(m_device, m_overlayFontSampler, nullptr);
    if (m_fontImageView != VK_NULL_HANDLE)
        vkDestroyImageView(m_device, m_fontImageView, nullptr);
    if (m_fontImage != VK_NULL_HANDLE)
        vkDestroyImage(m_device, m_fontImage, nullptr);
    if (m_fontImageMemory != VK_NULL_HANDLE)
        vkFreeMemory(m_device, m_fontImageMemory, nullptr);

    m_overlayVBMapped = nullptr;
    m_overlayVB = VK_NULL_HANDLE;
    m_overlayVBMemory = VK_NULL_HANDLE;
    m_overlayPipeline = VK_NULL_HANDLE;
    m_overlayPipelineLayout = VK_NULL_HANDLE;
    m_overlayDsPool = VK_NULL_HANDLE;
    m_overlayDsLayout = VK_NULL_HANDLE;
    m_overlayFontSampler = VK_NULL_HANDLE;
    m_fontImageView = VK_NULL_HANDLE;
    m_fontImage = VK_NULL_HANDLE;
    m_fontImageMemory = VK_NULL_HANDLE;
    m_overlayReady = false;
}
