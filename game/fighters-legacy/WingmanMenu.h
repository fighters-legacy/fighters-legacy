// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IClock.h"
#include "IInput.h"
#include "RenderTypes.h"
#include "ai/WingmanCommand.h"
#include "net/GameProtocol.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace fl {

// The in-flight radio menu (#610): the player's UI for ordering their flight, and the zero-AI half of
// Epic O. Opens on InputAction::WingmanMenu (C by default); 1-6 pick a command; Escape closes.
//
// NON-MODAL BY DESIGN — this is the load-bearing UX decision. GameConsole suppresses flight input
// while it is open, which leaves MsgClientInput at its defaults, i.e. OPENING THE CONSOLE SENDS
// throttle = 0. That is survivable for a debug console; inheriting it for a combat radio menu would
// idle the engine mid-dogfight. So the aircraft keeps flying while the menu is up, and only the
// discrete keys/buttons the menu itself consumes are suppressed (see FlightInputCollector's
// uiFocused parameter). A radio call is a sub-second action; freezing the jet to make one is wrong.
//
// The brevity lines are CLIENT-SIDE (see kBrevity in the .cpp). The server sends a result CODE, never
// text: brevity calls are UI strings that must be localizable, and the menu needs the same table for
// its labels anyway.
class WingmanMenu {
  public:
    void toggle() noexcept;
    void close() noexcept;
    [[nodiscard]] bool isOpen() const noexcept {
        return m_open;
    }

    // Consume menu input for this frame. Returns the order to send when the player picks one.
    // Returns nullopt otherwise (including when the player has no flight — the menu still opens, so
    // the feature is discoverable, but a selection sends nothing).
    std::optional<MsgWingmanCommand> update(IInput& input);

    // Server response: an order outcome, the on-connect check-in, or a radio call RELAYED from a
    // commander (i.e. someone ordered *you*, as a human member of their flight).
    void onAck(const MsgWingmanAck& ack);

    [[nodiscard]] std::span<const HudElement> buildElements();

    void setClock(const fl::IClock& clock) noexcept {
        m_clock = &clock;
    }

    // Test/telemetry.
    [[nodiscard]] uint8_t flightSize() const noexcept {
        return m_flightSize;
    }
    [[nodiscard]] uint16_t flightId() const noexcept {
        return m_flightId;
    }
    [[nodiscard]] std::string_view brevity() const noexcept {
        return m_brevity;
    }

  private:
    void setBrevity(std::string_view line);

    static constexpr float kMenuTimeoutS = 8.f; // auto-close: an abandoned menu must not sit there
    static constexpr float kBrevityHoldS = 4.f; // how long a radio line stays on screen

    bool m_open{false};
    int m_selected{0};
    uint32_t m_seqNum{0};

    uint8_t m_flightSize{0}; // 0 until the check-in arrives; 0 = no flight, menu shows dimmed
    uint16_t m_flightId{kNoFlightId};

    // HudElement::text is a NON-OWNING string_view, so the backing store must outlive the frame.
    char m_brevity[64]{};
    bool m_brevityActive{false};
    std::chrono::steady_clock::time_point m_brevityUntil{};
    std::chrono::steady_clock::time_point m_openUntil{};

    std::array<HudElement, 10> m_elements{};
    const fl::IClock* m_clock{nullptr};
};

} // namespace fl
