// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wingman brevity voice keys (#1108).
//
// The wingman is the one radio voice whose TEXT is client-side by policy — the server sends a
// result CODE and never a string, because brevity calls have to stay localizable. So its voice keys
// live here rather than beside the LSO's and the crew chief's, and they need the same pinning: a
// key is a published name a content pack records `radio/<key>.ogg` against, and renaming one
// silently breaks every pack that voiced it.
//
// Reaching them through onAck() rather than the key function directly is deliberate — that is the
// path the network handler actually takes, so this also pins that an ack fires the callback at all.

#include "WingmanMenu.h"

#include "ai/WingmanCommand.h"
#include "net/GameProtocol.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using namespace fl;

namespace {

struct Spoken {
    std::string line;
    std::string key;
    int count{0};
};

// Fire one ack through the menu and report what it asked the host to speak.
Spoken speak(uint8_t command, WingmanResult result) {
    WingmanMenu menu;
    Spoken got;
    menu.brevityCallback = [&got](const std::string& line, const char* voiceKey) {
        got.line = line;
        got.key = voiceKey ? voiceKey : "";
        ++got.count;
    };

    MsgWingmanAck ack{};
    ack.command = command;
    ack.result = static_cast<uint8_t>(result);
    ack.flightSize = 2;
    ack.memberIdx = 1;
    menu.onAck(ack);
    return got;
}

constexpr uint8_t cmd(fl::ai::WingmanCommand c) {
    return static_cast<uint8_t>(c);
}

} // namespace

TEST_CASE("an ack speaks its brevity call with a voice key (#1108)", "[voicekey][wingman]") {
    // The regression: before this the wingman had no audio hook of any kind, so a pack could voice
    // the tower and the deck but never the flight.
    const Spoken s = speak(cmd(fl::ai::WingmanCommand::AttackMyTarget), WingmanResult::Acknowledged);
    CHECK(s.count == 1);
    CHECK(s.key == "wingman.engaged");
    CHECK(s.line == "TWO: Engaged.");
}

TEST_CASE("every acknowledged order has its own wingman key (#1108)", "[voicekey][wingman]") {
    const std::pair<fl::ai::WingmanCommand, const char*> expected[] = {
        {fl::ai::WingmanCommand::AttackMyTarget, "wingman.engaged"},
        {fl::ai::WingmanCommand::EngageBandits, "wingman.engaging"},
        {fl::ai::WingmanCommand::Rejoin, "wingman.rejoining"},
        {fl::ai::WingmanCommand::CoverMe, "wingman.covering"},
        {fl::ai::WingmanCommand::HoldFire, "wingman.weapons_hold"},
        {fl::ai::WingmanCommand::ReturnToBase, "wingman.rtb"},
    };
    for (const auto& [c, key] : expected) {
        const Spoken s = speak(cmd(c), WingmanResult::Acknowledged);
        INFO("command ordinal " << static_cast<int>(c));
        CHECK(s.key == key);
        CHECK_FALSE(s.line.empty());
    }
}

TEST_CASE("a relayed order is voiced as the LEAD, not the wingman (#1108)", "[voicekey][wingman]") {
    // Relayed means someone ordered US. It is a different voice saying a different thing, so it must
    // not share the wingman's key — a pack records the two separately.
    const std::pair<fl::ai::WingmanCommand, const char*> expected[] = {
        {fl::ai::WingmanCommand::AttackMyTarget, "lead.attack_my_target"},
        {fl::ai::WingmanCommand::EngageBandits, "lead.engage_bandits"},
        {fl::ai::WingmanCommand::Rejoin, "lead.rejoin"},
        {fl::ai::WingmanCommand::CoverMe, "lead.cover_me"},
        {fl::ai::WingmanCommand::HoldFire, "lead.hold_fire"},
        {fl::ai::WingmanCommand::ReturnToBase, "lead.return_to_base"},
    };
    for (const auto& [c, key] : expected) {
        const Spoken s = speak(cmd(c), WingmanResult::Relayed);
        INFO("command ordinal " << static_cast<int>(c));
        CHECK(s.key == key);
        CHECK(s.line.rfind("LEAD:", 0) == 0);
    }
}

TEST_CASE("every non-acknowledgement outcome is voiced too (#1108)", "[voicekey][wingman]") {
    const std::pair<WingmanResult, const char*> expected[] = {
        {WingmanResult::CheckIn, "wingman.check_in"},      {WingmanResult::NoTarget, "wingman.no_joy"},
        {WingmanResult::NoFlight, "wingman.no_flight"},    {WingmanResult::Unavailable, "wingman.unavailable"},
        {WingmanResult::RateLimited, "wingman.say_again"}, {WingmanResult::Rejected, "wingman.say_again"},
        {WingmanResult::NotLead, "wingman.not_lead"},
    };
    for (const auto& [r, key] : expected) {
        const Spoken s = speak(cmd(fl::ai::WingmanCommand::Rejoin), r);
        INFO("result ordinal " << static_cast<int>(r));
        CHECK(s.key == key);
        CHECK_FALSE(s.line.empty());
    }
}

TEST_CASE("wingman voice keys are namespaced, unique and wire-sized (#1108)", "[voicekey][wingman]") {
    std::set<std::string> keys;
    for (uint8_t c = 0; c < fl::ai::kWingmanCommandCount; ++c) {
        for (auto r : {WingmanResult::Acknowledged, WingmanResult::Relayed, WingmanResult::CheckIn,
                       WingmanResult::NoTarget, WingmanResult::NoFlight, WingmanResult::Unavailable,
                       WingmanResult::RateLimited, WingmanResult::Rejected, WingmanResult::NotLead}) {
            const std::string k = speak(c, r).key;
            INFO("key " << k);
            CHECK_FALSE(k.empty());
            // MsgRadioTransmission::voiceKey is char[32] and the wire builder truncates SILENTLY, so
            // an over-long key would resolve to a file nobody can name.
            CHECK(k.size() < 32u);
            CHECK((k.rfind("wingman.", 0) == 0 || k.rfind("lead.", 0) == 0));
            keys.insert(k);
        }
    }
    // 6 acks + 6 relayed + 6 distinct outcome keys (RateLimited and Rejected deliberately share
    // "say again", which is one line in the fiction as well as in the code).
    CHECK(keys.size() == 18u);
}

TEST_CASE("a menu with no host audio wired in still acks safely (#1108)", "[voicekey][wingman]") {
    // The callback is optional: a headless or audio-less host leaves it unset, and the subtitle path
    // must be unaffected. An unguarded std::function call here would be a crash on every ack.
    WingmanMenu menu;
    MsgWingmanAck ack{};
    ack.command = cmd(fl::ai::WingmanCommand::Rejoin);
    ack.result = static_cast<uint8_t>(WingmanResult::Acknowledged);
    CHECK_NOTHROW(menu.onAck(ack));
}
