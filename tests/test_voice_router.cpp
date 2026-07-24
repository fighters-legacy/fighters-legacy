// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "net/VoiceRouter.h"

#include <algorithm>
#include <vector>

using namespace fl;

namespace {

VoicePeerView peer(uint32_t id, uint16_t faction = 1, uint32_t formation = kNoVoiceFormation, double x = 0.0) {
    VoicePeerView v;
    v.peerId = id;
    v.faction = faction;
    v.formationId = formation;
    v.admitted = true;
    v.hasPosition = true;
    v.pos[0] = x;
    return v;
}

VoicePeerView observer(uint32_t id) {
    VoicePeerView v;
    v.peerId = id;
    v.admitted = true;
    v.faction = kNoVoiceFaction;
    v.hasPosition = false;
    return v;
}

RadioNetTable table() {
    RadioNetTable t;
    for (auto& def : builtinRadioNets())
        t.add(def);
    return t;
}

bool contains(const std::vector<uint32_t>& v, uint32_t id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}

} // namespace

TEST_CASE("voice routing never echoes a speaker back to themselves", "[voice]") {
    const auto t = table();
    const auto sender = peer(1);
    std::vector<VoicePeerView> peers{sender, peer(2)};
    std::vector<uint32_t> out;
    // A network round trip of your own voice is the single most disorienting thing a voice system
    // can do; sidetone, if we ever want it, belongs on the client.
    REQUIRE(selectVoiceRecipients(t, t.indexOf("team"), sender, peers, out));
    REQUIRE_FALSE(contains(out, 1));
    REQUIRE(contains(out, 2));
}

TEST_CASE("voice routing on a team net is faction-scoped", "[voice]") {
    const auto t = table();
    const auto sender = peer(1, /*faction=*/1);
    std::vector<VoicePeerView> peers{sender, peer(2, 1), peer(3, 2), peer(4, 1)};
    std::vector<uint32_t> out;
    REQUIRE(selectVoiceRecipients(t, t.indexOf("team"), sender, peers, out));
    REQUIRE(out.size() == 2);
    REQUIRE(contains(out, 2));
    REQUIRE(contains(out, 4));
    REQUIRE_FALSE(contains(out, 3));
}

TEST_CASE("voice routing refuses a team transmission from a teamless peer", "[voice]") {
    const auto t = table();
    const auto ghost = observer(9);
    std::vector<VoicePeerView> peers{ghost, peer(1, 1)};
    std::vector<uint32_t> out;
    // "My team" has no referent. Refusing is safer than a frame that silently reaches nobody — or,
    // worse, everybody.
    REQUIRE_FALSE(selectVoiceRecipients(t, t.indexOf("team"), ghost, peers, out));
    REQUIRE(out.empty());
}

TEST_CASE("voice routing on a flight net is formation-scoped and includes the lead", "[voice]") {
    const auto t = table();
    const auto lead = peer(1, 1, /*formation=*/7);
    std::vector<VoicePeerView> peers{lead, peer(2, 1, 7), peer(3, 1, 8), peer(4, 1, kNoVoiceFormation)};
    std::vector<uint32_t> out;
    REQUIRE(selectVoiceRecipients(t, t.indexOf("flight"), lead, peers, out));
    REQUIRE(out.size() == 1);
    REQUIRE(contains(out, 2));

    // A peer in no formation cannot transmit on the flight net at all.
    const auto loner = peer(4, 1, kNoVoiceFormation);
    REQUIRE_FALSE(selectVoiceRecipients(t, t.indexOf("flight"), loner, peers, out));
}

TEST_CASE("voice routing on a proximity net is distance-scoped and side-agnostic", "[voice]") {
    const auto t = table();
    const uint8_t prox = t.indexOf("proximity");
    const RadioNetDef* def = t.byIndex(prox);
    REQUIRE(def != nullptr);
    const double range = static_cast<double>(def->rangeM);

    const auto sender = peer(1, /*faction=*/1, kNoVoiceFormation, 0.0);
    std::vector<VoicePeerView> peers{sender, peer(2, 1, kNoVoiceFormation, range * 0.5), // in range, same side
                                     peer(3, 2, kNoVoiceFormation, range * 0.9),         // in range, ENEMY
                                     peer(4, 1, kNoVoiceFormation, range * 1.5)};        // out of range
    std::vector<uint32_t> out;
    REQUIRE(selectVoiceRecipients(t, prox, sender, peers, out));
    REQUIRE(contains(out, 2));
    REQUIRE(contains(out, 3)); // proximity does not care whose side you are on
    REQUIRE_FALSE(contains(out, 4));
}

TEST_CASE("voice routing excludes a positionless peer from a proximity net", "[voice]") {
    const auto t = table();
    const uint8_t prox = t.indexOf("proximity");
    const auto sender = peer(1);
    std::vector<VoicePeerView> peers{sender, observer(2)};
    std::vector<uint32_t> out;
    REQUIRE(selectVoiceRecipients(t, prox, sender, peers, out));
    // An observer has no aircraft, so there is nothing for them to be near.
    REQUIRE(out.empty());
    REQUIRE_FALSE(canTransmitOn(*t.byIndex(prox), observer(2)));
}

TEST_CASE("voice routing on global and atc nets reaches everyone including observers", "[voice]") {
    const auto t = table();
    RadioNetTable withGlobal;
    withGlobal.add(RadioNetDef{"global", "ALL", RadioNetKind::Global});
    const auto sender = peer(1, 1);
    std::vector<VoicePeerView> peers{sender, peer(2, 2), observer(3)};
    std::vector<uint32_t> out;
    REQUIRE(selectVoiceRecipients(withGlobal, 0, sender, peers, out));
    REQUIRE(out.size() == 2);

    // ATC is the one net a teamless spectator must still be able to use — it is how a player who is
    // not yet flying talks to the tower.
    const auto ghost = observer(3);
    REQUIRE(selectVoiceRecipients(t, t.indexOf("atc"), ghost, peers, out));
    REQUIRE(out.size() == 2);
}

TEST_CASE("voice routing drops an unadmitted peer on both sides", "[voice]") {
    const auto t = table();
    auto sender = peer(1);
    sender.admitted = false;
    auto listener = peer(2);
    listener.admitted = false;
    std::vector<VoicePeerView> peers{sender, listener, peer(3)};
    std::vector<uint32_t> out;
    REQUIRE_FALSE(selectVoiceRecipients(t, t.indexOf("team"), sender, peers, out));

    const auto ok = peer(1);
    REQUIRE(selectVoiceRecipients(t, t.indexOf("team"), ok, peers, out));
    REQUIRE_FALSE(contains(out, 2));
    REQUIRE(contains(out, 3));
}

TEST_CASE("voice routing silences a muted peer's transmit but not their receive", "[voice]") {
    const auto t = table();
    auto muted = peer(1);
    muted.voiceMuted = true;
    std::vector<VoicePeerView> peers{muted, peer(2)};
    std::vector<uint32_t> out;
    REQUIRE_FALSE(selectVoiceRecipients(t, t.indexOf("team"), muted, peers, out));

    // Muting is a moderation action against what someone broadcasts, not a punishment that also
    // blinds them to their own team.
    const auto other = peer(2);
    REQUIRE(selectVoiceRecipients(t, t.indexOf("team"), other, peers, out));
    REQUIRE(contains(out, 1));
}

TEST_CASE("voice routing rejects an unknown net id", "[voice]") {
    const auto t = table();
    const auto sender = peer(1);
    std::vector<VoicePeerView> peers{sender, peer(2)};
    std::vector<uint32_t> out;
    REQUIRE_FALSE(selectVoiceRecipients(t, 200, sender, peers, out));
    REQUIRE_FALSE(selectVoiceRecipients(t, kInvalidRadioNet, sender, peers, out));
    REQUIRE(out.empty());
}

TEST_CASE("voice routing treats a zero-range proximity net as unbounded", "[voice]") {
    RadioNetTable t;
    t.add(RadioNetDef{"prox", "PROX", RadioNetKind::Proximity, /*positional=*/true, /*rangeM=*/0.f});
    const auto sender = peer(1, 1, kNoVoiceFormation, 0.0);
    std::vector<VoicePeerView> peers{sender, peer(2, 1, kNoVoiceFormation, 1.0e9)};
    std::vector<uint32_t> out;
    // Honour the configuration literally rather than substituting a default the operator did not ask
    // for: 0 means "no limit", and a silent 3 km would be a very confusing net to debug.
    REQUIRE(selectVoiceRecipients(t, 0, sender, peers, out));
    REQUIRE(contains(out, 2));
}
