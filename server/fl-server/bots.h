// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "match/BotFillPolicy.h"
#include "match/TeamBalancer.h"
#include "net/GameProtocol.h" // kBotParticipantBase
#include "net/WorldBroadcaster.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace fl {

// Fills a match with AI bots to a target participant count (#87). Bots are server-side AI entities, not
// network peers — the transport never sees them. This owns the bot lifecycle (spawn / retire / respawn)
// and registers each as a scoreboard participant; fl-server supplies the spawn/kill/liveness seams
// (they need engine-ai + engine-script, which engine-net does not link).
class BotRoster {
  public:
    struct Config {
        int fill{0};     // desired total participants (humans + bots); 0 = bots disabled
        int maxBots{16}; // cap on live bots
        bool balanceTeams{true};
    };

    // spawn(faction) -> spawns a bot aircraft on `faction` (entity + AI controller + faction stamp) and
    // returns its EntityId (invalid on failure). kill(eid) tears the entity down. alive(eid) is true
    // while the entity is live.
    using SpawnFn = std::function<EntityId(uint16_t faction)>;
    using KillFn = std::function<void(EntityId)>;
    using AliveFn = std::function<bool(EntityId)>;

    BotRoster(WorldBroadcaster& b, Config cfg, std::vector<uint16_t> teams, SpawnFn spawn, KillFn kill, AliveFn alive)
        : m_b(b), m_cfg(cfg), m_teams(std::move(teams)), m_spawn(std::move(spawn)), m_kill(std::move(kill)),
          m_alive(std::move(alive)) {}

    // Called ~1 Hz. Reaps dead bots, then spawns/retires to reach the fill target. `humans` = current
    // human participant count. Gentle churn: at most one add or remove per call.
    void step(int humans) {
        // 1. Reap dead bots (killed in combat).
        for (auto it = m_bots.begin(); it != m_bots.end();) {
            if (m_alive && !m_alive(it->eid)) {
                m_b.removeBotParticipant(it->pid);
                it = m_bots.erase(it);
            } else {
                ++it;
            }
        }

        const int want = desiredBots(humans, m_cfg.fill, m_cfg.maxBots);
        const int live = static_cast<int>(m_bots.size());
        if (live < want && !m_teams.empty() && m_spawn) {
            spawnOne(humans);
        } else if (live > want && !m_bots.empty()) {
            retireOne();
        }
    }

    // Retire every bot (a match rotation / shutdown).
    void clear() {
        for (const Bot& bot : m_bots) {
            if (m_kill)
                m_kill(bot.eid);
            m_b.removeBotParticipant(bot.pid);
        }
        m_bots.clear();
    }

    [[nodiscard]] std::size_t liveCount() const noexcept {
        return m_bots.size();
    }

  private:
    struct Bot {
        uint32_t pid{0};
        EntityId eid{};
        uint16_t faction{0};
    };

    // Pick the team with the fewest total (human + bot) participants, so bots even out the sides.
    uint16_t pickBotTeam() {
        std::vector<TeamState> counts;
        counts.reserve(m_teams.size());
        for (uint16_t f : m_teams)
            counts.push_back(TeamState{f, 0, 0});
        // Human counts.
        m_b.forEachPeer([&](const PeerInfo& pi) {
            const uint16_t f = m_b.factionForPeer(pi.peerId);
            for (auto& c : counts)
                if (c.factionIndex == f)
                    ++c.count;
        });
        // Bot counts.
        for (const Bot& bot : m_bots)
            for (auto& c : counts)
                if (c.factionIndex == bot.faction)
                    ++c.count;
        if (auto t = pickTeam(counts))
            return *t;
        return m_teams.front();
    }

    void spawnOne(int /*humans*/) {
        const uint16_t faction = m_cfg.balanceTeams ? pickBotTeam() : m_teams.front();
        const EntityId eid = m_spawn(faction);
        if (!eid.valid())
            return;
        const uint32_t pid = kBotParticipantBase + m_nextN++;
        m_b.registerBotParticipant(pid, eid, generateCallsign(), faction);
        m_bots.push_back(Bot{pid, eid, faction});
    }

    void retireOne() {
        const Bot bot = m_bots.back();
        m_bots.pop_back();
        if (m_kill)
            m_kill(bot.eid);
        m_b.removeBotParticipant(bot.pid);
    }

    std::string generateCallsign() {
        static const char* kNames[] = {"Viper", "Cobra", "Falcon", "Raptor", "Ghost",
                                       "Hawk",  "Wolf",  "Eagle",  "Fury",   "Talon"};
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%s-%u", kNames[m_nextN % 10], m_nextN + 1);
        return buf;
    }

    WorldBroadcaster& m_b;
    Config m_cfg;
    std::vector<uint16_t> m_teams;
    SpawnFn m_spawn;
    KillFn m_kill;
    AliveFn m_alive;
    std::vector<Bot> m_bots;
    uint32_t m_nextN{0};
};

} // namespace fl
