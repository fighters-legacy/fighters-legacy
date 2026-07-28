// SPDX-License-Identifier: GPL-3.0-or-later
#include "WingmanMenu.h"

#include <cstdio>
#include <cstring>

namespace fl {
namespace {

// Menu labels, in grammar order (WingmanCommand ordinals 0..5). The digit keys map straight onto
// these, so the ordinal IS the menu position — no lookup table to fall out of sync.
constexpr std::string_view kLabels[fl::ai::kWingmanCommandCount] = {
    "1  Attack my target", "2  Engage bandits", "3  Rejoin", "4  Cover me", "5  Hold fire", "6  Return to base",
};

// The brevity call the wingman answers with. Client-side because these are UI strings that need
// localizing; the server only ever sends a result code.
std::string_view brevityFor(uint8_t command, WingmanResult result) {
    switch (result) {
    case WingmanResult::CheckIn:
        return "TWO: On your wing.";
    case WingmanResult::NoTarget:
        // The honest answer when nothing is in the boresight cone. The wingman did NOT pick its own
        // target, and the player needs to know that rather than wondering why nothing happened.
        return "TWO: No joy.";
    case WingmanResult::NoFlight:
        return "No flight assigned.";
    case WingmanResult::Unavailable:
        return "TWO is down.";
    case WingmanResult::RateLimited:
    case WingmanResult::Rejected:
        return "TWO: Say again?";
    case WingmanResult::NotLead:
        return "You are not the flight lead.";
    case WingmanResult::Relayed:
        // Delivered TO a human member: someone has ordered YOU. Rendered as an inbound radio call.
        switch (static_cast<fl::ai::WingmanCommand>(command)) {
        case fl::ai::WingmanCommand::AttackMyTarget:
            return "LEAD: Attack my target.";
        case fl::ai::WingmanCommand::EngageBandits:
            return "LEAD: Engage bandits.";
        case fl::ai::WingmanCommand::Rejoin:
            return "LEAD: Rejoin.";
        case fl::ai::WingmanCommand::CoverMe:
            return "LEAD: Cover me.";
        case fl::ai::WingmanCommand::HoldFire:
            return "LEAD: Hold fire.";
        case fl::ai::WingmanCommand::ReturnToBase:
            return "LEAD: Return to base.";
        default:
            return "LEAD: Say again?";
        }
    case WingmanResult::Acknowledged:
    default:
        switch (static_cast<fl::ai::WingmanCommand>(command)) {
        case fl::ai::WingmanCommand::AttackMyTarget:
            return "TWO: Engaged.";
        case fl::ai::WingmanCommand::EngageBandits:
            return "TWO: Engaging.";
        case fl::ai::WingmanCommand::Rejoin:
            return "TWO: Rejoining.";
        case fl::ai::WingmanCommand::CoverMe:
            return "TWO: Covering.";
        case fl::ai::WingmanCommand::HoldFire:
            return "TWO: Weapons hold.";
        case fl::ai::WingmanCommand::ReturnToBase:
            return "TWO: RTB.";
        default:
            return "TWO: Copy.";
        }
    }
}

const fl::IClock& clockOf(const fl::IClock* c) {
    return c ? *c : fl::SystemClock::instance();
}

} // namespace

void WingmanMenu::toggle() noexcept {
    m_open = !m_open;
    if (m_open) {
        m_selected = 0;
        m_openUntil =
            clockOf(m_clock).now() + std::chrono::milliseconds(static_cast<long long>(kMenuTimeoutS * 1000.f));
    }
}

void WingmanMenu::close() noexcept {
    m_open = false;
}

void WingmanMenu::setBrevity(std::string_view line) {
    std::snprintf(m_brevity, sizeof(m_brevity), "%.*s", static_cast<int>(line.size()), line.data());
    m_brevityActive = true;
    m_brevityUntil = clockOf(m_clock).now() + std::chrono::milliseconds(static_cast<long long>(kBrevityHoldS * 1000.f));
}

std::optional<MsgWingmanCommand> WingmanMenu::update(IInput& input) {
    if (!m_open)
        return std::nullopt;

    // An abandoned menu must not sit on the HUD forever while the player is busy flying.
    if (clockOf(m_clock).now() >= m_openUntil) {
        m_open = false;
        return std::nullopt;
    }

    if (input.isKeyJustPressed(Key::Escape)) {
        m_open = false; // FlightScreen must NOT also read this as "pause" — see its menuWasOpen guard
        return std::nullopt;
    }

    // Arrow/gamepad navigation, for players who would rather not hunt for a digit mid-turn.
    if (input.isKeyJustPressed(Key::ArrowDown))
        m_selected = (m_selected + 1) % static_cast<int>(fl::ai::kWingmanCommandCount);
    if (input.isKeyJustPressed(Key::ArrowUp))
        m_selected = (m_selected + static_cast<int>(fl::ai::kWingmanCommandCount) - 1) %
                     static_cast<int>(fl::ai::kWingmanCommandCount);

    int chosen = -1;
    // Digits 1-6 select directly. The digit IS the ordinal + 1, so there is no mapping to get wrong.
    constexpr Key kDigits[fl::ai::kWingmanCommandCount] = {Key::Num1, Key::Num2, Key::Num3,
                                                           Key::Num4, Key::Num5, Key::Num6};
    for (int i = 0; i < static_cast<int>(fl::ai::kWingmanCommandCount); ++i) {
        if (input.isKeyJustPressed(kDigits[i]))
            chosen = i;
    }
    if (input.isKeyJustPressed(Key::Enter))
        chosen = m_selected;

    if (chosen < 0)
        return std::nullopt;

    m_open = false;

    if (m_flightSize == 0) {
        // The menu still opened (discoverability), but there is nobody to order. Say so locally
        // rather than sending a packet the server would only refuse.
        setBrevity(brevityFor(static_cast<uint8_t>(chosen), WingmanResult::NoFlight));
        return std::nullopt;
    }

    MsgWingmanCommand msg{};
    msg.command = static_cast<uint8_t>(chosen);
    msg.memberIdx = kFlightAll; // the radio menu orders the whole flight; the admin path addresses members
    msg.flightId = m_flightId != kNoFlightId ? m_flightId : kOwnFlight;
    msg.seqNum = ++m_seqNum;
    return msg;
}

void WingmanMenu::setVoiceStatus(std::string status) {
    // Routed through the brevity line rather than a second overlay: it is the same kind of message
    // in the same place a pilot already looks for the wingman's answer, and an empty status clears
    // it exactly as a timed-out brevity line does.
    if (status.empty()) {
        m_brevityActive = false;
        m_brevity[0] = '\0';
        return;
    }
    setBrevity(status);
}

void WingmanMenu::onAck(const MsgWingmanAck& ack) {
    const auto result = static_cast<WingmanResult>(ack.result);

    // A Relayed call is someone ordering US — it says nothing about a flight we command, so it must
    // not overwrite our own flight size or id.
    if (result != WingmanResult::Relayed) {
        m_flightSize = ack.flightSize;
        if (ack.flightId != kNoFlightId)
            m_flightId = ack.flightId;
    }

    setBrevity(brevityFor(ack.command, result));
}

std::span<const HudElement> WingmanMenu::buildElements() {
    std::size_t n = 0;
    const auto now = clockOf(m_clock).now();

    if (m_brevityActive && now >= m_brevityUntil)
        m_brevityActive = false;

    if (m_open) {
        const bool haveFlight = m_flightSize > 0;

        HudElement title{};
        title.type = HudElement::Type::Text;
        title.x = 0.03f;
        title.y = 0.42f;
        title.scale = 1.f;
        title.r = 0.35f;
        title.g = 1.f;
        title.b = 0.35f;
        title.a = 1.f;
        title.text = haveFlight ? "FLIGHT" : "NO FLIGHT";
        m_elements[n++] = title;

        for (std::size_t i = 0; i < fl::ai::kWingmanCommandCount; ++i) {
            HudElement e{};
            e.type = HudElement::Type::Text;
            e.x = 0.03f;
            e.y = 0.46f + 0.035f * static_cast<float>(i);
            e.scale = 1.f;
            const bool sel = (static_cast<int>(i) == m_selected);
            e.r = sel ? 1.f : 0.35f;
            e.g = 1.f;
            e.b = sel ? 0.5f : 0.35f;
            // Dimmed when there is nobody to order: the menu is still discoverable, but visibly inert.
            e.a = haveFlight ? 1.f : 0.35f;
            e.text = kLabels[i];
            m_elements[n++] = e;
        }
    }

    if (m_brevityActive) {
        HudElement e{};
        e.type = HudElement::Type::Text;
        e.x = 0.03f;
        e.y = 0.72f;
        e.scale = 1.f;
        e.r = 0.6f;
        e.g = 1.f;
        e.b = 0.6f;
        e.a = 1.f;
        e.text = m_brevity; // backed by the member buffer, so it outlives the frame
        m_elements[n++] = e;
    }

    return std::span<const HudElement>(m_elements.data(), n);
}

} // namespace fl
