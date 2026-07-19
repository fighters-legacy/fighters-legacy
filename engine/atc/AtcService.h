// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "atc/AtcFacility.h"
#include "atc/AtcTypes.h"
#include "entity/EntityId.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {
class EntityManager;
class AirportRegistry;
} // namespace fl

namespace fl::atc {

// The server-authoritative ATC brain (#702). A pure deterministic FSM — no model involvement — built
// on top of the AirportRegistry. Facilities are created LAZILY (only for airports a flight actually
// uses), so an 80k-airport registry costs nothing until traffic appears there.
//
// Threading: every public method takes an internal mutex, so requests may arrive from the sim thread
// (admin callbacks), the AI pass (Lua atc.* bindings run on worker threads), or a scramble, while
// tick() runs in WorldBroadcaster's serial Maintenance phase. The critical sections are tiny (a map
// lookup and a few queue ops) — appropriate for an arcade-weight service.
class AtcService {
  public:
    // ATC steps at 1 Hz; WorldBroadcaster gates its tick() call on this.
    static constexpr uint64_t kIntervalTicks = 60;

    // What a scramble hands to the spawn handler: enough to place the aircraft at the hold-short and
    // build its TakeoffController. The handler (fl-server) spawns, registers a departure controller,
    // and calls requestTakeoff — the ATC service never touches EntityManager::spawn directly.
    struct DepartureSpawn {
        std::string typeId;
        std::string facilityId;
        FacilityPose holdShort;
        AtcFacility::Runway runway;
    };
    using SpawnHandler = std::function<void(const DepartureSpawn&)>;

    // em/registry must outlive the service. Both are only read.
    AtcService(const fl::EntityManager& em, const fl::AirportRegistry& registry, double planetRadiusM);

    void setSpawnHandler(SpawnHandler h);

    // Requests. facilityId empty = the facility nearest the flight's current position. Safe to call
    // from any thread.
    void requestTakeoff(fl::EntityId flight, const std::string& facilityId = {});
    void requestLanding(fl::EntityId flight, const std::string& facilityId = {});
    void declareInbound(fl::EntityId flight, const std::string& facilityId = {});
    // Drop a flight from every facility's queues (a pilot cancelling a request).
    void cancel(fl::EntityId flight);
    void holdDepartures(const std::string& facilityId, bool hold);

    // Spawn `count` departures from `facilityId`, placed hold-short. Returns false if the airport is
    // unknown or no spawn handler is wired. Sim-thread only (invokes the spawn handler).
    bool scramble(const std::string& facilityId, const std::string& typeId, int count);

    // Polled by the AI departure/arrival compositions and `atc_status`. None if the flight is unknown.
    [[nodiscard]] ClearanceState clearanceState(fl::EntityId flight) const;

    // Advance every active facility one ATC step. Called by WorldBroadcaster when
    // tickIndex % kIntervalTicks == 0.
    void tick(const fl::EntityManager& em, uint64_t tickIndex);

    // Take and clear the pending radio transmissions (WorldBroadcaster routes them to clients).
    [[nodiscard]] std::vector<RadioTransmission> drainTransmissions();

    // Human-readable multi-line status of every active facility (queues, occupancy, hold). For
    // `atc_status`. `airportFilter` empty = all active facilities.
    [[nodiscard]] std::string statusText(const std::string& airportFilter = {}) const;

    // Facility count currently instantiated (test/telemetry).
    [[nodiscard]] std::size_t activeFacilityCount() const;

  private:
    // Must be called with m_mutex held. Returns nullptr if the airport id is not in the registry.
    AtcFacility* getOrCreateFacility(const std::string& airportId);
    // Nearest airport id to a flight (empty if none / flight gone). Called with m_mutex held.
    [[nodiscard]] std::string nearestAirportId(fl::EntityId flight) const;

    const fl::EntityManager& m_em;
    const fl::AirportRegistry& m_registry;
    double m_planetRadiusM;
    SpawnHandler m_spawnHandler;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::unique_ptr<AtcFacility>> m_facilities;
    std::vector<RadioTransmission> m_outbox;
};

} // namespace fl::atc
