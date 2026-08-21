// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/PeerAdmission.h"

#include "ILogger.h"
#include "INetwork.h"
#include "ai/WingmanCommand.h" // the flight check-in ack sent on admission (#610)
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "net/AdminChannel.h" // the shared admin frontend: the per-IP lockout the gauntlet consults
#include "net/NetworkUtils.h" // normalizeIp + extractIp — the one IP-matching pair (#1243)
#include "net/WireCodec.h"    // appendMsg / appendExtRaw / findExt / readRecordAt
#include "net/WorldBroadcaster.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

namespace fl {

namespace {

// ---------------------------------------------------------------------------
// Connection-rejection reason table — one place mapping each ConnectRefusalCode
// to the client-facing reason text and the server-side log phrase/level.
// ---------------------------------------------------------------------------
struct RejectInfo {
    const char* reason;    // sent to the client in MsgConnectRefusal
    const char* logPhrase; // context logged server-side
    LogLevel level;
};
RejectInfo rejectInfoFor(fl::ConnectRefusalCode code) {
    using C = fl::ConnectRefusalCode;
    switch (code) {
    case C::Banned:
        return {"You are banned from this server.", "banned", LogLevel::Info};
    case C::AccessDenied:
        return {"Access denied.", "not on allowlist", LogLevel::Info};
    case C::RateLimited:
        return {"Connection rate limit exceeded. Try again later.", "rate-limited", LogLevel::Info};
    case C::TooManyConnections:
        return {"Too many connections from your address.", "too many connections from this address", LogLevel::Info};
    case C::AdminLockout:
        return {"Access denied.", "admin auth lockout active", LogLevel::Warn};
    case C::RoleDenied:
        return {"The server denied the requested role.", "requested role not allowed", LogLevel::Info};
    case C::MissingRequiredPack:
        return {"You are missing a content pack this server requires.", "missing a required content pack",
                LogLevel::Info};
    case C::EntitlementRequired:
        return {"This server requires premium content you do not own.", "entitlement required", LogLevel::Info};
    case C::MatchFull:
        return {"All teams are full.", "all teams full", LogLevel::Info};
    case C::BadPassword:
        return {"Incorrect server password.", "wrong join password", LogLevel::Info};
    case C::ServerFull:
        // Warn, not Info: unlike the other refusals this one is not about the client at all — it says
        // the operator's entity cap is binding, and it is the line they need to see in the log.
        return {"The server world is full. Try again shortly.", "world at entity soft cap", LogLevel::Warn};
    case C::NoAirframe:
        // Error: nobody can fly on this server until the operator fixes the config, so this is not a
        // condition that clears on its own the way ServerFull does.
        return {"This server has no aircraft available.",
                "no spawnable player entity type (check [world] "
                "player_entity_type and the loaded packs)",
                LogLevel::Error};
    case C::Generic:
        break;
    }
    return {"Access denied.", "access denied", LogLevel::Info};
}
} // namespace

PeerAdmission::PeerAdmission(WorldBroadcaster& wb, EntityManager& entityManager, EntityTypeRegistry& registry,
                             INetwork& net, ILogger& logger, const WorldQueries& queries,
                             const WorldBroadcasterHooks& hooks) noexcept
    : m_wb(wb), m_entityManager(entityManager), m_registry(registry), m_net(net), m_logger(logger), m_queries(queries),
      m_hooks(hooks) {}

void PeerAdmission::setPlayerEntityType(std::string type) {
    m_playerEntityType = std::move(type);
}

void PeerAdmission::setAllowObservers(bool allow) noexcept {
    m_allowObservers = allow;
}

void PeerAdmission::setRequiredPacks(std::vector<RequiredPack> packs) {
    m_requiredPacks = std::move(packs);
}

void PeerAdmission::setRequiredPackPolicy(RequiredPackPolicy policy) noexcept {
    m_requiredPackPolicy = policy;
}

void PeerAdmission::setJoinPassword(std::string password) {
    m_joinPassword = std::move(password);
}

void PeerAdmission::setReconnectGraceTicks(uint64_t ticks) noexcept {
    m_reconnectGraceTicks = ticks;
}

void PeerAdmission::setClock(const IClock& clock) noexcept {
    m_clock = &clock;
}

void PeerAdmission::onDisconnect(uint32_t peerId) {
    // Reconnection (#524): before the broadcaster erases the score, snapshot this peer's identity +
    // tallies under its client guid so a reconnect within the grace window restores them. The score
    // erase stays on the broadcaster's side (peer-id reuse remains unsafe; the guid table is the
    // sanctioned inheritance path).
    if (m_reconnectGraceTicks > 0) {
        if (auto git = m_peerGuids.find(peerId); git != m_peerGuids.end()) {
            GraceRec g;
            if (const auto rit = m_wb.m_roster.find(peerId); rit != m_wb.m_roster.end()) {
                g.callsign = rit->second.callsign;
                g.factionIndex = rit->second.factionIndex;
            }
            if (const auto sit = m_wb.m_scores.find(peerId); sit != m_wb.m_scores.end()) {
                g.kills = sit->second.kills;
                g.losses = sit->second.losses;
                g.score = sit->second.score;
            }
            g.expiresTick = m_wb.m_currentTick + m_reconnectGraceTicks;
            m_disconnectGrace[git->second] = std::move(g);
        }
    }
    m_peerGuids.erase(peerId);
}

void PeerAdmission::pruneStaleRecords(uint64_t currentTick) {
    auto cutoff = m_clock->now() - std::chrono::seconds(m_connectRateWindowS);
    for (auto it = m_connectRecords.begin(); it != m_connectRecords.end();) {
        auto& ts = it->second.timestamps;
        while (!ts.empty() && ts.front() < cutoff)
            ts.pop_front();
        if (ts.empty())
            it = m_connectRecords.erase(it);
        else
            ++it;
    }
    if (m_hooks.comms.adminChannel)
        m_hooks.comms.adminChannel->pruneExpiredLockouts();
    // Reconnection grace (#524): purge expired held identities.
    for (auto it = m_disconnectGrace.begin(); it != m_disconnectGrace.end();) {
        if (currentTick > it->second.expiresTick)
            it = m_disconnectGrace.erase(it);
        else
            ++it;
    }
}

void PeerAdmission::clearDisconnectGrace() noexcept {
    m_disconnectGrace.clear();
}

void PeerAdmission::banAddress(std::string ip) {
    ip = fl::normalizeIp(ip);
    m_bannedAddresses.insert(ip);
    // Walks m_wb.m_peerInputs — EVERY connected peer — not m_wb.m_peerEntities (#1069). Keying the kick on
    // "has an entity" meant a ban did not reach an observer or a peer that had connected but not yet
    // sent its MsgConnectRequest, so the one thing an operator expects a ban to do it did not do for
    // exactly the peers with the least to lose. Reads the cached ip rather than re-extracting.
    for (const auto& [peerId, pin] : m_wb.m_peerInputs) {
        if (pin.peerIp == ip)
            m_net.disconnectPeer(peerId);
    }
}

void PeerAdmission::unbanAddress(const std::string& ip) {
    m_bannedAddresses.erase(fl::normalizeIp(ip));
}

void PeerAdmission::setBannedAddresses(std::unordered_set<std::string> addrs) {
    m_bannedAddresses = std::move(addrs);
}

void PeerAdmission::setAllowedAddresses(std::unordered_set<std::string> addrs) {
    m_allowedAddresses = std::move(addrs);
}

std::unordered_set<std::string> PeerAdmission::bannedAddresses() const {
    return m_bannedAddresses;
}

void PeerAdmission::setRateLimitParams(int maxConnects, int windowSeconds) noexcept {
    m_connectRateLimit = maxConnects;
    m_connectRateWindowS = windowSeconds;
}

void PeerAdmission::setMaxConnectionsPerIp(int max) noexcept {
    m_maxConnectionsPerIp = max;
}

void PeerAdmission::setSpawnPoints(std::vector<std::array<double, 3>> points) noexcept {
    m_spawnPoints = std::move(points);
}

void PeerAdmission::setPlayerFaction(uint16_t faction) noexcept {
    m_playerFaction = faction;
}
void PeerAdmission::onConnect(uint32_t peerId) {
    // Rejection gauntlet — each check logs, sends a MsgConnectRefusal with the matching reason,
    // and disconnects via rejectConnection(). Order matters: cheapest/most-decisive checks first.
    std::string ip = extractIp(m_net.getPeerAddress(peerId));

    // Ban check — reject banned IPs before any state is created.
    if (!ip.empty() && m_bannedAddresses.count(ip)) {
        rejectConnection(peerId, ip, ConnectRefusalCode::Banned);
        return;
    }

    // Allowlist check — if non-empty, only listed IPs may connect.
    if (!ip.empty() && !m_allowedAddresses.empty() && !m_allowedAddresses.count(ip)) {
        rejectConnection(peerId, ip, ConnectRefusalCode::AccessDenied);
        return;
    }

    // Connection rate limit — sliding window per IP.
    if (!ip.empty()) {
        auto now = m_clock->now();
        auto& rec = m_connectRecords[ip];
        auto cutoff = now - std::chrono::seconds(m_connectRateWindowS);
        while (!rec.timestamps.empty() && rec.timestamps.front() < cutoff)
            rec.timestamps.pop_front();
        rec.timestamps.push_back(now);
        if (static_cast<int>(rec.timestamps.size()) > m_connectRateLimit) {
            rejectConnection(peerId, ip, ConnectRefusalCode::RateLimited);
            return;
        }
    }

    // Per-IP concurrent connection limit. Count all connected peers (m_wb.m_peerInputs), including observers
    // and not-yet-admitted peers, not just spawned pilots (#853 defers the spawn past onConnect).
    // Reads each peer's CACHED ip (#1069): this walk previously called
    // extractIp(getPeerAddress(pid)) per connected peer per connect attempt, building a std::string
    // every time — an O(P) allocation storm on the sim thread that an attacker triggers by connecting.
    if (m_maxConnectionsPerIp > 0 && !ip.empty()) {
        int count = 0;
        for (const auto& [pid, pin] : m_wb.m_peerInputs)
            if (pin.peerIp == ip)
                ++count;
        if (count >= m_maxConnectionsPerIp) {
            rejectConnection(peerId, ip, ConnectRefusalCode::TooManyConnections);
            return;
        }
    }

    // Admin auth lockout — refuse reconnections from IPs with an active lockout.
    if (m_hooks.comms.adminChannel && m_hooks.comms.adminChannel->lockedOut(ip)) {
        rejectConnection(peerId, ip, ConnectRefusalCode::AdminLockout);
        return;
    }

    char msg[64];
    std::snprintf(msg, sizeof(msg), "peer %u connected", peerId);
    m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);

    // Version handshake. The client checks protocolVersion and disconnects on mismatch.
    //
    // The BUILD version rides along as a TLV (#1074). kProtocolVersion stays 1 for every additive
    // message and ExtTag through primary development, so it cannot distinguish a v0.3.11 peer from a
    // v0.4.1 one: both advertise protocol 1, complete the handshake, and then silently disagree about
    // every message added in between. MsgHello is the first thing a server sends, so the client knows
    // the build before committing to anything. Warn-only — see ExtTag::HelloBuildVersion.
    std::vector<uint8_t> helloBuf;
    MsgHello hello;
    appendMsg(helloBuf, hello);
    if (!m_wb.m_buildVersion.empty()) {
        const auto len = static_cast<uint16_t>(std::min(m_wb.m_buildVersion.size(), kBuildVersionBytes));
        appendExtRaw(helloBuf, static_cast<uint16_t>(ExtTag::HelloBuildVersion), m_wb.m_buildVersion.data(), len);
    }
    m_net.send(peerId, helloBuf.data(), helloBuf.size(), /*reliable=*/true);

    // Create the peer's input slot now, BEFORE admission (#853). A peer that connects but never sends a
    // MsgConnectRequest keeps this slot (so idle-timeout covers it) but has no entity, no role, and no
    // snapshot delivery. Admission — spawn, ConnectAck, MOTD, flight — happens in handleConnectRequest
    // when the request arrives, replacing the old "server unilaterally spawns on connect" flow.
    m_wb.m_peerInputs[peerId] = {};
    m_wb.m_peerInputs[peerId].lastActivityTick = m_wb.m_currentTick;
    // Resolve the source IP once (#1069). Every later per-IP question — the concurrent-connection
    // count above, and anything that follows it — reads this instead of re-extracting from the
    // transport. Empty when the address is unknown, which the IP checks already treat as "skip".
    m_wb.m_peerInputs[peerId].peerIp = ip;

    m_wb.m_activePeerCount.fetch_add(1, std::memory_order_relaxed);
}

EntityId PeerAdmission::admitPilot(uint32_t peerId, const std::string& entityType, uint16_t faction) {
    EntityTransform t{};
    t.quat[3] = 1.0f; // identity quaternion (w component; XYZW layout)
    if (!m_spawnPoints.empty()) {
        // Explicit cast avoids uint32_t/size_t width mismatch warning on MSVC (/W4 → error).
        const std::size_t idx = static_cast<std::size_t>(m_nextSpawnIdx++) % m_spawnPoints.size();
        t.pos[0] = m_spawnPoints[idx][0];
        t.pos[1] = m_spawnPoints[idx][1];
        t.pos[2] = m_spawnPoints[idx][2];
    } else {
        constexpr double kSpawnAGL = 500.0;
        t.pos[0] = 0.0;
        t.pos[2] = 60.0; // 60 m ahead of origin so peer doesn't overlap sandbox entity 0
        t.pos[1] = static_cast<double>(m_wb.m_groundElevation.load(std::memory_order_relaxed)) + kSpawnAGL;
    }
    // Sandbox / round-robin fallback path: spawn stationary. The bare no-mission player flies the
    // builtin UFO, which is controllable at zero airspeed; a mission's airborne player comes through the
    // slot path below with a real cruise speed (#883).
    const uint16_t f = (faction == kNoFaction) ? m_playerFaction : faction;
    return m_wb.spawnPilotEntity(peerId, entityType, t, f, /*initialAirspeed=*/0.f);
}

void PeerAdmission::setMissionPlayerSlots(std::vector<MissionSpawnSlot> slots) {
    m_missionSlots = std::move(slots);
    m_slotOccupant.assign(m_missionSlots.size(), kSlotFree);
    m_peerSlot.clear();
}

int PeerAdmission::claimMissionSlot(uint32_t peerId, uint16_t preferredFaction) {
    // First pass: prefer an open slot whose faction matches the requested team (#522). Slots are
    // authoritative about WHERE a pilot spawns; the balancer is authoritative about WHICH SIDE.
    if (preferredFaction != kNoFaction) {
        for (std::size_t i = 0; i < m_slotOccupant.size(); ++i) {
            if (m_slotOccupant[i] == kSlotFree && m_missionSlots[i].factionIndex == preferredFaction) {
                m_slotOccupant[i] = peerId;
                m_peerSlot[peerId] = static_cast<int>(i);
                return static_cast<int>(i);
            }
        }
    }
    // Fallback: the next open slot regardless of faction (its faction becomes the pilot's team).
    for (std::size_t i = 0; i < m_slotOccupant.size(); ++i) {
        if (m_slotOccupant[i] == kSlotFree) {
            m_slotOccupant[i] = peerId;
            m_peerSlot[peerId] = static_cast<int>(i);
            return static_cast<int>(i);
        }
    }
    return -1; // no slots, or all occupied → caller falls back to the round-robin path
}

void PeerAdmission::releaseMissionSlot(uint32_t peerId) {
    const auto it = m_peerSlot.find(peerId);
    if (it == m_peerSlot.end())
        return;
    const int idx = it->second;
    if (idx >= 0 && static_cast<std::size_t>(idx) < m_slotOccupant.size()) {
        m_slotOccupant[idx] = kSlotFree;
        // Unbind the mission object id (#884): the slot is open again, so destroy(<id>) must report the
        // slot as unoccupied (not destroyed) rather than tracking the just-despawned aircraft.
        const MissionSpawnSlot& slot = m_missionSlots[static_cast<std::size_t>(idx)];
        if (m_hooks.admission.missionSlotBinder && !slot.missionObjectId.empty())
            m_hooks.admission.missionSlotBinder(slot.missionObjectId, EntityId{});
    }
    m_peerSlot.erase(it);
}

std::string PeerAdmission::resolvePlayerEntityType(const char* requested) const {
    // A client-requested type wins only if it names a REGISTERED type (server-clamped allowlist — the
    // client cannot conjure an arbitrary spawn). Otherwise fall back to the [world] default, then the
    // builtin. An unregistered request is not an error; it is served the default (#834).
    if (requested && requested[0] != '\0') {
        if (m_registry.findById(requested))
            return requested;
        char msg[160];
        std::snprintf(msg, sizeof(msg), "peer requested unregistered entity type '%.64s'; using server default",
                      requested);
        m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);
    }
    if (!m_playerEntityType.empty() && m_registry.findById(m_playerEntityType.c_str()))
        return m_playerEntityType;
    return "builtin:debug-entity";
}

void PeerAdmission::handleConnectRequest(uint32_t peerId, const void* data, std::size_t size) {
    if (size < sizeof(MsgConnectRequest))
        return; // truncated; ignore
    MsgConnectRequest req;
    std::memcpy(&req, data, sizeof(req));
    req.requestedEntityType[sizeof(req.requestedEntityType) - 1] = '\0'; // untrusted char[]: force-terminate

    // The peer's source IP, resolved once at connect (#1069). Every refusal below reports it; before
    // the cache each of those seven sites re-extracted and re-normalized the same string.
    const std::string& peerIp = m_wb.m_peerInputs[peerId].peerIp;

    if (req.protocolVersion != kProtocolVersion) {
        // The client also checks MsgHello and disconnects; refuse here as a backstop.
        rejectConnection(peerId, peerIp, ConnectRefusalCode::Generic);
        return;
    }

    PeerInputState& pin = m_wb.m_peerInputs[peerId];
    if (pin.handshakeComplete) {
        // A peer sends exactly one request; ignore repeats so it can't re-spawn or churn its role.
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__, "duplicate MsgConnectRequest ignored");
        return;
    }

    // Join password (#998): the cheapest decisive check, before any admission side effect. Compare the
    // client's ConnectJoinPassword TLV against the configured password in constant time (no length or
    // early-exit timing oracle). Missing/wrong => refuse. Applies to pilots AND observers.
    if (!m_joinPassword.empty()) {
        const std::size_t extOff =
            sizeof(MsgConnectRequest) + static_cast<std::size_t>(req.packCount) * sizeof(PackManifestEntry);
        uint8_t supplied[64] = {};
        std::size_t suppliedLen = 0;
        if (extOff <= size) {
            uint16_t plen = 0;
            const uint8_t* pp = findExt(static_cast<const uint8_t*>(data) + extOff, size - extOff,
                                        static_cast<uint16_t>(ExtTag::ConnectJoinPassword), plen);
            if (pp && plen > 0u && plen <= sizeof(supplied)) {
                std::memcpy(supplied, pp, plen);
                suppliedLen = plen;
            }
        }
        const std::string& pw = m_joinPassword;
        uint8_t diff = (suppliedLen == pw.size()) ? 0u : 1u;
        for (std::size_t i = 0; i < sizeof(supplied); ++i) {
            const uint8_t a = supplied[i];
            const uint8_t b = (i < pw.size()) ? static_cast<uint8_t>(pw[i]) : 0u;
            diff |= (a ^ b);
        }
        for (std::size_t i = sizeof(supplied); i < pw.size(); ++i)
            diff |= static_cast<uint8_t>(pw[i]);
        if (diff != 0u) {
            rejectConnection(peerId, peerIp, ConnectRefusalCode::BadPassword);
            return;
        }
    }

    // Required-pack policy (#872). The connect handshake carries the client's mounted-pack manifest;
    // compare it against the server's required set and apply the configured policy. `missingPackNotice`
    // is filled under the WARN policy so the client is notified (via MsgServerNotice) AFTER admission.
    std::string missingPackNotice;
    if (!m_requiredPacks.empty()) {
        std::vector<ClientPack> clientPacks;
        std::size_t off = sizeof(MsgConnectRequest);
        for (uint16_t i = 0; i < req.packCount; ++i) {
            PackManifestEntry pe;
            if (!readRecordAt(data, size, off, pe))
                break; // truncated manifest — treat the rest as absent
            off += sizeof(PackManifestEntry);
            pe.id[sizeof(pe.id) - 1] = '\0';           // untrusted char[]: force-terminate
            pe.version[sizeof(pe.version) - 1] = '\0'; // untrusted char[]: force-terminate
            clientPacks.push_back({pe.id, pe.version});
        }
        const std::vector<std::string> missing = missingRequiredPacks(m_requiredPacks, clientPacks);
        if (!missing.empty()) {
            std::string list;
            for (const std::string& id : missing) {
                if (!list.empty())
                    list += ", ";
                list += id;
            }
            const std::string& ip = peerIp;
            switch (m_requiredPackPolicy) {
            case RequiredPackPolicy::Refuse: {
                char logmsg[256];
                std::snprintf(logmsg, sizeof(logmsg), "peer %u from %s refused: missing required pack(s): %.160s",
                              peerId, ip.c_str(), list.c_str());
                m_logger.log(LogLevel::Info, __FILE__, __LINE__, logmsg);
                char reason[sizeof(MsgConnectRefusal::reason)];
                std::snprintf(reason, sizeof(reason), "Missing required pack(s): %s", list.c_str());
                sendConnectRefusal(peerId, ConnectRefusalCode::MissingRequiredPack, reason);
                m_net.disconnectPeer(peerId);
                return; // NOT admitted: handshakeComplete stays false, no entity spawned
            }
            case RequiredPackPolicy::Warn: {
                for (const std::string& id : missing) {
                    char msg[128];
                    std::snprintf(msg, sizeof(msg), "peer %u is missing required content pack '%.80s'", peerId,
                                  id.c_str());
                    m_logger.log(LogLevel::Warn, __FILE__, __LINE__, msg);
                }
                missingPackNotice = list; // notify the client after admission
                break;
            }
            case RequiredPackPolicy::AllowPlaceholder: {
                char msg[192];
                std::snprintf(msg, sizeof(msg),
                              "peer %u admitted with %zu missing content pack(s) (allow-placeholder): %.120s", peerId,
                              missing.size(), list.c_str());
                m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);
                break;
            }
            }
        }
    }

    // Grant a role (#857). An out-of-grammar role byte is refused; an observer is refused when the
    // server disallows the role. (The required-pack policy #872 has already run above.)
    if (!isPeerRoleOrdinal(req.requestedRole)) {
        rejectConnection(peerId, peerIp, ConnectRefusalCode::Generic);
        return;
    }
    const PeerRole grantedRole = static_cast<PeerRole>(req.requestedRole);
    if (grantedRole == PeerRole::Observer && !m_allowObservers) {
        rejectConnection(peerId, peerIp, ConnectRefusalCode::RoleDenied);
        return;
    }

    // #974 join-at-connect: the client may claim a seat of an EXISTING crewed aircraft (a ConnectSeatClaim
    // TLV in the ConnectRequest ext block) instead of spawning its own. Parsed here; applied in the Pilot
    // branch below. Falls back to a normal spawn when the seat is unavailable, so a pilot always gets in.
    bool seatClaim = false;
    EntityId claimEntity{};
    uint8_t claimSeat = 0;
    std::string reconnectGuid; // #524: client identity, for team/score restore on reconnect
    {
        const std::size_t extOff =
            sizeof(MsgConnectRequest) + static_cast<std::size_t>(req.packCount) * sizeof(PackManifestEntry);
        if (extOff <= size) {
            const uint8_t* ext = static_cast<const uint8_t*>(data) + extOff;
            const std::size_t extSize = size - extOff;
            uint16_t clen = 0;
            const uint8_t* cp = findExt(ext, extSize, static_cast<uint16_t>(ExtTag::ConnectSeatClaim), clen);
            if (cp && clen >= 9u) {
                uint32_t ei = 0, eg = 0;
                std::memcpy(&ei, cp, 4);
                std::memcpy(&eg, cp + 4, 4);
                claimSeat = cp[8];
                claimEntity = EntityId{ei, eg};
                seatClaim = true;
            }
            uint16_t glen = 0;
            const uint8_t* gp = findExt(ext, extSize, static_cast<uint16_t>(ExtTag::ConnectIdentity), glen);
            if (gp && glen > 0u && glen <= 40u)
                reconnectGuid.assign(reinterpret_cast<const char*>(gp), glen);
        }
    }

    // Reconnection (#524): if this guid is held in the grace table and not expired, the reconnecting
    // player keeps their team + score. A guid already held by a LIVE peer is treated as fresh. Store the
    // guid for the disconnect snapshot regardless.
    const GraceRec* grace = nullptr;
    if (!reconnectGuid.empty()) {
        bool liveDuplicate = false;
        for (const auto& [pid, g] : m_peerGuids) {
            (void)pid;
            if (g == reconnectGuid) {
                liveDuplicate = true;
                break;
            }
        }
        if (!liveDuplicate) {
            if (auto git = m_disconnectGrace.find(reconnectGuid); git != m_disconnectGrace.end()) {
                if (m_wb.m_currentTick <= git->second.expiresTick)
                    grace = &git->second;
                else
                    m_disconnectGrace.erase(git); // expired — purge lazily
            }
        } else {
            m_logger.log(LogLevel::Warn, __FILE__, __LINE__, "duplicate live client guid on connect; not restoring");
        }
        m_peerGuids[peerId] = reconnectGuid;
    }

    // Team assignment (#522): consult the mode balancer BEFORE claiming a slot. nullopt = every team is
    // full → refuse. Unset assigner ⇒ kNoFaction, preserving the legacy slot/player-faction behavior.
    uint16_t assignedFaction = kNoFaction;
    if (grantedRole == PeerRole::Pilot && m_queries.teamAssigner) {
        std::optional<uint16_t> team = m_queries.teamAssigner(peerId);
        if (!team.has_value()) {
            rejectConnection(peerId, peerIp, ConnectRefusalCode::MatchFull);
            return;
        }
        assignedFaction = *team;
        // A reconnecting player rejoins their old team rather than being re-balanced (#524).
        if (grace)
            assignedFaction = grace->factionIndex;
    }

    EntityId assigned{}; // invalid for an observer — it has no entity
    // Watermark for telling the two admission failures apart below (#1049): a spawn refused by the
    // soft cap bumps this counter, an unregistered entity type does not. Cheaper and more exact than
    // re-deriving "is the cap binding" from a live count that is only republished at end of tick.
    const uint64_t capRefusalsBefore = m_entityManager.softCapRefusals();
    if (grantedRole == PeerRole::Pilot) {
        // Seat claim first (#974): occupy the requested seat of an existing crewed aircraft, viewing it
        // as our "own" entity. Only when the seat is actually joinable; else fall through to a spawn.
        if (seatClaim && m_wb.evaluateSeatRequest(claimEntity, claimSeat, peerId) == SeatResultCode::Granted) {
            m_wb.setSeatOccupant(claimEntity, claimSeat, peerId);
            assigned = claimEntity; // a gunner does not OWN the airframe (no m_wb.m_peerEntities entry)
        } else {
            // Mission player slots (#854): a loaded mission's `player: true` objects become joinable slots.
            // Assign the next open one (its type/faction/spawn pins where and what the pilot flies, ignoring
            // the client's requested type). No slots / all occupied / a slot type that won't spawn ⇒ fall
            // back to the round-robin default so a pilot always gets an aircraft. When a team is assigned,
            // prefer a slot on that side (#522).
            const int slotIdx = claimMissionSlot(peerId, assignedFaction);
            if (slotIdx >= 0) {
                const MissionSpawnSlot& slot = m_missionSlots[static_cast<std::size_t>(slotIdx)];
                EntityTransform t{};
                t.pos[0] = slot.pos[0];
                t.pos[1] = slot.pos[1];
                t.pos[2] = slot.pos[2];
                for (int c = 0; c < 4; ++c)
                    t.quat[c] = slot.quat[c];
                assigned = m_wb.spawnPilotEntity(peerId, slot.entityType, t, slot.factionIndex, slot.airspeed);
                if (!assigned.valid()) {
                    releaseMissionSlot(peerId); // slot type unspawnable — free it and use the default path
                    assigned = admitPilot(peerId, resolvePlayerEntityType(req.requestedEntityType), assignedFaction);
                } else {
                    // The fit the mission chose for this slot (#1209), applied now that the airframe
                    // exists and has a controller — a gunnery lesson flown with the gun, not with two
                    // missiles still on the rails. Empty (every slot before this) leaves the entity
                    // def's default payload untouched. Per-store problems are the AUTHOR's to hear
                    // about, so they are logged rather than swallowed; the pilot still flies.
                    if (!slot.loadout.empty()) {
                        std::vector<std::string> warnings;
                        const std::vector<std::string> stores = slot.loadout; // slot aliases m_missionSlots
                        if (!m_wb.setEntityLoadout(assigned, stores, warnings))
                            m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                                         "mission slot loadout not applied (no weapon registry or no stations)");
                        for (const std::string& w : warnings)
                            m_logger.log(LogLevel::Warn, __FILE__, __LINE__, ("mission slot loadout: " + w).c_str());
                    }
                    if (m_hooks.admission.missionSlotBinder && !slot.missionObjectId.empty()) {
                        // Register the pilot's aircraft under the slot's mission object id so destroy(<id>)
                        // tracks it (#884). slot is a reference into m_missionSlots; read its id before any
                        // further work.
                        m_hooks.admission.missionSlotBinder(slot.missionObjectId, assigned);
                    }
                }
            } else {
                assigned = admitPilot(peerId, resolvePlayerEntityType(req.requestedEntityType), assignedFaction);
            }
        }

        // No airframe (#1049). Every fallback above has been tried, so this is the world itself
        // refusing. Acking anyway would admit a "pilot" with nothing to fly — no camera anchor, no
        // controller, no own-record in the snapshot — i.e. a client stuck on a loading screen with no
        // reason given, which is what the server did before this. Refuse, and name the wall that was
        // hit: a full world clears on its own, a missing entity type needs the operator.
        if (!assigned.valid()) {
            releaseMissionSlot(peerId);
            const bool capBound = m_entityManager.softCapRefusals() > capRefusalsBefore;
            rejectConnection(peerId, peerIp,
                             capBound ? ConnectRefusalCode::ServerFull : ConnectRefusalCode::NoAirframe);
            return;
        }
    } else {
        // Observer (#857): no entity, no controller. Seed the interest center from the first spawn point
        // (or origin) as a placeholder until #858 drives it from the client's camera eye.
        pin.interestCenter = !m_spawnPoints.empty()
                                 ? glm::dvec3(m_spawnPoints[0][0], m_spawnPoints[0][1], m_spawnPoints[0][2])
                                 : glm::dvec3(0.0, 0.0, 0.0);
    }

    pin.role = grantedRole;
    pin.handshakeComplete = true;

    sendConnectAck(peerId, assigned, grantedRole);

    // MOTD, unicast once on admission (moved from onConnect; the banner itself is SessionComms', #1087).
    m_wb.m_comms.sendMotdTo(peerId);

    // Missing-content notice (#872 warn policy): tell the admitted client which required packs it lacks,
    // so a content mismatch is visible instead of silent placeholders. Reuses the MsgServerNotice banner
    // channel the client already surfaces (console + banner).
    if (!missingPackNotice.empty()) {
        MsgServerNotice notice;
        std::snprintf(notice.text, sizeof(notice.text), "Missing content: %s", missingPackNotice.c_str());
        m_net.send(peerId, &notice, sizeof(notice), /*reliable=*/true);
    }

    // Form this peer's flight (#610). The spawner lives in fl-server (it needs engine-ai to build
    // controllers); with no spawner installed the peer simply flies alone, which is exactly the
    // pre-#610 behavior. A seat-claim joiner (#974) OWNS no airframe (not in m_wb.m_peerEntities), so it
    // forms no flight — it is a gunner on someone else's aircraft.
    if (assigned.valid() && m_queries.flightSpawner && m_wb.m_peerEntities.count(peerId) != 0u) {
        const fl::FormationId fid = m_queries.flightSpawner(peerId, assigned);
        if (const fl::Formation* f = m_wb.m_formations.get(fid)) {
            // Unsolicited check-in: this is how the client learns it HAS a flight, how big it is, and
            // what id to address it by. MsgConnectAck cannot carry it — it is immediately followed by
            // MsgEntityTypeDef records, so appending a field there would shift them.
            MsgWingmanAck ack{};
            ack.command = static_cast<uint8_t>(fl::ai::WingmanCommand::Rejoin);
            ack.result = static_cast<uint8_t>(WingmanResult::CheckIn);
            ack.flightSize = static_cast<uint8_t>(std::min<std::size_t>(f->members.size(), 255));
            ack.memberIdx = f->members.empty() ? kFlightAll : f->members.front().id.index;
            ack.flightId = fid;
            m_net.send(peerId, &ack, sizeof(ack), /*reliable=*/true);
        }
    }

    // A crewed pilot aircraft just spawned — its Fly seat is now Human. Tell existing peers so their
    // seat rosters stay current (the joiner already received all rosters via sendConnectAck). #972.
    if (assigned.valid())
        m_wb.broadcastCrewRoster(assigned);

    // Match roster (#996), last so the ordered MOTD/flight-check-in sends above keep their positions.
    // Send the joiner the current roster (everyone already here), then insert + broadcast its own
    // record so existing peers learn of the join and the joiner sees itself last.
    req.callsign[sizeof(req.callsign) - 1] = '\0'; // untrusted char[]: force-terminate
    m_wb.sendFullRoster(peerId);
    {
        uint16_t myFaction = 0;
        if (assigned.valid()) {
            if (const EntityState* s = m_entityManager.get(assigned))
                myFaction = s->factionIndex;
        }
        WorldBroadcaster::RosterRec rec;
        rec.callsign = WorldBroadcaster::sanitizeCallsign(req.callsign, peerId);
        rec.factionIndex = myFaction;
        rec.role = grantedRole;
        rec.isBot = false;
        m_wb.upsertRoster(peerId, rec);
        // A pilot (not an observer) is a match participant with a scoreboard row (#523).
        if (grantedRole == PeerRole::Pilot)
            m_wb.recordParticipant(peerId, myFaction, /*isBot=*/false, /*joined=*/true);
    }

    // Reconnection (#524): restore the held score tallies, then consume the grace entry. The dirty flag
    // owes the reconnector a fresh Stats unicast + refreshes the scoreboard.
    if (grace) {
        WorldBroadcaster::PeerScore& s = m_wb.m_scores[peerId];
        s.kills = grace->kills;
        s.losses = grace->losses;
        s.score = grace->score;
        s.dirty = true;
        m_wb.m_comms.markScoreboardDirty();
        m_disconnectGrace.erase(reconnectGuid);
    }

    m_wb.m_comms.invalidateVoiceViews(); // #1090: a new admitted peer changes the voice recipient set

    // Match state + scoreboard for the late joiner (#523): the current phase/scores + everyone's row.
    m_wb.sendMatchStateTo(peerId);
    m_wb.m_comms.sendScoreboardTo(peerId);
}
void PeerAdmission::sendConnectAck(uint32_t peerId, EntityId assigned, PeerRole grantedRole) {
    // Type-table skip (#1070). This function is re-sent on every seat change, role change, team change
    // and authority grant — not only at connect — and the table is typeCount x 380 B, about 23 KB at a
    // realistic 60-type registry. The client already has it; re-sending was pure amplification, and it
    // is what made the un-rate-limited seat/team requests (#1069) worth attacking. When the registry's
    // generation is the one this peer was last sent, the records are omitted and a
    // ConnectAckTypesUnchanged tag says so. A peer that has never been sent the table always gets it.
    const uint32_t tableGen = m_registry.generation();
    bool sendTypes = true;
    if (auto pit = m_wb.m_peerInputs.find(peerId); pit != m_wb.m_peerInputs.end())
        sendTypes = !pit->second.hasTypeTable || pit->second.sentTypeTableGen != tableGen;
    const uint32_t typeCount = sendTypes ? m_registry.typeCount() : 0u;

    std::vector<uint8_t> buf;
    buf.reserve(sizeof(MsgConnectAck) + typeCount * sizeof(MsgEntityTypeDef));

    MsgConnectAck ack;
    ack.msgId = static_cast<uint8_t>(MsgId::ConnectAck);
    ack.tickRateHz = m_wb.m_tickRate.hz(); // #1075: the value that actually governs, not a literal
    ack.typeCount = static_cast<uint16_t>(typeCount);
    ack.assignedEntityIdx = assigned.index;
    ack.assignedEntityGen = assigned.generation;
    ack.planetRadiusKm = m_wb.m_planetRadiusKm;
    ack.grantedRole = static_cast<uint8_t>(grantedRole);
    ack.peerId = peerId; // the client's own id, for the roster "you" highlight + chat self-echo (#996)
    appendMsg(buf, ack);

    for (uint32_t i = 0; i < typeCount; ++i) {
        const EntityDef* def = m_registry.byIndex(i);
        if (!def)
            break;
        MsgEntityTypeDef typeDef{};
        typeDef.typeIndex = i;
        std::snprintf(typeDef.id, sizeof(typeDef.id), "%s", def->id.c_str());
        std::snprintf(typeDef.mesh, sizeof(typeDef.mesh), "%s", def->mesh.c_str());
        std::snprintf(typeDef.dmgMesh, sizeof(typeDef.dmgMesh), "%s", def->classicDamageMesh.c_str());
        // The client integrates the same aircraft we do, or its prediction is a fiction (#811).
        std::snprintf(typeDef.flightModel, sizeof(typeDef.flightModel), "%s", def->flightModelAsset.c_str());
        // ...carrying the same stores, at the same cost (#812). The client gets two floats rather
        // than the hardpoints and a weapon registry it would need to derive them itself.
        const PayloadEffect payload = m_queries.payload ? m_queries.payload(*def) : PayloadEffect{};
        typeDef.payloadMassKg = payload.extra_mass_kg;
        typeDef.payloadCd0 = payload.extra_cd0;
        // Friendly display name for the observer entity picker (#860); empty falls back to id client-side.
        std::snprintf(typeDef.name, sizeof(typeDef.name), "%s", def->name.c_str());
        // Category + projectile weapon class (#886): the client selects the builtin placeholder
        // silhouette (and, later, picker grouping / map icons) on these; without them every
        // client-side def read back as an AirVehicle.
        typeDef.category = static_cast<uint8_t>(def->category);
        typeDef.projectileKind = static_cast<uint8_t>(def->projectileKind);
        // Flight-deck footprint (#38): the client composes its prediction floor as
        // max(terrain, moving deck) from these three; 0 = no deck.
        if (def->deck && def->acceptsLandings) {
            typeDef.deckLengthM = def->deck->lengthM;
            typeDef.deckWidthM = def->deck->widthM;
            typeDef.deckHeightM = def->deck->heightM;
        }
        // Variant node-set (#882): which tagged node-set of a shared family mesh the client draws.
        // A render-only selection, but the client has no pack entity def to read it from.
        std::snprintf(typeDef.meshVariant, sizeof(typeDef.meshVariant), "%s", def->meshVariant.c_str());

        appendMsg(buf, typeDef);
    }

    // Type-table skip marker (#1070): zero-length tag, present exactly when the records above were
    // omitted. Written BEFORE the authority TLV so the block stays in ascending tag order. Record the
    // generation this peer now holds, so the next re-ack can compare against it.
    if (!sendTypes) {
        appendExtRaw(buf, static_cast<uint16_t>(ExtTag::ConnectAckTypesUnchanged), nullptr, 0);
    } else if (auto pit = m_wb.m_peerInputs.find(peerId); pit != m_wb.m_peerInputs.end()) {
        pit->second.sentTypeTableGen = tableGen;
        pit->second.hasTypeTable = true;
    }

    // Granted-authority TLV (#949): appended after the entity-type records when this peer holds caps,
    // so the client can show/hide GM/moderator/faction-leader UI. Cosmetic only — the server remains
    // the enforcement point. Old clients iterate the records by typeCount and skip the unknown tag.
    // Re-sent on a mid-session grant/revoke (setPeerAuthority re-calls sendConnectAck). Payload is
    // { uint64 caps LE, uint16 factionIndex LE }, 10 bytes, matching ExtTag::ConnectAckAuthority.
    if (auto pit = m_wb.m_peerInputs.find(peerId); pit != m_wb.m_peerInputs.end() && pit->second.authority.any()) {
        const PeerAuthority& auth = pit->second.authority;
        uint8_t payload[sizeof(uint64_t) + sizeof(uint16_t)];
        const uint64_t caps = auth.caps;
        std::memcpy(payload, &caps, sizeof(caps));
        std::memcpy(payload + sizeof(caps), &auth.factionIndex, sizeof(auth.factionIndex));
        appendExtRaw(buf, static_cast<uint16_t>(ExtTag::ConnectAckAuthority), payload, sizeof(payload));
    }

    m_net.send(peerId, buf.data(), buf.size(), /*reliable=*/true);

    // Faction id/name table (#860): one reliable packet of concatenated MsgFactionDef records, so the
    // client can name the faction behind each entity's snapshot factionIndex. Skipped when no registry
    // is configured (the client then shows the faction index alone).
    if (m_wb.m_factionRegistry) {
        const uint16_t factionCount = m_wb.m_factionRegistry->count();
        if (factionCount > 0) {
            std::vector<uint8_t> fbuf;
            fbuf.reserve(static_cast<std::size_t>(factionCount) * sizeof(MsgFactionDef));
            for (uint16_t fi = 0; fi < factionCount; ++fi) {
                const FactionDef* fdef = m_wb.m_factionRegistry->get(fi);
                if (!fdef)
                    continue;
                MsgFactionDef fmsg{};
                fmsg.factionIndex = fi;
                std::snprintf(fmsg.id, sizeof(fmsg.id), "%s", fdef->id.c_str());
                std::snprintf(fmsg.name, sizeof(fmsg.name), "%s", fdef->name.c_str());
                appendMsg(fbuf, fmsg);
            }
            if (!fbuf.empty())
                m_net.send(peerId, fbuf.data(), fbuf.size(), /*reliable=*/true);

            // Current airspace posture per faction (#162). MsgAlertLevelChange is broadcast on
            // change, so without this a peer joining after the change sits at Peacetime while the
            // war is on. Sent as individual messages rather than a table: the same message serves
            // both paths, so there is one decode branch on the client instead of two.
            for (uint16_t fi = 0; fi < factionCount; ++fi) {
                MsgAlertLevelChange amsg;
                amsg.factionIndex = fi;
                amsg.level = static_cast<uint8_t>(m_wb.m_factionRegistry->alertLevel(fi));
                m_net.send(peerId, &amsg, sizeof(amsg), /*reliable=*/true);
            }
        }
    }

    // Radio-net table (#532): the client cannot key a mic until it knows which nets exist and what
    // each one sounds like. Sent beside the faction table for the same reason — both are small,
    // server-authoritative vocabularies the client needs before its first frame.
    m_wb.m_comms.sendVoiceNetDefs(peerId);

    // Mission roster (#914): one reliable packet of concatenated MsgMissionRoster records mapping each
    // spawned mission object's entity idx/gen -> its mission object id, so the cinematic recorder (#909)
    // can resolve an entity-relative camera shot's target/look_at (a mission object id) to a live
    // network entity. Only entries whose entity is currently valid are sent (an unoccupied player slot
    // has an invalid EntityId — omitted until a pilot binds it, then delivered as a delta). Empty when
    // no mission is loaded.
    if (!m_wb.m_missionRoster.empty()) {
        std::vector<uint8_t> rbuf;
        rbuf.reserve(m_wb.m_missionRoster.size() * sizeof(MsgMissionRoster));
        for (const auto& [objectId, eid] : m_wb.m_missionRoster) {
            if (eid.generation == 0)
                continue; // invalid entity (e.g. an unbound player slot)
            MsgMissionRoster rmsg{};
            rmsg.entityIdx = eid.index;
            rmsg.entityGen = static_cast<uint16_t>(eid.generation);
            std::snprintf(rmsg.objectId, sizeof(rmsg.objectId), "%s", objectId.c_str());
            appendMsg(rbuf, rmsg);
        }
        if (!rbuf.empty())
            m_net.send(peerId, rbuf.data(), rbuf.size(), /*reliable=*/true);
    }

    // Crew rosters (#972): after the faction table, send this peer the seat roster of EVERY crewed
    // aircraft in the world — its own (so it learns which seat it occupies and who its bots are) and any
    // other crewed airframe (so the seat-selection UI #975 and spectate can label it). Single-seat
    // aircraft send nothing (buildCrewRosterPacket returns false). Occupancy changes re-broadcast later.
    for (const auto& [entityIdx, ce] : m_wb.m_controlledEntities) {
        (void)entityIdx;
        if (ce.crew.crewed())
            m_wb.sendCrewRoster(peerId, ce.id);
    }
}

void PeerAdmission::readmitPilots() {
    // Collect peer ids first — admitPilot / sendConnectAck mutate maps we are iterating.
    std::vector<uint32_t> pilots;
    for (const auto& [peerId, pin] : m_wb.m_peerInputs) {
        if (pin.handshakeComplete && pin.role == PeerRole::Pilot && m_wb.m_peerEntities.count(peerId) == 0u)
            pilots.push_back(peerId);
    }
    for (uint32_t peerId : pilots) {
        uint16_t fac = kNoFaction;
        if (m_queries.teamAssigner) {
            std::optional<uint16_t> t = m_queries.teamAssigner(peerId);
            if (!t.has_value())
                continue; // no room — leave the pilot entity-less (a rare edge; they can retry)
            fac = *t;
        }
        const EntityId assigned = admitPilot(peerId, resolvePlayerEntityType(""), fac);
        if (!assigned.valid()) {
            // World at the entity soft cap (#1049) — same handling as "no room on any team" above:
            // leave the pilot entity-less and say so, rather than acking an aircraft that does not
            // exist. The next round start or a freed slot picks them up.
            char msg[112];
            std::snprintf(msg, sizeof(msg), "peer %u not readmitted: no airframe (entity soft cap?)", peerId);
            m_logger.log(LogLevel::Warn, __FILE__, __LINE__, msg);
            continue;
        }
        sendConnectAck(peerId, assigned, PeerRole::Pilot);
        // Keep the roster + match participant state consistent with the new team.
        uint16_t myFaction = 0;
        if (const EntityState* s = m_entityManager.get(assigned))
            myFaction = s->factionIndex;
        if (auto rit = m_wb.m_roster.find(peerId); rit != m_wb.m_roster.end()) {
            WorldBroadcaster::RosterRec rec = rit->second;
            rec.factionIndex = myFaction;
            m_wb.upsertRoster(peerId, rec);
        }
        m_wb.recordParticipant(peerId, myFaction, /*isBot=*/false, /*joined=*/true);
    }
}

void PeerAdmission::sendConnectRefusal(uint32_t peerId, ConnectRefusalCode code, const char* reason) {
    MsgConnectRefusal msg{};
    msg.code = static_cast<uint8_t>(code);
    std::snprintf(msg.reason, sizeof(msg.reason), "%s", reason);
    m_net.send(peerId, &msg, sizeof(msg), /*reliable=*/true);
}

void PeerAdmission::rejectConnection(uint32_t peerId, const std::string& ip, ConnectRefusalCode code) {
    const RejectInfo info = rejectInfoFor(code);
    char msg[160];
    std::snprintf(msg, sizeof(msg), "peer %u from %s rejected (%s) -- disconnecting", peerId, ip.c_str(),
                  info.logPhrase);
    m_logger.log(info.level, __FILE__, __LINE__, msg);
    sendConnectRefusal(peerId, code, info.reason);
    m_net.disconnectPeer(peerId);
}

} // namespace fl
