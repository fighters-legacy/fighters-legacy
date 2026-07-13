// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/FormationRegistry.h"

#include <algorithm>
#include <utility>

namespace fl {

uint8_t FormationRegistry::depthOf(FormationId id) const noexcept {
    uint8_t depth = 0;
    FormationId cur = id;
    // Bounded by kMaxFormationDepth so a corrupted parent cycle cannot hang the sim thread.
    while (cur != kNoFormation && depth < kMaxFormationDepth) {
        auto it = m_formations.find(cur);
        if (it == m_formations.end()) {
            break;
        }
        cur = it->second.parent;
        ++depth;
    }
    return depth;
}

FormationId FormationRegistry::create(std::string callsign, EntityId anchor, uint32_t commanderPeerId,
                                      FormationId parent) {
    if (parent != kNoFormation) {
        auto pit = m_formations.find(parent);
        if (pit == m_formations.end()) {
            return kNoFormation; // unknown parent
        }
        if (depthOf(parent) >= kMaxFormationDepth) {
            return kNoFormation; // tree too deep
        }
    }

    // Skip ids already in use (only reachable after ~65k creations wrap the counter).
    while (m_nextId == kNoFormation || m_formations.contains(m_nextId)) {
        ++m_nextId;
    }
    const FormationId id = m_nextId++;

    Formation f{};
    f.id = id;
    f.parent = parent;
    f.callsign = std::move(callsign);
    f.anchor = anchor;
    f.commanderPeerId = commanderPeerId;
    m_formations.emplace(id, std::move(f));

    if (parent != kNoFormation) {
        m_formations[parent].children.push_back(id);
    }
    return id;
}

bool FormationRegistry::destroy(FormationId id) {
    auto it = m_formations.find(id);
    if (it == m_formations.end()) {
        return false;
    }
    const FormationId parent = it->second.parent;

    // Re-parent children rather than destroying them: disbanding a package must not delete the
    // flights inside it.
    for (const FormationId child : it->second.children) {
        auto cit = m_formations.find(child);
        if (cit != m_formations.end()) {
            cit->second.parent = parent;
            if (parent != kNoFormation) {
                m_formations[parent].children.push_back(child);
            }
        }
    }

    for (const FormationMember& m : it->second.members) {
        m_memberIndex.erase(m.id.index);
    }

    if (parent != kNoFormation) {
        auto pit = m_formations.find(parent);
        if (pit != m_formations.end()) {
            auto& kids = pit->second.children;
            kids.erase(std::remove(kids.begin(), kids.end(), id), kids.end());
        }
    }

    m_formations.erase(it);
    return true;
}

bool FormationRegistry::addMember(FormationId id, FormationMember member) {
    if (!member.id.valid()) {
        return false;
    }
    auto it = m_formations.find(id);
    if (it == m_formations.end()) {
        return false;
    }
    // An aircraft belongs to exactly one formation: adding it to a second MOVES it, rather than
    // leaving it taking orders from two chains of command.
    removeEntity(member.id);

    m_memberIndex[member.id.index] = id;
    it->second.members.push_back(member);
    return true;
}

FormationId FormationRegistry::removeEntity(EntityId entity) {
    auto idx = m_memberIndex.find(entity.index);
    if (idx == m_memberIndex.end()) {
        return kNoFormation;
    }
    const FormationId fid = idx->second;
    m_memberIndex.erase(idx);

    auto it = m_formations.find(fid);
    if (it != m_formations.end()) {
        auto& ms = it->second.members;
        ms.erase(std::remove_if(ms.begin(), ms.end(), [&](const FormationMember& m) { return m.id == entity; }),
                 ms.end());
    }
    return fid;
}

const Formation* FormationRegistry::get(FormationId id) const noexcept {
    auto it = m_formations.find(id);
    return it == m_formations.end() ? nullptr : &it->second;
}

Formation* FormationRegistry::get(FormationId id) noexcept {
    auto it = m_formations.find(id);
    return it == m_formations.end() ? nullptr : &it->second;
}

FormationId FormationRegistry::formationOfEntity(EntityId entity) const noexcept {
    auto it = m_memberIndex.find(entity.index);
    if (it == m_memberIndex.end()) {
        return kNoFormation;
    }
    // Guard a recycled pool slot: the index may now name a different entity entirely.
    const Formation* f = get(it->second);
    if (!f) {
        return kNoFormation;
    }
    const bool stillMember =
        std::any_of(f->members.begin(), f->members.end(), [&](const FormationMember& m) { return m.id == entity; });
    return stillMember ? it->second : kNoFormation;
}

FormationId FormationRegistry::formationAnchoredOn(EntityId entity) const noexcept {
    for (const auto& [id, f] : m_formations) {
        if (f.anchor == entity) {
            return id;
        }
    }
    return kNoFormation;
}

bool FormationRegistry::commands(uint32_t peerId, FormationId id) const noexcept {
    if (peerId == kNoPeer || id == kNoFormation) {
        return false; // the game master goes through the admin console, which bypasses this check
    }
    FormationId cur = id;
    for (uint8_t hops = 0; cur != kNoFormation && hops < kMaxFormationDepth; ++hops) {
        const Formation* f = get(cur);
        if (!f) {
            return false;
        }
        if (f->commanderPeerId == peerId) {
            return true; // commands it directly, or commands an ancestor -> command cascades down
        }
        cur = f->parent;
    }
    return false;
}

std::vector<FormationId> FormationRegistry::subtree(FormationId id) const {
    std::vector<FormationId> out;
    if (!get(id)) {
        return out;
    }
    out.push_back(id);
    // Breadth-first: parents before children, so an order applies top-down.
    for (std::size_t i = 0; i < out.size(); ++i) {
        const Formation* f = get(out[i]);
        if (!f) {
            continue;
        }
        for (const FormationId child : f->children) {
            // Defensive: a malformed tree must not make this unbounded.
            if (std::find(out.begin(), out.end(), child) == out.end()) {
                out.push_back(child);
            }
        }
    }
    return out;
}

std::vector<FormationId> FormationRegistry::commandedBy(uint32_t peerId) const {
    std::vector<FormationId> out;
    if (peerId == kNoPeer) {
        return out;
    }
    for (const auto& [id, f] : m_formations) {
        if (f.commanderPeerId == peerId) {
            out.push_back(id);
        }
    }
    std::sort(out.begin(), out.end()); // deterministic order for tests and admin output
    return out;
}

void FormationRegistry::releasePeer(uint32_t peerId) {
    if (peerId == kNoPeer) {
        return;
    }
    for (auto& [id, f] : m_formations) {
        if (f.commanderPeerId == peerId) {
            // The formation outlives its commander: an AI flight whose AWACS logged off is still
            // flying, and the game master can still order it. It is simply unclaimed.
            f.commanderPeerId = kNoPeer;
        }
        auto& ms = f.members;
        for (const FormationMember& m : ms) {
            if (m.peerId == peerId) {
                m_memberIndex.erase(m.id.index);
            }
        }
        ms.erase(std::remove_if(ms.begin(), ms.end(), [&](const FormationMember& m) { return m.peerId == peerId; }),
                 ms.end());
    }
}

void FormationRegistry::forEach(const std::function<void(const Formation&)>& fn) const {
    for (const auto& [id, f] : m_formations) {
        fn(f);
    }
}

void FormationRegistry::clear() noexcept {
    m_formations.clear();
    m_memberIndex.clear();
    m_nextId = 1;
}

} // namespace fl
