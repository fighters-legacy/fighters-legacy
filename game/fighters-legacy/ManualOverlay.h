// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h" // HudElement
#include "manual/AircraftManual.h"

#include <span>
#include <vector>

namespace fl {

class IInput;

// The in-flight aircraft manual (#821), reachable WITHOUT LEAVING THE AIRCRAFT -- which is an
// acceptance criterion, not a nicety. A reference you have to quit the mission to read is a reference
// nobody reads.
//
// It renders an AircraftManual, which is generated from the flight model, the entity's stations, the
// weapon registry and the sensor suite. Nothing here authors a number; this class only lays out what
// the engine already knows.
//
// NON-MODAL, for the same reason WingmanMenu is (#610): GameConsole suppresses flight input while
// open, which leaves MsgClientInput at its defaults and idles the engine. That is survivable for a
// debug console. Inheriting it for a reference card you might want to glance at in the air is not --
// so the aircraft keeps flying, and only the keys the overlay consumes are gated.
class ManualOverlay {
  public:
    // Cached: buildAircraftManual() trims the flight model, which is a root-finding loop and has no
    // business running per frame. Call once when the aircraft is known.
    void setManual(AircraftManual manual);

    [[nodiscard]] bool isOpen() const noexcept {
        return m_open;
    }
    void toggle() noexcept;
    void close() noexcept;

    // Scrolls; a full manual does not fit on one screen.
    void scroll(int lines) noexcept;

    // Rebuilds the HudElements for this frame. No-op when closed.
    void update();

    [[nodiscard]] std::span<const HudElement> elements() const noexcept {
        return m_elements;
    }

    [[nodiscard]] bool hasManual() const noexcept {
        return !m_manual.sections.empty() || !m_manual.prose.empty();
    }

  private:
    AircraftManual m_manual;
    std::vector<HudElement> m_elements;
    bool m_open{false};
    int m_scroll{0};
};

} // namespace fl
