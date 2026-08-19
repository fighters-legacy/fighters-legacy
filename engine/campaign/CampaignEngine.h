// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The deterministic dynamic-campaign runtime (#635): the theater-graph + frontline state machine that
// selects the player's next mission, advances the frontline as outcomes resolve, injects story missions
// at their triggers, and round-trips its state for save/restore. ZERO AI/LLM involvement — the campaign
// director epic (#590) later drives this SAME machinery through these interfaces, so this must stand
// alone first. Determinism: mission selection is a seeded weighted draw and all state transitions are
// pure functions of (outcome, current state), so a replay is byte-identical.
//
// Frontline rasters are held per theater as decoded pixels (Frontline). PNG decoding is the host's
// concern: the engine calls an injected FrontlineLoader to fetch a raster by path (initial_frontline /
// set_frontline), keeping engine-campaign free of any image library and trivially unit-testable.

#include "campaign/CampaignDef.h"
#include "campaign/Frontline.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fl {

// The next mission the player should fly, as chosen by the engine.
struct NextMission {
    enum class Kind : uint8_t { None, Story, Dynamic };
    Kind kind{Kind::None};
    std::string missionFile; // story.file, or a template.file for a generated sortie
    std::string missionId;   // the story id, or "dynamic:<theater>:<role>#<n>" for a dynamic sortie
    std::string theaterId;   // theater the mission belongs to
    std::string role;        // dynamic sortie role tag (empty for a story)
    // Resolved dynamic fills (valid only for Kind::Dynamic): the world positions the generator picked
    // from the live frontline, and the enemy force count scaled from ground_units. A materializer
    // substitutes these into the template's ${...} holes; a story mission carries none.
    bool hasFill{false};
    double targetWorld[3]{};   // an enemy/contested cell centre (the sortie's objective area)
    double ingressWorld[3]{};  // a friendly cell centre (the ingress point)
    double targetLatDeg{0.0};  // the same two cells in geodetic degrees — what a template needs to
    double targetLonDeg{0.0};  //   place an AIRBORNE object (object-level lat:/lon: + alt: MSL);
    double ingressLatDeg{0.0}; //  the world triples are sea-level points and raw Y is not altitude
    double ingressLonDeg{0.0}; //  away from the origin
    int opforCount{0};         // scaled enemy force count
    // Structured fills for template materialization (materializeMissionTemplate): fill name -> field ->
    // value string. Populated alongside the scalar fields above (target_area/ingress carry x/y/z + pos,
    // opfor carries count, player_flight carries size). Empty for a story mission.
    std::map<std::string, std::map<std::string, std::string>> fills;
};

class CampaignEngine {
  public:
    // `seed` seeds the deterministic weighted mission draw. Load a raster by path (initial_frontline /
    // set_frontline) via `loader`; without one the frontline stays empty (selection still works, fills
    // are skipped). planetRadiusM maps frontline cells to world positions for the fills.
    using FrontlineLoader = std::function<bool(const std::string& path, Frontline& out)>;
    CampaignEngine(CampaignDef def, uint64_t seed, FrontlineLoader loader = {}, double planetRadiusM = kEarthRadiusM);

    // The mission the player should fly next: a pending story mission whose trigger is satisfied (in
    // declaration order), else a generated dynamic sortie from an active, unlocked theater, else None.
    [[nodiscard]] NextMission nextMission();

    // Record the outcome of the mission just flown. A story applies on_complete / on_fail (frontline
    // replace, theater unlock, next-mission arming, retry/branch); a dynamic sortie advances the sortie
    // counter and, on success, applies attrition to the enemy order of battle. No-op for an unknown id.
    void recordOutcome(const std::string& missionId, bool success);

    // Mark a frontline milestone tag as reached, arming any story whose trigger is
    // `frontline_reaches:<tag>`. The host calls this from a mission/campaign trigger; the engine does
    // not infer tags from raster contents (the spec leaves tag emission to content).
    void reachFrontlineTag(const std::string& tag);

    // Queries (telemetry / UI).
    [[nodiscard]] int sortiesFlown() const noexcept {
        return m_sortiesFlown;
    }
    [[nodiscard]] const std::vector<std::string>& completedStory() const noexcept {
        return m_completedStory;
    }
    [[nodiscard]] bool theaterUnlocked(const std::string& id) const;
    [[nodiscard]] bool dynamicLocked() const; // true while a locks_dynamic story is pending
    [[nodiscard]] const Frontline* frontline(const std::string& theaterId) const;
    [[nodiscard]] float frontlineFraction(const std::string& theaterId, int side) const;

    // Save / restore (#635 part 3). serialize() emits a compact, human-readable snapshot of the
    // mutable runtime state (sorties, completed/pending story, unlocked theaters, per-theater frontline
    // path + ground_units). deserialize() restores it onto a CampaignEngine built from the same def,
    // reloading each theater's current frontline via the loader. Returns false on a malformed blob.
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] bool deserialize(const std::string& blob);

  private:
    struct TheaterState {
        std::string currentFrontlinePath; // initial_frontline, or the last set_frontline applied
        std::map<std::string, std::map<std::string, int>> groundUnits; // mutable order of battle
        bool unlocked{true};
        Frontline frontline;
    };
    struct StoryState {
        bool armed{false};           // its trigger is being watched
        bool completed{false};       // flown successfully
        int64_t armBaseline{0};      // sortiesFlown when armed (for after_sorties)
        std::string triggerOverride; // set when armed via a prior mission's next: (else the def trigger)
    };

    const CampaignStoryMission* findStory(const std::string& id) const;
    int storyIndex(const std::string& id) const;
    void armStory(const std::string& id, const std::string& trigger);
    [[nodiscard]] bool storyTriggerSatisfied(std::size_t idx) const;
    void applySetFrontline(const std::string& theaterId, const std::string& path);
    [[nodiscard]] bool theaterEverUnlockedByStory(const std::string& id) const;
    void buildDynamicSortie(const CampaignTheater& th, NextMission& out);
    uint32_t nextRand();

    CampaignDef m_def;
    uint64_t m_rng;
    FrontlineLoader m_loader;
    double m_planetRadiusM;

    std::map<std::string, TheaterState> m_theaters;
    std::vector<StoryState> m_storyStates; // parallel to m_def.story
    std::vector<std::string> m_completedStory;
    std::vector<std::string> m_reachedTags;
    int m_sortiesFlown{0};
    uint64_t m_dynamicCounter{0}; // for unique dynamic sortie ids
};

} // namespace fl
