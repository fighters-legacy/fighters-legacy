// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The campaign orchestration layer (#635/#584): ties the deterministic CampaignEngine to the mission
// content and persistence so a campaign actually RUNS — resolve the next sortie into a concrete mission
// YAML, fly it, record the outcome (which advances the frontline / injects the next story mission), and
// save/restore across sessions. The host (fl-server) drives it: it injects a content loader (path ->
// mission/template bytes) and a frontline loader (path -> decoded raster), and it owns where the save
// blob is stored. Keeping the loaders injected leaves engine-campaign free of any filesystem/image
// dependency and makes the whole loop unit-testable with synthetic content.

#include "campaign/CampaignEngine.h"
#include "campaign/CampaignParser.h"

#include <functional>
#include <optional>
#include <string>

namespace fl {

class CampaignRunner {
  public:
    // Resolve a pack-relative path (a story mission file, or a dynamic template file) to its bytes.
    // Returns nullopt when the content is missing.
    using ContentLoader = std::function<std::optional<std::string>(const std::string& path)>;

    CampaignRunner(CampaignDef def, uint64_t seed, ContentLoader content,
                   CampaignEngine::FrontlineLoader frontline = {}, double planetRadiusM = kEarthRadiusM);

    // The concrete mission YAML for the next sortie, and its campaign mission id (out param). A story
    // mission returns its file verbatim; a dynamic sortie loads its template and materializes the
    // ${...} fills into a plain mission file. Returns nullopt when the campaign has no next mission
    // (complete) or the content could not be loaded (the id is still set for logging).
    [[nodiscard]] std::optional<std::string> nextMissionYaml(std::string& outMissionId);

    // Record the outcome of the mission just flown, advancing the campaign (frontline replace, theater
    // unlock, story arming, attrition). Mirror of CampaignEngine::recordOutcome.
    void recordOutcome(const std::string& missionId, bool success);

    // Persistence: a compact blob of the mutable campaign state (delegates to the engine).
    [[nodiscard]] std::string save() const;
    [[nodiscard]] bool restore(const std::string& blob);

    [[nodiscard]] CampaignEngine& engine() noexcept {
        return m_engine;
    }
    [[nodiscard]] const CampaignEngine& engine() const noexcept {
        return m_engine;
    }
    [[nodiscard]] const std::string& name() const noexcept {
        return m_name;
    }

  private:
    std::string m_name;
    CampaignEngine m_engine;
    ContentLoader m_content;
};

} // namespace fl
