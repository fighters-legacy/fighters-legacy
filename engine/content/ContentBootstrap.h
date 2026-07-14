// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace fl {

class AssetManager;
class ContentIndex;
class EntityTypeRegistry;
class ILogger;
class WeaponRegistry;

namespace sensor {
struct SensorDef;
}

// Loads every content-pack entity definition into `registry` (#683).
//
// Enumerates AssetManager::listAssets(AssetType::EntityDef) (first-pack-wins dedup, matching asset
// priority), parses each via parseEntityDef, and registers it. A def that fails to load, fails to
// parse, or duplicates an already-registered id logs a Warn and is skipped -- it never aborts the
// caller. Must run on the main thread before GameLoop::start() (the registry is read-only during
// simulation). Returns the number of pack types successfully registered.
uint32_t registerPackEntityDefs(AssetManager& assets, EntityTypeRegistry& registry, ILogger& log);

// Loads every content-pack weapon definition into `registry` (#812), the same shape as
// registerPackEntityDefs above. Enumerates AssetManager::listAssets(AssetType::Weapon), parses each
// via parseWeaponDef, and registers it by its ID -- so a hardpoint resolves its stores without ever
// touching the filesystem. A def that fails to load, fails to parse, or duplicates an already
// registered id logs a Warn and is skipped; it never aborts the caller.
//
// Call BEFORE registerPackEntityDefs, so an entity's hardpoints have weapons to resolve against.
// Main thread, before GameLoop::start(). Returns the number of weapons registered.
uint32_t registerPackWeaponDefs(AssetManager& assets, WeaponRegistry& registry, ILogger& log);

// Builds the resolver WorldBroadcaster calls on the spawn path to turn an EntityDef::sensorIds entry
// into a parsed SensorDef (#685), routed through ContentIndex (#810).
//
// A sensor reference is an ID ("fl-base:apq159"), not an asset name. Handing it straight to
// AssetManager -- which is what this code did until #810 -- builds "sensors/fl-base:apq159.toml", a
// path that cannot exist, so EVERY aircraft in every pack silently flew with no radar. The index is
// what makes the id resolvable; this factory is what makes that resolution testable.
//
// A miss is logged at ERROR and yields nullptr: the entity keeps the rest of its suite (and, with
// none left, the builtin eyeball) rather than being denied a spawn over one missing file. Misses are
// cached too, so a bad id is not re-reported on every spawn.
//
// `assets`, `index` and `log` must outlive the returned resolver.
using SensorDefResolver = std::function<std::shared_ptr<const sensor::SensorDef>(const std::string& id)>;
SensorDefResolver makeSensorDefResolver(AssetManager& assets, const ContentIndex& index, ILogger& log);

} // namespace fl
