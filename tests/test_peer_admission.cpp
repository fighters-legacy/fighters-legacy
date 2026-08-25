// SPDX-License-Identifier: GPL-3.0-or-later
#include "world_broadcaster_test_util.h"

using namespace fl;

// ---------------------------------------------------------------------------
// PeerAdmission (#1085) — whether and how a peer enters the world.
//
// These cases moved verbatim from test_world_broadcaster.cpp with the code they exercise: the
// five-gate connect gauntlet, the MsgConnectRequest handshake, the ConnectAck reply burst, the
// spawn-point and mission-slot placement, and the reconnect grace window. They drive the
// collaborator through WorldBroadcaster, which is how production reaches it — a test that
// constructed a bare PeerAdmission would assert against a seam no caller uses.
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: onConnect sends ConnectAck with registered types and spawns entity",
          "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef()); // required for peer-entity spawn
    registry.registerType(makeDebugDef("type:a"));
    registry.registerType(makeDebugDef("type:b"));

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    REQUIRE(net.sends.size() == 4u); // +1 PlayerRoster (#996), +1 VoiceNetDef (#532)
    CHECK(net.sendReliable);

    fl::MsgConnectAck ack = parseSendAck(net);
    CHECK(ack.msgId == static_cast<uint8_t>(fl::MsgId::ConnectAck));
    CHECK(ack.tickRateHz == 60);
    CHECK(ack.typeCount == 3u);
    CHECK(ack.assignedEntityGen != 0u); // generation != 0 means a valid entity was assigned

    // liveCount() is an atomic snapshot updated at the end of onTick() — drive one tick.
    broadcaster.onTick(1.0 / 60.0, 1u);
    CHECK(em.liveCount() == 1u);
}

TEST_CASE("WorldBroadcaster: join password gates admission (#998)", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJoinPassword("s3cret");

    auto connectWithPassword = [&](uint32_t peerId, const char* pw) {
        broadcaster.onConnect(peerId);
        fl::MsgConnectRequest req{};
        req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
        std::vector<uint8_t> buf;
        fl::appendMsg(buf, req);
        if (pw)
            fl::appendExtRaw(buf, static_cast<uint16_t>(fl::ExtTag::ConnectJoinPassword), pw,
                             static_cast<uint16_t>(std::strlen(pw)));
        broadcaster.onReceive(peerId, buf.data(), buf.size());
    };
    auto refusedBadPassword = [&]() {
        for (const auto& pkt : net.sends)
            if (!pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::ConnectRefusal) &&
                pkt.size() >= sizeof(fl::MsgConnectRefusal)) {
                fl::MsgConnectRefusal r{};
                std::memcpy(&r, pkt.data(), sizeof(r));
                if (r.code == static_cast<uint8_t>(fl::ConnectRefusalCode::BadPassword))
                    return true;
            }
        return false;
    };

    SECTION("missing password is refused") {
        connectWithPassword(0u, nullptr);
        broadcaster.onTick(1.0 / 60.0, 1u);
        CHECK(refusedBadPassword());
        CHECK(em.liveCount() == 0u);
    }
    SECTION("wrong password is refused") {
        connectWithPassword(0u, "nope");
        broadcaster.onTick(1.0 / 60.0, 1u);
        CHECK(refusedBadPassword());
    }
    SECTION("correct password admits") {
        connectWithPassword(0u, "s3cret");
        broadcaster.onTick(1.0 / 60.0, 1u);
        CHECK_FALSE(refusedBadPassword());
        CHECK(em.liveCount() == 1u);
    }
}

TEST_CASE("WorldBroadcaster: team assigner stamps faction, refuses when full (#522)", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    // Frozen at construction (#1082): each SECTION sets what the assigner answers, it does not
    // install a different assigner.
    std::optional<uint16_t> assignedTeam = uint16_t{7};
    fl::WorldQueries q_broadcaster;
    q_broadcaster.teamAssigner = [&assignedTeam](uint32_t) { return assignedTeam; };
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, std::move(q_broadcaster));

    SECTION("assigner faction is stamped onto the spawned aircraft") {
        assignedTeam = uint16_t{7};
        connectPilotPeer(broadcaster, net, 0u);
        broadcaster.onTick(1.0 / 60.0, 1u);
        CHECK(broadcaster.factionForPeer(0u) == 7u);
    }

    SECTION("assigner returning nullopt refuses with MatchFull") {
        assignedTeam = std::nullopt;
        broadcaster.onConnect(0u);
        fl::MsgConnectRequest req{};
        req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
        broadcaster.onReceive(0u, &req, sizeof(req));
        // A MsgConnectRefusal with code MatchFull was sent and the peer disconnected.
        bool refused = false;
        for (const auto& pkt : net.sends) {
            if (!pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::ConnectRefusal) &&
                pkt.size() >= sizeof(fl::MsgConnectRefusal)) {
                fl::MsgConnectRefusal r{};
                std::memcpy(&r, pkt.data(), sizeof(r));
                if (r.code == static_cast<uint8_t>(fl::ConnectRefusalCode::MatchFull))
                    refused = true;
            }
        }
        CHECK(refused);
        CHECK(broadcaster.factionForPeer(0u) == fl::WorldBroadcaster::kNoFaction);
    }
}

TEST_CASE("WorldBroadcaster: a world at the entity soft cap refuses pilots (#1049)", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    SECTION("the player reserve keeps a pilot flyable in a world full of AI") {
        em.setSoftCap(3, /*playerReserve=*/1);
        // Fill the world tier with non-player entities, as a runaway script or a projectile storm would.
        fl::EntityTransform t{};
        REQUIRE(em.spawn("builtin:debug-entity", t).valid());
        REQUIRE(em.spawn("builtin:debug-entity", t).valid());
        REQUIRE_FALSE(em.spawn("builtin:debug-entity", t).valid()); // world tier exhausted

        connectPilotPeer(broadcaster, net, 0u);
        broadcaster.onTick(1.0 / 60.0, 1u);
        // The pilot got the reserved slot: an ack with a real entity, not a refusal.
        CHECK(net.disconnectedPeers.empty());
        CHECK(parseSendAck(net).assignedEntityGen != 0u);
    }

    SECTION("a genuinely full world refuses with ServerFull instead of acking a pilot with no aircraft") {
        em.setSoftCap(2, /*playerReserve=*/0); // flat cap, no headroom for anyone
        fl::EntityTransform t{};
        REQUIRE(em.spawn("builtin:debug-entity", t).valid());
        REQUIRE(em.spawn("builtin:debug-entity", t).valid());

        connectPilotPeer(broadcaster, net, 0u);
        bool refused = false;
        for (const auto& pkt : net.sends) {
            if (!pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::ConnectRefusal) &&
                pkt.size() >= sizeof(fl::MsgConnectRefusal)) {
                fl::MsgConnectRefusal r{};
                std::memcpy(&r, pkt.data(), sizeof(r));
                if (r.code == static_cast<uint8_t>(fl::ConnectRefusalCode::ServerFull))
                    refused = true;
            }
        }
        CHECK(refused);
        CHECK(net.disconnectedPeers.size() == 1u);
        // No ConnectAck at all: the alternative — admitting a "pilot" with no entity — is a client
        // stuck on a loading screen with nothing to fly and no reason given.
        for (const auto& pkt : net.sends)
            CHECK(pkt[0] != static_cast<uint8_t>(fl::MsgId::ConnectAck));
        CHECK(em.softCapRefusals() >= 1u);
    }

    SECTION("observer -> pilot at the cap leaves the peer an observer") {
        broadcaster.onConnect(0u); // observers are allowed by default (#857)
        fl::MsgConnectRequest req{};
        req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Observer);
        broadcaster.onReceive(0u, &req, sizeof(req));

        // Now fill the world completely, then try to promote the observer.
        em.setSoftCap(1, /*playerReserve=*/0);
        fl::EntityTransform t{};
        REQUIRE(em.spawn("builtin:debug-entity", t).valid());
        net.sends.clear();
        CHECK_FALSE(broadcaster.setPeerRole(0u, fl::PeerRole::Pilot));
        // Still an observer, still connected, and told why over the notice channel.
        CHECK(net.disconnectedPeers.empty());
        bool notice = false;
        for (const auto& pkt : net.sends)
            if (!pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::ServerNotice))
                notice = true;
        CHECK(notice);
    }
}

TEST_CASE("WorldBroadcaster: ConnectAck type defs carry category and projectileKind ordinals (#886)",
          "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef()); // typeIndex 0: AirVehicle (peer spawn)
    fl::EntityDef bunker = makeDebugDef("builtin:static-target");
    bunker.category = fl::ObjectCategory::Structure;
    registry.registerType(std::move(bunker)); // typeIndex 1
    fl::EntityDef bombProjectile = makeDebugDef("projectile:builtin:bomb");
    bombProjectile.category = fl::ObjectCategory::Projectile;
    bombProjectile.projectileKind = fl::ProjectileKind::Bomb;
    registry.registerType(std::move(bombProjectile)); // typeIndex 2

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    REQUIRE(parseSendAck(net).typeCount == 3u);
    const auto& pkt = net.sends[1];
    std::size_t off = sizeof(fl::MsgConnectAck);
    fl::MsgEntityTypeDef tds[3];
    for (auto& td : tds) {
        REQUIRE(fl::readRecordAt(pkt.data(), pkt.size(), off, td));
        off += sizeof(td);
    }
    CHECK(tds[0].category == static_cast<uint8_t>(fl::ObjectCategory::AirVehicle));
    CHECK(tds[0].projectileKind == static_cast<uint8_t>(fl::ProjectileKind::None));
    CHECK(tds[1].category == static_cast<uint8_t>(fl::ObjectCategory::Structure));
    CHECK(tds[2].category == static_cast<uint8_t>(fl::ObjectCategory::Projectile));
    CHECK(tds[2].projectileKind == static_cast<uint8_t>(fl::ProjectileKind::Bomb));
}

TEST_CASE("WorldBroadcaster: peer entity spawns at terrain height plus 500 m AGL", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:5000";
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setGroundElevation(550.f);
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back();
    REQUIRE(parseSnapshotHeader(pkt).recordCount >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // 550 m terrain + 500 m AGL = 1050 m; allow ±10 m for one flight-integrator tick
    CHECK(e.pos[1] >= 1040.0);
    CHECK(e.pos[1] <= 1060.0);
}

TEST_CASE("WorldBroadcaster: peer entity spawns at 500 m AGL when ground elevation is zero", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:5000";
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    // setGroundElevation not called — default m_groundElevation = 0.f
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back();
    REQUIRE(parseSnapshotHeader(pkt).recordCount >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // 0 m terrain + 500 m AGL = 500 m; allow ±10 m for one flight-integrator tick
    CHECK(e.pos[1] >= 490.0);
    CHECK(e.pos[1] <= 510.0);
}

TEST_CASE("WorldBroadcaster: bare admitPilot spawns at flying speed, not stationary (#1334)", "[world_broadcaster]") {
    // The uncovered path the #1334 survey flagged: the no-mission/no-spawn-point sandbox spawn used
    // to be stationary at 500 m AGL, which only the old no-stall builtin could survive — a real
    // model dropped at zero airspeed departs before the pilot ever has control. The spawn now rides
    // the same kDefaultSpawnAirspeedMps cruise every airborne mission spawn gets, and this pins it:
    // the first snapshot must already show body-forward flying speed.
    NullLogger logger;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:5000";
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back();
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    const double speed =
        std::sqrt(double(e.vel[0]) * e.vel[0] + double(e.vel[1]) * e.vel[1] + double(e.vel[2]) * e.vel[2]);
    CHECK(speed > 100.0); // the 120 m/s default cruise, quantized, less one tick of drag/gravity
    CHECK(speed < 140.0);
    // Along the spawn heading (identity = +X), not a vertical drop.
    CHECK(e.vel[0] > 100.f);
    CHECK(std::abs(e.vel[1]) < 30.f);
}

TEST_CASE("WorldBroadcaster: peer entity spawns at configured spawn point XYZ", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:5000";
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setSpawnPoints({std::array<double, 3>{1000.0, 750.0, -500.0}});
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back();
    REQUIRE(parseSnapshotHeader(pkt).recordCount >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // X and Z are set from the spawn point; Y allows ±10 m and X +3 m for one flight-integrator
    // tick — an admitPilot spawn is airborne at the default cruise since #1334, so it covers ~2 m
    // of body-forward (+X) travel before the first snapshot.
    CHECK(e.pos[0] >= 999.0);
    CHECK(e.pos[0] <= 1003.0);
    CHECK(e.pos[1] >= 740.0);
    CHECK(e.pos[1] <= 760.0);
    CHECK(e.pos[2] >= -501.0);
    CHECK(e.pos[2] <= -499.0);
}

TEST_CASE("WorldBroadcaster: spawn points assigned round-robin to peers", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:5000";
    net.peerAddresses[1] = "5.6.7.8:6000";
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setSpawnPoints({std::array<double, 3>{0.0, 200.0, 0.0}, std::array<double, 3>{1000.0, 300.0, 500.0}});
    connectPilotPeer(broadcaster, net, 0u); // → point 0 (X=0)
    connectPilotPeer(broadcaster, net, 1u); // → point 1 (X=1000)
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back();
    auto _ents = decodeEntities(pkt);
    REQUIRE(_ents.size() >= 2u);

    DecodedEntity e0 = _ents[0], e1 = _ents[1];
    // Sort by X so the check is order-independent.
    if (e0.pos[0] > e1.pos[0])
        std::swap(e0, e1);

    // e0 → point 0 (X≈0), e1 → point 1 (X≈1000)
    CHECK(e0.pos[0] >= -1.0);
    CHECK(e0.pos[0] <= 3.0); // +3 m: one tick of body-forward travel at the #1334 spawn cruise
    CHECK(e1.pos[0] >= 999.0);
    CHECK(e1.pos[0] <= 1003.0); // +3 m: one tick of body-forward travel at the #1334 spawn cruise
    CHECK(e1.pos[2] >= 499.0);
    CHECK(e1.pos[2] <= 501.0);
}

TEST_CASE("WorldBroadcaster: spawn point index wraps round-robin with three peers two points", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:5000";
    net.peerAddresses[1] = "5.6.7.8:6000";
    net.peerAddresses[2] = "9.10.11.12:7000";
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setSpawnPoints({std::array<double, 3>{0.0, 200.0, 0.0}, std::array<double, 3>{1000.0, 300.0, 0.0}});
    connectPilotPeer(broadcaster, net, 0u); // → point 0 (X≈0)
    connectPilotPeer(broadcaster, net, 1u); // → point 1 (X≈1000)
    connectPilotPeer(broadcaster, net, 2u); // → point 0 again (wrap)
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back();
    const auto entries = decodeEntities(pkt);
    REQUIRE(entries.size() >= 3u);

    // Count how many entities landed near X=0 vs X=1000 (−1..+3 m: one tick of body-forward travel
    // at the #1334 spawn cruise rides on top of the spawn point).
    int nearPoint0 = 0;
    int nearPoint1 = 0;
    for (const auto& e : entries) {
        if (e.pos[0] >= -1.0 && e.pos[0] <= 3.0)
            ++nearPoint0;
        else if (e.pos[0] >= 999.0 && e.pos[0] <= 1003.0)
            ++nearPoint1;
    }
    CHECK(nearPoint0 == 2); // peers 0 and 2 → point 0
    CHECK(nearPoint1 == 1); // peer 1 → point 1
}

TEST_CASE("WorldBroadcaster: onConnect with empty registry sends typeCount=0 and assigns no entity",
          "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    connectPilotPeer(broadcaster, net, 0u);

    // #1049: a pilot whose aircraft cannot be spawned is REFUSED, not acked with a null entity. The
    // cause here is configuration (no registered type), not capacity, so the code is NoAirframe.
    bool refused = false;
    for (const auto& pkt : net.sends) {
        if (!pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::ConnectRefusal) &&
            pkt.size() >= sizeof(fl::MsgConnectRefusal)) {
            fl::MsgConnectRefusal r{};
            std::memcpy(&r, pkt.data(), sizeof(r));
            if (r.code == static_cast<uint8_t>(fl::ConnectRefusalCode::NoAirframe))
                refused = true;
        }
    }
    CHECK(refused);
    CHECK(net.disconnectedPeers.size() == 1u);
    for (const auto& pkt : net.sends)
        CHECK(pkt[0] != static_cast<uint8_t>(fl::MsgId::ConnectAck));
    CHECK(em.liveCount() == 0u);
    CHECK(em.softCapRefusals() == 0u); // not a cap refusal — the counter must stay honest
}

TEST_CASE("WorldBroadcaster: onConnect without builtin type registered assigns no entity", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    // Register a type but NOT "builtin:debug-entity"
    registry.registerType(makeDebugDef("other:type"));

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // "builtin:debug-entity" is not registered, so no aircraft can be spawned — refuse with
    // NoAirframe rather than admitting a pilot with nothing to fly (#1049).
    bool refused = false;
    for (const auto& pkt : net.sends) {
        if (!pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::ConnectRefusal) &&
            pkt.size() >= sizeof(fl::MsgConnectRefusal)) {
            fl::MsgConnectRefusal r{};
            std::memcpy(&r, pkt.data(), sizeof(r));
            if (r.code == static_cast<uint8_t>(fl::ConnectRefusalCode::NoAirframe))
                refused = true;
        }
    }
    CHECK(refused);
    CHECK(em.liveCount() == 0u);
}

TEST_CASE("WorldBroadcaster: onConnect sends MsgHello as first reliable packet", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    REQUIRE(net.sends.size() == 4u); // +1 PlayerRoster (#996), +1 VoiceNetDef (#532)

    fl::MsgHello hello = parseSendHello(net);
    CHECK(hello.msgId == static_cast<uint8_t>(fl::MsgId::Hello));
    CHECK(hello.protocolVersion == fl::kProtocolVersion);

    fl::MsgConnectAck ack = parseSendAck(net);
    CHECK(ack.msgId == static_cast<uint8_t>(fl::MsgId::ConnectAck));
}

TEST_CASE("WorldBroadcaster: banAddress kicks connected peer with matching IPv4", "[world_broadcaster][admin]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    net.peerAddresses[0] = "1.2.3.4:5000";
    connectPilotPeer(broadcaster, net, 0u);
    net.disconnectedPeers.clear();

    broadcaster.banAddress("1.2.3.4");

    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 0u);
}

TEST_CASE("WorldBroadcaster: banAddress with no connected peers does not crash", "[world_broadcaster][admin]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    REQUIRE_NOTHROW(broadcaster.banAddress("10.0.0.1"));
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: banAddress does not kick peer on different IP", "[world_broadcaster][admin]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    net.peerAddresses[0] = "5.5.5.5:5000";
    connectPilotPeer(broadcaster, net, 0u);
    net.disconnectedPeers.clear();

    broadcaster.banAddress("1.2.3.4"); // different IP — peer 0 must not be kicked

    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: banned IPv4 peer is rejected on onConnect", "[world_broadcaster][admin]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.banAddress("1.2.3.4");
    net.peerAddresses[0] = "1.2.3.4:5000";
    connectPilotPeer(broadcaster, net, 0u);

    // Peer was rejected: MsgConnectRefusal sent, then disconnectPeer called.
    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 0u);
    auto ref = parseSendRefusal(net);
    CHECK(ref.msgId == static_cast<uint8_t>(fl::MsgId::ConnectRefusal));
    CHECK(std::string_view(ref.reason) == "You are banned from this server.");
}

TEST_CASE("WorldBroadcaster: IPv4-mapped IPv6 peer is rejected when IPv4 is banned", "[world_broadcaster][admin]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.banAddress("1.2.3.4");
    net.peerAddresses[0] = "[::ffff:1.2.3.4]:5000"; // dual-stack mapped form
    connectPilotPeer(broadcaster, net, 0u);

    REQUIRE(net.disconnectedPeers.size() == 1u);
    auto ref = parseSendRefusal(net);
    CHECK(ref.msgId == static_cast<uint8_t>(fl::MsgId::ConnectRefusal));
    CHECK(std::string_view(ref.reason) == "You are banned from this server.");
}

TEST_CASE("WorldBroadcaster: peer on non-banned IP is allowed on onConnect", "[world_broadcaster][admin]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.banAddress("9.9.9.9");
    net.peerAddresses[0] = "1.2.3.4:5000"; // different IP — must be allowed
    connectPilotPeer(broadcaster, net, 0u);

    CHECK(net.disconnectedPeers.empty());
    CHECK(net.sends.size() >= 2u); // MsgHello + ConnectAck
}

TEST_CASE("WorldBroadcaster: unbanAddress allows reconnect after ban", "[world_broadcaster][admin]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.banAddress("1.2.3.4");
    broadcaster.unbanAddress("1.2.3.4");

    net.peerAddresses[0] = "1.2.3.4:5000";
    connectPilotPeer(broadcaster, net, 0u);

    CHECK(net.disconnectedPeers.empty());
    CHECK(net.sends.size() >= 2u);
}

TEST_CASE("WorldBroadcaster: IP under rate limit is not disconnected", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setRateLimitParams(5, 10, 3);

    net.peerAddresses[0] = "1.2.3.4:1000";
    for (int i = 0; i < 4; ++i) {
        connectPilotPeer(broadcaster, net, 0u);
        broadcaster.onDisconnect(0u);
        net.disconnectedPeers.clear();
        net.sends.clear();
    }
    // 5th connect (at limit, size == limit, not strictly over): should be allowed
    connectPilotPeer(broadcaster, net, 0u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: IP exceeding rate limit is disconnected", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setRateLimitParams(3, 10, 3);

    net.peerAddresses[0] = "1.2.3.4:1000";
    // 3 connects fill the window
    for (int i = 0; i < 3; ++i) {
        connectPilotPeer(broadcaster, net, 0u);
        broadcaster.onDisconnect(0u);
        net.disconnectedPeers.clear();
        net.sends.clear();
    }
    // 4th connect (over limit) must be rejected
    connectPilotPeer(broadcaster, net, 0u);
    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 0u);
    auto ref = parseSendRefusal(net);
    CHECK(ref.msgId == static_cast<uint8_t>(fl::MsgId::ConnectRefusal));
    CHECK(std::string_view(ref.reason) == "Connection rate limit exceeded. Try again later.");
}

TEST_CASE("WorldBroadcaster: rate limit resets after window expires", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setRateLimitParams(2, 5, 3);

    net.peerAddresses[0] = "1.2.3.4:1000";
    // Fill window: 2 connects
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onDisconnect(0u);
    net.disconnectedPeers.clear();
    net.sends.clear();
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onDisconnect(0u);
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Advance clock past the window
    t.advance(std::chrono::seconds(6));

    // Should be allowed again
    connectPilotPeer(broadcaster, net, 0u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: rate limit tracks different IPs independently", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setRateLimitParams(2, 10, 3);

    // IP-A fills limit
    net.peerAddresses[0] = "1.1.1.1:1000";
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onDisconnect(0u);
    net.disconnectedPeers.clear();
    net.sends.clear();
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onDisconnect(0u);
    net.disconnectedPeers.clear();
    net.sends.clear();
    connectPilotPeer(broadcaster, net, 0u); // 3rd from IP-A: rejected
    REQUIRE(net.disconnectedPeers.size() == 1u);
    net.disconnectedPeers.clear();

    // IP-B (different IP) is unaffected
    net.peerAddresses[1] = "2.2.2.2:2000";
    connectPilotPeer(broadcaster, net, 1u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: null getPeerAddress does not crash rate limit", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setRateLimitParams(2, 10, 3);

    // peer 0 has no address entry — getPeerAddress returns nullptr
    connectPilotPeer(broadcaster, net, 0u); // must not crash
    // peer was not disconnected (unknown IP skips rate-limit and allowlist checks)
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: empty allowlist allows all IPs", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    net.peerAddresses[0] = "9.9.9.9:1000";
    connectPilotPeer(broadcaster, net, 0u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: IP on allowlist is permitted", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setAllowedAddresses({"1.2.3.4"});

    net.peerAddresses[0] = "1.2.3.4:1000";
    connectPilotPeer(broadcaster, net, 0u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: IP not on allowlist is rejected", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setAllowedAddresses({"9.9.9.9"});

    net.peerAddresses[0] = "1.2.3.4:1000";
    connectPilotPeer(broadcaster, net, 0u);
    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 0u);
    auto ref = parseSendRefusal(net);
    CHECK(ref.msgId == static_cast<uint8_t>(fl::MsgId::ConnectRefusal));
    CHECK(std::string_view(ref.reason) == "Access denied.");
}

TEST_CASE("WorldBroadcaster: setting empty allowlist re-enables all IPs", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setAllowedAddresses({"9.9.9.9"});
    // Clear allowlist
    broadcaster.setAllowedAddresses({});

    net.peerAddresses[0] = "1.2.3.4:1000";
    connectPilotPeer(broadcaster, net, 0u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: banned IP rejected even if on allowlist", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.banAddress("1.2.3.4");
    broadcaster.setAllowedAddresses({"1.2.3.4"});

    net.peerAddresses[0] = "1.2.3.4:1000";
    connectPilotPeer(broadcaster, net, 0u);
    REQUIRE(net.disconnectedPeers.size() == 1u);
}

TEST_CASE("WorldBroadcaster: per-IP limit of zero allows unlimited connections", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    // limit=0 = unlimited; connect three peers from the same IP

    net.peerAddresses[0] = "1.2.3.4:1001";
    net.peerAddresses[1] = "1.2.3.4:1002";
    net.peerAddresses[2] = "1.2.3.4:1003";
    connectPilotPeer(broadcaster, net, 0u);
    connectPilotPeer(broadcaster, net, 1u);
    connectPilotPeer(broadcaster, net, 2u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: per-IP limit allows last connection at limit", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setMaxConnectionsPerIp(2);

    net.peerAddresses[0] = "1.2.3.4:1001";
    net.peerAddresses[1] = "1.2.3.4:1002";
    connectPilotPeer(broadcaster, net, 0u);
    net.disconnectedPeers.clear();
    net.sends.clear();
    connectPilotPeer(broadcaster, net, 1u); // count was 1, limit is 2 — allowed
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: per-IP limit rejects connection over limit", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setMaxConnectionsPerIp(2);

    net.peerAddresses[0] = "1.2.3.4:1001";
    net.peerAddresses[1] = "1.2.3.4:1002";
    net.peerAddresses[2] = "1.2.3.4:1003";
    connectPilotPeer(broadcaster, net, 0u);
    connectPilotPeer(broadcaster, net, 1u);
    net.disconnectedPeers.clear();
    net.sends.clear();
    connectPilotPeer(broadcaster, net, 2u); // count is 2, limit is 2 — rejected
    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 2u);
    auto ref = parseSendRefusal(net);
    CHECK(ref.msgId == static_cast<uint8_t>(fl::MsgId::ConnectRefusal));
    CHECK(std::string_view(ref.reason) == "Too many connections from your address.");
}

TEST_CASE("WorldBroadcaster: per-IP limit counts only matching-IP peers", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setMaxConnectionsPerIp(2);

    net.peerAddresses[0] = "1.2.3.4:1001";
    net.peerAddresses[1] = "1.2.3.4:1002";
    net.peerAddresses[2] = "5.5.5.5:1001";
    net.peerAddresses[3] = "5.5.5.5:1002";
    connectPilotPeer(broadcaster, net, 0u);
    connectPilotPeer(broadcaster, net, 1u);
    connectPilotPeer(broadcaster, net, 2u);
    connectPilotPeer(broadcaster, net, 3u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: null getPeerAddress does not crash per-IP limit check", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setMaxConnectionsPerIp(1);

    // peer 0 has no address entry → getPeerAddress returns nullptr
    connectPilotPeer(broadcaster, net, 0u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: per-IP limit slot freed after disconnect allows reconnect",
          "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setMaxConnectionsPerIp(1);

    net.peerAddresses[0] = "1.2.3.4:1001";
    net.peerAddresses[1] = "1.2.3.4:1002";
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onDisconnect(0u); // frees the slot
    net.disconnectedPeers.clear();
    net.sends.clear();
    connectPilotPeer(broadcaster, net, 1u); // count is now 0 — should be allowed
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: per-IP limit counts IPv4-mapped IPv6 as same address", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setMaxConnectionsPerIp(1);

    net.peerAddresses[0] = "1.2.3.4:1001";
    net.peerAddresses[1] = "[::ffff:1.2.3.4]:1002"; // IPv4-mapped IPv6 — same host
    connectPilotPeer(broadcaster, net, 0u);
    net.disconnectedPeers.clear();
    net.sends.clear();
    connectPilotPeer(broadcaster, net, 1u); // normalizeIp maps ::ffff:1.2.3.4 → 1.2.3.4 → rejected
    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 1u);
    auto ref = parseSendRefusal(net);
    CHECK(ref.msgId == static_cast<uint8_t>(fl::MsgId::ConnectRefusal));
    CHECK(std::string_view(ref.reason) == "Too many connections from your address.");
}

TEST_CASE("WorldBroadcaster: onConnect sends only MsgHello; admission waits for MsgConnectRequest (#853)",
          "[world_broadcaster][handshake]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.onConnect(0u);
    // Only the version handshake so far — no spawn, no ConnectAck (the old flow acked here).
    REQUIRE(net.sends.size() == 1u);
    CHECK(parseSendHello(net).msgId == static_cast<uint8_t>(fl::MsgId::Hello));
    broadcaster.onTick(1.0 / 60.0, 1u);
    CHECK(em.liveCount() == 0u); // not-yet-admitted peer has no entity

    // The client's request admits it: spawn + ConnectAck(grantedRole=Pilot, assigned entity).
    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
    broadcaster.onReceive(0u, &req, sizeof(req));
    auto ack = parseSendAck(net);
    CHECK(ack.msgId == static_cast<uint8_t>(fl::MsgId::ConnectAck));
    CHECK(ack.grantedRole == static_cast<uint8_t>(fl::PeerRole::Pilot));
    CHECK(ack.assignedEntityGen != 0u);
    broadcaster.onTick(1.0 / 60.0, 2u);
    CHECK(em.liveCount() == 1u);
}

TEST_CASE("WorldBroadcaster: a duplicate MsgConnectRequest is ignored (no re-spawn) (#853)",
          "[world_broadcaster][handshake]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    connectPilotPeer(broadcaster, net, 0u); // onConnect + first request -> admitted
    broadcaster.onTick(1.0 / 60.0, 1u);
    REQUIRE(em.liveCount() == 1u);

    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
    broadcaster.onReceive(0u, &req, sizeof(req)); // duplicate — must be ignored
    broadcaster.onTick(1.0 / 60.0, 2u);
    CHECK(em.liveCount() == 1u); // no second entity spawned
}

TEST_CASE("WorldBroadcaster: pilot flies the requested registered type, clamped to the allowlist (#834)",
          "[world_broadcaster][handshake]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    const uint32_t jetIdx = registry.registerType(makeDebugDef("fl-base:f5e"));
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    connectPilotPeer(broadcaster, net, 0u, "fl-base:f5e"); // request a registered type
    auto ack = parseSendAck(net);
    REQUIRE(ack.assignedEntityGen != 0u);
    const fl::EntityState* s = em.get(fl::EntityId{ack.assignedEntityIdx, ack.assignedEntityGen});
    REQUIRE(s != nullptr);
    CHECK(s->typeIndex == jetIdx);
}

TEST_CASE("WorldBroadcaster: an unregistered requested type falls back to the server default (#834)",
          "[world_broadcaster][handshake]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    const uint32_t debugIdx = registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    connectPilotPeer(broadcaster, net, 0u, "no-such:aircraft"); // not registered -> fall back
    auto ack = parseSendAck(net);
    REQUIRE(ack.assignedEntityGen != 0u);
    const fl::EntityState* s = em.get(fl::EntityId{ack.assignedEntityIdx, ack.assignedEntityGen});
    REQUIRE(s != nullptr);
    CHECK(s->typeIndex == debugIdx); // builtin:debug-entity default
}

TEST_CASE("WorldBroadcaster: empty requested type uses the [world] player_entity_type default (#834)",
          "[world_broadcaster][handshake]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    const uint32_t jetIdx = registry.registerType(makeDebugDef("fl-base:f5e"));
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::WorldBroadcasterConfig cfg;
    cfg.playerEntityType = "fl-base:f5e";
    broadcaster.applyConfig(cfg);

    connectPilotPeer(broadcaster, net, 0u, ""); // empty request -> server default
    auto ack = parseSendAck(net);
    REQUIRE(ack.assignedEntityGen != 0u);
    const fl::EntityState* s = em.get(fl::EntityId{ack.assignedEntityIdx, ack.assignedEntityGen});
    REQUIRE(s != nullptr);
    CHECK(s->typeIndex == jetIdx);
}

TEST_CASE("WorldBroadcaster: a client missing a required pack is warned but still admitted (#872)",
          "[world_broadcaster][handshake][packs]") {
    RecordingLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::WorldBroadcasterConfig cfg;
    cfg.requiredPacks = {"fl-base"};
    broadcaster.applyConfig(cfg);

    connectPilotWithPacks(broadcaster, 0u, {"other-pack"}); // does NOT have fl-base

    auto ack = parseSendAck(net);
    CHECK(ack.assignedEntityGen != 0u); // warn-only: still admitted
    bool warned = false;
    for (const std::string& w : logger.messages(LogLevel::Warn))
        if (w.find("fl-base") != std::string::npos)
            warned = true;
    CHECK(warned);
}

TEST_CASE("WorldBroadcaster: a client with the required pack is admitted with no missing-pack warning (#872)",
          "[world_broadcaster][handshake][packs]") {
    RecordingLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::WorldBroadcasterConfig cfg;
    cfg.requiredPacks = {"fl-base"};
    broadcaster.applyConfig(cfg);

    connectPilotWithPacks(broadcaster, 0u, {"fl-base", "theater-pack"});

    auto ack = parseSendAck(net);
    CHECK(ack.assignedEntityGen != 0u);
    for (const std::string& w : logger.messages(LogLevel::Warn))
        CHECK(w.find("missing required content pack") == std::string::npos);
}

TEST_CASE("WorldBroadcaster: warn policy notifies the admitted client of the missing packs (#872)",
          "[world_broadcaster][handshake][packs]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::WorldBroadcasterConfig cfg;
    cfg.requiredPacks = {"fl-base"};
    cfg.requiredPackPolicy = fl::RequiredPackPolicy::Warn; // default, made explicit
    broadcaster.applyConfig(cfg);

    connectPilotWithPacks(broadcaster, 0u, {"other-pack"});

    CHECK(parseSendAck(net).assignedEntityGen != 0u); // admitted
    // A MsgServerNotice carrying the missing list reaches the client so the mismatch is visible.
    bool notified = false;
    for (const auto& pkt : net.sends) {
        if (!pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::ServerNotice)) {
            fl::MsgServerNotice sn{};
            std::memcpy(&sn, pkt.data(), sizeof(sn));
            if (std::string(sn.text).find("fl-base") != std::string::npos)
                notified = true;
        }
    }
    CHECK(notified);
}

TEST_CASE("WorldBroadcaster: refuse policy disconnects a client missing a required pack (#872)",
          "[world_broadcaster][handshake][packs]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::WorldBroadcasterConfig cfg;
    cfg.requiredPacks = {"fl-base"};
    cfg.requiredPackPolicy = fl::RequiredPackPolicy::Refuse;
    broadcaster.applyConfig(cfg);

    connectPilotWithPacks(broadcaster, 0u, {"other-pack"});

    auto ref = findSentRefusal(net);
    CHECK(ref.code == static_cast<uint8_t>(fl::ConnectRefusalCode::MissingRequiredPack));
    CHECK(std::string(ref.reason).find("fl-base") != std::string::npos); // reason names the missing pack
    CHECK(!anySentIs(net, fl::MsgId::ConnectAck));                       // never admitted
    CHECK(std::find(net.disconnectedPeers.begin(), net.disconnectedPeers.end(), 0u) != net.disconnectedPeers.end());

    broadcaster.onTick(1.0 / 60.0, 1u);
    CHECK(em.liveCount() == 0u); // no entity spawned for a refused peer
}

TEST_CASE("WorldBroadcaster: refuse policy enforces a pinned pack version (#872)",
          "[world_broadcaster][handshake][packs]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::WorldBroadcasterConfig cfg;
    cfg.requiredPacks = {fl::RequiredPack{"fl-base", "1.2"}};
    cfg.requiredPackPolicy = fl::RequiredPackPolicy::Refuse;
    broadcaster.applyConfig(cfg);

    // Right pack id, wrong version -> refused, reason names the required version.
    connectPilotWithPackVersions(broadcaster, 0u, {{"fl-base", "1.1"}});
    auto ref = findSentRefusal(net);
    CHECK(ref.code == static_cast<uint8_t>(fl::ConnectRefusalCode::MissingRequiredPack));
    CHECK(std::string(ref.reason).find("fl-base@1.2") != std::string::npos);

    // A second peer on the matching version is admitted.
    net.sends.clear();
    connectPilotWithPackVersions(broadcaster, 1u, {{"fl-base", "1.2"}});
    CHECK(anySentIs(net, fl::MsgId::ConnectAck));
}

TEST_CASE("WorldBroadcaster: allow-placeholder policy admits a client missing a pack without warning (#872)",
          "[world_broadcaster][handshake][packs]") {
    RecordingLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::WorldBroadcasterConfig cfg;
    cfg.requiredPacks = {"fl-base"};
    cfg.requiredPackPolicy = fl::RequiredPackPolicy::AllowPlaceholder;
    broadcaster.applyConfig(cfg);

    connectPilotWithPacks(broadcaster, 0u, {"other-pack"});

    CHECK(parseSendAck(net).assignedEntityGen != 0u); // admitted
    for (const std::string& w : logger.messages(LogLevel::Warn))
        CHECK(w.find("missing required content pack") == std::string::npos); // no Warn under allow-placeholder
    CHECK(!anySentIs(net, fl::MsgId::ServerNotice));                         // and no client-facing notice
}

TEST_CASE("WorldBroadcaster: an observer connects with no entity and grantedRole=Observer (#857)",
          "[world_broadcaster][handshake][observer]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.onConnect(0u);
    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Observer);
    broadcaster.onReceive(0u, &req, sizeof(req));

    auto ack = parseSendAck(net);
    CHECK(ack.grantedRole == static_cast<uint8_t>(fl::PeerRole::Observer));
    CHECK(ack.assignedEntityGen == 0u); // observer has no assigned entity
    broadcaster.onTick(1.0 / 60.0, 1u);
    CHECK(em.liveCount() == 0u); // observer spawned nothing
}

TEST_CASE("WorldBroadcaster: an observer request is refused when observers are disabled (#857)",
          "[world_broadcaster][observer]") {
    NullLogger logger;
    MockNetwork net;
    net.peerAddresses[0u] = "1.2.3.4";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::WorldBroadcasterConfig cfg;
    cfg.allowObservers = false;
    broadcaster.applyConfig(cfg);

    broadcaster.onConnect(0u);
    net.sends.clear(); // drop the MsgHello so only the refusal remains
    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Observer);
    broadcaster.onReceive(0u, &req, sizeof(req));

    REQUIRE_FALSE(net.disconnectedPeers.empty());
    CHECK(net.disconnectedPeers.back() == 0u);
    auto ref = parseSendRefusal(net);
    CHECK(ref.code == static_cast<uint8_t>(fl::ConnectRefusalCode::RoleDenied));
}

TEST_CASE("WorldBroadcaster: setBannedAddresses replaces existing ban set", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.banAddress("1.1.1.1");
    // Replace with a different set
    broadcaster.setBannedAddresses({"2.2.2.2"});

    // Old ban is gone — 1.1.1.1 can connect
    net.peerAddresses[0] = "1.1.1.1:1000";
    connectPilotPeer(broadcaster, net, 0u);
    CHECK(net.disconnectedPeers.empty());

    // New ban is active — 2.2.2.2 is rejected
    net.peerAddresses[1] = "2.2.2.2:2000";
    connectPilotPeer(broadcaster, net, 1u);
    REQUIRE(net.disconnectedPeers.size() == 1u);
}

TEST_CASE("WorldBroadcaster: getBannedAddresses returns current set", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.banAddress("1.2.3.4");
    broadcaster.banAddress("5.6.7.8");
    auto banned = broadcaster.getBannedAddresses();
    CHECK(banned.count("1.2.3.4") == 1u);
    CHECK(banned.count("5.6.7.8") == 1u);
    CHECK(banned.size() == 2u);
}

TEST_CASE("WorldBroadcaster: onConnect null getPeerAddress skips allowlist and rate limit",
          "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setAllowedAddresses({"9.9.9.9"});
    broadcaster.setRateLimitParams(1, 10, 3);

    // peer 0 has no address — getPeerAddress returns nullptr
    // With a non-empty allowlist, a known IP would be rejected.
    // But empty IP bypasses both allowlist and rate limit — no disconnect.
    connectPilotPeer(broadcaster, net, 0u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: rate limit prune preserves entries with unexpired timestamps",
          "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setRateLimitParams(10, 5, 3);

    fl::ManualClock t;
    broadcaster.setClock(t);

    net.peerAddresses[0] = "1.2.3.4:1000";
    // One recent connect
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onDisconnect(0u);
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Trigger prune by running 600 ticks (but clock hasn't advanced past window)
    for (int i = 0; i < 600; ++i)
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(i));

    // Entry was NOT pruned (timestamp still in window) — reconnect should increment counter
    connectPilotPeer(broadcaster, net, 0u); // 2nd connect — should succeed (limit is 10)
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: rate limit prune removes fully expired entries", "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setRateLimitParams(1, 5, 3);

    fl::ManualClock t;
    broadcaster.setClock(t);

    net.peerAddresses[0] = "1.2.3.4:1000";
    // One connect fills limit (limit=1)
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onDisconnect(0u);
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Advance clock past window
    t.advance(std::chrono::seconds(6));

    // Run 600 ticks to trigger prune (timestamps now all expired)
    for (int i = 0; i < 600; ++i)
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(i));

    // After prune, IP-A can connect again (counter reset by prune)
    connectPilotPeer(broadcaster, net, 0u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: MsgConnectAck planetRadiusKm is Earth radius by default", "[world_broadcaster][gravity]") {
    NullLogger log;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);

    connectPilotPeer(broadcaster, net, 0u);

    fl::MsgConnectAck ack = parseSendAck(net);
    CHECK(ack.planetRadiusKm == Catch::Approx(6371.f).epsilon(1e-4f));
}

TEST_CASE("WorldBroadcaster: setGravityField propagates planetRadiusKm to MsgConnectAck",
          "[world_broadcaster][gravity]") {
    NullLogger log;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);

    broadcaster.setGravityField(fl::CentralGravityField::earthInstance(), 6371.f);
    connectPilotPeer(broadcaster, net, 0u);

    fl::MsgConnectAck ack = parseSendAck(net);
    CHECK(ack.planetRadiusKm == Catch::Approx(6371.f).epsilon(1e-4f));
}

TEST_CASE("WorldBroadcaster: reconnect within the grace window restores team + score (#524)",
          "[world_broadcaster][combat]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    em.addEventHandler(&broadcaster);
    broadcaster.setReconnectGraceTicks(300);

    connectPilotWithGuid(broadcaster, 0u, "player-guid-abc");
    const auto ack = parseSendAck(net);
    broadcaster.onTick(1.0 / 60.0, 1u);
    em.applyDamage({ack.assignedEntityIdx, ack.assignedEntityGen}, 200.f, fl::EntityId::null());
    broadcaster.onTick(1.0 / 60.0, 2u);

    broadcaster.onDisconnect(0u); // snapshots {losses:1} under the guid

    net.perPeerSends.clear();
    connectPilotWithGuid(broadcaster, 1u, "player-guid-abc"); // reconnect as a new peer id, same guid
    broadcaster.onTick(1.0 / 60.0, 3u);
    const auto stats = lastStatsFor(net, 1u);
    REQUIRE(stats.has_value());
    CHECK(stats->b == 1u); // losses restored (Stats: a=kills, b=losses)
}

TEST_CASE("WorldBroadcaster: a pilot binds to a mission player slot (type/faction/spawn)", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    registry.registerType(makeDebugDef("mission:fighter"));

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::WorldBroadcaster::MissionSpawnSlot slot;
    slot.entityType = "mission:fighter";
    slot.factionIndex = 3;
    slot.pos[0] = 1000.0;
    slot.pos[1] = 2000.0;
    slot.pos[2] = 3000.0;
    broadcaster.setMissionPlayerSlots({slot});

    // A client requesting a DIFFERENT type is overridden by the slot (the slot pins what/where it flies).
    connectPilotPeer(broadcaster, net, 0u, "builtin:debug-entity");

    const fl::EntityState* e = slotPeerEntity(em, 0u);
    REQUIRE(e != nullptr);
    CHECK(e->typeIndex == registry.indexById("mission:fighter"));
    CHECK(e->factionIndex == 3);
    CHECK(e->transform.pos[0] == 1000.0);
    CHECK(e->transform.pos[1] == 2000.0);
    CHECK(e->transform.pos[2] == 3000.0);
}

TEST_CASE("WorldBroadcaster: a mission slot frees on disconnect and rebinds the next pilot", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    registry.registerType(makeDebugDef("mission:fighter"));

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    fl::WorldBroadcaster::MissionSpawnSlot slot;
    slot.entityType = "mission:fighter";
    slot.factionIndex = 2;
    slot.pos[0] = 500.0;
    slot.pos[1] = 750.0;
    slot.pos[2] = -250.0;
    broadcaster.setMissionPlayerSlots({slot});

    connectPilotPeer(broadcaster, net, 0u);
    REQUIRE(slotPeerEntity(em, 0u) != nullptr);
    broadcaster.onTick(1.0 / 60.0, 1); // reap nothing; just advance
    broadcaster.onDisconnect(0u);      // frees the slot

    connectPilotPeer(broadcaster, net, 1u);
    const fl::EntityState* e = slotPeerEntity(em, 1u);
    REQUIRE(e != nullptr);
    CHECK(e->factionIndex == 2); // rebound to the freed slot
    CHECK(e->transform.pos[0] == 500.0);
}

TEST_CASE("WorldBroadcaster: banning an IP disconnects observers and unadmitted peers too (#1069)",
          "[world_broadcaster][security]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    net.peerAddresses[0] = "9.9.9.9:1000"; // pilot (has an entity)
    net.peerAddresses[1] = "9.9.9.9:1001"; // observer (no entity)
    net.peerAddresses[2] = "9.9.9.9:1002"; // connected but never sent a MsgConnectRequest
    connectPilotPeer(broadcaster, net, 0u);
    connectObserverPeer(broadcaster, net, 1u);
    broadcaster.onConnect(2u);

    broadcaster.banAddress("9.9.9.9");

    // The kick used to walk m_peerEntities — peers WITH an entity — so a ban reached the pilot and
    // left the observer and the unadmitted peer connected, which is the opposite of what a ban means.
    CHECK(std::count(net.disconnectedPeers.begin(), net.disconnectedPeers.end(), 0u) == 1);
    CHECK(std::count(net.disconnectedPeers.begin(), net.disconnectedPeers.end(), 1u) == 1);
    CHECK(std::count(net.disconnectedPeers.begin(), net.disconnectedPeers.end(), 2u) == 1);
}

TEST_CASE("WorldBroadcaster: a re-ack omits the unchanged entity-type table (#1070)", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    registry.registerType(makeCrewBomberDef());
    fl::WeaponRegistry weapons;
    weapons.registerWeapon(makeRktWeapon());
    fl::EntityManager em(logger, registry);
    fl::WorldQueries q_wb;
    q_wb.seatControllerFactory = [](const fl::SeatDef&, uint8_t,
                                    const fl::SeatBotContext&) -> std::unique_ptr<fl::ISeatController> {
        return nullptr;
    };
    fl::WorldBroadcaster wb(em, registry, net, logger, nullptr, std::move(q_wb));
    fl::ManualClock clock;
    wb.setClock(clock);
    wb.setWeaponRegistry(&weapons);
    wb.setGroundElevation(0.f);

    fl::EntityTransform t{};
    t.pos[1] = 800.0;
    t.quat[3] = 1.f;
    const fl::EntityId bomber = em.spawn("test:crewbomber", t);
    wb.registerController(bomber, std::make_unique<StillCtl>(), nullptr, 0.f);

    connectPilotPeer(wb, net, 1u);

    // The FIRST ack always carries the table — a fresh peer has nothing cached.
    auto acks = connectAcksFor(net, 1u);
    REQUIRE(acks.size() == 1u);
    fl::MsgConnectAck first{};
    REQUIRE(fl::readMsg(acks[0].data(), acks[0].size(), first));
    CHECK(first.typeCount == registry.typeCount());
    CHECK_FALSE(ackSkipsTypes(acks[0]));
    const std::size_t firstAckBytes = acks[0].size();

    // A seat hop re-acks. The table is unchanged, so this one carries no records.
    net.perPeerSends.clear();
    const fl::MsgSeatRequest join = joinReq(bomber, 1);
    wb.onReceive(1u, &join, sizeof(join));

    acks = connectAcksFor(net, 1u);
    REQUIRE(acks.size() == 1u);
    fl::MsgConnectAck second{};
    REQUIRE(fl::readMsg(acks[0].data(), acks[0].size(), second));
    CHECK(second.typeCount == 0u);
    CHECK(ackSkipsTypes(acks[0]));
    // The re-ack is a fixed header plus one 4-byte tag, not ~23 KB of records.
    CHECK(acks[0].size() < firstAckBytes);
    CHECK(acks[0].size() <= sizeof(fl::MsgConnectAck) + 8u);

    // Everything that identifies the peer still travels on the skipped ack: this is a payload
    // omission, not a degraded ack.
    CHECK(second.peerId == 1u);
    CHECK(second.planetRadiusKm == first.planetRadiusKm);
}

TEST_CASE("WorldBroadcaster: a changed type table is re-sent even to a peer that had one (#1070)",
          "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster wb(em, registry, net, logger);

    connectPilotPeer(wb, net, 1u);
    REQUIRE(connectAcksFor(net, 1u).size() == 1u);

    // Register a new type, then force a re-ack via an authority grant.
    fl::EntityDef extra;
    extra.id = "test:late-arrival";
    extra.name = "Late";
    extra.category = fl::ObjectCategory::AirVehicle;
    extra.maxHp = 50.f;
    registry.registerType(std::move(extra));

    net.perPeerSends.clear();
    fl::PeerAuthority auth;
    auth.caps = fl::kAllCaps;
    wb.setPeerAuthority(1u, auth);

    auto acks = connectAcksFor(net, 1u);
    REQUIRE(acks.size() == 1u);
    fl::MsgConnectAck ack{};
    REQUIRE(fl::readMsg(acks[0].data(), acks[0].size(), ack));
    CHECK(ack.typeCount == registry.typeCount()); // the new type must reach the client
    CHECK_FALSE(ackSkipsTypes(acks[0]));
}
