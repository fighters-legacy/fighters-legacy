// SPDX-License-Identifier: GPL-3.0-or-later
#include "atc/AtcFacility.h"

#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "flight/LocalFrame.h"

#include <algorithm>

namespace fl::atc {

namespace {
// An arrival within this range of the threshold blocks a new departure (~60 s at approach speed) —
// and, since #1154, is also the range at which it may be CLEARED to land. One range, one rule: an
// arrival claims the runway exactly when it is close enough that a departure could not get out
// first. Beyond it the flight is sequenced but not occupying, so queued departures still go.
constexpr double kDepartureHoldRangeM = 6000.0;
// An arrival this close with the runway occupied by someone else gets waved off (go around).
constexpr double kShortFinalRangeM = 3000.0;
// A departure has cleared the runway once it climbs past this AGL (occupancy release).
constexpr float kOccClearAglM = 40.f;
// Occupancy budgets. Backstops so an occupant that is never seen finishing cannot deadlock the
// runway forever. ATC ticks at 1 Hz and the sim at 60 Hz, so these are sim ticks.
//
// They differ because the jobs differ. A departure has ~90 s to get airborne from a standing start.
// An arrival is cleared at up to kDepartureHoldRangeM and then has to fly the approach, flare, roll
// out and brake to a stop — 80 s at the 75 m/s spec approach speed, and half as fast again for a
// slow aircraft. Sharing the departure's budget would wave off legitimate slow finals (#1154).
constexpr uint64_t kDepartureOccTimeoutTicks = 5400; // 90 s
constexpr uint64_t kArrivalOccTimeoutTicks = 14400;  // 240 s
// How long a waved-off flight stays out of the arrival sequence. Long enough for a departure to
// spend its own budget, so the wave-off actually buys the field a turn (#1154).
constexpr uint64_t kGoAroundHoldOffTicks = 7200; // 120 s
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
    Clearance& c = m_clearance[flight.index];
    // A hold-off outlives a state change. It is ATC's instruction to break off, so a pilot cannot
    // shed it by re-requesting the landing, and stamping GoAround must not clear the one that goes
    // with it. It is dropped only with the whole record — cancel, or the entity dying (#1154).
    if (c.id != flight)
        c.holdOffUntilTick = 0; // a recycled pool slot is a different flight
    c.id = flight;
    c.state = s;
}

void AtcFacility::holdOff(fl::EntityId flight, uint64_t untilTick) {
    Clearance& c = m_clearance[flight.index];
    c.id = flight; // the slot may be fresh; sequenceable() keys on the id matching
    c.holdOffUntilTick = untilTick;
}

bool AtcFacility::sequenceable(fl::EntityId flight, uint64_t tick) const {
    auto it = m_clearance.find(flight.index);
    if (it == m_clearance.end() || it->second.id != flight)
        return true; // unknown to the clearance table = never waved off
    return tick >= it->second.holdOffUntilTick;
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
    // Do not downgrade a flight that is already cleared/rolling. A TERMINAL state is not a live
    // clearance, though: a flight that has landed (or departed and come back) is asking for a new
    // one, and would otherwise sit in the queue still reading `landed` (#1149).
    const ClearanceState cur = clearanceState(flight);
    if (cur == ClearanceState::None || cur == ClearanceState::Landed || cur == ClearanceState::Departed)
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
        if (climbedOut || tick - m_occSinceTick > kDepartureOccTimeoutTicks) {
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
        if (stopped) {
            // Down and stopped: the landing is COMPLETE, so retire the flight from the arrival
            // sequence. Without this it is still the nearest arrival, so step 4 below re-clears it
            // to land and re-takes the runway on this very tick — for the rest of the session, since
            // arrivals beat departures (#1149).
            const fl::EntityId lander = m_occupant;
            m_occupant = {};
            m_occKind = OccupancyKind::None;
            setClearance(lander, ClearanceState::Landed);
            m_arrivals.erase(std::remove(m_arrivals.begin(), m_arrivals.end(), lander), m_arrivals.end());
            outbox.push_back(makeTransmission(AtcPhrase::TaxiToParking, m_speaker, lander));
        } else if (tick - m_occSinceTick > kArrivalOccTimeoutTicks) {
            // Over budget and still not down — a hover, an approach that never works out, a flight
            // that turned away. Releasing the runway alone achieves nothing: the flight is still the
            // nearest arrival, so it re-takes it on this tick and starves the field forever (#1154).
            //
            // So intervene only when someone is actually starved. With the field otherwise empty
            // nobody is waiting, and a slow aircraft still working the approach deserves the runway:
            // re-arm the budget and leave it alone. With traffic waiting, wave it off and hold it out
            // of the sequence long enough for that traffic to get a turn.
            const bool anotherArrivalWaiting = std::any_of(m_arrivals.begin(), m_arrivals.end(), [&](fl::EntityId id) {
                return id != m_occupant && sequenceable(id, tick);
            });
            const bool departureWaiting = !m_hold && !m_departures.empty();
            if (anotherArrivalWaiting || departureWaiting) {
                const fl::EntityId waved = m_occupant;
                m_occupant = {};
                m_occKind = OccupancyKind::None;
                setClearance(waved, ClearanceState::GoAround);
                holdOff(waved, tick + kGoAroundHoldOffTicks);
                outbox.push_back(makeTransmission(AtcPhrase::GoAround, m_speaker, waved));
            } else {
                m_occSinceTick = tick;
            }
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

    // The head of the sequence is the nearest arrival that is not serving a wave-off. A held-off
    // flight is invisible to everything below — it neither takes the runway nor holds a departure on
    // the ground, which is what stops one that refuses to leave from owning the field (#1154).
    auto head =
        std::find_if(m_arrivals.begin(), m_arrivals.end(), [&](fl::EntityId id) { return sequenceable(id, tick); });
    const bool haveArrival = head != m_arrivals.end();
    double nearestArrivalDist = 1e30;
    if (haveArrival) {
        if (const EntityState* s = em.get(*head))
            nearestArrivalDist = horizDistToThreshold(s->transform.pos);
    }
    const bool arrivalNear = haveArrival && nearestArrivalDist < kDepartureHoldRangeM;
    const bool arrivalShortFinal = haveArrival && nearestArrivalDist < kShortFinalRangeM;

    // 4. Grant clearances. Arrivals win the runway over departures — you cannot hold a landing.
    if (m_occKind == OccupancyKind::None) {
        // ...but only from inside the range where a departure could not have got out first. An
        // arrival 30 km out used to claim the runway for the whole approach, blocking every
        // departure and blowing the occupancy budget on the way in (#1154). Further out it stays
        // sequenced, in the pattern, and the field keeps working.
        if (arrivalNear) {
            const fl::EntityId lander = *head;
            if (clearanceState(lander) != ClearanceState::ClearedToLand) {
                setClearance(lander, ClearanceState::ClearedToLand);
                outbox.push_back(makeTransmission(AtcPhrase::ClearedToLand, m_speaker, lander));
            }
            m_occupant = lander;
            m_occKind = OccupancyKind::Arrival;
            m_occSinceTick = tick;
        } else if (!m_hold && !m_departures.empty()) {
            const fl::EntityId dep = m_departures.front();
            m_departures.pop_front();
            setClearance(dep, ClearanceState::ClearedTakeoff);
            outbox.push_back(makeTransmission(AtcPhrase::ClearedTakeoff, m_speaker, dep));
            m_occupant = dep;
            m_occKind = OccupancyKind::Departure;
            m_occSinceTick = tick;
        }
    } else {
        // Runway occupied by someone other than the aircraft on short final: wave it off. The
        // hold-off is what makes that mean something — without it the flight is re-cleared to land
        // the instant the runway frees, while it is still climbing away from the go-around (#1154).
        if (arrivalShortFinal && *head != m_occupant && clearanceState(*head) != ClearanceState::GoAround) {
            const fl::EntityId waved = *head;
            setClearance(waved, ClearanceState::GoAround);
            holdOff(waved, tick + kGoAroundHoldOffTicks);
            outbox.push_back(makeTransmission(AtcPhrase::GoAround, m_speaker, waved));
        }
    }
}

} // namespace fl::atc
