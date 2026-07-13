// SPDX-License-Identifier: GPL-3.0-or-later
#include "sensor/SensorSystem.h"

#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "sensor/BuiltinSensors.h"
#include "spatial/SpatialIndex.h"

#include <algorithm>
#include <cmath>

namespace fl::sensor {

namespace {

// Key one (target, sensor slot) track. A single observer may hold the same target on two sensors at
// once — found on radar and also visible to the eyeball — and those are two independent lifecycles.
[[nodiscard]] uint64_t trackKey(uint32_t targetIdx, uint32_t slot) noexcept {
    return (static_cast<uint64_t>(targetIdx) << 32) | slot;
}

// Ranking for the contact cap: a lock outranks a mere detection, and among equals the nearer target
// wins. Ties break on entity index, so the truncation is deterministic — two workers must never
// disagree about which 32 contacts survived.
[[nodiscard]] int stateRank(ContactState s) noexcept {
    switch (s) {
    case ContactState::Locked:
        return 3;
    case ContactState::Detected:
        return 2;
    case ContactState::Coasting:
        return 1;
    case ContactState::Lost:
        return 0;
    }
    return 0;
}

[[nodiscard]] double dist2(const double a[3], const double b[3]) noexcept {
    const double dx = a[0] - b[0];
    const double dy = a[1] - b[1];
    const double dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

} // namespace

const Contact* ContactTable::find(EntityId id) const noexcept {
    for (const Contact& c : contacts) {
        if (c.id.index == id.index && c.id.generation == id.generation)
            return &c;
    }
    return nullptr;
}

SensorSystem::SensorSystem(const EntityManager& em, const EntityTypeRegistry& registry)
    : m_entityManager(em), m_registry(registry) {}

void SensorSystem::setResolver(SensorDefResolver fn) {
    m_resolver = std::move(fn);
}

std::shared_ptr<const SensorDef> SensorSystem::resolve(const std::string& id) const {
    if (!m_resolver || id.empty())
        return nullptr;
    return m_resolver(id);
}

void SensorSystem::recomputeSignatureScale() {
    // How far beyond a sensor's BASELINE range the loudest thing in the world can be seen. Without
    // this the spatial query would be cut at the baseline range and a very loud target sitting just
    // outside it would never even reach the cone test — it would be invisible for a reason that has
    // nothing to do with sensing. Computed from the registry, which is fixed before the loop starts.
    float maxScale = 1.f;
    for (uint32_t i = 0;; ++i) {
        const EntityDef* def = m_registry.byIndex(i);
        if (!def)
            break;
        const SignatureDef& s = def->signatures;
        maxScale = std::max({maxScale, std::sqrt(std::max(0.f, s.rcs)), s.ir, s.visual, s.laser});
    }
    m_maxSignatureScale = maxScale;
}

void SensorSystem::addObserver(uint32_t entityIdx, const std::vector<std::string>& sensorIds, float skill,
                               float reaction) {
    ObserverState obs;
    obs.skill = skill;
    obs.reaction = reaction;

    for (const std::string& id : sensorIds) {
        if (auto def = resolve(id))
            obs.sensors.push_back(std::move(def));
        // An unresolved id is skipped, not fatal: the entity keeps the rest of its suite. The
        // warning is the caller's (it owns the logger); refusing to spawn an aircraft because one
        // sensor file is missing would be a worse failure than flying it half-blind.
    }

    // The decision record's default: an entity that declares no sensors is not omniscient and not
    // blind — it has eyes. There is no configuration of the engine, including the zero-content
    // sandbox, in which an observer sees through terrain.
    if (obs.sensors.empty()) {
        static const auto kEyeball = std::make_shared<const SensorDef>(BuiltinSensors::eyeball());
        obs.sensors.push_back(kEyeball);
    }

    m_observers[entityIdx] = std::move(obs);
}

void SensorSystem::removeObserver(uint32_t entityIdx) {
    m_observers.erase(entityIdx);
}

void SensorSystem::setEmitting(uint32_t entityIdx, bool emitting) {
    if (auto it = m_observers.find(entityIdx); it != m_observers.end())
        it->second.emitting = emitting;
}

bool SensorSystem::emitting(uint32_t entityIdx) const {
    auto it = m_observers.find(entityIdx);
    return it != m_observers.end() ? it->second.emitting : false;
}

const ContactTable* SensorSystem::contactsFor(uint32_t entityIdx) const {
    auto it = m_observers.find(entityIdx);
    return it != m_observers.end() ? &it->second.contacts : nullptr;
}

std::vector<SensorSystem::ObserverWork>& SensorSystem::gatherDue(uint64_t tickIndex, uint32_t strideTicks,
                                                                 double simDt) {
    m_work.clear();
    const uint32_t stride = std::max(1u, strideTicks);

    for (auto& [idx, obs] : m_observers) {
        // Staggered: spread the checks across the stride window rather than firing every sensor in
        // the world on the same tick and idling for the other nine at 10 Hz. The pattern depends
        // only on (tick, entityIdx), so it is identical on any worker count.
        if (((tickIndex + idx) % stride) != 0u)
            continue;

        const EntityState* st = m_entityManager.getByIndex(idx);
        if (!st || st->dead)
            continue;

        // Wall seconds since THIS observer's previous check — what the coast runs down on, so a
        // lock survives its lock_hold_s in real time regardless of the configured cadence.
        const float dtS = obs.everChecked
                              ? static_cast<float>(static_cast<double>(tickIndex - obs.lastCheckTick) * simDt)
                              : static_cast<float>(static_cast<double>(stride) * simDt);
        m_work.push_back(ObserverWork{idx, &obs, st, dtS});
    }

    // Deterministic order. The evaluation itself is order-independent (each worker writes only its
    // own observer), but a stable gather keeps the work split identical across runs.
    std::sort(m_work.begin(), m_work.end(),
              [](const ObserverWork& a, const ObserverWork& b) { return a.entityIdx < b.entityIdx; });
    return m_work;
}

void SensorSystem::evaluateObserver(const ObserverWork& work, const SpatialIndex& si, uint64_t tickIndex,
                                    const SensingEnvironment& env, float radarRangeFraction, float reactionTimeS) {
    ObserverState& obs = *work.obs;
    const EntityState& self = *work.state;

    obs.lastCheckTick = tickIndex;
    obs.everChecked = true;

    // The spatial query must cover the loudest target this sensor could see, not the baseline one.
    float queryRangeM = 0.f;
    for (const auto& s : obs.sensors)
        queryRangeM = std::max(queryRangeM, s->search.maxRangeM);
    queryRangeM *= m_maxSignatureScale;
    if (queryRangeM <= 0.f)
        return;

    // Candidates. queryRadius is conservative at cell level; the exact geometry is the cone test.
    struct Candidate {
        uint32_t idx;
        const EntityState* st;
    };
    std::vector<Candidate> candidates;
    si.queryRadius(self.transform.pos, static_cast<double>(queryRangeM), [&](uint32_t idx, const double*) {
        if (idx == self.id.index)
            return; // an observer does not detect itself
        const EntityState* st = m_entityManager.getByIndex(idx);
        if (!st || st->dead)
            return;
        candidates.push_back({idx, st});
    });
    // queryRadius visits cells in hash order, which is not a promise; sort so the per-target work
    // (and therefore nothing at all) depends on cell iteration order.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.idx < b.idx; });

    std::vector<Contact> found;
    found.reserve(candidates.size());

    for (const Candidate& cand : candidates) {
        const EntityState& tgt = *cand.st;
        const EntityDef* tgtDef = m_registry.byIndex(tgt.typeIndex);
        const SignatureDef sig = tgtDef ? tgtDef->signatures : SignatureDef{};

        // Merge every sensor's opinion of this one target into a single contact: the best state any
        // sensor holds. A target found on radar AND visible to the eyeball is one contact, not two —
        // the consumer wants to know what is out there, not which piece of glass saw it.
        ContactState best = ContactState::Lost;
        uint64_t firstDetected = 0;
        uint64_t lastSeen = 0;

        for (uint32_t slot = 0; slot < obs.sensors.size(); ++slot) {
            const SensorDef& def = *obs.sensors[slot];

            const SensorEvaluation ev =
                evaluateSensor(def, obs.emitting, self.transform.pos, self.transform.quat, tgt.transform.pos, sig,
                               obs.skill, env, radarRangeFraction, self.id.index, cand.idx, tickIndex, slot);

            const uint64_t key = trackKey(cand.idx, slot);
            ContactTrack prev{};
            if (auto it = obs.tracks.find(key); it != obs.tracks.end())
                prev = it->second;

            const ContactTrack next = stepContact(prev, ev, def.lockHoldS, work.checkDtS, tickIndex);

            if (next.state == ContactState::Lost)
                obs.tracks.erase(key); // do not accumulate dead keys for every entity ever seen
            else
                obs.tracks[key] = next;

            if (stateRank(next.state) > stateRank(best))
                best = next.state;
            if (next.firstDetectedTick != 0 && (firstDetected == 0 || next.firstDetectedTick < firstDetected))
                firstDetected = next.firstDetectedTick;
            lastSeen = std::max(lastSeen, next.lastSeenTick);
        }

        if (best == ContactState::Lost)
            continue;

        Contact c;
        c.id = tgt.id;
        c.typeIndex = tgt.typeIndex;
        c.factionIndex = tgt.factionIndex;
        c.state = best;
        c.firstDetectedTick = firstDetected;
        c.lastSeenTick = lastSeen;

        // Last-KNOWN state. While the contact is held this is where the target is; while it coasts
        // this is where it WAS, and the consumer is not told the difference by being handed a fresh
        // position it has not earned.
        const Contact* prevContact = obs.contacts.find(tgt.id);
        if (best == ContactState::Coasting && prevContact) {
            std::copy(std::begin(prevContact->lastKnownPos), std::end(prevContact->lastKnownPos),
                      std::begin(c.lastKnownPos));
            std::copy(std::begin(prevContact->lastKnownVel), std::end(prevContact->lastKnownVel),
                      std::begin(c.lastKnownVel));
        } else {
            c.lastKnownPos[0] = tgt.transform.pos[0];
            c.lastKnownPos[1] = tgt.transform.pos[1];
            c.lastKnownPos[2] = tgt.transform.pos[2];
            c.lastKnownVel[0] = tgt.transform.vel[0];
            c.lastKnownVel[1] = tgt.transform.vel[1];
            c.lastKnownVel[2] = tgt.transform.vel[2];
        }

        // Carry the reaction flag across checks; updateReactions() owns flipping it.
        c.reacted = prevContact ? prevContact->reacted : false;
        found.push_back(c);
    }

    // Apply the cap by RELEVANCE, then restore index order. Truncating by index would silently blind
    // an observer to the enemy on its nose because a friendly happened to have a lower entity index.
    if (found.size() > kMaxContactsPerObserver) {
        std::sort(found.begin(), found.end(), [&](const Contact& a, const Contact& b) {
            const int ra = stateRank(a.state);
            const int rb = stateRank(b.state);
            if (ra != rb)
                return ra > rb;
            const double da = dist2(a.lastKnownPos, self.transform.pos);
            const double db = dist2(b.lastKnownPos, self.transform.pos);
            if (da != db)
                return da < db;
            return a.id.index < b.id.index;
        });
        found.resize(kMaxContactsPerObserver);
    }
    std::sort(found.begin(), found.end(), [](const Contact& a, const Contact& b) { return a.id.index < b.id.index; });

    obs.contacts.contacts = std::move(found);

    // A track whose target was not a candidate this check (out of the query radius entirely, or
    // despawned) is dropped rather than left to rot: the query radius is the outer bound of what
    // this observer can ever see, so a track beyond it cannot recover.
    std::vector<uint32_t> candIdx;
    candIdx.reserve(candidates.size());
    for (const Candidate& c : candidates)
        candIdx.push_back(c.idx); // already ascending
    for (auto it = obs.tracks.begin(); it != obs.tracks.end();) {
        const auto targetIdx = static_cast<uint32_t>(it->first >> 32);
        it = std::binary_search(candIdx.begin(), candIdx.end(), targetIdx) ? std::next(it) : obs.tracks.erase(it);
    }

    (void)reactionTimeS; // reaction is applied in updateReactions(), every tick, not on the check
}

void SensorSystem::updateReactions(uint64_t tickIndex, double simDt, float reactionTimeS) {
    if (simDt <= 0.0)
        return;

    for (auto& [idx, obs] : m_observers) {
        if (obs.contacts.empty())
            continue;

        // reaction 0.5 (the AiTuning default) is unity, exactly like skill: an entity that authors no
        // [ai] section reacts in precisely the configured difficulty time, no faster and no slower.
        const float delayS = std::max(0.f, reactionTimeS) * (0.5f + std::clamp(obs.reaction, 0.f, 1.f));
        const auto delayTicks = static_cast<uint64_t>(std::lround(static_cast<double>(delayS) / simDt));

        for (Contact& c : obs.contacts.contacts) {
            if (c.reacted || c.firstDetectedTick == 0)
                continue;
            if (tickIndex >= c.firstDetectedTick + delayTicks)
                c.reacted = true;
        }
    }
}

} // namespace fl::sensor
