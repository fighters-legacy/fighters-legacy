// SPDX-License-Identifier: GPL-3.0-or-later
// The reload-class table (#1081, D15).
//
// What these pin is the property the prose could not have: that "hot" MEANS something. A row cannot
// claim to be hot without carrying the code that applies it, every key the shipped default file
// mentions has a class, and a restart-only key that an operator edited is NAMED rather than silently
// dropped. `tools/docs_drift.py config-keys` covers the other two directions -- table vs the parser's
// key set, and table vs the documented reload matrix.
#include "ConfigReload.h"
#include "server_config.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace fl;

namespace {

// Keys mentioned in the shipped default server.toml, as "section.key". A deliberately small scanner:
// the default file is flat `key = value` lines under `[section]` headings, and using the real parser
// here would test the parser rather than the table's coverage of it.
std::set<std::string> keysInDefaultToml() {
    std::set<std::string> keys;
    std::string section;
    std::string_view text = defaultServerConfigToml();
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, (nl == std::string_view::npos ? text.size() : nl) - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;

        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        if (line.empty() || line.front() == '#')
            continue;
        if (line.front() == '[') {
            const std::size_t close = line.find(']');
            if (close == std::string_view::npos)
                continue;
            std::string_view name = line.substr(1, close - 1);
            if (!name.empty() && name.front() == '[')
                continue; // [[array.of.tables]] rows are not scalar keys
            section = std::string(name);
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos || section.empty())
            continue;
        std::string_view key = line.substr(0, eq);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.remove_suffix(1);
        if (key.empty() || key.find(' ') != std::string_view::npos)
            continue;
        keys.insert(section + "." + std::string(key));
    }
    return keys;
}

bool isRejected(std::string_view key) {
    for (std::string_view r : rejectedConfigKeys())
        if (r == key)
            return true;
    return false;
}

} // namespace

TEST_CASE("config reload table: every key carries a reader and a section-qualified name", "[config_reload]") {
    REQUIRE(configKeyTable().size() > 100); // a floor, so a broken generator cannot pass by being empty
    for (const ConfigKeyInfo& row : configKeyTable()) {
        INFO("key: " << row.key);
        CHECK(row.read != nullptr);
        CHECK(row.key.find('.') != std::string_view::npos);
    }
}

TEST_CASE("config reload table: a hot key carries the code that applies it, and only a hot key does",
          "[config_reload]") {
    // The invariant that makes the class honest. Before this, "hot" was a name in a help string, and
    // the doc's hot-reload table claimed security.banlist_path reloaded -- which reload_config has
    // never done.
    for (const ConfigKeyInfo& row : configKeyTable()) {
        INFO("key: " << row.key);
        if (row.reload == ReloadClass::Hot)
            CHECK(row.apply != nullptr);
        else
            CHECK(row.apply == nullptr);
    }
    CHECK(hotKeyCount() > 0);
}

TEST_CASE("config reload table: no key appears twice", "[config_reload]") {
    std::set<std::string_view> seen;
    for (const ConfigKeyInfo& row : configKeyTable()) {
        INFO("key: " << row.key);
        CHECK(seen.insert(row.key).second);
    }
}

TEST_CASE("config reload table: every key in the shipped default config is classified", "[config_reload]") {
    // The completeness assertion the section-level design could not have supported: a new key added
    // to the default file without a class fails here rather than silently inheriting whichever class
    // its section happened to carry.
    for (const std::string& key : keysInDefaultToml()) {
        if (isRejected(key))
            continue;
        INFO("unclassified key in the default server.toml: " << key);
        CHECK(findConfigKey(key) != nullptr);
    }
}

TEST_CASE("config reload table: a rejected key has no row", "[config_reload]") {
    // ai.provider.api_key is recognised by the parser only in order to refuse it. A row would imply
    // it is a setting.
    for (std::string_view key : rejectedConfigKeys()) {
        INFO("key: " << key);
        CHECK(findConfigKey(key) == nullptr);
    }
}

TEST_CASE("config reload table: the classification is per key, not per section", "[config_reload]") {
    // [world] is the counterexample that killed the section-level design in the issue text, so it is
    // pinned: hot and restart-only keys live side by side in the biggest section.
    REQUIRE(findConfigKey("world.entity_soft_cap") != nullptr);
    REQUIRE(findConfigKey("world.max_catchup_ticks") != nullptr);
    CHECK(findConfigKey("world.entity_soft_cap")->reload == ReloadClass::Hot);
    CHECK(findConfigKey("world.draw_distance_km")->reload == ReloadClass::Hot);
    CHECK(findConfigKey("world.sensor_check_hz")->reload == ReloadClass::Hot);
    CHECK(findConfigKey("world.max_catchup_ticks")->reload == ReloadClass::Restart);
    CHECK(findConfigKey("world.planet_radius_m")->reload == ReloadClass::Restart);
    // [network] is mixed the same way.
    CHECK(findConfigKey("network.compress_snapshots")->reload == ReloadClass::Hot);
    CHECK(findConfigKey("network.gns_nagle_time_us")->reload == ReloadClass::Restart);
}

TEST_CASE("config reload table: a credential is never printed into a diff", "[config_reload]") {
    // reload_config's output reaches stdout, the shell ring, the RCON socket and the /events mirror
    // at once. Whether a secret is SET is the operator-useful fact; its value is not.
    ServerConfig cfg;
    cfg.security.operatorPassword = "hunter2";
    cfg.rcon.password = "hunter2";
    cfg.server.password = "hunter2";
    for (std::string_view key : {"security.operator_password", "rcon.password", "server.password"}) {
        const ConfigKeyInfo* row = findConfigKey(key);
        REQUIRE(row != nullptr);
        INFO("key: " << key);
        CHECK(row->read(cfg) == "<set>");
        CHECK(row->read(ServerConfig{}) == "<unset>");
    }
}

// ---------------------------------------------------------------------------
// Diff reporting
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> keysOf(const std::vector<ConfigKeyDiff>& diffs) {
    std::vector<std::string> out;
    out.reserve(diffs.size());
    for (const ConfigKeyDiff& d : diffs)
        out.emplace_back(d.key);
    return out;
}

bool contains(const std::vector<std::string>& v, std::string_view key) {
    for (const std::string& k : v)
        if (k == key)
            return true;
    return false;
}

} // namespace

TEST_CASE("config reload: an edited restart-only key is named, with both values", "[config_reload]") {
    ServerConfig running;
    ServerConfig incoming = running;
    incoming.world.maxCatchupTicks = 16; // restart-only
    const auto ignored = restartOnlyDiffs(running, incoming);
    REQUIRE(ignored.size() == 1u);
    CHECK(ignored[0].key == "world.max_catchup_ticks");
    CHECK(ignored[0].running == "8");
    CHECK(ignored[0].incoming == "16");
}

TEST_CASE("config reload: an edited hot key is reported as changed, not as needing a restart", "[config_reload]") {
    ServerConfig running;
    ServerConfig incoming = running;
    incoming.world.entitySoftCap = 500;
    CHECK(restartOnlyDiffs(running, incoming).empty());
    const auto changed = keysOf(changedHotKeys(running, incoming));
    REQUIRE(changed.size() == 1u);
    CHECK(changed[0] == "world.entity_soft_cap");
}

TEST_CASE("config reload: an unchanged file reports nothing at all", "[config_reload]") {
    ServerConfig running;
    const ServerConfig incoming = running;
    CHECK(restartOnlyDiffs(running, incoming).empty());
    CHECK(changedHotKeys(running, incoming).empty());
}

TEST_CASE("config reload: several keys across sections are each named", "[config_reload]") {
    ServerConfig running;
    ServerConfig incoming = running;
    incoming.server.port = 4779;                // restart
    incoming.rcon.enabled = true;               // restart
    incoming.network.gnsNagleTimeUs = 5000;     // restart
    incoming.world.drawDistanceKm = 50.0;       // hot
    incoming.network.compressSnapshots = false; // hot
    const auto ignored = keysOf(restartOnlyDiffs(running, incoming));
    CHECK(ignored.size() == 3u);
    CHECK(contains(ignored, "server.port"));
    CHECK(contains(ignored, "rcon.enabled"));
    CHECK(contains(ignored, "network.gns_nagle_time_us"));

    const auto changed = keysOf(changedHotKeys(running, incoming));
    CHECK(changed.size() == 2u);
    CHECK(contains(changed, "world.draw_distance_km"));
    CHECK(contains(changed, "network.compress_snapshots"));
}

TEST_CASE("config reload: a changed secret is reported by name without its value", "[config_reload]") {
    ServerConfig running;
    ServerConfig incoming = running;
    incoming.security.operatorPassword = "hunter2";
    const auto ignored = restartOnlyDiffs(running, incoming);
    REQUIRE(ignored.size() == 1u);
    CHECK(ignored[0].key == "security.operator_password");
    CHECK(ignored[0].running == "<unset>");
    CHECK(ignored[0].incoming == "<set>");
}
