// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

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

// AA mode — ordinals must stay in sync with AntiAliasingMode in engine/config/GraphicsSettings.h.
// MSAA was removed in favour of TAA (covers shading aliasing + is the temporal-upscaling on-ramp).
enum class RendererAAMode : uint8_t { Off, FXAA, TAA };

// Shadow quality — ordinals must stay in sync with ShadowQuality in engine/config/GraphicsSettings.h.
enum class RendererShadowQuality : uint8_t { Off, Low, Medium, High, Ultra };

// Particle density — ordinals must stay in sync with ParticleDensity in engine/config/GraphicsSettings.h.
enum class RendererParticleDensity : uint8_t { Low, Medium, High, Ultra };

// Ambient-occlusion quality (GTAO) — ordinals must stay in sync with AmbientOcclusion in
// engine/config/GraphicsSettings.h.
enum class RendererAOMode : uint8_t { Off, Low, High };

// Sky scattering model — ordinals must stay in sync with SkyQuality in
// engine/config/GraphicsSettings.h.  Procedural = per-pixel analytic; LUT = precomputed Hillaire LUTs.
enum class RendererSkyQuality : uint8_t { Procedural, LUT };

struct RendererSettings {
    RendererVsyncMode vsync{RendererVsyncMode::On};
    RendererAAMode aaMode{RendererAAMode::TAA};
    bool bloom{true};            // bloom on/off
    float drawDistanceKm{50.0f}; // entity cull distance in km (used by SceneRenderer)
    RendererShadowQuality shadowQuality{RendererShadowQuality::High};
    RendererParticleDensity particleDensity{RendererParticleDensity::High};
    RendererAOMode aoMode{RendererAOMode::High};            // GTAO quality
    RendererSkyQuality skyQuality{RendererSkyQuality::LUT}; // sky scattering model
    bool autoExposure{true};                                // HDR eye adaptation (always-on baseline)
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
// Frame capture (#912) — the pixel readback delivered to IRenderer::setCaptureSink,
// used by the cinematic recorder (#909) to pipe frames to video.
// ---------------------------------------------------------------------------

enum class CapturePixelFormat : uint8_t {
    RGBA8, // 8-bit R,G,B,A byte order (what the readback delivers; ffmpeg -pix_fmt rgba)
    BGRA8, // 8-bit B,G,R,A byte order (some swapchains; the consumer swizzles if it sees this)
};

// A single captured frame. `pixels` points into a renderer-owned buffer valid ONLY for the duration of
// the sink callback (copy what you keep). Tightly packed, `width*height*4` bytes, top row first.
struct CaptureFrame {
    uint32_t width{0};
    uint32_t height{0};
    CapturePixelFormat fmt{CapturePixelFormat::RGBA8};
    const uint8_t* pixels{nullptr};
    uint64_t frameIndex{0}; // monotonically increasing per delivered frame
};

// Convert a mapped swapchain readback (`pixelCount` RGBA/BGRA quads in `src`) to tightly packed RGBA8
// with an opaque alpha, into `dst` (>= pixelCount*4 bytes). When `bgra` is true the R and B channels are
// swapped (common swapchain byte order). Pure — the GPU-independent core of the capture readback, so it
// can be unit-tested without a device (#912).
inline void captureSwizzleToRgba(const uint8_t* src, uint32_t pixelCount, bool bgra, uint8_t* dst) {
    for (uint32_t p = 0; p < pixelCount; ++p) {
        const uint32_t i = p * 4u;
        dst[i + 0] = bgra ? src[i + 2] : src[i + 0];
        dst[i + 1] = src[i + 1];
        dst[i + 2] = bgra ? src[i + 0] : src[i + 2];
        dst[i + 3] = 255;
    }
}

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

    // The .glb is authored in the standard glTF/Blender CONTENT convention (nose along +Z); rotate it
    // into the engine body frame (nose along +X) on upload — see platform/MeshOrient.h (#906). Set by
    // the entity-mesh loader for pack-authored aircraft/unit/cockpit/damage meshes. Engine-generated
    // meshes (terrain tiles, the builtin placeholders and floor) are already in the body/world frame
    // and leave this false.
    bool contentForward{false};

    // Optional: resolve a glTF image URI (e.g. "../../textures/f5e_diffuse.ktx2") to raw KTX2/PNG
    // texture-file bytes. The engine layer wires this to AssetManager::loadTexture, because URI →
    // asset-name → file resolution belongs to the content system, not the GPU backend (#833). When
    // set, createMesh consumes the primitive's PBR material and attaches a MaterialHandle retrievable
    // via IRenderer::getMeshMaterial(); unset (builtin/terrain meshes) or an empty return leaves the
    // mesh with the renderer's default textures. Returns {} on a miss.
    std::function<std::vector<uint8_t>(std::string_view uri)> textureResolver{};
};

// Raw texture bytes: KTX2 (Basis Universal) preferred; PNG accepted as fallback.
struct TextureUploadDesc {
    std::string_view name;
    std::span<const uint8_t> bytes;
    bool srgb{true}; // true=color (sRGB view), false=linear (normal/ORM)
    // Raw-RGBA8 upload (#867): when rawWidth > 0, `bytes` is uncompressed RGBA8 (rawWidth*rawHeight*4),
    // uploaded directly with no KTX2/PNG decode. Used for the builtin procedural textures so the
    // albedo/normal/ORM sampling path runs with no content pack. 0 = decode `bytes` as KTX2 or PNG.
    uint32_t rawWidth{0};
    uint32_t rawHeight{0};
    // 2D texture-ARRAY upload (#446): when rawLayers > 1, the raw-RGBA path treats `bytes` as
    // rawLayers concatenated rawWidth*rawHeight*4 layers (layer-major) and builds a VK_IMAGE_VIEW_TYPE
    // _2D_ARRAY. 0/1 = a plain 2D texture. Used by the biome terrain arrays' builtin fallback; a KTX2
    // array (numLayers > 1) is detected from the container, so this field is for the raw path.
    uint32_t rawLayers{0};
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
    // World-space planet centre ({0,-R,0} for the spherical Earth), for the terrain shader's radial
    // "up" (#475). Set by SceneRenderer from the terrain streamer's baked radius; {0,0,0} default is
    // harmless when no terrain sphere is present (terrain shading is the only consumer).
    glm::dvec3 planetCenter{};
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
static constexpr uint32_t kRenderFlagDamaged = 1u << 0;        // use _b damage-variant nodes
static constexpr uint32_t kRenderFlagShadowOnly = 1u << 1;     // depth pass only
static constexpr uint32_t kRenderFlagTerrain = 1u << 2;        // apply elevation/slope shading in the forward pass
static constexpr uint32_t kRenderFlagDebugFaceColor = 1u << 3; // per-face debug colour (builtin placeholder mesh)
static constexpr uint32_t kRenderFlagRunway = 1u << 4; // paved runway: procedural markings in the forward pass (#487)
static constexpr uint32_t kRenderFlagTerrainSatellite =
    1u << 5; // terrain tile with a satellite albedo texture (#488): sample baseColorTex, not the biomes

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
    float windX{0.0f};         // world-frame wind m/s (from MsgWeatherState)
    float windZ{0.0f};         // world-frame wind m/s (from MsgWeatherState)
    float turbulenceAmp{0.0f}; // turbulence amplitude m/s (from MsgWeatherState, #426); client prediction
                               // feeds it to weatherTurbulence() to reproduce the server's per-tick turbulence
    bool isSnowPrecipitation{
        false}; // true when server preset is Snow or Blizzard; set by WeatherController::applyPresetToEnv

    // Altitude wind profile (#489): world-frame wind (m/s) at up to kWindProfileMaxKnots altitudes,
    // ascending. count == 0 means "no profile" — use the datum-level windX/windZ scalar above. Set on
    // the server from the mission/theater profile and mirrored to the client via the MsgWeatherState
    // TLV; both sides interpolate with the SAME pure code (WindProfile.h) so prediction stays in parity.
    static constexpr int kWindProfileMaxKnots = 8;
    struct WindKnot {
        float altM{0.0f};
        float windX{0.0f};
        float windZ{0.0f};
    };
    uint8_t windProfileCount{0};
    WindKnot windProfile[kWindProfileMaxKnots]{};

    // Night sky (#484), all set by WeatherController::applyGeographicCelestial (client-side, from the
    // shared UTC clock + the camera's lat/lon). Consumed by the sky shader for the Moon disc + the
    // geographically-oriented star field. celestialValid == false leaves the legacy day sky untouched.
    glm::vec3 moonDirection{0.0f, 1.0f, 0.0f}; // world-space, points toward the Moon
    float moonAngularRadius{0.0045f};          // radians (~0.26 deg)
    float moonIllumination{1.0f};              // [0,1]; 0 = new, 1 = full (disc phase is geometric)
    glm::mat3 worldToCelestial{1.0f};          // rotates a world ray into the fixed star frame
    bool celestialValid{false};
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
    float initialSpeed{5.0f};        // m/s, randomised within cone of coneHalfAngleDeg around emitDirection
    glm::vec3 colorStart{1.0f, 0.5f, 0.1f};
    glm::vec3 colorEnd{0.3f, 0.3f, 0.3f};
    float sizeStart{0.5f};                     // world-space metres at birth
    float sizeEnd{2.0f};                       // world-space metres at death
    bool additive{true};                       // true=additive blend (fire/explosion), false=alpha (smoke)
    glm::vec3 emitDirection{0.0f, 1.0f, 0.0f}; // normalised; cone centred on this axis
    float coneHalfAngleDeg{90.0f};             // emission cone half-angle; copied from preset by emit()
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

// Horizontal anchoring for HudElement Text: how `x` positions the string.
enum class HudAlign : uint8_t { Left, Center, Right };

// Base glyph cell of the HUD monospace font (GNU Unifont) in pixels at scale
// 1.0. Atlas layout details live in platform/vulkan/UnifontBitmap.h.
inline constexpr float kHudGlyphWidthPx = 8.0f;
inline constexpr float kHudGlyphHeightPx = 16.0f;

// Pixel offset added to the anchor x for a text run `widthPx` wide.
constexpr float hudAlignOffsetPx(HudAlign align, float widthPx) noexcept {
    if (align == HudAlign::Center)
        return -0.5f * widthPx;
    if (align == HudAlign::Right)
        return -widthPx;
    return 0.0f;
}

// ---------------------------------------------------------------------------
// Screen-space 2D overlay element for IRenderer::submitOverlayElements().
// Positions are normalized (0–1), top-left origin. For Text, `x` is the
// anchor point interpreted per `align` (left edge / center / right edge);
// Line/Rect ignore `align`.
// string_view data must remain alive until after IRenderer::endFrame().
// ---------------------------------------------------------------------------
struct HudElement {
    enum class Type : uint8_t { Text, Line, Rect };

    Type type{Type::Text};
    float x{0.f};           // Text anchor / line-start X (0–1)
    float y{0.f};           // top-left / line-start Y (0–1)
    float x2{0.f};          // line-end X / rect right / unused for Text
    float y2{0.f};          // line-end Y / rect bottom / unused for Text
    float strokeWidth{1.f}; // Line: thickness in screen pixels
    float r{1.f}, g{1.f}, b{1.f}, a{1.f};
    float scale{1.f};               // Text: glyph scale multiplier (1.0 = base 8×16 px)
    HudAlign align{HudAlign::Left}; // Text only: x anchors left edge / center / right edge
    std::string_view text;          // Type::Text only; empty for Line/Rect
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

    // Secondary-camera inset viewport (#695): render the same scene a second time from another camera
    // into a sub-rect (the #698 target-slaved inset now; rear-view / missile-cam / MFD repeaters later).
    // POD fields only — NO new IRenderer pure virtuals, so MockRenderer and every setScene consumer
    // compile unchanged. Disabled by default and bit-identical to today when off.
    bool insetEnabled{false};
    CameraView insetCamera{}; // its proj should be built with the inset rect's aspect
    float insetRect[4]{};     // normalized x, y, w, h (top-left origin — the HudElement convention)
};

} // namespace fl
