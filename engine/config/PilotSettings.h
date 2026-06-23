// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

struct PilotProfile {
    std::string callsign = "Pilot";
    std::string guid; // UUID-v4, auto-generated on first save
    int kills = 0;
    int losses = 0;
    int64_t flightTimeS = 0; // total accumulated seconds
};

struct PilotCampaignState {
    std::string activeCampaign; // content-pack id, empty = none
    int currentMission = 0;
    std::vector<std::string> completed;                    // completed mission ids
    std::unordered_map<std::string, int> factionStandings; // faction_id -> reputation
};

struct PilotSettings {
    PilotProfile profile;
    PilotCampaignState campaign;
};

} // namespace fl
