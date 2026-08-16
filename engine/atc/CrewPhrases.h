// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "atc/AtcTypes.h"

#include <string>

namespace fl::atc {

// The LSO and crew-chief phrase vocabularies (#1108).
//
// These lines existed before this header did, as string literals at their call sites — which meant
// they carried no voiceKey, so a content pack could not voice them the way it can voice ATC. The
// wire has always had room for the key (MsgRadioTransmission::voiceKey), and the client has always
// resolved a non-empty one to the audio asset `radio/<voiceKey>`; nothing was populating it.
//
// Enumerating them is what makes them packageable, exactly as AtcPhrase does for the tower: the key
// is the stable name a pack records an OGG against, and an enum (rather than free-form text) is what
// lets a test assert by value and a pack ship a complete set without guessing.
//
// ⚠ A voiceKey is a PUBLISHED NAME. Renaming one silently breaks every pack that recorded audio
// against it, and the wire field is 32 bytes, so a key must stay under 31 characters.

// The LSO ("Paddles") talking an aircraft down onto the deck.
enum class LsoPhrase : uint8_t {
    OnGlideslope, // "on glideslope, on speed"
    High,         // above the glideslope
    Low,          // below it — the dangerous one
    Fast,         // above approach speed
    Slow,         // below it
    WaveOff,      // go around, and it is never suppressed as a repeat
    GoodTrap,     // the wire is caught: the recovery is complete
};

// The crew chief, who answers ground service requests on the ramp or the deck.
enum class CrewChiefPhrase : uint8_t {
    SayAgain,      // the request did not parse
    NoAircraft,    // the pilot has no live aircraft
    ShutDownFirst, // still rolling or airborne
    NoBase,        // not at a base or a deck
    Refueled,
    Rearmed,
    Repaired,
};

[[nodiscard]] const char* lsoPhraseVoiceKey(LsoPhrase p) noexcept;
[[nodiscard]] const char* lsoPhraseText(LsoPhrase p) noexcept;
[[nodiscard]] const char* crewChiefPhraseVoiceKey(CrewChiefPhrase p) noexcept;
[[nodiscard]] const char* crewChiefPhraseText(CrewChiefPhrase p) noexcept;

// Build a transmission the same way makeTransmission does for ATC, so a caller cannot populate the
// text and forget the key — which is the entire defect this fixes.
[[nodiscard]] RadioTransmission makeLsoTransmission(LsoPhrase phrase, fl::EntityId target);
[[nodiscard]] RadioTransmission makeCrewChiefTransmission(CrewChiefPhrase phrase, fl::EntityId target);

} // namespace fl::atc
