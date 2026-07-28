// SPDX-License-Identifier: GPL-3.0-or-later
#include "mission_validator.h"

#include "entity/EntityDef.h"
#include "entity/EntityDefParser.h"
#include "mission/Mission.h"
#include "mission/MissionParser.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace fl {

namespace fs = std::filesystem;

// The schema-only overload now lives in engine-mission (MissionValidator.cpp) so fl-server's MCP
// submit_mission tool can reach it; this file keeps the --pack cross-check that needs a pack on disk.

namespace {
// Load every entities/*.toml in a pack into an id -> EntityDef map (only the fields parseEntityDef
// reads matter here — we need each def's declared [[crew]]). A def that fails to parse is skipped; the
// dedicated validate-entity --pack surfaces its errors, so we do not double-report them.
std::unordered_map<std::string, EntityDef> loadPackEntityDefs(const std::string& packDir) {
    std::unordered_map<std::string, EntityDef> byId;
    const fs::path entitiesDir = fs::path(packDir) / "entities";
    std::error_code ec;
    if (!fs::is_directory(entitiesDir, ec))
        return byId;
    for (const auto& entry : fs::recursive_directory_iterator(entitiesDir, ec)) {
        if (ec || !entry.is_regular_file() || entry.path().extension() != ".toml")
            continue;
        std::ifstream f(entry.path());
        if (!f)
            continue;
        std::ostringstream ss;
        ss << f.rdbuf();
        try {
            EntityDef def = parseEntityDef(ss.str());
            if (!def.id.empty())
                byId.emplace(def.id, std::move(def));
        } catch (...) {
            // parse error — validate-entity owns reporting it.
        }
    }
    return byId;
}
} // namespace

MissionValidationResult validateMission(std::string_view yamlContent, const std::string& packDir) {
    MissionParseResult parsed = parseMission(yamlContent);
    MissionValidationResult r;
    r.ok = parsed.ok;
    r.errors = std::move(parsed.errors);
    r.warnings = std::move(parsed.warnings);
    if (packDir.empty())
        return r;

    const std::unordered_map<std::string, EntityDef> defs = loadPackEntityDefs(packDir);

    for (std::size_t i = 0; i < parsed.mission.objects.size(); ++i) {
        const MissionObject& obj = parsed.mission.objects[i];
        if (!obj.crew)
            continue;
        const std::string where = "objects[" + std::to_string(i) + "].crew";
        const auto dit = defs.find(obj.type);
        if (dit == defs.end()) {
            r.warnings.push_back(where + ": entity type '" + obj.type +
                                 "' not found in the pack — crew cross-check skipped (may be builtin)");
            continue;
        }
        const EntityDef& def = dit->second;
        if (def.crew.empty()) {
            r.errors.push_back(where + ": entity type '" + obj.type +
                               "' declares no [[crew]] seats, but a crew: block was given");
            r.ok = false;
            continue;
        }
        for (std::size_t s = 0; s < obj.crew->seats.size(); ++s) {
            const MissionCrewSeat& ms = obj.crew->seats[s];
            const std::string sw = where + ".seats[" + std::to_string(s) + "]";
            if (ms.seatIndex >= 0) {
                if (static_cast<std::size_t>(ms.seatIndex) >= def.crew.size()) {
                    r.errors.push_back(sw + ": seat index " + std::to_string(ms.seatIndex) + " is out of range ('" +
                                       obj.type + "' has " + std::to_string(def.crew.size()) + " seats)");
                    r.ok = false;
                }
            } else if (!ms.role.empty()) {
                bool found = false;
                for (const SeatDef& sd : def.crew)
                    if (sd.role == ms.role) {
                        found = true;
                        break;
                    }
                if (!found) {
                    r.errors.push_back(sw + ": role '" + ms.role + "' is not a seat on entity type '" + obj.type + "'");
                    r.ok = false;
                }
            }
        }
    }
    return r;
}

} // namespace fl
