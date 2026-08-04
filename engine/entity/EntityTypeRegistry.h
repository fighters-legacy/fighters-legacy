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

    // Monotonic change counter, bumped by every successful registerType and by clear(). A consumer
    // that has REPLICATED this table (the #1070 connect-ack skip) remembers the value it replicated
    // and compares. typeCount() cannot answer that question: clear() followed by re-registering the
    // same number of types leaves the count identical and the contents different, and a consumer
    // keying on the count would keep a stale table forever. Starts at 0 = "nothing registered yet",
    // which is why a replicated generation of 0 never matches a populated registry.
    [[nodiscard]] uint32_t generation() const noexcept {
        return m_generation;
    }

    void clear();

  private:
    std::vector<EntityDef> m_defs;
    std::unordered_map<std::string, uint32_t> m_index;
    uint32_t m_generation{0};
};

} // namespace fl
