// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IClock.h"
#include "RenderTypes.h"
#include "net/GameProtocol.h" // ChatChannel, kMaxChatBytes

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fl {

class IGui;

// In-match text chat overlay (#646). Two halves:
//   * DISPLAY — a bottom-left ring of recent lines rendered as HudElements (dwell + fade, injectable
//     clock). Lines from a locally muted callsign are dropped at push time.
//   * INPUT — an IGui text box opened on the chat key; the game feeds edits through IGui::inputText and
//     submits the buffer on the Send button (or, wired by FlightScreen, the Enter key). Escape cancels.
//
// The overlay owns persistent char buffers for the display lines because HudElement::text is a
// non-owning string_view and must outlive endFrame. Main thread only.
class ChatOverlay {
  public:
    static constexpr std::size_t kMaxLines = 8;
    static constexpr float kDwellSecs = 10.f;
    static constexpr float kFadeSecs = 2.f;
    static constexpr float kLifeSecs = kDwellSecs + kFadeSecs;

    void setClock(const fl::IClock& clock) noexcept {
        m_clock = &clock;
    }

    // ── Display ──────────────────────────────────────────────────────────────────────────────────
    // Append a routed line. `sender` empty (or system=true) renders without the "name:" prefix. A line
    // from a locally muted callsign is dropped.
    void pushLine(std::string_view sender, std::string_view text, ChatChannel channel, bool system = false);
    [[nodiscard]] std::span<const HudElement> buildElements();
    void clear() noexcept;

    // Local, client-side mute by callsign (not peer id — a callsign is what the player sees).
    void muteCallsign(std::string_view callsign) {
        m_muted.emplace(callsign);
    }
    void unmuteCallsign(std::string_view callsign) {
        m_muted.erase(std::string(callsign));
    }
    [[nodiscard]] bool isMuted(std::string_view callsign) const {
        return m_muted.count(std::string(callsign)) != 0;
    }

    // ── Input ────────────────────────────────────────────────────────────────────────────────────
    void open(ChatChannel channel) noexcept {
        m_inputOpen = true;
        m_channel = channel;
        m_buf[0] = '\0';
    }
    [[nodiscard]] bool isInputOpen() const noexcept {
        return m_inputOpen;
    }
    [[nodiscard]] ChatChannel channel() const noexcept {
        return m_channel;
    }
    [[nodiscard]] std::string_view text() const noexcept {
        return m_buf;
    }
    // Emit the input box through the GUI. Returns true if the user pressed Send this frame (the caller
    // then reads text() and calls submit()). No-op returning false when the box is closed or gui is null.
    bool renderInput(IGui* gui);
    // Close the box, keeping/clearing the buffer. submit() is called after the caller consumed text().
    void submit() noexcept {
        m_inputOpen = false;
        m_buf[0] = '\0';
    }
    void cancel() noexcept {
        m_inputOpen = false;
        m_buf[0] = '\0';
    }

  private:
    struct Line {
        std::array<char, 256> buf{};
        std::chrono::steady_clock::time_point spawn{};
        float r{1.f}, g{1.f}, b{1.f};
    };

    const fl::IClock* m_clock{&fl::SystemClock::instance()};
    std::vector<Line> m_lines; // oldest-first, capped at kMaxLines
    std::array<HudElement, kMaxLines> m_elems{};
    std::unordered_set<std::string> m_muted;

    bool m_inputOpen{false};
    ChatChannel m_channel{ChatChannel::All};
    char m_buf[kMaxChatBytes + 1]{};
};

} // namespace fl
