// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"
#include "render/RenderSnapshot.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>

namespace fl {

// Minimal in-flight HUD for the no-content-pack sandbox.
// Produces a list of 2D HudElements (text + geometry) for IRenderer::submitHudElements().
//
// The HUD is active only when a valid EntityRenderEntry pointer is passed to update().
// Pass nullptr (e.g. when not in Cockpit camera mode) to suppress all output.
//
// Default color: bright military green. Will be user-configurable in a later phase.
class FlightHud {
  public:
    // Build HUD elements for this frame.
    // Pass nullptr to produce no elements (e.g. when camera mode != Cockpit).
    // timeOfDay: hours [0, 24) displayed as HH:MM in the top-right corner.
    void update(const EntityRenderEntry* playerEntry, float timeOfDay = 12.0f);

    // Returns positioned, colored 2D elements for IRenderer::submitHudElements().
    // The span is valid until the next call to update().
    [[nodiscard]] std::span<const HudElement> elements() const;

  private:
    static constexpr int kMaxElements = 16;
    static constexpr int kMaxStrings = 10;

    std::array<HudElement, kMaxElements> m_elements;
    std::array<std::string, kMaxStrings> m_strings;
    std::size_t m_elementCount{0};
    std::size_t m_stringCount{0};
};

} // namespace fl
