# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/docs_drift.py.

Every case here drives an extractor against a fixture snippet rather than the real tree, so
the tests stay valid as the documentation is corrected. The one thing they must guarantee is
that the extractors are not silently over- or under-matching: a drift gate that quietly stops
finding names reports the documentation as perfect forever, which is worse than no gate.
"""

import importlib.util
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
_MODULE_PATH = REPO_ROOT / "tools" / "docs_drift.py"
_spec = importlib.util.spec_from_file_location("docs_drift", _MODULE_PATH)
dd = importlib.util.module_from_spec(_spec)
# Registered before exec: @dataclass resolves annotations through sys.modules, so a module
# loaded by path alone cannot define one.
sys.modules["docs_drift"] = dd
_spec.loader.exec_module(dd)


# ---- floor_check -------------------------------------------------------------------------------


def test_floor_check_passes_at_the_floor():
    result = dd.CheckResult("t")
    assert dd.floor_check(result, "thing", 10, 10) is True
    assert result.errors == []


def test_floor_check_fails_below_and_explains_itself():
    result = dd.CheckResult("t")
    assert dd.floor_check(result, "thing", 2, 10) is False
    assert "extracted 2" in result.errors[0]
    assert "do not lower the floor" in result.errors[0]


# ---- config doc key extraction -----------------------------------------------------------------

CONFIG_DOC = """\
# fl-server configuration

## Configuration precedence

| Tier | Source | Example |
|---|---|---|
| 1 (lowest) | `server.toml` | `[server] port = 9000` |

## [server] — Server identity

### `name`

| Type | Default |
|---|---|
| string | `"Unnamed Server"` |

### `port`

| Type | Default | Valid range |
|---|---|---|
| integer | `4778` | 1-65535 |

## `[http_admin]` — REST admin API

| Key | Type | Default | Description |
|---|---|---|---|
| `enabled` | bool | `false` | Off unless tokens exist |
| `port` | int | `8080` | Listener port |
| `[[http_admin.tokens]].token` | string | — | Bearer credential |

## [voice] — Voice comms

### `[[voice.nets]]`

| Key | Type | Default | Range |
|---|---|---|---|
| `id` | string | — | Net identifier |
| `gain` | float | `1.0` | 0-2 |

## Operator command reference

| Command | Arguments | Description |
|---|---|---|
| `kick` | `<peerId>` | Disconnect a peer |
"""


def test_config_doc_keys_reads_heading_style_sections():
    keys = dd.extract_config_doc_keys(CONFIG_DOC)
    assert "server.name" in keys
    assert "server.port" in keys


def test_config_doc_keys_reads_settings_table_style_sections():
    keys = dd.extract_config_doc_keys(CONFIG_DOC)
    assert "http_admin.enabled" in keys
    assert "http_admin.port" in keys


def test_config_doc_keys_attributes_array_rows_to_the_array_not_the_section():
    keys = dd.extract_config_doc_keys(CONFIG_DOC)
    assert "voice.nets.id" in keys
    assert "voice.nets.gain" in keys
    # The whole point: these are NOT keys of [voice].
    assert "voice.id" not in keys
    assert "voice.gain" not in keys


def test_config_doc_keys_handles_fully_qualified_array_rows():
    assert "http_admin.tokens.token" in dd.extract_config_doc_keys(CONFIG_DOC)


def test_config_doc_keys_ignores_non_key_tables():
    """A doc uses tables for many things; only a `Key`-headed one lists settings."""
    keys = dd.extract_config_doc_keys(CONFIG_DOC)
    assert not any(k.endswith(".kick") for k in keys)  # command reference table
    assert "server.1 (lowest)" not in keys  # precedence table
    assert not any(k.startswith("server.string") for k in keys)  # Type/Default table


RELOAD_TABLE_SRC = """
const std::array kTable = std::to_array<ConfigKeyInfo>({
    {"server.name", ReloadClass::Hot,
     [](const ServerConfig& c) { return valueText(c.server.name); }, applyBeaconName},
    {"server.port", ReloadClass::Restart,
     [](const ServerConfig& c) { return valueText(c.server.port); }, nullptr},
});
"""

RELOAD_DOC = """## Runtime administration

#### Hot-reload behaviour (`reload_config`)

Prose, and a code block that must not be mistaken for the matrix.

| Key | Reload |
|---|---|
| `server.name` | **Hot** |
| `server.port` | Restart |

#### Access control

| Key | Reload |
|---|---|
| `server.motd` | **Hot** |
"""


def test_reload_table_reads_the_class_of_each_row():
    table = dd.extract_reload_table(RELOAD_TABLE_SRC)
    assert table == {"server.name": "hot", "server.port": "restart"}


def test_reload_table_is_empty_for_a_file_without_one():
    assert dd.extract_reload_table("int main() { return 0; }") == {}


def test_reload_matrix_reads_the_documented_class():
    matrix = dd.extract_reload_matrix(RELOAD_DOC)
    assert matrix["server.name"] == "hot"
    assert matrix["server.port"] == "restart"


def test_reload_matrix_stops_at_the_next_heading():
    """A `Key | Reload` table under a LATER heading is not part of the matrix.

    Without the stop the checker would silently absorb any similarly shaped table further down
    the page, and a key missing from the real matrix would look documented.
    """
    assert "server.motd" not in dd.extract_reload_matrix(RELOAD_DOC)


def test_reload_matrix_is_empty_when_the_section_is_absent():
    assert dd.extract_reload_matrix("## Runtime administration\n\nNo matrix here.\n") == {}


def test_config_doc_keys_ignores_the_reload_matrix():
    """The matrix rows are fully qualified (`section.key`), so they are not section keys."""
    assert dd.extract_config_doc_keys(RELOAD_DOC) == set()


def test_config_doc_keys_does_not_leak_across_an_unbracketed_heading():
    """Keys after a prose h2 must not be attributed to the last config section.

    This is the bug that made a `[trace]` key look like a `[replay]` key -- the exact
    defect class the check exists to find, so the checker must not reproduce it.
    """
    doc = "## [replay] — Match recording\n\n### `dir`\n\n## Notes\n\n### `orphan`\n"
    keys = dd.extract_config_doc_keys(doc)
    assert "replay.dir" in keys
    assert "replay.orphan" not in keys


# ---- enum extraction ---------------------------------------------------------------------------

PROTOCOL_SRC = """\
enum class MsgId : uint8_t {
    Hello = 0x00,          // server->client
    ConnectAck = 0x01,
    LanBeacon = 0x20,
};

enum class ExtTag : uint16_t {
    SnapshotPeerCount = 0x0100,
    SnapshotServerThrottle = 0x0108,
};
"""


def test_enum_entries_reads_both_enums():
    assert dd._enum_entries(PROTOCOL_SRC, "MsgId") == {
        "Hello": "0x00",
        "ConnectAck": "0x01",
        "LanBeacon": "0x20",
    }
    assert dd._enum_entries(PROTOCOL_SRC, "ExtTag") == {
        "SnapshotPeerCount": "0x0100",
        "SnapshotServerThrottle": "0x0108",
    }


def test_enum_entries_returns_empty_for_a_missing_enum():
    assert dd._enum_entries(PROTOCOL_SRC, "NoSuchEnum") == {}


# ---- kMsgTable metadata extraction (#1068) -----------------------------------------------------

# The second row wraps the way clang-format wraps the real table; the extractor must not require a
# row to fit on one line.
MSG_TABLE_SRC = """\
inline constexpr MsgInfo kMsgTable[] = {
    {MsgId::Hello, "Hello", MsgDir::ServerToClient, MsgReliability::Reliable, kNetChReliable, 4, false},
    {MsgId::WorldSnapshot, "WorldSnapshot", MsgDir::ServerToClient, MsgReliability::Unreliable, kNetChUnreliable, 24,
     false},
    {MsgId::ConnectRequest, "ConnectRequest", MsgDir::ClientToServer, MsgReliability::Reliable, kNetChReliable, 104,
     true},
    {MsgId::VoiceFrame, "VoiceFrame", MsgDir::ClientToServer, MsgReliability::Unreliable, kNetChVoice, 8, false},
    {MsgId::LanBeacon, "LanBeacon", MsgDir::ServerToClient, MsgReliability::Datagram, kNoNetChannel, 78, false},
};
"""


def test_msg_table_rows_read_all_columns_including_wrapped_rows():
    rows = dd._msg_table_rows(MSG_TABLE_SRC)
    assert rows == {
        "Hello": ("server→client", "reliable", "4"),
        "WorldSnapshot": ("server→client", "unreliable", "24"),
        "ConnectRequest": ("client→server", "reliable", "104"),
        "VoiceFrame": ("client→server", "voice", "8"),
        "LanBeacon": ("server→client", "raw-udp", "78"),
    }


def test_msg_table_rows_returns_empty_when_the_table_is_absent():
    assert dd._msg_table_rows(PROTOCOL_SRC) == {}


def test_doc_delivery_normalizes_the_channel_cell_vocabulary():
    assert dd._doc_delivery("reliable") == "reliable"
    assert dd._doc_delivery("unreliable") == "unreliable"
    assert dd._doc_delivery("unreliable (voice channel)") == "voice"
    assert dd._doc_delivery("raw UDP (not ENet)") == "raw-udp"
    assert dd._doc_delivery("raw UDP") == "raw-udp"


# ---- lua binding extraction --------------------------------------------------------------------

LUA_SRC = """\
void registerAtc(lua_State* L) {
    static const luaL_Reg kFuncs[] = {
        {"clearance", luaAtcClearance},
        {"scramble", luaAtcScramble},
        {nullptr, nullptr},
    };
    lua_newtable(L);
    for (const luaL_Reg* r = kFuncs; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
    lua_setglobal(L, "atc");
}

void registerHaptics(lua_State* L) {
    static const luaL_Reg kHaptics[] = {
        {"rumble", luaRumble},
        {nullptr, nullptr},
    };
    for (const luaL_Reg* r = kHaptics; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setglobal(L, r->name);
    }
}

void registerMisc(lua_State* L) {
    lua_pushcfunction(L, luaNearbyEntities);
    lua_setglobal(L, "nearby_entities");
}
"""


def test_lua_bindings_prefix_table_entries_with_their_module():
    names = dd._lua_binding_names(LUA_SRC)
    assert "atc.clearance" in names
    assert "atc.scramble" in names


def test_lua_bindings_treat_per_entry_setglobal_as_bare_globals():
    """kHaptics registers each entry as its own global -- `rumble()`, not `haptics.rumble()`."""
    names = dd._lua_binding_names(LUA_SRC)
    assert "rumble" in names
    assert not any(n.endswith(".rumble") for n in names)


def test_lua_bindings_include_standalone_globals():
    assert "nearby_entities" in dd._lua_binding_names(LUA_SRC)


def test_lua_bindings_do_not_inherit_the_next_functions_disposition():
    """The lookahead window reaches into the following function.

    Resolving by presence rather than by position gave the `atc` table the haptics
    table's per-entry-global disposition, and every `atc.*` name vanished from the check.
    """
    names = dd._lua_binding_names(LUA_SRC)
    assert "clearance" not in names  # would be bare if the window bled through


# ---- command name extraction -------------------------------------------------------------------

COMMANDS_SRC = """\
    registry.registerCommand("help", "help [command]", 0, handler);
    registry.registerCommand(
        "status",
        "status  -- show server state",
        0, handler);
    for (const bool muteVal : {true, false}) {
        const char* name = muteVal ? "mute" : "unmute";
        const char* help = muteVal ? "mute <peerId>" : "unmute <peerId>";
        registry.registerCommand(name, help, capBit(Capability::Mute), handler);
    }
"""


def test_command_names_read_inline_and_wrapped_calls():
    names = dd._registered_command_names(COMMANDS_SRC)
    assert "help" in names
    assert "status" in names


def test_command_names_recover_loop_registered_pairs():
    names = dd._registered_command_names(COMMANDS_SRC)
    assert "mute" in names
    assert "unmute" in names


# ---- driver ------------------------------------------------------------------------------------


def test_every_check_is_reachable_from_the_cli():
    import argparse

    parser_choices = set(dd.CHECKS) | {"all"}
    assert "all" in parser_choices
    for name in dd.CHECKS:
        assert callable(dd.CHECKS[name])


@pytest.mark.parametrize("check_name", sorted(dd.CHECKS))
def test_checks_run_against_the_real_tree_without_crashing(check_name):
    """The checks may report drift -- they must never raise.

    A check that throws takes the whole gate down and gets disabled; a check that
    reports drift gets fixed.
    """
    result = dd.CHECKS[check_name]()
    assert isinstance(result, dd.CheckResult)
    # Whatever the verdict, the extractor floors must have been met: an errored check
    # means the pattern broke against the current tree, which is a bug in this script.
    assert result.errors == [], result.errors


# ---- input actions -----------------------------------------------------------------------------

INPUT_ACTION_SRC = """\
enum class InputAction : uint32_t {
    // Continuous axes
    PitchAxis,
    RollAxis,

    // Master arm (#641). The old `FireMissile` name and `InputAction::MasterArm` appear in this
    // comment on purpose: prose inside the enum must not be mistaken for enumerators.
    MasterArm,
    FireStore,

    Count
};
"""


def test_input_action_names_reads_the_enum_and_ignores_its_comments():
    names = dd._input_action_names(INPUT_ACTION_SRC)
    assert names == {"PitchAxis", "RollAxis", "MasterArm", "FireStore"}
    assert "Count" not in names
    assert "FireMissile" not in names  # named only in a comment


def test_input_action_names_returns_empty_when_the_enum_is_absent():
    assert dd._input_action_names("enum class Other : uint8_t { A, B };\n") == set()


def test_input_actions_check_flags_an_undocumented_action(tmp_path, monkeypatch):
    """An action added to the enum but never written into the key map is the drift to catch."""

    def fake_read(rel_path: str) -> str:
        if rel_path.endswith("InputAction.h"):
            return INPUT_ACTION_SRC
        return "| Fire the gun | `FireStore` |\n| Pitch | `PitchAxis` |\n"

    monkeypatch.setattr(dd, "read", fake_read)
    monkeypatch.setattr(dd, "floor_check", lambda *a, **k: True)
    result = dd.check_input_actions()
    assert result.code_only == {"RollAxis", "MasterArm"}
    assert not result.ok
