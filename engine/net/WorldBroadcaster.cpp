// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/WorldBroadcaster.h"

#include "render/ArtChannel.h" // the articulation channel vocabulary (#840); stdlib-only, no link dep
#include "render/RenderSnapshot.h"
#include "render/SurfaceType.h" // groundFrictionFor (#487)

#include "ILogger.h"
#include "INetwork.h"
// The wingman grammar (#610). Header-only and stdlib-only, so this adds NO link dependency —
// engine-net still does not link engine-ai (cmake/layering.cmake), and must not: building a
// controller is engine-ai's job and reaches this file only through the FlightOrderHandler hook.
// Including it here rather than hardcoding ordinals keeps one source of truth for the vocabulary.
#include "Utf8Decode.h" // chat text sanitization (#646)
#include "ai/WingmanCommand.h"
#include "atc/AtcService.h" // the deterministic ATC FSM ticked at 1 Hz (#702)
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "entity/IEntityController.h"
#include "flight/BuiltinFlightModel.h"
#include "flight/CentralGravityField.h"
#include "flight/FlightIntegrator.h"
#include "flight/ForceModelSelect.h" // applyForceModelFor — the shared role → force-model seam (#349)
#include "flight/StallBuffet.h"
#include "job/JobSystem.h"
#include "net/AckWindow.h"
#include "net/BitStream.h"
#include "net/GameProtocol.h"
#include "net/NetworkUtils.h"
#include "net/ReplayStateHash.h" // the per-tick world fingerprint the replay tap stamps (#644)
#include "net/SeatInput.h"       // seat-scoped input routing (#972)
#include "net/SnapshotCodec.h"
#include "net/SnapshotCompression.h"
#include "net/WireCodec.h"
#include "sensor/TrackPicture.h"
#include "weapon/Turret.h" // crew turret slew servo + world-bore (#970/#969)
#include "weather/Turbulence.h"
#include "weather/WeatherController.h"
#include "weather/WindProfile.h" // altitude wind interp (#489)
#include "world/FactionDef.h"
#include "world/FactionRegistry.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp> // glm::radians for turret limit conversion (#969)

#include <algorithm>
#include <bit> // std::popcount for the articulation TLV (#843)
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

static_assert(std::atomic<double>::is_always_lock_free,
              "WorldBroadcaster requires lock-free double atomics for entity XZ cache");

using namespace fl;

// ---------------------------------------------------------------------------
// Control sources
// ---------------------------------------------------------------------------

namespace {
// Drives an entity from the latest MsgClientInput stored for its connected peer. Holds a pointer to
// the peer's stable PeerInputState slot in WorldBroadcaster::m_peerInputs (unordered_map element
// pointers stay valid across rehash); the slot outlives the controller (torn down first on disconnect).
class PeerController final : public fl::IEntityController {
  public:
    explicit PeerController(const fl::PeerInputState* input) : m_input(input) {}

    fl::ControlInput sample(const fl::EntityState& /*state*/, uint64_t /*tick*/, double /*dt*/,
                            const fl::AiTickContext& /*ctx*/ = {}) override {
        fl::ControlInput ctrl{};
        ctrl.throttle = m_input->throttle;
        ctrl.elevator = m_input->elevator;
        ctrl.aileron = m_input->aileron;
        ctrl.rudder = m_input->rudder;
        ctrl.afterburner = (m_input->buttons & 0x02u) != 0; // bit 1 per MsgClientInput::buttons
        // Fire intent (#625): the bit that used to die right here. Level semantics both — the
        // weapons pass (FireControl) owns edge detection and rate limiting per entity.
        ctrl.trigger = (m_input->buttons & 0x01u) != 0;
        ctrl.release = (m_input->buttons & 0x04u) != 0;
        ctrl.station = m_input->selectedStation;
        // Electronic warfare intent (#529): bit 3 = dispense chaff/flare (edge-detected in the weapons
        // pass), bit 4 = ECM jammer on (level).
        ctrl.dispenseCm = (m_input->buttons & 0x08u) != 0;
        ctrl.ecm = (m_input->buttons & 0x10u) != 0;
        // Articulation commands (#843): a human pilot can finally raise the gear. Absolute state, so
        // a dropped packet costs one tick of lag rather than a permanently wrong configuration.
        ctrl.flaps = static_cast<float>(m_input->flaps) / 255.f;
        ctrl.speedbrake = static_cast<float>(m_input->speedbrake) / 255.f;
        ctrl.gear_down = (m_input->artButtons & fl::kArtButtonGearDown) != 0;
        ctrl.hook_down = (m_input->artButtons & fl::kArtButtonHookDown) != 0;
        ctrl.canopy_open = (m_input->artButtons & fl::kArtButtonCanopyOpen) != 0;
        // Wheel brakes (#700): the integrator only acts on this while the aircraft is in ground
        // contact, so a held brake in flight is harmless. Level on the wire; no edge detection needed.
        ctrl.wheelBrake = (m_input->buttons & fl::kInputButtonWheelBrake) != 0 ? 1.f : 0.f;
        return ctrl;
    }

  private:
    const fl::PeerInputState* m_input;
};

// A frozen-input autopilot (#974): returns a fixed ControlInput every tick. When a peer-spawned
// aircraft's owning pilot leaves but a human gunner remains, the aircraft must keep flying without a
// dangling PeerController pointing at the departed peer's (freed) input slot. Swapping to this holds
// the pilot's LAST attitude/throttle so the gunner keeps a stable platform until the last human leaves.
// The fire fields are cleared so an orphaned airframe never keeps firing the pilot's guns.
class HoldController final : public fl::IEntityController {
  public:
    explicit HoldController(const fl::ControlInput& held) : m_held(held) {
        m_held.trigger = false;
        m_held.release = false;
        m_held.dispenseCm = false;
        m_held.ecm = false;
    }
    fl::ControlInput sample(const fl::EntityState& /*state*/, uint64_t /*tick*/, double /*dt*/,
                            const fl::AiTickContext& /*ctx*/ = {}) override {
        return m_held;
    }

  private:
    fl::ControlInput m_held;
};
} // namespace

// ---------------------------------------------------------------------------
// IP address helpers
// ---------------------------------------------------------------------------

// Extract the normalized IP from an "ip:port" or "[ip]:port" string returned by getPeerAddress().
static std::string extractIp(const char* addrPort) {
    if (!addrPort)
        return {};
    std::string_view av(addrPort);
    std::string_view ipv;
    if (!av.empty() && av.front() == '[') {
        av.remove_prefix(1);
        auto end = av.find(']');
        ipv = (end != std::string_view::npos) ? av.substr(0, end) : av;
    } else {
        auto colon = av.rfind(':');
        ipv = (colon != std::string_view::npos) ? av.substr(0, colon) : av;
    }
    return fl::normalizeIp(ipv);
}

// ---------------------------------------------------------------------------
// Quaternion helpers — pure float array math, no GLM dependency.
// Convention: q = [x, y, z, w] matching EntityTransform::quat.
// ---------------------------------------------------------------------------

// Rotate vector v by quaternion q using the Rodrigues formula.
static void quatRotate(const float q[4], const float v[3], float out[3]) {
    float tx = q[1] * v[2] - q[2] * v[1];
    float ty = q[2] * v[0] - q[0] * v[2];
    float tz = q[0] * v[1] - q[1] * v[0];
    out[0] = v[0] + 2.f * q[3] * tx + 2.f * (q[1] * tz - q[2] * ty);
    out[1] = v[1] + 2.f * q[3] * ty + 2.f * (q[2] * tx - q[0] * tz);
    out[2] = v[2] + 2.f * q[3] * tz + 2.f * (q[0] * ty - q[1] * tx);
}

// Returns true if `incoming` is strictly newer than `last` under uint32 wrap-around.
// Uses the half-window comparison: a difference in [1, 2^31-1] (mod 2^32) is "newer".
static bool isNewerSeq(uint32_t incoming, uint32_t last) noexcept {
    return incoming != last && ((incoming - last) & 0x80000000u) == 0u;
}

// ---------------------------------------------------------------------------
// Connection-rejection reason table — one place mapping each ConnectRefusalCode
// to the client-facing reason text and the server-side log phrase/level.
// ---------------------------------------------------------------------------
namespace {
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
    case C::Generic:
        break;
    }
    return {"Access denied.", "access denied", LogLevel::Info};
}
} // namespace

namespace fl {

// Over-G damage scale (#816): hit points per g beyond the structural limit, per damage event
// (one event per 0.5 s of sustained overstress). Tuned so that yanking an 8 g airframe to 12 g and
// holding it there costs real airframe life within a few seconds, without turning a brief excursion
// into an instant kill -- the pilot should be able to break the aeroplane, but should have to work at it.
constexpr float kOverGDamagePerG = 6.0f;

// HP fraction at or below which an AI/scripted pilot auto-ejects (#672). A future skill model can scale
// this per pilot (a nervous rookie punches out earlier than an ace); 15% is a reasonable baseline.
constexpr float kAiEjectHpFraction = 0.15f;

// Default airspeed for an airborne spawn that does not ask for a specific one (#883). A gentle cruise
// (~230 kts) that is comfortably above the stall for the fighters this targets and flyable for the
// no-stall builtin UFO, so a freshly spawned aircraft — pilot, AI, or mission object — is in stable
// controlled flight at t=0 rather than dropped in at zero airspeed. A mission tunes it per object with
// `speed:`; a ground start (#885) passes 0 instead.
constexpr float kDefaultSpawnAirspeedMps = 120.0f;

// GameProtocol.h must stay stdlib-only (engine-protocol is zero-dep, enforced by fl_assert_zero_dep),
// so it MIRRORS these two values rather than including the headers that define them. This is the one
// TU that sees both sides, so this is where the mirrors are checked. If either fires, the wire and
// the engine disagree about the vocabulary, which is exactly the drift the #678 record and the #624
// validator were both cleaning up.
static_assert(kWingmanCommandCount == static_cast<uint8_t>(fl::ai::WingmanCommand::Count),
              "GameProtocol kWingmanCommandCount is out of sync with fl::ai::WingmanCommand");
static_assert(kNoFlightId == fl::kNoFormation, "GameProtocol kNoFlightId is out of sync with fl::kNoFormation");

WorldBroadcaster::WorldBroadcaster(EntityManager& entityManager, EntityTypeRegistry& registry, INetwork& net,
                                   ILogger& logger, WeatherController* weather)
    : m_entityManager(entityManager), m_registry(registry), m_net(net), m_logger(logger), m_weather(weather),
      m_sensorSystem(entityManager, registry), m_gravity(&fl::CentralGravityField::earthInstance()),
      m_planetRadiusKm(6371.f) {
    // The compiled-in radio-net stack (Epic J). Seeded here so voice works with zero configuration;
    // fl-server's [[voice.nets]] replaces it wholesale via setRadioNets().
    for (auto& def : builtinRadioNets())
        m_radioNets.add(def);

    // Seeker target signatures come from the same type registry the sensor system reads (#627).
    m_projectileSystem.setTypeRegistry(&registry);
    // SARH / pre-pitbull ARH shots are supported by the SHOOTER's contact table (#628): the missile
    // inherits the shooter's honest belief, never ground truth.
    m_projectileSystem.setSupportQuery(
        [this](uint32_t shooterIdx) -> const sensor::ContactTable* { return m_sensorSystem.contactsFor(shooterIdx); });

    // IFF (#527): resolve the observer→target coalition relationship through the faction registry when
    // one is loaded, else the affiliation-rule fallback — the SAME semantics as fl::hostile() (a
    // mission's relationship matrix wins; before a mission loads, distinct non-zero factions are
    // hostile). Read `m_factionRegistry` dynamically so a registry set after construction is picked up.
    m_sensorSystem.setIffResolver([this](uint16_t obs, uint16_t tgt) -> FactionRelation {
        return m_factionRegistry ? m_factionRegistry->relationship(obs, tgt) : sensor::affiliationRelation(obs, tgt);
    });

    // Countermeasure seduction (#529): a missile seeker asks the CountermeasureSystem whether a
    // chaff/flare decoy has broken its lock this check. All the physics (which decoy, proximity, the
    // deterministic die vs the head's susceptibility) lives behind the seam.
    m_projectileSystem.setCountermeasureCheck([this](uint32_t missileIdx, const glm::dvec3& targetPos,
                                                     sensor::SensorType channel,
                                                     const CountermeasureSusceptibility& susc, uint64_t tick) -> bool {
        return m_countermeasures.seduces(missileIdx, targetPos, channel, susc, tick);
    });

    // Missile-launch RWR warnings (#960): a live RADAR-guided missile guiding on a target is a
    // legitimate emission its RWR hears (the ARH seeker gone active, or the SARH illuminator riding
    // it) — so it earns a Launch-level threat, the honest counterpart to the deferred wallhack cue.
    // IR-seeker missiles are passive and deliberately never light the RWR. Only a missile actively
    // HOLDING the target (Detected/Locked) qualifies: a coasting/lost seeker drops the strobe, which
    // rewards defeating the lock. The seeker's targetId names the victim honestly (never invented).
    m_sensorSystem.setMissileThreatProvider([this](const sensor::SensorSystem::ThreatSink& sink) {
        for (const Projectile& p : m_projectileSystem.projectiles()) {
            if (!p.seekerDef || p.seekerDef->type != sensor::SensorType::Radar)
                continue; // only radar seekers paint a warning receiver
            if (!p.seeker.targetId.valid())
                continue;
            const sensor::ContactState st = p.seeker.track.state;
            if (st != sensor::ContactState::Detected && st != sensor::ContactState::Locked)
                continue; // must be actively guiding on the target right now
            const EntityState* tgt = m_entityManager.getByIndex(p.seeker.targetId.index);
            if (!tgt || tgt->dead || tgt->id.generation != p.seeker.targetId.generation)
                continue;

            sensor::ThreatWarning w;
            w.emitterId = p.entityId; // the missile itself is the emitter — a closing bearing
            const EntityState* mis = m_entityManager.getByIndex(p.entityId.index);
            const EntityState* shooter = m_entityManager.getByIndex(p.shooter.index);
            w.emitterTypeIndex = mis ? mis->typeIndex : 0;
            // Faction from the shooter's side so IFF classifies the launch as a foe, not the missile's
            // own (often neutral) faction.
            w.emitterFactionIndex = shooter ? shooter->factionIndex : (mis ? mis->factionIndex : 0);
            w.channel = sensor::SensorType::Radar;
            w.level = sensor::ThreatLevel::Launch;
            w.emitterPos[0] = p.pos[0];
            w.emitterPos[1] = p.pos[1];
            w.emitterPos[2] = p.pos[2];
            sink(p.seeker.targetId.index, w);
        }
    });
}

WorldBroadcaster::~WorldBroadcaster() = default;

// ---------------------------------------------------------------------------
// Peer management (sim-thread only)
// ---------------------------------------------------------------------------

void WorldBroadcaster::kickPeer(uint32_t peerId) {
    m_net.disconnectPeer(peerId);
}

void WorldBroadcaster::broadcastMusicState(uint8_t state) {
    MsgMusicState msg;
    msg.state = state;
    for (const auto& [peerId, pin] : m_peerInputs) {
        (void)pin;
        m_net.send(peerId, &msg, sizeof(msg), /*reliable=*/true);
    }
}

void WorldBroadcaster::recordParticipant(uint32_t participantId, uint16_t faction, bool isBot, bool joined) {
    if (m_matchParticipantSink)
        m_matchParticipantSink(participantId, faction, isBot, joined);

    MatchEvent me;
    me.tick = m_currentTick;
    me.type = joined ? MatchEventType::Join : MatchEventType::Leave;
    me.actor = participantId;
    me.factionIndex = faction;
    // isBot rides in `value` rather than a dedicated field: it is the only bool any participant
    // record needs, and a bool field on every record type to serve two of them is not worth it.
    me.value = isBot ? 1 : 0;
    // The callsign rides in `text` (#643): a replay's roster section is written when recording STARTS,
    // so a participant who joins later would otherwise be "participant 7" forever. The roster upsert
    // always precedes this call, so the name is already there to read. A join with no name is not
    // much use to the #600 event stream or the #601 audit mirror either.
    if (joined) {
        if (const auto it = m_roster.find(participantId); it != m_roster.end())
            me.text = it->second.callsign;
    }
    m_matchEventLog.append(std::move(me));
}

void WorldBroadcaster::broadcastAlertLevelChange(uint16_t factionIndex, uint8_t level) {
    MsgAlertLevelChange msg;
    msg.factionIndex = factionIndex;
    msg.level = level;
    for (const auto& [peerId, pin] : m_peerInputs) {
        (void)pin;
        m_net.send(peerId, &msg, sizeof(msg), /*reliable=*/true);
    }
}

void WorldBroadcaster::broadcastHaptic(uint8_t kind, float a, float b, uint16_t durationMs) {
    MsgHaptic msg;
    msg.kind = kind;
    msg.a = a;
    msg.b = b;
    msg.durationMs = durationMs;
    for (const auto& [peerId, pin] : m_peerInputs) {
        (void)pin;
        m_net.send(peerId, &msg, sizeof(msg), /*reliable=*/true);
    }
}

void WorldBroadcaster::broadcastMissionOutcome(uint8_t outcome, float elapsedSeconds, uint16_t triggersFired) {
    MsgMissionOutcome msg;
    msg.outcome = outcome;
    msg.elapsedSeconds = elapsedSeconds;
    msg.triggersFired = triggersFired;
    for (const auto& [peerId, pin] : m_peerInputs) {
        (void)pin;
        m_net.send(peerId, &msg, sizeof(msg), /*reliable=*/true);
    }
}

void WorldBroadcaster::updateMissionRoster(const std::string& missionObjectId, EntityId entity) {
    // Update the stored binding (an unknown object id is ignored — only mission-declared objects are in
    // the roster), then broadcast the single record so every connected recorder picks up the late bind.
    bool found = false;
    for (auto& [id, eid] : m_missionRoster) {
        if (id == missionObjectId) {
            eid = entity;
            found = true;
            break;
        }
    }
    if (!found || entity.generation == 0)
        return; // unknown object, or the slot was freed (invalid entity) — nothing to advertise
    MsgMissionRoster rmsg{};
    rmsg.entityIdx = entity.index;
    rmsg.entityGen = static_cast<uint16_t>(entity.generation);
    std::snprintf(rmsg.objectId, sizeof(rmsg.objectId), "%s", missionObjectId.c_str());
    for (const auto& [peerId, pin] : m_peerInputs) {
        (void)pin;
        m_net.send(peerId, &rmsg, sizeof(rmsg), /*reliable=*/true);
    }
}

EjectionOutcome WorldBroadcaster::ejectPilot(EntityId eid) {
    EntityState* st = m_entityManager.get(eid);
    if (!st || st->dead)
        return EjectionOutcome::KIA; // nothing (or nobody) left to eject

    // Seat envelope from the live flight state: AGL, speed, and the radial sink rate.
    const double pos[3] = {st->transform.pos[0], st->transform.pos[1], st->transform.pos[2]};
    const glm::dvec3 posv(pos[0], pos[1], pos[2]);
    const float terrainElev = m_groundQuery ? m_groundQuery(posv) : m_groundElevation.load(std::memory_order_relaxed);
    const double geoAlt = m_gravity ? m_gravity->geodeticAltitude(pos) : pos[1];
    const std::array<float, 3> up = m_gravity ? m_gravity->geodeticUp(pos) : std::array<float, 3>{0.f, 1.f, 0.f};

    EjectionEnvelope env;
    env.altitudeAglM = static_cast<float>(geoAlt - static_cast<double>(terrainElev));
    const float vx = st->transform.vel[0];
    const float vy = st->transform.vel[1];
    const float vz = st->transform.vel[2];
    env.speedMs = std::sqrt(vx * vx + vy * vy + vz * vz);
    env.sinkRateMs = -(vx * up[0] + vy * up[1] + vz * up[2]); // positive = descending
    const bool survived = ejectionSurvivable(env);

    // A replicating parachute at the aircraft position (if a type is configured + registered); it is a
    // plain entity, so it rides the normal snapshot path to every client. Zero velocity — a drift model
    // is a follow-on; the point is a visible, networked chute where the pilot got out.
    if (!m_parachuteType.empty()) {
        EntityTransform pt = st->transform;
        pt.vel[0] = pt.vel[1] = pt.vel[2] = 0.f;
        const EntityId pid = m_entityManager.spawn(m_parachuteType.c_str(), pt);
        if (EntityState* pst = m_entityManager.get(pid))
            pst->factionIndex = st->factionIndex;
    }

    const uint16_t faction = st->factionIndex;
    m_entityManager.kill(eid); // the airframe is lost regardless of whether the pilot made it

    // Resolve the landing territory: a campaign wires m_territoryQuery to its frontline (rescued over
    // friendly ground, captured over hostile); a plain server leaves it unset, so a survivor is MIA.
    const TerritoryControl territory = m_territoryQuery ? m_territoryQuery(posv, faction) : TerritoryControl::Neutral;
    const EjectionOutcome outcome = pilotOutcome(survived, territory);
    char m[144];
    std::snprintf(m, sizeof(m), "ejection: entity %u -> pilot %s (%.0f m AGL, %.0f m/s)", eid.index,
                  ejectionOutcomeName(outcome), static_cast<double>(env.altitudeAglM),
                  static_cast<double>(env.speedMs));
    m_logger.log(LogLevel::Info, __FILE__, __LINE__, m);
    return outcome;
}

void WorldBroadcaster::banAddress(std::string ip) {
    ip = fl::normalizeIp(ip);
    m_bannedAddresses.insert(ip);
    for (const auto& [peerId, eid] : m_peerEntities) {
        if (extractIp(m_net.getPeerAddress(peerId)) == ip)
            m_net.disconnectPeer(peerId);
    }
}

void WorldBroadcaster::unbanAddress(const std::string& ip) {
    m_bannedAddresses.erase(fl::normalizeIp(ip));
}

bool WorldBroadcaster::unlockAdminAuth(const std::string& ip) {
    std::string norm = fl::normalizeIp(ip);
    bool wasLocked = m_adminAuthTracker.isLockedOut(norm);
    m_adminAuthTracker.clearLockout(norm);
    return wasLocked;
}

AuthLockoutSummary WorldBroadcaster::getAuthLockoutSummary() const {
    AuthLockoutSummary s;
    s.threshold = m_adminAuthTracker.maxFailures();
    s.entries = m_adminAuthTracker.failureSummary();
    for (const auto& e : s.entries)
        if (e.lockedOut)
            ++s.activeCount;
    return s;
}

void WorldBroadcaster::setBannedAddresses(std::unordered_set<std::string> addrs) {
    m_bannedAddresses = std::move(addrs);
}

void WorldBroadcaster::setAllowedAddresses(std::unordered_set<std::string> addrs) {
    m_allowedAddresses = std::move(addrs);
}

std::unordered_set<std::string> WorldBroadcaster::getBannedAddresses() const {
    return m_bannedAddresses;
}

void WorldBroadcaster::setRateLimitParams(int maxConnects, int windowSeconds, int floodMultiplier) {
    m_connectRateLimit = maxConnects;
    m_connectRateWindowS = windowSeconds;
    m_floodMultiplier = floodMultiplier;
}

void WorldBroadcaster::setMaxConnectionsPerIp(int max) noexcept {
    m_maxConnectionsPerIp = max;
}

void WorldBroadcaster::setSpawnPoints(std::vector<std::array<double, 3>> points) noexcept {
    m_spawnPoints = std::move(points);
}

void WorldBroadcaster::setClock(const IClock& clock) {
    m_clock = &clock;
    m_adminAuthTracker.setClock(clock);
    m_tickProfiler.setClock(clock);
}

void WorldBroadcaster::setMotd(std::string motd) {
    m_motd = std::move(motd);
}

void WorldBroadcaster::setMotdDisplaySeconds(uint16_t seconds) noexcept {
    m_motdDisplaySeconds = seconds;
}

void WorldBroadcaster::setPlayerFaction(uint16_t faction) noexcept {
    m_playerFaction = faction;
}

void WorldBroadcaster::setFlightSpawner(FlightSpawner fn) {
    m_flightSpawner = std::move(fn);
}

void WorldBroadcaster::setFlightOrderHandler(FlightOrderHandler fn) {
    m_flightOrderHandler = std::move(fn);
}

void WorldBroadcaster::setTargetDesignator(TargetDesignator fn) {
    m_targetDesignator = std::move(fn);
}

void WorldBroadcaster::setFlightCommandRateLimit(int perSecond) noexcept {
    m_flightCmdRateLimit = perSecond > 0 ? perSecond : 1;
}

void WorldBroadcaster::setFlightModelResolver(FlightModelResolver fn) {
    m_flightModelResolver = std::move(fn);
}

void WorldBroadcaster::setPayloadResolver(PayloadResolver fn) {
    m_payloadResolver = std::move(fn);
}

void WorldBroadcaster::setSeatControllerFactory(SeatControllerFactory fn) {
    m_seatControllerFactory = std::move(fn);
}

void WorldBroadcaster::setSensorDefResolver(sensor::SensorSystem::SensorDefResolver fn) {
    // Aircraft radars and missile seeker heads resolve through the SAME function (#627): one
    // vocabulary, one resolution path, one cache.
    m_projectileSystem.setSensorResolver(fn);
    m_sensorSystem.setResolver(std::move(fn));
}

void WorldBroadcaster::setSensorCheckHz(float hz) noexcept {
    m_sensorCheckHz.store(std::clamp(hz, 1.f, 60.f), std::memory_order_relaxed);
}

void WorldBroadcaster::setEmitting(uint32_t entityIdx, bool emitting) {
    m_sensorSystem.setEmitting(entityIdx, emitting);
}

void WorldBroadcaster::setRadarMode(uint32_t entityIdx, sensor::RadarMode mode) {
    m_sensorSystem.setRadarMode(entityIdx, mode);
}

void WorldBroadcaster::setDesignatedTarget(uint32_t entityIdx, EntityId target) {
    m_sensorSystem.setDesignatedTarget(entityIdx, target);
}

const sensor::ContactTable* WorldBroadcaster::contactsFor(uint32_t entityIdx) const {
    return m_sensorSystem.contactsFor(entityIdx);
}

const sensor::ThreatWarningSet* WorldBroadcaster::threatsFor(uint32_t entityIdx) const {
    return m_sensorSystem.threatsFor(entityIdx);
}

void WorldBroadcaster::setAiScaling(const AiScaling& scaling) noexcept {
    m_aiScaling = scaling;
}

void WorldBroadcaster::setOperatorPassword(std::string password) {
    m_operatorPassword = std::move(password);
}

void WorldBroadcaster::setAdminDispatch(std::function<std::string(std::string_view, const CommandIssuer&)> fn) {
    m_adminDispatch = std::move(fn);
}

void WorldBroadcaster::setAdminShell(std::function<int()> markFn,
                                     std::function<std::vector<std::string>(int)> drainFn) {
    m_adminShellMark = std::move(markFn);
    m_adminShellDrain = std::move(drainFn);
}

void WorldBroadcaster::setAdminAuthParams(int maxFailures, int lockoutSeconds) {
    m_adminAuthTracker = AuthTracker(maxFailures, lockoutSeconds);
    m_adminAuthTracker.setClock(*m_clock);
}

void WorldBroadcaster::setGravityField(const IGravityField& field, float planetRadiusKm) noexcept {
    m_gravity = &field;
    m_planetRadiusKm = planetRadiusKm;
    // Projectiles fall in the same field as everything else (#625). Re-armed here so the
    // setWeaponRegistry/setGravityField call order does not matter.
    m_projectileSystem.configure(m_weaponRegistry, m_gravity);
}

void WorldBroadcaster::setGroundElevationQuery(std::function<float(glm::dvec3)> fn) {
    m_groundQuery = std::move(fn);
}

void WorldBroadcaster::setGroundSurfaceQuery(std::function<SurfaceType(glm::dvec3)> fn) {
    m_groundSurfaceQuery = std::move(fn);
}

void WorldBroadcaster::applyConfig(const WorldBroadcasterConfig& cfg) {
    setRateLimitParams(cfg.connectRateLimit, cfg.connectRateWindowS, cfg.floodMultiplier);
    setMaxConnectionsPerIp(cfg.maxConnectionsPerIp);
    setAdminAuthParams(cfg.adminAuthMaxFailures, cfg.adminAuthLockoutSeconds);
    setMotd(cfg.motd);
    setMotdDisplaySeconds(cfg.motdDisplaySeconds);
    setOperatorPassword(cfg.operatorPassword);
    m_playerEntityType = cfg.playerEntityType;     // pilot spawn default (#834); applyConfig runs pre-start
    m_allowObservers = cfg.allowObservers;         // #857; applyConfig runs pre-start
    m_requiredPacks = cfg.requiredPacks;           // #872; applyConfig runs pre-start
    m_requiredPackPolicy = cfg.requiredPackPolicy; // #872 warn / refuse / allow-placeholder
    setIdleTimeout(cfg.idleTimeoutS);
    setDrawDistance(cfg.drawDistanceKm);
    setSpatialCellSize(cfg.spatialCellSizeM); // after setDrawDistance: auto mode reads m_drawDistanceM
    setSnapshotBudget(cfg.snapshotBudgetBytes);
    setSnapshotCompression(cfg.compressSnapshots);
    setJitterBufferDepth(cfg.jitterBufferMaxDepth);
    setJitterAdaptWindow(cfg.jitterAdaptWindow);
    setJitterHysteresis(cfg.jitterHysteresis);
    setJitterMultiplier(cfg.jitterMultiplier);
    setCongestionParams(cfg.congestion);
    setGovernorParams(cfg.governor);
    setDamageRules(cfg.gameplay);
}

void WorldBroadcaster::setIdleTimeout(int timeoutSeconds) noexcept {
    m_idleTimeoutTicks = timeoutSeconds > 0 ? static_cast<uint64_t>(timeoutSeconds) * 60u : 0u;
}

void WorldBroadcaster::setInputTraceDir(std::string dir) {
    // Any change closes the currently open per-peer trace files so records never straddle two
    // directories; disabling clears the dir, enabling/switching creates the target and lets each
    // peer's writer reopen lazily on its next accepted input.
    m_peerTraceWriters.clear();
    m_inputTraceDir = std::move(dir);
    if (m_inputTraceDir.empty())
        return;
    std::error_code ec;
    std::filesystem::create_directories(m_inputTraceDir, ec);
    if (ec) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "input trace dir '%s' could not be created (%s) — tracing disabled",
                      m_inputTraceDir.c_str(), ec.message().c_str());
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__, msg);
        m_inputTraceDir.clear();
    }
}

void WorldBroadcaster::setDrawDistance(float km) noexcept {
    m_drawDistanceM = static_cast<double>(km) * 1000.0;
}

void WorldBroadcaster::setSpatialCellSize(double cellSizeM) {
    // Auto (<= 0): pick a cell so a full draw-distance query spans a bounded number of cells rather
    // than degenerating toward O(N) at high density. drawDistance/32 with a 500 m floor keeps a
    // 200 km radius near ~6 km cells (~13x13 cells/query) while never producing a zero/UB cell size.
    double resolved = cellSizeM;
    if (resolved <= 0.0) {
        const double dd = m_drawDistanceM > 0.0 ? m_drawDistanceM : 200'000.0;
        resolved = std::clamp(dd / 32.0, 500.0, 10'000.0);
    }
    m_spatialIndex.setCellSize(resolved);
}

void WorldBroadcaster::setSnapshotBudget(uint32_t bytes) noexcept {
    m_snapshotBudgetBytes.store(bytes, std::memory_order_relaxed);
}

void WorldBroadcaster::setSnapshotCompression(bool enabled) noexcept {
    m_compressSnapshots.store(enabled, std::memory_order_relaxed);
}

void WorldBroadcaster::setJitterBufferDepth(uint32_t maxDepth) noexcept {
    m_jitterMaxDepth.store(maxDepth == 0u ? 1u : maxDepth, std::memory_order_relaxed);
}

void WorldBroadcaster::setJitterAdaptWindow(uint32_t ticks) noexcept {
    m_jitterAdaptWindow = (ticks == 0u ? 1u : ticks);
}

void WorldBroadcaster::setJitterHysteresis(uint32_t ticks) noexcept {
    m_jitterHysteresis = ticks;
}

void WorldBroadcaster::setJitterMultiplier(float k) noexcept {
    m_jitterMultiplier = (k < 0.f ? 0.f : k);
}

void WorldBroadcaster::setCongestionParams(const CongestionParams& params) noexcept {
    m_congestionParams = params;
}

void WorldBroadcaster::setGovernorParams(const TickGovernorParams& params) noexcept {
    m_governorParams = params;
}

void WorldBroadcaster::forEachPeer(std::function<void(const PeerInfo&)> fn) const {
    for (const auto& [peerId, eid] : m_peerEntities) {
        PeerInfo pi;
        pi.peerId = peerId;
        pi.eid = eid;
        const char* raw = m_net.getPeerAddress(peerId);
        pi.addr = raw ? raw : "";
        if (auto it = m_peerInputs.find(peerId); it != m_peerInputs.end()) {
            const PeerInputState& ps = it->second;
            pi.delayTicks = ps.estimatedDelayTicks;
            pi.queueDepth = ps.jitterBuffer.size();
            pi.bufferMaxDepth = ps.jitterBuffer.maxDepth();
            pi.ewmaDelayTicks = ps.ewmaDelayTicks;
            pi.ewmaJitterTicks = ps.ewmaJitterTicks;
            // #576: the EFFECTIVE interval, composed from both levers — not the congestion
            // controller's alone. Reporting the congestion interval as "the send rate" understated
            // the decimation whenever the governor was the wider of the two, which is exactly the
            // case an operator is looking at when they run `peers` on a loaded server.
            const uint32_t interval = std::max(ps.effectiveIntervalTicks, uint32_t{1u});
            pi.sendRateHz = 60.f / static_cast<float>(interval);
            pi.effectiveIntervalTicks = interval;
            pi.governorBinding = ps.governorBinding;
            pi.congestionBinding = ps.congestionBinding;
            pi.effectiveBudget = ps.congestion.effectiveBudget(m_snapshotBudgetBytes.load(std::memory_order_relaxed));
            pi.packetLoss = m_net.getPeerLinkStats(peerId).packetLoss; // live ENet mean loss fraction
            pi.caps = ps.authority.caps;                               // granted authority (#946)
        }
        fn(pi);
    }
}

void WorldBroadcaster::onTick(double simDt, uint64_t tickIndex) {
    m_currentTick = tickIndex;

    // Per-phase tick-budget instrumentation. beginTick() resets the per-tick accumulators and
    // records the wall start; each phase boundary records its elapsed wall-time; endTick() rolls
    // the samples into the rolling window. See TickProfiler.h.
    m_tickProfiler.beginTick();
    const auto tMaintenanceStart = m_clock->now();

    // Coarse prune of stale rate-limit records every 600 ticks (~10 s at 60 Hz).
    if (++m_ratePruneTick % 600 == 0) {
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
        m_adminAuthTracker.pruneExpired();
        // Reconnection grace (#524): purge expired held identities.
        for (auto it = m_disconnectGrace.begin(); it != m_disconnectGrace.end();) {
            if (m_currentTick > it->second.expiresTick)
                it = m_disconnectGrace.erase(it);
            else
                ++it;
        }
    }

    // Idle timeout: disconnect peers that have sent no activity for m_idleTimeoutTicks ticks.
    if (m_idleTimeoutTicks > 0) {
        std::vector<uint32_t> toKick;
        for (const auto& [peerId, ps] : m_peerInputs) {
            if (tickIndex > ps.lastActivityTick && tickIndex - ps.lastActivityTick >= m_idleTimeoutTicks)
                toKick.push_back(peerId);
        }
        for (uint32_t pid : toKick) {
            char msg[80];
            std::snprintf(msg, sizeof(msg), "peer %u idle timeout — disconnecting", pid);
            m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);
            m_net.disconnectPeer(pid);
        }
    }

    // Fire deferred admin drains: deliver CommandShell output written by enqueueSimCallback
    // lambdas as follow-on MsgAdminResponseChunk packets. Uses a wall-clock deadline (20 ms,
    // matching the RCON drain) rather than a tick index, so drain timing is immune to
    // GameLoop tick-batch catch-up (up to kMaxTicksPerIteration ticks per iteration).
    if (!m_pendingAdminDrains.empty() && m_adminShellDrain) {
        auto it = m_pendingAdminDrains.begin();
        while (it != m_pendingAdminDrains.end()) {
            if (m_clock->now() < it->drainDeadline) {
                ++it;
                continue;
            }
            if (m_peerEntities.count(it->peerId)) {
                auto lines = m_adminShellDrain(it->shellMark);
                if (!lines.empty()) {
                    std::string payload;
                    for (const auto& ln : lines) {
                        if (!ln.empty()) {
                            payload += ln;
                            payload += '\n';
                        }
                    }
                    if (!payload.empty())
                        payload.pop_back(); // trim trailing newline
                    if (!payload.empty())
                        sendAdminResponse(m_net, it->peerId, it->reqId, payload);
                }
            }
            it = m_pendingAdminDrains.erase(it);
        }
    }

    // Rebuild spatial index from entity positions at tick start (previous-tick state).
    // Dead entities were reaped in the previous tick's m_entityManager.onTick(); forEach skips them.
    m_spatialIndex.clear();
    m_entityManager.forEach([this](const EntityState& s) { m_spatialIndex.insert(s.id.index, s.transform.pos); });

    // Rebuild the flight-deck records (#38) beside the spatial index — serial, then read-only for
    // the parallel integrate pass's ground-floor composition.
    rebuildDeckRecords();

    // Drain one buffered input per peer before stepping. When the buffer is empty the existing
    // control fields are retained (stale repeat) — the entity continues on its last known inputs
    // rather than coasting to zero. viewAxis is not buffered (camera only, not flight control).
    for (auto& [peerId, ps] : m_peerInputs) {
        BufferedInput bi;
        if (ps.jitterBuffer.pop(bi)) {
            ps.throttle = bi.throttle;
            ps.elevator = bi.elevator;
            ps.aileron = bi.aileron;
            ps.rudder = bi.rudder;
            ps.buttons = bi.buttons;
            ps.selectedStation = bi.selectedStation;
            ps.radarMode = bi.radarMode;
            ps.flaps = bi.flaps;           // #843
            ps.speedbrake = bi.speedbrake; // #843
            ps.artButtons = bi.artButtons; // #843
            // Apply the player's radar mode (#526) to their own aircraft's sensor observer. Absolute +
            // idempotent, so a repeated value costs nothing; 255 = keep whatever the server holds (an
            // unaware client / load bot leaves the spawned Search mode alone). Runs before the sensing
            // pass this tick, so a mode change takes effect immediately.
            if (sensor::isRadarModeOrdinal(ps.radarMode)) {
                if (auto eit = m_peerEntities.find(peerId); eit != m_peerEntities.end())
                    m_sensorSystem.setRadarMode(eit->second.index, static_cast<sensor::RadarMode>(ps.radarMode));
            }
            // Record the seqNum whose control fields now drive the sim (#427). A stale-repeat tick
            // (empty buffer) keeps the previous value: the same input is still what the snapshot
            // reflects, so the client should still treat newer seqNums as un-acked.
            ps.lastAppliedSeqNum = bi.seqNum;
            ps.hasAppliedSeq = true;
        } else {
            // Stale-repeat keeps the FLIGHT controls (prevents coasting under loss), but must not
            // keep re-asserting the store-release bit: FireControl edge-detects, so a held bit is
            // one shot — unless the buffer starves right after the release frame, when the repeat
            // would look like a fresh press next time real input resumes. Masking it off makes a
            // starved tick read as trigger-off, which is the safe reading of silence (#625).
            ps.buttons &= static_cast<uint8_t>(~0x04u);
        }
        // Ejection (#672): fire on the RISING edge of the eject bit so a held key is one ejection.
        // kill()/spawn() queue for end-of-tick, so mutating here while iterating m_peerInputs is safe.
        const bool ejectNow = (ps.buttons & kInputButtonEject) != 0;
        if (ejectNow && !ps.ejectHeld) {
            if (auto eit = m_peerEntities.find(peerId); eit != m_peerEntities.end())
                ejectPilot(eit->second);
        }
        ps.ejectHeld = ejectNow;

        // Respawn request (#648): rising edge marks the intent. Fired at dueTick by processRespawns
        // (queued if pressed early); a repeat/held bit is one request.
        const bool respawnNow = (ps.buttons & kInputButtonRespawn) != 0;
        if (respawnNow && !ps.respawnHeld) {
            if (auto rit = m_respawn.find(peerId); rit != m_respawn.end())
                rit->second.requested = true;
        }
        ps.respawnHeld = respawnNow;
    }

    // AI auto-eject (#672): a scripted/AI pilot punches out when its airframe is critically hit rather
    // than riding a doomed aircraft into the ground — the visible ejection the acceptance criteria want.
    // Players never auto-eject (they own the End key); only decimatable (AI/scripted) controlled entities
    // do. ejectPilot spawns a replicating chute + kills the airframe; the ejected guard fires it once.
    if (m_aiAutoEject)
        for (auto& [idx, ce] : m_controlledEntities) {
            (void)idx;
            if (!ce.decimatable || ce.ejected)
                continue;
            const EntityState* st = m_entityManager.get(ce.id);
            if (!st || st->dead || st->maxHp <= 0.f)
                continue;
            if (st->hp / st->maxHp <= kAiEjectHpFraction) {
                ejectPilot(ce.id);
                ce.ejected = true;
            }
        }

    // Adaptive jitter buffer resize: for each peer with a seeded EWMA, compute the target depth
    // from the delay EWMA and inter-arrival jitter EWMA, then resize if outside the hysteresis band.
    // Runs O(P) float comparisons per tick — negligible at max 32 peers × 60 Hz.
    {
        const uint32_t globalMax = m_jitterMaxDepth.load(std::memory_order_relaxed);
        const float k = m_jitterMultiplier;
        const uint32_t hysteresis = m_jitterHysteresis;
        for (auto& [peerId, ps] : m_peerInputs) {
            if (!ps.ewmaSeeded)
                continue;
            const float targetF =
                std::clamp(ps.ewmaDelayTicks + k * ps.ewmaJitterTicks, 1.0f, static_cast<float>(globalMax));
            const uint32_t target = static_cast<uint32_t>(std::ceil(targetF));
            const uint32_t current = ps.jitterBuffer.maxDepth();
            const bool shouldGrow = (target > current && target - current > hysteresis);
            const bool shouldShrink = (current > target && current - target > hysteresis);
            if (shouldGrow || shouldShrink)
                ps.jitterBuffer.setMaxDepth(target);
        }
    }

    // Adaptive send-rate / congestion response (#518): sample each connected peer's ENet link quality
    // and step its AIMD controller. The controller holds a per-peer throttle that gates both the
    // snapshot send cadence (decimation gate in the per-peer loop below) and the effective byte budget.
    // configure() each tick so reload_config param changes (and the enabled flag) take effect live; the
    // getPeerLinkStats call is a cheap field read (zeros from the mock/loopback => throttle stays 1).
    float congMinHzThisTick = 60.f; // min adaptive send rate across peers this tick
    float congMaxLossThisTick = 0.f;
    for (auto& [peerId, eid] : m_peerEntities) {
        (void)eid;
        PeerInputState& ps = m_peerInputs[peerId];
        const PeerLinkStats link = m_net.getPeerLinkStats(peerId);
        CongestionSample sample;
        sample.packetLoss = link.packetLoss;
        sample.rttMs = link.rttMs;
        sample.reliableBytesInFlight = link.reliableBytesInFlight;
        ps.congestion.configure(m_congestionParams);
        ps.congestion.update(tickIndex, sample);
        congMinHzThisTick = std::min(congMinHzThisTick, 60.f / static_cast<float>(ps.congestion.sendIntervalTicks()));
        congMaxLossThisTick = std::max(congMaxLossThisTick, link.packetLoss);
    }
    // Congestion telemetry watermarks (#714) — only advanced while peers are connected, so the values
    // freeze (rather than reset toward 60/0) once the load clients disconnect and the final metrics
    // writes still carry the run's evidence. A new all-time minimum resets the recovered watermark to
    // the current rate; recovery is then the max the (slowest) peer climbs back to afterwards.
    if (!m_peerEntities.empty()) {
        if (congMinHzThisTick < m_congMinSendHzSim) {
            m_congMinSendHzSim = congMinHzThisTick;
            m_congRecoveredSendHzSim = congMinHzThisTick;
        } else {
            m_congRecoveredSendHzSim = std::max(m_congRecoveredSendHzSim, congMinHzThisTick);
        }
        m_congMinSendHz.store(m_congMinSendHzSim, std::memory_order_relaxed);
        m_congRecoveredSendHz.store(m_congRecoveredSendHzSim, std::memory_order_relaxed);
        if (congMaxLossThisTick > m_congMaxLoss.load(std::memory_order_relaxed))
            m_congMaxLoss.store(congMaxLossThisTick, std::memory_order_relaxed);
    }

    // Wire-traffic sample (#772). Sim-thread only — the transport host is sim-thread-owned — and
    // only every kWireSampleTicks, because the GNS backend walks every live connection to build it.
    // Published to relaxed atomics for the --metrics-json writer.
    //
    // We keep the sample taken at the HIGHEST peer count seen so far (refreshing on ties, so it
    // tracks steady state rather than the first tick of the ramp), and never publish an idle one.
    // The gate reads a single end-of-run snapshot, and fl-server keeps writing that file while the
    // swarm ramps up and drains away — so "latest sample" would report the connect ramp or, worse,
    // the disconnect drain (measured: a 16-client run reported its wire rate at 2 peers), and
    // "latest sample with any peers" is no better. Full-load is the number worth gating on, and the
    // peer count travels WITH the sample so the per-client figure divides by the peers that actually
    // produced the traffic, not by whoever happens to be connected when the file is written. (Same
    // class of trap as the #714 congestion watermarks, which freeze for the same reason.)
    if (m_currentTick % kWireSampleTicks == 0 && !m_peerEntities.empty()) {
        const int peersNow = static_cast<int>(m_peerEntities.size());
        if (peersNow >= m_wirePeersAtSample.load(std::memory_order_relaxed)) {
            const WireStats w = m_net.getWireStats();
            m_wireOutKbs.store(w.outBytesPerSec / 1000.0, std::memory_order_relaxed);
            m_wireInKbs.store(w.inBytesPerSec / 1000.0, std::memory_order_relaxed);
            m_wireOutPps.store(w.outPacketsPerSec, std::memory_order_relaxed);
            m_wirePeersAtSample.store(peersNow, std::memory_order_relaxed);
        }
    }

    // Graceful tick-overrun governor (#514/#726): step from the PREVIOUS tick's measured wall-time vs
    // the fixed-step budget, then freeze its four lever values for this tick's parallel regions.
    // configure() each tick so reload_config (m_governorParams) takes effect live, like the per-peer
    // congestion controllers. Publish the levers into the atomics that getOverrunStatus() reads from
    // the main thread.
    m_tickGovernor.configure(m_governorParams);
    m_tickGovernor.update(tickIndex, m_tickProfiler.lastTotalMs(), simDt * 1000.0);
    const uint32_t govSnapInterval = m_tickGovernor.snapshotIntervalTicks();
    const uint32_t govAiStride = m_tickGovernor.aiSampleStride();
    const float govInterestScale = m_tickGovernor.interestScale();
    const float govLoadFactor = m_tickGovernor.loadFactor(); // #576: reported per throttled peer
    m_overrunLoadFactor.store(m_tickGovernor.loadFactor(), std::memory_order_relaxed);
    m_overrunSnapInterval.store(govSnapInterval, std::memory_order_relaxed);
    m_overrunAiStride.store(govAiStride, std::memory_order_relaxed);
    m_overrunInterestScale.store(govInterestScale, std::memory_order_relaxed);

    // Air-traffic control (#702): step the deterministic ATC FSM at 1 Hz on the serial sim thread,
    // BEFORE the AI pass, so a departure/arrival composition samples a clearance the service already
    // decided this tick. Drain its radio transmissions and route them through the injected sink (the
    // wire message in #703; logged until then).
    if (m_atcService && (tickIndex % fl::atc::AtcService::kIntervalTicks == 0)) {
        m_atcService->tick(m_entityManager, tickIndex);
        for (const fl::atc::RadioTransmission& tx : m_atcService->drainTransmissions()) {
            if (m_atcTransmissionSink)
                m_atcTransmissionSink(tx);
            else
                sendRadioTransmission(tx); // #703: the wire message is the default route
        }
    }

    m_tickProfiler.addPhaseSample(
        TickPhase::Maintenance, std::chrono::duration<double, std::milli>(m_clock->now() - tMaintenanceStart).count());

    // ---- Per-entity simulation: gather, AI sample pass, integrate pass ----
    // Two passes (rather than one interleaved loop) so AI sampling reads a consistent pre-step
    // world snapshot, and the integrate pass writes only each entity's own state — no cross-entity
    // writes. Both passes are therefore safe to run data-parallel (see runEntityPass). Each pass is
    // timed as one wall-clock phase.

    // Gather the live controlled entities into a contiguous, indexable range.
    // Reap orphaned controllers whose entity no longer exists (#702): an entity killed outside
    // onDisconnect (combat, AI arrival despawn, a Lua despawn) used to leave its controller lingering
    // in m_controlledEntities until its pool index was reused, so the AI/ATC passes churned a dead
    // lifecycle. get()==nullptr means the slot was freed/recycled — erase the stale entry. A present-
    // but-dead entity is left to EntityManager::onTick to reap; its controller is skipped this tick.
    m_stepItems.clear();
    for (auto it = m_controlledEntities.begin(); it != m_controlledEntities.end();) {
        auto& ce = it->second;
        EntityState* state = m_entityManager.get(ce.id);
        if (!state) {
            it = m_controlledEntities.erase(it);
            continue;
        }
        if (!state->dead)
            m_stepItems.push_back({it->first, &ce, state});
        ++it;
    }
    m_stepInputs.resize(m_stepItems.size());

    // ---- Sensing pass (#685) ----
    // Runs BEFORE the AI pass and AFTER the spatial rebuild, so a controller samples against contacts
    // detected from the same pre-step world it is about to steer in. Each observer writes only its own
    // ObserverState and reads only const world state, so the pass is data-parallel and
    // serial-equivalent: the dice are seeded from (observer, target, tick, slot, lobe) rather than
    // drawn from shared RNG state, so the contact tables are byte-identical on 1 worker and on 16.
    sensor::SensingEnvironment sensingEnv{};
    if (m_weather) {
        const EnvironmentState envState = m_weather->computeEnvironment();
        sensingEnv.cloudCoverage = envState.cloudCoverage;
        sensingEnv.fogDensity = envState.fogDensity;
        sensingEnv.fogStartDist = envState.fogStartDist;
        sensingEnv.timeOfDayH = envState.timeOfDay;
        sensingEnv.isNight = (envState.timeOfDay < 6.f || envState.timeOfDay >= 20.f);
        // Unpowered stores drift on the same steady wind the airframes fly in (#629).
        m_projectileSystem.setWind({envState.windX, 0.f, envState.windZ});
    }
    m_sensingEnv = sensingEnv; // the weapons pass reads the same conditions the sensing pass ran under (#627)
    // Unset difficulty = NO scaling: radar reaches its authored range and the AI reacts the moment it
    // detects. See setAiScaling — defaulting to AiScaling{} would silently apply the Cadet preset.
    const float radarRangeFraction = m_aiScaling ? m_aiScaling->radarSensorRange : 1.f;
    const float reactionTimeS = m_aiScaling ? m_aiScaling->reactionTimeS : 0.f;
    {
        const auto tSensingStart = m_clock->now();

        const float checkHz = m_sensorCheckHz.load(std::memory_order_relaxed);
        const auto stride =
            static_cast<uint32_t>(std::max(1L, std::lround(1.0 / (simDt * static_cast<double>(checkHz)))));

        auto& work = m_sensorSystem.gatherDue(tickIndex, stride, simDt);
        runEntityPass(work.size(),
                      [this, &work, tickIndex, &sensingEnv, radarRangeFraction, reactionTimeS](size_t b, size_t e) {
                          for (size_t i = b; i < e; ++i)
                              m_sensorSystem.evaluateObserver(work[i], m_spatialIndex, tickIndex, sensingEnv,
                                                              radarRangeFraction, reactionTimeS);
                      });

        // Reaction bookkeeping is NOT staggered: a contact's `reacted` flag must flip on the exact
        // tick its delay elapses, not up to a stride later. It touches no geometry, so it is cheap
        // enough to run every tick for every observer on the sim thread.
        m_sensorSystem.updateReactions(tickIndex, simDt, reactionTimeS);

        // RWR pass (#526): invert the just-computed contact tables into each observer's threat
        // warnings. Serial — an emitter writes into its TARGETS' state, not its own — but it reads
        // only data the parallel pass produced, so it stays serial-equivalent. Runs before the AI
        // pass so a defender can react to a lock this same tick.
        m_sensorSystem.buildThreatWarnings(tickIndex);

        m_tickProfiler.addPhaseSample(
            TickPhase::Sensing, std::chrono::duration<double, std::milli>(m_clock->now() - tSensingStart).count());
    }

    // Seat-scoped human input (#972), serial pre-pass: resolve each crewed entity's human NON-fly seat
    // input into that seat's cached SeatCommand BEFORE the parallel AI pass, so the pass reads only the
    // seat's own fields (the m_peerInputs cross-peer reads happen here, on the sim thread). A no-op
    // until a human occupies a non-fly seat (the #974 join protocol).
    applyHumanCrewInput(tickIndex);

    // AI pass: sample each controller. Read-only on shared world state (EntityState, SpatialIndex,
    // EntityManager); each controller's own mutable state is per-entity / disjoint.
    //
    // The world-wide parts of the context are shared const across workers; `contacts` is per-observer
    // and is looked up inside the worker, so a controller can only ever see ITS OWN detections.
    const AiScaling* difficulty = m_aiScaling ? &*m_aiScaling : nullptr;
    {
        const auto tAiStart = m_clock->now();
        runEntityPass(m_stepItems.size(),
                      [this, tickIndex, simDt, govAiStride, &sensingEnv, difficulty](size_t b, size_t e) {
                          for (size_t i = b; i < e; ++i) {
                              const StepItem& it = m_stepItems[i];
                              // AI-sample decimation (#514): a decimatable (non-player) entity reuses its last sampled
                              // input on ticks where (tickIndex + idx) % stride != 0 — a pure function of (idx, tick,
                              // stride), so the skip pattern is identical across worker counts (serial-equivalent), and
                              // each worker writes only its own entity's lastInput cache (disjoint). Players (stride
                              // applies to all, but decimatable=false) and any entity not yet sampled always sample.
                              if (it.ce->decimatable && it.ce->lastInputValid && govAiStride > 1u &&
                                  ((tickIndex + it.idx) % govAiStride) != 0u) {
                                  m_stepInputs[i] = it.ce->lastInput;
                              } else {
                                  const AiTickContext aiCtx{&m_spatialIndex, m_sensorSystem.contactsFor(it.idx),
                                                            &sensingEnv,     m_sensorSystem.threatsFor(it.idx),
                                                            difficulty,      m_factionRegistry};
                                  m_stepInputs[i] = it.ce->controller->sample(*it.state, tickIndex, simDt, aiCtx);
                                  it.ce->lastInput = m_stepInputs[i];
                                  it.ce->lastInputValid = true;
                                  // Crewed (#969): the Fly seat's flight is the ControlInput above; each
                                  // non-fly bot seat is sampled here and commands its turret. Per-entity
                                  // own writes stay inside this worker slice (serial-equivalent).
                                  if (it.ce->crew.crewed()) {
                                      sampleCrewSeats(*it.ce, *it.state, tickIndex, simDt, aiCtx);
                                      // #978: a knocked-out Fly seat leaves the airframe uncontrolled — no
                                      // pilot input reaches the integrator and its guns fall silent.
                                      if (it.ce->crew.flySeatDown()) {
                                          m_stepInputs[i] = ControlInput{};
                                          it.ce->lastInput = ControlInput{};
                                      }
                                  }
                              }
                          }
                      });
        m_tickProfiler.addPhaseSample(TickPhase::Ai,
                                      std::chrono::duration<double, std::milli>(m_clock->now() - tAiStart).count());
    }

    // Integrate pass: step each FlightIntegrator. Each worker writes only its own entity's state.
    {
        const auto tIntStart = m_clock->now();
        runEntityPass(m_stepItems.size(), [this, tickIndex, simDt](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i) {
                const StepItem& it = m_stepItems[i];
                stepFlightSim(*it.ce->sim, *it.state, m_stepInputs[i], it.ce->payload, simDt, it.idx, tickIndex);
                // Slew each crew turret toward its commanded direction (#969/#970). Per-entity own
                // write, so it stays disjoint in the parallel integrate pass (serial-equivalent).
                for (CrewTurret& tr : it.ce->crew.turrets)
                    stepTurret(tr.state, tr.limits, static_cast<float>(simDt));
            }
        });
        m_tickProfiler.addPhaseSample(TickPhase::Integrate,
                                      std::chrono::duration<double, std::milli>(m_clock->now() - tIntStart).count());
    }

    // Carrier deck operations (#38): deck carry, catapult strokes, arrest wires, LSO — SERIAL, right
    // after the integrate pass (mutates FlightIntegrator state and sends packets, neither worker-safe).
    runDeckOperations(simDt, tickIndex);

    // Over-G damage (#816), applied SERIALLY on the sim thread after the parallel integrate pass.
    //
    // The integrator only raises a one-shot flag; it does not damage the entity itself. It must not:
    // EntityManager::applyDamage fires event handlers and can kill the entity, and the integrate pass
    // above is data-parallel, so touching the entity manager from a worker would be a data race. This
    // is the same discipline updateTerrainSteerCache follows for its cross-entity write.
    for (const StepItem& it : m_stepItems) {
        if (!it.ce->sim->state().overg_damage)
            continue;
        // Damage scales with how far past the limit the airframe was pulled. instigator = null: this
        // is self-inflicted, and the pilot is the one who did it.
        const float n = std::abs(it.ce->sim->state().load_factor);
        const float limit = std::max(1.f, it.ce->sim->flightModel().limits.max_g_structural);
        const float excess = std::max(0.f, n - limit);
        const float damage = kOverGDamagePerG * excess;
        if (damage > 0.f)
            m_entityManager.applyDamage(it.ce->id, damage, EntityId::null());
    }

    // Crash damage (#626), same serial-apply discipline as over-G: the integrator reported a hard
    // ground impact this tick (a one-shot, cleared on its next step); the damage is applied here on
    // the sim thread, gated by the crashDamage difficulty toggle. Damage scales with how far past a
    // survivable arrival the impact was — a firm landing reports nothing, a 26 m/s arrival is fatal
    // to a 100 hp airframe.
    for (const StepItem& it : m_stepItems) {
        const float impact = it.ce->sim->state().ground_impact_speed;
        if (impact <= 0.f || !m_damageRules.crashDamage)
            continue;
        constexpr float kCrashFreeImpactMps = 6.f; // matches the integrator's reporting threshold
        constexpr float kCrashDamagePerMps = 5.f;
        const float damage = kCrashDamagePerMps * std::max(0.f, impact - kCrashFreeImpactMps);
        if (damage > 0.f)
            applyPointDamage(m_entityManager, it.ce->id, damage, EntityId::null(), m_damageRules);
    }

    // Lag-compensation history (#425): record every live entity's post-integrate position BEFORE
    // the weapons pass, so a rewind of 0 ticks and a live query see the same world. Dead entities
    // are simply not recorded — despawn GC is the ring wrapping.
    m_transformHistory.beginTick(tickIndex);
    m_entityManager.forEach([this, tickIndex](const EntityState& s) {
        m_transformHistory.add(tickIndex, s.id.index, static_cast<uint16_t>(s.id.generation), s.transform.pos);
    });

    // Weapons phase (#625): fire intents → hitscan/spawn, projectile flight, detonations. Serial
    // by design (spawn/kill/applyDamage all fire handlers), after the integrate pass so shots
    // originate from this tick's positions, before EntityManager::onTick so a same-tick kill reaps
    // on schedule.
    {
        const auto tWeapStart = m_clock->now();
        m_tickEffects.clear();
        runWeaponsPass(simDt, tickIndex);
        m_tickProfiler.addPhaseSample(TickPhase::Weapons,
                                      std::chrono::duration<double, std::milli>(m_clock->now() - tWeapStart).count());
    }

    // Cache the representative entity XZ for main-thread terrain streaming (single-player).
    updateTerrainSteerCache();

    // Diagnostics on the sim thread (after both passes, never from a worker): NaN/Inf detection and
    // the periodic trajectory trace.
    for (const StepItem& it : m_stepItems) {
        const FlightState& fs = it.ce->sim->state();
        const bool badPos =
            !std::isfinite(fs.pos_world[0]) || !std::isfinite(fs.pos_world[1]) || !std::isfinite(fs.pos_world[2]);
        const bool badVel =
            !std::isfinite(fs.vel_body[0]) || !std::isfinite(fs.vel_body[1]) || !std::isfinite(fs.vel_body[2]);
        if (badPos || badVel) {
            char msg[256];
            std::snprintf(
                msg, sizeof(msg), "[flight entity=%u] NaN/Inf — pos=(%.3g,%.3g,%.3g) vel_body=(%.3g,%.3g,%.3g)", it.idx,
                fs.pos_world[0], fs.pos_world[1], fs.pos_world[2], fs.vel_body[0], fs.vel_body[1], fs.vel_body[2]);
            m_logger.log(LogLevel::Error, __FILE__, __LINE__, msg);
        }
        // Flight-envelope departure (#891): the integrator's NaN-backstop speed guard bit, meaning
        // this entity's state diverged from anything thrust-vs-drag physics can produce. Log it once,
        // with the entity id and its state, so a diverged aircraft names itself instead of being
        // silently reaped (which made a self-resolving mission cost most of a day to trace).
        if (fs.speed_guard_clamped && m_envelopeWarned.insert(it.idx).second) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "[flight entity=%u] flight state left the physical envelope (speed guard "
                          "clamped) — vel_body=(%.1f,%.1f,%.1f) alt=%.0f; check the flight model / AI",
                          it.idx, fs.vel_body[0], fs.vel_body[1], fs.vel_body[2], fs.pos_world[1]);
            m_logger.log(LogLevel::Error, __FILE__, __LINE__, msg);
        }
        // Periodic state trace: once per second (60 Hz sim) for trajectory diagnostics.
        if (tickIndex % 60 == 0) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "[flight entity=%u] tick=%llu pos=(%.1f,%.1f,%.1f) vel_body=(%.1f,%.1f,%.1f) thr=%.0f%%",
                          it.idx, static_cast<unsigned long long>(tickIndex), fs.pos_world[0], fs.pos_world[1],
                          fs.pos_world[2], fs.vel_body[0], fs.vel_body[1], fs.vel_body[2], fs.throttle_actual * 100.f);
            m_logger.log(LogLevel::Trace, __FILE__, __LINE__, msg);
        }
    }

    // Collision phase (#630): real entity-entity collision detection, timed under `collision_ms`
    // — which until now measured EntityManager housekeeping and was a misnomer. The housekeeping
    // moves to Maintenance below; the documented meaning of `collision_ms` is unchanged (it was
    // always "the collision phase"), so the frozen schema (6) is untouched.
    {
        const auto tCollisionStart = m_clock->now();
        runCollisionPass(tickIndex);
        m_tickProfiler.addPhaseSample(
            TickPhase::Collision, std::chrono::duration<double, std::milli>(m_clock->now() - tCollisionStart).count());
    }

    // Entity-manager housekeeping (pool GC, damage-level events, generation bumps): re-attributed
    // to Maintenance (#630), where it belongs — it is not collision work.
    {
        const auto tMaintStart = m_clock->now();
        m_entityManager.onTick(simDt, tickIndex);
        m_tickProfiler.addPhaseSample(TickPhase::Maintenance,
                                      std::chrono::duration<double, std::milli>(m_clock->now() - tMaintStart).count());
    }

    // Serialize phase: telemetry, snapshot assembly + send, weather, and shutdown notices.
    const auto tSerializeStart = m_clock->now();

    // Build per-peer world snapshots with interest management and delta compression.
    //
    // Step 1: build telemetry from flight integrators (same as before).
    struct TelemetryEntry {
        uint8_t throttle;
        uint8_t fuelPct;
        uint8_t abEngaged;
        uint8_t engineFailFlags;
        float omega[3];                   // body-frame angular rates p,q,r (rad/s)
        ArticulationState articulation{}; // actuator positions (#843)
    };
    std::unordered_map<uint32_t, TelemetryEntry> entityTelemetry;
    for (auto& [entityIdx, ce] : m_controlledEntities) {
        const auto& s = ce.sim->state();
        entityTelemetry[entityIdx] = {static_cast<uint8_t>(s.throttle_actual * 100.f),
                                      static_cast<uint8_t>(std::clamp(s.fuel_kg / 4000.f * 100.f, 0.f, 100.f)),
                                      static_cast<uint8_t>(s.ab_engaged ? 1u : 0u),
                                      s.engineFailFlags,
                                      {s.omega[0], s.omega[1], s.omega[2]},
                                      s.articulation};
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
        const ArticulationState& a = tel.articulation;
        const float ch[5] = {a.gear, a.flaps, a.speedbrake, a.hook, a.canopy};
        static constexpr ArtChannel kMap[5] = {ArtChannel::Gear, ArtChannel::Flaps, ArtChannel::Speedbrake,
                                               ArtChannel::Hook, ArtChannel::Canopy};
        ArtSnap out;
        uint8_t n = 0;
        for (int i = 0; i < 5; ++i) {
            if (!(ch[i] > 0.f))
                continue; // at neutral: omit, so an unarticulated entity is free
            out.mask = static_cast<uint16_t>(out.mask | (1u << static_cast<unsigned>(kMap[i])));
            out.values[n++] = static_cast<uint8_t>(std::lround(std::clamp(ch[i], 0.f, 1.f) * 255.f));
        }
        out.count = n;
        if (out.mask != 0) {
            // FNV-1a over mask + values: the send policy needs only "did this change", and a hash is
            // one word per peer per entity instead of a whole channel set.
            uint32_t h = 2166136261u;
            auto mix = [&h](uint8_t b) {
                h ^= b;
                h *= 16777619u;
            };
            mix(static_cast<uint8_t>(out.mask & 0xFFu));
            mix(static_cast<uint8_t>(out.mask >> 8));
            for (uint8_t i = 0; i < n; ++i)
                mix(out.values[i]);
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
    snapMap.reserve(m_spatialIndex.entityCount());
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
        if (auto cit = m_controlledEntities.find(state.id.index);
            cit != m_controlledEntities.end() && !cit->second.fire.loadout.empty()) {
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
    if (m_replaySink)
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
        if (m_replaySink)
            replayEnts.emplace_back(encIdx, qe);
    }

    // Build the replay tick from those same blobs (#643). Serial, outside the parallel peer pass, and
    // skipped entirely when nobody is recording. Records go out in ASCENDING ENTITY INDEX: `encoded`
    // is an unordered_map, and a file (or a state hash) whose byte layout depended on hash-table
    // iteration order would differ between two runs of the same session and make the #644 gate
    // meaningless.
    if (m_replaySink) {
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
        m_replaySink(out);
    }

    // Crew turret-pose table (#972): for each CREWED entity that has turrets, the quantized mount-frame
    // az/el of every turret this tick. Built serially here (crew state is stable after the integrate
    // pass) so the parallel peer pass reads it lock-free and emits a per-peer SnapshotCrew TLV over the
    // entities in that peer's interest set. Single-seat/turretless entities are ABSENT, so a world of
    // only single-seat aircraft builds an empty table, emits no TLV, and is byte-identical to pre-#972.
    struct CrewSnap {
        std::vector<std::pair<int16_t, int16_t>> turrets; // (azQ, elQ) per turret, mount frame
    };
    auto quantAngle = [](float a, float range) -> int16_t {
        const float clamped = std::clamp(a, -range, range);
        return static_cast<int16_t>(std::lround(clamped / range * 32767.f));
    };
    std::unordered_map<uint32_t, CrewSnap> crewSnap;
    for (const auto& [cidx, ce] : m_controlledEntities) {
        if (!ce.crew.crewed() || ce.crew.turrets.empty())
            continue;
        if (snapMap.find(cidx) == snapMap.end())
            continue; // not live this tick
        CrewSnap cs;
        cs.turrets.reserve(ce.crew.turrets.size());
        for (const CrewTurret& tr : ce.crew.turrets)
            cs.turrets.emplace_back(quantAngle(tr.state.azRad, std::numbers::pi_v<float>),
                                    quantAngle(tr.state.elRad, std::numbers::pi_v<float> * 0.5f));
        crewSnap.emplace(cidx, std::move(cs));
    }

    const auto activePeers =
        static_cast<uint16_t>(std::max(0, std::min(m_activePeerCount.load(std::memory_order_relaxed), 65535)));

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
        m_tickGovernor.effectiveBudget(m_snapshotBudgetBytes.load(std::memory_order_relaxed));
    // Overrun interest-radius lever (#726): scale the per-peer interest radius by the governor's
    // frozen interestScale — the only lever that shrinks the visible set itself (the input to the
    // interest query, scheduler ranking, and encode) rather than trimming the encoded output after
    // ranking. A pure function of loadFactor, uniform across peers and frozen here BEFORE the
    // parallel build region, so the peer pass stays serial-equivalent by construction. Entities
    // leaving the shrunk radius are ordinary interest-out (client retention + the
    // kSnapshotRetentionTicks force-full backstop handle re-entry) — never despawned, no wire
    // change. Healthy / disabled governor => interestScale == 1 => the exact configured radius.
    const double govInterestRadiusM = m_drawDistanceM * static_cast<double>(govInterestScale);
    // Snapshot payload compression (#775): frozen before the parallel region like the governor
    // levers, so every peer in this tick sees the same setting.
    const bool compressSnap = m_compressSnapshots.load(std::memory_order_relaxed);
    m_peerWork.clear();
    // Iterate all admitted peers (#857), not m_peerEntities: an OBSERVER has an input slot but no
    // entity, and must still receive snapshots. A not-yet-admitted peer (connected but no
    // MsgConnectRequest yet) is skipped until it has a role.
    for (auto& [peerId, pin] : m_peerInputs) {
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
        // #972: peerEid is the entity the peer OCCUPIES A SEAT IN (m_peerSeat) — a Fly-seat pilot's own
        // aircraft, or a gunner's host aircraft (#974) — so every seat occupant, not just the owner,
        // centers interest on that airframe and receives its omega-carrying own record.
        const auto sit = m_peerSeat.find(peerId);
        w.peerEid = (sit != m_peerSeat.end()) ? sit->second.entity : EntityId{};
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
    // reads (snapMap, m_spatialIndex, m_entityManager.get, m_drawDistanceM, the frozen
    // govInterestRadiusM local, m_snapshotBudgetBytes, m_schedulerWeights, m_congestionParams) are
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
            std::vector<uint32_t> visible;
            if (govInterestRadiusM > 0.0) {
                const double r2 = govInterestRadiusM * govInterestRadiusM;
                const double px = center[0], py = center[1], pz = center[2];
                m_spatialIndex.queryRadius(center, govInterestRadiusM, [&](uint32_t entityIdx, const double* pos) {
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
            std::vector<uint32_t> selected;
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
                std::vector<SnapshotCandidate> cands;
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
                selected = selectSnapshotRecords(cands, recordBudget, m_schedulerWeights, m_drawDistanceM);
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
            std::vector<std::array<double, 3>> originTable;
            std::vector<uint8_t> recordStream;
            auto originIndexOf = [&originTable](const double o[3]) -> uint32_t {
                for (uint32_t i = 0; i < originTable.size(); ++i)
                    if (originTable[i][0] == o[0] && originTable[i][1] == o[1] && originTable[i][2] == o[2])
                        return i;
                originTable.push_back({o[0], o[1], o[2]});
                return static_cast<uint32_t>(originTable.size() - 1u);
            };

            std::vector<uint8_t> ownBlob; // scratch for the (single) own-entity re-encode
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
                    std::min(static_cast<uint64_t>(pin.estimatedDelayTicks) * 1000u / 60u, uint64_t{65535u}));
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
                std::vector<uint32_t> ids;
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
            // capped. Read-only over the shared m_tickEffects (built serially in the weapons pass),
            // packed byte-serially into the unaligned TLV payload — parallel-safe, worker owns buf.
            if (!m_tickEffects.empty() && peerState && !peerState->dead) {
                std::vector<uint8_t> fx;
                fx.reserve(std::min(m_tickEffects.size(), kMaxEffectsPerSnapshot) * kEffectRecordBytes);
                const double er2 = govInterestRadiusM * govInterestRadiusM;
                std::size_t emitted = 0;
                for (const EffectRecord& e : m_tickEffects) {
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
                std::vector<uint8_t> cb;
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
                    std::vector<uint8_t> payload;
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
                std::vector<uint8_t> ab;
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
    // Spectate delay (#403): a spectator (observer / dead pilot) with m_spectateDelayTicks > 0 has its
    // POSITIONAL snapshot buffered for that many ticks before delivery (anti-ghosting) — the reliable
    // channels (chat / kill feed / match state) are unaffected. 0 = off = immediate send (byte-identical
    // to before). The send-cadence bookkeeping still advances at build time so the decimation gate is
    // unchanged; only the wire delivery is deferred.
    for (PeerSnapWork& w : m_peerWork) {
        w.pin->lastSnapshotSentTick = tickIndex;
        w.pin->sentSnapshot = true;
        if (m_spectateDelayTicks > 0 && w.spectator) {
            enqueueDelayedSnapshot(*w.pin, tickIndex + m_spectateDelayTicks, w.buf);
        } else {
            m_net.send(w.peerId, w.buf.data(), w.buf.size(), /*reliable=*/false);
        }
    }
    // Drain due delayed snapshots for every peer with a queue (including peers decimated this tick, so a
    // buffered payload is never stranded). Cleared on respawn / role change / disconnect.
    if (m_spectateDelayTicks > 0) {
        for (auto& [pid, pin] : m_peerInputs) {
            while (!pin.snapshotDelayQueue.empty() && pin.snapshotDelayQueue.front().first <= tickIndex) {
                auto& payload = pin.snapshotDelayQueue.front().second;
                m_net.send(pid, payload.data(), payload.size(), /*reliable=*/false);
                pin.snapshotDelayBytes -= payload.size();
                pin.snapshotDelayQueue.pop_front();
            }
        }
    }

    // Datalink / shared team track picture (#528), per-peer unreliable at ~6 Hz — its own lower
    // cadence, decoupled from the snapshot rate. Fuses each pilot's team into one picture.
    if (tickIndex % kDatalinkIntervalTicks == 0)
        broadcastDatalink(tickIndex);

    // Tick weather and broadcast MsgWeatherState every 10 ticks (~6 Hz at 60 Hz sim).
    if (m_weather) {
        m_weather->advance(simDt);
        ++m_weatherBroadcastTick;
        if (m_weatherBroadcastTick % 10 == 0) {
            const EnvironmentState env = m_weather->computeEnvironment();
            MsgWeatherState ws;
            ws.msgId = static_cast<uint8_t>(MsgId::WeatherState);
            ws.preset = static_cast<uint8_t>(m_weather->preset());
            auto tod = m_weather->timeOfDay();
            ws.timeOfDayTenths = static_cast<uint16_t>(tod * 10.f);
            ws.fogDensity = env.fogDensity;
            ws.fogStartDist = env.fogStartDist;
            ws.windX = env.windX;
            ws.windZ = env.windZ;
            ws.turbulenceAmp = env.turbulenceAmp;        // #426
            ws.utcJulianDay = m_weather->utcJulianDay(); // #481: shared UTC clock for the geographic sun
            if (env.windProfileCount == 0) {
                // No profile: byte-identical to the legacy 32-byte packet.
                m_net.broadcast(&ws, sizeof(ws), /*reliable=*/false);
            } else {
                // Append the altitude wind profile as a TLV (#489); old clients ignore the tail.
                std::vector<uint8_t> buf(reinterpret_cast<const uint8_t*>(&ws),
                                         reinterpret_cast<const uint8_t*>(&ws) + sizeof(ws));
                std::vector<uint8_t> payload;
                payload.push_back(env.windProfileCount);
                auto pushF = [&](float f) {
                    uint8_t b[4];
                    std::memcpy(b, &f, 4);
                    payload.insert(payload.end(), b, b + 4);
                };
                for (int i = 0; i < env.windProfileCount; ++i) {
                    pushF(env.windProfile[i].altM);
                    pushF(env.windProfile[i].windX);
                    pushF(env.windProfile[i].windZ);
                }
                appendExtRaw(buf, static_cast<uint16_t>(ExtTag::WeatherWindProfile), payload.data(),
                             static_cast<uint16_t>(payload.size()));
                m_net.broadcast(buf.data(), buf.size(), /*reliable=*/false);
            }
        }
    }

    // Kill feed + per-peer combat stats (#626), reliable — after the snapshot sends so a kill's
    // despawn and its credit arrive in the same tick's traffic.
    flushCombatEvents();

    // Full scoreboard (#523), unreliable — every ~2 s while dirty, so every peer sees everyone's
    // kills/deaths/score/ping. Cheap and self-describing; a dropped one is replaced next interval.
    if (m_scoreboardDirty && tickIndex - m_lastScoreboardTick >= 120) {
        broadcastScoreboard();
        m_lastScoreboardTick = tickIndex;
        m_scoreboardDirty = false;
    }

    // ~1 Hz aggregated world-state rebuild (#600 / #861). The bounded copy is the sim thread's only
    // cost; the GM-map feed (#861) reads m_worldState, and Epic M's JSON/event-stream surface will
    // serialize the same struct off-thread. Kept at ~1 Hz (agent/GM-map cadence), not the 60 Hz tick.
    if (tickIndex % kWorldStateIntervalTicks == 0) {
        rebuildWorldState(tickIndex);
        broadcastGmWorldState(); // #861: feed the fresh aggregate to any game-master peers
    }

    // Respawn (#648): deferred death teardown + fire any due respawns (humans on request, bots auto).
    processRespawns();

    // Shutdown countdown: fire at each interval and at T=0.
    if (m_shuttingDown) {
        using namespace std::chrono;
        auto now = m_clock->now();
        if (now >= m_shutdownAt) {
            broadcastShutdownNotice(0, makeShutdownMessage(0, m_shutdownReason).c_str());
            m_shuttingDown = false;
            m_shutdownActiveShared.store(false, std::memory_order_relaxed); // #226
            m_shutdownSecsShared.store(0, std::memory_order_relaxed);
            if (m_shutdownCallback)
                m_shutdownCallback();
        } else {
            // Publish the live remaining seconds each tick for the LAN beacon (#226).
            m_shutdownSecsShared.store(static_cast<uint32_t>(duration_cast<seconds>(m_shutdownAt - now).count()),
                                       std::memory_order_relaxed);
            if (now >= m_nextNoticeAt) {
                auto secsLeft = static_cast<uint32_t>(duration_cast<seconds>(m_shutdownAt - now).count());
                broadcastShutdownNotice(static_cast<uint16_t>(secsLeft),
                                        makeShutdownMessage(secsLeft, m_shutdownReason).c_str());
                // Always squeeze in a T-60s notice: if the next interval would skip past it, clamp.
                auto nextInterval = now + seconds(m_warningIntervalS);
                auto oneMinBefore = m_shutdownAt - seconds(60);
                m_nextNoticeAt = (nextInterval > oneMinBefore && oneMinBefore > now) ? oneMinBefore : nextInterval;
            }
        }
    }

    m_net.service(0);

    m_tickProfiler.addPhaseSample(TickPhase::Serialize,
                                  std::chrono::duration<double, std::milli>(m_clock->now() - tSerializeStart).count());
    m_tickProfiler.endTick();

    // Mission objective/trigger evaluation (#633), after the world has fully stepped this tick. Runs at
    // its own second-scale cadence internally; unset unless a mission is loaded.
    if (m_missionTickHook)
        m_missionTickHook(tickIndex);
}

void WorldBroadcaster::onConnect(uint32_t peerId) {
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

    // Per-IP concurrent connection limit. Count all connected peers (m_peerInputs), including observers
    // and not-yet-admitted peers, not just spawned pilots (#853 defers the spawn past onConnect).
    if (m_maxConnectionsPerIp > 0 && !ip.empty()) {
        int count = 0;
        for (const auto& [pid, pin] : m_peerInputs)
            if (extractIp(m_net.getPeerAddress(pid)) == ip)
                ++count;
        if (count >= m_maxConnectionsPerIp) {
            rejectConnection(peerId, ip, ConnectRefusalCode::TooManyConnections);
            return;
        }
    }

    // Admin auth lockout — refuse reconnections from IPs with an active lockout.
    if (!ip.empty() && m_adminAuthTracker.isLockedOut(ip)) {
        rejectConnection(peerId, ip, ConnectRefusalCode::AdminLockout);
        return;
    }

    char msg[64];
    std::snprintf(msg, sizeof(msg), "peer %u connected", peerId);
    m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);

    // Version handshake. The client checks this and disconnects on mismatch.
    MsgHello hello;
    m_net.send(peerId, &hello, sizeof(hello), /*reliable=*/true);

    // Create the peer's input slot now, BEFORE admission (#853). A peer that connects but never sends a
    // MsgConnectRequest keeps this slot (so idle-timeout covers it) but has no entity, no role, and no
    // snapshot delivery. Admission — spawn, ConnectAck, MOTD, flight — happens in handleConnectRequest
    // when the request arrives, replacing the old "server unilaterally spawns on connect" flow.
    m_peerInputs[peerId] = {};
    m_peerInputs[peerId].lastActivityTick = m_currentTick;

    m_activePeerCount.fetch_add(1, std::memory_order_relaxed);
}

EntityId WorldBroadcaster::spawnPilotEntity(uint32_t peerId, const std::string& entityType, const EntityTransform& t,
                                            uint16_t faction, float initialAirspeed) {
    EntityId id = m_entityManager.spawn(entityType.c_str(), t, peerId);
    if (id.valid()) {
        m_peerEntities[peerId] = id;

        // Becoming a live pilot ends any spectate (#403): clear the admin spectate target and drop any
        // buffered delayed snapshots (stale wreck-view frames from the dead window). Covers respawn,
        // observer→pilot role change, and the first spawn (a no-op there).
        if (const auto pit = m_peerInputs.find(peerId); pit != m_peerInputs.end()) {
            pit->second.spectateTargetIdx = PeerInputState::kNoSpectateTarget;
            pit->second.snapshotDelayQueue.clear();
            pit->second.snapshotDelayBytes = 0;
            pit->second.snapshotDelayEvicted = false;
        }

        // Stamp the player's faction. Without a non-zero faction the player is NEUTRAL, and
        // fl::areFactionsHostile gives a neutral entity no enemies at all — so nothing would be
        // hostile to them, their wingman's engage/cover conditions could never fire, and boresight
        // designation could never designate. Faction 0 leaves the entity neutral.
        if (EntityState* s = m_entityManager.get(id); s && faction != 0) {
            s->factionIndex = faction;
        }

        // Resolve the entity type's flight model (server-authoritative; never sent on the wire).
        // Empty id, no resolver, or an unknown id falls back to the builtin UFO model.
        std::shared_ptr<const FlightModelData> model = resolveFlightModel(id);

        // PeerController reads the peer's stable input slot (pointer valid across rehash, slot torn
        // down after the controller on disconnect). Start at throttle 0 so the entity is stationary.
        // decimatable=false: a player's input must be sampled every tick for responsiveness (#514).
        addControlledEntity(id, std::make_unique<PeerController>(&m_peerInputs[peerId]), std::move(model), 0.0f,
                            /*decimatable=*/false, initialAirspeed);

        // Seat binding (#972): a pilot occupies its aircraft's Fly seat. For a crewed aircraft, stamp
        // that seat's occupant so the roster reports the pilot as Human; a single-seat aircraft has no
        // CrewState, so the binding is the conceptual seat 0 used only for the snapshot own-record.
        uint8_t flySeat = 0;
        if (auto cit = m_controlledEntities.find(id.index); cit != m_controlledEntities.end()) {
            CrewState& crew = cit->second.crew;
            for (std::size_t s = 0; s < crew.seats.size(); ++s) {
                if (crew.seats[s].isFlySeat) {
                    flySeat = static_cast<uint8_t>(s);
                    crew.seats[s].occupantPeer = peerId;
                    crew.seats[s].botOccupied = false;
                    break;
                }
            }
        }
        m_peerSeat[peerId] = PeerSeatBinding{id, flySeat};
    }
    return id;
}

EntityId WorldBroadcaster::admitPilot(uint32_t peerId, const std::string& entityType, uint16_t faction) {
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
        t.pos[1] = static_cast<double>(m_groundElevation.load(std::memory_order_relaxed)) + kSpawnAGL;
    }
    // Sandbox / round-robin fallback path: spawn stationary. The bare no-mission player flies the
    // builtin UFO, which is controllable at zero airspeed; a mission's airborne player comes through the
    // slot path below with a real cruise speed (#883).
    const uint16_t f = (faction == kNoFaction) ? m_playerFaction : faction;
    return spawnPilotEntity(peerId, entityType, t, f, /*initialAirspeed=*/0.f);
}

void WorldBroadcaster::setMissionPlayerSlots(std::vector<MissionSpawnSlot> slots) {
    m_missionSlots = std::move(slots);
    m_slotOccupant.assign(m_missionSlots.size(), kSlotFree);
    m_peerSlot.clear();
}

int WorldBroadcaster::claimMissionSlot(uint32_t peerId, uint16_t preferredFaction) {
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

void WorldBroadcaster::releaseMissionSlot(uint32_t peerId) {
    const auto it = m_peerSlot.find(peerId);
    if (it == m_peerSlot.end())
        return;
    const int idx = it->second;
    if (idx >= 0 && static_cast<std::size_t>(idx) < m_slotOccupant.size()) {
        m_slotOccupant[idx] = kSlotFree;
        // Unbind the mission object id (#884): the slot is open again, so destroy(<id>) must report the
        // slot as unoccupied (not destroyed) rather than tracking the just-despawned aircraft.
        const MissionSpawnSlot& slot = m_missionSlots[static_cast<std::size_t>(idx)];
        if (m_missionSlotBinder && !slot.missionObjectId.empty())
            m_missionSlotBinder(slot.missionObjectId, EntityId{});
    }
    m_peerSlot.erase(it);
}

std::string WorldBroadcaster::resolvePlayerEntityType(const char* requested) const {
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

void WorldBroadcaster::handleConnectRequest(uint32_t peerId, const void* data, std::size_t size) {
    if (size < sizeof(MsgConnectRequest))
        return; // truncated; ignore
    MsgConnectRequest req;
    std::memcpy(&req, data, sizeof(req));
    req.requestedEntityType[sizeof(req.requestedEntityType) - 1] = '\0'; // untrusted char[]: force-terminate

    if (req.protocolVersion != kProtocolVersion) {
        // The client also checks MsgHello and disconnects; refuse here as a backstop.
        rejectConnection(peerId, extractIp(m_net.getPeerAddress(peerId)), ConnectRefusalCode::Generic);
        return;
    }

    PeerInputState& pin = m_peerInputs[peerId];
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
            rejectConnection(peerId, extractIp(m_net.getPeerAddress(peerId)), ConnectRefusalCode::BadPassword);
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
            const std::string ip = extractIp(m_net.getPeerAddress(peerId));
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
        rejectConnection(peerId, extractIp(m_net.getPeerAddress(peerId)), ConnectRefusalCode::Generic);
        return;
    }
    const PeerRole grantedRole = static_cast<PeerRole>(req.requestedRole);
    if (grantedRole == PeerRole::Observer && !m_allowObservers) {
        rejectConnection(peerId, extractIp(m_net.getPeerAddress(peerId)), ConnectRefusalCode::RoleDenied);
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
                if (m_currentTick <= git->second.expiresTick)
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
    if (grantedRole == PeerRole::Pilot && m_teamAssigner) {
        std::optional<uint16_t> team = m_teamAssigner(peerId);
        if (!team.has_value()) {
            rejectConnection(peerId, extractIp(m_net.getPeerAddress(peerId)), ConnectRefusalCode::MatchFull);
            return;
        }
        assignedFaction = *team;
        // A reconnecting player rejoins their old team rather than being re-balanced (#524).
        if (grace)
            assignedFaction = grace->factionIndex;
    }

    EntityId assigned{}; // invalid for an observer — it has no entity
    if (grantedRole == PeerRole::Pilot) {
        // Seat claim first (#974): occupy the requested seat of an existing crewed aircraft, viewing it
        // as our "own" entity. Only when the seat is actually joinable; else fall through to a spawn.
        if (seatClaim && evaluateSeatRequest(claimEntity, claimSeat, peerId) == SeatResultCode::Granted) {
            setSeatOccupant(claimEntity, claimSeat, peerId);
            assigned = claimEntity; // a gunner does not OWN the airframe (no m_peerEntities entry)
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
                assigned = spawnPilotEntity(peerId, slot.entityType, t, slot.factionIndex, slot.airspeed);
                if (!assigned.valid()) {
                    releaseMissionSlot(peerId); // slot type unspawnable — free it and use the default path
                    assigned = admitPilot(peerId, resolvePlayerEntityType(req.requestedEntityType), assignedFaction);
                } else if (m_missionSlotBinder && !slot.missionObjectId.empty()) {
                    // Register the pilot's aircraft under the slot's mission object id so destroy(<id>) tracks
                    // it (#884). slot is a reference into m_missionSlots; read its id before any further work.
                    m_missionSlotBinder(slot.missionObjectId, assigned);
                }
            } else {
                assigned = admitPilot(peerId, resolvePlayerEntityType(req.requestedEntityType), assignedFaction);
            }
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

    // MOTD, unicast once on admission (moved from onConnect).
    if (!m_motd.empty()) {
        const std::size_t textLen = std::min(m_motd.size(), kMaxMotdBytes);
        MsgMotdHeader mhdr{};
        mhdr.displaySeconds = m_motdDisplaySeconds;
        std::vector<uint8_t> pkt;
        pkt.reserve(sizeof(MsgMotdHeader) + textLen + 1);
        appendMsg(pkt, mhdr);
        pkt.insert(pkt.end(), m_motd.c_str(), m_motd.c_str() + textLen);
        pkt.push_back(0u); // NUL terminator
        m_net.send(peerId, pkt.data(), pkt.size(), /*reliable=*/true);
    }

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
    // pre-#610 behavior. A seat-claim joiner (#974) OWNS no airframe (not in m_peerEntities), so it
    // forms no flight — it is a gunner on someone else's aircraft.
    if (assigned.valid() && m_flightSpawner && m_peerEntities.count(peerId) != 0u) {
        const fl::FormationId fid = m_flightSpawner(peerId, assigned);
        if (const fl::Formation* f = m_formations.get(fid)) {
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
        broadcastCrewRoster(assigned);

    // Match roster (#996), last so the ordered MOTD/flight-check-in sends above keep their positions.
    // Send the joiner the current roster (everyone already here), then insert + broadcast its own
    // record so existing peers learn of the join and the joiner sees itself last.
    req.callsign[sizeof(req.callsign) - 1] = '\0'; // untrusted char[]: force-terminate
    sendFullRoster(peerId);
    {
        uint16_t myFaction = 0;
        if (assigned.valid()) {
            if (const EntityState* s = m_entityManager.get(assigned))
                myFaction = s->factionIndex;
        }
        RosterRec rec;
        rec.callsign = sanitizeCallsign(req.callsign, peerId);
        rec.factionIndex = myFaction;
        rec.role = grantedRole;
        rec.isBot = false;
        upsertRoster(peerId, rec);
        // A pilot (not an observer) is a match participant with a scoreboard row (#523).
        if (grantedRole == PeerRole::Pilot)
            recordParticipant(peerId, myFaction, /*isBot=*/false, /*joined=*/true);
    }

    // Reconnection (#524): restore the held score tallies, then consume the grace entry. The dirty flag
    // owes the reconnector a fresh Stats unicast + refreshes the scoreboard.
    if (grace) {
        PeerScore& s = m_scores[peerId];
        s.kills = grace->kills;
        s.losses = grace->losses;
        s.score = grace->score;
        s.dirty = true;
        m_scoreboardDirty = true;
        m_disconnectGrace.erase(reconnectGuid);
    }

    // Match state + scoreboard for the late joiner (#523): the current phase/scores + everyone's row.
    sendMatchStateTo(peerId);
    sendScoreboardTo(peerId);
}

void WorldBroadcaster::setPeerRole(uint32_t peerId, PeerRole role) {
    auto pit = m_peerInputs.find(peerId);
    if (pit == m_peerInputs.end() || !pit->second.handshakeComplete)
        return; // unknown or not-yet-admitted peer
    PeerInputState& pin = pit->second;
    if (pin.role == role)
        return; // already in the target role

    EntityId assigned{};
    if (role == PeerRole::Observer) {
        // pilot -> observer: keep the last aircraft position as the interest center, then despawn.
        if (const auto eit = m_peerEntities.find(peerId); eit != m_peerEntities.end()) {
            if (const EntityState* s = m_entityManager.get(eit->second))
                pin.interestCenter = glm::dvec3(s->transform.pos[0], s->transform.pos[1], s->transform.pos[2]);
        }
        despawnPeerEntity(peerId);
    } else {
        // observer -> pilot: spawn an aircraft (server default type; a lone pilot — no flight formed
        // on a mid-session transition, which #648 owns).
        assigned = admitPilot(peerId, resolvePlayerEntityType(""));
    }
    pin.role = role;

    // The client learns its new assigned entity + role from a fresh MsgConnectAck (its handler applies
    // both, idempotently re-registering already-known type defs). No new message type is needed.
    sendConnectAck(peerId, assigned, role);

    // Keep the match roster (#996) in sync with the new role/team. An observer reverts to faction 0.
    if (auto rit = m_roster.find(peerId); rit != m_roster.end()) {
        RosterRec rec = rit->second;
        rec.role = role;
        if (role == PeerRole::Observer)
            rec.factionIndex = 0;
        else if (assigned.valid()) {
            if (const EntityState* s = m_entityManager.get(assigned))
                rec.factionIndex = s->factionIndex;
        }
        upsertRoster(peerId, rec);
    }
}

bool WorldBroadcaster::setPeerAuthority(uint32_t peerId, const PeerAuthority& authority) {
    auto it = m_peerInputs.find(peerId);
    if (it == m_peerInputs.end())
        return false;
    // Sanitize the mask against the known bits so an unknown future capability can never be granted.
    it->second.authority.caps = authority.caps & kAllCaps;
    it->second.authority.factionIndex = authority.factionIndex;

    // Re-send MsgConnectAck so the client's granted-authority TLV (#949) updates and its GM/moderator/
    // faction-leader UI appears or disappears. Only for an admitted peer (one that already received a
    // first ConnectAck); the peer's current entity + role are unchanged (the client's handler applies
    // the ack idempotently). The setPeerRole precedent — no new message type.
    if (it->second.handshakeComplete) {
        EntityId assigned{};
        if (const auto eit = m_peerEntities.find(peerId); eit != m_peerEntities.end())
            assigned = eit->second;
        sendConnectAck(peerId, assigned, it->second.role);
    }
    return true;
}

PeerAuthority WorldBroadcaster::getPeerAuthority(uint32_t peerId) const {
    auto it = m_peerInputs.find(peerId);
    return (it != m_peerInputs.end()) ? it->second.authority : PeerAuthority{};
}

void WorldBroadcaster::rebuildWorldState(uint64_t tickIndex) {
    // Gather the per-peer summary (the "peer picture"): every admitted peer, its role, faction, and
    // latency class. Pilots and observers both appear (an observer has no entity but still holds a
    // role + latency the GM view wants).
    std::vector<WorldStatePeer> peers;
    peers.reserve(m_peerInputs.size());
    for (const auto& [peerId, ps] : m_peerInputs) {
        if (!ps.handshakeComplete)
            continue;
        WorldStatePeer wp;
        wp.peerId = peerId;
        wp.role = static_cast<uint8_t>(ps.role);
        wp.factionIndex = factionForPeer(peerId);
        wp.delayTicks = static_cast<uint16_t>(std::min<uint32_t>(ps.estimatedDelayTicks, 0xFFFFu));
        peers.push_back(wp);
    }

    WorldStateEnvironment env;
    if (m_weather) {
        env.weatherPreset = static_cast<uint8_t>(m_weather->preset());
        env.timeOfDayHours = m_weather->timeOfDay();
        const EnvironmentState es = m_weather->computeEnvironment();
        env.windX = es.windX;
        env.windZ = es.windZ;
    }

    m_worldState = buildWorldStateSnapshot(tickIndex, m_entityManager, m_registry, &m_formations, m_factionRegistry,
                                           std::move(peers), env, &m_worldStateMission);

    // Publish an immutable copy for off-thread readers (#600). The copy is the cost of letting REST,
    // MCP and the recorder read without touching sim-thread state; at ~1 Hz that is not a budget
    // anyone can measure, and it is what makes those consumers race-free by construction.
    m_worldStatePublisher.publish(std::make_shared<const WorldStateSnapshot>(m_worldState));
}

void WorldBroadcaster::broadcastGmWorldState() {
    // Which peers get the feed: those holding GmMap. Cheap to check first so a server with no GM peers
    // pays nothing beyond the (already-built) aggregate.
    std::vector<uint32_t> gmPeers;
    for (const auto& [peerId, ps] : m_peerInputs) {
        if (ps.handshakeComplete && ps.authority.has(Capability::GmMap))
            gmPeers.push_back(peerId);
    }
    if (gmPeers.empty())
        return;

    // Encode the record stream once (identical for every GM peer — the full-battlespace aggregate).
    const auto& ents = m_worldState.entities;
    std::vector<GmEntityRecord> records;
    records.reserve(ents.size());
    for (const auto& e : ents) {
        GmEntityRecord r;
        r.entityIdx = e.entityIdx;
        r.gen = e.gen;
        r.factionIndex = e.factionIndex;
        r.typeIndex = e.typeIndex;
        r.ownerPeerId = e.ownerPeerId;
        r.formationId = e.formationId;
        r.category = e.category;
        r.damageLevel = e.damageLevel;
        r.flags = e.flags;
        r.hpPct = static_cast<uint8_t>(std::clamp(e.hpFrac, 0.f, 1.f) * 100.f + 0.5f);
        r.pos[0] = static_cast<float>(e.pos[0]);
        r.pos[1] = static_cast<float>(e.pos[1]);
        r.pos[2] = static_cast<float>(e.pos[2]);
        r.velXZ[0] = e.vel[0];
        r.velXZ[1] = e.vel[2];
        records.push_back(r);
    }

    // Chunk under the single-fragment MTU. Always send at least one (empty) packet so the client learns
    // the tick advanced and clears a stale set. Reliable + ordered, so the client's per-tick
    // double-buffer sees whole ticks.
    const std::size_t total = records.size();
    std::size_t sent = 0;
    do {
        const std::size_t n = std::min(total - sent, kMaxGmRecordsPerPacket);
        std::vector<uint8_t> buf;
        buf.reserve(sizeof(MsgGmWorldStateHeader) + n * sizeof(GmEntityRecord));
        MsgGmWorldStateHeader hdr;
        hdr.count = static_cast<uint16_t>(n);
        hdr.tick = m_worldState.tick;
        appendMsg(buf, hdr);
        for (std::size_t i = 0; i < n; ++i)
            appendMsg(buf, records[sent + i]);
        for (const uint32_t peerId : gmPeers)
            m_net.send(peerId, buf.data(), buf.size(), /*reliable=*/true);
        sent += n;
    } while (sent < total);
}

uint16_t WorldBroadcaster::factionForPeer(uint32_t peerId) const noexcept {
    const auto it = m_peerEntities.find(peerId);
    if (it == m_peerEntities.end())
        return kNoFaction;
    if (const EntityState* s = m_entityManager.get(it->second))
        return s->factionIndex;
    return kNoFaction;
}

void WorldBroadcaster::setPeerFaction(uint32_t peerId, uint16_t faction) {
    const auto rit = m_roster.find(peerId);
    if (rit == m_roster.end())
        return; // not an admitted peer

    // A pilot with a live aircraft respawns on the new team. #648's respawn machinery refines the
    // timing (delay/waves); here the switch is immediate so the mechanism is complete on its own.
    const auto pit = m_peerInputs.find(peerId);
    if (pit != m_peerInputs.end() && pit->second.role == PeerRole::Pilot) {
        despawnPeerEntity(peerId);
        const EntityId assigned = admitPilot(peerId, resolvePlayerEntityType(""), faction);
        sendConnectAck(peerId, assigned, PeerRole::Pilot);
    }

    RosterRec rec = rit->second;
    rec.factionIndex = faction;
    upsertRoster(peerId, rec);
    // Re-key the participant's team in the match (#523): participantJoined updates the faction.
    if (rec.role == PeerRole::Pilot)
        recordParticipant(peerId, faction, /*isBot=*/false, /*joined=*/true);
}

void WorldBroadcaster::despawnPeerEntity(uint32_t peerId) {
    m_peerSeat.erase(peerId); // this peer no longer occupies its aircraft's Fly seat (#972)
    auto it = m_peerEntities.find(peerId);
    if (it == m_peerEntities.end())
        return; // observer or not-yet-admitted peer — nothing to tear down

    // Tear down this peer's flight (#610) before its own entity goes away.
    //
    // The formation the peer ANCHORED (their own flight) is destroyed with its AI members: those
    // aircraft exist to fly on that player, and leaving them holding station on a dead anchor
    // would leak an entity and a controller per disconnect. Formations the peer merely COMMANDED
    // from outside (an AWACS's AI flights) survive — the aircraft are still flying, and the game
    // master can still order them; releasePeer just clears the command role.
    const fl::FormationId owned = m_formations.formationAnchoredOn(it->second);
    if (const fl::Formation* f = m_formations.get(owned)) {
        const std::vector<fl::FormationMember> members = f->members;
        for (const fl::FormationMember& m : members) {
            if (m.isAi()) {
                m_controlledEntities.erase(m.id.index);
                m_sensorSystem.removeObserver(m.id.index);
                m_countermeasures.removeDispenser(m.id.index);
                m_entityManager.kill(m.id);
            }
        }
        m_formations.destroy(owned);
    }
    // Remove the peer's own aircraft from any formation it was flying IN as a member (a human
    // wingman in someone else's flight), and drop its command role everywhere.
    const EntityId ac = it->second;
    m_formations.removeEntity(ac);
    m_formations.releasePeer(peerId);
    m_peerEntities.erase(it);
    releaseMissionSlot(peerId); // free this peer's mission player slot for the next joiner (#854)

    // #974: does another human still occupy a seat of this aircraft? If so the airframe is NEVER
    // destroyed out from under them. Swap the Fly-seat controller off the departing peer's
    // (about-to-be-freed) input slot to a hold autopilot, vacate the Fly seat in the roster, and track
    // the airframe as an orphan so it is retired when its last human leaves.
    bool otherHumans = false;
    for (const auto& [pid, bind] : m_peerSeat) {
        if (bind.entity == ac) {
            otherHumans = true;
            break;
        }
    }
    if (otherHumans) {
        if (auto cit = m_controlledEntities.find(ac.index); cit != m_controlledEntities.end()) {
            ControlledEntity& ce = cit->second;
            const ControlInput held = ce.lastInputValid ? ce.lastInput : ControlInput{};
            ce.controller = std::make_unique<HoldController>(held);
            for (CrewSeat& s : ce.crew.seats) {
                if (s.isFlySeat) {
                    s.occupantPeer = kNoSeatPeer;
                    s.botOccupied = false; // no authored bot for a peer-spawned Fly seat; the autopilot holds it
                    break;
                }
            }
            m_peerSpawnedOrphans.insert(ac.index);
            broadcastCrewRoster(ac);
        }
        return; // airframe kept alive for the remaining occupant(s)
    }

    // No humans remain — tear the aircraft down. The controller (a PeerController) points into
    // m_peerInputs, so this must happen before the caller erases the input slot.
    killControlledAircraft(ac);
}

void WorldBroadcaster::killControlledAircraft(EntityId id) {
    m_controlledEntities.erase(id.index);
    m_sensorSystem.removeObserver(id.index);
    m_countermeasures.removeDispenser(id.index);
    m_entityManager.kill(id);
    m_peerSpawnedOrphans.erase(id.index);
}

void WorldBroadcaster::maybeRetireOrphan(EntityId id) {
    if (m_peerSpawnedOrphans.find(id.index) == m_peerSpawnedOrphans.end())
        return;
    for (const auto& [pid, bind] : m_peerSeat)
        if (bind.entity == id)
            return;             // a human still occupies a seat — keep it flying
    killControlledAircraft(id); // last human left an orphaned peer-spawned airframe: retire it
}

void WorldBroadcaster::onDisconnect(uint32_t peerId) {
    char msg[64];
    std::snprintf(msg, sizeof(msg), "peer %u disconnected", peerId);
    m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);

    recordParticipant(peerId, 0, false, /*joined=*/false); // match participant left (#523)
    clearSeatOccupant(peerId); // a gunner (#974) reverts its non-fly seat to its bot + re-broadcasts
    despawnPeerEntity(peerId); // a pilot's own aircraft is torn down (no-op for observer/gunner)
    removeRoster(peerId);      // broadcast the leave + drop the roster record (#996; before m_peerInputs erase)
    m_peerInputs.erase(peerId);
    m_peerFloodState.erase(peerId);
    m_peerKnownGens.erase(peerId);
    m_peerPendingDespawn.erase(peerId);
    m_peerTraceWriters.erase(peerId); // close this peer's input trace (#560), if any
    // Reconnection (#524): before erasing the score, snapshot this peer's identity + tallies under its
    // client guid so a reconnect within the grace window restores them. The m_scores erase below stays
    // (peer-id reuse remains unsafe; the guid table is the sanctioned inheritance path).
    if (m_reconnectGraceTicks > 0) {
        if (auto git = m_peerGuids.find(peerId); git != m_peerGuids.end()) {
            GraceRec g;
            if (const auto rit = m_roster.find(peerId); rit != m_roster.end()) {
                g.callsign = rit->second.callsign;
                g.factionIndex = rit->second.factionIndex;
            }
            if (const auto sit = m_scores.find(peerId); sit != m_scores.end()) {
                g.kills = sit->second.kills;
                g.losses = sit->second.losses;
                g.score = sit->second.score;
            }
            g.expiresTick = m_currentTick + m_reconnectGraceTicks;
            m_disconnectGrace[git->second] = std::move(g);
        }
    }
    m_peerGuids.erase(peerId);
    m_scores.erase(peerId);  // ENet reuses peer ids; a rejoiner must not inherit tallies (#626)
    m_respawn.erase(peerId); // drop any pending respawn (#648)
    m_activePeerCount.fetch_sub(1, std::memory_order_relaxed);

    m_pendingAdminDrains.erase(std::remove_if(m_pendingAdminDrains.begin(), m_pendingAdminDrains.end(),
                                              [peerId](const PendingAdminDrain& d) { return d.peerId == peerId; }),
                               m_pendingAdminDrains.end());
}

const FlightIntegrator* WorldBroadcaster::integratorFor(uint32_t entityIdx) const noexcept {
    auto it = m_controlledEntities.find(entityIdx);
    return (it != m_controlledEntities.end() && it->second.sim) ? it->second.sim.get() : nullptr;
}

WarheadResult WorldBroadcaster::applyWarheadAt(const double pos[3], const BlastSpec& blast, EntityId instigator) {
    return applyWarhead(
        m_entityManager, m_spatialIndex, pos, blast, instigator, m_damageRules,
        [this](EntityId victim) { m_sensorSystem.setAvionicsFailed(victim.index); },
        // Route blast damage to a subsystem (#675); direction = the shrapnel's travel (blast → victim).
        [this](EntityId victim, float amount, const float hitDir[3]) {
            routeSubsystemDamage(victim, amount, hitDir, m_currentTick);
        });
}

void WorldBroadcaster::setWeaponRegistry(const WeaponRegistry* weapons) noexcept {
    m_weaponRegistry = weapons;
    m_projectileSystem.configure(weapons, m_gravity ? m_gravity : &CentralGravityField::earthInstance());
}

void WorldBroadcaster::queueEffect(uint8_t type, uint8_t weaponClass, uint32_t srcIdx, uint32_t tgtIdx,
                                   const double pos[3]) {
    EffectRecord e;
    e.type = type;
    e.weaponClass = weaponClass;
    e.srcIdx = srcIdx;
    e.tgtIdx = tgtIdx;
    e.pos[0] = static_cast<float>(pos[0]);
    e.pos[1] = static_cast<float>(pos[1]);
    e.pos[2] = static_cast<float>(pos[2]);
    m_tickEffects.push_back(e);
}

void WorldBroadcaster::resolveHitscan(const FireRequest& req, const WeaponDef& def, uint64_t tickIndex) {
    const EntityState* shooter = m_entityManager.getByIndex(req.shooterIdx);
    if (!shooter || shooter->dead)
        return;

    // Muzzle + bore line, with deterministic cone dispersion: two small angular offsets hashed from
    // (shooter, tick) — the turbulence/detection idiom, so a replay reproduces every round. A
    // turret-mounted gun (#970) fires along its bore (req.aimDir) instead of the airframe nose, with
    // an orthonormal dispersion basis built around that bore; a nose gun is bit-identical to before.
    const float* q = shooter->transform.quat;
    const glm::quat rot{q[3], q[0], q[1], q[2]};
    glm::vec3 bore;
    glm::vec3 up;
    glm::vec3 right;
    if (req.hasAimDir) {
        bore = glm::normalize(glm::vec3{req.aimDir[0], req.aimDir[1], req.aimDir[2]});
        const glm::vec3 ref = std::abs(bore.y) < 0.99f ? glm::vec3{0.f, 1.f, 0.f} : glm::vec3{1.f, 0.f, 0.f};
        right = glm::normalize(glm::cross(bore, ref));
        up = glm::cross(right, bore);
    } else {
        bore = rot * glm::vec3{1.f, 0.f, 0.f};
        up = rot * glm::vec3{0.f, 1.f, 0.f};
        right = rot * glm::vec3{0.f, 0.f, 1.f};
    }
    constexpr float kGunDispersionRad = 0.012f; // ~0.7° cone half-angle
    const uint32_t h = req.shooterIdx * 0x9E3779B1u + static_cast<uint32_t>(tickIndex) * 0x85EBCA77u + 0x6B43A9B5u;
    const float r1 = (static_cast<float>((h >> 8) & 0xFFFu) / 4096.f - 0.5f) * 2.f;
    const float r2 = (static_cast<float>((h >> 20) & 0xFFFu) / 4096.f - 0.5f) * 2.f;
    bore = glm::normalize(bore + up * (r1 * kGunDispersionRad) + right * (r2 * kGunDispersionRad));

    const double range = static_cast<double>(def.performance.maxRangeM);
    const glm::dvec3 origin{shooter->transform.pos[0], shooter->transform.pos[1], shooter->transform.pos[2]};

    // Lag compensation (#425/#979): a PLAYER's gun tests targets where they were when the shooter saw
    // them — `tickIndex − estimatedDelayTicks`, clamped to the history ring (≈533 ms; the bound on
    // the "shot from around the corner" effect, see docs/network-protocol.md). AI shooters have no
    // latency and rewind 0; projectiles fly real-time and never rewind — both deliberate.
    //
    // The rewind is keyed off the SHOOTING SEAT'S occupant (#979): a turret gunner's shot must
    // compensate by the GUNNER's latency, not the pilot's — occupantPeerFor(airframe, seat) returns
    // the gunner for a crew seat and the pilot for the Fly seat (whose m_peerSeat binding is the fly
    // seat), so this covers both. The airframe-owner fallback preserves the single-seat / no-seat case.
    uint32_t shootingPeer = occupantPeerFor(shooter->id, req.seat);
    if (shootingPeer == kNoOwningPeer)
        shootingPeer = peerIdForEntity(shooter->id);
    uint64_t rewindTicks = 0;
    if (shootingPeer != kNoOwningPeer) {
        if (const auto pit = m_peerInputs.find(shootingPeer); pit != m_peerInputs.end())
            rewindTicks = std::min<uint64_t>(pit->second.estimatedDelayTicks, TransformHistory::kHistoryTicks - 1);
    }
    rewindTicks = std::min(rewindTicks, tickIndex);
    const uint64_t rewTick = tickIndex - rewindTicks;

    // Nearest entity whose centre passes within the hit radius of the ray, ahead of the muzzle and
    // inside the weapon's reach. A fixed target radius until #630 introduces per-entity extents.
    // The broadphase runs on CURRENT positions, so its radius is inflated by the farthest any
    // entity can have moved since the rewound tick (the snapshot codec's velocity cap bounds it);
    // the exact ray test then uses each candidate's REWOUND position.
    constexpr double kTargetRadiusM = 8.0;
    const double drift = static_cast<double>(rewindTicks) * (kVelMaxMps / 60.0);
    double bestT = range + 1.0;
    EntityId hitId = EntityId::null();
    glm::dvec3 hitPos{};
    m_spatialIndex.queryRadius(shooter->transform.pos, range + drift, [&](uint32_t idx, const double* ep) {
        if (idx == req.shooterIdx)
            return;
        const EntityState* cand = m_entityManager.getByIndex(idx);
        if (!cand || cand->dead)
            return;
        glm::dvec3 targetPos{ep[0], ep[1], ep[2]};
        if (rewindTicks > 0) {
            // The generation check means a recycled pool slot can never be hit through history.
            // An entity that did not exist at the rewound tick is tested where it is NOW — the
            // shooter could not have seen it, but it is physically in the bullet's path.
            if (const auto past = m_transformHistory.queryAt(rewTick, idx, static_cast<uint16_t>(cand->id.generation)))
                targetPos = *past;
        }
        const glm::dvec3 rel = targetPos - origin;
        const double t = rel.x * bore.x + rel.y * bore.y + rel.z * bore.z; // along-ray distance
        if (t < 0.0 || t > range || t >= bestT)
            return;
        const glm::dvec3 closest = rel - glm::dvec3(bore) * t;
        if (glm::dot(closest, closest) > kTargetRadiusM * kTargetRadiusM)
            return;
        bestT = t;
        hitId = cand->id;
        hitPos = origin + glm::dvec3(bore) * t;
    });

    // Tracer effect from the muzzle every resolved round; impact effect + damage on a hit.
    const double muzzle[3] = {origin.x, origin.y, origin.z};
    queueEffect(static_cast<uint8_t>(EffectType::WeaponFired), static_cast<uint8_t>(def.type), req.shooterIdx,
                0xFFFFFFFFu, muzzle);
    if (hitId.valid()) {
        const double at[3] = {hitPos.x, hitPos.y, hitPos.z};
        m_currentWeaponClass = static_cast<uint8_t>(def.type);
        const bool applied = applyPointDamage(m_entityManager, hitId, def.warhead.damage, shooter->id, m_damageRules);
        m_currentWeaponClass = 0xFF;
        if (applied) {
            queueEffect(static_cast<uint8_t>(EffectType::Impact), static_cast<uint8_t>(def.type), req.shooterIdx,
                        hitId.index, at);
            // The round's travel direction routes the damage to a subsystem (#675).
            const float hitDir[3] = {bore.x, bore.y, bore.z};
            routeSubsystemDamage(hitId, def.warhead.damage, hitDir, tickIndex);
        }
    }
}

void WorldBroadcaster::executeFireRequest(const FireRequest& req, uint64_t tickIndex) {
    if (!m_weaponRegistry)
        return;
    const WeaponDef* def = m_weaponRegistry->byIndex(req.weaponIndex);
    if (!def)
        return;

    if (req.kind == FireRequest::Kind::Hitscan) {
        resolveHitscan(req, *def, tickIndex);
        return;
    }

    const EntityState* shooter = m_entityManager.getByIndex(req.shooterIdx);
    if (!shooter || shooter->dead)
        return;
    const uint32_t ownerPeer = peerIdForEntity(shooter->id);

    // Launch designation (#627): a seeker weapon is launched AT something the shooter designated —
    // the missile never invents a target. No designator wired, or nothing designated ⇒ the store
    // flies dumb, which is the honest outcome of firing blind.
    EntityId designated = EntityId::null();
    if (def->seeker && def->seeker->type != SeekerType::Unguided)
        designated = designateFor(*shooter, ownerPeer);

    // A turret-mounted store leaves along the turret bore (#970); a nose store passes null.
    const glm::vec3 aimDir{req.aimDir[0], req.aimDir[1], req.aimDir[2]};
    const EntityId pid = m_projectileSystem.launch(m_entityManager, req.weaponIndex, *shooter,
                                                   ownerPeer == kNoOwningPeer ? 0u : ownerPeer, designated, tickIndex,
                                                   req.hasAimDir ? &aimDir : nullptr);
    if (pid.valid())
        queueEffect(static_cast<uint8_t>(EffectType::MissileLaunch), static_cast<uint8_t>(def->type), req.shooterIdx,
                    0xFFFFFFFFu, shooter->transform.pos);
}

EntityId WorldBroadcaster::designateFor(const EntityState& shooter, uint32_t ownerPeer) const {
    // The designator is the #610 seam (fl-server wires the contact-honest lambda); the look axis
    // is the peer's viewAxis for a player and the nose for an AI.
    if (!m_targetDesignator)
        return EntityId::null();
    float axis[3];
    bool haveAxis = false;
    if (ownerPeer != kNoOwningPeer) {
        if (const auto pit = m_peerInputs.find(ownerPeer); pit != m_peerInputs.end()) {
            axis[0] = pit->second.viewAxis[0];
            axis[1] = pit->second.viewAxis[1];
            axis[2] = pit->second.viewAxis[2];
            haveAxis = true;
        }
    }
    if (!haveAxis) {
        const float* q = shooter.transform.quat;
        const glm::quat rot{q[3], q[0], q[1], q[2]};
        const glm::vec3 nose = rot * glm::vec3{1.f, 0.f, 0.f};
        axis[0] = nose.x;
        axis[1] = nose.y;
        axis[2] = nose.z;
    }
    return m_targetDesignator(shooter, axis);
}

void WorldBroadcaster::runWeaponsPass(double simDt, uint64_t tickIndex) {
    // Controller spawn intents (#355 — a MIRV bus deploying RVs) drain FIRST and unconditionally:
    // the seam does not depend on the weapon vocabulary. Serial here, never in the parallel AI
    // pass: spawn mutates the pool and registerController mutates the roster. The child inherits
    // the REQUESTER's ownership (an RV kill credits whoever launched the bus) and, when the
    // request names no type, the requester's own entity type.
    {
        std::vector<std::pair<SpawnRequest, EntityId>> spawnWork;
        for (auto& [idx, ce] : m_controlledEntities) {
            if (!ce.controller)
                continue;
            for (SpawnRequest& req : ce.controller->drainSpawnRequests())
                spawnWork.emplace_back(std::move(req), ce.id);
        }
        for (auto& [req, requester] : spawnWork) {
            if (!req.makeController)
                continue; // a vehicle nobody integrates would hang in the air — refuse
            const EntityState* parent = m_entityManager.get(requester);
            if (!parent)
                continue;
            std::string typeId = req.typeId;
            if (typeId.empty()) {
                const EntityDef* parentDef = m_registry.byIndex(parent->typeIndex);
                if (!parentDef)
                    continue;
                typeId = parentDef->id;
            }
            const uint32_t ownerId = parent->ownerId;
            const EntityId child = m_entityManager.spawn(typeId.c_str(), req.transform, ownerId);
            if (child.valid())
                registerController(child, req.makeController(), nullptr);
        }
    }

    // Electronic warfare (#529), independent of the weapon vocabulary — a jammer needs no missiles.
    // ECM state is applied to the entity for the NEXT tick's sensing (a jammer toggle, one-tick
    // pipeline); a countermeasure dispense (edge-detected) drops decoys NOW so this same tick's seeker
    // checks in m_projectileSystem.step already see them. Then the decoy pool ages once.
    for (auto& [idx, ce] : m_controlledEntities) {
        if (!ce.lastInputValid)
            continue;
        EntityState* st = m_entityManager.get(ce.id);
        if (!st || st->dead)
            continue;
        st->ecmActive = ce.lastInput.ecm;
        const bool dispenseNow = ce.lastInput.dispenseCm && !ce.prevDispenseCm;
        ce.prevDispenseCm = ce.lastInput.dispenseCm;
        if (dispenseNow) {
            const glm::dvec3 pos(st->transform.pos[0], st->transform.pos[1], st->transform.pos[2]);
            const glm::vec3 vel(st->transform.vel[0], st->transform.vel[1], st->transform.vel[2]);
            if (m_countermeasures.dispense(idx, pos, vel, tickIndex))
                queueEffect(static_cast<uint8_t>(EffectType::CountermeasureRelease), 0xFF, idx, UINT32_MAX,
                            st->transform.pos);
        }
    }
    m_countermeasures.step(tickIndex, static_cast<float>(simDt));

    if (!m_weaponRegistry)
        return; // no vocabulary, no fire path — trigger intent is read and discarded (pre-#583)

    // 1. Fire intents → validated requests. Serial and cheap: FireControl is pure per-entity state.
    m_fireRequests.clear();
    // Combat freeze (#523): during Ending/PostMatch no new fire intents are generated (the per-entity
    // fire state is not advanced); in-flight projectiles below still resolve normally.
    if (!m_combatFrozen)
        for (auto& [idx, ce] : m_controlledEntities) {
            // A crewed aircraft (#969) evaluates fire per seat over each seat's disjoint loadout
            // partition; the single-seat path below is untouched (byte-identical) for a plain fighter.
            if (ce.crew.crewed()) {
                runCrewedFire(ce, idx, tickIndex);
                continue;
            }
            if (!ce.lastInputValid || ce.fire.loadout.empty())
                continue;
            const EntityState* st = m_entityManager.get(ce.id);
            if (!st || st->dead)
                continue;
            const bool hold = m_formations.weaponsHoldFor(ce.id); // #610's order, with teeth at last
            evaluateFire(ce.fire, *m_weaponRegistry, ce.lastInput, hold, tickIndex, idx, m_fireRequests);
            // The live loadout is the payload truth from here on (#625): releases shrink what the
            // integrator carries next tick.
            ce.payload.extra_mass_kg = ce.fire.loadout.payloadMassKg;
            ce.payload.extra_cd0 = ce.fire.loadout.payloadCd0;

            // The pre-launch LOCK cue (#628), at the sensing cadence so it costs one lobe test per
            // peer per 100 ms: would the SELECTED seeker take the designated target right now? Peers
            // only — AI reads its contacts directly and needs no annunciator.
            if (const uint32_t peer = peerIdForEntity(ce.id); peer != kNoOwningPeer && (tickIndex + idx) % 6 == 0) {
                bool cue = false;
                if (const StationState* sel = ce.fire.loadout.selectedStation();
                    sel && sel->weaponIndex != UINT32_MAX && sel->rounds > 0) {
                    const WeaponDef* w = m_weaponRegistry->byIndex(sel->weaponIndex);
                    if (w && w->seeker && w->seeker->type != SeekerType::Unguided) {
                        const EntityId designated = designateFor(*st, peer);
                        cue = designated.valid() &&
                              m_projectileSystem.wouldAcquire(m_entityManager, sel->weaponIndex, *st, designated);
                    }
                }
                ce.fire.seekerCue = cue;
            }
        }
    // Deterministic execution order regardless of map iteration: shooter idx, then seat (#969),
    // then station. For a single-seat entity seat is always 0, so this is identical to the old
    // (shooterIdx, station) ordering.
    std::sort(m_fireRequests.begin(), m_fireRequests.end(), [](const FireRequest& a, const FireRequest& b) {
        if (a.shooterIdx != b.shooterIdx)
            return a.shooterIdx < b.shooterIdx;
        if (a.seat != b.seat)
            return a.seat < b.seat;
        return a.station < b.station;
    });

    // 2. Execute serially: hitscan resolves now, stores become pooled projectile entities.
    for (const FireRequest& req : m_fireRequests)
        executeFireRequest(req, tickIndex);

    // 3. Projectile flight + endings. Detection inside step(), application here — the over-G
    // discipline: applyWarheadAt fires event handlers and must never run inside a traversal.
    m_tickImpacts.clear();
    m_projectileSystem.step(m_entityManager, m_spatialIndex, static_cast<float>(simDt), m_groundQuery, tickIndex,
                            m_sensingEnv, m_tickImpacts);
    for (const ProjectileImpact& imp : m_tickImpacts) {
        const WeaponDef* def = m_weaponRegistry->byIndex(imp.weaponIndex);
        if (!def)
            continue;
        m_currentWeaponClass = static_cast<uint8_t>(def->type);
        BlastSpec blast{def->warhead.blastRadiusM, def->warhead.damage, def->warhead.nuclear};
        applyWarheadAt(imp.pos, blast, imp.shooter);
        m_currentWeaponClass = 0xFF;
        queueEffect(static_cast<uint8_t>(def->warhead.nuclear ? EffectType::NuclearFlash : EffectType::Detonation),
                    static_cast<uint8_t>(def->type), imp.shooter.index,
                    imp.directHit.valid() ? imp.directHit.index : 0xFFFFFFFFu, imp.pos);
    }
}

void WorldBroadcaster::runCollisionPass(uint64_t tickIndex) {
    // 1. Serial gather: every live entity with a non-zero collision radius (projectiles have their
    // own fuze path and are excluded via a 0 category default). Post-integrate positions.
    m_collisionCands.clear();
    m_collisionIdxToSlot.clear();
    m_entityManager.forEach([this](const EntityState& s) {
        if (s.dead)
            return;
        const EntityDef* def = m_registry.byIndex(s.typeIndex);
        float radius = def && def->collisionRadiusM > 0.f
                           ? def->collisionRadiusM
                           : defaultCollisionRadiusM(def ? def->category : ObjectCategory::AirVehicle);
        if (radius <= 0.f)
            return;
        const uint32_t slot = static_cast<uint32_t>(m_collisionCands.size());
        CollisionCand c;
        c.id = s.id;
        c.pos[0] = s.transform.pos[0];
        c.pos[1] = s.transform.pos[1];
        c.pos[2] = s.transform.pos[2];
        c.vel[0] = s.transform.vel[0];
        c.vel[1] = s.transform.vel[1];
        c.vel[2] = s.transform.vel[2];
        c.radius = radius;
        m_collisionCands.push_back(c);
        m_collisionIdxToSlot[s.id.index] = slot;
    });
    if (m_collisionCands.size() < 2)
        return;

    m_collisionScratch.resize(m_collisionCands.size());
    for (auto& v : m_collisionScratch)
        v.clear();

    // The spatial index was built at start-of-tick (pre-integrate), so a candidate's stored bucket
    // may lag its current position by up to a tick of travel. Inflate the broadphase query by that
    // worst-case drift on BOTH entities plus twice the max radius, so no overlapping pair is missed.
    constexpr double kMaxRadiusM = 15.0;
    const double inflate = 2.0 * kMaxRadiusM + 2.0 * (kVelMaxMps / 60.0);

    // 2. Parallel-detect: each candidate writes only its own scratch slot; reads the frozen spatial
    // index + candidate list. Canonical pairs (lower index < higher index) so a pair is recorded
    // once regardless of which side's broadphase finds it — which makes the result independent of
    // worker count (serial-equivalent).
    runEntityPass(m_collisionCands.size(), [this, inflate](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) {
            const CollisionCand& ci = m_collisionCands[i];
            const double queryR = static_cast<double>(ci.radius) + inflate;
            m_spatialIndex.queryRadius(ci.pos, queryR, [&](uint32_t otherIdx, const double* /*storedPos*/) {
                const auto it = m_collisionIdxToSlot.find(otherIdx);
                if (it == m_collisionIdxToSlot.end())
                    return;
                const CollisionCand& cj = m_collisionCands[it->second];
                if (ci.id.index >= cj.id.index)
                    return; // canonical: only the lower-index side records the pair
                if (!spheresOverlap(ci.pos, ci.radius, cj.pos, cj.radius))
                    return;
                // Deck exemption (#38): an aircraft AT OR ABOVE a landing ship's deck plane lives
                // inside its collision sphere by design — parked, trapping, on short final, or
                // fresh off the catapult. That is the landing/launch path, not a mid-air; flying
                // into the hull BELOW deck level still collides. Deliberately not limited to the
                // footprint: a cat shot crosses the sphere boundary beyond the bow at deck height,
                // and killing it there would make every launch fatal. m_decks is frozen for the
                // tick — read-only here on workers.
                for (const DeckRec& rec : m_decks) {
                    const CollisionCand* other = nullptr;
                    if (rec.entityIdx == ci.id.index)
                        other = &cj;
                    else if (rec.entityIdx == cj.id.index)
                        other = &ci;
                    else
                        continue;
                    const DeckLocalPoint lp = deckLocalPoint(other->pos, rec.pos, rec.quat, *rec.deck);
                    if (lp.y >= rec.deck->heightM - 5.f)
                        return;
                }
                CollisionPair p;
                p.a = ci.id;
                p.b = cj.id;
                p.relativeSpeedMps = relativeSpeedMps(ci.vel, cj.vel);
                m_collisionScratch[i].push_back(p);
            });
        }
    });

    // 3. Serial dedup + apply: flatten, sort for a deterministic apply order, and damage BOTH
    // entities of each pair. A mid-air is nobody's kill: instigator is null (environmental), which
    // also always applies — a collision is physics, not friendly fire, so the FF gate must not
    // suppress two wingmen who merged. Gated by the crashDamage difficulty toggle.
    if (!m_damageRules.crashDamage)
        return;
    m_collisionPairs.clear();
    for (auto& v : m_collisionScratch)
        for (const CollisionPair& p : v)
            m_collisionPairs.push_back(p);
    std::sort(m_collisionPairs.begin(), m_collisionPairs.end(), [](const CollisionPair& x, const CollisionPair& y) {
        return x.a.index != y.a.index ? x.a.index < y.a.index : x.b.index < y.b.index;
    });
    for (const CollisionPair& p : m_collisionPairs) {
        const float dmg = collisionDamage(p.relativeSpeedMps);
        if (dmg <= 0.f)
            continue;
        // Re-check liveness: an earlier pair this tick may have already killed one side.
        const EntityState* sa = m_entityManager.get(p.a);
        const EntityState* sb = m_entityManager.get(p.b);
        if (sa && !sa->dead) {
            applyPointDamage(m_entityManager, p.a, dmg, EntityId::null(), m_damageRules);
            routeSubsystemDamage(p.a, dmg, nullptr, tickIndex); // undirected — a merge is everywhere at once
        }
        if (sb && !sb->dead) {
            applyPointDamage(m_entityManager, p.b, dmg, EntityId::null(), m_damageRules);
            routeSubsystemDamage(p.b, dmg, nullptr, tickIndex);
        }
    }
}

// Fuel-leak rate when the fuel subsystem fails (#675) — a ruptured tank the pilot cannot throttle
// away. ~2 kg/s empties a typical 4000 kg internal load over ~30 minutes: a slow bleed that turns a
// long egress into a coin flip, not an instant flame-out.
static constexpr float kSubsystemFuelLeakKgS = 2.f;

void WorldBroadcaster::routeSubsystemDamage(EntityId target, float amount, const float* hitDirWorld,
                                            uint64_t tickIndex) {
    if (amount <= 0.f)
        return;
    auto it = m_controlledEntities.find(target.index);
    if (it == m_controlledEntities.end() || it->second.id != target)
        return;
    ControlledEntity& ce = it->second;
    // The router runs when the entity has a fixed subsystem table (#675) OR damageable crew seats
    // (#978). Absent both, nothing to route (the #675 fallback / no crew-seat table = unchanged).
    const bool seatDmg = ce.crew.anyDamageableSeat();
    if (!ce.hasSubsystems && !seatDmg)
        return;
    const EntityState* state = m_entityManager.get(target);
    const EntityDef* def = state ? m_registry.byIndex(state->typeIndex) : nullptr;
    if (!state || !def)
        return;
    const bool haveFixed = ce.hasSubsystems && def->damage && def->damage->subsystems;
    if (!haveFixed && !seatDmg)
        return;

    // Rotate the world-frame hit direction into the target's body frame (x=fwd, y=up, z=right); a
    // null direction stays zero = undirected (weight-only pick).
    float hitDirBody[3] = {0.f, 0.f, 0.f};
    if (hitDirWorld) {
        const float* q = state->transform.quat;
        const glm::quat rot{q[3], q[0], q[1], q[2]};
        const glm::vec3 body = glm::conjugate(rot) * glm::vec3{hitDirWorld[0], hitDirWorld[1], hitDirWorld[2]};
        hitDirBody[0] = body.x;
        hitDirBody[1] = body.y;
        hitDirBody[2] = body.z;
    }

    // One combined weighted draw (#978) over the eligible fixed subsystems AND crew seats, so a hit is
    // routed by the SAME weighted/quadrant pick — a hit damages one target, not both domains.
    float subWeights[kSubsystemCount] = {};
    float total = 0.f;
    if (haveFixed) {
        const SubsystemSet& sd = *def->damage->subsystems;
        for (int i = 0; i < kSubsystemCount; ++i) {
            const Subsystem s = static_cast<Subsystem>(i);
            const bool eligible = sd.parts[i].hp > 0.f && !ce.subsystems.failed(s);
            subWeights[i] = eligible ? sd.parts[i].weight * subsystemDirectionalBias(s, hitDirBody) : 0.f;
            total += subWeights[i];
        }
    }
    // Eligible crew seats, parallel arrays (a crewed aircraft has at most a handful of seats).
    std::vector<std::size_t> seatIdx;
    std::vector<float> seatWeight;
    for (std::size_t s = 0; s < ce.crew.seats.size(); ++s) {
        const CrewSeat& seat = ce.crew.seats[s];
        if (!seat.damageable() || seat.knockedOut || seat.hp <= 0.f)
            continue;
        const float* eye = (s < def->crew.size()) ? def->crew[s].eyepoint : nullptr;
        const float bias = eye ? crewSeatDamageBias(eye, hitDirBody) : 1.f;
        const float w = seat.hitWeight * bias;
        seatIdx.push_back(s);
        seatWeight.push_back(w);
        total += w;
    }
    if (total <= 0.f)
        return;

    const uint32_t h = subsystemHash(target.index, tickIndex, 0x51A5u);
    const float r = (static_cast<float>(h) / static_cast<float>(0x01000000)) * total;
    float cum = 0.f;
    if (haveFixed) {
        for (int i = 0; i < kSubsystemCount; ++i) {
            cum += subWeights[i];
            if (subWeights[i] > 0.f && r < cum) {
                if (applySubsystemDamage(ce.subsystems, static_cast<Subsystem>(i), amount) != 0)
                    applySubsystemEffects(ce);
                return;
            }
        }
    }
    for (std::size_t k = 0; k < seatIdx.size(); ++k) {
        cum += seatWeight[k];
        if (r < cum) {
            applySeatDamage(ce, seatIdx[k], amount);
            return;
        }
    }
    // Float-rounding backstop: apply to the last eligible seat (or, if none, the last fixed subsystem).
    if (!seatIdx.empty())
        applySeatDamage(ce, seatIdx.back(), amount);
}

void WorldBroadcaster::applySeatDamage(ControlledEntity& ce, std::size_t seatIdx, float amount) {
    if (seatIdx >= ce.crew.seats.size())
        return;
    CrewSeat& seat = ce.crew.seats[seatIdx];
    if (!seat.damageable() || seat.knockedOut)
        return;
    seat.hp -= amount;
    if (seat.hp > 0.f)
        return;
    // The seat is KNOCKED OUT: it goes silent. A non-fly bot's controller stops (turret holds its last
    // pose); a human occupant's masked input is ignored (applyHumanCrewInput skips it); the Fly seat
    // being down leaves the airframe uncontrolled (the AI pass zeroes its input). Broadcast the roster
    // so every viewer — and the occupant — sees the killed-seat state.
    seat.hp = 0.f;
    seat.knockedOut = true;
    seat.lastCommandValid = false;
    seat.seatBot.reset();
    broadcastCrewRoster(ce.id);
}

void WorldBroadcaster::applySubsystemEffects(ControlledEntity& ce) {
    if (!ce.sim)
        return;

    // Engine-out flags → the force model's asymmetric thrust. OR into whatever the tier model set
    // (kEngineFailGeneric), so a shot-out left engine and a damage-tier generic impairment coexist.
    uint8_t engineFlags = ce.sim->engineFailFlags();
    if (ce.subsystems.failed(Subsystem::EngineLeft))
        engineFlags |= kEngineFailLeft;
    if (ce.subsystems.failed(Subsystem::EngineRight))
        engineFlags |= kEngineFailRight;
    if (ce.subsystems.failed(Subsystem::Engine)) // #901: centreline single engine — total loss, no yaw
        engineFlags |= kEngineFailCenter;
    ce.sim->setEngineFailFlags(engineFlags);

    // Controls and hydraulics each strip control authority; both failed = near-total loss.
    float control = 1.f;
    if (ce.subsystems.failed(Subsystem::Controls))
        control *= 0.4f;
    if (ce.subsystems.failed(Subsystem::Hydraulics))
        control *= 0.5f;
    ce.sim->setSubsystemControlFactor(control);

    // Avionics failure strips the sensor suite to eyes (honest — it changes what the AI can see).
    if (ce.subsystems.failed(Subsystem::Avionics))
        m_sensorSystem.setAvionicsFailed(ce.id.index);

    // A ruptured fuel tank leaks; the rate scales with how much tank capacity was in that pool.
    if (ce.subsystems.failed(Subsystem::Fuel)) {
        ce.fuelLeakKgS = kSubsystemFuelLeakKgS;
        ce.sim->setFuelLeakRate(ce.fuelLeakKgS);
    }
}

uint32_t WorldBroadcaster::peerIdForEntity(EntityId id) const noexcept {
    if (!id.valid())
        return kNoOwningPeer;
    for (const auto& [peerId, eid] : m_peerEntities) {
        if (eid == id)
            return peerId;
    }
    return kNoOwningPeer;
}

uint32_t WorldBroadcaster::participantForEntity(EntityId id) const noexcept {
    // A human peer's aircraft resolves to its peerId; an AI bot's (#87) to its kBotParticipantBase id.
    // Scoreboard/kill-feed/match scoring all key on the participant id.
    const uint32_t peer = peerIdForEntity(id);
    if (peer != kNoOwningPeer)
        return peer;
    if (id.valid()) {
        const auto it = m_botEntities.find(id.index);
        if (it != m_botEntities.end())
            return it->second;
    }
    return kNoOwningPeer;
}

uint32_t WorldBroadcaster::occupantPeerFor(EntityId id, uint8_t seat) const noexcept {
    if (!id.valid())
        return kNoOwningPeer;
    for (const auto& [peerId, bind] : m_peerSeat) {
        if (bind.entity == id && bind.seatIndex == seat)
            return peerId;
    }
    return kNoOwningPeer;
}

bool WorldBroadcaster::setSeatOccupant(EntityId id, uint8_t seat, uint32_t peerId) {
    auto cit = m_controlledEntities.find(id.index);
    if (cit == m_controlledEntities.end() || cit->second.id != id)
        return false;
    CrewState& crew = cit->second.crew;
    if (seat >= crew.seats.size())
        return false;
    CrewSeat& cs = crew.seats[seat];
    if (cs.isFlySeat)
        return false; // the Fly seat belongs to the owning pilot; it is not a joinable seat

    // Vacate whatever seat this peer already held (a peer occupies at most one seat), then bind here.
    // The authored bot (if any) stays on the seat but goes dormant: sampleCrewSeats prefers the human
    // while occupantPeer is set, and clearSeatOccupant resumes it on vacate.
    clearSeatOccupant(peerId);
    m_peerInputs[peerId]; // ensure the input slot exists so the masked-input pre-pass can read it
    cs.occupantPeer = peerId;
    cs.lastCommandValid = false; // no stale bot command carries into the first human tick
    m_peerSeat[peerId] = PeerSeatBinding{id, seat};
    broadcastCrewRoster(id);
    return true;
}

void WorldBroadcaster::clearSeatOccupant(uint32_t peerId) {
    const auto sit = m_peerSeat.find(peerId);
    if (sit == m_peerSeat.end())
        return;
    const EntityId id = sit->second.entity;
    const uint8_t seat = sit->second.seatIndex;
    auto cit = m_controlledEntities.find(id.index);
    if (cit != m_controlledEntities.end() && cit->second.id == id && seat < cit->second.crew.seats.size()) {
        CrewSeat& cs = cit->second.crew.seats[seat];
        if (cs.isFlySeat)
            return; // a Fly-seat pilot's teardown is despawnPeerEntity's job, not a seat vacate
        cs.occupantPeer = kNoSeatPeer;
        cs.lastCommandValid = false; // its bot resumes next AI pass (botOccupied/seatBot preserved)
        m_peerSeat.erase(sit);
        broadcastCrewRoster(id);
        maybeRetireOrphan(id); // #974: if this was the last human on an orphaned airframe, retire it
    } else {
        m_peerSeat.erase(sit); // aircraft gone — just drop the dangling binding
    }
}

SeatResultCode WorldBroadcaster::evaluateSeatRequest(EntityId id, uint8_t seat, uint32_t peerId) const noexcept {
    if (!id.valid())
        return SeatResultCode::NoSuchEntity;
    const auto cit = m_controlledEntities.find(id.index);
    if (cit == m_controlledEntities.end() || cit->second.id != id)
        return SeatResultCode::NoSuchEntity;
    const CrewState& crew = cit->second.crew;
    if (!crew.crewed())
        return SeatResultCode::NotCrewed;
    if (seat >= crew.seats.size())
        return SeatResultCode::NoSuchSeat;
    const CrewSeat& cs = crew.seats[seat];
    if (cs.isFlySeat)
        return SeatResultCode::FlySeatNotJoinable;
    if (cs.occupantPeer != kNoSeatPeer && cs.occupantPeer != peerId)
        return SeatResultCode::SeatOccupiedByHuman;
    return SeatResultCode::Granted;
}

void WorldBroadcaster::handleSeatRequest(uint32_t peerId, const MsgSeatRequest& req) {
    const auto pit = m_peerInputs.find(peerId);
    if (pit == m_peerInputs.end() || !pit->second.handshakeComplete)
        return; // not an admitted peer
    PeerInputState& pin = pit->second;

    auto reply = [&](SeatResultCode code, EntityId target, uint8_t seat) {
        MsgSeatResult res{};
        res.code = static_cast<uint8_t>(code);
        res.seatIndex = seat;
        res.entityIdx = target.index;
        res.entityGen = target.generation;
        m_net.send(peerId, &res, sizeof(res), /*reliable=*/true);
    };

    // Leave: vacate whatever non-fly seat this peer holds and become an observer. A peer that owns its
    // aircraft (Fly-seat pilot) cannot "leave" via a seat request — use set_role / disconnect.
    if ((req.flags & kSeatRequestFlagLeave) != 0u) {
        const auto sit = m_peerSeat.find(peerId);
        if (sit == m_peerSeat.end() || m_peerEntities.count(peerId) != 0u) {
            reply(SeatResultCode::NotInSeat, EntityId{}, 0);
            return;
        }
        // Seed the observer interest center from the host aircraft's last position before vacating.
        if (const EntityState* host = m_entityManager.get(sit->second.entity))
            pin.interestCenter = glm::dvec3(host->transform.pos[0], host->transform.pos[1], host->transform.pos[2]);
        clearSeatOccupant(peerId);
        pin.role = PeerRole::Observer;
        sendConnectAck(peerId, EntityId{}, PeerRole::Observer);
        reply(SeatResultCode::Granted, EntityId{}, 0);
        return;
    }

    // Join a specific seat.
    const EntityId target{req.entityIdx, req.entityGen};
    const SeatResultCode code = evaluateSeatRequest(target, req.seatIndex, peerId);
    if (code != SeatResultCode::Granted) {
        reply(code, target, req.seatIndex);
        return;
    }
    // Free-form policy (#974): a peer may hop aircraft mid-flight. If it currently owns an aircraft,
    // relinquish it first (which persists it for its own remaining humans, or retires it).
    if (m_peerEntities.count(peerId) != 0u)
        despawnPeerEntity(peerId);
    setSeatOccupant(target, req.seatIndex, peerId); // binds + re-broadcasts the roster
    pin.role = PeerRole::Pilot;                     // a seat occupant is a Pilot-role peer (it has an entity to view)
    // Reply with the result FIRST, then re-send ConnectAck: the client learns which seat it now holds
    // (so it can gate flight prediction off a non-fly seat, #975) before the ConnectAck re-setup runs.
    // Both are reliable, so ENet preserves this order. ConnectAck centers interest/camera on the host
    // aircraft (the proven mid-session re-setup path; it also re-sends the current crew rosters).
    reply(SeatResultCode::Granted, target, req.seatIndex);
    sendConnectAck(peerId, target, PeerRole::Pilot);
}

std::string WorldBroadcaster::crewRosterText(uint32_t entityIdx) const {
    const auto cit = m_controlledEntities.find(entityIdx);
    if (cit == m_controlledEntities.end())
        return "seats: no such entity";
    const CrewState& crew = cit->second.crew;
    if (!crew.crewed())
        return "seats: entity is single-seat (no crew)";
    const EntityState* st = m_entityManager.get(cit->second.id);
    const EntityDef* def = st ? m_registry.byIndex(st->typeIndex) : nullptr;
    std::string out = "seats for entity " + std::to_string(entityIdx) + ":\n";
    for (std::size_t s = 0; s < crew.seats.size(); ++s) {
        const CrewSeat& cs = crew.seats[s];
        const char* occ = cs.occupantPeer != kNoSeatPeer ? "human" : cs.botOccupied ? "bot" : "empty";
        const char* role = (def && s < def->crew.size()) ? def->crew[s].role.c_str() : "?";
        char line[160];
        if (cs.occupantPeer != kNoSeatPeer)
            std::snprintf(line, sizeof(line), "  seat %zu (%s): %s peer=%u%s\n", s, role, occ, cs.occupantPeer,
                          cs.isFlySeat ? " [fly]" : "");
        else
            std::snprintf(line, sizeof(line), "  seat %zu (%s): %s%s\n", s, role, occ, cs.isFlySeat ? " [fly]" : "");
        out += line;
    }
    return out;
}

std::string WorldBroadcaster::adminSetSeat(uint32_t entityIdx, uint8_t seat, SeatOccupancy occ, uint32_t peerId) {
    auto cit = m_controlledEntities.find(entityIdx);
    if (cit == m_controlledEntities.end())
        return "set_seat: no such entity";
    const EntityId id = cit->second.id;
    CrewState& crew = cit->second.crew;
    if (!crew.crewed())
        return "set_seat: entity is single-seat";
    if (seat >= crew.seats.size())
        return "set_seat: seat out of range";
    CrewSeat& cs = crew.seats[seat];
    if (cs.isFlySeat)
        return "set_seat: the Fly seat is not settable (use set_role / respawn)";

    if (occ == SeatOccupancy::Human) {
        if (evaluateSeatRequest(id, seat, peerId) != SeatResultCode::Granted)
            return "set_seat: seat unavailable (occupied by another human?)";
        setSeatOccupant(id, seat, peerId);
        return "";
    }
    // Bot / Empty: vacate any human first, then set the authored-bot flag.
    if (cs.occupantPeer != kNoSeatPeer)
        clearSeatOccupant(cs.occupantPeer); // reverts + re-broadcasts; may retire an orphan
    if (auto again = m_controlledEntities.find(id.index);
        again != m_controlledEntities.end() && again->second.id == id && seat < again->second.crew.seats.size()) {
        CrewSeat& cs2 = again->second.crew.seats[seat];
        cs2.botOccupied = (occ == SeatOccupancy::Bot);
        cs2.lastCommandValid = false;
        broadcastCrewRoster(id);
    }
    return "";
}

bool WorldBroadcaster::buildCrewRosterPacket(EntityId id, std::vector<uint8_t>& out) const {
    const auto cit = m_controlledEntities.find(id.index);
    if (cit == m_controlledEntities.end() || cit->second.id != id)
        return false;
    const CrewState& crew = cit->second.crew;
    if (crew.seats.size() <= 1u)
        return false; // single-seat / non-crewed: the implicit-single-pilot fast path sends no roster
    const EntityState* st = m_entityManager.get(id);
    const EntityDef* def = st ? m_registry.byIndex(st->typeIndex) : nullptr;

    MsgCrewRosterHeader hdr{};
    hdr.seatCount = static_cast<uint8_t>(std::min<std::size_t>(crew.seats.size(), 255));
    hdr.turretCount = static_cast<uint8_t>(std::min<std::size_t>(crew.turrets.size(), 255));
    hdr.entityIdx = id.index;
    hdr.entityGen = id.generation;
    appendMsg(out, hdr);

    for (std::size_t s = 0; s < crew.seats.size() && s < 255u; ++s) {
        const CrewSeat& seat = crew.seats[s];
        CrewRosterSeat rec{};
        rec.seatIndex = static_cast<uint8_t>(s);
        rec.occupancy = static_cast<uint8_t>(seat.occupantPeer != kNoSeatPeer ? SeatOccupancy::Human
                                             : seat.botOccupied               ? SeatOccupancy::Bot
                                                                              : SeatOccupancy::Empty);
        rec.capabilities = seat.capabilities;
        rec.occupantPeerId = seat.occupantPeer;
        rec.skillPct = static_cast<uint8_t>(std::clamp(std::lround(seat.skill * 100.f), 0L, 100L));
        rec.turretIndex = seat.turretIndex >= 0 ? static_cast<uint8_t>(seat.turretIndex) : 255u;
        rec.knockedOut = seat.knockedOut ? 1u : 0u; // #978

        // Role display string from the authored def (roles-as-data, #944). Empty if the def is gone.
        if (def && s < def->crew.size())
            std::snprintf(rec.role, sizeof(rec.role), "%s", def->crew[s].role.c_str());
        appendMsg(out, rec);
    }
    return true;
}

void WorldBroadcaster::sendCrewRoster(uint32_t peerId, EntityId id) {
    std::vector<uint8_t> buf;
    if (!buildCrewRosterPacket(id, buf))
        return;
    m_net.send(peerId, buf.data(), buf.size(), /*reliable=*/true);
}

void WorldBroadcaster::broadcastCrewRoster(EntityId id) {
    std::vector<uint8_t> buf;
    if (!buildCrewRosterPacket(id, buf))
        return;
    for (const auto& [peerId, pin] : m_peerInputs)
        if (pin.handshakeComplete)
            m_net.send(peerId, buf.data(), buf.size(), /*reliable=*/true);
}

void WorldBroadcaster::onEntityEvent(const EntityEvent& event) {
    switch (event.type) {
    case EntityEventType::Died: {
        const uint32_t victimPeer = participantForEntity(event.subject);
        const uint32_t killerPeer = participantForEntity(event.instigator);
        if (victimPeer != kNoOwningPeer) {
            PeerScore& s = m_scores[victimPeer];
            ++s.losses;
            s.dirty = true;
            m_scoreboardDirty = true;

            // Enroll the dead participant for respawn (#648). Only a HUMAN peer here (bots enroll via
            // registerBotParticipant, #87); the entity teardown is deferred to processRespawns since we
            // are inside an entity-event callback. Store the team from the roster.
            if (m_respawnEnabled && peerIdForEntity(event.subject) != kNoOwningPeer) {
                RespawnRec rec;
                uint64_t due = m_currentTick + m_respawnPolicy.delayTicks;
                if (m_respawnPolicy.waves && m_respawnPolicy.waveIntervalTicks > 0) {
                    const uint64_t iv = m_respawnPolicy.waveIntervalTicks;
                    due = ((due + iv - 1) / iv) * iv; // round up to the next wave boundary
                }
                rec.dueTick = due;
                if (const auto rit = m_roster.find(victimPeer); rit != m_roster.end())
                    rec.factionIndex = rit->second.factionIndex;
                rec.isBot = false;
                rec.requested = false;
                m_respawn[victimPeer] = rec;
                m_pendingDeathCleanup.push_back(victimPeer);
            }

            // Spectate seam (#403): seed the dead peer's interest center from the wreck so its first dead
            // tick shows the crash site even before its camera-eye stream arrives. Thereafter the
            // cameraEye (#858) drives it. Only for a human peer with an input slot (bots have none).
            const uint32_t deadHumanPeer = peerIdForEntity(event.subject);
            if (deadHumanPeer != kNoOwningPeer) {
                if (const auto pit = m_peerInputs.find(deadHumanPeer); pit != m_peerInputs.end()) {
                    if (const EntityState* wreck = m_entityManager.get(event.subject))
                        pit->second.interestCenter =
                            glm::dvec3(wreck->transform.pos[0], wreck->transform.pos[1], wreck->transform.pos[2]);
                }
            }
        }

        // Feed the match controller (#523): killer + victim participants, and whether it was a team
        // kill. sameFaction reads the still-live entity factions.
        if (m_matchEventSink) {
            bool sameFaction = false;
            if (const EntityState* v = m_entityManager.get(event.subject))
                if (const EntityState* k = m_entityManager.get(event.instigator))
                    sameFaction = v->factionIndex != 0 && v->factionIndex == k->factionIndex;
            m_matchEventSink(killerPeer, victimPeer, sameFaction);
        }

        CombatEventRecord rec{};
        rec.type = static_cast<uint8_t>(CombatEventType::Kill);
        rec.weaponClass = m_currentWeaponClass; // set while a weapon damage call is on the stack (#625)
        rec.subjectIdx = event.subject.index;
        rec.subjectGen = static_cast<uint16_t>(event.subject.generation);
        rec.instigatorIdx = event.instigator.valid() ? event.instigator.index : 0xFFFFFFFFu;
        rec.instigatorGen = static_cast<uint16_t>(event.instigator.generation);
        rec.a = killerPeer;
        rec.b = victimPeer;
        m_pendingKillEvents.push_back(rec);

        // Mirror into the match event log (#600). This carries the RICH payload (entity handles +
        // weapon class), not the three scalars MatchEventSink gets -- the sink exists to score a
        // match, the log exists to say what happened.
        {
            MatchEvent me;
            me.tick = m_currentTick;
            me.type = MatchEventType::Kill;
            me.subjectIdx = event.subject.index;
            me.subjectGen = static_cast<uint16_t>(event.subject.generation);
            if (event.instigator.valid()) {
                me.instigatorIdx = event.instigator.index;
                me.instigatorGen = static_cast<uint16_t>(event.instigator.generation);
            }
            me.actor = killerPeer;
            me.target = victimPeer;
            me.weaponClass = m_currentWeaponClass;
            if (const EntityState* v = m_entityManager.get(event.subject))
                me.factionIndex = v->factionIndex;
            m_matchEventLog.append(std::move(me));
        }
        break;
    }
    case EntityEventType::Spawned: {
        // The event that did not exist before #600: without it the log could say what died but never
        // where anything came from.
        MatchEvent me;
        me.tick = m_currentTick;
        me.type = MatchEventType::Spawn;
        me.subjectIdx = event.subject.index;
        me.subjectGen = static_cast<uint16_t>(event.subject.generation);
        if (const EntityState* s = m_entityManager.get(event.subject))
            me.factionIndex = s->factionIndex;
        m_matchEventLog.append(std::move(me));
        break;
    }
    case EntityEventType::ScoreAwarded: {
        // Combat freeze (#523): no scoring accrues during the Ending/PostMatch phases.
        if (m_combatFrozen)
            break;
        const uint32_t killerPeer = participantForEntity(event.instigator);
        if (killerPeer != kNoOwningPeer) {
            PeerScore& s = m_scores[killerPeer];
            ++s.kills;
            s.score += event.score;
            s.dirty = true;
            m_scoreboardDirty = true;
        }
        break;
    }
    case EntityEventType::DamageLevelChanged: {
        // DamageDef's penalties finally act (#626): pick the tier the entity just entered and apply
        // it to the flight integrator; avionics failure strips the sensor suite to eyes.
        auto it = m_controlledEntities.find(event.subject.index);
        if (it == m_controlledEntities.end() || !it->second.sim)
            break;
        const EntityState* state = m_entityManager.get(event.subject);
        if (!state)
            break;
        const EntityDef* def = m_registry.byIndex(state->typeIndex);
        if (!def || !def->damage)
            break;

        float thrust = 1.f;
        float control = 1.f;
        bool avionics = false;
        switch (event.newDamageLevel) {
        case DamageLevel::Intact:
            break;
        case DamageLevel::Light:
            thrust = def->damage->light.thrustFactor;
            control = def->damage->light.controlFactor;
            avionics = def->damage->light.avionicsFailure;
            break;
        case DamageLevel::Heavy:
            thrust = def->damage->heavy.thrustFactor;
            control = def->damage->heavy.controlFactor;
            avionics = def->damage->heavy.avionicsFailure;
            break;
        case DamageLevel::Critical:
        case DamageLevel::Destroyed:
            thrust = def->damage->critical.thrustFactor;
            control = def->damage->critical.controlFactor;
            avionics = def->damage->critical.avionicsFailure;
            break;
        }
        it->second.sim->setDamagePenalty(thrust, control);
        if (avionics)
            m_sensorSystem.setAvionicsFailed(event.subject.index);
        break;
    }
    }
}

void WorldBroadcaster::flushCombatEvents() {
    // Kill records broadcast to everyone, chunked to stay inside a single unfragmented packet.
    if (!m_pendingKillEvents.empty()) {
        constexpr std::size_t kMaxRecordsPerPacket = 15; // 4 + 15*32 = 484 bytes
        std::vector<uint8_t> buf;
        for (std::size_t off = 0; off < m_pendingKillEvents.size(); off += kMaxRecordsPerPacket) {
            const std::size_t n = std::min(kMaxRecordsPerPacket, m_pendingKillEvents.size() - off);
            buf.clear();
            MsgCombatEventHeader hdr;
            hdr.count = static_cast<uint8_t>(n);
            appendMsg(buf, hdr);
            for (std::size_t i = 0; i < n; ++i)
                appendMsg(buf, m_pendingKillEvents[off + i]);
            m_net.broadcast(buf.data(), buf.size(), /*reliable=*/true);
        }
        m_pendingKillEvents.clear();
    }

    // Per-peer stats, unicast — each peer only ever sees its own tallies.
    for (auto& [peerId, score] : m_scores) {
        if (!score.dirty)
            continue;
        score.dirty = false;

        std::vector<uint8_t> buf;
        MsgCombatEventHeader hdr;
        hdr.count = 1;
        appendMsg(buf, hdr);
        CombatEventRecord rec{};
        rec.type = static_cast<uint8_t>(CombatEventType::Stats);
        rec.weaponClass = 0xFF;
        rec.a = score.kills;
        rec.b = score.losses;
        rec.c = score.score;
        appendMsg(buf, rec);
        m_net.send(peerId, buf.data(), buf.size(), /*reliable=*/true);
    }
}

void WorldBroadcaster::broadcastDatalink(uint64_t tickIndex) {
    if (m_peerEntities.empty())
        return;

    // Group observers by faction ONCE this tick, so fusing a team is a lookup, not a scan-per-peer.
    // A faction-0 (neutral) entity is not on anyone's team and forms no datalink net (below).
    std::unordered_map<uint16_t, std::vector<uint32_t>> factionObservers;
    for (uint32_t idx : m_sensorSystem.observerIndices()) {
        const EntityState* st = m_entityManager.getByIndex(idx);
        if (!st || st->dead)
            continue;
        factionObservers[st->factionIndex].push_back(idx);
    }

    // Priority for the track cap: a firing-quality lock, then a positively-identified foe, then state
    // rank, then proximity. A datalink that dropped the bandit locked on your leader to keep a distant
    // friendly would be worse than useless.
    auto stateRankOf = [](sensor::ContactState s) {
        switch (s) {
        case sensor::ContactState::Locked:
            return 3;
        case sensor::ContactState::Detected:
            return 2;
        case sensor::ContactState::Coasting:
            return 1;
        case sensor::ContactState::Lost:
            return 0;
        }
        return 0;
    };
    auto d2 = [](const double a[3], const double b[3]) {
        const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return dx * dx + dy * dy + dz * dz;
    };

    std::vector<uint8_t> buf;
    for (const auto& [peerId, eid] : m_peerEntities) {
        const EntityState* self = m_entityManager.get(eid);
        if (!self || self->dead)
            continue;
        const uint16_t faction = self->factionIndex;

        // Fuse the peer's own contacts (marked ownSensor) with every same-faction teammate's. Faction
        // 0 is neutral: it fuses with no one, so a lone neutral still sees its own picture but shares
        // nothing — there is no "team" to share with.
        sensor::TrackFuser fuser;
        if (const sensor::ContactTable* own = m_sensorSystem.contactsFor(eid.index))
            fuser.add(*own, /*ownSensor=*/true);
        if (faction != 0) {
            if (auto it = factionObservers.find(faction); it != factionObservers.end()) {
                for (uint32_t obsIdx : it->second) {
                    if (obsIdx == eid.index)
                        continue; // own table already added
                    if (const sensor::ContactTable* t = m_sensorSystem.contactsFor(obsIdx))
                        fuser.add(*t, /*ownSensor=*/false);
                }
            }
        }

        std::vector<sensor::FusedTrack> tracks = fuser.tracks();
        if (tracks.size() > kMaxDatalinkTracks) {
            std::sort(tracks.begin(), tracks.end(), [&](const sensor::FusedTrack& a, const sensor::FusedTrack& b) {
                const int pa =
                    (a.firingQuality ? 8 : 0) + (a.ident == sensor::Identification::Foe ? 4 : 0) + stateRankOf(a.state);
                const int pb =
                    (b.firingQuality ? 8 : 0) + (b.ident == sensor::Identification::Foe ? 4 : 0) + stateRankOf(b.state);
                if (pa != pb)
                    return pa > pb;
                const double da = d2(a.lastKnownPos, self->transform.pos);
                const double db = d2(b.lastKnownPos, self->transform.pos);
                if (da != db)
                    return da < db;
                return a.id.index < b.id.index;
            });
            tracks.resize(kMaxDatalinkTracks);
        }

        const sensor::ThreatWarningSet* threats = m_sensorSystem.threatsFor(eid.index);
        const std::size_t threatCount = threats ? std::min(threats->size(), kMaxDatalinkThreats) : 0;

        buf.clear();
        MsgDatalinkHeader hdr;
        hdr.trackCount = static_cast<uint16_t>(tracks.size());
        hdr.threatCount = static_cast<uint16_t>(threatCount);
        hdr.tickIndex = tickIndex;
        hdr.origin[0] = self->transform.pos[0];
        hdr.origin[1] = self->transform.pos[1];
        hdr.origin[2] = self->transform.pos[2];
        appendMsg(buf, hdr);

        for (const sensor::FusedTrack& t : tracks) {
            DatalinkTrack r{};
            r.targetIdx = t.id.index;
            r.targetGen = static_cast<uint16_t>(t.id.generation);
            r.typeIndex = t.typeIndex;
            r.factionIndex = t.factionIndex;
            r.state = static_cast<uint8_t>(t.state);
            r.ident = static_cast<uint8_t>(t.ident);
            r.sensorTypeMask = t.sensorTypeMask;
            r.flags = static_cast<uint8_t>((t.firingQuality ? kDatalinkFlagFiringQuality : 0u) |
                                           (t.ownSensor ? kDatalinkFlagOwnSensor : 0u));
            r.relPos[0] = static_cast<float>(t.lastKnownPos[0] - self->transform.pos[0]);
            r.relPos[1] = static_cast<float>(t.lastKnownPos[1] - self->transform.pos[1]);
            r.relPos[2] = static_cast<float>(t.lastKnownPos[2] - self->transform.pos[2]);
            r.relVel[0] = t.lastKnownVel[0];
            r.relVel[1] = t.lastKnownVel[1];
            r.relVel[2] = t.lastKnownVel[2];
            appendMsg(buf, r);
        }

        for (std::size_t i = 0; i < threatCount; ++i) {
            const sensor::ThreatWarning& w = threats->threats[i];
            DatalinkThreat r{};
            r.emitterIdx = w.emitterId.index;
            r.emitterGen = static_cast<uint16_t>(w.emitterId.generation);
            r.emitterTypeIndex = w.emitterTypeIndex;
            r.emitterFactionIndex = w.emitterFactionIndex;
            r.channel = static_cast<uint8_t>(w.channel);
            r.level = static_cast<uint8_t>(w.level);
            // Correlate the emitter with IFF so a friendly emitter reads as benign on the display.
            r.ident = static_cast<uint8_t>(sensor::classifyIff(
                m_factionRegistry ? m_factionRegistry->relationship(faction, w.emitterFactionIndex)
                                  : sensor::affiliationRelation(faction, w.emitterFactionIndex),
                static_cast<uint8_t>(1u << static_cast<int>(w.channel)), w.level != sensor::ThreatLevel::Search));
            r.relPos[0] = static_cast<float>(w.emitterPos[0] - self->transform.pos[0]);
            r.relPos[1] = static_cast<float>(w.emitterPos[1] - self->transform.pos[1]);
            r.relPos[2] = static_cast<float>(w.emitterPos[2] - self->transform.pos[2]);
            appendMsg(buf, r);
        }

        m_net.send(peerId, buf.data(), buf.size(), /*reliable=*/false);
    }
}

void WorldBroadcaster::onReceive(uint32_t peerId, const void* data, std::size_t size) {
    if (size < 1)
        return;
    uint8_t msgId;
    std::memcpy(&msgId, data, 1);

    if (msgId == static_cast<uint8_t>(MsgId::ConnectRequest)) {
        handleConnectRequest(peerId, data, size);
        return;
    }

    if (msgId == static_cast<uint8_t>(MsgId::ClientInput)) {
        if (size < sizeof(MsgClientInput))
            return; // truncated; silently discard

        MsgClientInput msg;
        std::memcpy(&msg, data, sizeof(msg));

        if (msg.protocolVersion != kProtocolVersion) {
            char vmsg[96];
            std::snprintf(vmsg, sizeof(vmsg), "peer %u: ClientInput version mismatch (got %u, want %u) — discarding",
                          peerId, static_cast<unsigned>(msg.protocolVersion), static_cast<unsigned>(kProtocolVersion));
            m_logger.log(LogLevel::Warn, __FILE__, __LINE__, vmsg);
            return;
        }

        // Packet flood detection: disconnect peers that send faster than multiplier * tick rate.
        {
            auto& flood = m_peerFloodState[peerId];
            auto now = m_clock->now();
            if (now - flood.windowStart >= std::chrono::seconds(1)) {
                flood.windowStart = now;
                flood.packetCount = 0;
            }
            ++flood.packetCount;
            if (flood.packetCount > static_cast<uint32_t>(60 * m_floodMultiplier)) {
                char fmsg[96];
                std::snprintf(fmsg, sizeof(fmsg), "peer %u flooding — %u packets/s — disconnecting", peerId,
                              flood.packetCount);
                m_logger.log(LogLevel::Warn, __FILE__, __LINE__, fmsg);
                m_net.disconnectPeer(peerId);
                return;
            }
        }

        PeerInputState& stored = m_peerInputs[peerId];

        // Staleness guard: discard out-of-order and duplicate inputs.
        if (stored.hasSeq && !isNewerSeq(msg.seqNum, stored.lastSeqNum))
            return;

        // One-way delay estimate: ticks elapsed since the client last received a snapshot.
        if (msg.tickIndex <= m_currentTick)
            stored.estimatedDelayTicks = static_cast<uint32_t>(m_currentTick - msg.tickIndex);

        // Snapshot ack: the client echoes the last WorldSnapshot tick it processed plus a selective-ack
        // bitmask of recently decoded ticks (#566). Clamp to the present so a client can't ack a future
        // tick. Adopt the high-water tick and its mask together, only on a strict advance, so ackedTick
        // and ackMask always describe the same frame (a stale/reordered input keeps the prior pair).
        {
            const uint64_t ackT = std::min(msg.tickIndex, m_currentTick);
            if (ackT > stored.ackedTick) {
                stored.ackedTick = ackT;
                stored.ackMask = msg.ackMask;
            }
        }

        // On first input, seed the jitter buffer depth from the measured one-way delay,
        // capped at the configured global maximum.
        if (!stored.hasSeq) {
            const uint32_t maxD = m_jitterMaxDepth.load(std::memory_order_relaxed);
            const uint32_t depth = (stored.estimatedDelayTicks > 0u) ? std::min(stored.estimatedDelayTicks, maxD) : 1u;
            stored.jitterBuffer.setMaxDepth(depth);
        }

        // Update per-peer EWMA of one-way delay and inter-arrival jitter.
        // alpha = 1/adaptWindow; seeded on first packet, updated on each subsequent accepted input.
        {
            const float alpha = 1.0f / static_cast<float>(m_jitterAdaptWindow);
            if (!stored.ewmaSeeded) {
                stored.ewmaDelayTicks = static_cast<float>(stored.estimatedDelayTicks);
                stored.ewmaJitterTicks = 0.f;
                stored.lastInputTick = m_currentTick;
                stored.ewmaSeeded = true;
            } else {
                stored.ewmaDelayTicks =
                    alpha * static_cast<float>(stored.estimatedDelayTicks) + (1.f - alpha) * stored.ewmaDelayTicks;
                // RFC 3550 inter-arrival jitter: expected spacing is 1 tick (60 Hz client send rate).
                const uint64_t interArrival =
                    (m_currentTick > stored.lastInputTick) ? m_currentTick - stored.lastInputTick : 0u;
                const float deviation = std::abs(static_cast<float>(interArrival) - 1.f);
                stored.ewmaJitterTicks = alpha * deviation + (1.f - alpha) * stored.ewmaJitterTicks;
                stored.lastInputTick = m_currentTick;
            }
        }

        stored.lastSeqNum = msg.seqNum;
        stored.hasSeq = true;
        stored.lastActivityTick = m_currentTick;

        // Camera-position interest (#858): an entity-less peer (an observer ghost camera, or a dead
        // peer awaiting respawn) has no aircraft transform to key interest on, so it sends its camera
        // eye each frame and the snapshot gather centers queryRadius on this point (the entity-less
        // branch there reads interestCenter). Applied immediately, not jitter-buffered — it drives
        // culling, not physics, and only needs to be frozen before the parallel peer pass, which this
        // sim-thread onReceive (which runs before the tick's parallel region) already guarantees.
        // Ignored for a pilot, whose aircraft transform wins in the gather. Finite-guarded so a
        // hostile NaN/Inf can't poison the exact-distance / spatial-hash math and blank the world.
        if (std::isfinite(msg.cameraEye[0]) && std::isfinite(msg.cameraEye[1]) && std::isfinite(msg.cameraEye[2]))
            stored.interestCenter = glm::dvec3(msg.cameraEye[0], msg.cameraEye[1], msg.cameraEye[2]);

        // Clamp and enqueue into the jitter buffer. Control fields (throttle etc.) are
        // written to stored in onTick when the buffer is drained — not here.
        // Sanitize control floats from an untrusted client: reject NaN/Inf FIRST (std::clamp passes
        // NaN straight through — NaN < lo and hi < NaN are both false), then clamp to the actuator
        // range. An unsanitized NaN would propagate into the flight sim and later trip UB at, e.g.,
        // the float->uint8 throttle telemetry cast.
        auto ctl = [](float v, float lo, float hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.f; };
        BufferedInput bi;
        bi.throttle = ctl(msg.throttle, 0.f, 1.f);
        bi.elevator = ctl(msg.elevator, -1.f, 1.f);
        bi.aileron = ctl(msg.aileron, -1.f, 1.f);
        bi.rudder = ctl(msg.rudder, -1.f, 1.f);
        bi.buttons = msg.buttons;
        bi.selectedStation = msg.selectedStation; // clamped against the entity's stations at consumption
        bi.radarMode = msg.radarMode;             // absolute radar mode (#526); validated at consumption
        bi.flaps = msg.flaps;                     // articulation commands (#843), absolute state
        bi.speedbrake = msg.speedbrake;
        bi.artButtons = msg.artButtons;
        bi.seqNum = msg.seqNum; // carried through so the applied seqNum can be acked (#427)
        stored.jitterBuffer.push(bi);

        // Server-side input tracing (#560): append the accepted, sanitized control sample to this
        // peer's FLIT trace. The writer is opened lazily so trace_start mid-session captures already
        // connected peers too; tickRate is the fixed 60 Hz server step.
        if (!m_inputTraceDir.empty()) {
            auto& writer = m_peerTraceWriters[peerId];
            if (!writer) {
                char name[128];
                std::snprintf(name, sizeof(name), "trace_peer%u_%u.flit", peerId, m_traceFileSeq++);
                // std::filesystem::path end to end (#643): building the path as a narrow string and
                // handing it to ofstream loses a non-ASCII trace directory on Windows.
                const std::filesystem::path path = std::filesystem::path(m_inputTraceDir) / name;
                writer = std::make_unique<InputTraceWriter>(path, 60u);
                if (!writer->good()) {
                    char wmsg[576];
                    std::snprintf(wmsg, sizeof(wmsg), "could not open input trace '%s' for peer %u — not tracing it",
                                  path.string().c_str(), peerId);
                    m_logger.log(LogLevel::Warn, __FILE__, __LINE__, wmsg);
                }
            }
            if (writer && writer->good())
                writer->writeRecord(m_currentTick, bi.throttle, bi.elevator, bi.aileron, bi.rudder, bi.buttons,
                                    bi.flaps, bi.speedbrake, bi.artButtons);
        }

        // viewAxis is updated immediately — it is camera state, not a flight sim input.
        float vmag = std::sqrt(msg.viewAxis[0] * msg.viewAxis[0] + msg.viewAxis[1] * msg.viewAxis[1] +
                               msg.viewAxis[2] * msg.viewAxis[2]);
        if (vmag > 1e-6f) {
            stored.viewAxis[0] = msg.viewAxis[0] / vmag;
            stored.viewAxis[1] = msg.viewAxis[1] / vmag;
            stored.viewAxis[2] = msg.viewAxis[2] / vmag;
        }
        // else: degenerate viewAxis — retain previous good value
    } else if (msgId == static_cast<uint8_t>(MsgId::AdminCommand)) {
        // Feature gate: the dispatcher must be configured. Two authentication rungs then decide the
        // issuer (#946): (1) the operator password grants Admin caps for this command; (2) an
        // empty-token peer is authenticated by its GRANTED caps (the grant channel). With neither a
        // password set nor any peer granted caps, the channel is effectively off.
        if (!m_adminDispatch)
            return;
        if (size < sizeof(MsgAdminCommand))
            return;

        // Extract IP once — used for both failure and success tracking below.
        std::string adminIp = extractIp(m_net.getPeerAddress(peerId));

        MsgAdminCommand msg;
        std::memcpy(&msg, data, sizeof(msg));
        msg.token[sizeof(msg.token) - 1] = '\0';
        msg.command[sizeof(msg.command) - 1] = '\0';
        uint16_t const reqId = msg.reqId;

        // A client with no operator password configured sends an all-NUL token; that is the signal to
        // authenticate via granted caps instead of the password (a non-empty wrong token is a genuine
        // auth-failure attempt and must still trip the lockout).
        bool tokenAllZero = true;
        for (std::size_t i = 0; i < sizeof(msg.token); ++i) {
            if (msg.token[i] != '\0') {
                tokenAllZero = false;
                break;
            }
        }

        CommandIssuer issuer;
        bool authorized = false;

        // Rung 1: operator password (constant-time compare, only meaningful when a password is set —
        // an empty password must never match, or an empty token would authenticate everyone).
        if (!m_operatorPassword.empty()) {
            const std::string& pw = m_operatorPassword;
            uint8_t diff = 0;
            for (std::size_t i = 0; i < sizeof(msg.token); ++i) {
                uint8_t a = static_cast<uint8_t>(msg.token[i]);
                uint8_t b = (i < pw.size()) ? static_cast<uint8_t>(pw[i]) : 0u;
                diff |= (a ^ b);
            }
            for (std::size_t i = sizeof(msg.token); i < pw.size(); ++i)
                diff |= static_cast<uint8_t>(pw[i]);
            if (diff == 0) {
                issuer = CommandIssuer{peerId, kAdminCaps, factionForPeer(peerId)};
                authorized = true;
                if (!adminIp.empty())
                    m_adminAuthTracker.recordSuccess(adminIp);
            } else if (!tokenAllZero) {
                // A non-empty wrong token is a brute-force attempt: log + lockout, exactly as before.
                char lmsg[96];
                std::snprintf(lmsg, sizeof(lmsg), "peer %u: MsgAdminCommand bad token — discarding", peerId);
                m_logger.log(LogLevel::Warn, __FILE__, __LINE__, lmsg);
                if (!adminIp.empty() && m_adminAuthTracker.recordFailure(adminIp)) {
                    char lk[128];
                    std::snprintf(lk, sizeof(lk), "peer %u (%s): admin auth lockout triggered — kicking", peerId,
                                  adminIp.c_str());
                    m_logger.log(LogLevel::Warn, __FILE__, __LINE__, lk);
                    m_net.disconnectPeer(peerId);
                }
                return;
            }
            // else: password set but token empty — fall through to the grant channel below.
        }

        // Rung 2: the grant channel. A peer that did not authenticate with the password reaches
        // dispatch via its granted caps. A granted peer runs unthrottled (like an operator); a
        // zero-cap peer is a permission refusal, NOT an auth failure (no lockout pollution, #947).
        if (!authorized) {
            auto pit = m_peerInputs.find(peerId);
            PeerInputState* ps = (pit != m_peerInputs.end()) ? &pit->second : nullptr;

            if (ps && ps->authority.any()) {
                issuer = CommandIssuer{peerId, ps->authority.caps, ps->authority.factionIndex};
                authorized = true;
            } else {
                // No password match and no granted caps. Rate-limit this refuse path (1 s window,
                // wingman idiom) so it is not a free probe/amplification channel, then either refuse
                // with a clear message (when a password is configured, i.e. the channel is on and the
                // peer merely lacks a grant) or silently drop (no password set — the channel is off
                // for an ungranted peer, preserving the "no admin without credentials" behavior).
                if (ps) {
                    const auto now = m_clock->now();
                    if (now - ps->adminCmdWindowStart >= std::chrono::seconds(1)) {
                        ps->adminCmdWindowStart = now;
                        ps->adminCmdCount = 0;
                    }
                    if (++ps->adminCmdCount > kUnauthAdminCmdsPerSecond)
                        return;
                }
                if (!m_operatorPassword.empty()) {
                    sendAdminResponse(m_net, peerId, reqId,
                                      "permission denied: the admin channel requires a granted role or the "
                                      "operator password");
                }
                return;
            }
        }

        std::string_view cmdView(msg.command);
        if (cmdView.empty())
            return;

        // Dispatch on the sim thread (same as stdin admin loop). The registry permission-checks the
        // command against issuer.caps and refuses with a clear message when insufficient.
        // Mutating commands enqueue via gameLoop.enqueueSimCallback() internally.
        std::string result = m_adminDispatch(cmdView, issuer);

        // The audit record docs/ai-architecture.md §4 requires (#600): until now the only trace an admin
        // command left was the Info log line below, which no consumer can read.
        {
            MatchEvent me;
            me.tick = m_currentTick;
            me.type = MatchEventType::AdminCommand;
            me.actor = issuer.peerId;
            me.factionIndex = issuer.factionIndex;
            me.text = std::string(cmdView);
            m_matchEventLog.append(std::move(me));
        }

        {
            char lmsg[256];
            std::snprintf(lmsg, sizeof(lmsg), "peer %u [net-admin] %.*s -> %.*s", peerId,
                          static_cast<int>(cmdView.size()), cmdView.data(),
                          static_cast<int>(std::min(result.size(), std::size_t{80})), result.c_str());
            m_logger.log(LogLevel::Info, __FILE__, __LINE__, lmsg);
        }

        sendAdminResponse(m_net, peerId, reqId, result);

        // Queue a wall-clock-deferred drain: mark taken after dispatch (skips any sync
        // shell.print() calls made during dispatch); fires after kENetAdminDrainDelayMs ms,
        // giving enqueueSimCallback lambdas time to run regardless of tick-batch catch-up.
        if (m_adminShellMark && m_adminShellDrain)
            m_pendingAdminDrains.push_back({peerId, reqId, m_adminShellMark(),
                                            m_clock->now() + std::chrono::milliseconds(kENetAdminDrainDelayMs)});
    } else if (msgId == static_cast<uint8_t>(MsgId::Heartbeat)) {
        MsgHeartbeat hb;
        if (!readMsg(data, size, hb))
            return;
        auto& ps = m_peerInputs[peerId];
        ps.lastActivityTick = m_currentTick;
        if (hb.tickIndex <= m_currentTick)
            ps.estimatedDelayTicks = static_cast<uint32_t>(m_currentTick - hb.tickIndex);
        // Heartbeat also acks the last processed snapshot (idle clients with no MsgClientInput),
        // carrying the same selective-ack bitmask. Adopt tick + mask together on a strict advance.
        {
            const uint64_t ackT = std::min(hb.tickIndex, m_currentTick);
            if (ackT > ps.ackedTick) {
                ps.ackedTick = ackT;
                ps.ackMask = hb.ackMask;
            }
        }

        // Reply with the current delay estimate so the client can display "Ping: N ms".
        MsgPeerDelay pd;
        pd.delayTicks = static_cast<uint16_t>(std::min(ps.estimatedDelayTicks, 65535u));
        m_net.send(peerId, &pd, sizeof(pd), /*reliable=*/false);

    } else if (msgId == static_cast<uint8_t>(MsgId::WingmanCommand)) {
        handleWingmanCommand(peerId, data, size);
    } else if (msgId == static_cast<uint8_t>(MsgId::SeatRequest)) {
        MsgSeatRequest req;
        if (readMsg(data, size, req))
            handleSeatRequest(peerId, req);
    } else if (msgId == static_cast<uint8_t>(MsgId::RadioCommand)) {
        handleRadioCommand(peerId, data, size);
    } else if (msgId == static_cast<uint8_t>(MsgId::Chat)) {
        handleChat(peerId, data, size);
    } else if (msgId == static_cast<uint8_t>(MsgId::VoiceFrame)) {
        handleVoiceFrame(peerId, data, size);
    } else if (msgId == static_cast<uint8_t>(MsgId::TeamRequest)) {
        // Mid-match team switch request (#522). Guard it against unbalancing, then despawn+respawn on
        // the new team. An unadmitted peer or a guard denial is answered with a MsgServerNotice.
        MsgTeamRequest req;
        if (readMsg(data, size, req)) {
            const auto pit = m_peerInputs.find(peerId);
            if (pit != m_peerInputs.end() && pit->second.handshakeComplete) {
                const bool allowed = !m_teamSwitchGuard || m_teamSwitchGuard(peerId, req.factionIndex);
                if (allowed) {
                    setPeerFaction(peerId, req.factionIndex);
                } else {
                    sendNoticeTo(peerId, "Team switch denied (would unbalance).");
                }
            }
        }
    }
    // Unknown msgIds: silently discard (no log spam; future protocol versions may add new IDs)
}

void WorldBroadcaster::sendNoticeTo(uint32_t peerId, const char* text) {
    MsgServerNotice notice;
    std::snprintf(notice.text, sizeof(notice.text), "%s", text);
    m_net.send(peerId, &notice, sizeof(notice), /*reliable=*/true);
}

void WorldBroadcaster::sendWingmanAck(uint32_t peerId, uint8_t command, WingmanResult result, uint16_t flightId,
                                      uint8_t flightSize, uint32_t memberIdx, uint32_t targetIdx) {
    MsgWingmanAck ack{};
    ack.command = command;
    ack.result = static_cast<uint8_t>(result);
    ack.flightSize = flightSize;
    ack.memberIdx = memberIdx;
    ack.targetIdx = targetIdx;
    ack.flightId = flightId;
    m_net.send(peerId, &ack, sizeof(ack), /*reliable=*/true);
}

namespace {
// Copy an ATC RadioTransmission into its wire form; snprintf force-terminates each char[] safely.
MsgRadioTransmission buildRadioWire(const fl::atc::RadioTransmission& tx, uint8_t netId) {
    MsgRadioTransmission w{};
    w.netId = netId;
    w.displaySeconds = tx.displaySeconds;
    std::snprintf(w.speaker, sizeof(w.speaker), "%s", tx.speaker.c_str());
    std::snprintf(w.voiceKey, sizeof(w.voiceKey), "%s", tx.voiceKey.c_str());
    std::snprintf(w.text, sizeof(w.text), "%s", tx.text.c_str());
    return w;
}
} // namespace

void WorldBroadcaster::sendHapticTo(uint32_t peerId, uint8_t kind, float a, float b, uint16_t durationMs) {
    MsgHaptic msg;
    msg.kind = kind;
    msg.a = a;
    msg.b = b;
    msg.durationMs = durationMs;
    m_net.send(peerId, &msg, sizeof(msg), /*reliable=*/true);
}

void WorldBroadcaster::rebuildDeckRecords() {
    m_decks.clear();
    m_entityManager.forEach([this](const EntityState& s) {
        const EntityDef* def = m_registry.byIndex(s.typeIndex);
        if (!def || !def->deck || !def->acceptsLandings)
            return;
        DeckRec rec;
        rec.entityIdx = s.id.index;
        rec.pos[0] = s.transform.pos[0];
        rec.pos[1] = s.transform.pos[1];
        rec.pos[2] = s.transform.pos[2];
        for (int i = 0; i < 4; ++i)
            rec.quat[i] = s.transform.quat[i];
        for (int i = 0; i < 3; ++i)
            rec.vel[i] = s.transform.vel[i];
        rec.floorElevM = m_gravity->geodeticAltitude(rec.pos) + static_cast<double>(def->deck->heightM);
        rec.deck = &*def->deck;
        rec.shipDef = def;
        m_decks.push_back(rec);
    });
}

void WorldBroadcaster::runDeckOperations(double simDt, uint64_t tickIndex) {
    if (m_decks.empty())
        return;

    // LSO phrase codes (repeat-suppressed per aircraft; 255 = none yet).
    enum LsoPhrase : uint8_t { kOnGlideslope = 0, kHigh, kLow, kFast, kSlow, kWaveOff };
    static constexpr const char* kLsoText[] = {
        "Paddles: on glideslope, on speed.", "Paddles: you're HIGH.",
        "Paddles: you're LOW. Power!",       "Paddles: you're fast.",
        "Paddles: you're slow. Power!",      "Paddles: WAVE OFF, WAVE OFF!",
    };
    constexpr double kLsoRangeM = 5556.0; // 3 nm
    constexpr float kGlideslopeDeg = 3.5f;

    for (const StepItem& it : m_stepItems) {
        ControlledEntity& ce = *it.ce;
        if (ce.sim->flightModel().isVessel())
            continue; // a ship does not land on itself (or on its escorts)
        const FlightState& fs = ce.sim->state();
        const double ownAlt = m_gravity->geodeticAltitude(fs.pos_world);

        // Find the deck this aircraft is over (footprint + at deck level), if any.
        const DeckRec* over = nullptr;
        DeckLocalPoint local{};
        for (const DeckRec& rec : m_decks) {
            if (rec.entityIdx == it.idx)
                continue;
            const DeckLocalPoint lp = deckLocalPoint(fs.pos_world, rec.pos, rec.quat, *rec.deck);
            if (lp.inFootprint) {
                over = &rec;
                local = lp;
                break;
            }
        }

        const bool onDeck = over && deckFloorApplies(local, *over->deck) && (ownAlt - over->floorElevM) <= 0.6;
        if (onDeck) {
            const DeckDef& deck = *over->deck;
            FlightState ns = fs;

            // World-frame velocity of the aircraft (for speeds and the catapult delta-v).
            float velBodyF[3] = {float(ns.vel_body[0]), float(ns.vel_body[1]), float(ns.vel_body[2])};
            float velWorld[3];
            quatRotate(ns.quat, velBodyF, velWorld);
            // Ground speed RELATIVE TO THE DECK — a spot on a 15 m/s ship is stationary deck-wise.
            const float relVel[3] = {velWorld[0] - over->vel[0], velWorld[1] - over->vel[1],
                                     velWorld[2] - over->vel[2]};
            const float relSpd = std::sqrt(relVel[0] * relVel[0] + relVel[2] * relVel[2]);

            // ── Deck carry: whatever sits on the deck travels with the ship. ──────────────────
            ns.pos_world[0] += static_cast<double>(over->vel[0]) * simDt;
            ns.pos_world[1] += static_cast<double>(over->vel[1]) * simDt;
            ns.pos_world[2] += static_cast<double>(over->vel[2]) * simDt;

            // ── Arrest wires: armed by the TOUCHDOWN EDGE inside the wire zone at trap speed. ──
            if (!ce.arrestEngaged && !ce.wasOnDeck && std::abs(local.x - deck.wireXM) <= deck.wireZoneM * 0.5f &&
                relSpd > 5.f && relSpd <= deck.maxTrapSpeedMps) {
                ce.arrestEngaged = true;
                constexpr float kWireRunoutM = 90.f;
                ce.arrestDecelMps2 = (relSpd * relSpd) / (2.f * kWireRunoutM);
                for (const auto& [pid, eid] : m_peerEntities) {
                    if (eid == ce.id) {
                        sendHapticTo(pid, static_cast<uint8_t>(HapticKind::Triggers), 0.9f, 0.9f, 300);
                        fl::atc::RadioTransmission tx;
                        tx.target = ce.id;
                        tx.speaker = "Paddles";
                        tx.text = "Paddles: good trap!";
                        tx.displaySeconds = 4;
                        sendRadioTransmission(tx);
                        break;
                    }
                }
            }
            if (ce.arrestEngaged) {
                const float horiz =
                    std::sqrt(float(ns.vel_body[0] * ns.vel_body[0]) + float(ns.vel_body[2] * ns.vel_body[2]));
                if (horiz <= 0.5f || relSpd <= 0.5f) {
                    ce.arrestEngaged = false; // stopped in the wires
                } else {
                    const float newSpd = std::max(0.f, horiz - ce.arrestDecelMps2 * static_cast<float>(simDt));
                    const float scale = (horiz > 1e-4f) ? newSpd / horiz : 0.f;
                    ns.vel_body[0] *= scale;
                    ns.vel_body[2] *= scale;
                }
            }

            // ── Catapult: stopped on the stroke at military power = hooked up and shot. ────────
            const bool onStroke = local.x >= deck.catStartXM - 5.f && local.x <= deck.catStartXM + deck.catStrokeM &&
                                  std::abs(local.z) <= 15.f;
            if (!ce.catapultEngaged && !ce.arrestEngaged && onStroke && relSpd < 10.f && ce.lastInput.throttle > 0.9f) {
                ce.catapultEngaged = true;
                ce.catapultRunM = 0.f;
            }
            if (ce.catapultEngaged) {
                // End speed honours the aircraft's own minimum (CarrierData::cat_min_m_s, #38).
                float vEnd = deck.catEndSpeedMps;
                if (const auto& carrier = ce.sim->flightModel().carrier)
                    vEnd = std::max(vEnd, carrier->cat_min_m_s);
                const float accel = (vEnd * vEnd) / (2.f * deck.catStrokeM);
                const float dv = accel * static_cast<float>(simDt);
                // Shove along the SHIP's forward axis (the stroke direction), in the world frame.
                float fwdBody[3] = {1.f, 0.f, 0.f};
                float fwdWorld[3];
                quatRotate(over->quat, fwdBody, fwdWorld);
                float nvWorld[3] = {velWorld[0] + fwdWorld[0] * dv, velWorld[1] + fwdWorld[1] * dv,
                                    velWorld[2] + fwdWorld[2] * dv};
                // Back into the aircraft body frame.
                const float qc[4] = {-ns.quat[0], -ns.quat[1], -ns.quat[2], ns.quat[3]};
                float nvBody[3];
                quatRotate(qc, nvWorld, nvBody);
                ns.vel_body[0] = nvBody[0];
                ns.vel_body[1] = nvBody[1];
                ns.vel_body[2] = nvBody[2];
                ce.catapultRunM += (relSpd + dv * 0.5f) * static_cast<float>(simDt);
                if (ce.catapultRunM >= deck.catStrokeM || relSpd >= vEnd) {
                    ce.catapultEngaged = false; // released — flying speed off the bow
                    for (const auto& [pid, eid] : m_peerEntities) {
                        if (eid == ce.id) {
                            sendHapticTo(pid, static_cast<uint8_t>(HapticKind::Rumble), 1.f, 0.6f, 400);
                            break;
                        }
                    }
                }
            }

            // Write the adjusted state back to the integrator AND the replicated transform.
            ce.sim->reset(ns);
            it.state->transform.pos[0] = ns.pos_world[0];
            it.state->transform.pos[1] = ns.pos_world[1];
            it.state->transform.pos[2] = ns.pos_world[2];
            float nvb[3] = {float(ns.vel_body[0]), float(ns.vel_body[1]), float(ns.vel_body[2])};
            float nvw[3];
            quatRotate(ns.quat, nvb, nvw);
            it.state->transform.vel[0] = nvw[0];
            it.state->transform.vel[1] = nvw[1];
            it.state->transform.vel[2] = nvw[2];
            ce.wasOnDeck = true;
        } else {
            ce.wasOnDeck = false;
            ce.catapultEngaged = false; // off the stroke = off the shuttle
            ce.arrestEngaged = false;   // airborne again (a bolter) drops the wire state
        }

        // ── LSO (#38): glideslope calls inside 3 nm on approach to a deck, ~4 s cadence, only to
        // aircraft a peer is flying (an AI wingman does not need Paddles in its ear). ────────────
        if (onDeck || tickIndex < ce.lsoNextTick)
            continue;
        bool isPeerAircraft = false;
        uint32_t ownerPeer = 0;
        for (const auto& [pid, eid] : m_peerEntities) {
            if (eid == ce.id) {
                isPeerAircraft = true;
                ownerPeer = pid;
                break;
            }
        }
        (void)ownerPeer;
        if (!isPeerAircraft)
            continue;
        for (const DeckRec& rec : m_decks) {
            // The wire-zone touchdown point in world space is the LSO's aim reference.
            const DeckDef& deck = *rec.deck;
            float wireLocal[3] = {deck.wireXM, deck.heightM, 0.f};
            float wireOff[3];
            quatRotate(rec.quat, wireLocal, wireOff);
            const double wire[3] = {rec.pos[0] + wireOff[0], rec.pos[1] + wireOff[1], rec.pos[2] + wireOff[2]};
            const double dx = wire[0] - fs.pos_world[0];
            const double dy = wire[1] - fs.pos_world[1];
            const double dz = wire[2] - fs.pos_world[2];
            const double range = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (range > kLsoRangeM || range < 200.0)
                continue;
            // Approaching? Closing speed along the line to the wires.
            const float closure = static_cast<float>(
                (it.state->transform.vel[0] * dx + it.state->transform.vel[1] * dy + it.state->transform.vel[2] * dz) /
                std::max(range, 1.0));
            if (closure < 20.f)
                continue;
            const double horiz = std::max(1.0, std::sqrt(dx * dx + dz * dz));
            const float pathDeg =
                static_cast<float>(std::atan2(ownAlt - rec.floorElevM, horiz)) * 180.f / std::numbers::pi_v<float>;
            const float gsErr = pathDeg - kGlideslopeDeg;
            const DeckLocalPoint lp = deckLocalPoint(fs.pos_world, rec.pos, rec.quat, deck);
            uint8_t phrase;
            if (range < 1200.0 && (gsErr > 3.f || std::abs(lp.z) > 40.f)) {
                phrase = kWaveOff;
            } else if (gsErr > 1.2f) {
                phrase = kHigh;
            } else if (gsErr < -1.0f) {
                phrase = kLow;
            } else if (const auto& carrier = ce.sim->flightModel().carrier) {
                const float spd =
                    std::sqrt(float(fs.vel_body[0] * fs.vel_body[0]) + float(fs.vel_body[1] * fs.vel_body[1]) +
                              float(fs.vel_body[2] * fs.vel_body[2]));
                phrase = (spd > carrier->approach_m_s + 8.f)   ? kFast
                         : (spd < carrier->approach_m_s - 8.f) ? kSlow
                                                               : kOnGlideslope;
            } else {
                phrase = kOnGlideslope;
            }
            if (phrase != ce.lsoLastPhrase || phrase == kWaveOff) {
                fl::atc::RadioTransmission tx;
                tx.target = ce.id;
                tx.speaker = "Paddles";
                tx.text = kLsoText[phrase];
                tx.displaySeconds = 4;
                sendRadioTransmission(tx);
                ce.lsoLastPhrase = phrase;
            }
            ce.lsoNextTick = tickIndex + 240; // ~4 s between looks
            break;
        }
    }
}

void WorldBroadcaster::handleBaseOpsCommand(uint32_t peerId, EntityId flight, std::string_view op) {
    // The crew chief answers on the radio like everyone else (#55) — routed through the same
    // RadioTransmission wire/subtitle path as ATC, never bespoke UI text.
    auto reply = [&](const char* text) {
        fl::atc::RadioTransmission tx;
        tx.target = flight;
        tx.speaker = "Crew chief";
        tx.text = text;
        tx.displaySeconds = 5;
        const MsgRadioTransmission w = buildRadioWire(tx, m_radioNets.indexOf("atc"));
        m_net.send(peerId, &w, sizeof(w), /*reliable=*/true);
    };

    const bool knownOp = (op == "refuel" || op == "rearm" || op == "repair");
    if (!knownOp) {
        reply("Crew chief: say again?");
        return;
    }
    EntityState* st = flight.valid() ? m_entityManager.get(flight) : nullptr;
    const auto ceIt = m_controlledEntities.find(flight.index);
    if (!st || st->dead || ceIt == m_controlledEntities.end()) {
        reply("Crew chief: you don't have an aircraft.");
        return;
    }
    ControlledEntity& ce = ceIt->second;
    const FlightState& fs = ce.sim->state();

    // SERVER-AUTHORITATIVE eligibility: shut down on the ground AT A BASE — an airfield ramp (the
    // injected proximity query; unset = any ground, the zero-pack sandbox) or a carrier deck.
    const glm::dvec3 pos{fs.pos_world[0], fs.pos_world[1], fs.pos_world[2]};
    float floor = m_groundQuery ? m_groundQuery(pos) : m_groundElevation.load(std::memory_order_relaxed);
    bool atBase = !m_baseProximityQuery || m_baseProximityQuery(pos);
    for (const DeckRec& rec : m_decks) {
        if (rec.entityIdx == flight.index)
            continue;
        const DeckLocalPoint lp = deckLocalPoint(fs.pos_world, rec.pos, rec.quat, *rec.deck);
        if (deckFloorApplies(lp, *rec.deck)) {
            floor = std::max(floor, static_cast<float>(rec.floorElevM));
            atBase = true; // a flight deck always has a crew
            break;
        }
    }
    const double agl = m_gravity->geodeticAltitude(fs.pos_world) - static_cast<double>(floor);
    const double spd =
        std::sqrt(fs.vel_body[0] * fs.vel_body[0] + fs.vel_body[1] * fs.vel_body[1] + fs.vel_body[2] * fs.vel_body[2]);
    if (agl > 2.0 || spd > 3.0) {
        reply("Crew chief: shut down on the ramp first.");
        return;
    }
    if (!atBase) {
        reply("Crew chief: nobody out here. Get to a base.");
        return;
    }

    const EntityDef* def = m_registry.byIndex(st->typeIndex);
    if (op == "refuel") {
        FlightState ns = fs;
        ns.fuel_kg = ce.sim->flightModel().geometry.fuel_kg;
        ns.mass_kg = ce.sim->flightModel().geometry.mass_kg + ns.fuel_kg;
        ce.sim->reset(ns);
        ce.fuelLeakKgS = 0.f;
        ce.sim->setFuelLeakRate(0.f);
        reply("Crew chief: fueled and topped off.");
    } else if (op == "rearm") {
        // Fresh full loadout — the same builder a spawn uses (#812), so a rearmed jet and a fresh
        // one cannot differ. Crewed aircraft partition per seat and are out of scope here.
        if (m_weaponRegistry && def && !def->hardpoints.empty() && def->crew.empty()) {
            ce.fire.loadout = buildLoadout(*def, *m_weaponRegistry);
            ce.payload = m_payloadResolver ? m_payloadResolver(*def) : PayloadEffect{};
        }
        if (def && (def->chaffCount > 0 || def->flareCount > 0))
            m_countermeasures.registerDispenser(flight.index, def->chaffCount, def->flareCount); // refill
        reply("Crew chief: rearmed, pins pulled.");
    } else { // repair
        st->hp = st->maxHp;
        st->damageLevel = DamageLevel::Intact;
        if (ce.hasSubsystems && def && def->damage && def->damage->subsystems)
            ce.subsystems.init(*def->damage->subsystems); // fresh pools
        ce.sim->setDamagePenalty(1.f, 1.f);
        ce.sim->setSubsystemControlFactor(1.f);
        ce.sim->setFuelLeakRate(0.f);
        ce.fuelLeakKgS = 0.f;
        ce.sim->setEngineFailFlags(0);
        reply("Crew chief: patched up. She'll fly.");
    }
}

void WorldBroadcaster::sendRadioTransmission(const fl::atc::RadioTransmission& tx) {
    const MsgRadioTransmission w = buildRadioWire(tx, m_radioNets.indexOf("atc"));
    if (tx.target.valid()) {
        for (const auto& [pid, eid] : m_peerEntities) {
            if (eid == tx.target) {
                m_net.send(pid, &w, sizeof(w), /*reliable=*/true); // unicast to the addressed pilot
                return;
            }
        }
    }
    // No owning peer (an AI flight's clearance) or an undirected line: every peer hears it.
    for (const auto& [pid, ps] : m_peerInputs) {
        (void)ps;
        m_net.send(pid, &w, sizeof(w), /*reliable=*/true);
    }
}

void WorldBroadcaster::handleRadioCommand(uint32_t peerId, const void* data, std::size_t size) {
    MsgRadioCommand msg;
    if (!readMsg(data, size, msg))
        return; // truncated; silently discard
    msg.command[sizeof(msg.command) - 1] = '\0';

    auto& ps = m_peerInputs[peerId];

    // Per-peer rate limit (~m_flightCmdRateLimit/s). Silently drop over the limit — a radio flood must
    // not be amplified back at the sender with a reply per rejected packet.
    {
        const auto now = m_clock->now();
        if (now - ps.radioCmdWindowStart >= std::chrono::seconds(1)) {
            ps.radioCmdWindowStart = now;
            ps.radioCmdCount = 0;
        }
        ++ps.radioCmdCount;
        if (ps.radioCmdCount > static_cast<uint32_t>(m_flightCmdRateLimit))
            return;
    }

    // The flight the command applies to = the requesting peer's own aircraft (invalid for an observer).
    const auto peerEnt = m_peerEntities.find(peerId);
    const fl::EntityId flight = (peerEnt != m_peerEntities.end()) ? peerEnt->second : fl::EntityId{};

    // Reply directly to the requester (facility-specific clearances come later from the ATC tick).
    auto reply = [&](fl::atc::AtcPhrase phrase, const char* overrideText = nullptr) {
        fl::atc::RadioTransmission tx = fl::atc::makeTransmission(phrase, "Tower", flight);
        if (overrideText)
            tx.text = overrideText;
        const MsgRadioTransmission w = buildRadioWire(tx, m_radioNets.indexOf("atc"));
        m_net.send(peerId, &w, sizeof(w), /*reliable=*/true);
    };

    // Tokenize "atc <subverb> [facility]" or "base <subverb>" (#55).
    std::string_view cmd(msg.command);
    auto nextToken = [](std::string_view& s) -> std::string_view {
        while (!s.empty() && s.front() == ' ')
            s.remove_prefix(1);
        std::size_t sp = s.find(' ');
        std::string_view tok = s.substr(0, sp);
        s.remove_prefix(sp == std::string_view::npos ? s.size() : sp);
        return tok;
    };
    const std::string_view verb = nextToken(cmd);

    // Base operations (#55): ground-crew services. Routed before the ATC gate — the crew chief
    // works whether or not a tower does.
    if (verb == "base") {
        handleBaseOpsCommand(peerId, flight, nextToken(cmd));
        return;
    }

    if (!m_atcService) {
        reply(fl::atc::AtcPhrase::Unable, "no ATC available");
        return;
    }

    if (verb != "atc") {
        reply(fl::atc::AtcPhrase::Unable, "say again");
        return;
    }
    const std::string_view sub = nextToken(cmd);
    const std::string facility(nextToken(cmd)); // optional; empty = nearest

    if (sub == "request_takeoff") {
        m_atcService->requestTakeoff(flight, facility);
        reply(fl::atc::AtcPhrase::Roger);
    } else if (sub == "request_landing") {
        m_atcService->requestLanding(flight, facility);
        reply(fl::atc::AtcPhrase::Roger);
    } else if (sub == "inbound") {
        m_atcService->declareInbound(flight, facility);
        reply(fl::atc::AtcPhrase::Roger);
    } else if (sub == "cancel") {
        m_atcService->cancel(flight);
        reply(fl::atc::AtcPhrase::Roger);
    } else {
        reply(fl::atc::AtcPhrase::Unable, "say again");
    }
}

namespace {
// Sanitize a chat line: keep only printable BMP codepoints (drop control chars, DEL, and any invalid /
// non-BMP sequence which nextUtf8Codepoint reports as U+FFFD), copying the ORIGINAL bytes of each kept
// codepoint so valid multi-byte sequences survive intact, truncate on a codepoint boundary, and trim
// surrounding whitespace.
std::string sanitizeChat(std::string_view in) {
    std::string out;
    const char* p = in.data();
    const char* const end = p + in.size();
    while (p < end && out.size() < fl::kMaxChatBytes) {
        const char* prev = p;
        const uint32_t cp = fl::nextUtf8Codepoint(p, end);
        if (cp < 0x20u || cp == 0x7Fu || cp == 0xFFFDu)
            continue; // control / DEL / invalid
        const std::size_t n = static_cast<std::size_t>(p - prev);
        if (out.size() + n > fl::kMaxChatBytes)
            break;
        out.append(prev, n);
    }
    const auto notSpace = [](char c) { return c != ' ' && c != '\t'; };
    out.erase(out.begin(), std::find_if(out.begin(), out.end(), notSpace));
    out.erase(std::find_if(out.rbegin(), out.rend(), notSpace).base(), out.end());
    return out;
}
} // namespace

void WorldBroadcaster::sendChatEvent(uint32_t peerId, uint8_t channel, uint32_t senderPeerId, std::string_view text) {
    MsgChatEventHeader hdr;
    hdr.channel = channel;
    hdr.senderPeerId = senderPeerId;
    std::vector<uint8_t> pkt;
    pkt.reserve(sizeof(hdr) + text.size() + 1);
    appendMsg(pkt, hdr);
    pkt.insert(pkt.end(), text.begin(), text.end());
    pkt.push_back('\0');
    m_net.send(peerId, pkt.data(), pkt.size(), /*reliable=*/true);
}

void WorldBroadcaster::handleChat(uint32_t peerId, const void* data, std::size_t size) {
    if (!m_chatEnabled)
        return;
    MsgChatHeader hdr;
    if (!readMsg(data, size, hdr))
        return; // truncated
    if (!isChatChannelOrdinal(hdr.channel))
        return;

    const auto pit = m_peerInputs.find(peerId);
    if (pit == m_peerInputs.end() || !pit->second.handshakeComplete)
        return; // not admitted
    auto& ps = pit->second;
    if (ps.chatMuted)
        return; // muted: drop silently, no rate-limit warning

    // Extract + sanitize the NUL-terminated text at offset 4.
    const char* bytes = static_cast<const char*>(data);
    std::string_view raw(bytes + sizeof(hdr), size > sizeof(hdr) ? size - sizeof(hdr) : 0u);
    if (const auto z = raw.find('\0'); z != std::string_view::npos)
        raw = raw.substr(0, z);
    const std::string text = sanitizeChat(raw);
    if (text.empty())
        return;

    // Per-peer rate limit (warn once per window; never one reply per rejected packet — a flood must not
    // be amplified back at the sender).
    {
        const auto now = m_clock->now();
        if (now - ps.chatWindowStart >= std::chrono::seconds(1)) {
            ps.chatWindowStart = now;
            ps.chatCount = 0;
            ps.chatRateLimitWarned = false;
        }
        ++ps.chatCount;
        if (ps.chatCount > static_cast<uint32_t>(m_chatRateLimit)) {
            if (!ps.chatRateLimitWarned) {
                ps.chatRateLimitWarned = true;
                sendNoticeTo(peerId, "You are sending chat too fast.");
            }
            return;
        }
    }

    // Moderation hook: false = suppress (fl-server default logs an audit line and allows).
    if (m_chatModerationHook && !m_chatModerationHook(peerId, hdr.channel, text))
        return;

    // Record what was actually said (#600) -- after the veto, so a suppressed line is absent from
    // the log rather than present-but-unsent, which would make the log disagree with the match.
    {
        MatchEvent me;
        me.tick = m_currentTick;
        me.type = MatchEventType::Chat;
        me.actor = peerId;
        me.channel = hdr.channel;
        me.factionIndex = factionForPeer(peerId);
        me.text = std::string(text);
        m_matchEventLog.append(std::move(me));
    }

    // Offer the line to the intent tier (#611) — after the veto and after the record, so a line that
    // was suppressed never reaches a model, and what a model saw is what the match log says was
    // said. Team channel only: the wingman answers to their flight, not to everyone in the server.
    if (m_chatIntentHook && static_cast<ChatChannel>(hdr.channel) == ChatChannel::Team)
        m_chatIntentHook(peerId, hdr.channel, text);

    const auto channel = static_cast<ChatChannel>(hdr.channel);
    const uint16_t senderFaction = (channel == ChatChannel::Team) ? factionForPeer(peerId) : kNoFaction;
    for (const auto& [pid, pin] : m_peerInputs) {
        if (!pin.handshakeComplete)
            continue;
        if (channel == ChatChannel::Team) {
            // Team channel: same faction only. A teamless sender (observer) sees only its own echo.
            if (senderFaction == kNoFaction) {
                if (pid != peerId)
                    continue;
            } else if (factionForPeer(pid) != senderFaction) {
                continue;
            }
        }
        sendChatEvent(pid, hdr.channel, peerId, text);
    }
}

bool WorldBroadcaster::setPeerMuted(uint32_t peerId, bool muted) {
    const auto it = m_peerInputs.find(peerId);
    if (it == m_peerInputs.end())
        return false;
    it->second.chatMuted = muted;
    return true;
}

bool WorldBroadcaster::isPeerMuted(uint32_t peerId) const {
    const auto it = m_peerInputs.find(peerId);
    return it != m_peerInputs.end() && it->second.chatMuted;
}

// ---------------------------------------------------------------------------------------------
// Voice comms (#532)
// ---------------------------------------------------------------------------------------------
// The server's entire involvement with audio is: check the sender may talk on this net, work out
// who is on it, and copy the bytes. It never decodes a frame — which is what makes voice for 128
// players cost the server almost nothing, and what lets the codec change without a protocol change.

void WorldBroadcaster::setRadioNets(RadioNetTable nets) {
    m_radioNets = std::move(nets);
    if (m_radioNets.empty()) {
        // A server that configures no nets still gets a working radio rather than silent voice.
        for (auto& def : builtinRadioNets())
            m_radioNets.add(def);
    }
}

bool WorldBroadcaster::setPeerVoiceMuted(uint32_t peerId, bool muted) {
    const auto it = m_peerInputs.find(peerId);
    if (it == m_peerInputs.end())
        return false;
    it->second.voiceMuted = muted;
    return true;
}

bool WorldBroadcaster::isPeerVoiceMuted(uint32_t peerId) const {
    const auto it = m_peerInputs.find(peerId);
    return it != m_peerInputs.end() && it->second.voiceMuted;
}

std::vector<uint32_t> WorldBroadcaster::voiceMutedPeers() const {
    std::vector<uint32_t> out;
    for (const auto& [pid, ps] : m_peerInputs) {
        if (ps.voiceMuted)
            out.push_back(pid);
    }
    std::sort(out.begin(), out.end());
    return out;
}

VoicePeerView WorldBroadcaster::voicePeerView(uint32_t peerId) const {
    VoicePeerView v;
    v.peerId = peerId;
    const auto pit = m_peerInputs.find(peerId);
    if (pit == m_peerInputs.end())
        return v;
    v.admitted = pit->second.handshakeComplete;
    v.voiceMuted = pit->second.voiceMuted;

    const auto eit = m_peerEntities.find(peerId);
    if (eit == m_peerEntities.end())
        return v; // observer / not yet spawned: admitted, but with no team, flight or position
    if (const EntityState* st = m_entityManager.get(eit->second)) {
        // Faction 0 is NEUTRAL, not "teamless" — matching handleChat, where neutral players form
        // their own team. Only the kNoFaction sentinel (no entity at all) means teamless.
        v.faction = st->factionIndex;
        v.hasPosition = true;
        v.pos[0] = st->transform.pos[0];
        v.pos[1] = st->transform.pos[1];
        v.pos[2] = st->transform.pos[2];
    }
    // "My flight" is the formation holding this aircraft as a member, or — for a flight lead, who is
    // the ANCHOR rather than a member — the formation anchored on it. Missing either reading would
    // silently exclude every lead from their own flight net.
    fl::FormationId fid = m_formations.formationOfEntity(eit->second);
    if (fid == fl::kNoFormation)
        fid = m_formations.formationAnchoredOn(eit->second);
    v.formationId = fid;
    return v;
}

void WorldBroadcaster::buildVoicePeerViews(std::vector<VoicePeerView>& out) const {
    out.clear();
    out.reserve(m_peerInputs.size());
    for (const auto& [pid, ps] : m_peerInputs) {
        if (!ps.handshakeComplete)
            continue;
        out.push_back(voicePeerView(pid));
    }
}

void WorldBroadcaster::sendVoiceNetDefs(uint32_t peerId) {
    std::vector<uint8_t> buf;
    MsgVoiceNetDefHeader hdr;
    const bool on = m_voiceEnabled && !m_radioNets.empty();
    hdr.flags = on ? kVoiceServerEnabled : 0u;
    hdr.netCount = on ? static_cast<uint8_t>(m_radioNets.size()) : 0u;
    buf.reserve(sizeof(hdr) + (on ? m_radioNets.size() * sizeof(MsgVoiceNetRecord) : 0u));
    appendMsg(buf, hdr);
    if (on) {
        for (std::size_t i = 0; i < m_radioNets.size(); ++i) {
            const RadioNetDef& def = m_radioNets.nets()[i];
            MsgVoiceNetRecord rec{};
            rec.netId = static_cast<uint8_t>(i);
            rec.kind = static_cast<uint8_t>(def.kind);
            rec.flags = static_cast<uint8_t>((def.positional ? kVoiceNetFlagPositional : 0u) |
                                             (def.radioEffect ? kVoiceNetFlagRadioEffect : 0u) |
                                             (def.defaultNet ? kVoiceNetFlagDefault : 0u));
            rec.rangeM = def.rangeM;
            rec.gain = def.gain;
            std::snprintf(rec.id, sizeof(rec.id), "%s", def.id.c_str());
            std::snprintf(rec.name, sizeof(rec.name), "%s", def.name.c_str());
            appendMsg(buf, rec);
        }
    }
    // Reliable: a client that never learns the table cannot key a mic at all, and the table is one
    // packet per connect.
    m_net.send(peerId, buf.data(), buf.size(), /*reliable=*/true);
}

void WorldBroadcaster::handleVoiceFrame(uint32_t peerId, const void* data, std::size_t size) {
    if (!m_voiceEnabled)
        return;
    MsgVoiceFrameHeader hdr;
    if (!readMsg(data, size, hdr))
        return; // truncated

    const auto pit = m_peerInputs.find(peerId);
    if (pit == m_peerInputs.end() || !pit->second.handshakeComplete)
        return; // not admitted
    auto& ps = pit->second;

    // Length validation is the ONLY thing we can do to a payload nobody on this machine will ever
    // decode; do it before anything else touches the bytes.
    const std::size_t avail = size > sizeof(hdr) ? size - sizeof(hdr) : 0u;
    const std::size_t payloadBytes = std::min<std::size_t>(hdr.payloadBytes, avail);
    if (hdr.payloadBytes > kMaxVoiceFrameBytes || payloadBytes != hdr.payloadBytes)
        return;

    // Bandwidth bound (see PeerInputState). Dropped silently: a reply to a flood is amplification.
    {
        const auto now = m_clock->now();
        if (now - ps.voiceWindowStart >= std::chrono::seconds(1)) {
            ps.voiceWindowStart = now;
            ps.voiceFrameCount = 0;
        }
        ++ps.voiceFrameCount;
        if (ps.voiceFrameCount > static_cast<uint32_t>(m_voiceFrameRateLimit))
            return;
    }

    const VoicePeerView sender = voicePeerView(peerId);
    buildVoicePeerViews(m_voicePeerScratch);
    if (!selectVoiceRecipients(m_radioNets, hdr.netId, sender, m_voicePeerScratch, m_voiceRecipientScratch))
        return; // unknown net, muted, or no membership on this net
    if (m_voiceRecipientScratch.empty())
        return; // nobody on the net — a common and completely normal case

    MsgVoiceRelayHeader rel;
    rel.netId = hdr.netId;
    rel.seq = hdr.seq;
    rel.flags = hdr.flags;
    rel.senderPeerId = peerId;
    rel.payloadBytes = static_cast<uint16_t>(payloadBytes);
    rel.senderEntityIdx = kNoVoiceEntity;
    if (const auto eit = m_peerEntities.find(peerId); eit != m_peerEntities.end())
        rel.senderEntityIdx = eit->second.index;

    m_voiceRelayScratch.clear();
    m_voiceRelayScratch.reserve(sizeof(rel) + payloadBytes);
    appendMsg(m_voiceRelayScratch, rel);
    const auto* bytes = static_cast<const uint8_t*>(data) + sizeof(hdr);
    m_voiceRelayScratch.insert(m_voiceRelayScratch.end(), bytes, bytes + payloadBytes);

    // Unreliable, on the dedicated voice channel: a lost frame is 20 ms the receiver conceals, and
    // retransmitting it would deliver it after the moment it belonged to. The dedicated channel
    // keeps ENet's per-channel unreliable sequencing from making voice and snapshots drop each
    // other (see kNetChVoice).
    for (const uint32_t rid : m_voiceRecipientScratch)
        m_net.sendChannel(rid, m_voiceRelayScratch.data(), m_voiceRelayScratch.size(), /*reliable=*/false, kNetChVoice);
}

std::vector<uint32_t> WorldBroadcaster::mutedPeers() const {
    std::vector<uint32_t> out;
    for (const auto& [pid, ps] : m_peerInputs)
        if (ps.chatMuted)
            out.push_back(pid);
    std::sort(out.begin(), out.end());
    return out;
}

bool WorldBroadcaster::setSpectateTarget(uint32_t peerId, uint32_t entityIdx) {
    const auto it = m_peerInputs.find(peerId);
    if (it == m_peerInputs.end())
        return false;
    it->second.spectateTargetIdx = entityIdx;
    return true;
}

void WorldBroadcaster::enqueueDelayedSnapshot(PeerInputState& pin, uint64_t dueTick,
                                              const std::vector<uint8_t>& payload) {
    constexpr std::size_t kMaxDelayBytes = 4u * 1024u * 1024u; // 4 MB/peer
    while (!pin.snapshotDelayQueue.empty() && pin.snapshotDelayBytes + payload.size() > kMaxDelayBytes) {
        pin.snapshotDelayBytes -= pin.snapshotDelayQueue.front().second.size();
        pin.snapshotDelayQueue.pop_front();
        if (!pin.snapshotDelayEvicted) {
            pin.snapshotDelayEvicted = true;
            m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                         "spectate delay buffer full (4 MB); dropping oldest snapshot for a peer");
        }
    }
    pin.snapshotDelayQueue.emplace_back(dueTick, payload);
    pin.snapshotDelayBytes += payload.size();
}

void WorldBroadcaster::handleWingmanCommand(uint32_t peerId, const void* data, std::size_t size) {
    MsgWingmanCommand msg;
    if (!readMsg(data, size, msg))
        return; // truncated; silently discard

    if (msg.protocolVersion != kProtocolVersion) {
        char vmsg[96];
        std::snprintf(vmsg, sizeof(vmsg), "peer %u: WingmanCommand version mismatch (got %u, want %u) — discarding",
                      peerId, static_cast<unsigned>(msg.protocolVersion), static_cast<unsigned>(kProtocolVersion));
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__, vmsg);
        return;
    }

    // The order channel is off (no handler wired): discard rather than acking, so a server without
    // the feature is indistinguishable from one that never received the packet.
    if (!m_flightOrderHandler)
        return;

    auto& ps = m_peerInputs[peerId];

    // Dup/reorder guard. Reliable delivery is ordered, but a reconnecting client restarts its
    // counter, so accept the first packet unconditionally (hasSeq mirrors the MsgClientInput path).
    if (ps.hasWingmanSeq && !isNewerSeq(msg.seqNum, ps.lastWingmanSeq))
        return;
    ps.hasWingmanSeq = true;
    ps.lastWingmanSeq = msg.seqNum;

    // Per-peer order rate limit. Acked ONCE per window, never per packet: an ack for every rejected
    // packet would turn a flood into an amplifier pointed back at the sender.
    {
        const auto now = m_clock->now();
        if (now - ps.wingmanCmdWindowStart >= std::chrono::seconds(1)) {
            ps.wingmanCmdWindowStart = now;
            ps.wingmanCmdCount = 0;
            ps.wingmanRateLimitAcked = false;
        }
        ++ps.wingmanCmdCount;
        if (ps.wingmanCmdCount > static_cast<uint32_t>(m_flightCmdRateLimit)) {
            if (!ps.wingmanRateLimitAcked) {
                ps.wingmanRateLimitAcked = true;
                sendWingmanAck(peerId, msg.command, WingmanResult::RateLimited, msg.flightId, 0, kFlightAll, kNoTarget);
            }
            return;
        }
    }

    issueWingmanOrder(peerId, msg.command, msg.flightId, msg.memberIdx, (msg.flags & kFlightFlagCascade) != 0);
}

// The order path itself, reached identically by the wire (MsgWingmanCommand above), by the radio
// menu that produces it, and by the #611 chat-to-intent bridge. That the bridge lands HERE rather
// than in a lookalike of it is what "the LLM chooses among validated commands, and execution goes
// through the scripted grammar" actually means in code.
WingmanResult WorldBroadcaster::issueWingmanOrder(uint32_t peerId, uint8_t command, uint16_t flightId,
                                                  uint32_t memberIdx, bool cascade) {
    if (!m_flightOrderHandler)
        return WingmanResult::Rejected;
    auto& ps = m_peerInputs[peerId];

    // Resolve the addressed formation. kOwnFlight = "the one I command" — the common case, so a
    // pilot who leads a single flight never has to know its id. A commander of several (an AWACS, a
    // package commander) MUST name one, because "my flight" is then ambiguous: refuse rather than
    // guess which of their formations they meant.
    fl::FormationId fid = flightId;
    if (fid == kOwnFlight) {
        const std::vector<fl::FormationId> mine = m_formations.commandedBy(peerId);
        if (mine.size() != 1) {
            sendWingmanAck(peerId, command, WingmanResult::NoFlight, kNoFlightId, 0, kFlightAll, kNoTarget);
            return WingmanResult::NoFlight;
        }
        fid = mine.front();
    }

    const fl::Formation* formation = m_formations.get(fid);

    // AUTHORITY. `commands()` walks UP the parent chain, so a package commander may order a flight
    // inside their package without being that flight's own commander. A peer that does not command
    // it gets NoFlight — deliberately the same code an unknown formation returns, so the order
    // channel cannot be used to enumerate which formations exist or who leads them.
    if (!formation || !m_formations.commands(peerId, fid)) {
        sendWingmanAck(peerId, command, WingmanResult::NoFlight, kNoFlightId, 0, kFlightAll, kNoTarget);
        return WingmanResult::NoFlight;
    }

    if (!fl::ai::isWingmanCommandOrdinal(command)) {
        sendWingmanAck(peerId, command, WingmanResult::Rejected, fid, 0, kFlightAll, kNoTarget);
        return WingmanResult::Rejected;
    }
    const auto cmd = static_cast<fl::ai::WingmanCommand>(command);

    // Designate a target for attack_my_target from the COMMANDER's own boresight — state the server
    // already owns (PeerInputState::viewAxis, refreshed at 60 Hz from MsgClientInput). Nothing in the
    // cone means the order is REFUSED and behavior is unchanged: an attack order that quietly picks
    // its own target is worse than one that declines.
    EntityId designated{};
    if (cmd == fl::ai::WingmanCommand::AttackMyTarget) {
        const auto peerEnt = m_peerEntities.find(peerId);
        const EntityState* commander = peerEnt != m_peerEntities.end() ? m_entityManager.get(peerEnt->second) : nullptr;
        if (commander && m_targetDesignator) {
            designated = m_targetDesignator(*commander, ps.viewAxis);
        }
        if (!designated.valid()) {
            const auto liveNow = static_cast<uint8_t>(std::min<std::size_t>(formation->members.size(), 255));
            sendWingmanAck(peerId, command, WingmanResult::NoTarget, fid, liveNow, memberIdx, kNoTarget);
            return WingmanResult::NoTarget;
        }
    }

    const auto callerEnt = m_peerEntities.find(peerId);
    const uint32_t callerIdx = callerEnt != m_peerEntities.end() ? callerEnt->second.index : kFlightAll;

    const FlightOrderReport rep = dispatchOrder(fid, command, memberIdx, cascade, designated, peerId, callerIdx);

    // Fold the per-member outcomes into one answer for the commander.
    WingmanResult result = WingmanResult::NoFlight;
    if (rep.aiRetasked > 0) {
        result = WingmanResult::Acknowledged;
    } else if (rep.humansRelayed > 0) {
        // Every addressed member was human: the call went out, but no aircraft was retasked. Say so,
        // rather than letting the commander believe a machine is now obeying.
        result = WingmanResult::Relayed;
    } else if (rep.deadSkipped > 0) {
        result = WingmanResult::Unavailable;
    }

    const auto liveMembers = static_cast<uint8_t>(std::min(rep.aiRetasked + rep.humansRelayed, 255));
    sendWingmanAck(peerId, command, result, fid, liveMembers, memberIdx,
                   designated.valid() ? designated.index : kNoTarget);
    return result;
}

WorldBroadcaster::FlightOrderReport WorldBroadcaster::applyFlightOrder(fl::FormationId fid, uint8_t command,
                                                                       uint32_t memberIdx, bool cascade,
                                                                       EntityId designatedTarget) {
    // kNoPeer = the game master: no peer-authority check (the console is authorized by the operator
    // password, not by commanding the formation), and relays carry no caller entity because the GM is
    // not flying one. Note this is NOT peer 0 — peer 0 is an ordinary player.
    return dispatchOrder(fid, command, memberIdx, cascade, designatedTarget, /*callerPeerId=*/fl::kNoPeer,
                         /*callerEntityIdx=*/kFlightAll);
}

WorldBroadcaster::FlightOrderReport WorldBroadcaster::dispatchOrder(fl::FormationId fid, uint8_t command,
                                                                    uint32_t memberIdx, bool cascade,
                                                                    EntityId designatedTarget, uint32_t callerPeerId,
                                                                    uint32_t callerEntityIdx) {
    FlightOrderReport rep{};
    if (!m_flightOrderHandler || !fl::ai::isWingmanCommandOrdinal(command))
        return rep;
    const auto cmd = static_cast<fl::ai::WingmanCommand>(command);

    // The formations an order reaches: just this one, or its whole subtree when cascading (a package
    // commander sending the whole package home, rather than each flight in turn).
    const std::vector<fl::FormationId> targets =
        cascade ? m_formations.subtree(fid) : std::vector<fl::FormationId>{fid};

    for (const fl::FormationId tid : targets) {
        const fl::Formation* f = m_formations.get(tid);
        if (!f)
            continue;

        // Copy the member list: the order handler retasks controllers, and a handler that spawns or
        // kills would otherwise invalidate the vector we are walking.
        const std::vector<fl::FormationMember> members = f->members;
        const auto flightSize = static_cast<uint8_t>(std::min<std::size_t>(members.size(), 255));

        for (const fl::FormationMember& m : members) {
            if (memberIdx != kFlightAll && m.id.index != memberIdx)
                continue;

            const EntityState* ms = m_entityManager.get(m.id);
            if (!ms || ms->dead) {
                ++rep.deadSkipped;
                continue;
            }

            if (m.isAi()) {
                // The server owns this aircraft: retask its controller.
                if (m_flightOrderHandler(*f, m, command, designatedTarget)) {
                    ++rep.aiRetasked;
                    // Record the weapons hold. It has no teeth until weapons land (#583) - this is
                    // where the firing trigger will read it - but the order is stored rather than
                    // dropped on the floor.
                    if (fl::Formation* mut = m_formations.get(tid)) {
                        for (fl::FormationMember& mm : mut->members) {
                            if (mm.id == m.id) {
                                if (cmd == fl::ai::WingmanCommand::HoldFire) {
                                    mm.weaponsHold = true;
                                } else if (fl::ai::clearsWeaponsHold(cmd)) {
                                    mm.weaponsHold = false; // an engage order implies weapons free
                                }
                            }
                        }
                    }
                }
            } else if (m.peerId != callerPeerId) {
                // A HUMAN member. The server cannot retask a person, and pretending otherwise would
                // be the dishonest design: relay the call to their client and let them decide.
                // memberIdx carries the CALLER's entity, so the recipient knows who is ordering them.
                sendWingmanAck(m.peerId, command, WingmanResult::Relayed, tid, flightSize, callerEntityIdx,
                               designatedTarget.valid() ? designatedTarget.index : kNoTarget);
                ++rep.humansRelayed;
            }
        }
    }
    return rep;
}

void WorldBroadcaster::sendAdminResponse(INetwork& net, uint32_t peerId, uint16_t reqId, const std::string& result) {
    if (result.size() <= kAdminResponseFastPathMax) {
        MsgAdminResponse resp{};
        resp.reqId = reqId;
        std::memcpy(resp.text, result.c_str(), result.size());
        resp.text[result.size()] = '\0';
        net.send(peerId, &resp, sizeof(resp), /*reliable=*/true);
        return;
    }
    uint16_t seq = 0;
    std::size_t offset = 0;
    while (offset < result.size()) {
        MsgAdminResponseChunk chunk{};
        chunk.reqId = reqId;
        chunk.seqNum = seq++;
        std::size_t n = std::min(result.size() - offset, kAdminChunkPayload);
        std::memcpy(chunk.body, result.data() + offset, n);
        chunk.body[n] = '\0';
        offset += n;
        if (offset >= result.size())
            chunk.flags = kChunkFlagEnd;
        net.send(peerId, &chunk, sizeof(chunk), /*reliable=*/true);
    }
}

std::shared_ptr<const FlightModelData> WorldBroadcaster::resolveFlightModel(EntityId id) {
    const EntityState* st = m_entityManager.get(id);
    if (!st)
        return nullptr;
    const EntityDef* def = m_registry.byIndex(st->typeIndex);
    if (!def || def->flightModelAsset.empty() || !m_flightModelResolver)
        return nullptr;
    std::shared_ptr<const FlightModelData> model = m_flightModelResolver(def->flightModelAsset);
    if (!model) {
        char wmsg[160];
        std::snprintf(wmsg, sizeof(wmsg), "flight model '%s' not found -- using builtin model",
                      def->flightModelAsset.c_str());
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__, wmsg);
    }
    return model;
}

void WorldBroadcaster::addControlledEntity(EntityId id, std::unique_ptr<IEntityController> controller,
                                           std::shared_ptr<const FlightModelData> model, float initialThrottle,
                                           bool decimatable, float initialAirspeed) {
    const EntityState* st = m_entityManager.get(id);
    if (!st)
        return;
    if (!model)
        model = BuiltinFlightModel::get();

    FlightState fs{};
    fs.pos_world[0] = st->transform.pos[0];
    fs.pos_world[1] = st->transform.pos[1];
    fs.pos_world[2] = st->transform.pos[2];
    // Seed the integrator orientation from the spawn transform. It was left at identity, so the mission
    // heading baked into the transform quaternion was dropped on the first tick (#883) — an airborne
    // spawn faced the wrong way AND, with zero body velocity below, tumbled out of controlled flight.
    fs.quat[0] = st->transform.quat[0];
    fs.quat[1] = st->transform.quat[1];
    fs.quat[2] = st->transform.quat[2];
    fs.quat[3] = st->transform.quat[3];
    // Initial airspeed along body-forward (+X), so an airborne spawn is in stable, controllable flight
    // at t=0 (#883). kAutoSpawnAirspeed picks a sane cruise default; a ground start passes 0 (parked,
    // held static by the integrator's parking hold). Purely body-forward, so it needs no world->body
    // rotation — the heading rides on fs.quat above.
    fs.vel_body[0] = (initialAirspeed < 0.f) ? kDefaultSpawnAirspeedMps : initialAirspeed;
    // Gear configuration at spawn (#639): DOWN for a parked start (airspeed 0), UP for an airborne
    // one. An aircraft is parked on its wheels, and since #639 the wheels are what carry brakes,
    // tyre grip and nosewheel steering — a ground spawn with the gear stowed would belly-scrape on
    // the runway. Anything that disagrees simply commands the other position and the actuator travels
    // there: an AI, which never touches the gear switch, retracts within its transit window.
    fs.articulation.gear = (initialAirspeed == 0.f) ? 1.f : 0.f;
    fs.fuel_kg = model->geometry.fuel_kg;
    fs.mass_kg = model->geometry.mass_kg + fs.fuel_kg;
    fs.throttle_actual = initialThrottle;

    auto fi = std::make_unique<FlightIntegrator>(model);
    fi->setGravityField(*m_gravity);
    fi->setEarthRotationRate(m_earthRotationRate); // Coriolis/centrifugal in the Earth-fixed frame (#482)
    // Bind the role's force model (ballistic boost/coast, multirotor thrust mixing, ...). ONE
    // shared seam with ClientPrediction (#349) — see flight/ForceModelSelect.h for why two
    // hand-maintained copies would be a per-tick prediction divergence.
    applyForceModelFor(*fi, *model);
    fi->reset(fs);
    // Give the controller the world's planet radius so its local-level (tangent-plane) guidance is
    // correct far from the origin. Every controller — peer or AI — enters through this single path.
    if (controller)
        controller->setPlanetRadius(static_cast<double>(m_planetRadiusKm) * 1000.0);
    ControlledEntity ce{id, std::move(fi), std::move(controller)};
    ce.decimatable = decimatable;

    // What the default loadout costs this airframe (#812), resolved ONCE here and carried for the
    // life of the entity. NOT folded into fs.mass_kg above: FlightIntegrator::step adds
    // payload.extra_mass_kg to the effective mass on every tick, so doing both would count the
    // stores twice.
    const EntityDef* spawnDef = m_registry.byIndex(st->typeIndex);
    if (m_payloadResolver && spawnDef)
        ce.payload = m_payloadResolver(*spawnDef);

    // Live stations (#625). Born from the same default loadout the payload above describes; from
    // here on the LOADOUT is the truth — a released store shrinks both, in evaluateFire.
    //
    // A CREWED aircraft (#969) instead partitions the loadout across its seats (buildCrew): each
    // seat owns its stations' ammo exclusively (the one-owner invariant), so ce.fire stays empty and
    // the crewed weapons pass drives per-seat fire. A plain fighter takes the single-seat path.
    if (m_weaponRegistry && spawnDef && !spawnDef->crew.empty())
        buildCrew(ce, *spawnDef);
    else if (m_weaponRegistry && spawnDef && !spawnDef->hardpoints.empty())
        ce.fire.loadout = buildLoadout(*spawnDef, *m_weaponRegistry);

    // Per-subsystem damage pools (#675), born from the def's [damage.subsystems]. Absent = the
    // 3-level tier model is the whole story (hasSubsystems stays false, routing is a no-op).
    if (spawnDef && spawnDef->damage && spawnDef->damage->subsystems) {
        ce.subsystems.init(*spawnDef->damage->subsystems);
        ce.hasSubsystems = true;
    }

    m_controlledEntities[id.index] = std::move(ce);

    // Every controlled entity is an observer — PLAYERS INCLUDED. Player avionics (#526) inherits
    // exactly these semantics rather than growing a parallel "what can the player see" path, which is
    // the whole point of one vocabulary with three consumers.
    const EntityDef* def = m_registry.byIndex(st->typeIndex);
    const std::vector<std::string> sensorIds = def ? def->sensorIds : std::vector<std::string>{};
    const AiTuning tuning = (def && def->aiTuning) ? *def->aiTuning : AiTuning{};
    m_sensorSystem.addObserver(id.index, sensorIds, tuning.skill, tuning.reaction);

    // Countermeasure magazines (#529): register the dispenser from the def's chaff/flare counts (0 =
    // no dispenser of that kind, which is the default — most entities carry none).
    if (def && (def->chaffCount > 0 || def->flareCount > 0))
        m_countermeasures.registerDispenser(id.index, def->chaffCount, def->flareCount);

    // The candidate query is widened by the loudest signature in the registry, which can only change
    // as types are registered. Recomputing here is cheap (types are registered at startup) and keeps
    // it correct without a callback into the registry.
    m_sensorSystem.recomputeSignatureScale();
}

void WorldBroadcaster::buildCrew(ControlledEntity& ce, const EntityDef& def) {
    ce.crew.seats.clear();
    ce.crew.turrets.clear();

    // Turrets first so a seat can index them; convert the authored degrees/quat to the servo's radians.
    for (const TurretDef& td : def.turrets) {
        CrewTurret t;
        t.limits.azMinRad = glm::radians(td.azMinDeg);
        t.limits.azMaxRad = glm::radians(td.azMaxDeg);
        t.limits.elMinRad = glm::radians(td.elMinDeg);
        t.limits.elMaxRad = glm::radians(td.elMaxDeg);
        t.limits.slewRateRadS = glm::radians(td.slewRateDegS);
        t.mountRest = glm::quat{td.mountOrient[3], td.mountOrient[0], td.mountOrient[1], td.mountOrient[2]};
        t.stations.assign(td.stations.begin(), td.stations.end());
        ce.crew.turrets.push_back(std::move(t));
    }
    auto turretIndexById = [&](const std::string& id) -> int {
        for (std::size_t i = 0; i < def.turrets.size(); ++i)
            if (def.turrets[i].id == id)
                return static_cast<int>(i);
        return -1;
    };

    const double planetRadiusM = static_cast<double>(m_planetRadiusKm) * 1000.0;
    float seatMassSum = 0.f;
    float seatCd0Sum = 0.f;
    for (std::size_t s = 0; s < def.crew.size(); ++s) {
        const SeatDef& sd = def.crew[s];
        CrewSeat seat;
        seat.capabilities = sd.capabilities;
        seat.isFlySeat = hasCapability(sd.capabilities, CrewCapability::Fly);
        seat.skill = sd.defaultSkill;
        seat.reaction = def.aiTuning ? def.aiTuning->reaction : 0.5f; // per-instance reaction refined in #971
        seat.turretIndex = sd.turret.empty() ? -1 : turretIndexById(sd.turret);
        seat.botOccupied = (sd.defaultOccupancy == SeatOccupancyDefault::Bot);
        seat.maxHp = sd.damageHp; // #978: 0 = non-damageable seat
        seat.hp = sd.damageHp;
        seat.hitWeight = sd.hitWeight;

        // Fire seats get a loadout PARTITION — their own stations plus, if they aim a turret, the
        // turret's mounted stations. Disjoint across seats by the one-owner invariant.
        if (m_weaponRegistry && hasCapability(sd.capabilities, CrewCapability::Fire)) {
            std::vector<int> slots = sd.stations;
            if (seat.turretIndex >= 0) {
                const auto& ts = def.turrets[static_cast<std::size_t>(seat.turretIndex)].stations;
                slots.insert(slots.end(), ts.begin(), ts.end());
            }
            seat.fire.loadout = buildSeatLoadout(def, *m_weaponRegistry, slots);
            seatMassSum += seat.fire.loadout.payloadMassKg;
            seatCd0Sum += seat.fire.loadout.payloadCd0;
        }

        // A non-fly bot seat gets its ISeatController from the injected factory (#971 supplies the
        // gunner). The Fly seat flies via ce.controller and never gets a seatBot. The bot context (#976)
        // starts at the seat's authored skill (mission seed 0); a mission crew: block overrides it in
        // applyCrewSpawnConfig.
        if (!seat.isFlySeat && seat.botOccupied && m_seatControllerFactory) {
            const SeatBotContext botCtx{sd.defaultSkill, sd.defaultSkill, 0};
            seat.seatBot = m_seatControllerFactory(sd, static_cast<uint8_t>(s), botCtx);
            if (seat.seatBot)
                seat.seatBot->setPlanetRadius(planetRadiusM);
        }
        ce.crew.seats.push_back(std::move(seat));
    }

    // Payload of hardpoints owned by NO seat (inert drop tanks a pilot never fires): the airframe
    // total = base + sum over seats. ce.payload was resolved to the full default just above.
    ce.crew.baseMassKg = std::max(0.f, ce.payload.extra_mass_kg - seatMassSum);
    ce.crew.baseCd0 = std::max(0.f, ce.payload.extra_cd0 - seatCd0Sum);
}

void WorldBroadcaster::applyCrewSpawnConfig(EntityId id, const CrewSpawnConfig& cfg) {
    auto cit = m_controlledEntities.find(id.index);
    if (cit == m_controlledEntities.end() || cit->second.id != id || !cit->second.crew.crewed())
        return;
    ControlledEntity& ce = cit->second;
    const EntityState* st = m_entityManager.get(id);
    const EntityDef* def = st ? m_registry.byIndex(st->typeIndex) : nullptr;
    if (!def)
        return;
    const double planetRadiusM = static_cast<double>(m_planetRadiusKm) * 1000.0;

    for (std::size_t s = 0; s < ce.crew.seats.size() && s < def->crew.size(); ++s) {
        CrewSeat& seat = ce.crew.seats[s];
        if (seat.isFlySeat || seat.occupantPeer != kNoSeatPeer)
            continue; // the Fly seat is not a bot seat; a human occupant is not overridden by mission config

        // Effective skill: a per-seat override wins, else the aircraft-level range, else the authored
        // default. Effective occupancy + bot spec: a per-seat override wins.
        float skillMin = seat.skill, skillMax = seat.skill;
        if (cfg.skillMin && cfg.skillMax) {
            skillMin = *cfg.skillMin;
            skillMax = *cfg.skillMax;
        }
        SeatDef effDef = def->crew[s]; // a copy we may override before re-building the bot
        bool wantBot = seat.botOccupied;

        for (const CrewSeatSpawnOverride& ov : cfg.seats) {
            if (ov.seatIndex != static_cast<uint8_t>(s))
                continue;
            if (ov.skillMin && ov.skillMax) {
                skillMin = *ov.skillMin;
                skillMax = *ov.skillMax;
            }
            if (ov.botSpec)
                effDef.botSpec = *ov.botSpec;
            if (ov.empty)
                wantBot = !*ov.empty;
            break;
        }

        seat.skill = skillMin; // the roll refines within [min,max]; store the floor as the seat's baseline
        seat.botOccupied = wantBot;
        if (wantBot && m_seatControllerFactory) {
            const SeatBotContext botCtx{skillMin, skillMax, cfg.missionSeed};
            seat.seatBot = m_seatControllerFactory(effDef, static_cast<uint8_t>(s), botCtx);
            if (seat.seatBot)
                seat.seatBot->setPlanetRadius(planetRadiusM);
        } else {
            seat.seatBot.reset(); // empty seat: no bot, no fire
        }
        seat.lastCommandValid = false;
    }
    broadcastCrewRoster(id); // occupancy/roles may have changed
}

void WorldBroadcaster::sampleCrewSeats(ControlledEntity& ce, const EntityState& st, uint64_t tick, double dt,
                                       const AiTickContext& ctx) {
    const glm::quat airQ{st.transform.quat[3], st.transform.quat[0], st.transform.quat[1], st.transform.quat[2]};
    for (std::size_t s = 0; s < ce.crew.seats.size(); ++s) {
        CrewSeat& seat = ce.crew.seats[s];
        if (seat.isFlySeat)
            continue; // flight is the airframe controller's job
        if (seat.knockedOut) {
            seat.lastCommandValid = false; // a knocked-out seat goes silent — no command, turret holds (#978)
            continue;
        }

        SeatCommand cmd;
        bool haveCmd = false;
        if (seat.occupantPeer != kNoSeatPeer) {
            // Human seat (#972): the command was resolved from the peer's masked MsgClientInput in the
            // serial applyHumanCrewInput pre-pass. Reuse it here (this parallel pass reads no m_peerInputs).
            if (seat.lastCommandValid) {
                cmd = seat.lastCommand;
                haveCmd = true;
            }
        } else if (seat.botOccupied && seat.seatBot) {
            // Bot seat (#969): sample its ISeatController with the seat's honest view.
            SeatView view;
            view.seatIndex = static_cast<uint8_t>(s);
            view.capabilities = seat.capabilities;
            view.skill = seat.skill;
            view.reaction = seat.reaction;
            if (seat.turretIndex >= 0) {
                const CrewTurret& tr = ce.crew.turrets[static_cast<std::size_t>(seat.turretIndex)];
                view.turret.present = true;
                view.turret.mountRest = tr.mountRest;
                view.turret.azMinRad = tr.limits.azMinRad;
                view.turret.azMaxRad = tr.limits.azMaxRad;
                view.turret.elMinRad = tr.limits.elMinRad;
                view.turret.elMaxRad = tr.limits.elMaxRad;
                view.turret.boreWorld = turretWorldDir(tr.state, tr.mountRest, airQ); // current aim (from last slew)
            }
            cmd = seat.seatBot->sample(st, view, tick, dt, ctx);
            seat.lastCommand = cmd;
            seat.lastCommandValid = true;
            haveCmd = true;
        } else {
            seat.lastCommandValid = false; // empty seat (no bot, no human): its channels stay silent
        }

        // Command the turret servo toward the (bot or human) seat's aim. The servo clamps + slews in
        // the integrate pass; this only sets the target. Per-entity write, disjoint in the parallel pass.
        if (haveCmd && seat.turretIndex >= 0 && cmd.hasAim) {
            CrewTurret& tr = ce.crew.turrets[static_cast<std::size_t>(seat.turretIndex)];
            commandTurretWorld(tr.state, tr.limits, tr.mountRest, airQ, cmd.aimDirWorld);
        }
    }
}

void WorldBroadcaster::applyHumanCrewInput(uint64_t tick) {
    (void)tick;
    for (auto& [idx, ce] : m_controlledEntities) {
        if (!ce.crew.crewed())
            continue;
        const EntityState* st = m_entityManager.get(ce.id);
        if (!st || st->dead)
            continue;
        const glm::quat airQ{st->transform.quat[3], st->transform.quat[0], st->transform.quat[1],
                             st->transform.quat[2]};
        for (std::size_t s = 0; s < ce.crew.seats.size(); ++s) {
            CrewSeat& seat = ce.crew.seats[s];
            if (seat.isFlySeat || seat.occupantPeer == kNoSeatPeer || seat.knockedOut)
                continue; // the Fly seat flies via the controller; only LIVE human NON-fly seats route here
            const auto pit = m_peerInputs.find(seat.occupantPeer);
            if (pit == m_peerInputs.end()) {
                seat.lastCommandValid = false;
                continue;
            }
            const PeerInputState& in = pit->second;
            const bool aimsTurret = seat.turretIndex >= 0;
            const SeatInputRouting route = seatInputRouting(seat.capabilities, aimsTurret);

            SeatCommand cmd;
            if (route.aimTurret) {
                // viewAxis is the gunner's world-space look direction; the servo slews the turret toward
                // it (clamped to the seat's arc). A degenerate zero vector leaves the last aim.
                const glm::vec3 aim{in.viewAxis[0], in.viewAxis[1], in.viewAxis[2]};
                if (glm::dot(aim, aim) > 1e-6f) {
                    cmd.hasAim = true;
                    cmd.aimDirWorld = aim;
                }
            }
            if (route.driveFire) {
                cmd.trigger = (in.buttons & 0x01u) != 0u;          // gun trigger (level)
                cmd.release = (in.buttons & 0x04u) != 0u;          // fire selected store (edge downstream)
                cmd.station = clampSeatStation(in.selectedStation, // clamp to the seat's own partition
                                               seat.fire.loadout.stations.size());
            }
            seat.lastCommand = cmd;
            seat.lastCommandValid = true;
            (void)airQ;
        }
    }
}

void WorldBroadcaster::runCrewedFire(ControlledEntity& ce, uint32_t idx, uint64_t tick) {
    const EntityState* st = m_entityManager.get(ce.id);
    if (!st || st->dead)
        return;
    const bool hold = m_formations.weaponsHoldFor(ce.id); // #610's order, with teeth
    const glm::quat airQ{st->transform.quat[3], st->transform.quat[0], st->transform.quat[1], st->transform.quat[2]};

    float massSum = 0.f;
    float cd0Sum = 0.f;
    for (std::size_t s = 0; s < ce.crew.seats.size(); ++s) {
        CrewSeat& seat = ce.crew.seats[s];
        massSum += seat.fire.loadout.payloadMassKg; // a knocked-out seat's stores still weigh on the airframe
        cd0Sum += seat.fire.loadout.payloadCd0;
        if (seat.knockedOut) // #978: a knocked-out seat fires nothing (its channel is silent)
            continue;
        if (!hasCapability(seat.capabilities, CrewCapability::Fire) || seat.fire.loadout.empty())
            continue;

        // The fire input: the Fly seat fires from the pilot's ControlInput; a gunner from its bot's
        // last SeatCommand. Both project onto WeaponControls.
        WeaponControls wc;
        bool active = false;
        if (seat.isFlySeat) {
            wc = weaponControlsOf(ce.lastInput);
            active = ce.lastInputValid;
        } else if (seat.lastCommandValid) {
            // A non-fly seat: bot (sampled in the AI pass) OR human (masked input, applyHumanCrewInput).
            wc = WeaponControls{seat.lastCommand.trigger, seat.lastCommand.release, seat.lastCommand.station};
            active = true;
        }
        if (!active)
            continue;

        const std::size_t before = m_fireRequests.size();
        evaluateFire(seat.fire, *m_weaponRegistry, wc, hold, tick, idx, m_fireRequests);

        // Stamp the seat index (for the deterministic sort) and, for a turret seat, the world-space
        // bore the shot leaves along (#970) onto the requests this seat just appended.
        glm::vec3 aim{0.f};
        bool hasAim = false;
        if (seat.turretIndex >= 0) {
            const CrewTurret& tr = ce.crew.turrets[static_cast<std::size_t>(seat.turretIndex)];
            aim = turretWorldDir(tr.state, tr.mountRest, airQ);
            hasAim = true;
        }
        for (std::size_t r = before; r < m_fireRequests.size(); ++r) {
            m_fireRequests[r].seat = static_cast<uint8_t>(s);
            if (hasAim) {
                m_fireRequests[r].hasAimDir = true;
                m_fireRequests[r].aimDir[0] = aim.x;
                m_fireRequests[r].aimDir[1] = aim.y;
                m_fireRequests[r].aimDir[2] = aim.z;
            }
        }
    }
    ce.payload.extra_mass_kg = ce.crew.baseMassKg + massSum;
    ce.payload.extra_cd0 = ce.crew.baseCd0 + cd0Sum;
}

void WorldBroadcaster::registerController(EntityId id, std::unique_ptr<IEntityController> controller,
                                          std::shared_ptr<const FlightModelData> model, float initialAirspeed,
                                          std::string aiScriptName) {
    // An AI aircraft flies ITS OWN aeroplane. When the caller does not hand us a model, resolve the
    // entity type's own flightModelAsset rather than silently defaulting to the builtin UFO -- which
    // is what every `spawn <type> --ai <behavior>` did, so an AI F-5E flew a UFO with an F-5E's mesh
    // on it. Same silent-builtin-fallback bug as #811, on the server side of the same seam.
    if (!model)
        model = resolveFlightModel(id); // logs and returns null if the id is unknown; builtin below
    // AI/scripted entities are decimatable — their sample() may be skipped under tick overrun (#514).
    addControlledEntity(id, std::move(controller), std::move(model), 0.f, /*decimatable=*/true, initialAirspeed);
    // Tag the entity with the Lua script it was built from (#152), so a changed ai/*.lua rebuilds it.
    if (!aiScriptName.empty()) {
        if (auto it = m_controlledEntities.find(id.index); it != m_controlledEntities.end())
            it->second.aiScriptName = std::move(aiScriptName);
    }
}

void WorldBroadcaster::reloadFlightModels() {
    for (auto& [idx, ce] : m_controlledEntities) {
        if (!ce.sim)
            continue;
        std::shared_ptr<const FlightModelData> model = resolveFlightModel(ce.id);
        if (!model)
            continue; // empty/unknown flightModelAsset -> keep the current model (never fall to builtin here)
        ce.sim->setFlightModel(model);
        applyForceModelFor(*ce.sim, *model); // the model's force-model role may have changed
    }
}

bool WorldBroadcaster::replaceController(EntityId id, std::unique_ptr<IEntityController> controller,
                                         std::string aiScriptName) {
    auto it = m_controlledEntities.find(id.index);
    if (it == m_controlledEntities.end() || it->second.id.generation != id.generation)
        return false;
    it->second.controller = std::move(controller);
    it->second.lastInputValid = false; // force a fresh sample next tick
    it->second.aiScriptName = std::move(aiScriptName);
    if (it->second.controller)
        it->second.controller->setPlanetRadius(static_cast<double>(m_planetRadiusKm) * 1000.0);
    return true;
}

std::vector<EntityId> WorldBroadcaster::entitiesUsingAiScript(std::string_view scriptName) const {
    std::vector<EntityId> out;
    for (const auto& [idx, ce] : m_controlledEntities)
        if (ce.aiScriptName == scriptName)
            out.push_back(ce.id);
    return out;
}

bool WorldBroadcaster::setEntityLoadout(EntityId id, const std::vector<std::string>& stores,
                                        std::vector<std::string>& warnings) {
    auto it = m_controlledEntities.find(id.index);
    if (it == m_controlledEntities.end() || !m_weaponRegistry)
        return false;
    const EntityState* st = m_entityManager.get(id);
    if (!st)
        return false;
    const EntityDef* def = m_registry.byIndex(st->typeIndex);
    if (!def || def->hardpoints.empty())
        return false;

    it->second.fire.loadout = buildLoadoutOverride(*def, *m_weaponRegistry, stores, warnings);
    // Re-cost the airframe: the payload the integrator adds each tick is the override's mass/drag now,
    // not the #812 default (which addControlledEntity resolved). Keeping the loadout and payload in sync
    // is the same invariant #625 maintains as stores leave the rails.
    it->second.payload.extra_mass_kg = it->second.fire.loadout.payloadMassKg;
    it->second.payload.extra_cd0 = it->second.fire.loadout.payloadCd0;
    return true;
}

// Grain size for the per-entity parallel passes: enough indices per chunk to amortise the
// dynamic-claim atomic without starving load balancing across workers.
static constexpr std::size_t kEntityPassGrain = 16;

void WorldBroadcaster::runEntityPass(std::size_t count, const std::function<void(std::size_t, std::size_t)>& fn) {
    if (count == 0)
        return;
    if (m_jobs)
        m_jobs->parallel_for(count, kEntityPassGrain, fn);
    else
        fn(0, count); // inline / serial fallback (unit tests, single-threaded servers)
}

// Grain size for the per-peer snapshot build: 1, because each peer is a heavy, heterogeneous-cost
// unit (a draw-distance interest query + priority/budget scheduler + quantized bitstream encode), so
// the finest grain gives the dynamic-claim cursor the best load balancing across workers.
static constexpr std::size_t kPeerPassGrain = 1;

void WorldBroadcaster::runPeerPass(std::size_t count, const std::function<void(std::size_t, std::size_t)>& fn) {
    if (count == 0)
        return;
    if (m_jobs)
        m_jobs->parallel_for(count, kPeerPassGrain, fn);
    else
        fn(0, count); // inline / serial fallback (unit tests, single-threaded servers)
}

void WorldBroadcaster::updateTerrainSteerCache() {
    // Lowest live entity index = a stable representative (m_controlledEntities order is unordered).
    const ControlledEntity* rep = nullptr;
    uint32_t repIdx = 0;
    for (const StepItem& it : m_stepItems) {
        if (!rep || it.idx < repIdx) {
            rep = it.ce;
            repIdx = it.idx;
        }
    }
    if (rep) {
        const FlightState& fs = rep->sim->state();
        m_entityX.store(fs.pos_world[0], std::memory_order_relaxed);
        m_entityZ.store(fs.pos_world[2], std::memory_order_relaxed);
    }
}

void WorldBroadcaster::stepFlightSim(FlightIntegrator& fi, EntityState& state, const ControlInput& ctrl,
                                     const PayloadEffect& payload, double simDt, uint32_t entityIdx,
                                     uint64_t tickIndex) {
    WindInfluence wind{};
    if (m_weather) {
        if (m_weather->hasWindProfile()) {
            // Altitude wind (#489): a high-flying entity feels the wind at ITS altitude, not the
            // surface scalar. Matches the client's ClientPrediction interp on the broadcast profile.
            const auto& p = fi.state().pos_world;
            const float altM = static_cast<float>(
                windAltitudeM(glm::dvec3(p[0], p[1], p[2]), static_cast<double>(m_planetRadiusKm) * 1000.0));
            const glm::vec2 w = m_weather->windAtAltitude(altM);
            wind.wind_world[0] = w.x;
            wind.wind_world[2] = w.y;
        } else {
            wind.wind_world[0] = m_weather->windX();
            wind.wind_world[2] = m_weather->windZ();
        }
        // Per-entity deterministic turbulence: a pure function of (entityIdx, tickIndex) + amplitude,
        // so the perturbation is independent of evaluation order, identical across worker counts and
        // platforms, and — since #426 broadcasts the amplitude in MsgWeatherState — reproducible on
        // the client for prediction (weatherTurbulence(), shared with ClientPrediction).
        const auto turb = weatherTurbulence(entityIdx, tickIndex, m_weather->turbulenceAmplitude());
        wind.turbulence_body[0] = turb[0];
        wind.turbulence_body[1] = turb[1];
        wind.turbulence_body[2] = turb[2];
    }

    // Stall buffet (#816). Folded in HERE rather than inside the integrator, and computed from a
    // deterministic (entityIdx, tickIndex) seed, so ClientPrediction can reproduce it exactly --
    // see StallBuffet.h. A buffet the client could not predict would tear prediction apart at
    // precisely the moment the aircraft is hardest to fly.
    if (fi.state().stalled) {
        const auto buffet = stallBuffet(entityIdx, tickIndex);
        wind.turbulence_body[0] += buffet[0];
        wind.turbulence_body[1] += buffet[1];
        wind.turbulence_body[2] += buffet[2];
    }
    const glm::dvec3 groundPos{fi.state().pos_world[0], fi.state().pos_world[1], fi.state().pos_world[2]};
    float groundElev = m_groundQuery ? m_groundQuery(groundPos) : m_groundElevation.load(std::memory_order_relaxed);
    // Per-surface ground handling (#487): grass/gravel/water differ from a paved runway in the rollout.
    // groundFrictionFor is pure table math, shared with ClientPrediction, so the server and the client
    // shed ground speed identically. No query set ⇒ default (paved) ⇒ bit-identical to before.
    fl::GroundFriction ground =
        m_groundSurfaceQuery ? groundFrictionFor(m_groundSurfaceQuery(groundPos)) : fl::GroundFriction{};

    const bool isVessel = fi.flightModel().isVessel();
    if (isVessel) {
        // A ship (#38) floats: its floor is the SEA SURFACE, never the seabed the bathymetry query
        // returns, and water "rolling resistance" is meaningless for a hull whose drag lives in
        // VesselForceModel.
        groundElev = std::max(groundElev, 0.f);
        ground = fl::GroundFriction{};
    } else {
        // Carrier decks (#38): the floor under an aircraft is the HIGHER of the terrain and any
        // deck plane it is over. m_decks is rebuilt serially at tick start and read-only during
        // this parallel pass; deckFloorApplies keeps a pass under the bow from being teleported up.
        for (const DeckRec& rec : m_decks) {
            if (rec.entityIdx == entityIdx)
                continue;
            const DeckLocalPoint local = deckLocalPoint(fi.state().pos_world, rec.pos, rec.quat, *rec.deck);
            if (deckFloorApplies(local, *rec.deck))
                groundElev = std::max(groundElev, static_cast<float>(rec.floorElevM));
        }
    }
    fi.step(static_cast<float>(simDt), ctrl, payload, wind, groundElev, ground);

    const FlightState& fs = fi.state();
    // (Terrain-steer XZ cache moved to updateTerrainSteerCache(), run once after the integrate pass
    // — keeps this routine free of cross-entity writes so it is safe to call from worker threads.)

    // World velocity: rotate body velocity into world frame.
    // vel_body is double; cast to float here — wire protocol and render bridge stay float.
    float vel_body_f[3] = {float(fs.vel_body[0]), float(fs.vel_body[1]), float(fs.vel_body[2])};
    float wv[3];
    quatRotate(fs.quat, vel_body_f, wv);

    // Coordinate conventions are identical (both Y-up) — copy directly.
    state.transform.pos[0] = fs.pos_world[0];
    state.transform.pos[1] = fs.pos_world[1];
    state.transform.pos[2] = fs.pos_world[2];

    state.transform.vel[0] = wv[0];
    state.transform.vel[1] = wv[1];
    state.transform.vel[2] = wv[2];

    std::memcpy(state.transform.quat, fs.quat, 4 * sizeof(float));
}

void WorldBroadcaster::sendConnectAck(uint32_t peerId, EntityId assigned, PeerRole grantedRole) {
    const uint32_t typeCount = m_registry.typeCount();

    std::vector<uint8_t> buf;
    buf.reserve(sizeof(MsgConnectAck) + typeCount * sizeof(MsgEntityTypeDef));

    MsgConnectAck ack;
    ack.msgId = static_cast<uint8_t>(MsgId::ConnectAck);
    ack.tickRateHz = 60;
    ack.typeCount = static_cast<uint16_t>(typeCount);
    ack.assignedEntityIdx = assigned.index;
    ack.assignedEntityGen = assigned.generation;
    ack.planetRadiusKm = m_planetRadiusKm;
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
        const PayloadEffect payload = m_payloadResolver ? m_payloadResolver(*def) : PayloadEffect{};
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

    // Granted-authority TLV (#949): appended after the entity-type records when this peer holds caps,
    // so the client can show/hide GM/moderator/faction-leader UI. Cosmetic only — the server remains
    // the enforcement point. Old clients iterate the records by typeCount and skip the unknown tag.
    // Re-sent on a mid-session grant/revoke (setPeerAuthority re-calls sendConnectAck). Payload is
    // { uint64 caps LE, uint16 factionIndex LE }, 10 bytes, matching ExtTag::ConnectAckAuthority.
    if (auto pit = m_peerInputs.find(peerId); pit != m_peerInputs.end() && pit->second.authority.any()) {
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
    if (m_factionRegistry) {
        const uint16_t factionCount = m_factionRegistry->count();
        if (factionCount > 0) {
            std::vector<uint8_t> fbuf;
            fbuf.reserve(static_cast<std::size_t>(factionCount) * sizeof(MsgFactionDef));
            for (uint16_t fi = 0; fi < factionCount; ++fi) {
                const FactionDef* fdef = m_factionRegistry->get(fi);
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
                amsg.level = static_cast<uint8_t>(m_factionRegistry->alertLevel(fi));
                m_net.send(peerId, &amsg, sizeof(amsg), /*reliable=*/true);
            }
        }
    }

    // Radio-net table (#532): the client cannot key a mic until it knows which nets exist and what
    // each one sounds like. Sent beside the faction table for the same reason — both are small,
    // server-authoritative vocabularies the client needs before its first frame.
    sendVoiceNetDefs(peerId);

    // Mission roster (#914): one reliable packet of concatenated MsgMissionRoster records mapping each
    // spawned mission object's entity idx/gen -> its mission object id, so the cinematic recorder (#909)
    // can resolve an entity-relative camera shot's target/look_at (a mission object id) to a live
    // network entity. Only entries whose entity is currently valid are sent (an unoccupied player slot
    // has an invalid EntityId — omitted until a pilot binds it, then delivered as a delta). Empty when
    // no mission is loaded.
    if (!m_missionRoster.empty()) {
        std::vector<uint8_t> rbuf;
        rbuf.reserve(m_missionRoster.size() * sizeof(MsgMissionRoster));
        for (const auto& [objectId, eid] : m_missionRoster) {
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
    for (const auto& [entityIdx, ce] : m_controlledEntities) {
        (void)entityIdx;
        if (ce.crew.crewed())
            sendCrewRoster(peerId, ce.id);
    }
}

// ── match lifecycle + scoring (#523) ────────────────────────────────────────
void WorldBroadcaster::setMatchState(const MatchStatePod& state) {
    m_matchState = state;
    m_haveMatchState = true;
    broadcastMatchState();
}

void WorldBroadcaster::sendMatchStateTo(uint32_t peerId) {
    if (!m_haveMatchState)
        return;
    MsgMatchState msg{};
    msg.phase = m_matchState.phase;
    msg.scoreLimit = m_matchState.scoreLimit;
    msg.teamCount = static_cast<uint8_t>(std::min<std::size_t>(m_matchState.teamScores.size(), 255));
    msg.phaseEndTick = m_matchState.phaseEndTick;
    std::snprintf(msg.modeId, sizeof(msg.modeId), "%s", m_matchState.modeId.c_str());
    std::snprintf(msg.modeName, sizeof(msg.modeName), "%s", m_matchState.modeName.c_str());
    std::vector<uint8_t> pkt;
    pkt.reserve(sizeof(msg) + m_matchState.teamScores.size() * sizeof(MatchTeamScore));
    appendMsg(pkt, msg);
    for (const auto& [fac, score] : m_matchState.teamScores) {
        MatchTeamScore rec{};
        rec.factionIndex = fac;
        rec.score = score;
        appendMsg(pkt, rec);
    }
    m_net.send(peerId, pkt.data(), pkt.size(), /*reliable=*/true);
}

void WorldBroadcaster::broadcastMatchState() {
    for (const auto& [pid, pin] : m_peerInputs) {
        if (pin.handshakeComplete)
            sendMatchStateTo(pid);
    }
}

void WorldBroadcaster::appendScoreboardRows(std::vector<uint8_t>& pkt, std::size_t begin, std::size_t count,
                                            const std::vector<uint32_t>& order) const {
    for (std::size_t i = begin; i < begin + count; ++i) {
        const uint32_t pid = order[i];
        const auto sit = m_scores.find(pid);
        ScoreboardRow row{};
        row.participantId = pid;
        if (sit != m_scores.end()) {
            row.score = sit->second.score;
            row.kills = static_cast<uint16_t>(std::min<uint32_t>(sit->second.kills, 0xFFFFu));
            row.deaths = static_cast<uint16_t>(std::min<uint32_t>(sit->second.losses, 0xFFFFu));
        }
        // Ping: humans carry their estimatedDelayTicks; bots have none.
        if (!isBotParticipant(pid)) {
            const auto pit = m_peerInputs.find(pid);
            if (pit != m_peerInputs.end()) {
                const uint32_t ms = static_cast<uint32_t>(pit->second.estimatedDelayTicks) * 1000u / 60u;
                row.pingMs = static_cast<uint16_t>(std::min<uint32_t>(ms, 0xFFFFu));
            }
        }
        // Team from the roster (the authoritative team record) else the live entity.
        const auto rit = m_roster.find(pid);
        row.factionIndex = (rit != m_roster.end()) ? rit->second.factionIndex : factionForPeer(pid);
        appendMsg(pkt, row);
    }
}

void WorldBroadcaster::sendScoreboardTo(uint32_t peerId) {
    if (m_scores.empty())
        return;
    std::vector<uint32_t> order;
    order.reserve(m_scores.size());
    for (const auto& [pid, sc] : m_scores) {
        (void)sc;
        order.push_back(pid);
    }
    std::sort(order.begin(), order.end()); // deterministic
    for (std::size_t i = 0; i < order.size(); i += kMaxScoreboardRowsPerPacket) {
        const std::size_t n = std::min(kMaxScoreboardRowsPerPacket, order.size() - i);
        MsgScoreboardHeader hdr{};
        hdr.count = static_cast<uint8_t>(n);
        std::vector<uint8_t> pkt;
        pkt.reserve(sizeof(hdr) + n * sizeof(ScoreboardRow));
        appendMsg(pkt, hdr);
        appendScoreboardRows(pkt, i, n, order);
        m_net.send(peerId, pkt.data(), pkt.size(), /*reliable=*/false);
    }
}

void WorldBroadcaster::broadcastScoreboard() {
    for (const auto& [pid, pin] : m_peerInputs) {
        if (pin.handshakeComplete)
            sendScoreboardTo(pid);
    }
}

void WorldBroadcaster::resetWorld() {
    // Despawn every peer's aircraft (and its owned AI flight) via the existing teardown primitive.
    std::vector<uint32_t> peerIds;
    peerIds.reserve(m_peerEntities.size());
    for (const auto& [pid, eid] : m_peerEntities) {
        (void)eid;
        peerIds.push_back(pid);
    }
    for (uint32_t pid : peerIds)
        despawnPeerEntity(pid);

    // Kill any remaining controlled entity (AI, mission objects). Copy the keys first — killing mutates
    // m_controlledEntities.
    std::vector<EntityId> controlled;
    controlled.reserve(m_controlledEntities.size());
    for (const auto& [idx, ce] : m_controlledEntities) {
        (void)idx;
        controlled.push_back(ce.id);
    }
    for (EntityId id : controlled)
        killControlledAircraft(id);

    // Clear match-scoped state; keep peers connected (m_peerInputs / m_roster / handshake survive).
    // In-flight projectiles are left to expire on their own TTL (a rotation is rare, and they cannot
    // score during the Ending freeze) rather than reaching into the ProjectileSystem's mirror entities.
    m_pendingKillEvents.clear();
    m_respawn.clear(); // #648: clear pending respawns for the new round
    m_pendingDeathCleanup.clear();
    m_disconnectGrace.clear(); // #524: scores belong to a match; a new round starts clean
    setMissionPlayerSlots({});
    m_missionRoster.clear();
    for (auto& [pid, sc] : m_scores) {
        (void)pid;
        sc = PeerScore{};
    }
    m_scoreboardDirty = true;
}

void WorldBroadcaster::readmitPilots() {
    // Collect peer ids first — admitPilot / sendConnectAck mutate maps we are iterating.
    std::vector<uint32_t> pilots;
    for (const auto& [peerId, pin] : m_peerInputs) {
        if (pin.handshakeComplete && pin.role == PeerRole::Pilot && m_peerEntities.count(peerId) == 0u)
            pilots.push_back(peerId);
    }
    for (uint32_t peerId : pilots) {
        uint16_t fac = kNoFaction;
        if (m_teamAssigner) {
            std::optional<uint16_t> t = m_teamAssigner(peerId);
            if (!t.has_value())
                continue; // no room — leave the pilot entity-less (a rare edge; they can retry)
            fac = *t;
        }
        const EntityId assigned = admitPilot(peerId, resolvePlayerEntityType(""), fac);
        sendConnectAck(peerId, assigned, PeerRole::Pilot);
        // Keep the roster + match participant state consistent with the new team.
        uint16_t myFaction = 0;
        if (const EntityState* s = m_entityManager.get(assigned))
            myFaction = s->factionIndex;
        if (auto rit = m_roster.find(peerId); rit != m_roster.end()) {
            RosterRec rec = rit->second;
            rec.factionIndex = myFaction;
            upsertRoster(peerId, rec);
        }
        recordParticipant(peerId, myFaction, /*isBot=*/false, /*joined=*/true);
    }
}

// ── AI bot participants (#87) ─────────────────────────────────────────────────
void WorldBroadcaster::registerBotParticipant(uint32_t participantId, EntityId entity, const std::string& callsign,
                                              uint16_t faction) {
    if (entity.valid())
        m_botEntities[entity.index] = participantId;
    RosterRec rec;
    rec.callsign = callsign.empty() ? ("Bot-" + std::to_string(participantId)) : callsign;
    rec.factionIndex = faction;
    rec.role = PeerRole::Pilot;
    rec.isBot = true;
    upsertRoster(participantId, rec);
    m_scores[participantId] = PeerScore{};
    m_scoreboardDirty = true;
    recordParticipant(participantId, faction, /*isBot=*/true, /*joined=*/true);
}

void WorldBroadcaster::removeBotParticipant(uint32_t participantId) {
    for (auto it = m_botEntities.begin(); it != m_botEntities.end();) {
        if (it->second == participantId)
            it = m_botEntities.erase(it);
        else
            ++it;
    }
    removeRoster(participantId);
    m_scores.erase(participantId);
    m_scoreboardDirty = true;
    recordParticipant(participantId, 0, /*isBot=*/true, /*joined=*/false);
}

// ── respawn (#648) ───────────────────────────────────────────────────────────
void WorldBroadcaster::respawnParticipant(uint32_t participantId) {
    // Bots (#87) respawn through their own path; here we handle human peers.
    const auto pit = m_peerInputs.find(participantId);
    if (pit == m_peerInputs.end() || !pit->second.handshakeComplete || pit->second.role != PeerRole::Pilot)
        return;

    uint16_t faction = kNoFaction;
    if (const auto rit = m_respawn.find(participantId); rit != m_respawn.end())
        faction = rit->second.factionIndex;
    // If the team was removed (e.g. by rotation) re-balance.
    if (faction == kNoFaction && m_teamAssigner) {
        std::optional<uint16_t> t = m_teamAssigner(participantId);
        if (t.has_value())
            faction = *t;
    }

    // Clean up any dangling entity mapping first, then spawn fresh.
    if (m_peerEntities.count(participantId) != 0u)
        despawnPeerEntity(participantId);
    const EntityId assigned = admitPilot(participantId, resolvePlayerEntityType(""), faction);
    sendConnectAck(participantId, assigned, PeerRole::Pilot);

    uint16_t myFaction = 0;
    if (const EntityState* s = m_entityManager.get(assigned))
        myFaction = s->factionIndex;
    if (auto rit = m_roster.find(participantId); rit != m_roster.end()) {
        RosterRec rec = rit->second;
        rec.factionIndex = myFaction;
        upsertRoster(participantId, rec);
    }
    recordParticipant(participantId, myFaction, /*isBot=*/false, /*joined=*/true);

    m_respawn.erase(participantId);
}

void WorldBroadcaster::processRespawns() {
    // Deferred death teardown (safe here, outside the entity-event callback).
    if (!m_pendingDeathCleanup.empty()) {
        for (uint32_t peerId : m_pendingDeathCleanup) {
            if (m_peerEntities.count(peerId) != 0u)
                despawnPeerEntity(peerId);
        }
        m_pendingDeathCleanup.clear();
    }

    if (m_respawn.empty() || m_combatFrozen)
        return; // no pending respawns, or combat frozen (Ending/PostMatch) — hold

    // Collect the ids that are due AND requested (humans) or automatic (bots); respawnParticipant
    // erases from m_respawn, so gather first.
    std::vector<uint32_t> ready;
    for (const auto& [pid, rec] : m_respawn) {
        if (m_currentTick < rec.dueTick)
            continue;
        if (!rec.isBot && !rec.requested)
            continue; // a human must ask to respawn (they may want to keep spectating)
        ready.push_back(pid);
    }
    for (uint32_t pid : ready)
        respawnParticipant(pid);
}

void WorldBroadcaster::sendConnectRefusal(uint32_t peerId, ConnectRefusalCode code, const char* reason) {
    MsgConnectRefusal msg{};
    msg.code = static_cast<uint8_t>(code);
    std::snprintf(msg.reason, sizeof(msg.reason), "%s", reason);
    m_net.send(peerId, &msg, sizeof(msg), /*reliable=*/true);
}

// ── match roster (#996) ─────────────────────────────────────────────────────
std::string WorldBroadcaster::sanitizeCallsign(const char* raw, uint32_t participantId) {
    std::string out;
    if (raw) {
        // Copy at most 31 printable chars; drop ASCII control bytes (< 0x20, 0x7F). Leave UTF-8 lead/
        // continuation bytes (>= 0x80) intact so a non-Latin callsign survives — the HUD font is BMP.
        for (const char* p = raw; *p && out.size() < 31u; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c < 0x20u || c == 0x7Fu)
                continue;
            out.push_back(static_cast<char>(c));
        }
    }
    // Trim leading/trailing spaces.
    const std::size_t b = out.find_first_not_of(' ');
    const std::size_t e = out.find_last_not_of(' ');
    out = (b == std::string::npos) ? std::string{} : out.substr(b, e - b + 1);
    if (out.empty()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Pilot-%u", participantId);
        out = buf;
    }
    return out;
}

void WorldBroadcaster::upsertRoster(uint32_t participantId, const RosterRec& rec) {
    m_roster[participantId] = rec;

    MsgPlayerRosterHeader hdr{};
    hdr.count = 1;
    PlayerRosterEntry e{};
    e.participantId = participantId;
    e.factionIndex = rec.factionIndex;
    e.role = static_cast<uint8_t>(rec.role);
    e.flags = rec.isBot ? kRosterBot : 0u;
    std::snprintf(e.callsign, sizeof(e.callsign), "%s", rec.callsign.c_str());

    std::vector<uint8_t> pkt;
    pkt.reserve(sizeof(hdr) + sizeof(e));
    appendMsg(pkt, hdr);
    appendMsg(pkt, e);
    // Broadcast to every peer that has completed its handshake (so the roster is consistent for all).
    for (const auto& [pid, pin] : m_peerInputs) {
        if (pin.handshakeComplete)
            m_net.send(pid, pkt.data(), pkt.size(), /*reliable=*/true);
    }
}

void WorldBroadcaster::removeRoster(uint32_t participantId) {
    if (m_roster.erase(participantId) == 0u)
        return;
    MsgPlayerRosterHeader hdr{};
    hdr.count = 1;
    PlayerRosterEntry e{};
    e.participantId = participantId;
    e.flags = kRosterLeave;
    std::vector<uint8_t> pkt;
    pkt.reserve(sizeof(hdr) + sizeof(e));
    appendMsg(pkt, hdr);
    appendMsg(pkt, e);
    for (const auto& [pid, pin] : m_peerInputs) {
        if (pin.handshakeComplete)
            m_net.send(pid, pkt.data(), pkt.size(), /*reliable=*/true);
    }
}

void WorldBroadcaster::sendFullRoster(uint32_t peerId) {
    if (m_roster.empty())
        return;
    // Chunk into packets of at most kMaxRosterEntriesPerPacket upsert records.
    std::vector<std::pair<uint32_t, RosterRec>> all(m_roster.begin(), m_roster.end());
    for (std::size_t i = 0; i < all.size(); i += kMaxRosterEntriesPerPacket) {
        const std::size_t n = std::min(kMaxRosterEntriesPerPacket, all.size() - i);
        MsgPlayerRosterHeader hdr{};
        hdr.count = static_cast<uint8_t>(n);
        std::vector<uint8_t> pkt;
        pkt.reserve(sizeof(hdr) + n * sizeof(PlayerRosterEntry));
        appendMsg(pkt, hdr);
        for (std::size_t j = 0; j < n; ++j) {
            const auto& [pid, rec] = all[i + j];
            PlayerRosterEntry e{};
            e.participantId = pid;
            e.factionIndex = rec.factionIndex;
            e.role = static_cast<uint8_t>(rec.role);
            e.flags = rec.isBot ? kRosterBot : 0u;
            std::snprintf(e.callsign, sizeof(e.callsign), "%s", rec.callsign.c_str());
            appendMsg(pkt, e);
        }
        m_net.send(peerId, pkt.data(), pkt.size(), /*reliable=*/true);
    }
}

void WorldBroadcaster::rejectConnection(uint32_t peerId, const std::string& ip, ConnectRefusalCode code) {
    const RejectInfo info = rejectInfoFor(code);
    char msg[160];
    std::snprintf(msg, sizeof(msg), "peer %u from %s rejected (%s) -- disconnecting", peerId, ip.c_str(),
                  info.logPhrase);
    m_logger.log(info.level, __FILE__, __LINE__, msg);
    sendConnectRefusal(peerId, code, info.reason);
    m_net.disconnectPeer(peerId);
}

// ---------------------------------------------------------------------------
// Shutdown countdown
// ---------------------------------------------------------------------------

void WorldBroadcaster::setShutdownCallback(std::function<void()> fn) {
    m_shutdownCallback = std::move(fn);
}

void WorldBroadcaster::initiateShutdown(uint32_t secondsDelay, uint32_t warningIntervalS, std::string reason) {
    using namespace std::chrono;
    m_shuttingDown = true;
    m_shutdownAt = m_clock->now() + seconds(secondsDelay);
    m_warningIntervalS = warningIntervalS;
    m_nextNoticeAt = m_clock->now(); // fire on the very next tick
    m_shutdownReason = std::move(reason);
    m_shutdownActiveShared.store(true, std::memory_order_relaxed); // #226 beacon mirror
    m_shutdownSecsShared.store(secondsDelay, std::memory_order_relaxed);
}

void WorldBroadcaster::cancelShutdown() {
    m_shuttingDown = false;
    m_shutdownReason.clear();
    m_shutdownActiveShared.store(false, std::memory_order_relaxed); // #226
    m_shutdownSecsShared.store(0, std::memory_order_relaxed);
}

bool WorldBroadcaster::extendShutdown(uint32_t additionalSeconds) {
    if (!m_shuttingDown)
        return false;
    m_shutdownAt += std::chrono::seconds(additionalSeconds);
    m_nextNoticeAt = m_clock->now(); // immediate update notice on next tick
    m_shutdownSecsShared.fetch_add(additionalSeconds, std::memory_order_relaxed); // #226
    return true;
}

uint32_t WorldBroadcaster::secondsUntilShutdown() const noexcept {
    if (!m_shuttingDown)
        return 0;
    using namespace std::chrono;
    auto now = m_clock->now();
    if (now >= m_shutdownAt)
        return 0;
    return static_cast<uint32_t>(duration_cast<seconds>(m_shutdownAt - now).count());
}

std::string WorldBroadcaster::makeShutdownMessage(uint32_t secsLeft, const std::string& reason) {
    if (reason.empty()) {
        if (secsLeft == 0)
            return "Server is shutting down now.";
        if (secsLeft <= 60)
            return "Server shutting down in 1 minute -- save your progress.";
        if (secsLeft < 3600)
            return "Server shutting down in " + std::to_string(secsLeft / 60) + " minutes.";
        return "Server shutting down in " + std::to_string(secsLeft / 3600) + " hour(s).";
    }
    if (secsLeft == 0)
        return reason + " -- shutting down now.";
    if (secsLeft <= 60)
        return reason + " -- shutting down in 1 minute.";
    if (secsLeft < 3600)
        return reason + " -- shutting down in " + std::to_string(secsLeft / 60) + " minutes.";
    return reason + " -- shutting down in " + std::to_string(secsLeft / 3600) + " hour(s).";
}

void WorldBroadcaster::broadcastShutdownNotice(uint16_t secsLeft, const char* text) {
    MsgServerNotice notice;
    notice.secondsRemaining = secsLeft;
    if (std::strlen(text) >= sizeof(notice.text))
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                     "Shutdown notice truncated: reason too long for MsgServerNotice::text.");
    std::snprintf(notice.text, sizeof(notice.text), "%s", text);
    m_net.broadcast(&notice, sizeof(notice), /*reliable=*/true);
}

} // namespace fl
