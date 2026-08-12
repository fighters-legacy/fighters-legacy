// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "server_config.h"

#include <config/DifficultySettings.h> // AiScaling — resolved off the sim thread by reload_config

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

struct ServerCommandContext;

// The reload class of every server.toml key (#1081, D15).
//
// This used to live in two pieces of prose that disagreed with the code and with each other: a
// per-field sentence in server-config.md, and a hand-maintained list of 24 field names spelled out
// inline in `reload_config`'s help text, ending in "(other fields ... require restart)". An operator
// editing a restart-only key got silence -- not an explanation -- and the doc's hot-reload table
// claimed `security.banlist_path` reloads, which `reload_config` has never done.
//
// ⚑ The class is per KEY, not per section, and that is not a corner case -- it is the biggest
// section. `[world]` holds `entity_soft_cap` (hot, deliberately: #1049 made it the lever an operator
// pulls to relieve a world refusing spawns) next to `max_catchup_ticks` (restart: a GameLoop
// constructor value) and `planet_radius_m` (restart: terrain tiles bake the radius at generation
// time). `[network]` is mixed the same way. A section-level table could not express the truth, and a
// misclassified row is worse than the prose it replaces: it tells an operator a key applies live when
// it does not, so they stop double-checking.
enum class ReloadClass {
    Hot,     // reload_config applies it; the row carries the code that does so
    Restart, // the running server keeps its startup value; reload_config NAMES the difference
};

// What a hot key needs in order to apply itself. Built by reload_config and passed to every Hot row's
// apply function from inside ONE sim callback.
struct ReloadApplyContext {
    const ServerCommandContext& ctx;
    // The freshly parsed file. A row reads its OWN key from here.
    const ServerConfig& incoming;
    // The config the process started with. A row reads any RESTART-ONLY value it depends on from here
    // -- `entity_soft_cap` re-derives its player reserve from `max_peers`, and taking that from the
    // incoming file would apply a restart-only key through the back door.
    const ServerConfig& running;
    // Resolved off the sim thread (it may read data/difficulty.toml through the AssetManager).
    // nullopt = no resolver wired, so the running scaling is left alone rather than reset (#682).
    const std::optional<AiScaling>& aiScaling;
};

using ConfigApplyFn = void (*)(const ReloadApplyContext&);
using ConfigReadFn = std::string (*)(const ServerConfig&);

struct ConfigKeyInfo {
    std::string_view key; // "section.key", exactly as it appears in server.toml
    ReloadClass reload;
    // The key's value as operator-facing text, so reload_config can diff the incoming file against
    // the running config and report `key (old -> new)` without a comparison written per key.
    ConfigReadFn read;
    // Non-null iff reload == Hot. That is the invariant that makes "hot" mean something: a key cannot
    // be advertised as hot without the code that applies it, and a test asserts the correspondence
    // both ways.
    ConfigApplyFn apply;
};

// Every key, in server.toml order. Sorted within a section by nothing in particular -- the order is
// the file's, because an operator reading the reload matrix is reading it beside their config.
[[nodiscard]] std::span<const ConfigKeyInfo> configKeyTable();

// nullptr when the key is unknown.
[[nodiscard]] const ConfigKeyInfo* findConfigKey(std::string_view key);

// Keys the parser recognises ONLY in order to refuse them, so they hold no value and have no reload
// class. Today that is `ai.provider.api_key`: naming it in a config file is the mistake the parser
// exists to catch, and giving it a row here would imply it is a setting.
[[nodiscard]] std::span<const std::string_view> rejectedConfigKeys();

// A key whose incoming value differs from the running one.
struct ConfigKeyDiff {
    std::string_view key;
    std::string running;
    std::string incoming;
};

// How many keys reload_config applies. Derived from the table, so it cannot fall behind it the way
// the hand-written list in the command's help text did.
[[nodiscard]] std::size_t hotKeyCount();

// Apply every Hot key. Call from the SIM THREAD: the appliers touch live sim objects.
void applyHotKeys(const ReloadApplyContext& rc);

// The Hot keys whose value changed -- what reload_config reports as applied. Two plain configs and
// no live objects, so it runs on the calling thread and answers in the synchronous response.
[[nodiscard]] std::vector<ConfigKeyDiff> changedHotKeys(const ServerConfig& running, const ServerConfig& incoming);

// The Restart-only keys whose value changed: exactly what the reload IGNORED, and the half an
// operator used to get silence for. Pure reads; safe on any thread.
[[nodiscard]] std::vector<ConfigKeyDiff> restartOnlyDiffs(const ServerConfig& running, const ServerConfig& incoming);

} // namespace fl
