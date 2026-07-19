// SPDX-License-Identifier: GPL-3.0-or-later
#include "atc/AtcFacility.h"

#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "flight/LocalFrame.h"

#include <algorithm>

namespace fl::atc {

namespace {
// An arrival within this range of the threshold blocks a new departure (~60 s at approach speed).
constexpr double kDepartureHoldRangeM = 6000.0;
// An arrival this close with the runway occupied by someone else gets waved off (go around).
constexpr double kShortFinalRangeM = 3000.0;
// A departure has cleared the runway once it climbs past this AGL (occupancy release).
constexpr float kOccClearAglM = 40.f;
// Backstop so a stuck occupant (never seen climbing) cannot deadlock the runway forever. ATC ticks
// at 1 Hz, so this is ~90 s of sim time.
constexpr uint64_t kOccTimeoutTicks = 5400;
} // namespace

AtcFacility::AtcFacility(std::string id, std::string speaker, Runway runway, double planetRadiusM, bool acceptsLandings,
                         PoseProvider pose)
    : m_id(std::move(id)), m_speaker(std::move(speaker)), m_runway(runway), m_planetRadiusM(planetRadiusM),
      m_acceptsLandings(acceptsLandings), m_pose(std::move(pose)) {}

FacilityPose AtcFacility::holdShortPose() const {
    // A short distance before the threshold, aligned with the runway heading.
    FacilityPose p;
    p.origin = m_runway.threshold - m_runway.centerlineDir * 25.0;
    p.headingDeg = m_runway.headingDeg;
    return p;
}

void AtcFacility::setClearance(fl::EntityId flight, ClearanceState s) {
    m_clearance[flight.index] = {flight, s};
}

ClearanceState AtcFacility::clearanceState(fl::EntityId flight) const {
    auto it = m_clearance.find(flight.index);
    if (it == m_clearance.end() || it->second.id != flight)
        return ClearanceState::None;
    return it->second.state;
}

double AtcFacility::horizDistToThreshold(const double pos[3]) const {
    const glm::dvec3 p(pos[0], pos[1], pos[2]);
    const glm::dvec3 up(fl::radialUp(p, m_planetRadiusM));
    const glm::dvec3 d = m_runway.threshold - p;
    const glm::dvec3 horiz = d - glm::dot(d, up) * up;
    return glm::length(horiz);
}

double AtcFacility::aglOf(const double pos[3]) const {
    const glm::dvec3 p(pos[0], pos[1], pos[2]);
    return fl::localAltitude(p, m_planetRadiusM) - m_runway.elevationM;
}

void AtcFacility::requestTakeoff(fl::EntityId flight) {
    if (!flight.valid())
        return;
    if (std::find(m_departures.begin(), m_departures.end(), flight) == m_departures.end() && m_occupant != flight)
        m_departures.push_back(flight);
    // Do not downgrade a flight that is already cleared/rolling.
    if (clearanceState(flight) == ClearanceState::None)
        setClearance(flight, ClearanceState::HoldShort);
}

void AtcFacility::requestLanding(fl::EntityId flight) {
    if (!flight.valid() || !m_acceptsLandings)
        return;
    if (std::find(m_arrivals.begin(), m_arrivals.end(), flight) == m_arrivals.end())
        m_arrivals.push_back(flight);
    const ClearanceState cur = clearanceState(flight);
    if (cur != ClearanceState::ClearedToLand)
        setClearance(flight, ClearanceState::Pattern);
}

void AtcFacility::declareInbound(fl::EntityId flight, std::vector<RadioTransmission>& outbox) {
    if (!flight.valid() || !m_acceptsLandings)
        return;
    if (std::find(m_arrivals.begin(), m_arrivals.end(), flight) == m_arrivals.end())
        m_arrivals.push_back(flight);
    setClearance(flight, ClearanceState::Inbound);
    outbox.push_back(makeTransmission(AtcPhrase::ContactApproach, m_speaker, flight));
}

void AtcFacility::removeFlight(fl::EntityId flight) {
    m_departures.erase(std::remove(m_departures.begin(), m_departures.end(), flight), m_departures.end());
    m_arrivals.erase(std::remove(m_arrivals.begin(), m_arrivals.end(), flight), m_arrivals.end());
    m_clearance.erase(flight.index);
    if (m_occupant == flight) {
        m_occupant = {};
        m_occKind = OccupancyKind::None;
    }
}

void AtcFacility::update(const fl::EntityManager& em, uint64_t tick, std::vector<RadioTransmission>& outbox) {
    // 1. Prune flights whose entity is gone; drop stale clearance entries.
    auto gone = [&em](fl::EntityId id) {
        const EntityState* s = em.get(id);
        return s == nullptr || s->dead;
    };
    m_departures.erase(std::remove_if(m_departures.begin(), m_departures.end(), gone), m_departures.end());
    m_arrivals.erase(std::remove_if(m_arrivals.begin(), m_arrivals.end(), gone), m_arrivals.end());
    for (auto it = m_clearance.begin(); it != m_clearance.end();) {
        if (gone(it->second.id))
            it = m_clearance.erase(it);
        else
            ++it;
    }
    if (m_occKind != OccupancyKind::None && gone(m_occupant)) {
        m_occupant = {};
        m_occKind = OccupancyKind::None;
    }

    // 2. Release occupancy once a departure has climbed clear, or on the deadlock backstop.
    if (m_occKind == OccupancyKind::Departure) {
        const EntityState* s = em.get(m_occupant);
        const bool climbedOut = s && aglOf(s->transform.pos) > kOccClearAglM;
        if (climbedOut || tick - m_occSinceTick > kOccTimeoutTicks) {
            setClearance(m_occupant, ClearanceState::Departed);
            m_occupant = {};
            m_occKind = OccupancyKind::None;
        }
    } else if (m_occKind == OccupancyKind::Arrival) {
        // Release when the lander has slowed to a stop (or timed out).
        const EntityState* s = em.get(m_occupant);
        bool stopped = false;
        if (s) {
            const glm::vec3 v(s->transform.vel[0], s->transform.vel[1], s->transform.vel[2]);
            stopped = glm::length(v) < 3.f && aglOf(s->transform.pos) < 5.0;
        }
        if (stopped || tick - m_occSinceTick > kOccTimeoutTicks) {
            if (s)
                setClearance(m_occupant, ClearanceState::Landed);
            m_occupant = {};
            m_occKind = OccupancyKind::None;
        }
    }

    // 3. Sequence arrivals by distance to the threshold (nearest first).
    std::sort(m_arrivals.begin(), m_arrivals.end(), [&](fl::EntityId a, fl::EntityId b) {
        const EntityState* sa = em.get(a);
        const EntityState* sb = em.get(b);
        if (!sa)
            return false;
        if (!sb)
            return true;
        return horizDistToThreshold(sa->transform.pos) < horizDistToThreshold(sb->transform.pos);
    });

    const bool haveArrival = !m_arrivals.empty();
    double nearestArrivalDist = 1e30;
    if (haveArrival) {
        if (const EntityState* s = em.get(m_arrivals.front()))
            nearestArrivalDist = horizDistToThreshold(s->transform.pos);
    }
    const bool arrivalNear = haveArrival && nearestArrivalDist < kDepartureHoldRangeM;
    const bool arrivalShortFinal = haveArrival && nearestArrivalDist < kShortFinalRangeM;

    // 4. Grant clearances. Arrivals win the runway over departures — you cannot hold a landing.
    if (m_occKind == OccupancyKind::None) {
        if (haveArrival) {
            const fl::EntityId lander = m_arrivals.front();
            if (clearanceState(lander) != ClearanceState::ClearedToLand) {
                setClearance(lander, ClearanceState::ClearedToLand);
                outbox.push_back(makeTransmission(AtcPhrase::ClearedToLand, m_speaker, lander));
            }
            m_occupant = lander;
            m_occKind = OccupancyKind::Arrival;
            m_occSinceTick = tick;
        } else if (!m_hold && !m_departures.empty() && !arrivalNear) {
            const fl::EntityId dep = m_departures.front();
            m_departures.pop_front();
            setClearance(dep, ClearanceState::ClearedTakeoff);
            outbox.push_back(makeTransmission(AtcPhrase::ClearedTakeoff, m_speaker, dep));
            m_occupant = dep;
            m_occKind = OccupancyKind::Departure;
            m_occSinceTick = tick;
        }
    } else {
        // Runway occupied by someone other than the aircraft on short final: wave it off.
        if (arrivalShortFinal && m_arrivals.front() != m_occupant &&
            clearanceState(m_arrivals.front()) != ClearanceState::GoAround) {
            setClearance(m_arrivals.front(), ClearanceState::GoAround);
            outbox.push_back(makeTransmission(AtcPhrase::GoAround, m_speaker, m_arrivals.front()));
        }
    }
}

} // namespace fl::atc
