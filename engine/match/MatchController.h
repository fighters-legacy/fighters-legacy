// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/GameModeDef.h"
#include "match/TeamBalancer.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace fl {

// The lifecycle phase of a match (#523). Deterministic off tick counts (the MissionRuntime pattern) so
// the whole machine is unit-testable and harness-reproducible without a clock.
enum class MatchPhase : uint8_t {
    Idle = 0,      // no match running (no human has joined yet)
    Warmup = 1,    // players may spawn and fly; scoring is frozen; countdown runs while >= minPlayers
    Active = 2,    // scoring live; ends on score/time limit or a mission-driven forceEnd
    Ending = 3,    // combat frozen, scoreboard shown; a short window before rotation
    PostMatch = 4, // terminal for this match; the rotate hook has fired
};

inline bool isMatchPhaseOrdinal(uint8_t v) noexcept {
    return v <= static_cast<uint8_t>(MatchPhase::PostMatch);
}

// A team's running score during the match.
struct TeamScore {
    uint16_t factionIndex{0};
    int32_t score{0};
};

// The match-lifecycle state machine (#523). Pure logic: it holds counts and scores and emits phase
// transitions, but touches no world/network state — fl-server owns it and translates its outputs
// (phase, team scores, phase-end tick) into MsgMatchState, and drives scoring from the combat path.
//
// step(tick) is called every sim tick with the current tick index; all timing is derived from tick
// deltas and the configured sim dt, so behavior is identical on any wall clock.
class MatchController {
  public:
    using PhaseHook = std::function<void(MatchPhase from, MatchPhase to)>;
    using RotateHook = std::function<void()>;

    // Configure for a mode + its teams. Resets the machine to Idle. simDt is seconds per tick (1/60).
    void configure(const GameModeDef& mode, std::vector<TeamState> teams, double simDt = 1.0 / 60.0);
    void setEndingSeconds(double s) noexcept {
        m_endingSeconds = s;
    }
    void setOnPhase(PhaseHook fn) {
        m_onPhase = std::move(fn);
    }
    void setOnRotate(RotateHook fn) {
        m_onRotate = std::move(fn);
    }

    // Per-tick advance. Drives the countdowns and the score/time-limit checks.
    void step(uint64_t tick);

    // Participant bookkeeping (humans + bots). Warmup starts on the first human; the warmup countdown
    // only runs while human count >= minPlayers.
    void participantJoined(uint32_t participantId, uint16_t faction, bool bot);
    void participantLeft(uint32_t participantId);

    // Credit a kill. Ignored outside the Active phase (warmup/ending are score-frozen). `sameTeam`
    // suppresses the point award (a team kill scores nothing) but the event still happened.
    void recordKill(uint32_t killer, uint32_t victim, bool sameTeam);

    // Force the match to end now (a mission-scripted victory, #584). Optional winner for the record.
    void forceEnd(std::optional<uint16_t> winner);

    // Outputs.
    [[nodiscard]] MatchPhase phase() const noexcept {
        return m_phase;
    }
    [[nodiscard]] uint64_t phaseEndTick() const noexcept {
        return m_phaseEndTick; // 0 = untimed (the client renders remaining = end - tickIndex)
    }
    [[nodiscard]] const std::vector<TeamScore>& teamScores() const noexcept {
        return m_scores;
    }
    [[nodiscard]] std::optional<uint16_t> winner() const noexcept {
        return m_winner;
    }
    [[nodiscard]] const std::string& modeId() const noexcept {
        return m_mode.id;
    }
    [[nodiscard]] const std::string& modeName() const noexcept {
        return m_mode.name.empty() ? m_mode.id : m_mode.name;
    }
    [[nodiscard]] int scoreLimit() const noexcept {
        return m_mode.scoreLimit;
    }
    // A monotone version bumped on any change worth broadcasting (phase or a team score). fl-server
    // compares it against the last-broadcast value to coalesce MsgMatchState sends.
    [[nodiscard]] uint64_t stateVersion() const noexcept {
        return m_stateVersion;
    }
    // True while combat should be frozen (Ending / PostMatch): the server gates the fire path on this.
    [[nodiscard]] bool combatFrozen() const noexcept {
        return m_phase == MatchPhase::Ending || m_phase == MatchPhase::PostMatch;
    }

  private:
    void transitionTo(MatchPhase to, uint64_t tick);
    void addScore(uint16_t faction, int points);
    [[nodiscard]] int humanCount() const noexcept {
        return m_humans;
    }

    GameModeDef m_mode;
    std::vector<TeamScore> m_scores; // one per configured team
    double m_simDt{1.0 / 60.0};
    double m_endingSeconds{10.0};

    MatchPhase m_phase{MatchPhase::Idle};
    uint64_t m_phaseEndTick{0};    // tick at which the current timed phase ends (0 = untimed)
    uint64_t m_activeStartTick{0}; // tick Active began (for the time limit)
    std::optional<uint16_t> m_winner;
    uint64_t m_stateVersion{0};

    int m_humans{0};
    // Per-participant record: id, team, and bot flag — so a leaver decrements the human count only for
    // humans, and recordKill can resolve the killer's team.
    struct Participant {
        uint32_t id{0};
        uint16_t faction{0};
        bool bot{false};
    };
    std::vector<Participant> m_participants;

    PhaseHook m_onPhase;
    RotateHook m_onRotate;
    bool m_rotateFired{false};
    bool m_pendingForceEnd{false}; // forceEnd sets this; step() applies it on the next tick with a real tick
};

} // namespace fl
