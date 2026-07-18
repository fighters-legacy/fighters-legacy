// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"
#include "world/AirportRegistry.h"

#include <cstdint>
#include <glm/vec3.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

class IRenderer;

// Emits runway geometry (#487). For each runway near the camera it draws a camera-relative slab
// tessellated along the spherical datum a few cm above the flattened terrain (a flat quad would chord
// below the curved flattened surface), tinted by surface, with paved runways carrying procedural
// markings drawn in-shader from the slab's runway-local UV. Meshes/materials are cached per runway;
// the visible set is culled to the airports near the camera via the registry grid. SceneRenderer owns
// one and calls appendRenderItems each frame beside the terrain items.
class AirportRenderer {
  public:
    explicit AirportRenderer(IRenderer& renderer);

    void setRegistry(const AirportRegistry* reg) noexcept {
        m_registry = reg;
    }

    // Appends one RenderItem per visible runway to `out`. cameraWorldOrigin is the camera-relative
    // rebase origin (CameraView::worldOrigin); planetRadiusM is the registry's radius.
    void appendRenderItems(glm::dvec3 cameraWorldOrigin, std::vector<RenderItem>& out);

  private:
    struct CachedRunway {
        MeshHandle mesh;
        glm::dvec3 origin{0.0}; // runway-centre world position (the rebase anchor)
        bool paved{false};
    };
    IRenderer& m_renderer;
    const AirportRegistry* m_registry{nullptr};
    std::unordered_map<std::string, CachedRunway> m_cache; // key = "<airport id>:<runway idx>"
    MaterialHandle m_materials[6]{};                       // one per RunwaySurface ordinal
    bool m_materialsBuilt{false};

    void ensureMaterials();
};

// The runway slab vertices in world space (metres), tessellated along the spherical datum at
// elevationM + a small lift. Exposed as a free function for unit testing the geometry (vertex count,
// extents, on-sphere placement); Water runways return an empty vector (the sea is the runway).
[[nodiscard]] std::vector<glm::dvec3> runwaySlabVertices(const ResolvedRunway& rw, double elevationM,
                                                         double planetRadiusM);

// Binary glTF (GLB) for a runway slab: POSITION + NORMAL + TEXCOORD_0 (u=along [0,1], v=across [0,1]),
// vertices relative to `originWorld`. Empty for a Water runway. surface is carried by the material.
[[nodiscard]] std::vector<uint8_t> buildRunwaySlabGlb(const ResolvedRunway& rw, double elevationM, double planetRadiusM,
                                                      glm::dvec3 originWorld);

} // namespace fl
