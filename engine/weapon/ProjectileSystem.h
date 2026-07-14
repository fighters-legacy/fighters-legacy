// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "weapon/Seeker.h"
#include "weapon/WeaponDef.h"
#include "weapon/WeaponRegistry.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fl {

class EntityManager;
class EntityTypeRegistry;
class SpatialIndex;
class IGravityField;
struct EntityState;

namespace sensor {
struct ContactTable; // SensorSystem.h — the SARH/ARH support source (#628)
}

// One projectile in flight (#625). Deliberately NOT a FlightIntegrator (6-DOF, spool lag, a
// Mach-6 numerical guard — all wrong for ordnance) and NOT a ControlledEntity: a 3-DOF point mass
// with its own tiny integrator. It mirrors into a pooled ObjectCategory::Projectile entity each
// step, which is what buys replication, interest management, and despawn TLVs for free.
struct Projectile {
    EntityId entityId;                // the pooled entity this projectile drives
    uint32_t weaponIndex{UINT32_MAX}; // into the WeaponRegistry
    EntityId shooter;                 // attribution chain: warhead damage credits this entity
    glm::dvec3 pos{};
    glm::vec3 vel{};
    glm::dvec3 launchPos{};
    float ageS{0.f};
    float motorRemainingS{0.f};
    float thrustAccelMps2{0.f}; // fixed boost acceleration derived from the def (see launch())
    bool armed{false};          // false until min_range_nm from the launch point
    // Seeker (#627). Null seekerDef = an unguided store (or a failed launch gate): pure ballistics.
    SeekerTrack seeker;
    std::shared_ptr<const sensor::SensorDef> seekerDef;
    bool emitting{false}; // the seeker head is radiating (ARH pitbull turns this on, #628)
    bool pitbull{false};  // ARH only: the missile's own radar has taken over from the datalink (#628)
};

// Where and why a projectile stopped flying. The system only DETECTS — warhead application,
// effects, and kill attribution are the caller's serial job (the over-G discipline).
struct ProjectileImpact {
    double pos[3]{};
    uint32_t weaponIndex{UINT32_MAX};
    EntityId shooter;
    EntityId directHit; // the entity a contact fuze triggered on; null for ground/self-destruct
};

// The projectile pool (#625). Owned by WorldBroadcaster, stepped serially in the weapons phase —
// serial is deliberate for v1: the #573/#580 load data shows per-entity integration is cheap, and
// serial sidesteps every determinism hazard. Each projectile writes only itself + its own entity,
// so a parallel_for is a one-line upgrade when a workload ever asks for it.
class ProjectileSystem {
  public:
    // Terrain elevation above the datum along the radial at a world position (the
    // TerrainStreamer::heightAt contract); null = datum floor at 0.
    using GroundQuery = std::function<float(glm::dvec3)>;
    // Resolves a seeker's `sensor_id` to a parsed SensorDef — the same shape (and, in fl-server,
    // the same function) as SensorSystem::SensorDefResolver, so seekers and aircraft radars read
    // one vocabulary through one resolution path (#810).
    using SensorResolver = std::function<std::shared_ptr<const sensor::SensorDef>(const std::string& id)>;
    // The SHOOTER's contact table (#628): what a SARH or pre-pitbull ARH shot is supported BY. The
    // missile inherits the shooter's honest belief — never ground truth. Null table = no support.
    using SupportQuery = std::function<const sensor::ContactTable*(uint32_t shooterIdx)>;

    void configure(const WeaponRegistry* weapons, const IGravityField* gravity) noexcept {
        m_weapons = weapons;
        m_gravity = gravity;
    }
    void setSensorResolver(SensorResolver fn) {
        m_sensorResolver = std::move(fn);
    }
    void setTypeRegistry(const EntityTypeRegistry* registry) noexcept {
        m_registry = registry; // target signatures (SignatureDef) come from the type def
    }
    void setCountermeasureCheck(SeekerCountermeasureCheck fn) {
        m_cmCheck = std::move(fn); // #529 plugs in here; null = no expendables exist
    }
    void setSupportQuery(SupportQuery fn) {
        m_supportQuery = std::move(fn); // WorldBroadcaster wires SensorSystem::contactsFor (#628)
    }

    // Would `weaponIndex`'s seeker take `target` from `shooter`'s hands right now (#628)? The
    // launch gate as a question — a supported (SARH / pitbull-ARH) weapon asks whether the
    // shooter's contact table holds the target LOCKED; a self-contained seeker asks its own
    // acquisition-lobe geometry. Drives the pre-launch HUD LOCK cue; costs one lobe test.
    [[nodiscard]] bool wouldAcquire(const EntityManager& em, uint32_t weaponIndex, const EntityState& shooter,
                                    EntityId target) const;

    // Launch `def` from the shooter's current state: spawns the projectile's pooled entity
    // (typeId = "projectile:<weapon id>", registered at startup because MsgEntityTypeDef only
    // travels in ConnectAck), inherits the shooter's velocity plus a nose-line boost, and arms at
    // the weapon's minimum range. Returns the projectile entity id (null = pool refused).
    //
    // `designatedTarget` (#627) is the SHOOTER's designated target — a player's boresight/contact
    // designation or an AI's — never something the missile invents. The LAUNCH GATE: a seeker
    // weapon starts locked only if the target is inside its acquisition lobe at release (the
    // pre-launch growl compressed to geometry — the PoD dice were paid during the shooter's own
    // acquisition); otherwise, or with no designation at all, the store flies dumb.
    EntityId launch(EntityManager& em, uint32_t weaponIndex, const EntityState& shooterState,
                    uint32_t shooterPeerOwnerId, EntityId designatedTarget = EntityId::null());

    // Advance every projectile by dt: boost/coast point-mass flight (gravity from the field, a
    // simple speed-proportional coast decay, thrust along the velocity), seeker checks at the
    // 10 Hz reference cadence staggered by projectile id (#627 — the shared Detection.h machine:
    // PoD gates acquisition, geometry maintains, coast then geometric reacquire), true
    // proportional-navigation steering at the seeker's LAST-KNOWN state clamped by max_g, mirror
    // transforms into the pooled entities, and detect endings — proximity fuze (closest approach
    // of the step SEGMENT within kContactFuzeM of a non-shooter entity once armed, so a Mach-4
    // closure cannot tunnel through the fuze bubble in one tick), ground impact (radial altitude
    // at or below terrain), overrange (1.5 × max range) and TTL. Ended projectiles are collected
    // into `outImpacts` and their entities killed; the caller detonates. Deterministic: every die
    // is the seeded (missile, target, tick) detection hash.
    void step(EntityManager& em, const SpatialIndex& si, float dt, const GroundQuery& ground, uint64_t tickIndex,
              const sensor::SensingEnvironment& env, std::vector<ProjectileImpact>& outImpacts);

    [[nodiscard]] std::size_t liveCount() const noexcept {
        return m_projectiles.size();
    }
    [[nodiscard]] const std::vector<Projectile>& projectiles() const noexcept {
        return m_projectiles;
    }

    static constexpr float kContactFuzeM = 8.f;
    static constexpr float kMaxFlightTimeS = 90.f;
    static constexpr float kCoastDecayPerS = 0.035f;     // fraction of speed shed per second unpowered
    static constexpr uint32_t kSeekerCheckTicks = 6;     // 10 Hz at the 60 Hz tick — the reference cadence
    static constexpr float kGravityAccelMps2 = 9.80665f; // max_g → lateral-accel clamp

  private:
    const WeaponRegistry* m_weapons{nullptr};
    const IGravityField* m_gravity{nullptr};
    const EntityTypeRegistry* m_registry{nullptr};
    SensorResolver m_sensorResolver;
    SeekerCountermeasureCheck m_cmCheck;
    SupportQuery m_supportQuery;
    std::vector<Projectile> m_projectiles;
};

// The projectile entity-type id for a weapon: "projectile:<weapon id>". One vocabulary for the
// spawner (here), the startup registration (ContentBootstrap) and the client's mesh resolution.
[[nodiscard]] std::string projectileTypeId(const WeaponDef& def);

} // namespace fl
