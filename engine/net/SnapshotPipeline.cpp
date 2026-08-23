// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/SnapshotPipeline.h"
#include "math/Angles.h"
#include "math/Fnv.h"

#include "ILogger.h"
#include "INetwork.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "flight/ArticulationChannels.h"
#include "flight/EngineFailFlags.h"
#include "flight/FlightIntegrator.h"
#include "job/JobSystem.h"
#include "net/AckWindow.h"
#include "net/BitStream.h"
#include "net/GameProtocol.h"
#include "net/Quantization.h"
#include "net/ReplayStateHash.h"
#include "net/SnapshotCodec.h"
#include "net/SnapshotCompression.h"
#include "net/WireCodec.h"
#include "net/WorldBroadcaster.h"
#include "render/ArtChannel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <utility>

namespace fl {

// Grain size for the per-peer snapshot build: 1, because each peer is a heavy, heterogeneous-cost
// unit (a draw-distance interest query + priority/budget scheduler + quantized bitstream encode), so
// the finest grain gives the dynamic-claim cursor the best load balancing across workers.
static constexpr std::size_t kPeerPassGrain = 1;

SnapshotPipeline::SnapshotPipeline(WorldBroadcaster& wb, EntityManager& entityManager, INetwork& net,
                                   const WorldBroadcasterHooks& hooks) noexcept
    : m_wb(wb), m_entityManager(entityManager), m_net(net), m_hooks(hooks) {}

void SnapshotPipeline::onDisconnect(uint32_t peerId) {
    m_peerKnownGens.erase(peerId);
    m_peerPendingDespawn.erase(peerId);
}

void SnapshotPipeline::setBudgetBytes(uint32_t bytes) noexcept {
    m_snapshotBudgetBytes.store(bytes, std::memory_order_relaxed);
}

uint32_t SnapshotPipeline::budgetBytes() const noexcept {
    return m_snapshotBudgetBytes.load(std::memory_order_relaxed);
}

void SnapshotPipeline::setCompression(bool enabled) noexcept {
    m_compressSnapshots.store(enabled, std::memory_order_relaxed);
}

void SnapshotPipeline::setReplayKeyframeInterval(uint32_t ticks) noexcept {
    if (ticks != 0)
        m_replayKeyframeInterval = ticks;
    m_replayForceKeyframe = true;
}

void SnapshotPipeline::runPeerPass(std::size_t count, const std::function<void(std::size_t, std::size_t)>& fn) {
    if (count == 0)
        return;
    if (m_wb.m_jobs)
        m_wb.m_jobs->parallel_for(count, kPeerPassGrain, fn);
    else
        fn(0, count); // inline / serial fallback (unit tests, single-threaded servers)
}

void SnapshotPipeline::run(uint64_t tickIndex, uint32_t govSnapInterval, float govInterestScale, float govLoadFactor) {
    // Build per-peer world snapshots with interest management and delta compression.
    //
    // Step 1: build telemetry from flight integrators (same as before).
    struct TelemetryEntry {
        uint8_t throttle;
        uint8_t fuelPct;
        uint8_t abEngaged;
        uint8_t engineFailFlags;
        float omega[3];                        // body-frame angular rates p,q,r (rad/s)
        float artChannels[kArtChannelCount]{}; // normalized articulation channels (#843, #1195)
    };
    std::unordered_map<uint32_t, TelemetryEntry> entityTelemetry;
    for (auto& [entityIdx, ce] : m_wb.m_controlledEntities) {
        const auto& s = ce.sim->state();
        TelemetryEntry te{static_cast<uint8_t>(s.throttle_actual * 100.f),
                          static_cast<uint8_t>(std::clamp(s.fuel_kg / 4000.f * 100.f, 0.f, 100.f)),
                          static_cast<uint8_t>(s.ab_engaged ? 1u : 0u),
                          s.engineFailFlags,
                          {s.omega[0], s.omega[1], s.omega[2]},
                          {}};
        // ONE mapping, shared with the client's own-aircraft path (#1195). Reading the channels off
        // FlightState by hand here is what left `sweep` dark for two releases.
        fillArtChannels(s, ce.sim->flightModel(), te.artChannels);
        entityTelemetry[entityIdx] = te;
    }

    // Articulation table (#843): the quantized channel values for every entity whose actuators are
    // off their neutral positions. Built serially here (the integrate pass is done), read lock-free
    // by the parallel per-peer pass, which emits only the entities in that peer's interest set.
    //
    // AN ENTITY AT ALL-DEFAULT COSTS ZERO BYTES: it is simply absent from the table, so a world of
    // unarticulated meshes emits no TLV and its snapshot is byte-identical to pre-#843.
    struct ArtSnap {
        uint16_t mask{0};
        uint8_t count{0};
        uint8_t values[kArtChannelCount]{};
        uint32_t hash{0}; // change detector for the per-peer send policy
    };
    std::unordered_map<uint32_t, ArtSnap> artSnap;
    for (const auto& [entityIdx, tel] : entityTelemetry) {
        ArtSnap out;
        uint8_t n = 0;
        for (std::size_t i = 0; i < kArtChannelCount; ++i) {
            const auto channel = static_cast<ArtChannel>(i);
            const float v = tel.artChannels[i];
            if (v == artChannelNeutral(channel))
                continue; // at neutral: omit, so an unarticulated entity is free
            out.mask = static_cast<uint16_t>(out.mask | (1u << i));
            // Signed channels are offset binary around 128; unsigned a plain 0..255 fraction. The
            // decoder has always read both (ClientNetEventHandler), and testing `> 0.f` here instead
            // of "off neutral" would silently drop every negative value the moment a signed channel
            // acquires a writer.
            out.values[n++] = artChannelIsSigned(channel) ? quantSignedU8(v) : quantUnitU8(v);
        }
        out.count = n;
        if (out.mask != 0) {
            // FNV-1a over mask + values: the send policy needs only "did this change", and a hash is
            // one word per peer per entity instead of a whole channel set.
            uint32_t h = kFnv1a32Basis;
            fnv1a32Byte(h, static_cast<uint8_t>(out.mask & 0xFFu));
            fnv1a32Byte(h, static_cast<uint8_t>(out.mask >> 8));
            for (uint8_t i = 0; i < n; ++i)
                fnv1a32Byte(h, out.values[i]);
            out.hash = h ? h : 1u; // 0 is the "never sent" sentinel in PeerEntityRec
            artSnap.emplace(entityIdx, out);
        }
    }

    // Step 2: build entity snapshot map — one pass shared across all per-peer loops.
    struct EntitySnap {
        const EntityState* state;
        uint8_t throttle;
        uint8_t fuelPct;
        uint8_t abEngaged;
        uint8_t engineFailFlags;
        float omega[3]; // body-frame angular rates p,q,r (rad/s)
        // Own-record loadout extras (#625) — consumed only when this entity is the receiving peer's.
        uint8_t selectedStation{255};
        uint16_t stationRounds{0};
        uint8_t weaponFlags{0};
        float payloadMassKg{0.f};
        float payloadCd0{0.f};
    };
    std::unordered_map<uint32_t, EntitySnap> snapMap;
    snapMap.reserve(m_wb.m_spatialIndex.entityCount());
    m_entityManager.forEach([&](const EntityState& state) {
        auto tit = entityTelemetry.find(state.id.index);
        uint8_t efFlags = (tit != entityTelemetry.end()) ? tit->second.engineFailFlags : 0u;
        if (static_cast<uint8_t>(state.damageLevel) >= 2u)
            efFlags |= fl::kEngineFailGeneric;
        const float* omegaPtr = (tit != entityTelemetry.end()) ? tit->second.omega : nullptr;
        EntitySnap snap{&state,
                        (tit != entityTelemetry.end()) ? tit->second.throttle : uint8_t{0},
                        (tit != entityTelemetry.end()) ? tit->second.fuelPct : uint8_t{0},
                        (tit != entityTelemetry.end()) ? tit->second.abEngaged : uint8_t{0},
                        efFlags,
                        {omegaPtr ? omegaPtr[0] : 0.f, omegaPtr ? omegaPtr[1] : 0.f, omegaPtr ? omegaPtr[2] : 0.f}};
        if (auto cit = m_wb.m_controlledEntities.find(state.id.index);
            cit != m_wb.m_controlledEntities.end() && !cit->second.fire.loadout.empty()) {
            const LoadoutState& lo = cit->second.fire.loadout;
            snap.selectedStation = lo.selected;
            if (lo.selected < lo.stations.size())
                snap.stationRounds = lo.stations[lo.selected].rounds;
            snap.weaponFlags = cit->second.fire.seekerCue ? 0x01u : 0x00u; // HUD LOCK cue (#628)
            snap.payloadMassKg = lo.payloadMassKg;
            snap.payloadCd0 = lo.payloadCd0;
        }
        snapMap[state.id.index] = snap;
    });

    // Encode-once (#725): quantize + bit-pack each live entity ONCE this tick, relative to its shared
    // grid origin (SnapshotCodec::originForPos), as a full and a delta blob. Per-peer assembly stitches
    // these blobs by memcpy instead of re-quantizing, so encode is O(entities) not O(peers x visible).
    // Blobs carry no per-peer state (absolute idx, byte-aligned, position relative to the shared
    // origin), so they drop into any peer's stream in any order. The receiving peer's OWN record is the
    // sole exception — it alone carries omega — so it is re-encoded per peer below (one extra encode per
    // peer, negligible). Order-free: each blob is independent, so the map's iteration order is irrelevant
    // (serial-equivalence preserved).
    struct EncodedRecord {
        double origin[3];
        std::vector<uint8_t> fullBlob;
        std::vector<uint8_t> deltaBlob;
    };
    std::unordered_map<uint32_t, EncodedRecord> encoded;
    encoded.reserve(snapMap.size());
    // Replay tap (#643): the recorder needs the QuantEntity VALUES as well as the blobs -- the state
    // hash (#644) is computed over the quantized integer domain, which is the only domain in which a
    // recorded and a replayed world can be compared at all. Populated only when a sink is installed.
    std::vector<std::pair<uint32_t, QuantEntity>> replayEnts;
    if (m_hooks.snapshot.replaySink)
        replayEnts.reserve(snapMap.size());
    for (const auto& [encIdx, snap] : snapMap) {
        const EntityState& st = *snap.state;
        QuantEntity qe;
        qe.idx = st.id.index;
        qe.gen = st.id.generation;
        qe.typeIndex = st.typeIndex;
        qe.factionIndex = st.factionIndex; // #860: client-cached like typeIndex, drives the observer picker label
        qe.hasOmega = false;               // the once-encoded blob never carries omega (own record re-encoded per peer)
        qe.pos[0] = st.transform.pos[0];
        qe.pos[1] = st.transform.pos[1];
        qe.pos[2] = st.transform.pos[2];
        qe.vel[0] = st.transform.vel[0];
        qe.vel[1] = st.transform.vel[1];
        qe.vel[2] = st.transform.vel[2];
        qe.quat[0] = st.transform.quat[0];
        qe.quat[1] = st.transform.quat[1];
        qe.quat[2] = st.transform.quat[2];
        qe.quat[3] = st.transform.quat[3];
        qe.damageLevel = static_cast<uint8_t>(st.damageLevel);
        qe.engineFailFlags = snap.engineFailFlags;
        qe.throttle = snap.throttle;
        qe.fuelPct = snap.fuelPct;
        qe.abEngaged = snap.abEngaged != 0u;
        qe.playerOwned = st.playerOwned;

        EncodedRecord rec;
        originForPos(qe.pos, rec.origin);
        qe.isFull = true;
        encodeStandaloneRecord(rec.fullBlob, qe, rec.origin, /*sendGen=*/true);
        qe.isFull = false;
        encodeStandaloneRecord(rec.deltaBlob, qe, rec.origin, /*sendGen=*/false);
        encoded.emplace(encIdx, std::move(rec));
        if (m_hooks.snapshot.replaySink)
            replayEnts.emplace_back(encIdx, qe);
    }

    // Build the replay tick from those same blobs (#643). Serial, outside the parallel peer pass, and
    // skipped entirely when nobody is recording. Records go out in ASCENDING ENTITY INDEX: `encoded`
    // is an unordered_map, and a file (or a state hash) whose byte layout depended on hash-table
    // iteration order would differ between two runs of the same session and make the #644 gate
    // meaningless.
    if (m_hooks.snapshot.replaySink) {
        std::sort(replayEnts.begin(), replayEnts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

        const bool keyframe =
            m_replayForceKeyframe || (m_replayKeyframeInterval > 0 && (tickIndex % m_replayKeyframeInterval) == 0);

        ReplayTickRecords out;
        out.tick = tickIndex;
        out.keyframe = keyframe;

        // Origin table, deduped in first-use order. A handful of entries in practice (kOriginGridM is
        // ~65 km), so a linear scan beats a map and keeps the order deterministic.
        auto originIndexOf = [&out](const double o[3]) -> uint32_t {
            for (std::size_t i = 0; i * 3 + 2 < out.origins.size(); ++i) {
                if (out.origins[i * 3] == o[0] && out.origins[i * 3 + 1] == o[1] && out.origins[i * 3 + 2] == o[2])
                    return static_cast<uint32_t>(i);
            }
            out.origins.push_back(o[0]);
            out.origins.push_back(o[1]);
            out.origins.push_back(o[2]);
            return static_cast<uint32_t>(out.origins.size() / 3 - 1);
        };

        std::vector<QuantEntity> written; // the pre-encode entities, in written order
        written.reserve(replayEnts.size());
        for (const auto& [ridx, rqe] : replayEnts) {
            const auto eit = encoded.find(ridx);
            if (eit == encoded.end())
                continue;
            const auto kit = m_replayKnownGens.find(ridx);
            const auto gen16 = static_cast<uint16_t>(rqe.gen);
            const bool full = keyframe || kit == m_replayKnownGens.end() || kit->second != gen16;
            appendStitchedRecord(out.records, originIndexOf(eit->second.origin),
                                 full ? eit->second.fullBlob : eit->second.deltaBlob);
            m_replayKnownGens[ridx] = gen16;
            ++out.recordCount;
            written.push_back(rqe);
        }

        // Forget entities that are no longer live, so a reused pool slot is recorded full rather than
        // as a delta against a corpse.
        if (m_replayKnownGens.size() > replayEnts.size()) {
            for (auto it = m_replayKnownGens.begin(); it != m_replayKnownGens.end();)
                it = (snapMap.find(it->first) == snapMap.end()) ? m_replayKnownGens.erase(it) : std::next(it);
        }

        // Hash what a READER will see, by decoding the stream just built -- NOT the values the encoder
        // was fed. The two are not interchangeable, and the #644 gate is what proved it: the
        // smallest-three orientation encoding drops the largest-magnitude component, so a rotation
        // whose two largest components are nearly equal can have that choice tip when quantized. The
        // decoded quaternion then re-encodes to a different dropped component and a different hash --
        // a mismatch that is not drift at all, and one that no amount of re-normalizing fixes, because
        // the tie sits exactly on the boundary.
        //
        // Decoding our own stream (one pass, only while recording) removes the question: both sides
        // hash a value that came out of the codec, so the hash means "the world a replay will show".
        // Sim drift still changes it, because a different world encodes to different bytes.
        std::vector<QuantEntity> hashEnts;
        hashEnts.reserve(written.size());
        {
            BitReader hr(out.records.data(), out.records.size());
            const auto originCount = static_cast<uint32_t>(out.origins.size() / 3);
            for (const QuantEntity& src : written) {
                QuantEntity dec;
                bool genPresent = false;
                if (!decodeStandaloneRecord(hr, dec, out.origins.data(), originCount, genPresent))
                    break; // cannot happen for bytes we just wrote; fail closed rather than guess
                // A delta carries no typeIndex/factionIndex/gen -- a reader restores them from its
                // cache, which holds exactly these values.
                dec.typeIndex = src.typeIndex;
                dec.factionIndex = src.factionIndex;
                if (!genPresent)
                    dec.gen = src.gen;
                hashEnts.push_back(dec);
            }
        }
        out.stateHash = hashTickState(tickIndex, hashEnts.data(), hashEnts.size());
        m_replayForceKeyframe = false;
        m_hooks.snapshot.replaySink(out);
    }

    // Crew turret-pose table (#972): for each CREWED entity that has turrets, the quantized mount-frame
    // az/el of every turret this tick. Built serially here (crew state is stable after the integrate
    // pass) so the parallel peer pass reads it lock-free and emits a per-peer SnapshotCrew TLV over the
    // entities in that peer's interest set. Single-seat/turretless entities are ABSENT, so a world of
    // only single-seat aircraft builds an empty table, emits no TLV, and is byte-identical to pre-#972.
    struct CrewSnap {
        std::vector<std::pair<int16_t, int16_t>> turrets; // (azQ, elQ) per turret, mount frame
    };
    std::unordered_map<uint32_t, CrewSnap> crewSnap;
    for (const auto& [cidx, ce] : m_wb.m_controlledEntities) {
        if (!ce.crew.crewed() || ce.crew.turrets.empty())
            continue;
        if (snapMap.find(cidx) == snapMap.end())
            continue; // not live this tick
        CrewSnap cs;
        cs.turrets.reserve(ce.crew.turrets.size());
        for (const CrewTurret& tr : ce.crew.turrets)
            cs.turrets.emplace_back(quantAngleI16(tr.state.azRad, kPi<float>),
                                    quantAngleI16(tr.state.elRad, kPi<float> * 0.5f));
        crewSnap.emplace(cidx, std::move(cs));
    }

    const auto activePeers =
        static_cast<uint16_t>(std::max(0, std::min(m_wb.m_activePeerCount.load(std::memory_order_relaxed), 65535)));

    // Client-acked delta baselines with selective-ack precision (#566): a record is `full` (carries
    // typeIndex + gen) when the peer has not seen this entity/gen before, the generation changed, the
    // peer has not confirmed it DECODED the tick this full streak started on (fullStreakTick), or the
    // peer was not sent it within kSnapshotRetentionTicks (it may have time-evicted it, so a delta
    // would be undecodable); otherwise a delta. Pure — captures nothing.
    //
    // ackReceived() consults the peer's selective-ack window (high-water ackedTick + ackMask bitmask):
    // it confirms delivery of the SPECIFIC fullStreakTick rather than a high-water mark, closing the
    // #517 residual where acking a later tick could falsely confirm a full the client never decoded.
    // (An existing rec always has a real fullStreakTick: the first send for any entity is a full,
    // which seeds it; rec == nullptr is the "never" case.)
    auto decideFull = [](const PeerEntityRec* rec, uint16_t gen, uint64_t ackedTick, uint32_t ackMask,
                         uint64_t T) -> bool {
        return rec == nullptr || rec->gen != gen || !ackReceived(ackedTick, ackMask, rec->fullStreakTick) ||
               (T - rec->lastSentTick) >= kSnapshotRetentionTicks;
    };

    // Step 3: per-peer snapshot — interest filter (queryRadius) + client-acked delta compression.
    //
    // Serial gather: resolve each sending peer's stable per-peer state pointers once. The operator[]
    // insertions (and any rehash) happen here on the sim thread, never inside the parallel build, so
    // the workers see a frozen map structure (unordered_map keeps element pointers valid across later
    // rehashes). Decimated peers (#518) are excluded from the work set — a decimated tick mutates no
    // per-peer state, exactly matching the previous in-loop `continue`.
    // Compose the per-peer congestion send-interval with the server-wide overrun-governor interval
    // (#514): a peer is decimated by whichever lever spaces it out more. The governor-scaled static
    // budget below is likewise the per-client budget after server-wide overrun shedding, fed into each
    // peer's congestion budget lever. Both governor values are frozen sim-thread locals — the parallel
    // build region never touches the governor object.
    const uint32_t govStaticBudget =
        m_wb.m_tickGovernor.effectiveBudget(m_snapshotBudgetBytes.load(std::memory_order_relaxed));
    // Overrun interest-radius lever (#726): scale the per-peer interest radius by the governor's
    // frozen interestScale — the only lever that shrinks the visible set itself (the input to the
    // interest query, scheduler ranking, and encode) rather than trimming the encoded output after
    // ranking. A pure function of loadFactor, uniform across peers and frozen here BEFORE the
    // parallel build region, so the peer pass stays serial-equivalent by construction. Entities
    // leaving the shrunk radius are ordinary interest-out (client retention + the
    // kSnapshotRetentionTicks force-full backstop handle re-entry) — never despawned, no wire
    // change. Healthy / disabled governor => interestScale == 1 => the exact configured radius.
    const double govInterestRadiusM = m_wb.m_drawDistanceM * static_cast<double>(govInterestScale);
    // Snapshot payload compression (#775): frozen before the parallel region like the governor
    // levers, so every peer in this tick sees the same setting.
    const bool compressSnap = m_compressSnapshots.load(std::memory_order_relaxed);
    m_peerWork.clear();
    // Iterate all admitted peers (#857), not m_wb.m_peerEntities: an OBSERVER has an input slot but no
    // entity, and must still receive snapshots. A not-yet-admitted peer (connected but no
    // MsgConnectRequest yet) is skipped until it has a role.
    for (auto& [peerId, pin] : m_wb.m_peerInputs) {
        if (!pin.handshakeComplete)
            continue;
        const uint32_t congInterval = pin.congestion.sendIntervalTicks();
        const uint32_t sendInterval = std::max(congInterval, govSnapInterval);
        if (pin.sentSnapshot && tickIndex - pin.lastSnapshotSentTick < sendInterval)
            continue; // adaptive send-rate decimation: too few ticks since the last send
        PeerSnapWork w;
        w.peerId = peerId;
        // #576: record WHICH lever set that interval, here in the serial gather where both numbers
        // are in hand. The governor is server-wide but only BINDS for a peer whose own congestion
        // interval is narrower — so "is the server throttling this peer" is a per-peer question and
        // is answered per peer. Ties count as the governor: at equal intervals the server is
        // shedding work regardless of what the link is doing.
        w.sendIntervalTicks = sendInterval;
        w.governorBinding = govSnapInterval > 1 && govSnapInterval >= congInterval;
        pin.effectiveIntervalTicks = sendInterval;
        pin.governorBinding = w.governorBinding;
        pin.congestionBinding = congInterval > 1 && congInterval > govSnapInterval;
        // A pilot centers interest on its aircraft; an observer on its stored interest point (the #858
        // camera-position seam). peerEid invalid + peerState null flag the entity-less case downstream.
        // #972: peerEid is the entity the peer OCCUPIES A SEAT IN (m_wb.m_peerSeat) — a Fly-seat pilot's own
        // aircraft, or a gunner's host aircraft (#974) — so every seat occupant, not just the owner,
        // centers interest on that airframe and receives its omega-carrying own record.
        const auto sit = m_wb.m_peerSeat.find(peerId);
        w.peerEid = (sit != m_wb.m_peerSeat.end()) ? sit->second.entity : EntityId{};
        w.peerState = w.peerEid.valid() ? m_entityManager.get(w.peerEid) : nullptr;
        // A LIVE-seated peer centers interest on its airframe. A spectator — an observer (no entity), or a
        // dead pilot awaiting respawn (#403) — centers on an admin spectate target if one is set + alive
        // (auto-clearing it otherwise), else on its stored interest point (the #858 cameraEye / the wreck
        // seed from the Died handler).
        const bool spectating = (w.peerState == nullptr) || w.peerState->dead;
        w.spectator = spectating;
        if (!spectating) {
            w.center[0] = w.peerState->transform.pos[0];
            w.center[1] = w.peerState->transform.pos[1];
            w.center[2] = w.peerState->transform.pos[2];
        } else {
            const EntityState* tgt = (pin.spectateTargetIdx != PeerInputState::kNoSpectateTarget)
                                         ? m_entityManager.getByIndex(pin.spectateTargetIdx)
                                         : nullptr;
            if (tgt && !tgt->dead) {
                w.center[0] = tgt->transform.pos[0];
                w.center[1] = tgt->transform.pos[1];
                w.center[2] = tgt->transform.pos[2];
            } else {
                pin.spectateTargetIdx = PeerInputState::kNoSpectateTarget; // target gone: auto-clear
                w.center[0] = pin.interestCenter.x;
                w.center[1] = pin.interestCenter.y;
                w.center[2] = pin.interestCenter.z;
            }
        }
        w.pin = &pin;
        w.knownGens = &m_peerKnownGens[peerId];
        w.pending = &m_peerPendingDespawn[peerId];
        m_peerWork.push_back(std::move(w));
    }

    // Parallel build: each worker assembles one peer's snapshot into its own w.buf and mutates only
    // that peer's private state (knownGens GC/records, pending despawns, pin EWMA-free fields). Shared
    // reads (snapMap, m_wb.m_spatialIndex, m_entityManager.get, m_wb.m_drawDistanceM, the frozen
    // govInterestRadiusM local, m_snapshotBudgetBytes, m_schedulerWeights, m_wb.m_congestionParams) are
    // read-only for the whole region. No m_net.send here —
    // the ENetHost is sim-thread-owned, so the actual send + send-cadence bookkeeping is the serial
    // flush below.
    runPeerPass(m_peerWork.size(), [&](std::size_t wbegin, std::size_t wend) {
        for (std::size_t wi = wbegin; wi < wend; ++wi) {
            PeerSnapWork& w = m_peerWork[wi];
            const EntityId peerEid = w.peerEid;
            PeerInputState& pin = *w.pin;
            const EntityState* peerState = w.peerState;
            auto& knownGens = *w.knownGens;
            const uint64_t peerAckedTick = pin.ackedTick;
            const uint32_t peerAckMask = pin.ackMask;

            // Confirmed-despawn detection (#516) + GC prune, in one pass over the known set:
            //   * Absent from the live snapMap → removed from the sim entirely (kill/despawn). Queue an
            //     explicit despawn so the client drops it promptly rather than waiting out the retention
            //     timeout, and erase it from the known set.
            //   * Present but not sent within kSnapshotRetentionTicks → the client has already
            //     time-evicted it (interest-out), so prune the record to bound the map (replacing the GC
            //     the removed periodic baseline clear used to provide). No despawn TLV — it's a timeout,
            //     not a kill; a re-entry is force-fulled by the retention clause in decideFull().
            {
                auto& pending = *w.pending;
                for (auto it = knownGens.begin(); it != knownGens.end();) {
                    if (snapMap.find(it->first) == snapMap.end()) {
                        pending[it->first] = kDespawnRepeatTicks;
                        it = knownGens.erase(it);
                    } else if (tickIndex - it->second.lastSentTick >= kSnapshotRetentionTicks) {
                        it = knownGens.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            std::vector<uint8_t>& buf = w.buf; // reused scratch, retains capacity across ticks
            buf.clear();
            buf.reserve(sizeof(MsgWorldSnapshotHeader) + 256);

            MsgWorldSnapshotHeader hdr;
            hdr.msgId = static_cast<uint8_t>(MsgId::WorldSnapshot);
            hdr.protocolVersion = static_cast<uint8_t>(kProtocolVersion);
            hdr.recordCount = 0;
            hdr.bitstreamBytes = 0;
            hdr.tickIndex = tickIndex;
            hdr.originCount = 0; // shared-origin table (#725); filled after the stitch loop below
            const std::size_t hdrOffset = buf.size();
            appendMsg(buf, hdr); // placeholder; recordCount/originCount/bitstreamBytes patched below

            // Collect visible entity indices via the spatial index (conservative XZ cells), then apply
            // an exact 3D (XYZ) distance gate (#402) and sort ascending so the bitstream's idx deltas
            // stay small. Both bounds use the governor-scaled interest radius (#726 — frozen before
            // this parallel region). Interest is centered on w.center — a live pilot's aircraft, or a
            // spectator's point (an observer's, or a dead pilot's spectate/camera/wreck center, #403).
            // The dead-pilot header-only blackout was lifted by #403 so a dead peer spectates the world
            // around its center instead of going black between death and respawn.
            const double* const center = w.center;
            std::vector<uint32_t>& visible = w.visible; // reused across ticks (#1092)
            visible.clear();
            if (govInterestRadiusM > 0.0) {
                const double r2 = govInterestRadiusM * govInterestRadiusM;
                const double px = center[0], py = center[1], pz = center[2];
                m_wb.m_spatialIndex.queryRadius(center, govInterestRadiusM, [&](uint32_t entityIdx, const double* pos) {
                    if (snapMap.find(entityIdx) == snapMap.end())
                        return; // died this tick after the index was built
                    const double dx = pos[0] - px, dy = pos[1] - py, dz = pos[2] - pz;
                    if (dx * dx + dy * dy + dz * dz > r2)
                        return; // 3D interest cull (#402)
                    visible.push_back(entityIdx);
                });
                std::sort(visible.begin(), visible.end());
            }

            // Priority/budget scheduling (#516). When a per-client byte budget is set, rank the visible
            // entities by relevance (distance / closing-speed / recency / player-owned) and keep only the
            // highest-priority set that fits; the rest are deferred to a later tick. budget == 0 keeps the
            // legacy behaviour (every visible entity, ascending idx). The own entity is always admitted.
            std::vector<uint32_t>& selected = w.selected;
            selected.clear();
            // Congestion response (#518): scale the static byte budget by this peer's congestion throttle.
            // A static budget of 0 (unlimited) stays 0 here — under congestion only the send-rate lever
            // applies for unlimited-budget servers.
            // govStaticBudget is the static per-client budget after server-wide overrun shedding (#514);
            // the per-peer congestion lever (#518) then scales it further for this peer.
            const uint32_t budget = pin.congestion.effectiveBudget(govStaticBudget);
            if (budget == 0u || visible.size() <= 1u) {
                selected = visible;
            } else {
                // Reserve fixed overhead (header + TLV block) out of the budget for the record bitstream.
                constexpr uint32_t kFixedOverhead = sizeof(MsgWorldSnapshotHeader) + 32u;
                const uint32_t recordBudget = budget > kFixedOverhead ? budget - kFixedOverhead : 1u;
                const double px = center[0], py = center[1], pz = center[2];
                // Observer (peerState null) has no velocity — closing speed is relative to a still point.
                const double pvx = peerState ? static_cast<double>(peerState->transform.vel[0]) : 0.0;
                const double pvy = peerState ? static_cast<double>(peerState->transform.vel[1]) : 0.0;
                const double pvz = peerState ? static_cast<double>(peerState->transform.vel[2]) : 0.0;
                std::vector<SnapshotCandidate>& cands = w.cands;
                cands.clear();
                cands.reserve(visible.size());
                for (uint32_t idx : visible) {
                    const EntitySnap& snap = snapMap.at(idx);
                    const EntityState& st = *snap.state;
                    SnapshotCandidate c;
                    c.idx = idx;
                    const double dx = st.transform.pos[0] - px, dy = st.transform.pos[1] - py,
                                 dz = st.transform.pos[2] - pz;
                    c.distSq = dx * dx + dy * dy + dz * dz;
                    // Closing speed: range rate toward the peer (positive = approaching). r_hat points
                    // peer→entity; closing = dot(peerVel - entityVel, r_hat).
                    const double dist = std::sqrt(c.distSq);
                    if (dist > 1e-3) {
                        const double rx = dx / dist, ry = dy / dist, rz = dz / dist;
                        const double rvx = pvx - st.transform.vel[0];
                        const double rvy = pvy - st.transform.vel[1];
                        const double rvz = pvz - st.transform.vel[2];
                        c.closingSpeed = static_cast<float>(rvx * rx + rvy * ry + rvz * rz);
                    }
                    c.isOwn =
                        peerEid.valid() && (st.id.index == peerEid.index && st.id.generation == peerEid.generation);
                    c.playerOwned = st.playerOwned;
                    const uint16_t gen = static_cast<uint16_t>(st.id.generation);
                    auto kit = knownGens.find(idx);
                    const PeerEntityRec* rec = (kit == knownGens.end()) ? nullptr : &kit->second;
                    c.ticksSinceSent = (rec == nullptr) ? UINT64_MAX : (tickIndex - rec->lastSentTick);
                    const bool isFull = decideFull(rec, gen, peerAckedTick, peerAckMask, tickIndex);
                    // Absolute idx (#725) + a conservative 1-byte origin index; the real origin index
                    // isn't known until the per-peer origin table is built after selection, but the budget
                    // is a soft cap so a per-record ±1 byte is acceptable.
                    c.estBytes = estimateRecordBytes(isFull, isFull, c.isOwn, st.typeIndex, /*entityIndex=*/idx,
                                                     /*originIndex=*/1u);
                    cands.push_back(c);
                }
                selected = selectSnapshotRecords(cands, recordBudget, m_schedulerWeights, m_wb.m_drawDistanceM);
                std::sort(selected.begin(), selected.end()); // ascending for the codec's idx-delta varints

                // No deferral guard is needed under selective-ack (#566): a scheduler-withheld entity is
                // not SENT this tick, so its fullStreakTick keeps its earlier value; the peer's ack of
                // this tick sets only this tick's bit and cannot confirm that earlier fullStreakTick.
                // decideFull() confirms the specific full-sent tick, so the #517 streak-bump workaround
                // (which the high-water mark required) is now redundant.
            }

            // Assemble the stitched record stream (#725): for each selected entity, pick its pre-encoded
            // blob (full or delta per decideFull), record its shared origin into this peer's origin table
            // (deduped), and stitch [origin index][blob]. The peer's OWN entity is the one per-peer record
            // — re-encoded here with omega. Records are byte-aligned, so the stitch is a memcpy, not a
            // re-quantization. Deterministic (selected is sorted; origin table is first-seen order) so the
            // per-peer buffer is byte-identical across worker counts (serial-equivalence, #512).
            std::vector<std::array<double, 3>>& originTable = w.originTable;
            originTable.clear();
            std::vector<uint8_t>& recordStream = w.recordStream;
            recordStream.clear();
            auto originIndexOf = [&originTable](const double o[3]) -> uint32_t {
                for (uint32_t i = 0; i < originTable.size(); ++i)
                    if (originTable[i][0] == o[0] && originTable[i][1] == o[1] && originTable[i][2] == o[2])
                        return i;
                originTable.push_back({o[0], o[1], o[2]});
                return static_cast<uint32_t>(originTable.size() - 1u);
            };

            std::vector<uint8_t>& ownBlob = w.ownBlob; // scratch for the (single) own-entity re-encode
            ownBlob.clear();
            for (uint32_t idx : selected) {
                const EntitySnap& snap = snapMap.at(idx);
                const EntityState& state = *snap.state;
                const uint16_t gen = static_cast<uint16_t>(state.id.generation);
                auto kit = knownGens.find(idx);
                const PeerEntityRec* rec = (kit == knownGens.end()) ? nullptr : &kit->second;
                const bool isFull = decideFull(rec, gen, peerAckedTick, peerAckMask, tickIndex);
                const bool isOwn =
                    peerEid.valid() && (state.id.index == peerEid.index && state.id.generation == peerEid.generation);

                double recOrigin[3];
                const std::vector<uint8_t>* blob = nullptr;
                if (isOwn) {
                    QuantEntity qe;
                    qe.idx = state.id.index;
                    qe.gen = state.id.generation;
                    qe.typeIndex = state.typeIndex;
                    qe.factionIndex = state.factionIndex; // #860
                    qe.isFull = isFull;
                    qe.hasOmega = true; // the own record alone carries omega
                    qe.pos[0] = state.transform.pos[0];
                    qe.pos[1] = state.transform.pos[1];
                    qe.pos[2] = state.transform.pos[2];
                    qe.vel[0] = state.transform.vel[0];
                    qe.vel[1] = state.transform.vel[1];
                    qe.vel[2] = state.transform.vel[2];
                    qe.quat[0] = state.transform.quat[0];
                    qe.quat[1] = state.transform.quat[1];
                    qe.quat[2] = state.transform.quat[2];
                    qe.quat[3] = state.transform.quat[3];
                    qe.omega[0] = snap.omega[0];
                    qe.omega[1] = snap.omega[1];
                    qe.omega[2] = snap.omega[2];
                    qe.damageLevel = static_cast<uint8_t>(state.damageLevel);
                    qe.engineFailFlags = snap.engineFailFlags;
                    qe.throttle = snap.throttle;
                    qe.fuelPct = snap.fuelPct;
                    qe.abEngaged = snap.abEngaged != 0u;
                    qe.playerOwned = state.playerOwned;
                    // Own-record loadout block (#625) — gated by the same hasOmega bit.
                    qe.selectedStation = snap.selectedStation;
                    qe.stationRounds = snap.stationRounds;
                    qe.weaponFlags = snap.weaponFlags;
                    qe.payloadMassKg = snap.payloadMassKg;
                    qe.payloadCd0 = snap.payloadCd0;
                    originForPos(qe.pos, recOrigin);
                    ownBlob.clear();
                    encodeStandaloneRecord(ownBlob, qe, recOrigin, /*sendGen=*/isFull);
                    blob = &ownBlob;
                } else {
                    const EncodedRecord& er = encoded.at(idx);
                    recOrigin[0] = er.origin[0];
                    recOrigin[1] = er.origin[1];
                    recOrigin[2] = er.origin[2];
                    blob = isFull ? &er.fullBlob : &er.deltaBlob;
                }
                appendStitchedRecord(recordStream, originIndexOf(recOrigin), *blob);

                // Record what we sent. Freeze fullStreakTick at the start of a contiguous run of fulls
                // (same entity, full last tick, sent consecutively) so the client only has to ack the
                // streak start to converge to deltas; a send gap (deferral) or a prior delta restarts it.
                PeerEntityRec& r = knownGens[idx];
                if (isFull) {
                    const bool contiguous = r.lastWasFull && r.lastSentTick + 1 == tickIndex;
                    r.fullStreakTick = contiguous ? r.fullStreakTick : tickIndex;
                }
                r.gen = gen;
                r.lastSentTick = tickIndex;
                r.lastWasFull = isFull;
                ++hdr.recordCount;
            }

            // Body layout: [origin table: originCount x double[3]][stitched record stream]. Append both
            // after the header placeholder, then patch the header counts.
            hdr.originCount = static_cast<uint16_t>(originTable.size());
            for (const auto& o : originTable) {
                const auto* p = reinterpret_cast<const uint8_t*>(o.data());
                buf.insert(buf.end(), p, p + 3u * sizeof(double));
            }
            buf.insert(buf.end(), recordStream.begin(), recordStream.end());
            hdr.bitstreamBytes = static_cast<uint32_t>(recordStream.size());
            writeMsgAt(buf, hdrOffset, hdr);

            // TLV extension block.
            appendExt(buf, static_cast<uint16_t>(ExtTag::SnapshotPeerCount), activePeers);
            // Per-peer latency TLVs. Omitted when estimatedDelayTicks == 0 (e.g. single-player localhost)
            // so the client's m_hasSnapshotLatency stays false and the HUD indicator remains hidden.
            if (pin.estimatedDelayTicks > 0) {
                const auto latMs = static_cast<uint16_t>(
                    std::min(m_wb.m_tickRate.ticksToMs(pin.estimatedDelayTicks), uint64_t{65535u}));
                appendExt(buf, static_cast<uint16_t>(ExtTag::SnapshotPeerLatency), latMs);
                const auto delayTicks = static_cast<uint16_t>(std::min(pin.estimatedDelayTicks, uint32_t{65535u}));
                appendExt(buf, static_cast<uint16_t>(ExtTag::SnapshotPeerDelayTicks), delayTicks);
            }
            // Server-throttle TLV (#576). OMITTED unless the governor is the binding lever for THIS
            // peer, which keeps the healthy path byte-identical to pre-#576 — the same rule
            // SnapshotCrew and SnapshotArticulation follow. A client that never sees this tag
            // cannot mistake a bad link for server overload, because the server never claimed it.
            if (w.governorBinding) {
                const uint8_t loadPct =
                    static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(govLoadFactor * 100.f)), 1, 100));
                const uint8_t intervalTicks = static_cast<uint8_t>(std::min(w.sendIntervalTicks, uint32_t{255u}));
                const uint8_t payload[2] = {loadPct, intervalTicks};
                appendExtRaw(buf, static_cast<uint16_t>(ExtTag::SnapshotServerThrottle), payload, sizeof(payload));
            }
            // Exact acked-seqNum (#427): the seqNum of the last input the server applied for this peer.
            // The client replays inputs newer than this rather than approximating from delay ticks.
            // Omitted until the first input is applied (a peer's very first snapshots).
            if (pin.hasAppliedSeq)
                appendExt(buf, static_cast<uint16_t>(ExtTag::SnapshotLastAckedSeqNum), pin.lastAppliedSeqNum);
            // Explicit despawn TLV (#516): indices the peer knew that left the sim. Repeated for a few
            // ticks (drop tolerance on the unreliable channel), decrementing each entry's remaining count.
            if (auto& pendingDespawn = *w.pending; !pendingDespawn.empty()) {
                std::vector<uint32_t>& ids = w.despawnIds;
                ids.clear();
                ids.reserve(pendingDespawn.size());
                for (auto it = pendingDespawn.begin(); it != pendingDespawn.end();) {
                    ids.push_back(it->first);
                    if (--(it->second) == 0u)
                        it = pendingDespawn.erase(it);
                    else
                        ++it;
                }
                appendExtRaw(buf, static_cast<uint16_t>(ExtTag::SnapshotDespawn), ids.data(),
                             static_cast<uint16_t>(ids.size() * sizeof(uint32_t)));
            }
            // Cosmetic weapon effects (#625): this tick's events within the peer's interest radius,
            // capped. Read-only over the shared m_wb.m_tickEffects (built serially in the weapons pass),
            // packed byte-serially into the unaligned TLV payload — parallel-safe, worker owns buf.
            if (!m_wb.m_tickEffects.empty() && peerState && !peerState->dead) {
                std::vector<uint8_t>& fx = w.effectsBlob;
                fx.clear();
                fx.reserve(std::min(m_wb.m_tickEffects.size(), kMaxEffectsPerSnapshot) * kEffectRecordBytes);
                const double er2 = govInterestRadiusM * govInterestRadiusM;
                std::size_t emitted = 0;
                for (const WorldBroadcaster::EffectRecord& e : m_wb.m_tickEffects) {
                    if (emitted >= kMaxEffectsPerSnapshot)
                        break;
                    const double dx = static_cast<double>(e.pos[0]) - peerState->transform.pos[0];
                    const double dy = static_cast<double>(e.pos[1]) - peerState->transform.pos[1];
                    const double dz = static_cast<double>(e.pos[2]) - peerState->transform.pos[2];
                    if (dx * dx + dy * dy + dz * dz > er2)
                        continue;
                    const std::size_t at = fx.size();
                    fx.resize(at + kEffectRecordBytes);
                    uint8_t* p = fx.data() + at;
                    p[0] = e.type;
                    p[1] = e.weaponClass;
                    std::memcpy(p + 2, &e.srcIdx, 4);
                    std::memcpy(p + 6, &e.tgtIdx, 4);
                    std::memcpy(p + 10, e.pos, 12);
                    ++emitted;
                }
                if (!fx.empty())
                    appendExtRaw(buf, static_cast<uint16_t>(ExtTag::SnapshotEffects), fx.data(),
                                 static_cast<uint16_t>(fx.size()));
            }
            // Crew turret pose (#972): live mount-frame az/el of each turret on the CREWED entities in
            // this peer's interest set (`selected`, already interest-filtered). Read-only over the
            // serially-built crewSnap — worker owns buf, so byte-identical across worker counts (#512).
            // Absent for a single-seat-only interest set → no TLV → byte-identical to pre-#972.
            if (!crewSnap.empty()) {
                std::vector<uint8_t>& cb = w.crewBlob;
                cb.clear();
                uint8_t count = 0;
                for (uint32_t idx : selected) {
                    if (count == 255u)
                        break;
                    const auto csit = crewSnap.find(idx);
                    if (csit == crewSnap.end())
                        continue;
                    const auto& turrets = csit->second.turrets;
                    const auto tc = static_cast<uint8_t>(std::min<std::size_t>(turrets.size(), 255));
                    const std::size_t at = cb.size();
                    cb.resize(at + 5u + static_cast<std::size_t>(tc) * 4u);
                    uint8_t* p = cb.data() + at;
                    std::memcpy(p, &idx, 4);
                    p[4] = tc;
                    for (uint8_t t = 0; t < tc; ++t) {
                        const int16_t azQ = turrets[t].first, elQ = turrets[t].second;
                        std::memcpy(p + 5 + t * 4, &azQ, 2);
                        std::memcpy(p + 5 + t * 4 + 2, &elQ, 2);
                    }
                    ++count;
                }
                if (count > 0u) {
                    std::vector<uint8_t>& payload = w.payload;
                    payload.clear();
                    payload.reserve(cb.size() + 1u);
                    payload.push_back(count);
                    payload.insert(payload.end(), cb.begin(), cb.end());
                    appendExtRaw(buf, static_cast<uint16_t>(ExtTag::SnapshotCrew), payload.data(),
                                 static_cast<uint16_t>(payload.size()));
                }
            }
            // Actuator positions (#843): the articulated entities in this peer's interest set
            // (`selected`, already interest-filtered), so a REMOTE aircraft's gear and flaps move.
            // Read-only over the serially-built artSnap — this worker owns buf, so the per-peer buffer
            // stays byte-identical across worker counts (#512). Absent for an unarticulated interest
            // set, which is what keeps a world of static meshes at pre-#843 bytes.
            if (!artSnap.empty()) {
                std::vector<uint8_t>& ab = w.articulationBlob;
                ab.clear();
                for (uint32_t idx : selected) {
                    const auto ait = artSnap.find(idx);
                    if (ait == artSnap.end())
                        continue;
                    const ArtSnap& a = ait->second;
                    // Send policy: on CHANGE, plus a periodic refresh (drop tolerance on the
                    // unreliable channel, and late joiners / re-entering interest), plus always the
                    // first time. A steady-state aircraft therefore costs zero articulation bytes
                    // between refreshes, and gear/flap transitions are rare.
                    PeerEntityRec& rec = knownGens[idx];
                    const bool changed = rec.artHash != a.hash;
                    const bool refresh = rec.artSentTick == 0u || (tickIndex - rec.artSentTick) >= kArtRefreshTicks;
                    if (!changed && !refresh)
                        continue;
                    rec.artHash = a.hash;
                    rec.artSentTick = tickIndex;

                    const std::size_t at = ab.size();
                    ab.resize(at + 6u + a.count);
                    uint8_t* p = ab.data() + at;
                    std::memcpy(p, &idx, 4);
                    std::memcpy(p + 4, &a.mask, 2);
                    std::memcpy(p + 6, a.values, a.count);
                }
                if (!ab.empty())
                    appendExtRaw(buf, static_cast<uint16_t>(ExtTag::SnapshotArticulation), ab.data(),
                                 static_cast<uint16_t>(ab.size()));
            }
            // Snapshot payload compression (#775): zstd the fully-assembled payload (origin table +
            // record stream + TLV block) in place, still inside the parallel per-peer region — the
            // codec is deterministic and this worker owns buf, so the #512 byte-identical-across-
            // worker-counts guarantee holds. Raw fallback when compression does not strictly win
            // (tiny/incompressible payloads keep flags == 0 and are wire-identical to compression
            // off). The 24-byte header always stays raw: dispatch, the client's tick dedup, and
            // bot_swarm's metrics read it without decompressing.
            if (const std::size_t payloadSize = buf.size() - sizeof(MsgWorldSnapshotHeader);
                compressSnap && payloadSize <= kMaxSnapshotPayloadBytes) {
                const std::size_t csz = compressSnapshotPayload(buf.data() + sizeof(MsgWorldSnapshotHeader),
                                                                payloadSize, w.compressScratch);
                if (csz > 0u) {
                    hdr.flags |= kSnapshotFlagCompressed;
                    hdr.uncompressedBytes = static_cast<uint32_t>(payloadSize);
                    writeMsgAt(buf, hdrOffset, hdr);
                    buf.resize(sizeof(MsgWorldSnapshotHeader) + csz);
                    std::memcpy(buf.data() + sizeof(MsgWorldSnapshotHeader), w.compressScratch.data(), csz);
                }
            }
            // No m_net.send here — buf is flushed by the sim thread below.
        }
    });

    // Serial flush (sim thread): send each built buffer over the sim-thread-owned ENetHost and record
    // the send-cadence bookkeeping the decimation gate reads next tick (#518). Empty work entries are
    // peers that were decimated this tick (excluded in the gather) and are simply not present.
    //
    // Spectate delay (#403): a spectator (observer / dead pilot) with m_wb.m_spectateDelayTicks > 0 has its
    // POSITIONAL snapshot buffered for that many ticks before delivery (anti-ghosting) — the reliable
    // channels (chat / kill feed / match state) are unaffected. 0 = off = immediate send (byte-identical
    // to before). The send-cadence bookkeeping still advances at build time so the decimation gate is
    // unchanged; only the wire delivery is deferred.
    for (PeerSnapWork& w : m_peerWork) {
        w.pin->lastSnapshotSentTick = tickIndex;
        w.pin->sentSnapshot = true;
        if (m_wb.m_spectateDelayTicks > 0 && w.spectator) {
            m_wb.enqueueDelayedSnapshot(*w.pin, tickIndex + m_wb.m_spectateDelayTicks, w.buf);
        } else {
            m_net.send(w.peerId, w.buf.data(), w.buf.size(), /*reliable=*/false);
        }
    }
    // Drain due delayed snapshots for every peer with a queue (including peers decimated this tick, so a
    // buffered payload is never stranded). Cleared on respawn / role change / disconnect.
    if (m_wb.m_spectateDelayTicks > 0) {
        for (auto& [pid, pin] : m_wb.m_peerInputs) {
            while (!pin.snapshotDelayQueue.empty() && pin.snapshotDelayQueue.front().first <= tickIndex) {
                auto& payload = pin.snapshotDelayQueue.front().second;
                m_net.send(pid, payload.data(), payload.size(), /*reliable=*/false);
                pin.snapshotDelayBytes -= payload.size();
                pin.snapshotDelayQueue.pop_front();
            }
        }
    }
}

} // namespace fl
