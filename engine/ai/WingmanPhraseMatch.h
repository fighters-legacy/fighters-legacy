// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ai/WingmanCommand.h"

#include <cstdint>
#include <optional>
#include <string_view>

// Deterministic phrase matching for the voice-command tier (#935).
//
// A speech transcript becomes one of the six scripted wingman commands with NO LLM anywhere. That is
// the whole point of this tier: it is CPU-viable, independent of the #769 GPU decision, and it works
// on every server — including the ones that will never run a model. The LLM intent tier (#611)
// layers on top where a provider exists; this is what is underneath it.
//
// ── WHY FUZZY HERE AND NOT IN #611 ───────────────────────────────────────────────────────────────
//
// #611 refuses a near-miss on a command name, because a model was handed the vocabulary verbatim and
// getting it wrong is a model problem worth seeing. Here the noise is ACOUSTIC: an ASR pass turns
// "engage bandits" into "engage band its" or "in gauge bandits", and refusing those would make the
// feature useless while measuring nothing. So this scores phrases, not names.
//
// ── DETERMINISM ──────────────────────────────────────────────────────────────────────────────────
//
// Integer scoring, no RNG, no floating point, no locale-dependent comparison. The same transcript
// produces the same command on every machine and in every build — the property every scoring path in
// this codebase holds (Detection.h's rollPasses, the turbulence hash), and the reason the eval suite
// can assert on outcomes rather than on ranges.
//
// ── DECLINING ────────────────────────────────────────────────────────────────────────────────────
//
// Below threshold, or ambiguous between two commands, the answer is nullopt. There is no "best
// guess": ordering a wingman to break off because someone said something that rhymed with it is
// worse than not hearing them, and the radio menu is always right there.

namespace fl::ai {

struct PhraseMatch {
    WingmanCommand command{WingmanCommand::Rejoin};
    int score{0};    // higher is better; kMinScore is the acceptance floor
    int runnerUp{0}; // best score among the OTHER commands, for the ambiguity check
};

// Acceptance floor, in the same integer units the scorer emits (one point per matched keyword,
// weighted by how strongly that keyword identifies its command). Tuned so a clean transcript of any
// of the six commands clears it comfortably and a sentence merely containing one weak cue does not.
inline constexpr int kMinPhraseScore = 3;

// A match must beat its runner-up by this much. "Engage" appears in both engage_bandits and
// attack_my_target's phrasings, so a transcript that scores near-equally on two commands is
// genuinely ambiguous and declining is the honest answer.
inline constexpr int kMinPhraseMargin = 2;

// Longest transcript considered. An ASR pass handed a long stretch of open-mic audio produces
// something a phrase matcher should not try to interpret as a single order.
inline constexpr std::size_t kMaxTranscriptBytes = 512;

// Score a transcript against one command. Exposed for the eval harness and for tests that want to
// assert on the shape of the scoring rather than only on its outcome.
[[nodiscard]] int scoreWingmanPhrase(std::string_view transcript, WingmanCommand cmd) noexcept;

// The best match, with its score and runner-up, or nullopt when the transcript is empty/over-long.
// Does NOT apply the threshold — matchWingmanPhrase does. Use this when you want to see why a
// transcript was declined.
[[nodiscard]] std::optional<PhraseMatch> bestWingmanPhrase(std::string_view transcript) noexcept;

// The command a transcript orders, or nullopt when it is below threshold, ambiguous, empty, or too
// long. THIS is the function the voice path calls; the ordinal it returns goes through the same
// WorldBroadcaster::issueWingmanOrder the radio menu and the wire drive.
[[nodiscard]] std::optional<WingmanCommand> matchWingmanPhrase(std::string_view transcript) noexcept;

} // namespace fl::ai
