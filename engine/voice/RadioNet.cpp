// SPDX-License-Identifier: GPL-3.0-or-later
#include "voice/RadioNet.h"

#include <algorithm>

namespace fl {

const char* radioNetKindName(RadioNetKind kind) noexcept {
    switch (kind) {
    case RadioNetKind::Global:
        return "global";
    case RadioNetKind::Team:
        return "team";
    case RadioNetKind::Flight:
        return "flight";
    case RadioNetKind::Proximity:
        return "proximity";
    case RadioNetKind::Atc:
        return "atc";
    }
    return "unknown";
}

bool radioNetKindFromString(std::string_view s, RadioNetKind& out) noexcept {
    if (s == "global") {
        out = RadioNetKind::Global;
    } else if (s == "team") {
        out = RadioNetKind::Team;
    } else if (s == "flight") {
        out = RadioNetKind::Flight;
    } else if (s == "proximity") {
        out = RadioNetKind::Proximity;
    } else if (s == "atc") {
        out = RadioNetKind::Atc;
    } else {
        return false;
    }
    return true;
}

uint8_t RadioNetTable::add(RadioNetDef def) {
    if (def.id.empty() || m_nets.size() >= kMaxRadioNets)
        return kInvalidRadioNet;
    if (indexOf(def.id) != kInvalidRadioNet)
        return kInvalidRadioNet;
    if (def.id.size() > kMaxRadioNetIdChars)
        def.id.resize(kMaxRadioNetIdChars);
    if (def.name.empty())
        def.name = def.id;
    if (def.name.size() > kMaxRadioNetNameChars)
        def.name.resize(kMaxRadioNetNameChars);
    def.gain = std::clamp(def.gain, 0.f, 4.f);
    def.rangeM = std::max(0.f, def.rangeM);
    const auto index = static_cast<uint8_t>(m_nets.size());
    m_nets.push_back(std::move(def));
    return index;
}

const RadioNetDef* RadioNetTable::byIndex(uint8_t netId) const noexcept {
    if (netId >= m_nets.size())
        return nullptr;
    return &m_nets[netId];
}

uint8_t RadioNetTable::indexOf(std::string_view id) const noexcept {
    for (std::size_t i = 0; i < m_nets.size(); ++i) {
        if (m_nets[i].id == id)
            return static_cast<uint8_t>(i);
    }
    return kInvalidRadioNet;
}

uint8_t RadioNetTable::defaultIndex() const noexcept {
    if (m_nets.empty())
        return kInvalidRadioNet;
    for (std::size_t i = 0; i < m_nets.size(); ++i) {
        if (m_nets[i].defaultNet)
            return static_cast<uint8_t>(i);
    }
    for (std::size_t i = 0; i < m_nets.size(); ++i) {
        if (m_nets[i].kind == RadioNetKind::Team)
            return static_cast<uint8_t>(i);
    }
    return 0u;
}

std::vector<RadioNetDef> builtinRadioNets() {
    std::vector<RadioNetDef> nets;
    // TEAM first: it is both the default PTT net and the one that works in every game mode,
    // including a single-faction sandbox where formations and proximity have nothing to say.
    nets.push_back(RadioNetDef{"team", "TEAM", RadioNetKind::Team, /*positional=*/false, /*rangeM=*/0.f,
                               /*radioEffect=*/true, /*gain=*/1.f, /*defaultNet=*/true});
    nets.push_back(RadioNetDef{"flight", "FLIGHT", RadioNetKind::Flight, false, 0.f, true, 1.f, false});
    nets.push_back(RadioNetDef{"atc", "ATC", RadioNetKind::Atc, false, 0.f, true, 1.f, false});
    // Proximity is the only positional net by default: hearing a nearby aircraft's transmission come
    // from its bearing is the whole point, and it is the one net where "nearby" is defined.
    nets.push_back(RadioNetDef{"proximity", "PROX", RadioNetKind::Proximity, /*positional=*/true,
                               /*rangeM=*/3000.f, /*radioEffect=*/true, /*gain=*/1.f, /*defaultNet=*/false});
    return nets;
}

} // namespace fl
