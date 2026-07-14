// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "weapon/WeaponDef.h"
#include "weapon/WeaponRegistry.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fl {

class EntityManager;
class SpatialIndex;
class IGravityField;
struct EntityState;

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

    void configure(const WeaponRegistry* weapons, const IGravityField* gravity) noexcept {
        m_weapons = weapons;
        m_gravity = gravity;
    }

    // Launch `def` from the shooter's current state: spawns the projectile's pooled entity
    // (typeId = "projectile:<weapon id>", registered at startup because MsgEntityTypeDef only
    // travels in ConnectAck), inherits the shooter's velocity plus a nose-line boost, and arms at
    // the weapon's minimum range. Returns the projectile entity id (null = pool refused).
    EntityId launch(EntityManager& em, uint32_t weaponIndex, const EntityState& shooterState,
                    uint32_t shooterPeerOwnerId);

    // Advance every projectile by dt: boost/coast point-mass flight (gravity from the field, a
    // simple speed-proportional coast decay, thrust along the velocity), mirror transforms into
    // the pooled entities, and detect endings — contact fuze (within kContactFuzeM of a non-shooter
    // entity once armed), ground impact (radial altitude at or below terrain), overrange
    // (1.5 × max range) and TTL. Ended projectiles are collected into `outImpacts` and their
    // entities killed; the caller detonates. Deterministic: no dice anywhere in flight.
    void step(EntityManager& em, const SpatialIndex& si, float dt, const GroundQuery& ground,
              std::vector<ProjectileImpact>& outImpacts);

    [[nodiscard]] std::size_t liveCount() const noexcept {
        return m_projectiles.size();
    }
    [[nodiscard]] const std::vector<Projectile>& projectiles() const noexcept {
        return m_projectiles;
    }

    static constexpr float kContactFuzeM = 8.f;
    static constexpr float kMaxFlightTimeS = 90.f;
    static constexpr float kCoastDecayPerS = 0.035f; // fraction of speed shed per second unpowered

  private:
    const WeaponRegistry* m_weapons{nullptr};
    const IGravityField* m_gravity{nullptr};
    std::vector<Projectile> m_projectiles;
};

// The projectile entity-type id for a weapon: "projectile:<weapon id>". One vocabulary for the
// spawner (here), the startup registration (ContentBootstrap) and the client's mesh resolution.
[[nodiscard]] std::string projectileTypeId(const WeaponDef& def);

} // namespace fl
