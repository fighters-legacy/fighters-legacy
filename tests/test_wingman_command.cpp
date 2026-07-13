// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scripted wingman grammar (#610) and its two wire messages. The grammar is a community-facing,
// LLM-facing vocabulary, so these tests pin the exact names: a rename must be a deliberate edit here,
// not a silent refactor that desyncs the eval suite and every pack that scripts against it.
#include <catch2/catch_test_macros.hpp>

#include "ai/WingmanCommand.h"
#include "net/GameProtocol.h"
#include "world/Formation.h"

using namespace fl;
using fl::ai::WingmanCommand;

TEST_CASE("wingman grammar has exactly six commands") {
    // The Phase 4 acceptance criterion is "responds to all six commands". If this number changes,
    // the acceptance text and docs/roadmap.md must change with it.
    STATIC_REQUIRE(fl::ai::kWingmanCommandCount == 6u);
    STATIC_REQUIRE(static_cast<uint8_t>(WingmanCommand::Count) == 6u);
}

TEST_CASE("wingman command names are the stable wire vocabulary") {
    CHECK(fl::ai::wingmanCommandName(WingmanCommand::AttackMyTarget) == "attack_my_target");
    CHECK(fl::ai::wingmanCommandName(WingmanCommand::EngageBandits) == "engage_bandits");
    CHECK(fl::ai::wingmanCommandName(WingmanCommand::Rejoin) == "rejoin");
    CHECK(fl::ai::wingmanCommandName(WingmanCommand::CoverMe) == "cover_me");
    CHECK(fl::ai::wingmanCommandName(WingmanCommand::HoldFire) == "hold_fire");
    CHECK(fl::ai::wingmanCommandName(WingmanCommand::ReturnToBase) == "return_to_base");
}

TEST_CASE("wingman command name round-trips through parse") {
    for (uint8_t i = 0; i < fl::ai::kWingmanCommandCount; ++i) {
        const auto cmd = static_cast<WingmanCommand>(i);
        const auto parsed = fl::ai::parseWingmanCommand(fl::ai::wingmanCommandName(cmd));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == cmd);
    }
}

TEST_CASE("parseWingmanCommand rejects anything outside the vocabulary") {
    CHECK_FALSE(fl::ai::parseWingmanCommand("").has_value());
    CHECK_FALSE(fl::ai::parseWingmanCommand("fly_into_the_sun").has_value());
    CHECK_FALSE(fl::ai::parseWingmanCommand("REJOIN").has_value()); // case-sensitive by design
    CHECK_FALSE(fl::ai::parseWingmanCommand("rejoin ").has_value());

    // "unknown" is the intent mapper's DECLINE sentinel, not an executable order. If it ever parsed,
    // an LLM that correctly refused an out-of-grammar utterance would end up ordering the wingman.
    CHECK_FALSE(fl::ai::parseWingmanCommand("unknown").has_value());
}

TEST_CASE("isWingmanCommandOrdinal gates an attacker-supplied byte") {
    for (uint8_t i = 0; i < 6; ++i)
        CHECK(fl::ai::isWingmanCommandOrdinal(i));
    CHECK_FALSE(fl::ai::isWingmanCommandOrdinal(6));
    CHECK_FALSE(fl::ai::isWingmanCommandOrdinal(255));
}

TEST_CASE("an engage order implies weapons free") {
    CHECK(fl::ai::clearsWeaponsHold(WingmanCommand::AttackMyTarget));
    CHECK(fl::ai::clearsWeaponsHold(WingmanCommand::EngageBandits));
    CHECK_FALSE(fl::ai::clearsWeaponsHold(WingmanCommand::HoldFire));
    CHECK_FALSE(fl::ai::clearsWeaponsHold(WingmanCommand::Rejoin));
    CHECK_FALSE(fl::ai::clearsWeaponsHold(WingmanCommand::CoverMe));
    CHECK_FALSE(fl::ai::clearsWeaponsHold(WingmanCommand::ReturnToBase));
}

TEST_CASE("GameProtocol wingman constants mirror the engine enums") {
    // GameProtocol.h must stay stdlib-only (engine-protocol is zero-dep), so it MIRRORS these two
    // values rather than including the headers that define them. WorldBroadcaster.cpp static_asserts
    // the same thing; this is the runtime-visible version of that guarantee.
    STATIC_REQUIRE(kWingmanCommandCount == static_cast<uint8_t>(WingmanCommand::Count));
    STATIC_REQUIRE(kNoFlightId == kNoFormation);
}

TEST_CASE("MsgWingmanCommand wire layout") {
    STATIC_REQUIRE(sizeof(MsgWingmanCommand) == 16u);
    STATIC_REQUIRE(alignof(MsgWingmanCommand) == 4u);
    STATIC_REQUIRE(offsetof(MsgWingmanCommand, command) == 1u);
    STATIC_REQUIRE(offsetof(MsgWingmanCommand, protocolVersion) == 2u);
    STATIC_REQUIRE(offsetof(MsgWingmanCommand, memberIdx) == 4u);
    STATIC_REQUIRE(offsetof(MsgWingmanCommand, seqNum) == 8u);
    STATIC_REQUIRE(offsetof(MsgWingmanCommand, flightId) == 12u);
    STATIC_REQUIRE(offsetof(MsgWingmanCommand, flags) == 14u);

    MsgWingmanCommand m{};
    CHECK(m.msgId == static_cast<uint8_t>(MsgId::WingmanCommand));
    CHECK(m.protocolVersion == kProtocolVersion);
    CHECK(m.memberIdx == kFlightAll); // default addresses the whole flight
    CHECK(m.flightId == kOwnFlight);  // default is "the formation I command"
}

TEST_CASE("MsgWingmanAck wire layout") {
    STATIC_REQUIRE(sizeof(MsgWingmanAck) == 16u);
    STATIC_REQUIRE(alignof(MsgWingmanAck) == 4u);
    STATIC_REQUIRE(offsetof(MsgWingmanAck, result) == 2u);
    STATIC_REQUIRE(offsetof(MsgWingmanAck, flightSize) == 3u);
    STATIC_REQUIRE(offsetof(MsgWingmanAck, memberIdx) == 4u);
    STATIC_REQUIRE(offsetof(MsgWingmanAck, targetIdx) == 8u);
    STATIC_REQUIRE(offsetof(MsgWingmanAck, flightId) == 12u);

    MsgWingmanAck a{};
    CHECK(a.msgId == static_cast<uint8_t>(MsgId::WingmanAck));
    CHECK(a.targetIdx == kNoTarget);
}

TEST_CASE("the two new MsgIds are additive and do not bump the protocol version") {
    // Rule (a) of the compatibility model: a new message type gets a new id and old peers discard it.
    STATIC_REQUIRE(static_cast<uint8_t>(MsgId::WingmanCommand) == 0x0D);
    STATIC_REQUIRE(static_cast<uint8_t>(MsgId::WingmanAck) == 0x0E);
    STATIC_REQUIRE(kProtocolVersion == 1u);
}
