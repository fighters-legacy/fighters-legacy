// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// The scripted wingman command grammar (#610) — the SINGLE SOURCE OF TRUTH for the vocabulary.
//
// This is the zero-AI half of Epic O and the CI-tested path: with no provider configured (or on a
// CPU-only server, which cannot meet the 2 s intent budget — decision #769, docs/developer/ai-architecture.md
// §9), the wingman is driven entirely by these six commands from the in-game radio menu. It also
// satisfies the Phase 4 acceptance criterion on its own ("wingman follows player and responds to all
// six commands").
//
// The LLM path (Epic O, #591) never bypasses this grammar: a provider maps free text to one of these
// ordinals and the ordinal is executed through the same code the menu drives. That is what bounds the
// blast radius of a prompt injection to "a real command at the wrong time" instead of arbitrary
// actuation.
//
// TWO PROPERTIES ARE LOAD-BEARING AND MUST SURVIVE ANY EDIT:
//
//  1. The grammar is a FLAT, PARAMETERLESS ENUM. Intent latency on CPU is prompt-eval dominated —
//     the cost is ingesting the grammar, not emitting the ~12-token answer — so the vocabulary IS
//     the latency lever (#769). A flat enum is also the shortest possible constrained-decoding
//     alternation, and it lets the eval harness score by string equality with no code change.
//     A command that needs a target does NOT take one from the model: see attack_my_target, whose
//     target is resolved server-side from state the server already owns (ai/Threat.h).
//
//  2. The doc comment on each command below IS the text of the eval system prompt
//     (tools/ai_eval/suites/intent.json). Keep them one short line each. tests/test_ai_eval.py
//     asserts the suite's grammar array matches kWingmanCommandNames, so a rename here fails there.
//
// There is deliberately NO "unknown" ordinal. "unknown" is an eval-suite sentinel meaning "this
// utterance maps to no command" — the intent mapper sends nothing at all in that case. Declining is
// correct and preferred over guessing; it must never be representable as an executable order.

namespace fl::ai {

enum class WingmanCommand : uint8_t {
    // Attack the target the flight lead has designated.
    AttackMyTarget = 0,
    // Engage hostile aircraft at will.
    EngageBandits = 1,
    // Return to formation on the lead.
    Rejoin = 2,
    // Defensive support for the lead.
    CoverMe = 3,
    // Weapons hold, do not fire.
    HoldFire = 4,
    // Disengage and fly home.
    ReturnToBase = 5,

    Count = 6,
};

inline constexpr size_t kWingmanCommandCount = static_cast<size_t>(WingmanCommand::Count);

// Wire/grammar names, indexed by ordinal. These strings are the stable vocabulary: they appear on
// the LLM's grammar allowlist, in the admin `wingman order` command, and in the eval suite. Renaming
// one is a deliberate, breaking vocabulary change, not a refactor.
inline constexpr std::string_view kWingmanCommandNames[kWingmanCommandCount] = {
    "attack_my_target", "engage_bandits", "rejoin", "cover_me", "hold_fire", "return_to_base",
};

[[nodiscard]] inline constexpr std::string_view wingmanCommandName(WingmanCommand cmd) noexcept {
    const auto i = static_cast<size_t>(cmd);
    return i < kWingmanCommandCount ? kWingmanCommandNames[i] : std::string_view{};
}

// Parse a grammar name to its command. Returns nullopt for anything not in the vocabulary —
// including "unknown", which is the mapper's decline sentinel and is NOT an executable order.
[[nodiscard]] inline constexpr std::optional<WingmanCommand> parseWingmanCommand(std::string_view name) noexcept {
    for (size_t i = 0; i < kWingmanCommandCount; ++i) {
        if (kWingmanCommandNames[i] == name) {
            return static_cast<WingmanCommand>(i);
        }
    }
    return std::nullopt;
}

// True when `ordinal` (as carried on the wire) names a real command. The server MUST gate on this
// before casting an attacker-supplied byte to WingmanCommand.
[[nodiscard]] inline constexpr bool isWingmanCommandOrdinal(uint8_t ordinal) noexcept {
    return ordinal < kWingmanCommandCount;
}

// An engage order implies weapons-free: ordering the wingman to attack anything while it is under a
// hold_fire is contradictory, so the engage orders implicitly clear the hold. (The hold itself has
// no teeth until weapons land in #583 — today it means "break off and rejoin", plus a roster flag
// that will gate the AI's firing trigger. Documented in docs/user-guide/controls.md.)
[[nodiscard]] inline constexpr bool clearsWeaponsHold(WingmanCommand cmd) noexcept {
    return cmd == WingmanCommand::AttackMyTarget || cmd == WingmanCommand::EngageBandits;
}

} // namespace fl::ai
