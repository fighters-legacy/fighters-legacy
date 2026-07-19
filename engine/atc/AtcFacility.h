// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "atc/AtcTypes.h"
#include "entity/EntityId.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {
class EntityManager;
} // namespace fl

namespace fl::atc {

// One controlling facility bound to a single runway of an airport (or a carrier deck later). It owns
// the deterministic sequencing FSM: a departure FIFO, an arrival sequence ordered by distance to the
// threshold, single-runway occupancy, and a per-flight ClearanceState. `update()` advances the FSM
// one ATC step and appends any radio lines to `outbox`. Player flights participate identically —
// touchdown/occupancy is read from EntityState, never from who is flying.
//
// The pose is supplied through a std::function so a static airport passes a constant field pose while
// a moving carrier entity (the #38 seam) supplies a live one, with no change to this logic.
class AtcFacility {
  public:
    using PoseProvider = std::function<FacilityPose()>;

    struct Runway {
        glm::dvec3 threshold{0.0};               // departure/landing end
        glm::dvec3 oppositeEnd{0.0};             // far end
        glm::dvec3 centerlineDir{1.0, 0.0, 0.0}; // unit threshold -> oppositeEnd
        float headingDeg{90.f};
        double elevationM{0.0};
    };

    enum class OccupancyKind : uint8_t { None, Departure, Arrival };

    AtcFacility(std::string id, std::string speaker, Runway runway, double planetRadiusM, bool acceptsLandings,
                PoseProvider pose);

    // Sim-thread request entry points (AtcService serialises access). Idempotent — a repeat request
    // for a flight already in the queue is ignored.
    void requestTakeoff(fl::EntityId flight);
    void requestLanding(fl::EntityId flight);
    void declareInbound(fl::EntityId flight, std::vector<RadioTransmission>& outbox);
    void setHoldDepartures(bool hold) noexcept {
        m_hold = hold;
    }
    void removeFlight(fl::EntityId flight);

    // Advance the FSM one ATC step against the live world; append transmissions to outbox.
    void update(const fl::EntityManager& em, uint64_t tick, std::vector<RadioTransmission>& outbox);

    [[nodiscard]] ClearanceState clearanceState(fl::EntityId flight) const;
    [[nodiscard]] const std::string& id() const noexcept {
        return m_id;
    }
    [[nodiscard]] const std::string& speaker() const noexcept {
        return m_speaker;
    }
    [[nodiscard]] const Runway& runway() const noexcept {
        return m_runway;
    }
    [[nodiscard]] bool acceptsLandings() const noexcept {
        return m_acceptsLandings;
    }
    // Hold-short spawn pose (a short distance before the threshold, on the runway heading).
    [[nodiscard]] FacilityPose holdShortPose() const;

    // Telemetry for `atc_status`.
    [[nodiscard]] std::size_t departureQueueDepth() const noexcept {
        return m_departures.size();
    }
    [[nodiscard]] std::size_t arrivalCount() const noexcept {
        return m_arrivals.size();
    }
    [[nodiscard]] bool runwayOccupied() const noexcept {
        return m_occKind != OccupancyKind::None;
    }
    [[nodiscard]] fl::EntityId occupant() const noexcept {
        return m_occupant;
    }
    [[nodiscard]] bool holding() const noexcept {
        return m_hold;
    }

  private:
    void setClearance(fl::EntityId flight, ClearanceState s);
    [[nodiscard]] double horizDistToThreshold(const double pos[3]) const;
    [[nodiscard]] double aglOf(const double pos[3]) const;

    std::string m_id;
    std::string m_speaker;
    Runway m_runway;
    double m_planetRadiusM;
    bool m_acceptsLandings;
    PoseProvider m_pose;

    std::deque<fl::EntityId> m_departures; // FIFO awaiting takeoff clearance
    std::vector<fl::EntityId> m_arrivals;  // sequenced by distance-to-threshold in update()
    struct Clearance {
        fl::EntityId id;
        ClearanceState state{ClearanceState::None};
    };
    std::unordered_map<uint32_t, Clearance> m_clearance; // keyed by entity pool index

    fl::EntityId m_occupant{};
    OccupancyKind m_occKind{OccupancyKind::None};
    uint64_t m_occSinceTick{0};
    bool m_hold{false};
};

} // namespace fl::atc
