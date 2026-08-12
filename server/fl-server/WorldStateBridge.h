// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "loop/ISimUpdate.h"
#include "match/MatchController.h"
#include "mission/MissionRuntime.h"
#include "net/GameProtocol.h" // MissionResultCode — the outcome vocabulary the wire already uses
#include "net/WorldBroadcaster.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace fl {

// WorldStateBridge (#1078) — pushes mission and match state INTO the broadcaster.
//
// This is the one part of fl-server's old setMissionTickHook composite that is not a system stepping
// itself: it reads two systems and writes a third. It exists because `engine-net` does not link
// `engine-mission` or `engine-match`, so the broadcaster cannot pull either — the direction of the
// dependency decides the direction of the data, and this is the product library's job to bridge.
//
// A registered ISimUpdate rather than a lambda, so its position in the tick is data like every other
// system's: it must run AFTER the mission runtime and the match controller have stepped, or it publishes
// last tick's outcome. Its two cadences are its own concern (that is the point of #1078): the mission
// block refreshes every 30 ticks, just under the ~1 Hz world-state rebuild so the snapshot never carries
// a stale outcome, and the match block fires on the controller's state version changing rather than on a
// clock — a phase transition or a score is worth a MsgMatchState the moment it happens, and nothing
// otherwise.
class WorldStateBridge final : public ISimUpdate {
  public:
    // `missionRuntime` is nullable: a server with no mission loaded has no mission block to publish.
    WorldStateBridge(WorldBroadcaster& broadcaster, MatchController& matchController, MissionRuntime* missionRuntime,
                     std::string missionName)
        : m_broadcaster(broadcaster), m_match(matchController), m_mission(missionRuntime),
          m_missionName(std::move(missionName)) {}

    void onTick(double /*simDt*/, uint64_t tick) override {
        publishMission(tick);
        publishMatchState();
    }

  private:
    void publishMission(uint64_t tick) {
        if (!m_mission || tick % kMissionPushTicks != 0)
            return;
        const MissionOutcome& outcome = m_mission->outcome();
        WorldStateMission wm;
        wm.active = true;
        wm.name = m_missionName;
        // MissionState {Active, Complete, Failed} maps onto the MissionResultCode the wire and the
        // debrief already use, so an agent reading the snapshot and a client reading MsgMissionOutcome
        // see the same vocabulary rather than two spellings of one fact.
        wm.outcome = static_cast<uint8_t>(outcome.state == MissionState::Complete ? MissionResultCode::Success
                                          : outcome.state == MissionState::Failed ? MissionResultCode::Failure
                                                                                  : MissionResultCode::Incomplete);
        wm.triggersFired = outcome.triggersFired;
        wm.elapsedSeconds = outcome.elapsedSeconds;
        m_broadcaster.setWorldStateMission(std::move(wm));
    }

    void publishMatchState() {
        const uint64_t version = m_match.stateVersion();
        if (version == m_lastMatchVersion)
            return;
        m_lastMatchVersion = version;

        WorldBroadcaster::MatchStatePod pod;
        pod.phase = static_cast<uint8_t>(m_match.phase());
        pod.scoreLimit = static_cast<uint16_t>(std::clamp(m_match.scoreLimit(), 0, 65535));
        pod.phaseEndTick = m_match.phaseEndTick();
        pod.modeId = m_match.modeId();
        pod.modeName = m_match.modeName();
        // Reading the mode fields from the controller rather than from config keeps this correct across
        // a rotation.
        for (const TeamScore& ts : m_match.teamScores())
            pod.teamScores.emplace_back(ts.factionIndex, ts.score);
        m_broadcaster.setMatchState(pod);
    }

    static constexpr uint64_t kMissionPushTicks = 30;

    WorldBroadcaster& m_broadcaster;
    MatchController& m_match;
    MissionRuntime* m_mission;
    std::string m_missionName;
    uint64_t m_lastMatchVersion{0};
};

} // namespace fl
