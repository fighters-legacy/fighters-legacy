// SPDX-License-Identifier: GPL-3.0-or-later
#include "world_broadcaster_test_util.h"

using namespace fl;

// ---------------------------------------------------------------------------
// SessionComms (#1087) — everything the server says to players that is not world state.
//
// Moved verbatim from test_world_broadcaster.cpp with the code they exercise: text chat and its
// moderation veto, ATC radio dispatch, the voice relay and its talker cap, the kill feed, the
// build-once scoreboard broadcast, and the MOTD banner.
//
// The shutdown-countdown notices stay in the broadcaster suite: they share MsgServerNotice with this
// class but the countdown itself is the broadcaster's lifecycle, not session comms.
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: a radio command with no ATC answers 'no ATC available' (#703)", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    auto cmd = makeRadioCmd("atc request_landing");
    broadcaster.onReceive(0u, &cmd, sizeof(cmd));

    auto rt = lastRadioTo(net, 0u);
    REQUIRE(rt.has_value());
    CHECK(std::string(rt->text) == "no ATC available");
}

TEST_CASE("WorldBroadcaster: an ATC radio command is dispatched and answered (#703)", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::AirportRegistry airports;
    airports.load({fl::builtinAirfield()}, fl::kEarthRadiusM, nullptr);
    fl::atc::AtcService atc(em, airports, fl::kEarthRadiusM);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setAtcService(&atc);
    connectPilotPeer(broadcaster, net, 0u);

    // A recognised verb: dispatched to the service (flight sequenced) + an immediate acknowledgement.
    auto landCmd = makeRadioCmd("atc request_landing builtin:airfield");
    broadcaster.onReceive(0u, &landCmd, sizeof(landCmd));
    auto ack = lastRadioTo(net, 0u);
    REQUIRE(ack.has_value());
    CHECK(std::string(ack->text) == "roger");
    CHECK(std::string(ack->speaker).empty() == false);

    // The runway is free, so the next ATC tick clears the flight to land — a real clearance line.
    broadcaster.onTick(1.0 / 60.0, fl::atc::AtcService::kIntervalTicks);
    auto cleared = lastRadioTo(net, 0u);
    REQUIRE(cleared.has_value());
    CHECK(std::string(cleared->text) == "cleared to land");

    // An unknown verb is answered politely, not silently dropped.
    auto bogus = makeRadioCmd("atc do_a_barrel_roll");
    broadcaster.onReceive(0u, &bogus, sizeof(bogus));
    auto sayAgain = lastRadioTo(net, 0u);
    REQUIRE(sayAgain.has_value());
    CHECK(std::string(sayAgain->text) == "say again");
}

TEST_CASE("WorldBroadcaster: chat routing, mute, sanitize, rate limit, hook (#646)", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::ManualClock clock;
    fl::WorldQueries q_broadcaster;
    fl::WorldBroadcasterHooks h_broadcaster;
    q_broadcaster.teamAssigner = [](uint32_t p) -> std::optional<uint16_t> {
        return static_cast<uint16_t>(p == 1u ? 2 : 1);
    };
    h_broadcaster.comms.chatModeration = [](uint32_t, uint8_t, std::string_view text) {
        return text.find("badword") == std::string_view::npos;
    };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, std::move(q_broadcaster),
                                     std::move(h_broadcaster));
    broadcaster.setClock(clock);
    // Peer 0 -> faction 1, peer 1 -> faction 2, peer 2 -> faction 1 (peer 0's teammate).
    connectPilotPeer(broadcaster, net, 0u);
    connectPilotPeer(broadcaster, net, 1u);
    connectPilotPeer(broadcaster, net, 2u);
    broadcaster.onTick(1.0 / 60.0, 1u);
    REQUIRE(broadcaster.factionForPeer(0u) == 1u);
    REQUIRE(broadcaster.factionForPeer(1u) == 2u);
    REQUIRE(broadcaster.factionForPeer(2u) == 1u);

    SECTION("All channel reaches every peer including the sender") {
        net.sends.clear();
        net.perPeerSends.clear();
        const auto pkt = makeChatPkt(fl::ChatChannel::All, "hello world");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
        CHECK(countChatEvents(net) == 3);
        CHECK(lastChatEventText(net, 0u) == "hello world"); // self-echo
        CHECK(lastChatEventText(net, 1u) == "hello world");
        CHECK(lastChatEventText(net, 2u) == "hello world");
    }

    SECTION("Team channel reaches only the sender's faction") {
        net.sends.clear();
        net.perPeerSends.clear();
        const auto pkt = makeChatPkt(fl::ChatChannel::Team, "on me");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
        CHECK(countChatEvents(net) == 2); // peer 0 + peer 2 (faction 1); peer 1 excluded
        CHECK(lastChatEventText(net, 0u) == "on me");
        CHECK(lastChatEventText(net, 2u) == "on me");
        CHECK(lastChatEventText(net, 1u).empty());
    }

    SECTION("a muted peer's chat is dropped silently") {
        REQUIRE(broadcaster.setPeerMuted(0u, true));
        CHECK(broadcaster.isPeerMuted(0u));
        net.sends.clear();
        const auto pkt = makeChatPkt(fl::ChatChannel::All, "spam");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
        CHECK(countChatEvents(net) == 0);
        const auto muted = broadcaster.mutedPeers();
        CHECK(std::find(muted.begin(), muted.end(), 0u) != muted.end());
    }

    SECTION("control chars are stripped from the routed text") {
        net.sends.clear();
        net.perPeerSends.clear();
        const auto pkt = makeChatPkt(fl::ChatChannel::All, "hi\x07\x01 there");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
        CHECK(lastChatEventText(net, 1u) == "hi there");
    }

    SECTION("an all-whitespace / empty line is dropped") {
        net.sends.clear();
        const auto pkt = makeChatPkt(fl::ChatChannel::All, "   ");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
        CHECK(countChatEvents(net) == 0);
    }

    SECTION("rate limit warns once per window then drops") {
        broadcaster.setChatRateLimit(2);
        net.sends.clear();
        const auto pkt = makeChatPkt(fl::ChatChannel::All, "flood");
        for (int i = 0; i < 5; ++i)
            broadcaster.onReceive(0u, pkt.data(), pkt.size());
        // First 2 routed (3 recipients each = 6), then dropped; exactly one ServerNotice warning.
        CHECK(countChatEvents(net) == 6);
        int notices = 0;
        for (const auto& p : net.sends)
            if (!p.empty() && p[0] == static_cast<uint8_t>(fl::MsgId::ServerNotice))
                ++notices;
        CHECK(notices == 1);
    }

    SECTION("the moderation hook can suppress a line") {
        net.sends.clear();
        const auto blocked = makeChatPkt(fl::ChatChannel::All, "a badword here");
        broadcaster.onReceive(0u, blocked.data(), blocked.size());
        CHECK(countChatEvents(net) == 0);
        const auto ok = makeChatPkt(fl::ChatChannel::All, "clean line");
        broadcaster.onReceive(0u, ok.data(), ok.size());
        CHECK(countChatEvents(net) == 3);
    }

    SECTION("chat disabled drops everything") {
        broadcaster.setChatEnabled(false);
        net.sends.clear();
        const auto pkt = makeChatPkt(fl::ChatChannel::All, "hi");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
        CHECK(countChatEvents(net) == 0);
    }
}

TEST_CASE("WorldBroadcaster: no MOTD sent by default", "[world_broadcaster][motd]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    // setMotd NOT called

    connectPilotPeer(broadcaster, net, 0u);

    // Hello + ConnectAck; no MOTD packet
    CHECK(net.sends.size() == 4u); // +1 PlayerRoster (#996), +1 VoiceNetDef (#532)
    for (const auto& pkt : net.sends)
        CHECK(pkt[0] != static_cast<uint8_t>(fl::MsgId::Motd));
}

TEST_CASE("WorldBroadcaster: MOTD sent as third send when non-empty", "[world_broadcaster][motd]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setMotd("Welcome!");

    connectPilotPeer(broadcaster, net, 0u);

    REQUIRE(net.sends.size() == 5u); // +1 PlayerRoster (#996), +1 VoiceNetDef (#532)
    const auto* motd = findSend(net, fl::MsgId::Motd);
    REQUIRE(motd != nullptr);
    CHECK(parseMotdText(*motd) == "Welcome!");
}

TEST_CASE("WorldBroadcaster: oversized MOTD capped at kMaxMotdBytes", "[world_broadcaster][motd]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setMotd(std::string(fl::kMaxMotdBytes + 500, 'A'));

    connectPilotPeer(broadcaster, net, 0u);

    REQUIRE(net.sends.size() == 5u); // +1 PlayerRoster (#996), +1 VoiceNetDef (#532)
    // sizeof(MsgMotdHeader) (4) + kMaxMotdBytes (text) + 1 (NUL)
    const auto* motd = findSend(net, fl::MsgId::Motd);
    REQUIRE(motd != nullptr);
    CHECK(motd->size() == sizeof(fl::MsgMotdHeader) + fl::kMaxMotdBytes + 1u);
    CHECK(net.sends[2].back() == 0u); // NUL terminator
}

TEST_CASE("WorldBroadcaster: setMotd with empty string suppresses MOTD send", "[world_broadcaster][motd]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setMotd("hello");
    broadcaster.setMotd(""); // cleared

    connectPilotPeer(broadcaster, net, 0u);

    CHECK(net.sends.size() == 4u); // +1 PlayerRoster (#996), +1 VoiceNetDef (#532)
    for (const auto& pkt : net.sends)
        CHECK(pkt[0] != static_cast<uint8_t>(fl::MsgId::Motd));
}

TEST_CASE("WorldBroadcaster: MOTD displaySeconds is 0 by default", "[world_broadcaster][motd]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setMotd("Welcome!");

    connectPilotPeer(broadcaster, net, 0u);

    REQUIRE(net.sends.size() == 5u); // +1 PlayerRoster (#996), +1 VoiceNetDef (#532)
    const auto* motd = findSend(net, fl::MsgId::Motd);
    REQUIRE(motd != nullptr);
    REQUIRE(motd->size() >= sizeof(fl::MsgMotdHeader));
    uint16_t secs = 0;
    std::memcpy(&secs, motd->data() + offsetof(fl::MsgMotdHeader, displaySeconds), sizeof(secs));
    CHECK(secs == 0u);
}

TEST_CASE("WorldBroadcaster: MOTD packet displaySeconds matches setMotdDisplaySeconds", "[world_broadcaster][motd]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setMotd("Welcome!");
    broadcaster.setMotdDisplaySeconds(45u);

    connectPilotPeer(broadcaster, net, 0u);

    REQUIRE(net.sends.size() == 5u); // +1 PlayerRoster (#996), +1 VoiceNetDef (#532)
    const auto* motd = findSend(net, fl::MsgId::Motd);
    REQUIRE(motd != nullptr);
    REQUIRE(motd->size() >= sizeof(fl::MsgMotdHeader));
    uint16_t secs = 0;
    std::memcpy(&secs, motd->data() + offsetof(fl::MsgMotdHeader, displaySeconds), sizeof(secs));
    CHECK(secs == 45u);
}

TEST_CASE("WorldBroadcaster: applyConfig wires MOTD and display seconds in one call", "[world_broadcaster][motd]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);

    fl::WorldBroadcasterConfig cfg;
    cfg.motd = "Welcome via config!";
    cfg.motdDisplaySeconds = 30u;
    broadcaster.applyConfig(cfg);

    connectPilotPeer(broadcaster, net, 0u);

    REQUIRE(net.sends.size() == 5u); // +1 PlayerRoster (#996), +1 VoiceNetDef (#532)
    const auto* motd = findSend(net, fl::MsgId::Motd);
    REQUIRE(motd != nullptr);
    CHECK(parseMotdText(*motd) == "Welcome via config!");
    uint16_t secs = 0;
    std::memcpy(&secs, motd->data() + offsetof(fl::MsgMotdHeader, displaySeconds), sizeof(secs));
    CHECK(secs == 30u);
}

TEST_CASE("WorldBroadcaster: a kill broadcasts credit and unicasts the killer's stats", "[world_broadcaster][combat]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::EntityTransform t{};
    t.pos[1] = 500.0;
    const fl::EntityId victim = em.spawn("builtin:debug-entity", t);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    em.addEventHandler(&broadcaster);
    connectPilotPeer(broadcaster, net, 0u);
    const auto ack = parseSendAck(net);
    const fl::EntityId shooter{ack.assignedEntityIdx, ack.assignedEntityGen};

    em.applyDamage(victim, 200.f, shooter); // lethal, credited to peer 0's aircraft
    broadcaster.onTick(1.0 / 60.0, 1u);

    const auto kills = combatBroadcasts(net);
    REQUIRE(kills.size() == 1u);
    CHECK(kills[0].type == static_cast<uint8_t>(fl::CombatEventType::Kill));
    CHECK(kills[0].subjectIdx == victim.index);
    CHECK(kills[0].instigatorIdx == shooter.index);
    CHECK(kills[0].a == 0u);                // peer 0 gets the credit — a REAL peer id
    CHECK(kills[0].b == fl::kNoOwningPeer); // the victim was AI/server-owned

    const auto stats = lastStatsFor(net, 0u);
    REQUIRE(stats.has_value());
    CHECK(stats->a == 1u); // kills
    CHECK(stats->b == 0u); // losses
    CHECK(stats->c == 1);  // ScoreAwarded's default credit
}

TEST_CASE("WorldBroadcaster: the scoreboard broadcast builds once and sends identical bytes (#1091)",
          "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    constexpr int kPeers = 6;
    for (uint32_t p = 0; p < kPeers; ++p)
        connectPilotPeer(broadcaster, net, p);

    // Give the board rows to carry: a scoreboard row appears when a peer actually scores, so drive
    // the same ScoreAwarded event the kill path does.
    broadcaster.forEachPeer([&](const fl::PeerInfo& p) {
        fl::EntityEvent ev{};
        ev.type = fl::EntityEventType::ScoreAwarded;
        ev.instigator = p.eid; // the scoring entity resolves back to its owning peer
        ev.score = 10 + static_cast<int>(p.peerId);
        broadcaster.onEntityEvent(ev);
    });

    const uint64_t buildsBefore = broadcaster.scoreboardBuildCount();
    net.perPeerSends.clear();

    // Drive ticks until the ~2 s dirty window fires.
    for (uint64_t tick = 1; tick <= 130; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    const uint64_t builds = broadcaster.scoreboardBuildCount() - buildsBefore;
    REQUIRE(builds >= 1u);
    // ONE build per window regardless of peer count. Before this it was one per peer, so six peers
    // meant six identical builds — and 128 peers meant 128.
    CHECK(builds < static_cast<uint64_t>(kPeers));

    // Every peer received byte-identical scoreboard bytes: this is a computation fix, not a wire
    // change, so the payload must be unchanged.
    std::unordered_map<uint32_t, std::vector<std::vector<uint8_t>>> perPeer;
    for (const auto& [pid, pkt] : net.perPeerSends)
        if (!pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::Scoreboard))
            perPeer[pid].push_back(pkt);

    REQUIRE(perPeer.size() == static_cast<std::size_t>(kPeers));
    const auto& reference = perPeer.begin()->second;
    REQUIRE_FALSE(reference.empty());
    for (const auto& [pid, packets] : perPeer) {
        INFO("peer " << pid);
        REQUIRE(packets.size() == reference.size());
        for (std::size_t i = 0; i < packets.size(); ++i)
            CHECK(packets[i] == reference[i]);
    }
}

TEST_CASE("WorldBroadcaster: voice routing is unchanged at or below the talker cap (#1090)",
          "[world_broadcaster][voice]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    fl::ManualClock clock;
    broadcaster.setClock(clock);
    broadcaster.setVoiceEnabled(true);

    constexpr int kPeers = 8;
    for (uint32_t p = 0; p < kPeers; ++p)
        connectPilotPeer(broadcaster, net, p);
    broadcaster.onTick(1.0 / 60.0, 1);

    const uint8_t net0 = 0;
    const fl::RadioNetDef* def = broadcaster.radioNets().byIndex(net0);
    REQUIRE(def != nullptr);
    REQUIRE(def->maxTalkers == 4); // the D20 default

    // Four talkers — at the cap, so every frame must still relay exactly as before.
    const uint64_t before = broadcaster.voiceRelaySendCount();
    for (uint32_t talker = 0; talker < 4; ++talker) {
        const auto frame = makeVoiceFrame(net0, 1);
        broadcaster.onReceive(talker, frame.data(), frame.size());
    }
    const uint64_t relayed = broadcaster.voiceRelaySendCount() - before;
    CHECK(relayed > 0u); // routing happened: nothing at or below the cap is dropped

    // A fifth talker on the same net, in the same moment, is over the cap.
    const uint64_t beforeFifth = broadcaster.voiceRelaySendCount();
    const auto fifth = makeVoiceFrame(net0, 1);
    broadcaster.onReceive(4u, fifth.data(), fifth.size());
    CHECK(broadcaster.voiceRelaySendCount() == beforeFifth); // dropped, and dropped silently

    // ...but a talker that already holds a slot keeps relaying.
    const uint64_t beforeRepeat = broadcaster.voiceRelaySendCount();
    const auto repeat = makeVoiceFrame(net0, 2);
    broadcaster.onReceive(0u, repeat.data(), repeat.size());
    CHECK(broadcaster.voiceRelaySendCount() > beforeRepeat);

    // After the hold window, an idle talker's slot frees and the fifth peer can take it.
    clock.advance(std::chrono::milliseconds(400));
    const uint64_t beforeTakeover = broadcaster.voiceRelaySendCount();
    const auto retry = makeVoiceFrame(net0, 2);
    broadcaster.onReceive(4u, retry.data(), retry.size());
    CHECK(broadcaster.voiceRelaySendCount() > beforeTakeover);
}

TEST_CASE("WorldBroadcaster: the voice frame limit sits below the codec rate (#1090)", "[world_broadcaster][voice]") {
    // The shipped limit was 60/s while the codec produces 50/s, so it capped nothing a well-behaved
    // client could even reach. 52 leaves jitter headroom and actually binds.
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    fl::ManualClock clock;
    broadcaster.setClock(clock);
    broadcaster.setVoiceEnabled(true);

    connectPilotPeer(broadcaster, net, 0u);
    connectPilotPeer(broadcaster, net, 1u);
    broadcaster.onTick(1.0 / 60.0, 1);

    // 200 frames in one window from a single peer: the limiter must stop it well short.
    const uint64_t before = broadcaster.voiceRelaySendCount();
    for (int i = 0; i < 200; ++i) {
        const auto frame = makeVoiceFrame(0, static_cast<uint16_t>(i));
        broadcaster.onReceive(0u, frame.data(), frame.size());
    }
    const uint64_t sends = broadcaster.voiceRelaySendCount() - before;
    CHECK(sends <= 52u * 1u); // one recipient, at most the per-second frame budget
    CHECK(sends < 60u);       // and strictly below the old limit, which never bound at all
}
