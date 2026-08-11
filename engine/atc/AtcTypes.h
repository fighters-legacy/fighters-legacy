// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <string>

// Air-traffic-control shared types (#702, epic #673). The ATC layer is a pure deterministic FSM —
// no model involvement anywhere (docs/developer/ai-architecture.md), and this is the CI-tested path. Every
// transmission is a stable, enumerable phrase so the AI-voice ATC epic (#591/#936) can bind TTS to
// it 1:1 without touching this logic.
namespace fl::atc {

// Per-flight clearance state, as the facility sees it. Polled by the AI departure/arrival
// compositions (AtcBehaviors) via AtcService::clearanceState(), and reported by `atc_status`.
enum class ClearanceState : uint8_t {
    None = 0,       // not known to ATC / no clearance
    HoldShort,      // holding short of the runway, awaiting a takeoff clearance
    ClearedTakeoff, // cleared to take the runway and depart
    Departed,       // airborne and clear of the runway
    Inbound,        // declared inbound, sequencing for the approach
    Pattern,        // in the pattern, awaiting a landing clearance (or told to go around)
    ClearedToLand,  // cleared to land
    GoAround,       // wave-off: runway not available at short final
    Landed,         // down, stopped and retired from the arrival sequence (terminal; see below)
};

// Landed and Departed are TERMINAL: the facility has finished with the flight and stops sequencing
// it. They persist until the entity dies or the pilot makes a new request, so a caller polling
// clearanceState() can actually observe them — unlike the live states, which the FSM overwrites as
// the flight progresses. A new requestTakeoff() clears a terminal state (#1149).

[[nodiscard]] const char* clearanceStateName(ClearanceState s) noexcept;

// The reference pose of a facility's field: origin (world) + a heading. Constant for a static
// airport; a carrier entity supplies a moving pose later through the same std::function seam (#38),
// so naval recovery uses the identical ATC path — the FA "accepts landings" lesson.
struct FacilityPose {
    glm::dvec3 origin{0.0};
    float headingDeg{0.f};
};

// The enumerable phrase vocabulary. The deterministic service picks a phrase; the text is a
// compiled-in default (a content pack / the TTS service can re-voice by voiceKey). Keeping this an
// enum — not free-form strings — is what lets Epic O bind voices 1:1 and lets tests assert by value.
enum class AtcPhrase : uint8_t {
    HoldShort,       // "hold short, traffic on the runway"
    ClearedTakeoff,  // "cleared for takeoff"
    ClearedToLand,   // "cleared to land"
    GoAround,        // "go around, runway occupied"
    ContactApproach, // "radar contact, continue inbound" (inbound acknowledgement)
    Roger,           // "roger" (generic acknowledgement — request received / cancelled)
    Unable,          // "unable" (request refused — no runway / not sequenced / no ATC)
    TaxiToParking,   // "clear of the runway, taxi to parking" (the landing is complete)
};

[[nodiscard]] const char* atcPhraseVoiceKey(AtcPhrase p) noexcept;

// One radio line the server pushes toward clients (routed through the #499 radio-net + #704 subtitle
// path — never bespoke audio). target invalid = broadcast to everyone in range; otherwise unicast.
struct RadioTransmission {
    fl::EntityId target{}; // invalid (gen 0) = broadcast
    AtcPhrase phrase{AtcPhrase::HoldShort};
    std::string speaker;        // e.g. "Riverside Tower"
    std::string text;           // rendered clearance line (localizable; the client may re-render)
    std::string voiceKey;       // stable key for TTS / a pack OGG (atcPhraseVoiceKey)
    uint16_t displaySeconds{6}; // subtitle dwell
};

// Build a transmission for a phrase directed at `target` from `speaker` (facility callsign).
[[nodiscard]] RadioTransmission makeTransmission(AtcPhrase phrase, const std::string& speaker, fl::EntityId target);

} // namespace fl::atc
