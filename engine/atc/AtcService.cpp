// SPDX-License-Identifier: GPL-3.0-or-later
#include "atc/AtcService.h"

#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "world/AirportRegistry.h"

#include <cstdio>

namespace fl::atc {

AtcService::AtcService(const fl::EntityManager& em, const fl::AirportRegistry& registry, double planetRadiusM)
    : m_em(em), m_registry(registry), m_planetRadiusM(planetRadiusM) {}

void AtcService::setSpawnHandler(SpawnHandler h) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_spawnHandler = std::move(h);
}

AtcFacility* AtcService::getOrCreateFacility(const std::string& airportId) {
    if (airportId.empty())
        return nullptr;
    if (auto it = m_facilities.find(airportId); it != m_facilities.end())
        return it->second.get();

    const fl::ResolvedAirport* ap = m_registry.byId(airportId);
    if (!ap || ap->runways.empty())
        return nullptr;

    // Use the first runway (arcade-weight: no wind-based selection). Static pose from the field.
    const fl::ResolvedRunway& rw = ap->runways.front();
    AtcFacility::Runway r;
    r.threshold = rw.threshold;
    r.oppositeEnd = rw.oppositeEnd;
    r.centerlineDir = rw.centerlineDir;
    r.headingDeg = rw.headingDeg;
    r.elevationM = ap->elevationM;

    const std::string speaker = (ap->def.name.empty() ? ap->def.id : ap->def.name) + " Tower";
    const FacilityPose pose{ap->worldPos, rw.headingDeg};
    auto fac = std::make_unique<AtcFacility>(airportId, speaker, r, m_planetRadiusM, ap->def.acceptsLandings,
                                             [pose]() { return pose; });
    AtcFacility* raw = fac.get();
    m_facilities.emplace(airportId, std::move(fac));
    return raw;
}

std::string AtcService::nearestAirportId(fl::EntityId flight) const {
    const fl::EntityState* s = m_em.get(flight);
    if (!s)
        return {};
    const fl::ResolvedAirport* ap = m_registry.nearestTo(s->transform.pos[0], s->transform.pos[2], 1.0e9);
    return ap ? ap->def.id : std::string{};
}

void AtcService::requestTakeoff(fl::EntityId flight, const std::string& facilityId) {
    std::lock_guard<std::mutex> lk(m_mutex);
    const std::string fid = facilityId.empty() ? nearestAirportId(flight) : facilityId;
    if (AtcFacility* f = getOrCreateFacility(fid))
        f->requestTakeoff(flight);
}

void AtcService::requestLanding(fl::EntityId flight, const std::string& facilityId) {
    std::lock_guard<std::mutex> lk(m_mutex);
    const std::string fid = facilityId.empty() ? nearestAirportId(flight) : facilityId;
    if (AtcFacility* f = getOrCreateFacility(fid))
        f->requestLanding(flight);
}

void AtcService::declareInbound(fl::EntityId flight, const std::string& facilityId) {
    std::lock_guard<std::mutex> lk(m_mutex);
    const std::string fid = facilityId.empty() ? nearestAirportId(flight) : facilityId;
    if (AtcFacility* f = getOrCreateFacility(fid))
        f->declareInbound(flight, m_outbox);
}

void AtcService::cancel(fl::EntityId flight) {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& [id, f] : m_facilities)
        f->removeFlight(flight);
}

void AtcService::holdDepartures(const std::string& facilityId, bool hold) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (AtcFacility* f = getOrCreateFacility(facilityId))
        f->setHoldDepartures(hold);
}

bool AtcService::scramble(const std::string& facilityId, const std::string& typeId, int count) {
    std::lock_guard<std::mutex> lk(m_mutex);
    AtcFacility* f = getOrCreateFacility(facilityId);
    if (!f || !m_spawnHandler || count <= 0)
        return false;
    for (int i = 0; i < count; ++i) {
        DepartureSpawn spawn;
        spawn.typeId = typeId;
        spawn.facilityId = facilityId;
        spawn.holdShort = f->holdShortPose();
        spawn.runway = f->runway();
        m_spawnHandler(spawn);
    }
    return true;
}

ClearanceState AtcService::clearanceState(fl::EntityId flight) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& [id, f] : m_facilities) {
        const ClearanceState s = f->clearanceState(flight);
        if (s != ClearanceState::None)
            return s;
    }
    return ClearanceState::None;
}

void AtcService::tick(const fl::EntityManager& em, uint64_t tickIndex) {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& [id, f] : m_facilities)
        f->update(em, tickIndex, m_outbox);
}

std::vector<RadioTransmission> AtcService::drainTransmissions() {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<RadioTransmission> out;
    out.swap(m_outbox);
    return out;
}

std::string AtcService::statusText(const std::string& airportFilter) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_facilities.empty())
        return "ATC: no active facilities";
    std::string out = "ATC facilities:";
    for (const auto& [id, f] : m_facilities) {
        if (!airportFilter.empty() && id != airportFilter)
            continue;
        char line[192];
        std::snprintf(line, sizeof(line), "\n  %s: departures=%zu arrivals=%zu runway=%s%s", id.c_str(),
                      f->departureQueueDepth(), f->arrivalCount(), f->runwayOccupied() ? "occupied" : "free",
                      f->holding() ? " [HOLD]" : "");
        out += line;
    }
    return out;
}

std::size_t AtcService::activeFacilityCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_facilities.size();
}

} // namespace fl::atc
