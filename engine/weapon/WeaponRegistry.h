// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "weapon/WeaponDef.h"

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

// Owns all registered weapon definitions, keyed by WeaponDef::id. Content packs register their
// weapons here before the first sim tick; the registry is read-only during simulation.
//
// KEYED BY ID, NOT BY FILENAME, and that is the whole point (#812). A hardpoint names the stores it
// accepts by ID ("fl-base:aim9p"); resolving a loadout therefore never touches the filesystem. The
// id -> asset-name mapping happens exactly once, at bootstrap, in ContentIndex (#810) -- which is
// why that had to land first.
//
// Threading: main-thread-only (populate before GameLoop::start()), mirroring EntityTypeRegistry.
class WeaponRegistry {
  public:
    // Registers a weapon and returns its assigned index.
    // Returns std::numeric_limits<uint32_t>::max() if the id is already registered.
    uint32_t registerWeapon(WeaponDef def);

    // Returns nullptr if id is not registered.
    [[nodiscard]] const WeaponDef* findById(const char* id) const noexcept;
    [[nodiscard]] const WeaponDef* findById(const std::string& id) const noexcept {
        return findById(id.c_str());
    }

    // Returns std::numeric_limits<uint32_t>::max() if id is not registered.
    [[nodiscard]] uint32_t indexById(const char* id) const noexcept;

    // Returns nullptr if index is out of range.
    [[nodiscard]] const WeaponDef* byIndex(uint32_t index) const noexcept;

    [[nodiscard]] uint32_t weaponCount() const noexcept {
        return static_cast<uint32_t>(m_defs.size());
    }

    void clear();

  private:
    std::vector<WeaponDef> m_defs;
    std::unordered_map<std::string, uint32_t> m_index;
};

} // namespace fl
