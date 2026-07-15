// SPDX-License-Identifier: GPL-3.0-or-later
//
// The formation / command tree (#610). The cases that matter here are the AUTHORITY ones: command
// cascades DOWN the tree (a package commander may order a flight inside the package), and nothing
// else grants it.
#include <catch2/catch_test_macros.hpp>

#include "world/FormationRegistry.h"

using namespace fl;

namespace {
EntityId ent(uint32_t idx) {
    return EntityId{idx, 1};
}
FormationMember aiMember(uint32_t idx, uint32_t slot = 0) {
    FormationMember m{};
    m.id = ent(idx);
    m.peerId = kNoPeer; // AI
    m.slotIndex = slot;
    return m;
}
FormationMember humanMember(uint32_t idx, uint32_t peerId) {
    FormationMember m{};
    m.id = ent(idx);
    m.peerId = peerId;
    return m;
}
} // namespace

TEST_CASE("FormationRegistry create returns a non-zero id and stores the formation") {
    FormationRegistry reg;
    const FormationId id = reg.create("Viper", ent(10), /*commander=*/7);
    REQUIRE(id != kNoFormation);

    const Formation* f = reg.get(id);
    REQUIRE(f != nullptr);
    CHECK(f->callsign == "Viper");
    CHECK(f->anchor == ent(10));
    CHECK(f->commanderPeerId == 7u);
    CHECK(f->parent == kNoFormation);
    CHECK(reg.size() == 1u);
}

TEST_CASE("FormationRegistry a member belongs to exactly one formation") {
    FormationRegistry reg;
    const FormationId a = reg.create("A", ent(1), 1);
    const FormationId b = reg.create("B", ent(2), 2);

    REQUIRE(reg.addMember(a, aiMember(20)));
    CHECK(reg.formationOfEntity(ent(20)) == a);

    // Adding it to a second formation MOVES it rather than leaving it taking orders from two chains
    // of command.
    REQUIRE(reg.addMember(b, aiMember(20)));
    CHECK(reg.formationOfEntity(ent(20)) == b);
    CHECK(reg.get(a)->members.empty());
    CHECK(reg.get(b)->members.size() == 1u);
}

TEST_CASE("FormationRegistry commands is true for the direct commander") {
    FormationRegistry reg;
    const FormationId id = reg.create("Viper", ent(1), /*commander=*/5);
    CHECK(reg.commands(5, id));
    CHECK_FALSE(reg.commands(6, id));
}

TEST_CASE("FormationRegistry command CASCADES DOWN the tree to child formations") {
    // The whole point of a tiered structure: a package commander can order a flight inside the
    // package without being that flight's own commander.
    FormationRegistry reg;
    const FormationId package = reg.create("Uzi", ent(1), /*commander=*/9);
    const FormationId flight = reg.create("Viper", ent(2), /*commander=*/3, package);
    REQUIRE(flight != kNoFormation);

    CHECK(reg.commands(3, flight)); // its own commander
    CHECK(reg.commands(9, flight)); // the PACKAGE commander, one tier up
    CHECK(reg.commands(9, package));

    // Command does not flow UP: the flight lead does not command the package.
    CHECK_FALSE(reg.commands(3, package));
    // And an unrelated peer commands nothing.
    CHECK_FALSE(reg.commands(4, flight));
}

TEST_CASE("FormationRegistry kNoPeer commands nothing, but PEER 0 is a real player") {
    // kNoPeer (not 0!) is the game-master sentinel. Peer id 0 is an ordinary connected player — ENet
    // hands ids out from 0 — so if 0 were the sentinel, the FIRST PLAYER TO CONNECT could never
    // command their own flight, and a human member with peer id 0 would be misclassified as AI and
    // have their aircraft flown by an autopilot.
    FormationRegistry reg;
    const FormationId gmFlight = reg.create("AI flight", ent(1), /*commander=*/kNoPeer);
    CHECK_FALSE(reg.commands(kNoPeer, gmFlight)); // the GM goes through the admin console instead

    const FormationId peer0Flight = reg.create("Viper", ent(2), /*commander=*/0);
    CHECK(reg.commands(0, peer0Flight)); // peer 0 is a real player and commands their own flight
}

TEST_CASE("FormationMember peer 0 is a HUMAN, not an AI") {
    FormationMember m{};
    m.id = ent(5);
    m.peerId = 0; // an ordinary player
    CHECK_FALSE(m.isAi());
}

TEST_CASE("FormationRegistry an AWACS commands a flight it is not a member of") {
    FormationRegistry reg;
    const FormationId id = reg.create("Chevy", ent(1), /*commander=*/42); // anchor is an AI lead
    REQUIRE(reg.addMember(id, aiMember(2)));
    REQUIRE(reg.addMember(id, aiMember(3)));

    // The commander is not the anchor and is not in the members list — and still commands it.
    CHECK(reg.commands(42, id));
    CHECK(reg.formationOfEntity(ent(1)) == kNoFormation); // the anchor is not a member
    CHECK(reg.get(id)->members.size() == 2u);
}

TEST_CASE("FormationRegistry subtree returns the node and all descendants, parents first") {
    FormationRegistry reg;
    const FormationId pkg = reg.create("Pkg", ent(1), 1);
    const FormationId f1 = reg.create("F1", ent(2), 1, pkg);
    const FormationId f2 = reg.create("F2", ent(3), 1, pkg);
    const FormationId e1 = reg.create("E1", ent(4), 1, f1);

    const auto sub = reg.subtree(pkg);
    REQUIRE(sub.size() == 4u);
    CHECK(sub[0] == pkg); // parents before children, so an order applies top-down
    CHECK(std::find(sub.begin(), sub.end(), f1) != sub.end());
    CHECK(std::find(sub.begin(), sub.end(), f2) != sub.end());
    CHECK(std::find(sub.begin(), sub.end(), e1) != sub.end());

    // A leaf's subtree is just itself.
    CHECK(reg.subtree(e1).size() == 1u);
}

TEST_CASE("FormationRegistry destroy re-parents children instead of destroying them") {
    // Disbanding a package must not shoot down the flights inside it.
    FormationRegistry reg;
    const FormationId pkg = reg.create("Pkg", ent(1), 1);
    const FormationId flight = reg.create("Viper", ent(2), 1, pkg);

    REQUIRE(reg.destroy(pkg));
    REQUIRE(reg.get(pkg) == nullptr);

    const Formation* f = reg.get(flight);
    REQUIRE(f != nullptr);            // the flight survives
    CHECK(f->parent == kNoFormation); // promoted to the top of its chain
}

TEST_CASE("FormationRegistry removeEntity drops a member on death or disconnect") {
    FormationRegistry reg;
    const FormationId id = reg.create("Viper", ent(1), 1);
    REQUIRE(reg.addMember(id, aiMember(20)));

    CHECK(reg.removeEntity(ent(20)) == id);
    CHECK(reg.formationOfEntity(ent(20)) == kNoFormation);
    CHECK(reg.get(id)->members.empty());
    CHECK(reg.removeEntity(ent(20)) == kNoFormation); // idempotent
}

TEST_CASE("FormationRegistry releasePeer clears the command role but keeps the formation flying") {
    // An AI flight whose AWACS logged off is still airborne. It becomes unclaimed, not deleted —
    // and the game master can still order it.
    FormationRegistry reg;
    const FormationId id = reg.create("Chevy", ent(1), /*commander=*/42);
    REQUIRE(reg.addMember(id, aiMember(2)));
    REQUIRE(reg.addMember(id, humanMember(3, /*peerId=*/42)));

    reg.releasePeer(42);

    const Formation* f = reg.get(id);
    REQUIRE(f != nullptr);                // the formation survives its commander
    CHECK(f->commanderPeerId == kNoPeer); // unclaimed
    CHECK(f->members.size() == 1u);       // the human's aircraft left with them
    CHECK(f->members[0].id == ent(2));
    CHECK_FALSE(reg.commands(42, id));
}

TEST_CASE("FormationRegistry rejects an unknown parent") {
    FormationRegistry reg;
    CHECK(reg.create("Orphan", ent(1), 1, /*parent=*/999) == kNoFormation);
}

TEST_CASE("FormationRegistry enforces a depth cap") {
    FormationRegistry reg;
    FormationId parent = kNoFormation;
    for (int i = 0; i < kMaxFormationDepth; ++i) {
        const FormationId id = reg.create("N", ent(static_cast<uint32_t>(i + 1)), 1, parent);
        REQUIRE(id != kNoFormation);
        parent = id;
    }
    // One tier past the cap is refused rather than recursing forever.
    CHECK(reg.create("TooDeep", ent(99), 1, parent) == kNoFormation);
}

TEST_CASE("FormationMember isAi distinguishes a server aircraft from a person") {
    CHECK(aiMember(1).isAi());
    CHECK_FALSE(humanMember(2, /*peerId=*/7).isAi());
}

TEST_CASE("FormationRegistry weaponsHoldFor reads the member's hold flag, false outside any formation") {
    FormationRegistry reg;
    CHECK_FALSE(reg.weaponsHoldFor(ent(20))); // no formation at all

    const FormationId id = reg.create("Viper", ent(10), 7);
    REQUIRE(reg.addMember(id, aiMember(20)));
    CHECK_FALSE(reg.weaponsHoldFor(ent(20))); // default: weapons free

    reg.get(id)->members[0].weaponsHold = true; // the hold_fire order (#610)
    CHECK(reg.weaponsHoldFor(ent(20)));
    CHECK_FALSE(reg.weaponsHoldFor(ent(10))); // the ANCHOR is not a member; no hold applies

    reg.removeEntity(ent(20));
    CHECK_FALSE(reg.weaponsHoldFor(ent(20))); // death/disconnect clears it with the membership
}
