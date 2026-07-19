// SPDX-License-Identifier: GPL-3.0-or-later
#include "campaign/CampaignRunner.h"

#include "campaign/MissionTemplate.h"

namespace fl {

CampaignRunner::CampaignRunner(CampaignDef def, uint64_t seed, ContentLoader content,
                               CampaignEngine::FrontlineLoader frontline, double planetRadiusM)
    : m_name(def.name), m_engine(std::move(def), seed, std::move(frontline), planetRadiusM),
      m_content(std::move(content)) {}

std::optional<std::string> CampaignRunner::nextMissionYaml(std::string& outMissionId) {
    const NextMission nm = m_engine.nextMission();
    outMissionId = nm.missionId;
    if (nm.kind == NextMission::Kind::None || nm.missionFile.empty())
        return std::nullopt; // campaign complete (nothing armed / no unlocked theater)

    std::optional<std::string> bytes = m_content ? m_content(nm.missionFile) : std::nullopt;
    if (!bytes)
        return std::nullopt; // missing content — the caller logs outMissionId

    if (nm.kind == NextMission::Kind::Story)
        return bytes; // a story mission file is a plain mission already

    // A dynamic sortie: the loaded file is a template; materialize its ${...} fills into a concrete
    // mission that the ordinary mission parser loads.
    return materializeMissionTemplate(*bytes, nm.fills);
}

void CampaignRunner::recordOutcome(const std::string& missionId, bool success) {
    m_engine.recordOutcome(missionId, success);
}

std::string CampaignRunner::save() const {
    return m_engine.serialize();
}

bool CampaignRunner::restore(const std::string& blob) {
    return m_engine.deserialize(blob);
}

} // namespace fl
