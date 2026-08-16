// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LSO and crew-chief voice keys (#1108).
//
// Before this, both surfaces built their RadioTransmission by hand at the call site and left
// voiceKey empty, so a content pack could voice the tower and nothing else — the wire field, the
// client resolver and the subtitle fallback all existed already and simply had nothing to carry.
//
// These tests exist because a voice key is a PUBLISHED NAME: a pack records an OGG at
// radio/<key>, so renaming one silently breaks every pack that voiced it. Pinning the strings is
// the point, not an implementation detail.

#include "atc/CrewPhrases.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <string_view>

using namespace fl::atc;

namespace {

constexpr LsoPhrase kAllLso[] = {
    LsoPhrase::OnGlideslope, LsoPhrase::High,    LsoPhrase::Low,      LsoPhrase::Fast,
    LsoPhrase::Slow,         LsoPhrase::WaveOff, LsoPhrase::GoodTrap,
};

constexpr CrewChiefPhrase kAllCrewChief[] = {
    CrewChiefPhrase::SayAgain, CrewChiefPhrase::NoAircraft, CrewChiefPhrase::ShutDownFirst, CrewChiefPhrase::NoBase,
    CrewChiefPhrase::Refueled, CrewChiefPhrase::Rearmed,    CrewChiefPhrase::Repaired,
};

} // namespace

TEST_CASE("LSO voice keys are stable, namespaced and unique (#1108)", "[voicekey][lso]") {
    CHECK(std::string_view(lsoPhraseVoiceKey(LsoPhrase::OnGlideslope)) == "lso.on_glideslope");
    CHECK(std::string_view(lsoPhraseVoiceKey(LsoPhrase::High)) == "lso.high");
    CHECK(std::string_view(lsoPhraseVoiceKey(LsoPhrase::Low)) == "lso.low");
    CHECK(std::string_view(lsoPhraseVoiceKey(LsoPhrase::Fast)) == "lso.fast");
    CHECK(std::string_view(lsoPhraseVoiceKey(LsoPhrase::Slow)) == "lso.slow");
    CHECK(std::string_view(lsoPhraseVoiceKey(LsoPhrase::WaveOff)) == "lso.wave_off");
    CHECK(std::string_view(lsoPhraseVoiceKey(LsoPhrase::GoodTrap)) == "lso.good_trap");

    std::set<std::string> keys;
    for (LsoPhrase p : kAllLso) {
        const std::string k = lsoPhraseVoiceKey(p);
        INFO("key " << k);
        CHECK(k.rfind("lso.", 0) == 0);
        // MsgRadioTransmission::voiceKey is char[32] and buildRadioWire truncates SILENTLY.
        CHECK(k.size() < 32u);
        CHECK(keys.insert(k).second); // a duplicate would make two calls unvoiceable apart
        CHECK_FALSE(std::string(lsoPhraseText(p)).empty());
    }
    CHECK(keys.size() == 7u);
}

TEST_CASE("crew-chief voice keys are stable, namespaced and unique (#1108)", "[voicekey][crew]") {
    CHECK(std::string_view(crewChiefPhraseVoiceKey(CrewChiefPhrase::SayAgain)) == "crew.say_again");
    CHECK(std::string_view(crewChiefPhraseVoiceKey(CrewChiefPhrase::NoAircraft)) == "crew.no_aircraft");
    CHECK(std::string_view(crewChiefPhraseVoiceKey(CrewChiefPhrase::ShutDownFirst)) == "crew.shut_down_first");
    CHECK(std::string_view(crewChiefPhraseVoiceKey(CrewChiefPhrase::NoBase)) == "crew.no_base");
    CHECK(std::string_view(crewChiefPhraseVoiceKey(CrewChiefPhrase::Refueled)) == "crew.refueled");
    CHECK(std::string_view(crewChiefPhraseVoiceKey(CrewChiefPhrase::Rearmed)) == "crew.rearmed");
    CHECK(std::string_view(crewChiefPhraseVoiceKey(CrewChiefPhrase::Repaired)) == "crew.repaired");

    std::set<std::string> keys;
    for (CrewChiefPhrase p : kAllCrewChief) {
        const std::string k = crewChiefPhraseVoiceKey(p);
        INFO("key " << k);
        CHECK(k.rfind("crew.", 0) == 0);
        CHECK(k.size() < 32u);
        CHECK(keys.insert(k).second);
        CHECK_FALSE(std::string(crewChiefPhraseText(p)).empty());
    }
    CHECK(keys.size() == 7u);
}

TEST_CASE("a built LSO transmission carries its key, text and speaker (#1108)", "[voicekey][lso]") {
    // THE REGRESSION THIS FIXES: the old call sites populated text and left voiceKey empty, and the
    // client treats an empty key as "subtitle only" — so the line was unvoiceable no matter what a
    // pack shipped. Going through the builder is what makes forgetting it impossible.
    const fl::EntityId target{7, 1};
    for (LsoPhrase p : kAllLso) {
        const RadioTransmission t = makeLsoTransmission(p, target);
        INFO("phrase ordinal " << static_cast<int>(p));
        CHECK_FALSE(t.voiceKey.empty());
        CHECK(t.voiceKey == lsoPhraseVoiceKey(p));
        CHECK(t.text == lsoPhraseText(p));
        CHECK(t.speaker == "Paddles");
        CHECK(t.target.index == target.index);
        CHECK(t.displaySeconds > 0);
    }
}

TEST_CASE("a built crew-chief transmission carries its key, text and speaker (#1108)", "[voicekey][crew]") {
    const fl::EntityId target{3, 2};
    for (CrewChiefPhrase p : kAllCrewChief) {
        const RadioTransmission t = makeCrewChiefTransmission(p, target);
        INFO("phrase ordinal " << static_cast<int>(p));
        CHECK_FALSE(t.voiceKey.empty());
        CHECK(t.voiceKey == crewChiefPhraseVoiceKey(p));
        CHECK(t.text == crewChiefPhraseText(p));
        CHECK(t.speaker == "Crew chief");
        CHECK(t.target.index == target.index);
        CHECK(t.displaySeconds > 0);
    }
}

TEST_CASE("crew voice keys do not collide with the tower's (#1108)", "[voicekey]") {
    // All three namespaces resolve through the same `radio/<key>` lookup, so a collision would make
    // one surface silently play another's line.
    std::set<std::string> all;
    for (LsoPhrase p : kAllLso)
        CHECK(all.insert(lsoPhraseVoiceKey(p)).second);
    for (CrewChiefPhrase p : kAllCrewChief)
        CHECK(all.insert(crewChiefPhraseVoiceKey(p)).second);
    for (auto p : {AtcPhrase::HoldShort, AtcPhrase::ClearedTakeoff, AtcPhrase::ClearedToLand, AtcPhrase::GoAround,
                   AtcPhrase::ContactApproach, AtcPhrase::Roger, AtcPhrase::Unable, AtcPhrase::TaxiToParking})
        CHECK(all.insert(atcPhraseVoiceKey(p)).second);
    CHECK(all.size() == 22u); // 7 LSO + 7 crew chief + 8 ATC
}
