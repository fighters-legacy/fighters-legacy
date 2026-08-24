// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/WingmanPhraseMatch.h"
#include "util/Str.h" // the one ASCII case rule (#1265)

#include <array>
#include <cctype>
#include <string>

namespace fl::ai {

namespace {

// One keyword and what matching it is worth. Weights are integers and small on purpose: the scale is
// "how much does this phrase identify THIS command", not a probability, and keeping it integral is
// what makes the result bit-identical everywhere.
//
//   3 = the command's own distinctive verb ("rejoin", "disengage")
//   2 = a strong cue that could belong to a neighbour ("engage", "attack")
//   1 = a supporting word that only counts alongside something else ("me", "home")
struct Cue {
    std::string_view text;
    int weight;
};

// Phrasings drawn from how pilots actually say these, INCLUDING the ways an ASR pass mangles them:
// "in gauge" for "engage", "band its" for "bandits", "are tee bee" for "rtb". Those are not
// decoration — they are the cases that make a CPU-only voice tier usable instead of a demo.
constexpr std::array<Cue, 7> kAttackMyTarget{{
    {"my target", 3},
    {"my bandit", 3},
    {"designated", 3},
    // "locked" and "painting" each mean "the one I have designated" on their own — a pilot saying
    // "hit the guy I'm painting" has named a target as precisely as one saying "my target".
    {"locked", 3},
    {"painting", 3},
    {"my contact", 3},
    {"this guy", 2},
}};

constexpr std::array<Cue, 8> kEngageBandits{{
    {"bandits", 3},
    {"band its", 3}, // ASR split
    {"in gauge", 2}, // ASR split of "engage"
    {"engage", 2},
    {"weapons free", 3},
    {"at will", 3},
    {"take them", 2},
    {"fight", 1},
}};

constexpr std::array<Cue, 7> kRejoin{{
    {"rejoin", 4},
    {"re join", 4},
    {"form up", 4},
    {"formup", 4},
    {"formation", 3},
    {"on my wing", 3},
    {"get back", 2},
}};

constexpr std::array<Cue, 7> kCoverMe{{
    {"cover me", 4},
    {"cover", 2},
    {"watch my", 3},
    {"my six", 3},
    {"defend me", 4},
    {"protect me", 4},
    {"help me", 3},
}};

constexpr std::array<Cue, 7> kHoldFire{{
    {"hold fire", 4},
    {"hold your fire", 4},
    {"cease fire", 4},
    {"weapons hold", 4},
    {"weapons tight", 4},
    {"do not fire", 4},
    {"don't shoot", 4},
}};

constexpr std::array<Cue, 8> kReturnToBase{{
    {"return to base", 4},
    {"rtb", 4},
    {"are tee bee", 4}, // ASR spelling-out
    {"go home", 3},
    {"fly home", 3},
    {"bug out", 3},
    {"disengage", 3},
    {"head back", 3},
}};

[[nodiscard]] std::string normalise(std::string_view s) {
    // Lowercase, collapse everything that is not a letter or digit to a single space. Punctuation
    // and casing carry no meaning in a transcript, and collapsing them means "Two, engage!" and
    // "two engage" score identically.
    std::string out;
    out.reserve(s.size() + 2);
    out += ' '; // sentinels, so a leading/trailing cue still sits on a word boundary
    bool lastWasSpace = true;
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            out += asciiToLower(c);
            lastWasSpace = false;
        } else if (!lastWasSpace) {
            out += ' ';
            lastWasSpace = true;
        }
    }
    if (!lastWasSpace)
        out += ' ';
    else if (out.size() == 1)
        return {}; // nothing but separators
    return out;
}

// Whole-phrase containment on the normalised, space-padded string. Padding the cue the same way is
// what stops "cover" matching inside "undercover" and "hold" inside "household".
[[nodiscard]] std::size_t findPhrase(const std::string& haystack, std::string_view cue, std::size_t from) {
    std::string padded;
    padded.reserve(cue.size() + 2);
    padded += ' ';
    padded += cue;
    padded += ' ';
    return haystack.find(padded, from);
}

// Score the INDEPENDENT evidence in a transcript, not every spelling of the same evidence.
//
// The subtlety that matters: cue lists contain nested phrases — "cover me" and "cover", "hold fire"
// and "weapons hold". Counting both on one utterance inflates the score for saying one thing, which
// then beats a genuinely competing command by a margin that was never real. So a matched cue MASKS
// the words it consumed, longest cue first, and a shorter overlapping cue can no longer claim them.
//
// A transcript that really does contain two separate cues ("bug out, go home") still scores both,
// because they occupy different words.
template <std::size_t N> [[nodiscard]] int scoreCues(const std::string& text, const std::array<Cue, N>& cues) {
    // Longest first. Indices rather than a copy of the array; N is at most 8, so the insertion sort
    // is free and there is no allocation on a path that runs six times per transcript.
    std::array<std::size_t, N> order{};
    for (std::size_t i = 0; i < N; ++i)
        order[i] = i;
    for (std::size_t i = 1; i < N; ++i) {
        const std::size_t key = order[i];
        std::size_t j = i;
        while (j > 0 && cues[order[j - 1]].text.size() < cues[key].text.size()) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = key;
    }

    std::string working = text; // masked as cues consume words
    int total = 0;
    for (std::size_t idx : order) {
        const Cue& c = cues[idx];
        bool matched = false;
        std::size_t at = 0;
        while ((at = findPhrase(working, c.text, at)) != std::string::npos) {
            matched = true;
            // Blank the cue's words but keep the surrounding spaces, so neighbouring cues still see
            // intact word boundaries.
            for (std::size_t k = at + 1; k < at + 1 + c.text.size(); ++k)
                working[k] = ' ';
            at += 1;
        }
        if (matched)
            total += c.weight;
    }
    return total;
}

[[nodiscard]] int scoreNormalised(const std::string& text, WingmanCommand cmd) {
    switch (cmd) {
    case WingmanCommand::AttackMyTarget:
        return scoreCues(text, kAttackMyTarget);
    case WingmanCommand::EngageBandits:
        return scoreCues(text, kEngageBandits);
    case WingmanCommand::Rejoin:
        return scoreCues(text, kRejoin);
    case WingmanCommand::CoverMe:
        return scoreCues(text, kCoverMe);
    case WingmanCommand::HoldFire:
        return scoreCues(text, kHoldFire);
    case WingmanCommand::ReturnToBase:
        return scoreCues(text, kReturnToBase);
    case WingmanCommand::Count:
        break;
    }
    return 0;
}

} // namespace

int scoreWingmanPhrase(std::string_view transcript, WingmanCommand cmd) noexcept {
    if (transcript.empty() || transcript.size() > kMaxTranscriptBytes)
        return 0;
    return scoreNormalised(normalise(transcript), cmd);
}

std::optional<PhraseMatch> bestWingmanPhrase(std::string_view transcript) noexcept {
    if (transcript.empty() || transcript.size() > kMaxTranscriptBytes)
        return std::nullopt;
    const std::string text = normalise(transcript);
    if (text.empty())
        return std::nullopt;

    PhraseMatch best;
    best.score = -1;
    for (std::size_t i = 0; i < kWingmanCommandCount; ++i) {
        const auto cmd = static_cast<WingmanCommand>(i);
        const int s = scoreNormalised(text, cmd);
        if (s > best.score) {
            best.runnerUp = best.score < 0 ? 0 : best.score;
            best.score = s;
            best.command = cmd;
        } else if (s > best.runnerUp) {
            best.runnerUp = s;
        }
    }
    if (best.score < 0)
        return std::nullopt;
    return best;
}

std::optional<WingmanCommand> matchWingmanPhrase(std::string_view transcript) noexcept {
    const auto m = bestWingmanPhrase(transcript);
    if (!m)
        return std::nullopt;
    if (m->score < kMinPhraseScore)
        return std::nullopt;
    // Ambiguity is a decline, not a coin flip. "Engage" is a cue for two different commands, and
    // ordering the wrong one because a transcript landed between them is worse than not hearing it —
    // the radio menu is always right there.
    if (m->score - m->runnerUp < kMinPhraseMargin)
        return std::nullopt;
    return m->command;
}

} // namespace fl::ai
