// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/WingmanPhraseMatch.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace fl::ai;

// Deterministic phrase matching for the voice tier (#935).
//
// No model anywhere — that is the tier's whole reason to exist. Every case below is a transcript a
// real ASR pass could produce, including the ways it mangles things, because a matcher that only
// handles clean text would be a demo rather than a feature.

TEST_CASE("phrase: a clean utterance of each command matches it", "[phrase]") {
    CHECK(matchWingmanPhrase("two, attack my target") == WingmanCommand::AttackMyTarget);
    CHECK(matchWingmanPhrase("engage bandits") == WingmanCommand::EngageBandits);
    CHECK(matchWingmanPhrase("two, rejoin") == WingmanCommand::Rejoin);
    CHECK(matchWingmanPhrase("cover me") == WingmanCommand::CoverMe);
    CHECK(matchWingmanPhrase("hold fire") == WingmanCommand::HoldFire);
    CHECK(matchWingmanPhrase("return to base") == WingmanCommand::ReturnToBase);
}

TEST_CASE("phrase: natural phrasings match", "[phrase]") {
    CHECK(matchWingmanPhrase("hit the guy I'm painting") == WingmanCommand::AttackMyTarget);
    CHECK(matchWingmanPhrase("weapons free, take them") == WingmanCommand::EngageBandits);
    CHECK(matchWingmanPhrase("two, form up on my wing") == WingmanCommand::Rejoin);
    CHECK(matchWingmanPhrase("watch my six") == WingmanCommand::CoverMe);
    CHECK(matchWingmanPhrase("weapons tight, do not fire") == WingmanCommand::HoldFire);
    CHECK(matchWingmanPhrase("bug out, go home") == WingmanCommand::ReturnToBase);
}

TEST_CASE("phrase: ASR mangling still matches", "[phrase]") {
    // The cases that make this usable rather than a demo. An ASR pass splits compounds and spells
    // out initialisms, and #611 deliberately refuses near-misses because there the noise is a model
    // getting a name wrong — here it is acoustic, and refusing would measure nothing.
    CHECK(matchWingmanPhrase("in gauge band its") == WingmanCommand::EngageBandits);
    CHECK(matchWingmanPhrase("two re join") == WingmanCommand::Rejoin);
    CHECK(matchWingmanPhrase("are tee bee") == WingmanCommand::ReturnToBase);
    CHECK(matchWingmanPhrase("form up") == WingmanCommand::Rejoin);
}

TEST_CASE("phrase: punctuation and case are irrelevant", "[phrase]") {
    CHECK(matchWingmanPhrase("COVER ME!!!") == WingmanCommand::CoverMe);
    CHECK(matchWingmanPhrase("  ...hold   fire...  ") == WingmanCommand::HoldFire);
    CHECK(matchWingmanPhrase("Two-- REJOIN.") == WingmanCommand::Rejoin);
}

// ── declining ───────────────────────────────────────────────────────────────────────────────────

TEST_CASE("phrase: unrelated speech is declined", "[phrase]") {
    // Ordering a wingman because someone said something that rhymed with a command is worse than
    // not hearing them; the radio menu is always right there.
    CHECK_FALSE(matchWingmanPhrase("nice shot").has_value());
    CHECK_FALSE(matchWingmanPhrase("where is everyone").has_value());
    CHECK_FALSE(matchWingmanPhrase("uh").has_value());
}

TEST_CASE("phrase: empty and over-long transcripts are declined", "[phrase]") {
    CHECK_FALSE(matchWingmanPhrase("").has_value());
    CHECK_FALSE(matchWingmanPhrase("   ").has_value());
    CHECK_FALSE(matchWingmanPhrase("...,,,!!!").has_value()); // separators only
    CHECK_FALSE(matchWingmanPhrase(std::string(kMaxTranscriptBytes + 1, 'a')).has_value());
}

TEST_CASE("phrase: a single weak cue does not clear the threshold", "[phrase]") {
    // "fight" is worth 1. One supporting word is not an order.
    CHECK_FALSE(matchWingmanPhrase("good fight").has_value());
}

TEST_CASE("phrase: an ambiguous transcript is declined rather than guessed", "[phrase]") {
    // A transcript that scores near-equally on two commands is genuinely ambiguous, and picking one
    // would be a coin flip with a pilot's authority behind it.
    const auto m = bestWingmanPhrase("hold fire and cover me");
    REQUIRE(m.has_value());
    CHECK(m->score - m->runnerUp < kMinPhraseMargin);
    CHECK_FALSE(matchWingmanPhrase("hold fire and cover me").has_value());
}

TEST_CASE("phrase: word-boundary matching does not fire on substrings", "[phrase]") {
    // "cover" inside "undercover", "hold" inside "household". Padding both sides is what stops it.
    CHECK_FALSE(matchWingmanPhrase("undercover operations").has_value());
    CHECK_FALSE(matchWingmanPhrase("household name").has_value());
}

// ── determinism ─────────────────────────────────────────────────────────────────────────────────

TEST_CASE("phrase: scoring is deterministic and repeatable", "[phrase]") {
    // The property the eval suite depends on to assert outcomes rather than ranges: integer scoring,
    // no RNG, no float, no locale-dependent comparison.
    constexpr const char* kUtterance = "two, in gauge band its";
    const int first = scoreWingmanPhrase(kUtterance, WingmanCommand::EngageBandits);
    for (int i = 0; i < 100; ++i)
        CHECK(scoreWingmanPhrase(kUtterance, WingmanCommand::EngageBandits) == first);
    CHECK(first >= kMinPhraseScore);
}

TEST_CASE("phrase: an unrelated command scores zero on a clean utterance", "[phrase]") {
    CHECK(scoreWingmanPhrase("return to base", WingmanCommand::CoverMe) == 0);
    CHECK(scoreWingmanPhrase("cover me", WingmanCommand::ReturnToBase) == 0);
}

TEST_CASE("phrase: bestWingmanPhrase exposes why something was declined", "[phrase]") {
    // Score and runner-up are returned so a caller can tell "nothing matched" from "two things
    // matched equally" — different problems with different fixes.
    const auto weak = bestWingmanPhrase("good fight");
    REQUIRE(weak.has_value());
    CHECK(weak->score < kMinPhraseScore);

    const auto strong = bestWingmanPhrase("return to base");
    REQUIRE(strong.has_value());
    CHECK(strong->score >= kMinPhraseScore);
    CHECK(strong->command == WingmanCommand::ReturnToBase);
}

TEST_CASE("phrase: every command is reachable by some transcript", "[phrase]") {
    // A command the matcher can never produce would be a silently dead menu entry on this path.
    bool reached[kWingmanCommandCount] = {};
    for (const char* t : {"attack my target", "engage bandits", "rejoin", "cover me", "hold fire", "return to base"})
        if (const auto c = matchWingmanPhrase(t))
            reached[static_cast<std::size_t>(*c)] = true;
    for (bool r : reached)
        CHECK(r);
}
