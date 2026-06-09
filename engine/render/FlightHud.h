// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"
#include "render/IHud.h"
#include "render/RenderSnapshot.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>

namespace fl {

// Builtin aircraft HUD for the no-content-pack sandbox. Implements IHud.
// Produces a list of 2D HudElements (text + geometry) for IRenderer::submitOverlayElements().
//
// The HUD is active only when a valid EntityRenderEntry pointer is passed to update().
// Pass nullptr (e.g. when not in Cockpit camera mode) to suppress all output.
//
// Default color: bright military green. Will be user-configurable in a later phase.
class FlightHud : public IHud {
  public:
    // Build HUD elements for this frame.
    // Pass nullptr to produce no elements (e.g. when camera mode != Cockpit).
    // timeOfDay: hours [0, 24) displayed as HH:MM in the top-right corner.
    // terrainElevation: ground height in metres at the player XZ position (from
    // TerrainStreamer::heightAt). Falls back to 0.0 (AGL == MSL) when not loaded.
    void update(const EntityRenderEntry* playerEntry, float timeOfDay = 12.0f, float terrainElevation = 0.0f) override;

    // Returns elements for IRenderer::submitOverlayElements(). Valid until next update().
    [[nodiscard]] std::span<const HudElement> elements() const override;

  private:
    static constexpr int kMaxElements = 16;
    static constexpr int kMaxStrings = 10;

    std::array<HudElement, kMaxElements> m_elements;
    std::array<std::string, kMaxStrings> m_strings;
    std::size_t m_elementCount{0};
    std::size_t m_stringCount{0};
};

} // namespace fl
