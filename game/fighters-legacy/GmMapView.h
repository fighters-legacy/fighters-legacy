// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net/GameProtocol.h" // GmEntityRecord

#include <glm/vec2.hpp>

#include <cstdint>
#include <span>

// Pure top-down plan-view map transform for the game-master overview (#861). The analogue of
// HudProjection for a flat, north-up battlespace map: it maps world XZ (metres) to normalized [0,1]
// screen coordinates and back, and picks the nearest entity to a click. No SDL, no rendering, no
// state beyond the camera — unit-tested in test_gm_map_view, reused by GmMapOverlay for both drawing
// (world -> HudElement positions) and click-selection (mouse -> world -> nearest entity).
//
// Convention: world +X maps to screen +X (right), world +Z to screen +Y (down). `spanMetresY` is the
// number of world metres spanned across the FULL viewport height; the X axis is scaled by the
// viewport aspect (width/height) so a metre is the same number of pixels in both axes (a circle stays
// a circle, north-up).

namespace fl {

struct GmMapView {
    double centerX{0.0};         // world X at the viewport centre (metres)
    double centerZ{0.0};         // world Z at the viewport centre (metres)
    double spanMetresY{20000.0}; // world metres across the full viewport height (the zoom level)
    float aspect{16.f / 9.f};    // viewport width / height

    // A selected entity handle ({idx,gen}); gen 0 with idx 0 = nothing selected is ambiguous, so
    // selection validity is tracked by hasSelection, not by the handle value.
    // (kept minimal here — selection state lives in GmMapOverlay via EntitySelector-style handles.)

    [[nodiscard]] glm::vec2 worldToMap(double worldX, double worldZ) const noexcept {
        const double dx = worldX - centerX;
        const double dz = worldZ - centerZ;
        const double nx = 0.5 + dx / (spanMetresY * static_cast<double>(aspect));
        const double ny = 0.5 + dz / spanMetresY;
        return glm::vec2{static_cast<float>(nx), static_cast<float>(ny)};
    }

    // Inverse: a normalized [0,1] screen point back to world XZ (metres). Returns the world X in .x
    // and world Z in .y of the result (a 2D world point in the ground plane).
    [[nodiscard]] glm::dvec2 mapToWorld(float nx, float ny) const noexcept {
        const double dx = (static_cast<double>(nx) - 0.5) * spanMetresY * static_cast<double>(aspect);
        const double dz = (static_cast<double>(ny) - 0.5) * spanMetresY;
        return glm::dvec2{centerX + dx, centerZ + dz};
    }

    // Pan the map centre by a world-space delta (metres).
    void pan(double dWorldX, double dWorldZ) noexcept {
        centerX += dWorldX;
        centerZ += dWorldZ;
    }

    // Zoom about the map centre: factor < 1 zooms in (fewer metres on screen), > 1 zooms out.
    // Clamped to a sane range so the map never inverts or spans an absurd distance.
    void zoom(double factor) noexcept {
        spanMetresY *= factor;
        if (spanMetresY < kMinSpanMetres)
            spanMetresY = kMinSpanMetres;
        if (spanMetresY > kMaxSpanMetres)
            spanMetresY = kMaxSpanMetres;
    }

    static constexpr double kMinSpanMetres = 500.0;       // ~0.5 km across — tight tactical view
    static constexpr double kMaxSpanMetres = 4'000'000.0; // continental view

    // Pick the record nearest to a normalized screen point, within `radiusNorm` (normalized-height
    // units). Returns the index into `records` of the nearest hit, or -1 if none is within the radius.
    // Distances are aspect-corrected so the pick radius is a true circle on screen.
    [[nodiscard]] int pick(std::span<const GmEntityRecord> records, glm::vec2 mapPos, float radiusNorm) const noexcept {
        int best = -1;
        float bestD2 = radiusNorm * radiusNorm;
        for (std::size_t i = 0; i < records.size(); ++i) {
            const glm::vec2 p = worldToMap(records[i].pos[0], records[i].pos[2]);
            const float ddx = (p.x - mapPos.x) * aspect; // undo the x compression so the radius is round
            const float ddy = p.y - mapPos.y;
            const float d2 = ddx * ddx + ddy * ddy;
            if (d2 <= bestD2) {
                bestD2 = d2;
                best = static_cast<int>(i);
            }
        }
        return best;
    }
};

} // namespace fl
