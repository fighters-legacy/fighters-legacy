// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

class AssetManager;
class EntityTypeRegistry;
class ILogger;

// Loads every content-pack entity definition into `registry` (#683).
//
// Enumerates AssetManager::listAssets(AssetType::EntityDef) (first-pack-wins dedup, matching asset
// priority), parses each via parseEntityDef, and registers it. A def that fails to load, fails to
// parse, or duplicates an already-registered id logs a Warn and is skipped -- it never aborts the
// caller. Must run on the main thread before GameLoop::start() (the registry is read-only during
// simulation). Returns the number of pack types successfully registered.
uint32_t registerPackEntityDefs(AssetManager& assets, EntityTypeRegistry& registry, ILogger& log);

} // namespace fl
