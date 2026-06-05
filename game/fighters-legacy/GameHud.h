// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"
#include "render/FlightHud.h"

#include <cstdint> // uint8_t (CameraMode underlying type), uint16_t
#include <span>
#include <string_view>
#include <vector>

namespace fl {
struct EntityRenderEntry;
enum class CameraMode : uint8_t;
} // namespace fl

// Composite in-game HUD layer: flight data (from FlightHud) + server-notice banner.
//
// Intentionally decoupled from DebugConsole — the debug console is an independent
// overlay that must remain available in all game states including the main menu.
// GameHud produces only flight-relevant elements; callers merge the two spans
// themselves before calling IRenderer::submitHudElements.
class GameHud {
  public:
    // Called by ClientNetEventHandler when a MsgServerNotice arrives.
    void setNotice(std::string_view text, uint16_t secondsRemaining);

    // Build HUD elements for this frame.
    // player: nullptr suppresses flight data (e.g. not in Cockpit mode).
    // terrainElevation: ground height in metres at the player XZ position
    // (from TerrainStreamer::heightAt); 0.0 is safe when terrain is not loaded.
    void update(fl::CameraMode mode, const fl::EntityRenderEntry* player, float timeOfDay,
                float terrainElevation = 0.0f);

    // Returns flight HUD + active server-notice elements only.
    // Does NOT include debug console elements.
    [[nodiscard]] std::span<const HudElement> buildElements();

  private:
    fl::FlightHud m_flightHud;
    std::vector<HudElement> m_elements;

    char m_noticeBuf[72]{};
    uint16_t m_noticeSecsLeft{0};
    bool m_hasNotice{false};
};
