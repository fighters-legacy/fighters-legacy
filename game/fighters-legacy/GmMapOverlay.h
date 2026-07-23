// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "GmMapView.h"
#include "RenderTypes.h" // HudElement

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fl {

class IGui;
class IInput;
class IWindow;
struct ClientNetEventHandler;
class EntityTypeRegistry;

// Game-master overview map (#861). An in-flight overlay (the Scoreboard/Chat pattern — NOT a Screen)
// that draws the whole battlespace top-down from the aggregate GM feed, lets a GM click-select an
// entity, issue orders through the permission-checked admin channel, and drop into an entity's view.
//
// The map canvas is drawn with HudElements (IGui has no drawing primitives); the select/order panel
// uses IGui. Selection + click math live in the pure GmMapView. Orders are plain admin command
// strings sent through an injected callback (the server permission-checks them); the "View" button
// hands a {idx,gen} back to FlightScreen, which drives its EntitySelector + chase camera (#860).
//
// Non-modal like the wingman menu: FlightScreen keeps flying and only gates the keys/mouse the map
// consumes. The map opens for any peer (discoverability) but only a GmMap-capable peer's orders do
// anything — the panel shows a "no authority" note otherwise, and the server refuses regardless.
class GmMapOverlay {
  public:
    struct Deps {
        ClientNetEventHandler* net{nullptr};   // gm feed + roster/faction labels + granted caps
        EntityTypeRegistry* registry{nullptr}; // type-name labels for the icons
        IGui* gui{nullptr};                    // the select/order side panel; null = panel disabled
        // Sends an admin command string to the server (makeNetworkAdminSender). Null = orders disabled.
        std::function<void(std::string_view)> serverCommand;
    };

    // A "view from this entity" request the FlightScreen consumes (selects it + chase camera).
    struct ViewRequest {
        uint32_t idx{0};
        uint16_t gen{0};
    };

    GmMapOverlay() = default;
    explicit GmMapOverlay(Deps deps) : m_deps(std::move(deps)) {}

    // Set/replace the dependencies (Game.cpp wires these once the session objects exist). Closing the
    // map on a deps change avoids a dangling selection across sessions.
    void setDeps(Deps deps) {
        m_deps = std::move(deps);
        setOpen(false);
        m_hasSel = false;
    }

    [[nodiscard]] bool isOpen() const noexcept {
        return m_open;
    }
    void toggle() noexcept {
        setOpen(!m_open);
    }
    void setOpen(bool open) noexcept;

    // Per-frame update while open: pan/zoom keys, mouse pick, and the IGui select/order panel. Call
    // between gui->newFrame()/render(). No-op when closed.
    void update(IInput& input, IWindow& window);

    // The map canvas (background, grid, entity markers, selection box, labels) as HudElements. Empty
    // when closed. The backing string storage lives in the overlay, so the span is valid until the
    // next update()/buildElements().
    [[nodiscard]] std::span<const HudElement> buildElements() const noexcept {
        return {m_elements.data(), m_elements.size()};
    }

    // Take (and clear) a pending view-from-entity request. FlightScreen calls this each frame.
    [[nodiscard]] std::optional<ViewRequest> takeViewRequest() noexcept {
        auto r = m_viewRequest;
        m_viewRequest.reset();
        return r;
    }

    // Test/telemetry: the current selection handle (valid only when hasSelection()).
    [[nodiscard]] bool hasSelection() const noexcept {
        return m_hasSel;
    }
    [[nodiscard]] uint32_t selectedIdx() const noexcept {
        return m_selIdx;
    }

  private:
    void rebuildElements(int logicalW, int logicalH);
    void renderPanel(); // IGui side panel: details + order buttons
    void sendOrder(std::string_view cmd);

    // The map occupies the left part of the screen; the IGui panel the right strip.
    static constexpr float kMapX0 = 0.02f;
    static constexpr float kMapY0 = 0.06f;
    static constexpr float kMapX1 = 0.74f;
    static constexpr float kMapY1 = 0.96f;

    Deps m_deps;
    bool m_open{false};
    GmMapView m_view{};

    // Selection ({idx,gen} handle; resolved against the live feed each frame — a stale pick clears).
    bool m_hasSel{false};
    uint32_t m_selIdx{0};
    uint16_t m_selGen{0};

    std::optional<ViewRequest> m_viewRequest;

    std::vector<HudElement> m_elements;
    std::vector<std::string> m_labelStore; // owns the text HudElements point into
};

} // namespace fl
