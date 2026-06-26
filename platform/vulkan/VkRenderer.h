// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Concrete backend header — not a HAL interface file. Platform-specific headers
// are permitted here. Consumers hold IRenderer* and never include this directly.
#include "IRenderer.h"
#include "VkResources.h"
#include <array>
#include <cstddef>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

struct SDL_Window;

namespace fl {

static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
static constexpr uint32_t kNumCascades = 4;
static constexpr uint32_t kShadowRes = 2048;

// Maximum live particles in the pool across all emitters.
static constexpr uint32_t kMaxParticles = 8192;
// Maximum new particles spawned per frame (ring-buffer overwrite past this).
static constexpr uint32_t kMaxSpawnPerFrame = 512;

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
    glm::vec4 fogParams{0.0f, 5000.0f, 12.0f, 0.0f}; // x=density, y=startDist(m), z=timeOfDay(h), w=unused
};

// Push constant block for the forward pass — must be ≤ 128 bytes.
struct ForwardPushConstants {
    glm::mat4 model{1.0f};           // 64 bytes
    glm::vec4 baseColorFactor{1.0f}; // 16 bytes
    float metallicFactor{0.0f};
    float roughnessFactor{1.0f};
    float shadingMode{0.0f}; // 0 = normal PBR albedo, 1 = terrain elevation/slope, 2 = debug face colour
    float _pad{};            // total = 96
};
static_assert(sizeof(ForwardPushConstants) <= 128);

// GPU shadow UBO — cascade view-proj matrices + cascade split distances + runtime cascade count.
// std140: mat4[4]=256, vec4=16, uint32+pad[3]=16 → total 288 bytes.
struct ShadowUBO {
    glm::mat4 lightViewProj[kNumCascades]; // 256 bytes
    glm::vec4 splitDepths;                 // x/y/z = VS end of cascades 0/1/2, w = shadow far
    uint32_t numCascades;                  // active cascade count (0 = shadows disabled)
    uint32_t _pad[3];                      // explicit pad to 16-byte struct alignment
};
static_assert(sizeof(ShadowUBO) == 288u);
static_assert(offsetof(ShadowUBO, numCascades) == 272u);

// Push constants for the depth-only shadow pass.
struct ShadowPushConstants {
    glm::mat4 model{1.0f};  // 64 bytes
    uint32_t cascadeIdx{0}; // 4 bytes
    float _pad[3]{};        // 12 bytes — total 80
};
static_assert(sizeof(ShadowPushConstants) <= 128);

// Sky UBO (set 0, binding 0 of the sky descriptor set) — replaces the former 128-byte
// SkyPushConstants so the sky pass can carry quality selection + LUT samplers (bindings 1/2).
struct SkyUBO {
    glm::mat4 invViewProj{1.0f};                    // 64 bytes
    glm::vec4 sunDirection{};                       // 16 bytes  xyz = dir toward sun
    glm::vec4 sunColor{};                           // 16 bytes  xyz = color, w = intensity
    glm::vec4 skyParams{0.40f, 0.55f, 0.75f, 0.0f}; // 16 bytes  xyz=horizonColor, w=cloudCoverage[0,1]
    glm::vec4 fogParams{0.0f, 5.0f, 12.0f, 0.0f};   // 16 bytes  x=density, y=startDist(km), z=timeOfDay(h), w=camAltKm
    uint32_t qualityMode{0};                        // 0 = procedural, 1 = LUT
    float _pad[3]{};                                // pad to 16-byte alignment → 144 bytes
};
static_assert(sizeof(SkyUBO) == 144u);

// Push constants for the tonemap + FXAA + bloom + AO composite pass.
struct TonemapPush {
    float texelSizeX{0.0f};    // 1 / width  — used by FXAA for neighbour sampling
    float texelSizeY{0.0f};    // 1 / height
    uint32_t enableFxaa{0};    // 1 = apply FXAA on tonemapped output
    float bloomStrength{0.0f}; // bloom blend multiplier (0 = disabled)
    float aoStrength{0.0f};    // GTAO darkening strength (0 = AO disabled)
    float _pad[3]{};           // 16-byte alignment → 32 bytes
};
static_assert(sizeof(TonemapPush) == 32);
static_assert(sizeof(TonemapPush) <= 128);

// Push constants for the GTAO compute pass (full-resolution, horizon-based, world space).
struct GtaoPush {
    glm::mat4 invViewProj{1.0f}; // 64 — clip → world-space reconstruction
    glm::vec4 params{0.0f};      // x=radius(m), y=strength, z=frameIndex, w=intensityPow
    glm::vec4 texel{0.0f};       // x=1/width, y=1/height, z=width, w=height
};
static_assert(sizeof(GtaoPush) <= 128);

// Push constants for the bloom blur passes (shared by H and V).
struct BloomPush {
    float texelSizeX{0.0f};
    float texelSizeY{0.0f};
    uint32_t isVertical{0}; // 0 = horizontal blur, 1 = vertical blur
    float _pad{0.0f};
};
static_assert(sizeof(BloomPush) == 16);
static_assert(sizeof(BloomPush) <= 128);

// GPU particle state — must exactly match the Particle struct in particle_sim.comp
// and particle.vert (std430 layout: vec3+float pairs pack without padding).
struct GpuParticle {
    glm::vec3 pos;
    float age; // age <= 0 = inactive slot
    glm::vec3 vel;
    float maxAge;
    glm::vec4 colorStart;
    glm::vec4 colorEnd;
    float sizeStart;
    float sizeEnd;
    float additive;
    float _pad;
};
static_assert(sizeof(GpuParticle) == 80);

// Push constants for the particle compute pass.
struct ParticleSimPush {
    float dt;
    uint32_t count;
    float gravity;
    float _pad;
};
static_assert(sizeof(ParticleSimPush) <= 128);

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

    // ── Settings ───────────────────────────────────────────────────────────
    void applySettings(const RendererSettings& settings) override;

    // ── Per-frame stats ────────────────────────────────────────────────────
    FrameStats getFrameStats() const override;

    // ── Debug overlay + 2D overlays ───────────────────────────────────────
    void setOverlayLines(std::span<const std::string_view> lines) override;
    void submitOverlayElements(std::span<const HudElement> elements) override;
    void setConsoleElements(std::span<const HudElement> elements) override;

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
    bool createNormalImage();
    bool createHdrSampler();
    void destroyAttachments();

    // ── GTAO ───────────────────────────────────────────────────────────────
    bool createAoImage();         // full-res AO storage image (recreated with swapchain)
    bool createGtaoResources();   // sampler, descriptor layout/pool/set, compute pipeline
    void updateGtaoDescriptors(); // (re)bind depth/normal/AO views after a swapchain resize
    void destroyGtaoResources();  // permanent GTAO objects (pipeline/layout/pool/sampler)
    void recordGtao(VkCommandBuffer cmd);

    bool createAttachmentImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                               VkImageAspectFlags aspect, VkImage& image, VkDeviceMemory& memory, VkImageView& view);
    static uint32_t findMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags props);

    // ── Tonemap descriptor ─────────────────────────────────────────────────
    bool createTonemapDescriptors();
    void updateHdrDescriptor();

    // ── Per-frame UBO descriptors ──────────────────────────────────────────
    bool createPerFrameDescriptorLayout();
    bool createShadowDescriptorLayout();
    bool createMaterialDescriptorLayout();
    bool createPerFrameDescriptors();

    // Write camera + light + shadow UBO data for the current frame.
    void writeFrameUBOs(const FrameScene& scene);

    // Compute cascade split distances and light view-proj matrices.
    void computeCascades(const FrameScene& scene, ShadowUBO& out);

    // ── Shadow resources ───────────────────────────────────────────────────
    bool createShadowResources();
    void destroyShadowResources();
    void recreateShadowResources();
    void recreateParticleResources();

    // ── Pipelines ──────────────────────────────────────────────────────────
    bool createPipelineCache();
    bool createForwardPipeline();
    bool createTonemapPipeline();
    bool createShadowPipeline();
    bool createSkyPipeline();

    // ── Sky descriptor (set 0: SkyUBO) ─────────────────────────────────────
    bool createSkyDescriptorLayout();
    bool createSkyDescriptors(); // per-frame SkyUBO buffers + descriptor sets
    void destroySkyResources();

    // ── Commands + sync ────────────────────────────────────────────────────
    bool createCommandPool();
    bool allocateCommandBuffers();
    bool createSyncObjects();

    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex);

    // ── Overlay pipeline ──────────────────────────────────────────────────
    bool createOverlayPipeline();
    void recordOverlayPass(VkCommandBuffer cmd);
    void destroyOverlayResources();

    // ── Particle system ────────────────────────────────────────────────────
    bool createParticleResources();
    void destroyParticleResources();
    bool createParticleComputePipeline();
    bool createParticleRenderPipelines();
    void recordParticleCompute(VkCommandBuffer cmd, float dt);
    void recordParticleDraw(VkCommandBuffer cmd);

    // ── Transparent pass ───────────────────────────────────────────────────
    bool createForwardAlphaPipeline();

    // ── Bloom ──────────────────────────────────────────────────────────────
    bool createBloomImages();
    bool createBloomDescriptors();
    void updateBloomDescriptors();
    bool createBloomPipelines();
    void destroyBloomResources();
    void recordBloomPasses(VkCommandBuffer cmd);

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

    // ── G-buffer world-space normal attachment (octahedral-encoded, RGBA16F) ──
    // Second colour attachment of the forward-opaque pass; consumed by GTAO (and future
    // screen-space effects). Sampled via m_hdrSampler. Recreated with the swapchain.
    VkImage m_normalImage{VK_NULL_HANDLE};
    VkDeviceMemory m_normalMemory{VK_NULL_HANDLE};
    VkImageView m_normalView{VK_NULL_HANDLE};

    // ── GTAO (full-resolution horizon-based AO, RGBA16F; AO in .r) ──────────
    // Single compute pass after the forward-opaque pass; sampled by the tonemap pass. Half-res +
    // bilateral upsample and motion-vector temporal accumulation are a perf follow-on.
    VkImage m_aoImage{VK_NULL_HANDLE};
    VkDeviceMemory m_aoMemory{VK_NULL_HANDLE};
    VkImageView m_aoView{VK_NULL_HANDLE};
    VkSampler m_gtaoSampler{VK_NULL_HANDLE}; // nearest clamp for depth/normal/AO reads
    VkDescriptorSetLayout m_gtaoSetLayout{VK_NULL_HANDLE};
    VkPipelineLayout m_gtaoPipeLayout{VK_NULL_HANDLE};
    VkPipeline m_gtaoPipeline{VK_NULL_HANDLE};
    VkDescriptorPool m_gtaoPool{VK_NULL_HANDLE};
    VkDescriptorSet m_gtaoSet{VK_NULL_HANDLE};
    uint32_t m_gtaoFrame{0}; // rotates per-pixel noise

    // ── Tonemap descriptor ────────────────────────────────────────────────
    VkDescriptorSetLayout m_tonemapSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_tonemapPool{VK_NULL_HANDLE};
    VkDescriptorSet m_tonemapSet{VK_NULL_HANDLE};

    // ── Shadow map (2D array: kNumCascades layers × kShadowRes²) ─────────
    VkImage m_shadowImage{VK_NULL_HANDLE};
    VkDeviceMemory m_shadowMemory{VK_NULL_HANDLE};
    VkImageView m_shadowArrayView{VK_NULL_HANDLE};              // for sampling in forward pass
    std::array<VkImageView, kNumCascades> m_shadowLayerViews{}; // per-cascade for rendering
    VkSampler m_shadowSampler{VK_NULL_HANDLE};                  // PCF comparison sampler

    // ── Shadow pipeline descriptor set ────────────────────────────────────
    VkDescriptorSetLayout m_shadowSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_shadowPool{VK_NULL_HANDLE};

    // ── Per-frame descriptor set layout (set 0: camera + light + shadow UBOs + shadow map) ─
    VkDescriptorSetLayout m_perFrameSetLayout{VK_NULL_HANDLE};

    // ── Per-material descriptor set layout (set 1: base color + normal + ORM) ──
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

        VkBuffer shadowBuffer{VK_NULL_HANDLE}; // ShadowUBO — shared by both pipelines
        VkDeviceMemory shadowMemory{VK_NULL_HANDLE};
        void* shadowMapped{nullptr};

        VkBuffer skyBuffer{VK_NULL_HANDLE}; // SkyUBO (sky pass set 0, binding 0)
        VkDeviceMemory skyMemory{VK_NULL_HANDLE};
        void* skyMapped{nullptr};

        VkDescriptorSet descriptorSet{VK_NULL_HANDLE};       // forward pass set 0
        VkDescriptorSet shadowDescriptorSet{VK_NULL_HANDLE}; // shadow pipeline set 0
        VkDescriptorSet skyDescriptorSet{VK_NULL_HANDLE};    // sky pass set 0 (UBO + 2 LUTs)
    };
    std::array<PerFrameData, MAX_FRAMES_IN_FLIGHT> m_perFrame{};
    VkDescriptorPool m_perFramePool{VK_NULL_HANDLE};

    // ── Settings ──────────────────────────────────────────────────────────
    RendererSettings m_settings{};

    // Runtime shadow / particle parameters (derived from m_settings at init and on applySettings).
    uint32_t m_shadowRes{kShadowRes};
    uint32_t m_numCascades{kNumCascades};
    uint32_t m_maxParticles{kMaxParticles};
    uint32_t m_maxSpawnPerFrame{kMaxSpawnPerFrame};
    bool m_shadowDirty{false};
    bool m_particlesDirty{false};

    // ── Pipelines ─────────────────────────────────────────────────────────
    VkPipelineCache m_pipelineCache{VK_NULL_HANDLE};
    VkPipelineLayout m_forwardLayout{VK_NULL_HANDLE};
    VkPipeline m_forwardPipeline{VK_NULL_HANDLE};
    VkPipeline m_forwardAlphaPipeline{VK_NULL_HANDLE}; // alpha-blended transparent pass
    VkPipelineLayout m_tonemapLayout{VK_NULL_HANDLE};
    VkPipeline m_tonemapPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_shadowLayout{VK_NULL_HANDLE};
    VkPipeline m_shadowPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_skyLayout{VK_NULL_HANDLE};
    VkPipeline m_skyPipeline{VK_NULL_HANDLE};

    // ── Sky descriptor set (set 0, binding 0 = SkyUBO) ─────────────────────
    // Carries the sky parameters (including the procedural/atmospheric quality selector) that
    // formerly lived in push constants. The atmospheric (LUT) model is evaluated analytically in
    // sky.frag for now; precomputed transmittance/multi-scatter LUT textures bind here in a
    // follow-on (the descriptor set + UBO plumbing is already in place for it).
    VkDescriptorSetLayout m_skySetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_skyPool{VK_NULL_HANDLE};

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
    uint32_t m_framesRendered{0}; // guards timestamp query reads until queries are first written
    uint32_t m_currentImageIndex{0};
    uint64_t m_totalFrames{0};
    bool m_framebufferResized{false};
    bool m_frameAcquired{false};
    uint64_t m_lastFrameNs{0};     // nanosecond timestamp of previous beginFrame
    float m_frameDt{1.0f / 60.0f}; // wall-clock seconds since last frame (capped)

    // Scene submitted this frame (set by setScene, consumed by recordCommandBuffer).
    FrameScene m_pendingScene{};

    // ── Particle GPU resources ────────────────────────────────────────────
    // Persistent device-local SSBO holding all particle slots (kMaxParticles).
    VkBuffer m_particlePoolBuf{VK_NULL_HANDLE};
    VkDeviceMemory m_particlePoolMemory{VK_NULL_HANDLE};

    // Per-frame host-visible staging buffer for new particles (CPU→GPU ring-buffer spawn).
    struct ParticleSpawnFrame {
        VkBuffer buf{VK_NULL_HANDLE};
        VkDeviceMemory mem{VK_NULL_HANDLE};
        void* mapped{nullptr};
    };
    std::array<ParticleSpawnFrame, MAX_FRAMES_IN_FLIGHT> m_particleSpawn{};

    // Compute pipeline (particle_sim.comp): integrates pos/vel/age each frame.
    VkDescriptorSetLayout m_particleComputeSetLayout{VK_NULL_HANDLE};
    VkPipelineLayout m_particleComputeLayout{VK_NULL_HANDLE};
    VkPipeline m_particleComputePipeline{VK_NULL_HANDLE};
    VkDescriptorPool m_particleComputePool{VK_NULL_HANDLE};
    VkDescriptorSet m_particleComputeSet{VK_NULL_HANDLE};

    // Render pipeline (particle.vert/frag): instanced camera-facing billboards.
    // Combined set 0: [0]=camera UBO (per-frame), [1]=particle SSBO (read-only).
    VkDescriptorSetLayout m_particleRenderSetLayout{VK_NULL_HANDLE};
    VkPipelineLayout m_particleRenderLayout{VK_NULL_HANDLE};
    VkPipeline m_particleAdditPipeline{VK_NULL_HANDLE}; // additive blend
    VkPipeline m_particleAlphaPipeline{VK_NULL_HANDLE}; // alpha blend
    VkDescriptorPool m_particleRenderPool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_particleRenderSets{};

    // Ring-buffer spawn pointer (CPU-maintained; wraps around kMaxParticles).
    uint32_t m_nextParticleSlot{0};

    // Fractional particle spawn remainder per emitter (indexed by position in particleEmitters).
    // Reset to zero when the emitter list count changes between frames.
    std::vector<float> m_spawnAccum;

    // ── Bloom attachments (half-res RGBA16F — recreated with swapchain) ───
    VkImage m_bloomImage{VK_NULL_HANDLE};
    VkDeviceMemory m_bloomMemory{VK_NULL_HANDLE};
    VkImageView m_bloomView{VK_NULL_HANDLE};
    VkImage m_bloomAuxImage{VK_NULL_HANDLE};
    VkDeviceMemory m_bloomAuxMemory{VK_NULL_HANDLE};
    VkImageView m_bloomAuxView{VK_NULL_HANDLE};

    // ── Bloom descriptors (shared set layout: 1 combined sampler, binding 0) ─
    VkDescriptorSetLayout m_bloomSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_bloomPool{VK_NULL_HANDLE};
    VkDescriptorSet m_bloomThresholdSet{VK_NULL_HANDLE}; // input = HDR
    VkDescriptorSet m_bloomBlurHSet{VK_NULL_HANDLE};     // input = bloom → writes aux
    VkDescriptorSet m_bloomBlurVSet{VK_NULL_HANDLE};     // input = aux  → writes bloom

    // ── Bloom pipelines (tonemap.vert + bloom_threshold/blur.frag) ────────
    VkPipelineLayout m_bloomLayout{VK_NULL_HANDLE};
    VkPipeline m_bloomThresholdPipeline{VK_NULL_HANDLE};
    VkPipeline m_bloomBlurPipeline{VK_NULL_HANDLE};

    // ── GPU resource manager ──────────────────────────────────────────────
    VkResourceManager m_resources;

    SDL_Window* m_sdlWindow{nullptr};
    IWindow* m_iWindow{nullptr};
    std::string m_shaderDir;
    mutable std::string m_lastError;
    std::string m_gpuInfo;

    // ── Per-frame stats ───────────────────────────────────────────────────
    FrameStats m_frameStats{};
    uint32_t m_drawCallCount{0}; // incremented by each vkCmdDraw/vkCmdDrawIndexed

    // ── Timestamp query pool (optional — skipped when timestampValidBits == 0) ─
    VkQueryPool m_timestampPool{VK_NULL_HANDLE};
    float m_timestampPeriod{1.0f}; // nanoseconds per timestamp tick
    bool m_timestampSupported{false};

    // ── Debug overlay + 2D overlays ───────────────────────────────────────
    std::vector<std::string_view> m_overlayLines;  // set by setOverlayLines(), valid until endFrame
    std::vector<HudElement> m_overlayElements;     // accumulated by submitOverlayElements(), cleared by endFrame
    std::span<const HudElement> m_consoleElements; // set by setConsoleElements(), non-owning, cleared by endFrame
    bool m_overlayReady{false};                    // true once createOverlayPipeline() succeeds

    VkDescriptorSetLayout m_overlayDsLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_overlayDsPool{VK_NULL_HANDLE};
    VkDescriptorSet m_overlayDs{VK_NULL_HANDLE};
    VkPipelineLayout m_overlayPipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_overlayPipeline{VK_NULL_HANDLE};
    VkSampler m_overlayFontSampler{VK_NULL_HANDLE};
    // Font image managed directly (R8_UNORM raw pixels — not KTX2/PNG, so bypass resource manager).
    VkImage m_fontImage{VK_NULL_HANDLE};
    VkDeviceMemory m_fontImageMemory{VK_NULL_HANDLE};
    VkImageView m_fontImageView{VK_NULL_HANDLE};

    // Host-visible vertex buffer shared by overlay text and 2D HUD elements (rebuilt each frame).
    static constexpr uint32_t kMaxOverlayChars = 1024; // debug text characters
    static constexpr uint32_t kMaxHudVerts = 4096;     // 2D HUD line/rect/text vertices
    VkBuffer m_overlayVB{VK_NULL_HANDLE};
    VkDeviceMemory m_overlayVBMemory{VK_NULL_HANDLE};
    void* m_overlayVBMapped{nullptr};
};

} // namespace fl
