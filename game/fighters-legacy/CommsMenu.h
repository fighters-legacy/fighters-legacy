// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IClock.h"
#include "IInput.h"
#include "RadioMenuCommon.h" // MenuTimeout + pickMenuItem — the shared radio-menu skeleton (#1265)
#include "RenderTypes.h"
#include "net/GameProtocol.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace fl {

// The in-flight comms menu (#704): the player's UI for talking to ATC (#673). Opens on T; digits
// 1-9 pick an item; Escape backs out a page (or closes at the root). A selection emits a
// MsgRadioCommand (verb string) that FlightScreen sends reliably; the server dispatches it to the ATC
// service and replies with a MsgRadioTransmission the subtitle/voice pipeline renders.
//
// NON-MODAL BY DESIGN, exactly like WingmanMenu (#610): opening the menu must NOT idle the engine, so
// the aircraft keeps flying and only the discrete keys the menu consumes are suppressed (FlightScreen
// passes uiFocused to FlightInputCollector). T and the digit keys collide with nothing in the flight
// controls. This is the shared radio-menu core #610 already extended with a wingman page; the root
// reserves a "Flight" slot for that.
class CommsMenu {
  public:
    void toggle() noexcept;
    void close() noexcept;
    [[nodiscard]] bool isOpen() const noexcept {
        return m_open;
    }

    // Consume this frame's input. Returns a MsgRadioCommand to send when the player picks a command
    // leaf; nullopt otherwise (navigation, submenu descent, close).
    std::optional<MsgRadioCommand> update(IInput& input);

    std::span<const HudElement> buildElements();

    void setClock(const IClock& clock) noexcept {
        m_clock = &clock;
    }

    // Test/telemetry.
    [[nodiscard]] int page() const noexcept {
        return static_cast<int>(m_page);
    }
    [[nodiscard]] int selected() const noexcept {
        return m_selected;
    }

  private:
    enum class Page : uint8_t { Root, Atc, Base };
    enum class Kind : uint8_t { Submenu, Command, Placeholder };
    struct Item {
        std::string_view label;
        Kind kind;
        Page target;              // for Kind::Submenu
        std::string_view command; // for Kind::Command
    };

    [[nodiscard]] std::span<const Item> items() const noexcept;
    void openAt(Page p) noexcept;

    bool m_open{false};
    Page m_page{Page::Root};
    int m_selected{0};
    uint16_t m_reqId{0};
    static constexpr float kMenuTimeoutS = 8.f; // auto-close an abandoned menu
    MenuTimeout m_openUntil{};
    std::array<HudElement, 10> m_elements{}; // title + up to a few items
    const IClock* m_clock{&SystemClock::instance()};
};

} // namespace fl
