// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

// ---------------------------------------------------------------------------------------------
// The radio-net model (Epic J, #499)
// ---------------------------------------------------------------------------------------------
// Voice in a combat flight sim is RADIO, not lobby chat. The unit of routing is a NET: a named
// channel with a membership rule and a presentation profile. Nets are DATA (server config), not
// code — a theater pack or a server operator adds "tanker" or "awacs" without an engine change.
//
// Deliberately NOT modelled: a frequency dial. Tuning 251.000 to hear the tanker is ceremony, not
// gameplay, and it fights the arcade-to-sim pillar (a new player cannot discover it). Named nets
// are what SRS's frequency simulation is actually USED for, with none of its setup cost.
//
// A net id on the wire is its INDEX in the server's table (like factionIndex), so the string ids
// below never travel per frame. The table is sent once after ConnectAck (MsgVoiceNetDef).
//
// The server NEVER decodes audio: it routes opaque Opus frames to the recipient set a net's kind
// resolves to. That is what keeps voice near-zero server CPU at 128 players.
// ---------------------------------------------------------------------------------------------

// How a net's recipient set is computed, server-side, per transmission.
enum class RadioNetKind : uint8_t {
    Global = 0,    // everyone admitted on the server
    Team = 1,      // same faction as the speaker (a teamless observer hears only its own echo)
    Flight = 2,    // the speaker's formation (element/flight/package subtree), #610's command tree
    Proximity = 3, // every peer whose aircraft is within rangeM of the speaker's, regardless of side
    Atc = 4,       // the ATC-common net: humans plus the synthetic ATC/AWACS voice (#673/#704)
};

inline constexpr uint8_t kMaxRadioNetKind = static_cast<uint8_t>(RadioNetKind::Atc);

// Validate an attacker-supplied kind byte before casting (the isPeerRoleOrdinal idiom).
[[nodiscard]] inline bool isRadioNetKindOrdinal(uint8_t v) noexcept {
    return v <= kMaxRadioNetKind;
}

[[nodiscard]] const char* radioNetKindName(RadioNetKind kind) noexcept;

// Parse a config string ("team", "proximity", ...). Returns false on an unknown name.
[[nodiscard]] bool radioNetKindFromString(std::string_view s, RadioNetKind& out) noexcept;

// A net table is bounded so a peer's PTT selector, the wire record count, and the client's per-net
// mixer state are all statically sized. Eight nets is more than any real radio stack presents.
inline constexpr std::size_t kMaxRadioNets = 8;

// Sentinel for "no net" / "not a member" (also the wire value for an unrouteable frame).
inline constexpr uint8_t kInvalidRadioNet = 0xFFu;

// Wire-side string caps (see MsgVoiceNetRecord); the parser truncates to these.
inline constexpr std::size_t kMaxRadioNetIdChars = 23;
inline constexpr std::size_t kMaxRadioNetNameChars = 31;

// One radio net. Authored server-side ([[voice.nets]] in server.toml), replicated to clients.
struct RadioNetDef {
    std::string id;   // stable machine id, e.g. "team" — what config and admin commands name
    std::string name; // display label, e.g. "TEAM" — what the HUD and the PTT selector show
    RadioNetKind kind{RadioNetKind::Team};

    // Presentation (#925). The server carries these so every client on a server hears the same
    // radio, and a pack/operator can make one net sound different from another.
    bool positional{false}; // mix at the speaker's world position rather than head-locked
    float rangeM{0.f};      // Proximity: audible radius. Positional mix: rolloff ceiling. 0 = unlimited
    bool radioEffect{true}; // apply the radio DSP (bandpass + compression + squelch); false = "in the room"
    float gain{1.f};        // per-net trim, on top of the client's own per-net slider
    bool defaultNet{false}; // pre-selected under the primary PTT key when a client first connects
};

// The ordered net table. The index of a def IS its wire netId, so order is significant and stable
// for the life of a server session.
class RadioNetTable {
  public:
    void clear() noexcept {
        m_nets.clear();
    }

    // Appends a net. Returns its index, or kInvalidRadioNet when the table is full or `def.id` is
    // empty/duplicated (ids must be unique — config names them and admin commands address them).
    uint8_t add(RadioNetDef def);

    [[nodiscard]] const RadioNetDef* byIndex(uint8_t netId) const noexcept;
    [[nodiscard]] uint8_t indexOf(std::string_view id) const noexcept;

    // The net a fresh client selects under the primary PTT: the first def flagged defaultNet, else
    // the first Team net, else index 0. kInvalidRadioNet for an empty table.
    [[nodiscard]] uint8_t defaultIndex() const noexcept;

    [[nodiscard]] std::size_t size() const noexcept {
        return m_nets.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return m_nets.empty();
    }
    [[nodiscard]] const std::vector<RadioNetDef>& nets() const noexcept {
        return m_nets;
    }

  private:
    std::vector<RadioNetDef> m_nets;
};

// The compiled-in default net stack, so voice works with ZERO server configuration (the
// builtinAirfield()/builtinDefaultPlaylist() precedent): TEAM (default PTT), FLIGHT, ATC, and a
// positional PROXIMITY net at 3 km. An operator's [[voice.nets]] replaces this table wholesale.
[[nodiscard]] std::vector<RadioNetDef> builtinRadioNets();

} // namespace fl
