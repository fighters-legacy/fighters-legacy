// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"
#include "render/BuiltinShape.h"

#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

class AssetManager;
class ILogger;
class IRenderer;

// The debug shading a preview renders with (#836). Shaded/FaceColor work with the render flags that
// already exist; Wireframe/Normals exist in the enum from day one but map to Shaded until the
// renderer's LINE pipeline + normals shadingMode land (#838) — PreviewScene warns once when asked for
// an unsupported view so the enum is stable across the two commits.
enum class PreviewDebugView : uint8_t { Shaded, FaceColor, Wireframe, Normals };

// A spherical orbit around a focus point — the pure output of frameBounds() and the input to
// previewCameraView(). yaw 0 looks down -X at the nose; positive pitch raises the eye.
struct PreviewOrbit {
    glm::vec3 focus{0.0f};
    float yawDeg{35.0f};   // 3/4-front-left default so the silhouette reads
    float pitchDeg{18.0f}; // above the horizon
    float distance{10.0f}; // metres from focus to eye
};

// Frame an object-space AABB: focus = centre, distance sized so the bounding sphere fills `fovY` with
// `margin` slack. A degenerate/empty box (min > max) falls back to a 5 m stand-off. Pure — no renderer.
[[nodiscard]] PreviewOrbit frameBounds(const glm::vec3& minB, const glm::vec3& maxB, float fovY = 1.0472f,
                                       float margin = 1.15f);

// Build a CameraView for an orbit. Delegates to makeCameraView() so the reverse-Z / Y-flip /
// camera-relative conventions are identical to the game camera and cannot diverge. Pure.
[[nodiscard]] CameraView previewCameraView(const PreviewOrbit& orbit, float aspect, float fovY = 1.0472f,
                                           float near = 0.05f);

// A single-model preview: the shared bootstrap the headless snapshot (#666) and the interactive
// fl-viewer (#838) both drive. It uploads one content mesh (or a bare .glb's bytes), resolves its
// glTF PBR material through the SAME resolver SceneRenderer uses (MeshTextureResolver.h), computes
// the model bounds for camera framing, and builds the per-frame RenderItem list. It knows nothing
// about entity defs — its input is resolved asset names / raw bytes, so engine-render stays free of
// engine-entity (the MeshNameResolver-injection discipline).
//
// Threading: main thread only, like SceneRenderer.
class PreviewScene {
  public:
    // What to preview. Exactly one source is used: `glbBytes` (bare-file mode, textures resolved
    // relative to `glbDir`) when non-empty, else `meshAssetName` resolved through `assets`.
    struct ModelDesc {
        std::string meshAssetName;       // pack-mode primary geometry asset name
        std::string damageMeshAssetName; // optional damage variant (EntityDef::classicDamageMesh)
        std::unordered_map<std::string, std::string> liveryOverrides; // "<slot>.<map>" -> texture asset name (#845)

        std::vector<uint8_t> glbBytes; // bare-file mode: raw .glb contents (wins over meshAssetName)
        std::filesystem::path glbDir;  // directory the bare .glb lives in, for relative texture URIs
        bool contentForward{true};     // pack-authored nose +Z -> body +X (#906)
        BuiltinShape fallbackShape{BuiltinShape::AirVehicle}; // drawn when nothing loads
    };

    // `assets` may be null in bare-file mode (glbBytes set, no pack textures). `logger` may be null.
    PreviewScene(IRenderer& renderer, AssetManager* assets, ILogger* logger);
    ~PreviewScene();

    PreviewScene(const PreviewScene&) = delete;
    PreviewScene& operator=(const PreviewScene&) = delete;

    // Upload the model (and its damage variant, if any); resolve material; compute bounds. Returns
    // true when a content mesh loaded; false means the builtin placeholder is being drawn instead
    // (still a valid, framed scene). Replaces any previously loaded model (destroys old handles).
    bool load(ModelDesc desc);

    // Re-run load(m_desc) after the underlying files changed (hot-reload seam, #152/#838): destroys
    // the current handles and re-reads bytes. Returns load()'s result.
    bool reload();

    void setDamaged(bool on) noexcept;
    void setDebugView(PreviewDebugView view) noexcept;

    [[nodiscard]] bool hasDamageMesh() const noexcept;
    [[nodiscard]] bool loadedFromContent() const noexcept; // false = builtin fallback is drawn
    [[nodiscard]] glm::vec3 boundsMin() const noexcept {
        return m_boundsMin;
    }
    [[nodiscard]] glm::vec3 boundsMax() const noexcept {
        return m_boundsMax;
    }

    // Deterministic environment defaults tuned for reproducible goldens (noon sun, no fog/clouds).
    // Mutable so a caller can tweak lighting; returns a live reference.
    [[nodiscard]] EnvironmentState& environment() noexcept {
        return m_env;
    }
    [[nodiscard]] const EnvironmentState& environment() const noexcept {
        return m_env;
    }

    // RenderItems for this frame (1 model item today; grid/gizmo overlays are the tool's concern).
    // The model sits at the WORLD origin; `cameraWorldOrigin` (== CameraView::worldOrigin, i.e. the
    // eye) rebases it camera-relative on the CPU, as the renderer's invariant requires (the shader
    // does no further offset). The span is backed by member storage valid until the next buildItems()
    // call — the setScene contract.
    [[nodiscard]] std::span<const RenderItem> buildItems(const glm::dvec3& cameraWorldOrigin = glm::dvec3(0.0));

  private:
    void destroyHandles();
    // Upload the builtin procedural PBR textures + a grey material once (matches SceneRenderer's
    // m_fallbackEntityMat, so a material-less mesh previews exactly as the game draws it).
    MaterialHandle ensureFallbackMaterial();
    // Upload one mesh (pack asset name, or the bare glbBytes when `bareBytes` is non-empty) + resolve
    // its material; returns the mesh handle (invalid on failure). Fills `outMin/outMax` on success.
    MeshHandle uploadMesh(const std::string& assetName, std::span<const uint8_t> bareBytes, glm::vec3& outMin,
                          glm::vec3& outMax);
    uint32_t debugFlags() const noexcept;

    IRenderer& m_renderer;
    AssetManager* m_assets{nullptr};
    ILogger* m_logger{nullptr};

    ModelDesc m_desc;
    MeshHandle m_mesh{};
    MaterialHandle m_material{};
    MeshHandle m_damageMesh{};
    MaterialHandle m_damageMaterial{};
    MaterialHandle m_greyFallback{};   // lazily created grey PBR material for a material-less mesh
    TextureHandle m_fallbackBaseTex{}; // builtin procedural PBR maps backing m_greyFallback (#867)
    TextureHandle m_fallbackNormalTex{};
    TextureHandle m_fallbackOrmTex{};
    glm::vec3 m_boundsMin{-2.0f};
    glm::vec3 m_boundsMax{2.0f};

    bool m_loadedContent{false};
    bool m_damaged{false};
    PreviewDebugView m_view{PreviewDebugView::Shaded};

    EnvironmentState m_env;
    std::vector<RenderItem> m_items;
};

} // namespace fl
