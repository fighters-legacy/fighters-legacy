// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityDef.h"

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

// Owns all registered entity type definitions. Content packs register their types here
// before the first sim tick; the registry is read-only during simulation.
//
// Threading: all methods are main-thread-only (populate before GameLoop::start()).
class EntityTypeRegistry {
  public:
    // Registers a type definition and returns its assigned index.
    // Returns std::numeric_limits<uint32_t>::max() if the id is already registered.
    uint32_t registerType(EntityDef def);

    // Returns nullptr if id is not registered.
    [[nodiscard]] const EntityDef* findById(const char* id) const noexcept;

    // Returns std::numeric_limits<uint32_t>::max() if id is not registered.
    [[nodiscard]] uint32_t indexById(const char* id) const noexcept;

    // Returns nullptr if index is out of range.
    [[nodiscard]] const EntityDef* byIndex(uint32_t index) const noexcept;

    [[nodiscard]] uint32_t typeCount() const noexcept {
        return static_cast<uint32_t>(m_defs.size());
    }

    void clear();

  private:
    std::vector<EntityDef> m_defs;
    std::unordered_map<std::string, uint32_t> m_index;
};

} // namespace fl
