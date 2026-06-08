// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Renderer settings — platform-agnostic subset of GraphicsSettings used by
// IRenderer::applySettings().  Populated from engine/config/GraphicsSettings.h
// in main.cpp so that platform/ headers remain free of engine/ dependencies.
// ---------------------------------------------------------------------------

enum class RendererVsyncMode : uint8_t {
    Off,      // prefer IMMEDIATE, fallback MAILBOX, fallback FIFO
    On,       // always FIFO (guaranteed vsync)
    Adaptive, // prefer FIFO_RELAXED, fallback FIFO
};

struct RendererSettings {
    RendererVsyncMode vsync{RendererVsyncMode::On};
    bool antiAliasing{true};     // FXAA on/off
    bool bloom{true};            // bloom on/off
    float drawDistanceKm{50.0f}; // entity cull distance in km (used by SceneRenderer)
};

// Per-frame GPU and CPU timing statistics. Populated by IRenderer::getFrameStats()
// after endFrame() returns. Used by PerformanceOverlay and CI regression detection.
struct FrameStats {
    float frameDtMs{0.0f};         // wall-clock frame duration (beginFrame to beginFrame)
    float gpuDtMs{0.0f};           // GPU command buffer time from timestamp queries; 0 if unsupported
    uint32_t drawCalls{0};         // opaque + transparent + overlay draw calls this frame
    uint64_t gpuMemUsedBytes{0};   // device-local heap bytes in use (VMA budget query)
    uint64_t gpuMemBudgetBytes{0}; // device-local heap budget (VMA budget query)
};

// ---------------------------------------------------------------------------
// Resource upload descriptors.
//
// These are byte-blob views into data produced by IContentPack (engine/content/
// AssetTypes.h). Using span+string_view here keeps platform/ free of engine
// header dependencies while preserving zero-copy semantics.
// ---------------------------------------------------------------------------

// Raw glTF 2.0 (.glb) or .gltf+.bin mesh bytes.
// The renderer parses the first primitive of the first mesh node.
struct MeshUploadDesc {
    std::string_view name;          // asset name for debug labels / dedup
    std::span<const uint8_t> bytes; // .glb file contents
};

// Raw texture bytes: KTX2 (Basis Universal) preferred; PNG accepted as fallback.
struct TextureUploadDesc {
    std::string_view name;
    std::span<const uint8_t> bytes;
    bool srgb{true}; // true=color (sRGB view), false=linear (normal/ORM)
};

// ---------------------------------------------------------------------------
// Opaque typed GPU-resource handles.  id == 0 is null/invalid.
// ---------------------------------------------------------------------------

struct MeshHandle {
    uint32_t id{0};
    [[nodiscard]] bool valid() const noexcept {
        return id != 0;
    }
};
struct TextureHandle {
    uint32_t id{0};
    [[nodiscard]] bool valid() const noexcept {
        return id != 0;
    }
};
struct MaterialHandle {
    uint32_t id{0};
    [[nodiscard]] bool valid() const noexcept {
        return id != 0;
    }
};

// ---------------------------------------------------------------------------
// PBR metallic-roughness material description.
// Texture handles must be created before createMaterial is called;
// an invalid handle means use the default (white texture / flat factor).
// ---------------------------------------------------------------------------
struct MaterialDesc {
    TextureHandle baseColorTexture{};
    TextureHandle normalTexture{};
    TextureHandle ormTexture{}; // R=occlusion G=roughness B=metallic
    glm::vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallicFactor{0.0f};
    float roughnessFactor{1.0f};
    bool doubleSided{false};
    bool alphaBlend{false};
};

// ---------------------------------------------------------------------------
// Camera
//
// worldOrigin is the camera position in world space. The view matrix is built
// camera-relative (world rebased to worldOrigin), so large world coordinates
// remain float32-safe at arbitrary theater scale.
// ---------------------------------------------------------------------------
struct CameraView {
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::dvec3 worldOrigin{};
};

// ---------------------------------------------------------------------------
// Per-node rigid pose for named glTF animations.
// No joint skinning — nodes are transformed independently.
// ---------------------------------------------------------------------------
struct NodePose {
    uint32_t nodeIndex{0};
    glm::mat4 localTransform{1.0f};
};

// ---------------------------------------------------------------------------
// Render flags
// ---------------------------------------------------------------------------
static constexpr uint32_t kRenderFlagDamaged = 1u << 0;    // use _b damage-variant nodes
static constexpr uint32_t kRenderFlagShadowOnly = 1u << 1; // depth pass only

// ---------------------------------------------------------------------------
// A single draw call submitted to the renderer each frame.
// transform is in world space; the bridge rebases it to camera-relative before upload.
// ---------------------------------------------------------------------------
struct RenderItem {
    MeshHandle mesh{};
    MaterialHandle material{};
    glm::mat4 transform{1.0f};
    uint32_t lod{0};
    uint32_t flags{0};
    std::span<const NodePose> animPoses{};
};

// ---------------------------------------------------------------------------
// Lighting and atmospheric parameters for one frame.
// ---------------------------------------------------------------------------
struct EnvironmentState {
    glm::vec3 sunDirection{0.0f, -1.0f, 0.0f}; // world-space, points toward sun
    glm::vec3 sunColor{1.0f, 0.95f, 0.8f};
    glm::vec3 ambientColor{0.1f, 0.12f, 0.15f};
    float fogDensity{0.0f};
    float fogStartDist{5000.0f};
    float timeOfDay{12.0f};    // hours [0, 24)
    float cloudCoverage{0.0f}; // [0=clear .. 1=full storm cover]; driven by WeatherController
};

// ---------------------------------------------------------------------------
// Particle emitter state for one frame.
// effectName points to a static/constant string (preset name); nullptr = inactive.
// All remaining fields are filled by ParticleSystem::emit() from the registered preset.
// ---------------------------------------------------------------------------
struct ParticleEmitterState {
    glm::vec3 position{};
    const char* effectName{nullptr}; // nullptr = inactive
    float intensity{1.0f};           // multiplier on spawnRate
    float spawnRate{50.0f};          // particles per second at intensity=1
    float particleLifetime{2.0f};    // seconds
    float initialSpeed{5.0f};        // m/s, randomised over hemisphere centred on emitDirection
    glm::vec3 colorStart{1.0f, 0.5f, 0.1f};
    glm::vec3 colorEnd{0.3f, 0.3f, 0.3f};
    float sizeStart{0.5f};                     // world-space metres at birth
    float sizeEnd{2.0f};                       // world-space metres at death
    bool additive{true};                       // true=additive blend (fire/explosion), false=alpha (smoke)
    glm::vec3 emitDirection{0.0f, 1.0f, 0.0f}; // normalised; hemisphere centred on this axis
};

// ---------------------------------------------------------------------------
// Subtitle overlay — data model only; rendering deferred to Phase 4 IGui.
// SceneRenderer populates this each frame from SubtitleQueue; VkRenderer
// stores the field but ignores it until the IGui subtitle renderer is wired.
// ---------------------------------------------------------------------------
struct SubtitleEntry {
    std::string text;
    float alpha{1.0f}; // reserved for future fade envelope; currently always 1.0
};

// ---------------------------------------------------------------------------
// Screen-space 2D HUD element for IRenderer::submitHudElements().
// Positions are normalized (0–1), top-left origin.
// string_view data must remain alive until after IRenderer::endFrame().
// ---------------------------------------------------------------------------
struct HudElement {
    enum class Type : uint8_t { Text, Line, Rect };

    Type type{Type::Text};
    float x{0.f};           // top-left / line-start X (0–1)
    float y{0.f};           // top-left / line-start Y (0–1)
    float x2{0.f};          // line-end X / rect right / unused for Text
    float y2{0.f};          // line-end Y / rect bottom / unused for Text
    float strokeWidth{1.f}; // Line: thickness in screen pixels
    float r{1.f}, g{1.f}, b{1.f}, a{1.f};
    float scale{1.f};      // Text: glyph scale multiplier (1.0 = base 8×16 px)
    std::string_view text; // Type::Text only; empty for Line/Rect
};

// ---------------------------------------------------------------------------
// Full scene description submitted between IRenderer::beginFrame and endFrame.
// Spans are non-owning views; the caller must keep the backing arrays alive
// until after endFrame() returns.
// ---------------------------------------------------------------------------
struct FrameScene {
    CameraView camera{};
    std::span<const RenderItem> renderItems{};
    EnvironmentState environment{};
    std::span<const ParticleEmitterState> particleEmitters{};
    std::span<const SubtitleEntry> subtitles{}; // VkRenderer ignores until Phase 4 IGui
};
