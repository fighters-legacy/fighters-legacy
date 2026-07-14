// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ClientPrediction.h"

namespace fl {

class AssetManager;
class EntityTypeRegistry;
class ILogger;

// Builds the resolver ClientPrediction uses to find the flight model for an entity type (#811).
//
// THE BUG THIS REPLACES. The client's EntityDef comes off the wire, and MsgEntityTypeDef did not
// carry a flight model. So the resolver went back to disk to re-load the entity def BY ITS ID --
// loadEntityDef("fl-base:f15c") -> "entities/fl-base:f15c.toml" -- a filename with a colon in it,
// which does not exist anywhere and is not legal on Windows. The miss was swallowed by a bare
// catch (...), and every pack aircraft was predicted with the builtin UFO model while the server
// integrated the real one. Permanent prediction divergence, and not one line of it was logged.
//
// The wire now carries EntityDef::flightModelAsset, so the client reads the field it was always
// missing. Every fallback to the builtin model is logged at ERROR and names the id: a silent
// fallback to a different aircraft is the bug, a loud one is a diagnosable misconfiguration.
//
// Parsed models are cached per typeIndex -- the old lambda re-parsed the TOML on every resolve.
// `registry`, `assets` and `log` must outlive the returned resolver.
ClientPrediction::FlightModelResolver makeFlightModelResolver(const EntityTypeRegistry& registry, AssetManager& assets,
                                                              ILogger& log);

} // namespace fl
