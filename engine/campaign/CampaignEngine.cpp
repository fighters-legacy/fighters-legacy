// SPDX-License-Identifier: GPL-3.0-or-later
#include "campaign/CampaignEngine.h"

#include "flight/Geodetic.h" // geodeticToWorld

#include <charconv>
#include <sstream>

namespace fl {

namespace {

// Split "after_sorties:3" -> {"after_sorties", "3"}; a bare "campaign_start" -> {that, ""}.
std::pair<std::string, std::string> splitTrigger(const std::string& t) {
    const auto colon = t.find(':');
    if (colon == std::string::npos)
        return {t, ""};
    return {t.substr(0, colon), t.substr(colon + 1)};
}

int toInt(const std::string& s, int fallback = 0) {
    int v = fallback;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

std::vector<std::string> splitList(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == sep) {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

} // namespace

CampaignEngine::CampaignEngine(CampaignDef def, uint64_t seed, FrontlineLoader loader, double planetRadiusM)
    : m_def(std::move(def)), m_rng(seed ? seed : 0x9E3779B97F4A7C15ull), m_loader(std::move(loader)),
      m_planetRadiusM(planetRadiusM) {
    // Theater states: a theater referenced by any story's on_complete.unlock starts LOCKED (that story
    // adds it to the rotation); every other theater is active from the start.
    for (const CampaignTheater& th : m_def.theaters) {
        TheaterState ts;
        ts.currentFrontlinePath = th.initialFrontline;
        ts.groundUnits = th.groundUnits;
        ts.unlocked = !theaterEverUnlockedByStory(th.id);
        ts.frontline = Frontline(th.frontlineCols, th.frontlineRows,
                                 GeoBounds{}); // bounds are theater-manifest data; the loader may refine
        if (m_loader && !th.initialFrontline.empty())
            m_loader(th.initialFrontline, ts.frontline);
        m_theaters.emplace(th.id, std::move(ts));
    }

    // Story states parallel to m_def.story; arm each story that carries a top-level trigger.
    m_storyStates.resize(m_def.story.size());
    for (std::size_t i = 0; i < m_def.story.size(); ++i) {
        if (!m_def.story[i].trigger.empty())
            armStory(m_def.story[i].id, m_def.story[i].trigger);
    }
}

uint32_t CampaignEngine::nextRand() {
    // splitmix64 — deterministic, well-distributed, no external state.
    uint64_t z = (m_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    return static_cast<uint32_t>(z >> 32);
}

const CampaignStoryMission* CampaignEngine::findStory(const std::string& id) const {
    for (const auto& s : m_def.story)
        if (s.id == id)
            return &s;
    return nullptr;
}

int CampaignEngine::storyIndex(const std::string& id) const {
    for (std::size_t i = 0; i < m_def.story.size(); ++i)
        if (m_def.story[i].id == id)
            return static_cast<int>(i);
    return -1;
}

bool CampaignEngine::theaterEverUnlockedByStory(const std::string& id) const {
    for (const auto& s : m_def.story)
        if (s.onComplete.unlock == id)
            return true;
    return false;
}

void CampaignEngine::armStory(const std::string& id, const std::string& trigger) {
    const int idx = storyIndex(id);
    if (idx < 0)
        return;
    StoryState& st = m_storyStates[static_cast<std::size_t>(idx)];
    if (st.completed)
        return; // already flown; do not re-arm
    st.armed = true;
    st.armBaseline = m_sortiesFlown;
    st.triggerOverride = trigger.empty() ? "campaign_start" : trigger;
}

bool CampaignEngine::storyTriggerSatisfied(std::size_t idx) const {
    const StoryState& st = m_storyStates[idx];
    if (!st.armed || st.completed)
        return false;
    const auto [kind, arg] = splitTrigger(st.triggerOverride);
    if (kind == "campaign_start" || kind.empty())
        return true;
    if (kind == "after_sorties")
        return (m_sortiesFlown - st.armBaseline) >= toInt(arg);
    if (kind == "frontline_reaches") {
        for (const std::string& t : m_reachedTags)
            if (t == arg)
                return true;
        return false;
    }
    return false;
}

bool CampaignEngine::dynamicLocked() const {
    for (std::size_t i = 0; i < m_def.story.size(); ++i)
        if (m_def.story[i].locksDynamic && storyTriggerSatisfied(i))
            return true;
    return false;
}

bool CampaignEngine::theaterUnlocked(const std::string& id) const {
    auto it = m_theaters.find(id);
    return it != m_theaters.end() && it->second.unlocked;
}

const Frontline* CampaignEngine::frontline(const std::string& theaterId) const {
    auto it = m_theaters.find(theaterId);
    return it == m_theaters.end() ? nullptr : &it->second.frontline;
}

float CampaignEngine::frontlineFraction(const std::string& theaterId, int side) const {
    const Frontline* f = frontline(theaterId);
    return f ? f->sideFraction(side) : 0.f;
}

void CampaignEngine::reachFrontlineTag(const std::string& tag) {
    for (const std::string& t : m_reachedTags)
        if (t == tag)
            return;
    m_reachedTags.push_back(tag);
}

void CampaignEngine::applySetFrontline(const std::string& theaterId, const std::string& path) {
    auto it = m_theaters.find(theaterId);
    if (it == m_theaters.end() || path.empty())
        return;
    it->second.currentFrontlinePath = path;
    if (m_loader)
        m_loader(path, it->second.frontline); // replaces wholesale; ground_units untouched (spec)
}

NextMission CampaignEngine::nextMission() {
    // 1) A pending story mission whose trigger is satisfied, in declaration order, wins.
    for (std::size_t i = 0; i < m_def.story.size(); ++i) {
        if (!storyTriggerSatisfied(i))
            continue;
        NextMission nm;
        nm.kind = NextMission::Kind::Story;
        nm.missionFile = m_def.story[i].file;
        nm.missionId = m_def.story[i].id;
        nm.theaterId = m_def.story[i].theaterId;
        return nm;
    }

    // 2) A generated dynamic sortie from an active, unlocked theater (dynamic must be enabled and not
    //    frozen by a pending locks_dynamic story). Theaters are tried in declaration order.
    if (m_def.dynamicEnabled && !dynamicLocked()) {
        for (const CampaignTheater& th : m_def.theaters) {
            auto it = m_theaters.find(th.id);
            if (it == m_theaters.end() || !it->second.unlocked)
                continue;
            NextMission nm;
            buildDynamicSortie(th, nm);
            if (nm.kind == NextMission::Kind::Dynamic)
                return nm;
        }
    }
    return {}; // Kind::None
}

void CampaignEngine::buildDynamicSortie(const CampaignTheater& th, NextMission& out) {
    // Enemy side is the one the player is not flying for; default to sides[1] as the enemy when the
    // pilot side is sides[0].
    const bool pilotIsA = m_def.pilotSide.empty() || m_def.pilotSide == m_def.sides[0];
    const std::string& enemySide = pilotIsA ? m_def.sides[1] : m_def.sides[0];
    const FrontlineControl enemyControl = pilotIsA ? FrontlineControl::SideB : FrontlineControl::SideA;
    const FrontlineControl friendlyControl = pilotIsA ? FrontlineControl::SideA : FrontlineControl::SideB;

    auto& ts = m_theaters.at(th.id);

    // requires-tag eligibility: empty = always; else the enemy order of battle must field the unit the
    // tag names (e.g. "enemy_sam" -> a "sam" unit with count > 0).
    auto requiresSatisfied = [&](const std::string& tag) -> bool {
        if (tag.empty())
            return true;
        std::string want = tag;
        if (want.rfind("enemy_", 0) == 0)
            want = want.substr(6);
        auto sideIt = ts.groundUnits.find(enemySide);
        if (sideIt == ts.groundUnits.end())
            return false;
        for (const auto& [unit, count] : sideIt->second)
            if (count > 0 && (unit == want || unit.find(want) != std::string::npos))
                return true;
        return false;
    };

    // Weighted draw among eligible templates.
    int totalWeight = 0;
    for (const CampaignTemplate& t : th.templates)
        if (requiresSatisfied(t.requiresTag))
            totalWeight += t.weight;
    if (totalWeight <= 0)
        return; // no eligible template -> no sortie this theater

    int roll = static_cast<int>(nextRand() % static_cast<uint32_t>(totalWeight));
    const CampaignTemplate* chosen = nullptr;
    for (const CampaignTemplate& t : th.templates) {
        if (!requiresSatisfied(t.requiresTag))
            continue;
        roll -= t.weight;
        if (roll < 0) {
            chosen = &t;
            break;
        }
    }
    if (!chosen)
        return;

    out.kind = NextMission::Kind::Dynamic;
    out.missionFile = chosen->file;
    out.theaterId = th.id;
    out.role = chosen->role;
    out.missionId = "dynamic:" + th.id + ":" + chosen->role + "#" + std::to_string(++m_dynamicCounter);

    // Fills from the live frontline: an enemy/contested cell for the objective, a friendly cell for
    // ingress. Skipped when the raster is not loaded (no loader / headless).
    if (ts.frontline.valid()) {
        auto cellWorld = [&](int col, int row, double out3[3]) {
            double lat = 0.0;
            double lon = 0.0;
            ts.frontline.cellCenterLatLon(col, row, lat, lon);
            double wx = 0.0;
            double wy = 0.0;
            double wz = 0.0;
            geodeticToWorld(LatLonAlt{lat, lon, 0.0}, wx, wy, wz, m_planetRadiusM);
            out3[0] = wx;
            out3[1] = wy;
            out3[2] = wz;
        };
        bool haveTarget = false;
        bool haveIngress = false;
        for (int row = 0; row < ts.frontline.rows() && !(haveTarget && haveIngress); ++row) {
            for (int col = 0; col < ts.frontline.cols() && !(haveTarget && haveIngress); ++col) {
                const FrontlineControl fc = ts.frontline.at(col, row).control;
                if (!haveTarget && (fc == enemyControl || fc == FrontlineControl::Contested)) {
                    cellWorld(col, row, out.targetWorld);
                    haveTarget = true;
                } else if (!haveIngress && fc == friendlyControl) {
                    cellWorld(col, row, out.ingressWorld);
                    haveIngress = true;
                }
            }
        }
        out.hasFill = haveTarget;
    }
    // Enemy force count scaled from the order of battle (the total enemy units in the theater).
    int opfor = 0;
    if (auto sideIt = ts.groundUnits.find(enemySide); sideIt != ts.groundUnits.end())
        for (const auto& [unit, count] : sideIt->second)
            opfor += (count > 0 ? count : 0);
    out.opforCount = opfor;
}

void CampaignEngine::recordOutcome(const std::string& missionId, bool success) {
    const int sidx = storyIndex(missionId);
    if (sidx >= 0) {
        const CampaignStoryMission& sm = m_def.story[static_cast<std::size_t>(sidx)];
        StoryState& st = m_storyStates[static_cast<std::size_t>(sidx)];
        if (success) {
            st.completed = true;
            st.armed = false;
            m_completedStory.push_back(sm.id);
            if (!sm.onComplete.setFrontline.empty())
                applySetFrontline(sm.theaterId, sm.onComplete.setFrontline);
            if (!sm.onComplete.unlock.empty())
                if (auto it = m_theaters.find(sm.onComplete.unlock); it != m_theaters.end())
                    it->second.unlocked = true;
            if (!sm.onComplete.nextId.empty())
                armStory(sm.onComplete.nextId, sm.onComplete.nextTrigger);
        } else {
            // Failure policy. Default (retry) leaves the story armed/pending and the lock on.
            if (!sm.onFail.setFrontline.empty()) {
                applySetFrontline(sm.theaterId, sm.onFail.setFrontline); // a setback; resolved, not retried
                st.armed = false;
                st.completed = true; // resolved (failed), not re-flown
            } else if (!sm.onFail.nextId.empty()) {
                st.armed = false;
                st.completed = true;
                armStory(sm.onFail.nextId, ""); // branch to the failure mission
            } else if (sm.onFail.unlockDynamic) {
                st.armed = false;
                st.completed = true; // resolved: lift the lock and resume the dynamic war
            }
            // else: retry — stays armed/pending, re-flown next time.
        }
        return;
    }

    // A dynamic sortie: advance the sortie counter and, on success, apply attrition to the enemy order
    // of battle in its theater (decrement one enemy unit — the generator "decrements as missions
    // resolve"). Frozen theaters never produce a sortie, so this only fires for live ones.
    ++m_sortiesFlown;
    if (success && missionId.rfind("dynamic:", 0) == 0) {
        const std::string rest = missionId.substr(8);
        const std::string theaterId = rest.substr(0, rest.find(':'));
        auto it = m_theaters.find(theaterId);
        if (it != m_theaters.end()) {
            const bool pilotIsA = m_def.pilotSide.empty() || m_def.pilotSide == m_def.sides[0];
            const std::string& enemySide = pilotIsA ? m_def.sides[1] : m_def.sides[0];
            auto sideIt = it->second.groundUnits.find(enemySide);
            if (sideIt != it->second.groundUnits.end())
                for (auto& [unit, count] : sideIt->second)
                    if (count > 0) {
                        --count;
                        break;
                    }
        }
    }
}

std::string CampaignEngine::serialize() const {
    std::ostringstream os;
    os << "sorties=" << m_sortiesFlown << "\n";
    os << "completed=";
    for (std::size_t i = 0; i < m_completedStory.size(); ++i)
        os << (i ? "," : "") << m_completedStory[i];
    os << "\n";
    os << "reached=";
    for (std::size_t i = 0; i < m_reachedTags.size(); ++i)
        os << (i ? "," : "") << m_reachedTags[i];
    os << "\n";
    for (std::size_t i = 0; i < m_def.story.size(); ++i) {
        const StoryState& st = m_storyStates[i];
        os << "story=" << m_def.story[i].id << ";" << (st.armed ? 1 : 0) << ";" << (st.completed ? 1 : 0) << ";"
           << st.armBaseline << ";" << st.triggerOverride << "\n";
    }
    for (const auto& [id, ts] : m_theaters) {
        os << "theater=" << id << ";" << ts.currentFrontlinePath << ";" << (ts.unlocked ? 1 : 0) << ";";
        bool first = true;
        for (const auto& [side, units] : ts.groundUnits)
            for (const auto& [unit, count] : units) {
                os << (first ? "" : ",") << side << "/" << unit << "=" << count;
                first = false;
            }
        os << "\n";
    }
    return os.str();
}

bool CampaignEngine::deserialize(const std::string& blob) {
    std::istringstream is(blob);
    std::string line;
    while (std::getline(is, line)) {
        if (line.empty())
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            return false;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        if (key == "sorties") {
            m_sortiesFlown = toInt(val);
        } else if (key == "completed") {
            m_completedStory = splitList(val, ',');
        } else if (key == "reached") {
            m_reachedTags = splitList(val, ',');
        } else if (key == "story") {
            const std::vector<std::string> f = splitList(val, ';');
            if (f.empty())
                return false;
            const int idx = storyIndex(f[0]);
            if (idx < 0)
                continue; // a story id not in this def — ignore (forward/backward compat)
            StoryState& st = m_storyStates[static_cast<std::size_t>(idx)];
            st.armed = f.size() > 1 && f[1] == "1";
            st.completed = f.size() > 2 && f[2] == "1";
            st.armBaseline = f.size() > 3 ? toInt(f[3]) : 0;
            st.triggerOverride = f.size() > 4 ? f[4] : "";
        } else if (key == "theater") {
            const auto semi = val.find(';');
            if (semi == std::string::npos)
                return false;
            const std::string id = val.substr(0, semi);
            auto it = m_theaters.find(id);
            if (it == m_theaters.end())
                continue;
            std::string rest = val.substr(semi + 1);
            const std::vector<std::string> parts = splitList(rest, ';');
            // parts: [frontlinePath, unlocked, groundUnitsCsv]
            if (!parts.empty())
                it->second.currentFrontlinePath = parts[0];
            if (parts.size() > 1)
                it->second.unlocked = (parts[1] == "1");
            if (parts.size() > 2) {
                it->second.groundUnits.clear();
                for (const std::string& u : splitList(parts[2], ',')) {
                    const auto slash = u.find('/');
                    const auto eq2 = u.find('=');
                    if (slash == std::string::npos || eq2 == std::string::npos || eq2 < slash)
                        continue;
                    const std::string side = u.substr(0, slash);
                    const std::string unit = u.substr(slash + 1, eq2 - slash - 1);
                    it->second.groundUnits[side][unit] = toInt(u.substr(eq2 + 1));
                }
            }
            // Reload the current frontline raster from its path so control queries are restored.
            if (m_loader && !it->second.currentFrontlinePath.empty())
                m_loader(it->second.currentFrontlinePath, it->second.frontline);
        }
    }
    return true;
}

} // namespace fl
