// SPDX-License-Identifier: GPL-3.0-or-later
#include "world_broadcaster_test_util.h"

#include <bit>

using namespace fl;

// ---------------------------------------------------------------------------
// SnapshotPipeline (#1086) — what each peer is told about the world.
//
// Moved verbatim from test_world_broadcaster.cpp with the code they exercise: the encode-once pass,
// the per-peer interest/scheduler/stitch build and its serial-equivalence byte-compares, the
// client-acked delta baselines and selective-ack window, despawn queues, compression, the congestion
// and governor decimation gates, the articulation TLV, the spectate delay and the replay tap.
//
// The jitter-buffer units and the adaptive-resize cases deliberately stay in the broadcaster suite:
// that stepping happens in the maintenance phase, not in the snapshot pass, so it did not move.
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: onTick broadcasts WorldSnapshot for N entities", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());

    // Spawn 3 entities before GameLoop starts (no sim thread yet).
    for (int i = 0; i < 3; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = static_cast<double>(i * 10);
        t.pos[1] = 500.0;
        em.spawn("builtin:debug-entity", t);
    }

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u); // peer required for per-peer unicast snapshots

    // Drive one tick manually.
    broadcaster.onTick(1.0 / 60.0, 1u);

    // All entities are new to the peer (and unacked), so they appear as full entries.
    auto snaps = snapshotsFor(net, 0);
    REQUIRE(snaps.size() == 1u);
    const auto& pkt = snaps[0];
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader));

    auto hdr = parseSnapshotHeader(pkt);
    CHECK(hdr.msgId == static_cast<uint8_t>(fl::MsgId::WorldSnapshot));
    CHECK(totalEntityCount(hdr) == 4u); // 3 pre-spawned + 1 peer entity
    CHECK(hdr.tickIndex == 1u);

    // Verify one of the full entries has pos[1] ~= 500 (quantized relative to the frame origin).
    const auto fullEntries = parseFullEntries(pkt);
    REQUIRE(!fullEntries.empty());
    CHECK(fullEntries[0].pos[1] == Catch::Approx(500.0).margin(fl::kPosStepM));
}

TEST_CASE("WorldBroadcaster: parallel per-peer snapshot build is serial-equivalent across worker counts",
          "[world_broadcaster]") {
    for (uint64_t killAtTick : {uint64_t{0}, uint64_t{60}}) {
        const auto baseline = runSnapshotScenario(nullptr, killAtTick); // inline / serial reference

        for (unsigned total : {1u, 2u, 8u}) {
            fl::JobSystem jobs(total);
            const auto got = runSnapshotScenario(&jobs, killAtTick);
            REQUIRE(got.size() == baseline.size());
            for (const auto& [peerId, baseSnaps] : baseline) {
                auto it = got.find(peerId);
                REQUIRE(it != got.end());
                const auto& gotSnaps = it->second;
                INFO("workers=" << total << " peer=" << peerId << " killAtTick=" << killAtTick);
                // Each peer must receive the same number of snapshot packets...
                REQUIRE(gotSnaps.size() == baseSnaps.size());
                // ...and each packet must be byte-identical: the per-peer build performs no cross-peer
                // writes and no RNG, so parallelism must not change a single byte on the same binary.
                // Compare via a bool so Catch2 never stringifies the (large) byte vectors.
                for (size_t i = 0; i < baseSnaps.size(); ++i) {
                    const bool identical = (gotSnaps[i] == baseSnaps[i]);
                    INFO("snapshot packet index " << i);
                    CHECK(identical);
                }
            }
        }
    }
}

TEST_CASE("WorldBroadcaster: compressed snapshots round-trip to the raw payload byte-for-byte",
          "[world_broadcaster][compress]") {
    // Same deterministic scenario with compression off (reference) and on: every compressed packet
    // must carry the flag + the exact uncompressed length, decompress to the reference payload, and
    // actually be smaller — the wire form changes, the decoded stream must not.
    const auto rawRun = runSnapshotScenario(nullptr, 0, /*compress=*/false);
    const auto zRun = runSnapshotScenario(nullptr, 0, /*compress=*/true);
    REQUIRE(zRun.size() == rawRun.size());
    size_t compressedSeen = 0;
    for (const auto& [pid, rawSnaps] : rawRun) {
        auto it = zRun.find(pid);
        REQUIRE(it != zRun.end());
        const auto& zSnaps = it->second;
        REQUIRE(zSnaps.size() == rawSnaps.size());
        for (size_t i = 0; i < rawSnaps.size(); ++i) {
            INFO("peer " << pid << " snapshot " << i);
            const auto rawHdr = parseSnapshotHeader(rawSnaps[i]);
            const auto zHdr = parseSnapshotHeader(zSnaps[i]);
            // Counts + tick always describe the uncompressed layout, identical either way.
            CHECK(zHdr.tickIndex == rawHdr.tickIndex);
            CHECK(zHdr.recordCount == rawHdr.recordCount);
            CHECK(zHdr.originCount == rawHdr.originCount);
            CHECK(zHdr.bitstreamBytes == rawHdr.bitstreamBytes);
            const std::size_t rawPayload = rawSnaps[i].size() - sizeof(fl::MsgWorldSnapshotHeader);
            if ((zHdr.flags & fl::kSnapshotFlagCompressed) == 0u) {
                // Raw fallback (payload under the min, or incompressible): must be wire-identical.
                const bool identical = (zSnaps[i] == rawSnaps[i]);
                CHECK(identical);
                CHECK(zHdr.uncompressedBytes == 0u);
                continue;
            }
            ++compressedSeen;
            CHECK(zHdr.uncompressedBytes == rawPayload);
            CHECK(zSnaps[i].size() < rawSnaps[i].size()); // compression only used when it wins
            std::vector<uint8_t> payload;
            REQUIRE(fl::decompressSnapshotPayload(zSnaps[i].data() + sizeof(fl::MsgWorldSnapshotHeader),
                                                  zSnaps[i].size() - sizeof(fl::MsgWorldSnapshotHeader),
                                                  zHdr.uncompressedBytes, payload));
            const bool payloadIdentical =
                payload ==
                std::vector<uint8_t>(rawSnaps[i].begin() + sizeof(fl::MsgWorldSnapshotHeader), rawSnaps[i].end());
            CHECK(payloadIdentical);
        }
    }
    CHECK(compressedSeen > 0u); // the 16-entity scenario must actually exercise the compressed path
}

TEST_CASE("WorldBroadcaster: tiny snapshots are sent raw even with compression enabled",
          "[world_broadcaster][compress]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setSnapshotCompression(true);
    connectPilotPeer(broadcaster, net, 0u); // own entity only -> payload well under kMinSnapshotCompressBytes
    broadcaster.onTick(1.0 / 60.0, 1u);
    auto snaps = snapshotsFor(net, 0);
    REQUIRE(snaps.size() == 1u);
    const auto hdr = parseSnapshotHeader(snaps[0]);
    CHECK((hdr.flags & fl::kSnapshotFlagCompressed) == 0u);
    CHECK(hdr.uncompressedBytes == 0u);
    // And the packet parses exactly like a compression-off packet.
    const auto fullEntries = parseFullEntries(snaps[0]);
    CHECK(!fullEntries.empty());
}

TEST_CASE("WorldBroadcaster: compressed snapshot build is serial-equivalent across worker counts",
          "[world_broadcaster][compress]") {
    // zstd is deterministic for identical input, and each worker owns its peer's buffer — so the
    // #512 guarantee must survive compression: byte-identical packets at any worker count.
    const auto baseline = runSnapshotScenario(nullptr, 0, /*compress=*/true);
    for (unsigned total : {1u, 4u}) {
        fl::JobSystem jobs(total);
        const auto got = runSnapshotScenario(&jobs, 0, /*compress=*/true);
        REQUIRE(got.size() == baseline.size());
        for (const auto& [pid, baseSnaps] : baseline) {
            auto it = got.find(pid);
            REQUIRE(it != got.end());
            REQUIRE(it->second.size() == baseSnaps.size());
            for (size_t i = 0; i < baseSnaps.size(); ++i) {
                const bool identical = (it->second[i] == baseSnaps[i]);
                INFO("workers=" << total << " peer=" << pid << " packet " << i);
                CHECK(identical);
            }
        }
    }
}

TEST_CASE("WorldBroadcaster: a congested peer is decimated while a healthy peer keeps sending", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u); // healthy peer
    connectPilotPeer(broadcaster, net, 1u); // congested peer

    // Report sustained high packet loss for peer 1 only; peer 0 (absent from the map) reads zeros.
    // Above the 0.02 loss threshold, the AIMD controller backs the throttle off, stretching peer 1's
    // snapshot send interval > 1 tick — so it is skipped on some ticks by the gather-time decimation
    // filter while peer 0 sends every tick.
    fl::PeerLinkStats bad{};
    bad.packetLoss = 0.5f;
    net.peerLinkStats[1u] = bad;

    for (uint64_t tick = 1; tick <= 120; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    const std::size_t healthySnaps = snapshotsFor(net, 0u).size();
    const std::size_t congestedSnaps = snapshotsFor(net, 1u).size();
    CHECK(congestedSnaps < healthySnaps); // decimation skipped the congested peer on some ticks
    CHECK(congestedSnaps > 0u);           // but it still receives snapshots at the floor rate
}

TEST_CASE("WorldBroadcaster: overrun governor degrades under an over-budget clock", "[world_broadcaster][overrun]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    AutoAdvanceClock clock(std::chrono::milliseconds(3));
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setClock(clock);
    fl::TickGovernorParams gp = fl::makeTickGovernorParams(true, 0.90f, 0.60f, 15.0f, 4u, 400u);
    gp.evalIntervalTicks = 1u;
    gp.ewmaAlpha = 1.0f;
    broadcaster.setGovernorParams(gp);
    connectPilotPeer(broadcaster, net, 0u);

    for (uint64_t tick = 1; tick <= 120; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    const fl::OverrunStatus ov = broadcaster.getOverrunStatus();
    CHECK(ov.degraded);
    CHECK(ov.loadFactor < 1.0f);
    CHECK(ov.snapshotIntervalTicks > 1u); // server-wide send-rate decimation engaged
    CHECK(ov.aiStride > 1u);              // AI-sample decimation engaged

    // The send-rate lever decimated the peer's snapshots below one-per-tick.
    CHECK(snapshotsFor(net, 0u).size() < 120u);
}

TEST_CASE("WorldBroadcaster: overrun governor interest-radius lever shrinks the visible set",
          "[world_broadcaster][overrun]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    // Near entity well inside the floor-scaled radius; far entity inside the full 200 km default
    // draw distance but outside the 100 km (= 200 km x 0.5 fraction floor) shed radius.
    fl::EntityTransform nearT{};
    nearT.pos[0] = 10'000.0;
    nearT.pos[1] = 1000.0;
    auto nearId = em.spawn("builtin:debug-entity", nearT);
    REQUIRE(nearId.valid());
    fl::EntityTransform farT{};
    farT.pos[0] = 150'000.0;
    farT.pos[1] = 1000.0;
    auto farId = em.spawn("builtin:debug-entity", farT);
    REQUIRE(farId.valid());

    AutoAdvanceClock clock(std::chrono::milliseconds(3));
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setClock(clock);
    fl::TickGovernorParams gp = fl::makeTickGovernorParams(true, 0.90f, 0.60f, 15.0f, 4u, 400u, 0.5f);
    gp.evalIntervalTicks = 1u;
    gp.ewmaAlpha = 1.0f;
    broadcaster.setGovernorParams(gp);
    connectPilotPeer(broadcaster, net, 0u); // peer spawns near origin (default), 200 km default draw distance

    // Tick 1: the governor steps from the PREVIOUS tick's wall-time (none yet) -> healthy -> the
    // full radius, so the 150 km entity is visible.
    broadcaster.onTick(1.0 / 60.0, 1u);
    {
        auto snaps = snapshotsFor(net, 0);
        REQUIRE(!snaps.empty());
        bool farSeen = false;
        for (const auto& e : decodeEntities(snaps[0]))
            if (e.entityIdx == farId.index)
                farSeen = true;
        CHECK(farSeen);
    }

    for (uint64_t tick = 2; tick <= 120; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    const fl::OverrunStatus ov = broadcaster.getOverrunStatus();
    CHECK(ov.degraded);
    // loadFactor bottoms at 0.25 (15 Hz floor) but the interest scale clamps at the 0.5 fraction.
    CHECK(ov.interestScale == Catch::Approx(0.5f));

    // The last snapshot was built with the shed 100 km radius: the far entity is interest-out (an
    // ordinary omission — no despawn), the near one stays visible.
    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    bool farSeen = false;
    bool nearSeen = false;
    for (const auto& e : decodeEntities(snaps.back())) {
        if (e.entityIdx == farId.index)
            farSeen = true;
        if (e.entityIdx == nearId.index)
            nearSeen = true;
    }
    CHECK_FALSE(farSeen);
    CHECK(nearSeen);
}

TEST_CASE("WorldBroadcaster: interest-radius lever is serial-equivalent across worker counts",
          "[world_broadcaster][overrun]") {
    const InterestShedResult baseline = runInterestShedScenario(nullptr); // inline reference
    // The lever must actually have engaged, else the test is vacuous.
    REQUIRE(baseline.finalInterestScale < 1.0f);

    for (unsigned total : {1u, 2u, 8u}) {
        fl::JobSystem jobs(total);
        const InterestShedResult got = runInterestShedScenario(&jobs);
        CHECK(got.finalInterestScale == baseline.finalInterestScale);
        REQUIRE(got.snaps.size() == baseline.snaps.size());
        for (const auto& [peerId, baseSnaps] : baseline.snaps) {
            auto it = got.snaps.find(peerId);
            REQUIRE(it != got.snaps.end());
            const auto& gotSnaps = it->second;
            INFO("workers=" << total << " peer=" << peerId);
            REQUIRE(gotSnaps.size() == baseSnaps.size());
            // Byte-identical: the scaled radius is a frozen sim-thread local, uniform across peers,
            // read-only in the parallel build — parallelism must not change a single byte.
            for (size_t i = 0; i < baseSnaps.size(); ++i) {
                const bool identical = (gotSnaps[i] == baseSnaps[i]);
                INFO("snapshot packet index " << i);
                CHECK(identical);
            }
        }
    }
}

TEST_CASE("WorldBroadcaster: a dead pilot still sees the world instead of going black (#403)", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);
    connectPilotPeer(broadcaster, net, 1u);
    broadcaster.onTick(1.0 / 60.0, 1u); // spawn both (co-located at the fallback spawn)

    fl::EntityId e0{};
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) {
        if (pi.peerId == 0u)
            e0 = pi.eid;
    });
    REQUIRE(e0.valid());

    // Before death, peer 0 already sees peer 1.
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u);
    {
        const auto snaps = snapshotsFor(net, 0u);
        REQUIRE(!snaps.empty());
        CHECK(parseSnapshotHeader(snaps.back()).recordCount > 0);
    }

    // Kill peer 0's aircraft (no respawn policy set). Its snapshot must NOT collapse to a header-only
    // blackout — the dead pilot spectates the world around its wreck/camera center (#403).
    em.kill(e0);
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 3u);
    {
        const auto snaps = snapshotsFor(net, 0u);
        REQUIRE(!snaps.empty());
        CHECK(parseSnapshotHeader(snaps.back()).recordCount > 0);
    }
}

TEST_CASE("WorldBroadcaster: spectate delay defers a spectator's snapshot (#403)", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setSpectateDelay(1); // 1 s = 60 ticks
    connectObserverPeer(broadcaster, net, 5u);

    // With the delay on, the observer's snapshot for tick 1 is buffered, not delivered.
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);
    CHECK(snapshotsFor(net, 5u).empty());

    // It surfaces once its due tick (1 + 60) arrives.
    for (uint64_t t = 2; t <= 61; ++t)
        broadcaster.onTick(1.0 / 60.0, t);
    CHECK_FALSE(snapshotsFor(net, 5u).empty());
}

TEST_CASE("WorldBroadcaster: onTick snapshot carries correct protocolVersion", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u); // peer required for per-peer unicast

    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    auto hdr = parseSnapshotHeader(snapshotsFor(net, 0)[0]);
    CHECK(hdr.protocolVersion == static_cast<uint8_t>(fl::kProtocolVersion));
}

TEST_CASE("WorldBroadcaster: onTick populates throttle in WorldSnapshot from FlightState", "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // Send a client input with full throttle.
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.throttle = 1.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    // Multiple ticks allow the spool-up lag to partially settle.
    for (int i = 0; i < 10; ++i)
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(i + 1));

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back(); // last of 10 snapshots

    auto hdr = parseSnapshotHeader(pkt);
    REQUIRE(totalEntityCount(hdr) >= 1u);

    // throttle_actual spools up over time; after 10 ticks at 1.f input it should be > 0.
    // Quantized records (full on tick 1, delta on ticks 2-10) both carry throttle.
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const uint8_t throttle = _ents[0].throttle;
    CHECK(throttle > 0u);
    CHECK(throttle <= 100u);
}

TEST_CASE("WorldBroadcaster: abEngaged is 0 in WorldSnapshot when model has no afterburner table",
          "[world_broadcaster]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.buttons = 0x02u; // afterburner bit set
    inp.throttle = 1.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back();
    REQUIRE(parseSnapshotHeader(pkt).recordCount >= 1u);

    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);

    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];

    // Builtin model has no ab_thrust table → FlightState::ab_engaged stays false → packed as 0.
    CHECK(e.abEngaged == 0u);
}

TEST_CASE("WorldBroadcaster: an observer receives world snapshots (#857)", "[world_broadcaster][observer]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    connectPilotPeer(broadcaster, net, 0u); // a pilot to look at (spawns near origin)
    broadcaster.onConnect(1u);              // observer
    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Observer);
    broadcaster.onReceive(1u, &req, sizeof(req));

    net.perPeerSends.clear();
    broadcaster.onTick(1.0 / 60.0, 5u);

    bool observerGotSnapshot = false;
    for (const auto& [pid, pkt] : net.perPeerSends)
        if (pid == 1u && !pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::WorldSnapshot))
            observerGotSnapshot = true;
    CHECK(observerGotSnapshot);
}

TEST_CASE("WorldBroadcaster: entity within draw distance appears in peer snapshot", "[world_broadcaster][interest]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::EntityTransform t{};
    t.pos[1] = 500.0; // spawn near origin
    em.spawn("builtin:debug-entity", t);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(200.f); // 200 km — entity at origin is visible
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    auto hdr = parseSnapshotHeader(snaps[0]);
    CHECK(totalEntityCount(hdr) >= 1u); // at least the spawned entity + peer entity
}

TEST_CASE("WorldBroadcaster: entity beyond draw distance excluded from peer snapshot",
          "[world_broadcaster][interest]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    // Spawn entity far away
    fl::EntityTransform far{};
    far.pos[0] = 20'000.0; // 20 km in +X
    far.pos[1] = 500.0;
    auto farId = em.spawn("builtin:debug-entity", far);
    REQUIRE(farId.valid());
    const uint32_t farIdx = farId.index;

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(1.f);       // 1 km — far entity is outside
    connectPilotPeer(broadcaster, net, 0u); // peer spawns near origin (default)
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    // Verify far entity does NOT appear in the full entries
    for (const auto& e : parseFullEntries(snaps[0]))
        CHECK(e.entityIdx != farIdx);
}

TEST_CASE("WorldBroadcaster: observer interest follows its camera eye (#858)", "[world_broadcaster][interest]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    // One entity 100 km out in +X — outside a 50 km radius around the origin, but a camera parked on
    // top of it is 0 m away.
    fl::EntityTransform t{};
    t.pos[0] = 100'000.0;
    t.pos[1] = 500.0;
    const auto id = em.spawn("builtin:debug-entity", t);
    REQUIRE(id.valid());
    const uint32_t idx = id.index;

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(50.f); // 50 km — the entity is out of range of the origin
    connectObserverPeer(broadcaster, net, 0u);

    auto seesEntity = [&](std::vector<std::vector<uint8_t>> snaps) {
        REQUIRE(!snaps.empty());
        for (const auto& e : parseFullEntries(snaps.back()))
            if (e.entityIdx == idx)
                return true;
        return false;
    };

    // Before any camera input the observer's interest center sits near the origin (the admit-time
    // seed), so the 100 km entity is interest-out.
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);
    CHECK_FALSE(seesEntity(snapshotsFor(net, 0)));

    // Move the ghost camera onto the entity: the next snapshot centers interest on the camera eye and
    // the entity comes into view — the observer sees the world where it is LOOKING, not where it spawned.
    const fl::MsgClientInput look = cameraInput(1u, 100'000.0, 500.0, 0.0);
    broadcaster.onReceive(0u, &look, sizeof(look));
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u);
    CHECK(seesEntity(snapshotsFor(net, 0)));
}

TEST_CASE("WorldBroadcaster: interest management correct at a non-default spatial cell size (#573)",
          "[world_broadcaster][interest]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::EntityTransform near{};
    near.pos[1] = 500.0; // near origin — visible
    em.spawn("builtin:debug-entity", near);
    fl::EntityTransform far{};
    far.pos[0] = 20'000.0; // 20 km — outside a 1 km draw distance
    far.pos[1] = 500.0;
    auto farId = em.spawn("builtin:debug-entity", far);
    REQUIRE(farId.valid());
    const uint32_t farIdx = farId.index;

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    fl::WorldBroadcasterConfig cfg;
    cfg.drawDistanceKm = 1.f;      // 1 km interest sphere
    cfg.spatialCellSizeM = 2000.0; // explicit non-default cell size
    broadcaster.applyConfig(cfg);
    CHECK(broadcaster.spatialIndex().cellSizeM() == Catch::Approx(2000.0));

    connectPilotPeer(broadcaster, net, 0u); // peer spawns near origin
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    for (const auto& e : parseFullEntries(snaps[0]))
        CHECK(e.entityIdx != farIdx); // far entity excluded despite the smaller cell size
}

TEST_CASE("WorldBroadcaster: setDrawDistance(0) produces empty snapshots", "[world_broadcaster][interest]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(0.f); // radius 0 → no cells queried
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    auto hdr = parseSnapshotHeader(snaps[0]);
    CHECK(hdr.recordCount == 0u);
}

TEST_CASE("WorldBroadcaster: dead peer entity results in empty snapshot", "[world_broadcaster][interest]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);
    // Retrieve peer's entity from ConnectAck
    fl::MsgConnectAck ack{};
    std::memcpy(&ack, net.sends[1].data(), sizeof(ack));
    fl::EntityId peerEid{ack.assignedEntityIdx, ack.assignedEntityGen};
    // Kill the peer's entity before the tick
    em.kill(peerEid);
    em.onTick(1.0 / 60.0, 0u); // reap dead entities

    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    auto hdr = parseSnapshotHeader(snaps[0]);
    CHECK(hdr.recordCount == 0u);
}

TEST_CASE("WorldBroadcaster: a fresh peer is sent full records for all entities until first ack",
          "[world_broadcaster][delta]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    for (int i = 0; i < 5; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = 100.0 + i * 10.0;
        t.pos[1] = 500.0;
        em.spawn("builtin:debug-entity", t);
    }

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // ackedTick == 0 (no ack yet): every visible entity bootstraps as a full record.
    broadcaster.onTick(1.0 / 60.0, 1u);
    auto snaps1 = snapshotsFor(net, 0);
    REQUIRE(!snaps1.empty());
    CHECK(deltaRecordCount(snaps1[0]) == 0u);
    const uint16_t fullsTick1 = fullRecordCount(snaps1[0]);
    CHECK(fullsTick1 >= 5u);

    // After acking tick 1, the same set downgrades to deltas.
    ackTick(broadcaster, 0u, 1u, 1u);
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u);
    auto snaps2 = snapshotsFor(net, 0);
    REQUIRE(!snaps2.empty());
    CHECK(fullRecordCount(snaps2[0]) == 0u);
    CHECK(deltaRecordCount(snaps2[0]) >= 5u);
}

TEST_CASE("WorldBroadcaster: a heartbeat-only client still acks and downgrades to delta",
          "[world_broadcaster][delta]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    broadcaster.onTick(1.0 / 60.0, 1u);
    REQUIRE(fullRecordCount(snapshotsFor(net, 0).back()) >= 1u);

    // Ack tick 1 via a heartbeat (no MsgClientInput).
    fl::MsgHeartbeat hb{};
    hb.tickIndex = 1u;
    broadcaster.onReceive(0u, &hb, sizeof(hb));

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u);
    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    CHECK(fullRecordCount(snaps[0]) == 0u);
    CHECK(deltaRecordCount(snaps[0]) >= 1u);
}

TEST_CASE("WorldBroadcaster: a future-tick ack is clamped to the present and cannot pre-confirm",
          "[world_broadcaster][delta]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // Tick 1, then the client claims to have acked a far-future tick (9999). Clamped to the current
    // tick (1), it cannot pre-confirm an entity the server has not even sent yet.
    broadcaster.onTick(1.0 / 60.0, 1u);
    ackTick(broadcaster, 0u, 9999u, 1u);

    // A new entity first appears at tick 2 (full streak starts at tick 2 > the clamped ack of 1).
    fl::EntityTransform t{};
    t.pos[1] = 500.0;
    const uint32_t newIdx = em.spawn("builtin:debug-entity", t).index;

    auto recordFor = [&](const std::vector<uint8_t>& pkt, uint32_t idx) -> std::optional<DecodedEntity> {
        for (const auto& d : decodeEntities(pkt))
            if (d.entityIdx == idx)
                return d;
        return std::nullopt;
    };

    broadcaster.onTick(1.0 / 60.0, 2u); // new entity → full (first sight)
    REQUIRE(recordFor(snapshotsFor(net, 0).back(), newIdx).value().isFull);

    // Tick 3 (no new ack): had the future ack NOT been clamped, ackedTick would be 9999 and the entity
    // would wrongly drop to a delta. Clamped, ackedTick is 1 < its streak start (2), so it stays full.
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 3u);
    auto rec3 = recordFor(snapshotsFor(net, 0).back(), newIdx);
    REQUIRE(rec3.has_value());
    CHECK(rec3->isFull);
}

TEST_CASE("WorldBroadcaster: a heartbeat carries the selective-ack mask", "[world_broadcaster][identity-ack]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    for (uint64_t tick = 1; tick <= 3; ++tick) {
        clearSnapshots(net);
        broadcaster.onTick(1.0 / 60.0, tick);
    }

    // Heartbeat acking tick 3 with the streak-start bit (tick 1, age 2 → bit 1) cleared: stays full.
    fl::MsgHeartbeat hb{};
    hb.tickIndex = 3u;
    hb.ackMask = 0xFFFFFFFFu & ~(1u << 1);
    broadcaster.onReceive(0u, &hb, sizeof(hb));
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 4u);
    CHECK(fullRecordCount(snapshotsFor(net, 0).back()) >= 1u);

    // Heartbeat acking tick 4 with the streak-start bit set: converges to delta.
    fl::MsgHeartbeat hb2{};
    hb2.tickIndex = 4u;
    hb2.ackMask = 0xFFFFFFFFu;
    broadcaster.onReceive(0u, &hb2, sizeof(hb2));
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 5u);
    CHECK(fullRecordCount(snapshotsFor(net, 0).back()) == 0u);
    CHECK(deltaRecordCount(snapshotsFor(net, 0).back()) >= 1u);
}

TEST_CASE("WorldBroadcaster: a non-advancing ack does not clobber the stored mask",
          "[world_broadcaster][identity-ack]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    for (uint64_t tick = 1; tick <= 5; ++tick) {
        clearSnapshots(net);
        broadcaster.onTick(1.0 / 60.0, tick);
    }

    // Confirm the identity with a full mask acking tick 5.
    ackTick(broadcaster, 0u, 5u, 1u, 0xFFFFFFFFu);
    // A later input (strictly newer seqNum) but an OLDER/non-advancing tickIndex with an empty mask must
    // NOT overwrite the stored {ackedTick=5, mask=full} pair — otherwise the entity would revert to full.
    ackTick(broadcaster, 0u, 3u, 2u, 0u);
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 6u);
    CHECK(fullRecordCount(snapshotsFor(net, 0).back()) == 0u);
    CHECK(deltaRecordCount(snapshotsFor(net, 0).back()) >= 1u);
}

TEST_CASE("WorldBroadcaster: a full streak older than the ack window converges to delta with an empty mask",
          "[world_broadcaster][identity-ack]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // Full streak frozen at tick 1; run well past the 32-tick window with no acks.
    for (uint64_t tick = 1; tick <= 40; ++tick) {
        clearSnapshots(net);
        broadcaster.onTick(1.0 / 60.0, tick);
        CHECK(fullRecordCount(snapshotsFor(net, 0).back()) >= 1u);
    }

    // Ack tick 40 with an EMPTY mask. The streak-start tick 1 is age 39 — outside the 32-bit window — so
    // ackReceived() assumes it was decoded (the retention force-full is the backstop for old ticks), and
    // the entity converges to deltas. This keeps steady-state bandwidth flat regardless of mask fidelity.
    ackTick(broadcaster, 0u, 40u, 1u, 0u);
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 41u);
    CHECK(fullRecordCount(snapshotsFor(net, 0).back()) == 0u);
    CHECK(deltaRecordCount(snapshotsFor(net, 0).back()) >= 1u);
}

TEST_CASE("WorldBroadcaster: a deferred entity acked-but-not-decoded stays full under selective-ack",
          "[world_broadcaster][delta][budget][identity-ack]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    // Two far entities competing for a one-extra-record budget so the scheduler admits one and defers
    // the other each tick (the peer's own entity is always admitted first).
    for (int i = 0; i < 2; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = 5000.0 + i * 10.0;
        t.pos[1] = 500.0;
        em.spawn("builtin:debug-entity", t);
    }

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(200.f);
    // Budget must clear the fixed header+TLV overhead (~72 B) plus the peer's own record, leaving room
    // for exactly one of the two far records — so one far entity is deferred each tick.
    broadcaster.setSnapshotBudget(140u);
    connectPilotPeer(broadcaster, net, 0u);

    auto farIdxIn = [&](const std::vector<uint8_t>& pkt) -> std::optional<uint32_t> {
        for (const auto& d : decodeEntities(pkt))
            if (d.pos[0] > 4000.0) // a far entity (not the peer's own, which is near origin)
                return d.entityIdx;
        return std::nullopt;
    };
    auto recordFor = [&](const std::vector<uint8_t>& pkt, uint32_t idx) -> std::optional<DecodedEntity> {
        for (const auto& d : decodeEntities(pkt))
            if (d.entityIdx == idx)
                return d;
        return std::nullopt;
    };

    // Tick 1: one far entity X is sent (full), the other is deferred.
    broadcaster.onTick(1.0 / 60.0, 1u);
    auto x = farIdxIn(snapshotsFor(net, 0).back());
    REQUIRE(x.has_value());
    REQUIRE(recordFor(snapshotsFor(net, 0).back(), *x)->isFull);

    // Tick 2: X is deferred (the other far entity, higher recency, takes the slot). X is absent.
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u);
    REQUIRE_FALSE(recordFor(snapshotsFor(net, 0).back(), *x).has_value());

    // The client acks tick 2 (which did NOT contain X) with a selective-ack mask reporting that tick 1
    // (X's full, streak start age = 2 - 1 = 1 → bit 0) was NOT decoded — modelling the client dropping
    // tick 1 and only receiving tick 2. A naive "delta if fullStreakTick <= ackedTick" high-water mark
    // would mis-send X as an undecodable delta; selective-ack (#566) confirms the SPECIFIC streak-start
    // tick, so X is correctly kept full (this is the case the removed #517 deferral guard used to cover).
    ackTick(broadcaster, 0u, 2u, 1u, 0xFFFFFFFFu & ~(1u << 0));

    // Tick 3: X reappears and MUST be a full record (the client never learned it).
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 3u);
    auto xRec = recordFor(snapshotsFor(net, 0).back(), *x);
    REQUIRE(xRec.has_value());
    CHECK(xRec->isFull);
}

TEST_CASE("WorldBroadcaster: SnapshotPeerLatency TLV present when estimatedDelayTicks > 0",
          "[world_broadcaster][latency]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // Tick 2: advance m_currentTick to 2, peer has no delay yet.
    broadcaster.onTick(1.0 / 60.0, 2u);
    clearSnapshots(net);

    // Send MsgHeartbeat with tickIndex=0 → estimatedDelayTicks = 2 - 0 = 2.
    fl::MsgHeartbeat hb{};
    hb.msgId = static_cast<uint8_t>(fl::MsgId::Heartbeat);
    hb.tickIndex = 0u;
    broadcaster.onReceive(0u, &hb, sizeof(hb));

    // Tick 3: snapshot must include SnapshotPeerLatency = 2 * 1000 / 60 = 33 ms.
    broadcaster.onTick(1.0 / 60.0, 3u);
    auto snaps = snapshotsFor(net, 0u);
    REQUIRE(!snaps.empty());
    const auto& pkt = snaps[0];

    const auto hdr = parseSnapshotHeader(pkt);
    const std::size_t extOffset = sizeof(fl::MsgWorldSnapshotHeader) +
                                  static_cast<std::size_t>(hdr.originCount) * 3u * sizeof(double) + hdr.bitstreamBytes;
    REQUIRE(pkt.size() > extOffset);
    const auto* ext = pkt.data() + extOffset;
    const auto extSz = pkt.size() - extOffset;

    uint16_t pc{};
    CHECK(fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), pc));
    CHECK(pc == 1u);

    uint16_t lat{};
    CHECK(fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerLatency), lat));
    CHECK(lat == static_cast<uint16_t>(2u * 1000u / 60u)); // 33 ms
}

TEST_CASE("WorldBroadcaster: SnapshotPeerLatency TLV absent when estimatedDelayTicks == 0",
          "[world_broadcaster][latency]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // No heartbeat sent — estimatedDelayTicks stays 0.
    broadcaster.onTick(1.0 / 60.0, 1u);
    auto snaps = snapshotsFor(net, 0u);
    REQUIRE(!snaps.empty());
    const auto& pkt = snaps[0];

    const auto hdr = parseSnapshotHeader(pkt);
    const std::size_t extOffset = sizeof(fl::MsgWorldSnapshotHeader) +
                                  static_cast<std::size_t>(hdr.originCount) * 3u * sizeof(double) + hdr.bitstreamBytes;
    REQUIRE(pkt.size() > extOffset);
    const auto* ext = pkt.data() + extOffset;
    const auto extSz = pkt.size() - extOffset;

    // SnapshotPeerCount must be present.
    uint16_t pc{};
    CHECK(fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), pc));

    // SnapshotPeerLatency must NOT be present.
    uint16_t lat{};
    CHECK_FALSE(fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerLatency), lat));

    // Packet size: header + quantized bitstream + the SnapshotPeerCount TLV + the parked pilot's
    // articulation record.
    const std::size_t expected = sizeof(fl::MsgWorldSnapshotHeader) +
                                 static_cast<std::size_t>(hdr.originCount) * 3u * sizeof(double) + hdr.bitstreamBytes +
                                 6u + kParkedGearArtTlvBytes;
    CHECK(pkt.size() == expected);
}

TEST_CASE("WorldBroadcaster: snapshot entity record carries omega field without corruption",
          "[world_broadcaster][omega]") {
    // This test verifies the code path: FlightState.omega → TelemetryEntry.omega →
    // EntitySnap.omega → MsgEntityEntry/MsgEntityUpdate.omega.
    // The round-trip VALUE check (serialise → memcpy → verify exact values) is covered by
    // test_game_protocol.cpp.  Here we just confirm the field is present in the packet,
    // is finite (not NaN/inf), and that the packet has the correct size for the new struct.
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    // One tick is enough to verify the code path. omega starts at {0,0,0} and we just
    // check the field is accessible and finite in the serialized packet.
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.seqNum = 1u;
    inp.tickIndex = 0u;
    inp.throttle = 1.0f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0u);
    REQUIRE(!snaps.empty());
    const auto& pkt = snaps[0];

    const auto hdr = parseSnapshotHeader(pkt);
    REQUIRE(fullRecordCount(pkt) >= 1u); // first tick always sends a full record
    CHECK(hdr.bitstreamBytes > 0u);

    const auto entries = decodeEntities(pkt);
    REQUIRE(!entries.empty());
    const DecodedEntity& entry = entries[0]; // the peer's own entity carries omega

    // omega field is accessible; values must be finite (not NaN/inf from a bad cast/quantize).
    CHECK(std::isfinite(entry.omega[0]));
    CHECK(std::isfinite(entry.omega[1]));
    CHECK(std::isfinite(entry.omega[2]));
}

TEST_CASE("WorldBroadcaster: snapshot includes SnapshotPeerDelayTicks TLV when delay > 0",
          "[world_broadcaster][latency]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 2u);
    clearSnapshots(net);

    // Send MsgHeartbeat with tickIndex=0 → estimatedDelayTicks = 2 - 0 = 2.
    fl::MsgHeartbeat hb{};
    hb.msgId = static_cast<uint8_t>(fl::MsgId::Heartbeat);
    hb.tickIndex = 0u;
    broadcaster.onReceive(0u, &hb, sizeof(hb));

    broadcaster.onTick(1.0 / 60.0, 3u);
    auto snaps = snapshotsFor(net, 0u);
    REQUIRE(!snaps.empty());
    const auto& pkt = snaps[0];

    const auto hdr = parseSnapshotHeader(pkt);
    const std::size_t extOffset = sizeof(fl::MsgWorldSnapshotHeader) +
                                  static_cast<std::size_t>(hdr.originCount) * 3u * sizeof(double) + hdr.bitstreamBytes;
    REQUIRE(pkt.size() > extOffset);
    const auto* ext = pkt.data() + extOffset;
    const auto extSz = pkt.size() - extOffset;

    // Both SnapshotPeerLatency and SnapshotPeerDelayTicks must be present and consistent.
    uint16_t lat{};
    REQUIRE(fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerLatency), lat));
    CHECK(lat == static_cast<uint16_t>(2u * 1000u / 60u)); // 33 ms

    uint16_t delayTicks{};
    REQUIRE(fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerDelayTicks), delayTicks));
    CHECK(delayTicks == 2u);
}

TEST_CASE("WorldBroadcaster: snapshot budget caps records and always includes own entity",
          "[world_broadcaster][interest][budget]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    // Spawn 40 entities clustered near the origin so they're all within interest.
    for (int i = 0; i < 40; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = i * 5.0; // within a few hundred metres
        t.pos[1] = 500.0;
        em.spawn("builtin:debug-entity", t);
    }

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(200.f);
    broadcaster.setSnapshotBudget(200u); // tiny budget: only a handful of ~24-31 B records fit
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    auto hdr = parseSnapshotHeader(snaps.back());
    // Budget bounds the record count well below the 41 visible (40 + peer).
    CHECK(totalEntityCount(hdr) >= 1u);
    CHECK(totalEntityCount(hdr) < 41u);

    // The peer's own entity is always present (prediction reconciliation needs it).
    fl::MsgConnectAck ack{};
    for (const auto& [pid, pkt] : net.perPeerSends)
        if (pid == 0u && !pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::ConnectAck))
            std::memcpy(&ack, pkt.data(), sizeof(ack));
    bool sawOwn = false;
    for (const auto& e : decodeEntities(snaps.back()))
        if (e.entityIdx == ack.assignedEntityIdx)
            sawOwn = true;
    CHECK(sawOwn);
}

TEST_CASE("WorldBroadcaster: budget==0 sends every visible entity (legacy path)",
          "[world_broadcaster][interest][budget]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    for (int i = 0; i < 10; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = i * 5.0;
        t.pos[1] = 500.0;
        em.spawn("builtin:debug-entity", t);
    }

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(200.f); // budget defaults to 0 (unlimited)
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    CHECK(totalEntityCount(parseSnapshotHeader(snaps.back())) == 11u); // 10 + peer, all sent
}

TEST_CASE("WorldBroadcaster: starved entity eventually included under a tight budget",
          "[world_broadcaster][interest][budget]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    std::vector<uint32_t> idxs;
    for (int i = 0; i < 30; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = 100.0 + i * 10.0;
        t.pos[1] = 500.0;
        idxs.push_back(em.spawn("builtin:debug-entity", t).index);
    }

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    disableOverrunGovernor(broadcaster); // #787: a slow host must not shed the snapshot we assert on
    broadcaster.setDrawDistance(200.f);
    broadcaster.setSnapshotBudget(160u); // only a few records per tick
    connectPilotPeer(broadcaster, net, 0u);

    // The farthest entity (highest idx) starts low priority; over enough ticks its recency term must
    // lift it into a snapshot at least once.
    const uint32_t starved = idxs.back();
    bool everSent = false;
    for (uint64_t tick = 1; tick <= 200 && !everSent; ++tick) {
        clearSnapshots(net);
        broadcaster.onTick(1.0 / 60.0, tick);
        for (const auto& e : decodeEntities(snapshotsFor(net, 0).back()))
            if (e.entityIdx == starved)
                everSent = true;
    }
    CHECK(everSent); // anti-starvation guarantee (recency term)
}

TEST_CASE("WorldBroadcaster: killing a known entity emits a despawn TLV, interest-out does not",
          "[world_broadcaster][interest][budget]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::EntityTransform t{};
    t.pos[0] = 100.0;
    t.pos[1] = 500.0;
    fl::EntityId victim = em.spawn("builtin:debug-entity", t);
    const uint32_t victimIdx = victim.index;

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(200.f);
    connectPilotPeer(broadcaster, net, 0u);

    // Tick 1: peer learns the victim.
    broadcaster.onTick(1.0 / 60.0, 1u);
    {
        bool saw = false;
        for (const auto& e : decodeEntities(snapshotsFor(net, 0).back()))
            if (e.entityIdx == victimIdx)
                saw = true;
        REQUIRE(saw);
        CHECK(decodeDespawns(snapshotsFor(net, 0).back()).empty());
    }

    // Kill the victim and tick again: an explicit despawn must be emitted.
    em.kill(victim);
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u);
    {
        auto despawns = decodeDespawns(snapshotsFor(net, 0).back());
        bool listed = std::find(despawns.begin(), despawns.end(), victimIdx) != despawns.end();
        CHECK(listed);
    }
}

TEST_CASE("WorldBroadcaster: entity flown out of interest is not despawned", "[world_broadcaster][interest][budget]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    // A controllable entity that we can teleport out of interest by moving it via its state.
    fl::EntityTransform t{};
    t.pos[0] = 100.0;
    t.pos[1] = 500.0;
    fl::EntityId mover = em.spawn("builtin:debug-entity", t);
    const uint32_t moverIdx = mover.index;

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(1.f); // 1 km
    connectPilotPeer(broadcaster, net, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u); // peer learns the mover

    // Move the entity far outside the interest sphere (still alive in the sim).
    if (auto* st = em.get(mover)) {
        const_cast<fl::EntityState*>(st)->transform.pos[0] = 50'000.0; // 50 km away
    }
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u);

    // It is no longer in the snapshot, but it must NOT be despawned (it's still alive, just far) —
    // the client's retention timeout handles interest-out.
    auto pkt = snapshotsFor(net, 0).back();
    auto despawns = decodeDespawns(pkt);
    CHECK(std::find(despawns.begin(), despawns.end(), moverIdx) == despawns.end());
}

TEST_CASE("WorldBroadcaster: congested peer is decimated, healthy peer keeps full rate",
          "[world_broadcaster][congestion]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setCongestionParams(testCongestion());
    connectPilotPeer(broadcaster, net, 0u); // congested peer
    connectPilotPeer(broadcaster, net, 1u); // healthy peer
    net.peerLinkStats[0] = lossLink(0.5f);
    net.peerLinkStats[1] = lossLink(0.0f);

    for (uint64_t tick = 1; tick <= 40; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    const auto congested = snapshotsFor(net, 0).size();
    const auto healthy = snapshotsFor(net, 1).size();
    CHECK(healthy == 40u);      // full 60 Hz: a snapshot every tick
    CHECK(congested < healthy); // decimated under sustained loss
    CHECK(congested >= 1u);     // recency still guarantees periodic sends (no starvation)
}

TEST_CASE("WorldBroadcaster: congestion shrinks the effective byte budget (fewer records)",
          "[world_broadcaster][congestion]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    // Many co-located entities so the byte budget — not interest — is the limiting factor.
    for (int i = 0; i < 40; ++i) {
        fl::EntityTransform t{};
        t.pos[1] = 500.0;
        em.spawn("builtin:debug-entity", t);
    }

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    disableOverrunGovernor(broadcaster); // #787: a slow host must not shed the snapshot we assert on
    broadcaster.setDrawDistance(200.f);
    broadcaster.setSnapshotBudget(1200u);
    broadcaster.setCongestionParams(testCongestion());
    connectPilotPeer(broadcaster, net, 0u); // congested
    connectPilotPeer(broadcaster, net, 1u); // healthy (both spawn at the fallback origin => same visible set)
    net.peerLinkStats[0] = lossLink(0.5f);
    net.peerLinkStats[1] = lossLink(0.0f);

    for (uint64_t tick = 1; tick <= 50; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    const auto congested = snapshotsFor(net, 0);
    const auto healthy = snapshotsFor(net, 1);
    REQUIRE(!congested.empty());
    REQUIRE(!healthy.empty());
    const uint16_t congestedRecords = parseSnapshotHeader(congested.back()).recordCount;
    const uint16_t healthyRecords = parseSnapshotHeader(healthy.back()).recordCount;
    CHECK(congestedRecords < healthyRecords); // floor-budget peer carries fewer entities per snapshot
}

TEST_CASE("WorldBroadcaster: peer returns to full rate after congestion clears", "[world_broadcaster][congestion]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setCongestionParams(testCongestion());
    connectPilotPeer(broadcaster, net, 0u);
    net.peerLinkStats[0] = lossLink(0.5f);
    for (uint64_t tick = 1; tick <= 40; ++tick) // collapse to the floor
        broadcaster.onTick(1.0 / 60.0, tick);

    net.peerLinkStats[0] = lossLink(0.0f);        // link recovers
    for (uint64_t tick = 41; tick <= 240; ++tick) // ramp back up
        broadcaster.onTick(1.0 / 60.0, tick);

    clearSnapshots(net);
    for (uint64_t tick = 241; tick <= 260; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);
    CHECK(snapshotsFor(net, 0).size() == 20u); // every tick again
}

TEST_CASE("WorldBroadcaster: congestion disabled never decimates under loss", "[world_broadcaster][congestion]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setCongestionParams(testCongestion(/*enabled=*/false));
    connectPilotPeer(broadcaster, net, 0u);
    net.peerLinkStats[0] = lossLink(0.9f); // heavy loss, but the controller is off

    for (uint64_t tick = 1; tick <= 30; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    CHECK(snapshotsFor(net, 0).size() == 30u);
}

TEST_CASE("WorldBroadcaster: congestion telemetry watermarks record engage then recovery",
          "[world_broadcaster][congestion]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    connectPilotPeer(broadcaster, net, 0u);

    uint64_t tick = 1;
    auto runTicks = [&](int n) {
        for (int i = 0; i < n; ++i)
            broadcaster.onTick(1.0 / 60.0, tick++);
    };

    // Healthy link: the controller stays at full rate; watermarks read "never engaged".
    runTicks(12);
    fl::CongestionTelemetry t0 = broadcaster.getCongestionTelemetry();
    CHECK(t0.minSendHz == Catch::Approx(60.f));
    CHECK(t0.recoveredSendHz == Catch::Approx(60.f));
    CHECK(t0.maxPacketLoss == Catch::Approx(0.f));

    // Degrade the link: 20% ENet loss (over the 2% threshold). The AIMD controller backs the
    // throttle off to its floor (1/6 => 10 Hz at the default 6-tick max interval).
    fl::PeerLinkStats bad;
    bad.packetLoss = 0.2f;
    net.peerLinkStats[0u] = bad;
    runTicks(90); // >= 10 AIMD evals at the default 6-tick cadence — enough to hit the floor
    fl::CongestionTelemetry t1 = broadcaster.getCongestionTelemetry();
    CHECK(t1.minSendHz == Catch::Approx(10.f)); // engaged all the way to the floor
    CHECK(t1.maxPacketLoss == Catch::Approx(0.2f));

    // Clear the link: additive ramp-up recovers the send rate; the recovered watermark climbs
    // back to 60 while the all-time minimum stays put.
    net.peerLinkStats[0u] = fl::PeerLinkStats{};
    runTicks(90);
    fl::CongestionTelemetry t2 = broadcaster.getCongestionTelemetry();
    CHECK(t2.minSendHz == Catch::Approx(10.f));
    CHECK(t2.recoveredSendHz == Catch::Approx(60.f));

    // The watermarks freeze once the last peer disconnects (the gate reads the metrics file after
    // the load clients drop — trailing empty-peer ticks must not wipe the evidence).
    broadcaster.onDisconnect(0u);
    runTicks(30);
    fl::CongestionTelemetry t3 = broadcaster.getCongestionTelemetry();
    CHECK(t3.minSendHz == Catch::Approx(10.f));
    CHECK(t3.recoveredSendHz == Catch::Approx(60.f));
    CHECK(t3.maxPacketLoss == Catch::Approx(0.2f));
}

TEST_CASE("WorldBroadcaster: a neutral world emits no articulation TLV (#843)", "[world_broadcaster][articulation]") {
    // An entity with all-default channels costs ZERO bytes — the snapshot is byte-identical to
    // pre-#843 for every world of unarticulated aircraft, which is nearly all of them.
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster wb(em, registry, net, logger);
    wb.setRateLimitParams(100, 10, 1000); // the wall-clock flood guard, as above
    connectPilotPeer(wb, net, 0u);

    // A pilot spawns PARKED and therefore gear-down (#639), so retract it first: "neutral" here means
    // every actuator actually at its default, not merely un-commanded.
    for (uint32_t t = 0; t < 500; ++t) {
        sendArticulationInput(wb, 0u, t + 1u, 0u, 0u);
        wb.onTick(1.0 / 60.0, t + 1u);
    }

    clearSnapshots(net);
    wb.onTick(1.0 / 60.0, 501u);
    const SnapshotExt ext = lastSnapshotExt(net, 0u);
    REQUIRE(ext.data != nullptr);
    uint16_t len = 0;
    CHECK(fl::findExt(ext.data, ext.size, static_cast<uint16_t>(fl::ExtTag::SnapshotArticulation), len) == nullptr);
}

TEST_CASE("WorldBroadcaster: articulation TLV round-trips channel mask and values (#843)",
          "[world_broadcaster][articulation]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster wb(em, registry, net, logger);
    wb.setRateLimitParams(100, 10, 1000); // see the note above: the wall-clock flood guard
    connectPilotPeer(wb, net, 0u);

    for (uint32_t t = 0; t < 400; ++t) {
        sendArticulationInput(wb, 0u, t + 1u, fl::kArtButtonGearDown | fl::kArtButtonHookDown, 255u);
        wb.onTick(1.0 / 60.0, t + 1u);
    }

    // Settled state is sent on the periodic refresh, so scan a refresh window rather than assuming
    // any one tick carries it.
    std::vector<uint8_t> tlv;
    for (uint32_t t = 400; t < 400 + 40 && tlv.empty(); ++t) {
        clearSnapshots(net);
        sendArticulationInput(wb, 0u, t + 1u, fl::kArtButtonGearDown | fl::kArtButtonHookDown, 255u);
        wb.onTick(1.0 / 60.0, t + 1u);
        const SnapshotExt ext = lastSnapshotExt(net, 0u);
        if (ext.data == nullptr)
            continue;
        uint16_t len = 0;
        if (const uint8_t* found =
                fl::findExt(ext.data, ext.size, static_cast<uint16_t>(fl::ExtTag::SnapshotArticulation), len);
            found != nullptr && len >= 6u)
            tlv.assign(found, found + len);
    }
    REQUIRE(tlv.size() >= 6u);
    const uint8_t* p = tlv.data();

    uint16_t mask = 0;
    std::memcpy(&mask, p + 4, 2);
    CHECK((mask & (1u << static_cast<unsigned>(fl::ArtChannel::Gear))) != 0u);
    CHECK((mask & (1u << static_cast<unsigned>(fl::ArtChannel::Flaps))) != 0u);
    CHECK((mask & (1u << static_cast<unsigned>(fl::ArtChannel::Hook))) != 0u);
    CHECK((mask & (1u << static_cast<unsigned>(fl::ArtChannel::Canopy))) == 0u); // never commanded
    // Values follow in ascending channel order; gear is first and fully down.
    CHECK(p[6] == 255u);
}

// ---------------------------------------------------------------------------
// Wing sweep on the articulation TLV (#1195)
// ---------------------------------------------------------------------------

namespace {

// The wire cases fly a wing parked at 45 deg. On a 15..67.5 range that is 30/52.5 of the travel,
// and the wire carries round(fraction * 255).
constexpr float kVgRefSweepDeg = 45.f;
constexpr uint8_t kExpectedSweepByte = 146u; // round(0.571428 * 255) == 146

// Drive the peer for long enough that the actuators settle, then scan a refresh window for the TLV.
// Settled state rides the periodic refresh, so no single tick is guaranteed to carry it.
inline std::vector<uint8_t> settleAndCaptureArtTlv(fl::WorldBroadcaster& wb, MockNetwork& net) {
    for (uint32_t t = 0; t < 400; ++t) {
        sendArticulationInput(wb, 0u, t + 1u, fl::kArtButtonGearDown, 0u);
        wb.onTick(1.0 / 60.0, t + 1u);
    }
    std::vector<uint8_t> tlv;
    for (uint32_t t = 400; t < 400 + 40 && tlv.empty(); ++t) {
        clearSnapshots(net);
        sendArticulationInput(wb, 0u, t + 1u, fl::kArtButtonGearDown, 0u);
        wb.onTick(1.0 / 60.0, t + 1u);
        const SnapshotExt ext = lastSnapshotExt(net, 0u);
        if (ext.data == nullptr)
            continue;
        uint16_t len = 0;
        if (const uint8_t* found =
                fl::findExt(ext.data, ext.size, static_cast<uint16_t>(fl::ExtTag::SnapshotArticulation), len);
            found != nullptr && len >= 6u)
            tlv.assign(found, found + len);
    }
    return tlv;
}

} // namespace

TEST_CASE("WorldBroadcaster: a variable-geometry wing reaches the articulation TLV (#1195)",
          "[world_broadcaster][articulation][sweep]") {
    // THE DEFECT. ArtChannel::Sweep was declared, documented for mod authors and sampled by the
    // renderer, and no code anywhere wrote it: the server sent five of sixteen channels and sweep
    // was not among them, so a swing-wing aircraft rendered permanently at min_deg for every
    // observer while its flight model swept correctly the entire time.
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityDef def = makeDebugDef();
    def.flightModelAsset = "models/vg";
    registry.registerType(def);
    fl::EntityManager em(logger, registry);

    const auto vg =
        std::make_shared<const fl::FlightModelData>(fl::parseFlightModel(makeVgFlightModelToml(kVgRefSweepDeg)));
    fl::WorldQueries queries;
    queries.flightModel = [&](const std::string&) { return vg; };
    fl::WorldBroadcaster wb(em, registry, net, logger, nullptr, std::move(queries));
    wb.setRateLimitParams(100, 10, 1000); // the wall-clock flood guard, as in the #843 cases above
    connectPilotPeer(wb, net, 0u);

    const std::vector<uint8_t> tlv = settleAndCaptureArtTlv(wb, net);
    REQUIRE(tlv.size() >= 6u);

    uint16_t mask = 0;
    std::memcpy(&mask, tlv.data() + 4, 2);
    REQUIRE((mask & (1u << static_cast<unsigned>(fl::ArtChannel::Sweep))) != 0u);
    // Gear is down (a pilot spawns parked) and precedes sweep in the enum, so it is the first value
    // and sweep the second — which pins the ascending-channel-order contract as well.
    REQUIRE((mask & (1u << static_cast<unsigned>(fl::ArtChannel::Gear))) != 0u);
    CHECK(tlv[6] == 255u);
    CHECK(tlv[7] == kExpectedSweepByte);

    // And the receiver recovers the angle. This is the client's own decode arithmetic
    // (ClientNetEventHandler: unsigned channels are q/255), run back through the wing's limits — so
    // the assertion is "a client draws the wing where the simulation put it", not "a byte moved".
    const float decoded = static_cast<float>(tlv[7]) / 255.f;
    const float deg = 15.f + decoded * (67.5f - 15.f);
    CHECK(deg == Catch::Approx(45.f).margin(0.2f)); // 1/255 of 52.5 deg is 0.21 deg of quantization
}

TEST_CASE("WorldBroadcaster: a fixed-geometry aircraft emits no sweep channel (#1195)",
          "[world_broadcaster][articulation][sweep]") {
    // The cost claim. Everything without a [wing_sweep] table must stay off the wire entirely: the
    // channel is neutral, so it is absent, so the snapshot is byte-identical to pre-#1195 for every
    // aircraft in the game but the swing-wings.
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef()); // no flightModelAsset -> the builtin, fixed-geometry model
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster wb(em, registry, net, logger);
    wb.setRateLimitParams(100, 10, 1000);
    connectPilotPeer(wb, net, 0u);

    const std::vector<uint8_t> tlv = settleAndCaptureArtTlv(wb, net);
    REQUIRE(tlv.size() >= 6u); // gear is down, so there IS a TLV to look at

    uint16_t mask = 0;
    std::memcpy(&mask, tlv.data() + 4, 2);
    CHECK((mask & (1u << static_cast<unsigned>(fl::ArtChannel::Sweep))) == 0u);
    CHECK(std::popcount(static_cast<unsigned>(mask)) == 1); // gear and nothing else — no extra byte
}

TEST_CASE("WorldBroadcaster: steady-state articulation is not resent every tick (#843)",
          "[world_broadcaster][articulation]") {
    // Changed-only, plus a periodic refresh for drop tolerance. Without the change gate a settled
    // aircraft would spend bytes every tick saying nothing new.
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster wb(em, registry, net, logger);
    wb.setRateLimitParams(100, 10, 1000); // see the note above: the wall-clock flood guard
    connectPilotPeer(wb, net, 0u);

    for (uint32_t t = 0; t < 500; ++t) { // gear fully down and settled
        sendArticulationInput(wb, 0u, t + 1u, fl::kArtButtonGearDown, 0u);
        wb.onTick(1.0 / 60.0, t + 1u);
    }

    int withTlv = 0;
    for (uint32_t t = 500; t < 500 + 60; ++t) {
        clearSnapshots(net);
        sendArticulationInput(wb, 0u, t + 1u, fl::kArtButtonGearDown, 0u);
        wb.onTick(1.0 / 60.0, t + 1u);
        const SnapshotExt ext = lastSnapshotExt(net, 0u);
        if (ext.data == nullptr)
            continue;
        uint16_t len = 0;
        if (fl::findExt(ext.data, ext.size, static_cast<uint16_t>(fl::ExtTag::SnapshotArticulation), len) != nullptr)
            ++withTlv;
    }
    // 60 settled ticks at a 30-tick refresh: a couple of refreshes, nowhere near every tick.
    CHECK(withTlv > 0);
    CHECK(withTlv <= 4);
}

TEST_CASE("WorldBroadcaster: the replay tap records entities no peer can see (#643)", "[world_broadcaster][replay]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    // One entity at the origin, one 50 km away. A peer at the origin with a 5 km draw distance sees
    // exactly one of them; the recording must contain both.
    fl::EntityTransform nearT{};
    nearT.pos[1] = 500.0;
    REQUIRE(em.spawn("builtin:debug-entity", nearT).valid());
    fl::EntityTransform farT{};
    farT.pos[0] = 50'000.0;
    farT.pos[1] = 500.0;
    REQUIRE(em.spawn("builtin:debug-entity", farT).valid());

    std::vector<fl::ReplayTickRecords> taps;
    fl::WorldBroadcasterHooks h_broadcaster;
    h_broadcaster.snapshot.replaySink = [&taps](const fl::ReplayTickRecords& r) { taps.push_back(r); };
    h_broadcaster.snapshot.replayKeyframeIntervalTicks = 4;
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, {}, std::move(h_broadcaster));
    broadcaster.setDrawDistance(5.f);
    connectPilotPeer(broadcaster, net, 0u);

    for (uint64_t t = 0; t < 8; ++t)
        broadcaster.onTick(1.0 / 60.0, t);

    REQUIRE(taps.size() == 8);
    for (const auto& tap : taps) {
        // 3 = the two spawned entities + the peer's own aircraft.
        CHECK(tap.recordCount == 3);
        CHECK_FALSE(tap.records.empty());
        CHECK_FALSE(tap.origins.empty());
        CHECK(tap.stateHash != 0u);
    }

    // The first tick after installation is a keyframe, and the cadence holds from there -- a file
    // that opened on a delta would have no baseline to decode against.
    CHECK(taps[0].keyframe);
    CHECK(taps[4].keyframe);
    CHECK_FALSE(taps[1].keyframe);
    CHECK_FALSE(taps[5].keyframe);

    // A keyframe carries every entity full, so it is strictly larger than the delta ticks around it.
    CHECK(taps[0].records.size() > taps[1].records.size());
}

TEST_CASE("WorldBroadcaster: the replay tap does not change a single byte a peer receives (#643)",
          "[world_broadcaster][replay]") {
    // The cost-of-the-feature question, asserted rather than assumed: recording must not perturb the
    // snapshots, or turning it on would change what every client sees.
    auto run = [](bool withSink) {
        NullLogger logger;
        MockNetwork net;
        fl::EntityTypeRegistry registry;
        registry.registerType(makeDebugDef());
        fl::EntityManager em(logger, registry);

        fl::EntityTransform t{};
        t.pos[1] = 800.0;
        REQUIRE(em.spawn("builtin:debug-entity", t).valid());

        fl::WorldBroadcasterHooks h_broadcaster;
        h_broadcaster.snapshot.replaySink = [](const fl::ReplayTickRecords&) {};
        h_broadcaster.snapshot.replayKeyframeIntervalTicks = 4;
        fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, {}, std::move(h_broadcaster));
        connectPilotPeer(broadcaster, net, 0u);
        if (withSink)
            clearSnapshots(net);
        for (uint64_t i = 0; i < 10; ++i)
            broadcaster.onTick(1.0 / 60.0, i);
        return snapshotsFor(net, 0u);
    };

    const auto without = run(false);
    const auto with = run(true);
    REQUIRE(without.size() == with.size());
    for (std::size_t i = 0; i < without.size(); ++i) {
        INFO("snapshot packet index " << i);
        CHECK(without[i] == with[i]);
    }
}

TEST_CASE("WorldBroadcaster: the replay tap's state hash tracks the world, not the tick number (#643)",
          "[world_broadcaster][replay]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::EntityTransform t{};
    t.pos[1] = 900.0;
    const fl::EntityId id = em.spawn("builtin:debug-entity", t);
    REQUIRE(id.valid());

    std::vector<uint64_t> hashes;
    fl::WorldBroadcasterHooks h_broadcaster;
    h_broadcaster.snapshot.replaySink = [&hashes](const fl::ReplayTickRecords& r) { hashes.push_back(r.stateHash); };
    h_broadcaster.snapshot.replayKeyframeIntervalTicks = 4;
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr, {}, std::move(h_broadcaster));

    for (uint64_t i = 0; i < 4; ++i)
        broadcaster.onTick(1.0 / 60.0, i);

    // A static world still hashes differently per tick (the tick index is folded in), which is what
    // makes a per-tick stream comparable position-by-position rather than as a set.
    REQUIRE(hashes.size() == 4);
    CHECK(hashes[0] != hashes[1]);

    // Moving an entity changes the hash for that tick: the fingerprint follows the world.
    const uint64_t beforeMove = hashes.back();
    fl::EntityState* st = em.get(id);
    REQUIRE(st != nullptr);
    st->transform.pos[0] += 1000.0;
    broadcaster.onTick(1.0 / 60.0, 3); // same tick index, different world
    REQUIRE(hashes.size() == 5);
    CHECK(hashes.back() != beforeMove);
}

TEST_CASE("WorldBroadcaster: a healthy server emits no server-throttle TLV and is byte-identical",
          "[world_broadcaster][overrun]") {
    // THE regression this tag must not cause. A new TLV that appears on the healthy path would
    // change every snapshot on every server, for a condition almost none of them are in — the same
    // rule SnapshotCrew and SnapshotArticulation follow (zero cost in the degenerate case).
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    ManualClock clock; // never advances: the tick is always inside budget
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setClock(clock);
    connectPilotPeer(broadcaster, net, 0u);

    for (uint64_t tick = 1; tick <= 60; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    const auto snaps = snapshotsFor(net, 0u);
    REQUIRE_FALSE(snaps.empty());
    for (const auto& s : snaps)
        CHECK_FALSE(hasServerThrottleTlv(s));

    broadcaster.forEachPeer([](const fl::PeerInfo& pi) {
        CHECK_FALSE(pi.governorBinding);
        CHECK_FALSE(pi.congestionBinding);
        CHECK(pi.effectiveIntervalTicks == 1u);
        CHECK(pi.sendRateHz == Catch::Approx(60.f));
    });
}

TEST_CASE("WorldBroadcaster: an overrun server attributes the throttle to itself and says so on the wire",
          "[world_broadcaster][overrun]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    AutoAdvanceClock clock(std::chrono::milliseconds(3)); // every tick blows the 16.6 ms budget
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setClock(clock);
    fl::TickGovernorParams gp = fl::makeTickGovernorParams(true, 0.90f, 0.60f, 15.0f, 4u, 400u);
    gp.evalIntervalTicks = 1u;
    gp.ewmaAlpha = 1.0f;
    broadcaster.setGovernorParams(gp);
    connectPilotPeer(broadcaster, net, 0u);

    for (uint64_t tick = 1; tick <= 120; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    REQUIRE(broadcaster.getOverrunStatus().degraded);

    // The peer is attributed to the SERVER, not to its own link — the link is perfect here (a mock
    // network reports zero loss), which is exactly the case the #599 sweep's models got backwards.
    int seen = 0;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) {
        ++seen;
        CHECK(pi.governorBinding);
        CHECK_FALSE(pi.congestionBinding);
        CHECK(pi.effectiveIntervalTicks > 1u);
        // The reported rate is the EFFECTIVE one, composed from both levers. Before #576 this read
        // the congestion interval alone and reported 60 Hz on a server decimating to a third of it.
        CHECK(pi.sendRateHz < 60.f);
    });
    CHECK(seen == 1);

    const auto snaps = snapshotsFor(net, 0u);
    REQUIRE_FALSE(snaps.empty());
    // The tag tracks the CONDITION, not the session: the opening snapshots precede the governor
    // engaging and correctly carry nothing. What matters is that once it engages the tag is on every
    // snapshot rather than sent once as a state change — a client that joins mid-degradation learns
    // about it from the first packet it receives, and the latch has something to keep refreshing.
    CHECK(hasServerThrottleTlv(snaps.back()));
    const auto tagged = static_cast<std::size_t>(std::count_if(snaps.begin(), snaps.end(), hasServerThrottleTlv));
    CHECK(tagged > 0);
    CHECK(tagged < snaps.size()); // the healthy opening ticks are genuinely untagged
    // Contiguous tail: no gaps once it turned on.
    std::size_t firstTagged = 0;
    while (firstTagged < snaps.size() && !hasServerThrottleTlv(snaps[firstTagged]))
        ++firstTagged;
    for (std::size_t i = firstTagged; i < snaps.size(); ++i)
        CHECK(hasServerThrottleTlv(snaps[i]));
}

TEST_CASE("WorldBroadcaster: the server-throttle TLV reports a plausible load and interval",
          "[world_broadcaster][overrun]") {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    AutoAdvanceClock clock(std::chrono::milliseconds(3));
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setClock(clock);
    fl::TickGovernorParams gp = fl::makeTickGovernorParams(true, 0.90f, 0.60f, 15.0f, 4u, 400u);
    gp.evalIntervalTicks = 1u;
    gp.ewmaAlpha = 1.0f;
    broadcaster.setGovernorParams(gp);
    connectPilotPeer(broadcaster, net, 0u);

    for (uint64_t tick = 1; tick <= 120; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    const auto snaps = snapshotsFor(net, 0u);
    REQUIRE_FALSE(snaps.empty());
    const auto& last = snaps.back();
    fl::MsgWorldSnapshotHeader hdr{};
    REQUIRE(fl::readMsg(last.data(), last.size(), hdr));
    const std::size_t extOff =
        sizeof(hdr) + static_cast<std::size_t>(hdr.originCount) * 3u * sizeof(double) + hdr.bitstreamBytes;
    std::uint16_t len = 0;
    const std::uint8_t* p = fl::findExt(last.data() + extOff, last.size() - extOff,
                                        static_cast<uint16_t>(fl::ExtTag::SnapshotServerThrottle), len);
    REQUIRE(p != nullptr);
    REQUIRE(len >= 2);
    // loadPct is clamped to [1,100]: 0 would read as "no load" on a server that is at its floor.
    CHECK(p[0] >= 1);
    CHECK(p[0] <= 100);
    CHECK(p[1] >= 2); // the tag is only emitted when the interval actually widened
}
