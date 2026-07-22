// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/MatchController.h"

#include <algorithm>
#include <cmath>

namespace fl {

void MatchController::configure(const GameModeDef& mode, std::vector<TeamState> teams, double simDt) {
    m_mode = mode;
    m_simDt = simDt > 0.0 ? simDt : 1.0 / 60.0;
    m_scores.clear();
    for (const TeamState& t : teams)
        m_scores.push_back(TeamScore{t.factionIndex, 0});
    m_phase = MatchPhase::Idle;
    m_phaseEndTick = 0;
    m_activeStartTick = 0;
    m_winner.reset();
    m_participants.clear();
    m_humans = 0;
    m_rotateFired = false;
    ++m_stateVersion;
}

void MatchController::addScore(uint16_t faction, int points) {
    for (TeamScore& s : m_scores) {
        if (s.factionIndex == faction) {
            s.score += points;
            ++m_stateVersion;
            return;
        }
    }
}

void MatchController::transitionTo(MatchPhase to, uint64_t tick) {
    if (to == m_phase)
        return;
    const MatchPhase from = m_phase;
    m_phase = to;
    ++m_stateVersion;

    switch (to) {
    case MatchPhase::Warmup:
        // Zero scores at warmup entry; the countdown end tick is set once minPlayers is met (in step).
        for (TeamScore& s : m_scores)
            s.score = 0;
        m_phaseEndTick = 0;
        break;
    case MatchPhase::Active:
        m_activeStartTick = tick;
        m_phaseEndTick =
            m_mode.timeLimitS > 0.0 ? tick + static_cast<uint64_t>(std::llround(m_mode.timeLimitS / m_simDt)) : 0;
        break;
    case MatchPhase::Ending:
        m_phaseEndTick = tick + static_cast<uint64_t>(std::llround(m_endingSeconds / m_simDt));
        break;
    case MatchPhase::PostMatch:
        m_phaseEndTick = 0;
        break;
    case MatchPhase::Idle:
        m_phaseEndTick = 0;
        break;
    }

    if (m_onPhase)
        m_onPhase(from, to);
}

void MatchController::participantJoined(uint32_t participantId, uint16_t faction, bool bot) {
    for (Participant& p : m_participants) {
        if (p.id == participantId) {
            p.faction = faction; // update team on a re-key (team switch)
            return;
        }
    }
    m_participants.push_back(Participant{participantId, faction, bot});
    if (!bot)
        ++m_humans;
}

void MatchController::participantLeft(uint32_t participantId) {
    auto it = std::find_if(m_participants.begin(), m_participants.end(),
                           [participantId](const Participant& p) { return p.id == participantId; });
    if (it == m_participants.end())
        return;
    if (!it->bot)
        m_humans = std::max(0, m_humans - 1);
    m_participants.erase(it);
}

void MatchController::recordKill(uint32_t killer, uint32_t victim, bool sameTeam) {
    (void)victim;
    if (m_phase != MatchPhase::Active)
        return; // scoring frozen outside Active
    if (sameTeam || m_mode.pointsPerKill == 0)
        return; // a team kill (or a zero-point mode) awards nothing
    // Resolve the killer's team.
    for (const Participant& p : m_participants) {
        if (p.id == killer) {
            addScore(p.faction, m_mode.pointsPerKill);
            return;
        }
    }
}

void MatchController::recordObjective(uint16_t faction, int count) {
    if (m_phase != MatchPhase::Active)
        return; // scoring frozen outside Active
    if (count <= 0 || m_mode.pointsPerObjective == 0)
        return;
    addScore(faction, count * m_mode.pointsPerObjective);
}

void MatchController::forceEnd(std::optional<uint16_t> winner) {
    if (m_phase == MatchPhase::Ending || m_phase == MatchPhase::PostMatch)
        return;
    m_winner = winner;
    // The tick is unknown here; use 0 and let the next step() compute the ending end-tick. Simpler:
    // transition immediately using a synthetic "now" of the active start (Ending duration is relative).
    // step() is the authority on the tick, so route through it by marking a pending end.
    m_pendingForceEnd = true;
}

void MatchController::step(uint64_t tick) {
    // Idle -> Warmup on the first human participant.
    if (m_phase == MatchPhase::Idle) {
        if (m_humans > 0)
            transitionTo(MatchPhase::Warmup, tick);
        return;
    }

    if (m_pendingForceEnd && (m_phase == MatchPhase::Warmup || m_phase == MatchPhase::Active)) {
        m_pendingForceEnd = false;
        transitionTo(MatchPhase::Ending, tick);
        return;
    }

    if (m_phase == MatchPhase::Warmup) {
        if (m_mode.warmupS <= 0.0) {
            transitionTo(MatchPhase::Active, tick);
            return;
        }
        if (m_humans >= m_mode.minPlayers) {
            if (m_phaseEndTick == 0) {
                // Start (or restart) the warmup countdown now that we have enough players.
                m_phaseEndTick = tick + static_cast<uint64_t>(std::llround(m_mode.warmupS / m_simDt));
                ++m_stateVersion;
            } else if (tick >= m_phaseEndTick) {
                transitionTo(MatchPhase::Active, tick);
            }
        } else if (m_phaseEndTick != 0) {
            // Dropped below minPlayers — hold at warmup (clear the countdown).
            m_phaseEndTick = 0;
            ++m_stateVersion;
        }
        return;
    }

    if (m_phase == MatchPhase::Active) {
        // Score limit.
        if (m_mode.scoreLimit > 0) {
            for (const TeamScore& s : m_scores) {
                if (s.score >= m_mode.scoreLimit) {
                    m_winner = s.factionIndex;
                    transitionTo(MatchPhase::Ending, tick);
                    return;
                }
            }
        }
        // Time limit.
        if (m_phaseEndTick != 0 && tick >= m_phaseEndTick) {
            // Winner = leading team; a tie leaves m_winner unset (draw).
            m_winner.reset();
            int32_t best = INT32_MIN;
            bool tie = false;
            uint16_t bestFac = 0;
            for (const TeamScore& s : m_scores) {
                if (s.score > best) {
                    best = s.score;
                    bestFac = s.factionIndex;
                    tie = false;
                } else if (s.score == best) {
                    tie = true;
                }
            }
            if (!tie)
                m_winner = bestFac;
            transitionTo(MatchPhase::Ending, tick);
            return;
        }
        return;
    }

    if (m_phase == MatchPhase::Ending) {
        if (tick >= m_phaseEndTick) {
            transitionTo(MatchPhase::PostMatch, tick);
            if (!m_rotateFired) {
                m_rotateFired = true;
                if (m_onRotate)
                    m_onRotate();
            }
        }
        return;
    }
    // PostMatch: terminal, nothing more to do.
}

} // namespace fl
