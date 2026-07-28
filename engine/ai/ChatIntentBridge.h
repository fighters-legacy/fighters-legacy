// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ai/WingmanCommand.h"

#include <optional>
#include <string>
#include <string_view>

// The chat-to-intent bridge (#611): free text over team chat becomes ONE scripted wingman command.
//
// ── THE SHAPE, AND WHY IT IS THIS SHAPE ──────────────────────────────────────────────────────────
//
// The model CHOOSES AMONG VALIDATED COMMANDS. It never invents an action, never supplies a target,
// never emits anything the radio menu could not have emitted. The pipeline is:
//
//     player text  ->  templated prompt  ->  model  ->  {"command": "<name>"}
//                                                            |
//                                            schema validation + grammar allowlist
//                                                            |
//                                              fl::ai::WingmanCommand ordinal
//                                                            |
//                              the SAME execution path MsgWingmanCommand drives
//
// Everything after the model is in this header, and all of it is pure. That is deliberate: player
// text is untrusted input, so the defence has to be somewhere a test can aim at it directly, with no
// model in the loop (docs/ai-architecture.md §7 — CI never requires a model).
//
// ── WHAT BOUNDS A PROMPT INJECTION ───────────────────────────────────────────────────────────────
//
// Not the prompt. The prompt template helps, but the load-bearing property is that the ONLY thing
// that can come back is one of six parameterless ordinals, and even a perfectly successful injection
// therefore buys an attacker "a real command at the wrong time" — the same thing they could have
// achieved by pressing a key on the radio menu. `attack_my_target` does not carry a target: the
// target is resolved server-side from state the server already owns.
//
// "unknown" is the decline sentinel. It is deliberately NOT an executable ordinal — see
// WingmanCommand.h — so a model correctly refusing an out-of-grammar utterance cannot, by refusing,
// end up ordering the wingman.

namespace fl::ai {

// Why a response was refused. Reported rather than collapsed into nullopt, because "the model said
// something that is not JSON" and "the model named a command that does not exist" are different
// failures — the first is a broken backend, the second is a model that needs replacing, and an
// operator reading a log should be able to tell which one they have.
enum class IntentRejection : uint8_t {
    None = 0,
    NotJson,      // no {"command": "..."} could be found at all
    MissingField, // JSON-ish, but no "command" member
    NotInGrammar, // a command name the vocabulary does not contain
    Declined,     // the model answered "unknown" — a correct, expected outcome, not an error
    TooLong,      // the response exceeded the bound a six-word answer could possibly need
};

[[nodiscard]] std::string_view intentRejectionName(IntentRejection r) noexcept;

struct IntentResult {
    std::optional<WingmanCommand> command; // engaged only when rejection == None
    IntentRejection rejection{IntentRejection::None};
};

// A model answering with one of six short names cannot need more than this. The bound exists so a
// backend that starts emitting an essay — or a compromised one emitting megabytes — is refused
// before any parsing work happens.
inline constexpr std::size_t kMaxIntentResponseBytes = 512;

// The longest player utterance forwarded to a model. Longer lines are truncated rather than
// rejected: someone typing a paragraph at their wingman meant something by the first sentence, and
// the alternative is a feature that silently ignores long messages. The cap also bounds what an
// attacker can spend of a rate-limited model call.
inline constexpr std::size_t kMaxUtteranceBytes = 240;

// Build the system prompt: the fixed instruction plus the grammar, generated FROM
// kWingmanCommandNames so a command added to the enum cannot be missing from the prompt.
[[nodiscard]] std::string buildIntentSystemPrompt();

// Wrap a player utterance for a model. The text is placed inside an explicit delimiter and labelled
// as data to be classified — the utterance is NEVER concatenated into the instruction, because
// concatenation is how "ignore your instructions" becomes an instruction.
//
// Control characters are flattened, the delimiter's angle-runs are scrubbed, and the result is
// truncated to kMaxUtteranceBytes. BOTH scrubs are needed: flattening newlines alone does not stop
// delimiter forgery, because the tokens are perfectly writable on one line.
[[nodiscard]] std::string buildIntentUserPrompt(std::string_view utterance);

// Validate a model response and map it onto the grammar. Accepts either a bare command name or a
// {"command": "..."} object, because backends differ in whether they honour a JSON instruction and
// refusing the bare form would fail a model that answered correctly.
//
// This is the gate. Nothing downstream re-checks, so it fails closed on everything it does not
// positively recognise.
[[nodiscard]] IntentResult validateIntentResponse(std::string_view response);

// True when a chat line should be offered to the intent mapper at all.
//
// Cheap, local, and deliberately conservative: a rate-limited model call per chat line would make
// the team channel a denial-of-service lever against the server's own budget. A line is a candidate
// only if it plausibly addresses the flight — which also keeps ordinary team chatter out of a model
// that would otherwise be asked to classify every word said in a match.
[[nodiscard]] bool looksLikeWingmanAddress(std::string_view text);

} // namespace fl::ai
