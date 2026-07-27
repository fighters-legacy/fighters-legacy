// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Concrete backend header — not a HAL interface file. Platform-specific headers
// are permitted here. Consumers hold IRenderer* and never include this directly.
#include "IRenderer.h"
#include "VkResources.h"
#include <array>
#include <cstddef>
#include <span>
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
    glm::vec4 worldOrigin{0.0f};  // xyz = origin, w = unused
    glm::vec4 planetCenter{0.0f}; // xyz = planet centre, camera-relative (#475); w = unused
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
    float shadingMode{0.0f}; // 0 = PBR, 1 = terrain elev/slope, 2 = debug face colour, 3 = runway markings (#487)
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
    glm::vec4 moonDirection{0.0f, 1.0f, 0.0f, 0.0045f}; // 16 bytes  xyz = dir toward Moon, w = angular radius (#484)
    glm::vec4 moonParams{1.0f, 0.0f, 0.0f, 0.0f};       // 16 bytes  x=illumination, y=nightFactor, z=celestialValid
    glm::mat4 worldToCelestial{1.0f};                   // 64 bytes  mat3 (padded) rotating a world ray -> star frame
    uint32_t qualityMode{0};                            // 0 = procedural, 1 = LUT
    float _pad[3]{};                                    // pad to 16-byte alignment → 240 bytes
};
static_assert(sizeof(SkyUBO) == 240u);

// Push constants for the tonemap + FXAA + bloom + AO composite pass.
struct TonemapPush {
    float texelSizeX{0.0f};    // 1 / width  — used by FXAA for neighbour sampling
    float texelSizeY{0.0f};    // 1 / height
    uint32_t enableFxaa{0};    // 1 = apply FXAA on tonemapped output
    float bloomStrength{0.0f}; // bloom blend multiplier (0 = disabled)
    float aoStrength{0.0f};    // GTAO darkening strength (0 = AO disabled)
    float nvgIntensity{0.0f};  // night-vision green tint/gain (#210); 0 = off (consumes a pad slot)
    float exposureScale{1.0f}; // exp2(evOffset) from photo mode (#41); 1 = no change (a pad slot)
    float _pad[1]{};           // 16-byte alignment → 32 bytes
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
    bool initHeadless(uint32_t width, uint32_t height) override;
    void onResize(int width, int height) override;
    void beginFrame() override;
    void endFrame() override;
    void shutdown() override;
    const char* getLastError() const override;
    const char* gpuInfo() const override;

    // ── Resource methods ───────────────────────────────────────────────────
    MeshHandle createMesh(const MeshUploadDesc& desc) override;
    TextureHandle createTexture(const TextureUploadDesc& desc) override;
    TextureHandle createTextureArray(const TextureUploadDesc& desc) override;
    void setTerrainBiomeTextures(TextureHandle colorArray, TextureHandle normalOrmArray, uint32_t layerCount) override;
    MaterialHandle createMaterial(const MaterialDesc& desc) override;
    MaterialHandle getMeshMaterial(MeshHandle h) const override;
    bool getMeshBounds(MeshHandle h, glm::vec3& outMin, glm::vec3& outMax) const override;
    bool supportsWireframe() const override {
        return m_wireframeSupported && m_forwardWirePipeline != VK_NULL_HANDLE;
    }
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
    bool captureScreenshot(const char* path) override;
    bool setCaptureSink(std::function<void(const CaptureFrame&)> sink) override;
    void setNightVision(float intensity) override {
        m_nvgIntensity = intensity < 0.0f ? 0.0f : (intensity > 1.0f ? 1.0f : intensity); // #210
    }

    // ── Dear ImGui backend bridge (#156) ──────────────────────────────────────
    bool initGuiRenderBackend() override;
    void shutdownGuiRenderBackend() override;

  private:
    // ── Core Vulkan objects ────────────────────────────────────────────────
    bool createInstance();
    bool setupDebugMessenger();
    bool createSurface();
    bool pickPhysicalDevice();
    bool createLogicalDevice();

    // Shared init tail (props → device → swapchain/present-targets → pipelines → sync) used by both
    // init() and initHeadless() (#913). The two front paths differ only in instance extensions, surface
    // creation, and physical-device selection; everything from here is identical.
    bool finishInit(uint32_t width, uint32_t height);

    // ── Swapchain ──────────────────────────────────────────────────────────
    bool createSwapchain(int width, int height);
    // Headless present targets (#913): MAX_FRAMES_IN_FLIGHT owned images (R8G8B8A8_UNORM,
    // COLOR_ATTACHMENT | TRANSFER_SRC) the tonemap pass renders into and the capture sink reads back;
    // populates m_swapchainImages/Views/Format/Extent so the rest of the renderer is unchanged.
    bool createPresentTargets(uint32_t width, uint32_t height);
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
    bool createTerrainBiomeResources(); // set 2: biome basecolor + normalORM arrays (#446)
    void writeTerrainBiomeSet();        // (re)writes m_terrainBiomeSet from the current biome handles
    void savePipelineCache();           // #446 rider: persist the pipeline cache to the pref dir
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

    // Replay the opaque RenderItem draw loop (shadow-only items skipped) with the forward pipeline
    // already bound. `set0` is the per-frame set 0 (main camera, or the inset camera for the #695
    // secondary-viewport pass). Shared by the main forward-opaque pass and the inset pass so the
    // per-item bind/push/draw logic exists once.
    void drawOpaqueItems(VkCommandBuffer cmd, VkDescriptorSet set0);

    // ── Node-aware submesh draws (#839) ───────────────────────────────────
    // One resolved draw: an index-buffer slice, the model matrix its glTF node composes to, and the
    // per-primitive material. Every draw loop (shadow, forward-opaque, transparent, inset) walks the
    // same list, so per-node articulation, per-primitive materials and `_b` damage selection exist
    // once rather than four times.
    struct ResolvedDraw {
        uint32_t firstIndex{0};
        uint32_t indexCount{0};
        glm::mat4 model{1.0f};
        MaterialHandle material{};
    };
    // Fills m_drawScratch with this item's draws. Returns a view of it; valid until the next call.
    // A single-submesh mesh at an identity node produces exactly one draw with model == item.transform
    // (the pre-#839 output, bit-for-bit).
    std::span<const ResolvedDraw> resolveItemDraws(const RenderItem& item, const GpuMesh& mesh);

    std::vector<ResolvedDraw> m_drawScratch;   // reused per item; avoids per-draw allocation
    std::vector<glm::mat4> m_nodeXformScratch; // per-node composed global transforms (content frame)

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

    // ── Swapchain (or headless present targets, #913) ─────────────────────
    VkSwapchainKHR m_swapchain{VK_NULL_HANDLE}; // VK_NULL_HANDLE when headless
    std::vector<VkImage> m_swapchainImages;     // swapchain images, or owned headless targets
    std::vector<VkImageView> m_swapchainImageViews;
    std::vector<VkDeviceMemory> m_presentTargetMemory; // backing memory for owned headless targets only
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
    // Terrain biome arrays (#446): set 2 of the forward layout — two sampler2DArrays (basecolor,
    // normalORM). Bound for every forward draw; only the terrain path in mesh.frag samples it. A
    // 4-layer dummy keeps the set valid before real biome textures upload (headless never uploads).
    VkDescriptorSetLayout m_terrainBiomeSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_terrainBiomePool{VK_NULL_HANDLE};
    VkDescriptorSet m_terrainBiomeSet{VK_NULL_HANDLE};
    TextureHandle m_biomeColorTex{};
    TextureHandle m_biomeNormalOrmTex{};
    TextureHandle m_biomeDummyColor{};
    TextureHandle m_biomeDummyNormalOrm{};

    // ── Per-frame UBO buffers + descriptor sets ───────────────────────────
    // Uses raw Vulkan memory (not VMA): small host-visible buffers that change
    // every frame don't benefit from VMA sub-allocation.
    struct PerFrameData {
        VkBuffer cameraBuffer{VK_NULL_HANDLE};
        VkDeviceMemory cameraMemory{VK_NULL_HANDLE};
        void* cameraMapped{nullptr};

        // Secondary-camera inset (#695): a second CameraUBO + set-0 descriptor bound to the SAME
        // per-frame layout as descriptorSet. Only binding 0 (camera) differs — light/shadow UBOs and
        // the shadow map are shared with the main set. Written + drawn only when scene.insetEnabled.
        VkBuffer insetCameraBuffer{VK_NULL_HANDLE};
        VkDeviceMemory insetCameraMemory{VK_NULL_HANDLE};
        void* insetCameraMapped{nullptr};
        VkDescriptorSet insetDescriptorSet{VK_NULL_HANDLE};

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
    float m_nvgIntensity{0.0f}; // #210 night-vision goggles gain (0 = off); set via setNightVision()

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
    VkPipeline m_forwardWirePipeline{VK_NULL_HANDLE};  // LINE-polygon variant for the wireframe view (#838)
    VkPipeline m_forwardAlphaPipeline{VK_NULL_HANDLE}; // alpha-blended transparent pass
    bool m_wireframeSupported{false};                  // device has fillModeNonSolid (#838)
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
    bool m_headless{false}; // #913: swapchain-free init (no window, no present); set by initHeadless()
    std::string m_shaderDir;
    mutable std::string m_lastError;
    std::string m_pendingScreenshotPath;             // #909: capture the next presented frame to this PNG when set
    void writeSwapchainPng(const std::string& path); // reads the just-presented swapchain image → PNG
    // Per-frame capture sink (#912): when set, every rendered frame's RGBA pixels are delivered here at
    // the end of endFrame(). Reused headless (#913). Synchronous readback — correctness over the
    // zero-stall ring, since the recorder runs offline at a reduced time-rate (see VkRenderer.cpp).
    std::function<void(const CaptureFrame&)> m_captureSink;
    std::vector<uint8_t> m_captureBuf; // reused RGBA scratch for the sink (avoids per-frame realloc)
    // Copy `srcImage` (currently in `srcLayout`) into a host buffer and swizzle to tightly packed RGBA8
    // (opaque alpha) in `outRgba`; restores the image to `restoreLayout`. Returns false on failure.
    bool readbackImageRgba(VkImage srcImage, VkImageLayout srcLayout, VkImageLayout restoreLayout,
                           std::vector<uint8_t>& outRgba, uint32_t& outW, uint32_t& outH);
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

    // ── Dear ImGui backend (#156) ──────────────────────────────────────────
    // A dedicated descriptor pool for ImGui's font + user textures. m_guiEnabled gates the per-frame
    // draw-data recording (in recordCommandBuffer, after the overlay pass) and the teardown; it is only
    // ever set true by initGuiRenderBackend() when the IGui backend attaches. Windowed path only.
    VkDescriptorPool m_imguiPool{VK_NULL_HANDLE};
    bool m_guiEnabled{false};

    // Host-visible vertex buffer shared by overlay text and 2D HUD elements (rebuilt each frame).
    static constexpr uint32_t kMaxOverlayChars = 1024; // debug text characters
    static constexpr uint32_t kMaxHudVerts = 4096;     // 2D HUD line/rect/text vertices
    VkBuffer m_overlayVB{VK_NULL_HANDLE};
    VkDeviceMemory m_overlayVBMemory{VK_NULL_HANDLE};
    void* m_overlayVBMapped{nullptr};
};

} // namespace fl
