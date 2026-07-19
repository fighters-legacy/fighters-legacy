// SPDX-License-Identifier: GPL-3.0-or-later
#include "CommsMenu.h"

#include <cstring>

namespace fl {
namespace {

const fl::IClock& clockOf(const fl::IClock* c) {
    return c ? *c : fl::SystemClock::instance();
}

} // namespace

std::span<const CommsMenu::Item> CommsMenu::items() const noexcept {
    // Root page: ATC now, a Flight slot reserved for the #610 wingman page. ATC page verb strings
    // match the server's radio grammar ("atc <subverb>"). Static locals so they can name the private
    // nested types.
    static constexpr Item kRootItems[] = {
        {"1  ATC", Kind::Submenu, Page::Atc, {}},
        {"2  Ground crew", Kind::Submenu, Page::Base, {}},
        {"3  Flight (coming soon)", Kind::Placeholder, Page::Root, {}},
    };
    static constexpr Item kAtcItems[] = {
        {"1  Request takeoff", Kind::Command, Page::Atc, "atc request_takeoff"},
        {"2  Request landing", Kind::Command, Page::Atc, "atc request_landing"},
        {"3  Declare inbound", Kind::Command, Page::Atc, "atc inbound"},
        {"4  Cancel request", Kind::Command, Page::Atc, "atc cancel"},
    };
    // Base operations (#55): server-authoritative ground-crew services. The verbs match the server's
    // "base <subverb>" radio grammar; the crew chief answers over the same radio/subtitle path as
    // ATC, and refuses (with a reason) when the aircraft is not shut down at a base.
    static constexpr Item kBaseItems[] = {
        {"1  Refuel", Kind::Command, Page::Base, "base refuel"},
        {"2  Rearm", Kind::Command, Page::Base, "base rearm"},
        {"3  Repair", Kind::Command, Page::Base, "base repair"},
    };
    switch (m_page) {
    case Page::Atc:
        return {kAtcItems, std::size(kAtcItems)};
    case Page::Base:
        return {kBaseItems, std::size(kBaseItems)};
    case Page::Root:
    default:
        return {kRootItems, std::size(kRootItems)};
    }
}

void CommsMenu::openAt(Page p) noexcept {
    m_open = true;
    m_page = p;
    m_selected = 0;
    m_openUntil = clockOf(m_clock).now() + std::chrono::milliseconds(static_cast<long long>(kMenuTimeoutS * 1000.f));
}

void CommsMenu::toggle() noexcept {
    if (m_open)
        m_open = false;
    else
        openAt(Page::Root);
}

void CommsMenu::close() noexcept {
    m_open = false;
}

std::optional<MsgRadioCommand> CommsMenu::update(IInput& input) {
    if (!m_open)
        return std::nullopt;

    // An abandoned menu must not sit on the HUD forever while the player is busy flying.
    if (clockOf(m_clock).now() >= m_openUntil) {
        m_open = false;
        return std::nullopt;
    }

    if (input.isKeyJustPressed(Key::Escape)) {
        // Escape backs out one page; at the root it closes. FlightScreen must NOT read this as "pause"
        // — see its commsMenuWasOpen guard.
        if (m_page != Page::Root)
            openAt(Page::Root);
        else
            m_open = false;
        return std::nullopt;
    }

    const auto its = items();
    const int count = static_cast<int>(its.size());
    if (count <= 0)
        return std::nullopt;

    if (input.isKeyJustPressed(Key::ArrowDown))
        m_selected = (m_selected + 1) % count;
    if (input.isKeyJustPressed(Key::ArrowUp))
        m_selected = (m_selected + count - 1) % count;

    int chosen = -1;
    constexpr Key kDigits[9] = {Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5,
                                Key::Num6, Key::Num7, Key::Num8, Key::Num9};
    for (int i = 0; i < count && i < 9; ++i) {
        if (input.isKeyJustPressed(kDigits[i]))
            chosen = i;
    }
    if (input.isKeyJustPressed(Key::Enter))
        chosen = m_selected;

    if (chosen < 0 || chosen >= count)
        return std::nullopt;

    const Item& item = its[static_cast<std::size_t>(chosen)];
    switch (item.kind) {
    case Kind::Submenu:
        openAt(item.target);
        return std::nullopt;
    case Kind::Placeholder:
        return std::nullopt; // reserved (the #610 wingman page lives elsewhere for now)
    case Kind::Command: {
        m_open = false;
        MsgRadioCommand msg{};
        msg.reqId = ++m_reqId;
        std::snprintf(msg.command, sizeof(msg.command), "%.*s", static_cast<int>(item.command.size()),
                      item.command.data());
        return msg;
    }
    }
    return std::nullopt;
}

std::span<const HudElement> CommsMenu::buildElements() {
    if (!m_open)
        return {};

    std::size_t n = 0;
    HudElement title{};
    title.type = HudElement::Type::Text;
    title.x = 0.03f;
    title.y = 0.40f;
    title.scale = 1.f;
    title.r = 0.4f;
    title.g = 0.9f;
    title.b = 1.f;
    title.a = 1.f;
    title.text = (m_page == Page::Atc) ? "COMMS - ATC" : (m_page == Page::Base) ? "COMMS - GROUND CREW" : "COMMS";
    m_elements[n++] = title;

    const auto its = items();
    for (std::size_t i = 0; i < its.size() && n < m_elements.size(); ++i) {
        HudElement e{};
        e.type = HudElement::Type::Text;
        e.x = 0.03f;
        e.y = 0.44f + 0.035f * static_cast<float>(i);
        e.scale = 1.f;
        const bool sel = (static_cast<int>(i) == m_selected);
        const bool placeholder = its[i].kind == Kind::Placeholder;
        e.r = sel ? 1.f : 0.4f;
        e.g = sel ? 1.f : 0.9f;
        e.b = sel ? 0.5f : 1.f;
        e.a = placeholder ? 0.35f : 1.f; // reserved items are visibly inert
        e.text = its[i].label;
        m_elements[n++] = e;
    }
    return std::span<const HudElement>(m_elements.data(), n);
}

} // namespace fl
