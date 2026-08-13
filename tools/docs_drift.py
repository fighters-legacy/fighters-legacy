#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Fighters Legacy contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Documentation drift checks: does the prose still match the code?

Every check extracts a set of names from a source of truth in the codebase, extracts the
matching set from the documentation, and diffs them both ways. Names present only in code
are undocumented; names present only in the docs are ghosts -- text describing something the
binary does not have, which is the worse of the two because a reader acts on it.

Each extractor asserts a minimum expected count before comparing. A grep-shaped extractor
that silently stops matching (someone renames a variable, moves a table, reformats a
heading) would otherwise produce two empty sets, diff them successfully, and report the
documentation as perfect forever. The floors are deliberately far below the real counts:
they catch "the pattern broke", not "the number changed".

Usage:
    tools/docs_drift.py all
    tools/docs_drift.py config-keys --verbose
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


# --------------------------------------------------------------------------------------
# Result plumbing
# --------------------------------------------------------------------------------------


@dataclass
class CheckResult:
    """Outcome of one check: the two sets, plus any hard errors that stopped the diff."""

    name: str
    code_only: set[str] = field(default_factory=set)
    doc_only: set[str] = field(default_factory=set)
    errors: list[str] = field(default_factory=list)
    code_count: int = 0
    doc_count: int = 0
    # Names the check knows about but deliberately does not require on either side.
    waived: set[str] = field(default_factory=set)

    @property
    def ok(self) -> bool:
        return not self.errors and not self.code_only and not self.doc_only

    def report(self, verbose: bool = False) -> None:
        status = "PASS" if self.ok else "FAIL"
        print(f"[{status}] {self.name}  (code: {self.code_count}, docs: {self.doc_count})")
        for err in self.errors:
            print(f"    error: {err}")
        if self.code_only:
            print(f"    undocumented ({len(self.code_only)}) -- in code, absent from docs:")
            for item in sorted(self.code_only):
                print(f"        {item}")
        if self.doc_only:
            print(f"    ghosts ({len(self.doc_only)}) -- in docs, absent from code:")
            for item in sorted(self.doc_only):
                print(f"        {item}")
        if verbose and self.waived:
            print(f"    waived ({len(self.waived)}): {', '.join(sorted(self.waived))}")


def read(rel_path: str) -> str:
    path = REPO_ROOT / rel_path
    if not path.is_file():
        raise FileNotFoundError(f"source of truth missing: {rel_path}")
    return path.read_text(encoding="utf-8")


def floor_check(result: CheckResult, label: str, got: int, minimum: int) -> bool:
    """Guard an extractor against silently matching nothing.

    Returns False when the floor is not met, in which case the caller must not diff --
    an empty extraction compared against an empty extraction looks like success.
    """
    if got < minimum:
        result.errors.append(
            f"{label} extracted {got} names, expected at least {minimum}. "
            f"The extraction pattern has probably broken -- fix this script, do not lower the floor."
        )
        return False
    return True


# --------------------------------------------------------------------------------------
# check: config-keys
# --------------------------------------------------------------------------------------

# Keys the parser reads but that are not operator-facing configuration:
# sub-table roots that exist only as a path to their children.
CONFIG_SUBTABLE_ROOTS = {"ai.mcp", "ai.provider", "ai.chat_intent"}

# Array-of-table row fields. The parser reads these off each element table rather than off
# `tbl`, so the tbl["a"]["b"] pattern cannot see them; the docs spell them as
# `[[section.array]].field`. Listed here so both sides agree on what is out of scope for
# the automated key diff (they are still verified by hand in the audit).
# Keys the parser recognises only in order to REFUSE them, so they hold no value and have no reload
# class. Mirrors rejectedConfigKeys() in ConfigReload.cpp.
CONFIG_REJECTED_KEYS = {"ai.provider.api_key"}

CONFIG_ARRAY_FIELDS = {
    "http_admin.tokens",
    "spawn.points",
    "voice.nets",
    "wind.profile",
    "rotation.items",
    "mods.stack",
    "mods.required",
    "server.game_modes",
    "ai.mcp.allowlist",
}


def check_config_keys() -> CheckResult:
    result = CheckResult("config-keys: server.toml vs docs/server-ops/server-config.md")
    src = read("server/fl-server/server_config.cpp")

    code: set[str] = set()
    # Three-level first (dotted sections such as [ai.mcp]), so the two-level pattern does
    # not shadow them with a bare "ai.mcp".
    for sec, sub, key in re.findall(r'tbl\["([a-z_]+)"\]\["([a-z_]+)"\]\["([a-z_]+)"\]', src):
        code.add(f"{sec}.{sub}.{key}")
    for sec, key in re.findall(r'tbl\["([a-z_]+)"\]\["([a-z_]+)"\]', src):
        code.add(f"{sec}.{key}")
    # The clampDouble helper reads through a variable subscript, so it is invisible above.
    for sec, key in re.findall(r'clampDouble\(tbl,\s*"([a-z_]+)",\s*"([a-z_]+)"', src):
        code.add(f"{sec}.{key}")

    def is_array_row_field(key: str) -> bool:
        """True for `<array>.<field>` rows, which the tbl[...] pattern cannot see.

        The parser reads these off each element table (`(*t)["token"]`), so the code side
        of this check is structurally blind to them; excluding both sides keeps the diff
        honest rather than reporting every documented row field as a ghost. They are
        verified by hand in the audit instead.
        """
        return key in CONFIG_ARRAY_FIELDS or any(
            key.startswith(f"{array}.") for array in CONFIG_ARRAY_FIELDS
        )

    code = {k for k in code if k not in CONFIG_SUBTABLE_ROOTS and not is_array_row_field(k)}
    doc = {k for k in extract_config_doc_keys(read("docs/server-ops/server-config.md")) if not is_array_row_field(k)}

    result.code_count = len(code)
    result.doc_count = len(doc)
    result.waived = CONFIG_SUBTABLE_ROOTS | CONFIG_ARRAY_FIELDS
    if not floor_check(result, "server_config.cpp keys", len(code), 100):
        return result
    if not floor_check(result, "fl-server-config.md keys", len(doc), 100):
        return result

    result.code_only = code - doc
    result.doc_only = doc - code

    # The reload class of every key (#1081, D15). Three sources have to agree, not two: the parser,
    # the reload-class TABLE, and the documented matrix. Without this, a key could be added to the
    # parser and documented while nothing classified it -- which is the state this issue found, and
    # the state that let the doc claim security.banlist_path hot-reloads when it never has.
    table = extract_reload_table(read("server/fl-server/ConfigReload.cpp"))
    if not floor_check(result, "ConfigReload.cpp rows", len(table), 100):
        return result
    # Array keys are invisible to the parser-side regex above (the parser reads them off the array,
    # not through tbl[...]), but they ARE classified: an operator editing mods.stack should be told
    # it needs a restart. They are the only legitimate table-not-in-parser rows.
    result.code_only |= (code - set(table)) - CONFIG_REJECTED_KEYS
    result.doc_only |= {k for k in set(table) - code if not is_array_row_field(k)}

    matrix = extract_reload_matrix(read("docs/server-ops/server-config.md"))
    if not floor_check(result, "documented reload matrix rows", len(matrix), 100):
        return result
    for key in sorted(set(table) | set(matrix)):
        if key not in matrix:
            result.code_only.add(f"{key} (reload class missing from the documented matrix)")
        elif key not in table:
            result.doc_only.add(f"{key} (documented reload class for a key with no table row)")
        elif table[key] != matrix[key]:
            result.doc_only.add(f"{key} (table says {table[key]}, docs say {matrix[key]})")
    return result


def extract_reload_table(text: str) -> dict[str, str]:
    """`{"section.key", ReloadClass::Hot, ...}` rows from the reload-class table."""
    return {k: c.lower() for k, c in re.findall(r'\{"([a-z_.]+)",\s*ReloadClass::(Hot|Restart)', text)}


def extract_reload_matrix(text: str) -> dict[str, str]:
    """The `| `key` | Hot/Restart |` matrix under the hot-reload heading in server-config.md."""
    matrix: dict[str, str] = {}
    in_matrix = False
    for line in text.splitlines():
        if line.startswith("#### Hot-reload behaviour"):
            in_matrix = True
            continue
        if in_matrix and line.startswith("#"):
            break
        if not in_matrix or not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) != 2:
            continue
        key = re.match(r"^`([a-z_.]+)`$", cells[0])
        if not key:
            continue
        matrix[key.group(1)] = cells[1].replace("*", "").strip().lower()
    return matrix


def extract_config_doc_keys(text: str) -> set[str]:
    """Walk the config reference, tracking the current [section] heading.

    Two heading styles are in use -- `## [server] -- ...` and ``## `[http_admin]` -- ...`` --
    and keys appear two ways: as `### \\`key\\`` headings (the older sections, each followed
    by a Type/Default property table) and as rows of a settings table (the sections added
    with the REST and MCP surfaces).

    A table row counts as a key only when its table's header row begins with a `Key`
    column. The document uses tables for a great many other things -- the CLI flag list,
    the precedence tiers, per-key Type/Default tables, the MCP tool catalog, the
    hot-reload matrix -- and every one of those has a leading backticked cell that would
    otherwise be mistaken for a configuration key.
    """
    keys: set[str] = set()
    section: str | None = None
    array: str | None = None
    in_key_table = False
    for line in text.splitlines():
        # Any h2 ends the previous section. Only a bracketed one starts a new one, so the
        # prose sections between config sections do not inherit the last section's name.
        if line.startswith("## "):
            heading = re.match(r"^##\s+`?\[([a-z_.]+)\]`?", line)
            section = heading.group(1) if heading else None
            array = None
            in_key_table = False
            continue
        if line.startswith("#"):
            in_key_table = False
            # `### `[[voice.nets]]`` opens an array-of-tables subsection: the rows that
            # follow are that array's row fields, not keys of the enclosing section.
            array_heading = re.match(r"^###\s+`\[\[([a-z_.]+)\]\]`", line)
            if array_heading:
                array = array_heading.group(1)
                continue
            array = None
            if line.startswith("### ") and section:
                # One heading may cover several keys that share an explanation, e.g.
                # "### `incoming_bandwidth_bps` / `outgoing_bandwidth_bps`".
                for key in re.findall(r"`([a-z_]+)`", line):
                    keys.add(f"{section}.{key}")
            continue

        if line.startswith("|"):
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if not cells:
                continue
            if cells[0].lower() == "key":
                in_key_table = True
                continue
            if set(cells[0]) <= {"-", ":"} and cells[0]:
                continue  # separator row
            if in_key_table and (section or array):
                cell = cells[0]
                # `[[http_admin.tokens]].token` -- an array-of-tables row field.
                qualified = re.match(r"^`\[\[([a-z_.]+)\]\]\.([a-z_]+)`$", cell)
                if qualified:
                    keys.add(f"{qualified.group(1)}.{qualified.group(2)}")
                    continue
                plain = re.match(r"^`([a-z_]+)`$", cell)
                if plain:
                    keys.add(f"{array or section}.{plain.group(1)}")
            continue

        if not line.strip():
            continue
        in_key_table = False
    return keys


# --------------------------------------------------------------------------------------
# check: msg-ids
# --------------------------------------------------------------------------------------


def _enum_entries(src: str, enum_name: str) -> dict[str, str]:
    """Pull `Name = 0xNN,` rows out of one enum body."""
    match = re.search(rf"enum class {enum_name}\s*:\s*\w+\s*\{{(.*?)^\}};", src, re.S | re.M)
    if not match:
        return {}
    return {
        name: value.lower()
        for name, value in re.findall(r"^\s*(\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*,", match.group(1), re.M)
    }


# kMsgTable row -> the doc's Channel-column vocabulary. The pair (reliability, channel) collapses
# to one word because that is what a reader needs: which delivery contract the message rides.
_MSG_DELIVERY = {
    ("Reliable", "kNetChReliable"): "reliable",
    ("Unreliable", "kNetChUnreliable"): "unreliable",
    ("Unreliable", "kNetChVoice"): "voice",
    ("Datagram", "kNoNetChannel"): "raw-udp",
}

_MSG_DIR = {
    "ClientToServer": "client→server",
    "ServerToClient": "server→client",
    "Reserved": "reserved",
}

# One kMsgTable row. clang-format wraps long rows, so whitespace matches across newlines (re.S at
# the call site is not enough on its own -- \s already spans lines, but keep the pattern tolerant).
_MSG_TABLE_ROW = re.compile(
    r"\{MsgId::(\w+),\s*\"(\w+)\",\s*MsgDir::(\w+),\s*MsgReliability::(\w+),\s*"
    r"(kNetChReliable|kNetChUnreliable|kNetChVoice|kNoNetChannel),\s*(\d+),\s*(?:true|false)\}"
)


def _msg_table_rows(src: str) -> dict[str, tuple[str, str, str]]:
    """Name -> (direction, delivery, min-bytes) from kMsgTable, the message-metadata authority."""
    block = re.search(r"constexpr MsgInfo kMsgTable\[\] = \{(.*?)\n\};", src, re.S)
    if not block:
        return {}
    rows: dict[str, tuple[str, str, str]] = {}
    for _, name, direction, reliability, channel, min_bytes in _MSG_TABLE_ROW.findall(block.group(1)):
        rows[name] = (
            _MSG_DIR.get(direction, f"?{direction}"),
            _MSG_DELIVERY.get((reliability, channel), f"?{reliability}/{channel}"),
            min_bytes,
        )
    return rows


def _doc_delivery(cell: str) -> str:
    """Normalize the doc's Channel cell to the delivery vocabulary."""
    cell = cell.strip()
    if cell.startswith("raw UDP"):
        return "raw-udp"
    if "voice" in cell:
        return "voice"
    return cell


def check_msg_ids() -> CheckResult:
    result = CheckResult("msg-ids: GameProtocol.h vs docs/developer/network-protocol.md")
    src = read("engine/net/GameProtocol.h")
    doc = read("docs/developer/network-protocol.md")

    # --- id/name presence, both registries (MsgId + ExtTag), diffed both ways -------------------
    code: set[str] = set()
    for enum_name in ("MsgId", "ExtTag"):
        for name, value in _enum_entries(src, enum_name).items():
            code.add(f"{name}={value}")

    # Doc tables spell both as | `Name` | `0xNN` | ...
    documented = {
        f"{name}={value.lower()}"
        for name, value in re.findall(r"^\|\s*`(\w+)`\s*\|\s*`(0x[0-9A-Fa-f]+)`\s*\|", doc, re.M)
    }

    # --- per-message metadata columns: kMsgTable vs the ## Messages summary table ---------------
    # Doc row: | `Name` | `0xNN` | direction | channel | size | purpose |. The size cell must START
    # with the message's minimum byte count (the fixed struct/header); the formula tail is prose.
    table_rows = _msg_table_rows(src)
    for name, (direction, delivery, min_bytes) in table_rows.items():
        code.add(f"{name} direction={direction}")
        code.add(f"{name} delivery={delivery}")
        code.add(f"{name} min-bytes={min_bytes}")

    doc_msg_row = re.compile(
        r"^\|\s*`(\w+)`\s*\|\s*`0x[0-9A-Fa-f]+`\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|", re.M
    )
    doc_meta_count = 0
    for name, direction, channel, size in doc_msg_row.findall(doc):
        if name not in table_rows:
            continue  # an ExtTag row, or a ghost the presence diff above already reports
        doc_meta_count += 1
        documented.add(f"{name} direction={direction.strip()}")
        documented.add(f"{name} delivery={_doc_delivery(channel)}")
        size_lead = re.match(r"(\d+)", size.strip())
        documented.add(f"{name} min-bytes={size_lead.group(1) if size_lead else size.strip()}")

    result.code_count = len(code)
    result.doc_count = len(documented)
    if not floor_check(result, "GameProtocol.h enum rows", len(_enum_entries(src, 'MsgId')), 30):
        return result
    if not floor_check(result, "GameProtocol.h kMsgTable rows", len(table_rows), 30):
        return result
    if not floor_check(result, "network-protocol.md message rows", doc_meta_count, 30):
        return result

    result.code_only = code - documented
    result.doc_only = documented - code
    return result


# --------------------------------------------------------------------------------------
# check: lua-names
# --------------------------------------------------------------------------------------

def check_lua_names() -> CheckResult:
    result = CheckResult("lua-names: LuaController.cpp vs docs/modding/*.md")
    src = read("engine/script/LuaController.cpp")

    code = _lua_binding_names(src)
    docs = "\n".join(
        read(p)
        for p in (
            "docs/modding/ai.md",
            "docs/modding/missions.md",
            # The scoring bindings are documented with the game modes that award points,
            # which is where an author looking for them actually is.
            "docs/modding/game-modes.md",
        )
    )

    documented: set[str] = set()
    for name in code:
        # Documented means "named as a call in prose or a table" -- `world.spawn(` or
        # `world.spawn` in a reference row. Bare mention inside a wider sentence counts;
        # the audit's job is to catch absence, not to grade the explanation.
        if re.search(rf"`{re.escape(name)}\b", docs):
            documented.add(name)

    result.code_count = len(code)
    result.doc_count = len(documented)
    if not floor_check(result, "LuaController.cpp bindings", len(code), 15):
        return result

    result.code_only = code - documented
    # Ghost detection runs over the documented module prefixes only: a doc naming a
    # binding that does not exist is the dangerous direction, because a mission script
    # calling it fails at runtime in the field with the doc as its justification.
    prefixes = {n.split(".", 1)[0] for n in code if "." in n}
    claimed: set[str] = set()
    for prefix in prefixes:
        for name in re.findall(rf"`{prefix}\.([a-z_0-9]+)", docs):
            claimed.add(f"{prefix}.{name}")
    result.doc_only = claimed - code
    return result


def _lua_binding_names(src: str) -> set[str]:
    """Names Lua scripts can call, derived from each luaL_Reg table and its global.

    The tables are all named `kFuncs`/`kHaptics` inside separate functions, so the module
    prefix comes from the `lua_setglobal(L, "<module>")` that follows the registration
    loop. A loop that calls `lua_setglobal(L, r->name)` instead registers every entry as a
    bare global (the haptics functions), which is why the prefix is resolved per table
    rather than assumed.
    """
    names: set[str] = set()
    for match in re.finditer(r"luaL_Reg\s+(\w+)\[\]\s*=\s*\{(.*?)\};", src, re.S):
        entries = re.findall(r'\{\s*"([a-z_0-9]+)"\s*,\s*(\w+)\s*\}', match.group(2))
        tail = src[match.end() : match.end() + 900]
        module = re.search(r'lua_setglobal\(\s*\w+\s*,\s*"([a-z_0-9]+)"\s*\)', tail)
        per_entry_global = re.search(r"lua_setglobal\(\s*\w+\s*,\s*r->name\s*\)", tail)
        # Whichever setglobal comes first is this table's -- the tail window reaches into
        # the next function, so testing presence rather than position would give every
        # table the disposition of the one registered after it.
        if module and per_entry_global:
            if per_entry_global.start() < module.start():
                module = None
            else:
                per_entry_global = None
        for entry_name, _fn in entries:
            if per_entry_global:
                names.add(entry_name)
            elif module:
                names.add(f"{module.group(1)}.{entry_name}")
            else:
                names.add(entry_name)
    # Standalone globals pushed one at a time rather than through a table.
    for match in re.finditer(r'lua_setglobal\(\s*\w+\s*,\s*"([a-z_0-9]+)"\s*\)', src):
        name = match.group(1)
        if not any(n.startswith(f"{name}.") for n in names):
            names.add(name)
    return names


# --------------------------------------------------------------------------------------
# check: commands
# --------------------------------------------------------------------------------------


def check_commands() -> CheckResult:
    result = CheckResult("commands: server/MCP/REST surfaces vs docs")
    code: set[str] = set()

    src = read("server/fl-server/ServerCommands.cpp")
    for name in _registered_command_names(src):
        code.add(f"cmd:{name}")

    http = read("server/fl-server/HttpAdminServer.cpp")
    for verb, route in re.findall(r'\.(Get|Post|Put|Delete)\(\s*"(/[a-z_/]*)"', http):
        code.add(f"rest:{verb.upper()} {route}")

    mcp = read("server/fl-server/McpProtocol.cpp")
    for tool in re.findall(r'ToolDesc\{\s*"([a-z_]+)"', mcp):
        code.add(f"mcp:{tool}")
    if not any(n.startswith("mcp:") for n in code):
        # Tool catalog is a constexpr array; fall back to the known tool-name vocabulary.
        for tool in re.findall(r'"(world_state|events|admin_command|submit_mission)"', mcp):
            code.add(f"mcp:{tool}")

    docs = "\n".join(
        read(p)
        for p in (
            "docs/server-ops/server-config.md",
            "docs/server-ops/admin-api.md",
            "docs/server-ops/mcp-agent-surface.md",
            "docs/user-guide/controls.md",
            "docs/developer/debug-console.md",
            "docs/developer/ai-architecture.md",
        )
    )
    documented: set[str] = set()
    for name in {n.split(":", 1)[1] for n in code if n.startswith("cmd:")}:
        if re.search(rf"`{re.escape(name)}[ `]", docs) or re.search(rf"^\|\s*`{re.escape(name)}`", docs, re.M):
            documented.add(f"cmd:{name}")
    for entry in {n for n in code if n.startswith("rest:")}:
        route = entry.split(" ", 1)[1]
        # An endpoint reference spells the route with its query string --
        # `/events?after=N&max=M` -- so anchor on the path and allow a documented suffix.
        if re.search(rf"`{re.escape(route)}[?` ]", docs):
            documented.add(entry)
    for entry in {n for n in code if n.startswith("mcp:")}:
        tool = entry.split(":", 1)[1]
        if re.search(rf"`{re.escape(tool)}`", docs):
            documented.add(entry)

    result.code_count = len(code)
    result.doc_count = len(documented)
    if not floor_check(result, "command/route/tool names", len(code), 30):
        return result

    result.code_only = code - documented
    # The doc side is derived from the code side here (we ask "is this name mentioned"),
    # so a ghost cannot appear; ghost detection for prose commands stays a judgment call.
    return result


def _registered_command_names(src: str) -> set[str]:
    """Names passed to registerCommand(), including multi-line and loop registrations."""
    names: set[str] = set()
    for match in re.finditer(r"registerCommand\(", src):
        tail = src[match.end() : match.end() + 400]
        literal = re.match(r'\s*"([a-z_0-9]+)"', tail)
        if literal:
            names.add(literal.group(1))
            continue
        # Loop registration: the name is a variable assigned from a ternary just above.
        head = src[max(0, match.start() - 400) : match.start()]
        ternary = re.findall(r'\?\s*"([a-z_0-9]+)"\s*:\s*"([a-z_0-9]+)"', head)
        for first, second in ternary:
            names.add(first)
            names.add(second)
    return names


# --------------------------------------------------------------------------------------
# check: tools-list
# --------------------------------------------------------------------------------------

# Sample/demo executables that are not part of the documented tool surface.
TOOLS_NOT_DOCUMENTED = {"hello_triangle"}


def check_tools_list() -> CheckResult:
    result = CheckResult("tools-list: tools/CMakeLists.txt vs docs")
    cmake = read("tools/CMakeLists.txt")
    code = set(re.findall(r"add_executable\(\s*([A-Za-z0-9_-]+)", cmake)) - TOOLS_NOT_DOCUMENTED

    doc_paths = ["docs/developer/development.md", "README.md"]
    tools_catalog = REPO_ROOT / "docs/developer/tools.md"
    if tools_catalog.is_file():
        doc_paths.append("docs/developer/tools.md")
    docs = "\n".join(read(p) for p in doc_paths)

    documented = {name for name in code if re.search(rf"`{re.escape(name)}[ `\n]", docs)}

    result.code_count = len(code)
    result.doc_count = len(documented)
    result.waived = TOOLS_NOT_DOCUMENTED
    if not floor_check(result, "tools/CMakeLists.txt executables", len(code), 15):
        return result

    result.code_only = code - documented
    return result


# --------------------------------------------------------------------------------------
# check: input-actions
# --------------------------------------------------------------------------------------


def _input_action_names(src: str) -> set[str]:
    """Pull the InputAction enumerator names out of the enum body.

    Comments inside the enum name actions in prose (`InputAction::MasterArm`, `FireMissile`), so
    they are stripped first -- matching them would inflate the count with names that are not
    enumerators and hide a real one going missing.
    """
    match = re.search(r"enum class InputAction\s*:\s*\w+\s*\{(.*?)^\};", src, re.S | re.M)
    if not match:
        return set()
    body = re.sub(r"//[^\n]*", "", match.group(1))
    return {name for name in re.findall(r"^\s*(\w+)\s*,", body, re.M) if name != "Count"}


def check_input_actions() -> CheckResult:
    """Every InputAction must appear in the player-facing key map, and vice versa.

    The V-on-two-live-actions defect (#1050) was found by diffing docs/sandbox.md against
    InputBindings::applyDefaults() BY HAND. The binding table is now the authority for every
    gameplay control, so the key map is the one document that goes stale the moment an action is
    added -- which is exactly the drift this gate exists to catch.
    """
    result = CheckResult("input-actions: InputAction.h vs docs/user-guide/controls.md")
    src = read("engine/input/InputAction.h")
    doc = read("docs/user-guide/controls.md")

    code = _input_action_names(src)
    if not code:
        result.errors.append("could not find `enum class InputAction` in engine/input/InputAction.h")
        return result

    # The doc names an action in a `Binding` column cell, e.g. | ... | `MasterArm` |
    documented = {name for name in code if f"`{name}`" in doc}

    result.code_count = len(code)
    result.doc_count = len(documented)
    if not floor_check(result, "InputAction.h enum entries", len(code), 60):
        return result

    result.code_only = code - documented
    return result


# --------------------------------------------------------------------------------------
# check: input-keys
# --------------------------------------------------------------------------------------

# Pages whose key tables are checked against applyDefaults(). The whole user guide, because the
# defect this check exists for (#1047) was a page OUTSIDE the one the input-actions check reads:
# quickstart.md and voice-and-wingman.md kept the pre-#1060 keys -- W/A/S/D to fly, B and M for
# voice -- for two releases, on the public site, in the first table a new player reads. Both named
# no actions at all, so every name-based check was structurally blind to them.
USER_GUIDE_DIR = "docs/user-guide"

# First-column headers that mark a table as a key map.
KEY_COLUMN_HEADERS = {"key", "key / input"}

# Inputs that are real, documented, and not keyboard keys. Listed rather than silently skipped:
# an unrecognised token must be a typo'd key name, not a token the tokenizer has not met.
NON_KEYBOARD_INPUTS = {
    "left mouse",
    "right mouse",
    "middle mouse",
    "lmb",
    "rmb",
    "lmb drag",
    "rmb drag",
    "scroll wheel",
    "mouse wheel",
}

# Doc spellings that are not simply the enumerator name.
KEY_ALIASES = {
    "↑": "ArrowUp",
    "↓": "ArrowDown",
    "←": "ArrowLeft",
    "→": "ArrowRight",
    "arrow up": "ArrowUp",
    "arrow down": "ArrowDown",
    "arrow left": "ArrowLeft",
    "arrow right": "ArrowRight",
    "page up": "PageUp",
    "page down": "PageDown",
    "left shift": "LeftShift",
    "right shift": "RightShift",
    "left ctrl": "LeftCtrl",
    "right ctrl": "RightCtrl",
    "left alt": "LeftAlt",
    "right alt": "RightAlt",
    "-": "Minus",
    "=": "Equals",
    ",": "Comma",
    ".": "Period",
    "/": "Slash",
    ";": "Semicolon",
    "'": "Apostrophe",
    "[": "LeftBracket",
    "]": "RightBracket",
    "\\": "Backslash",
    "`": "Grave",
    "keypad +": "NumpadPlus",
    "keypad -": "NumpadMinus",
    "keypad −": "NumpadMinus",  # U+2212 MINUS SIGN, which is what the docs actually use
}

# Cells spell a family once and then abbreviate: "Arrow Up / Down", "Keypad 8 / 2 / 4 / 6".
# Without redistributing the prefix, "2" resolves to the digit key Num2 rather than Numpad2 --
# a silent wrong answer, which is worse than no check.
KEY_PREFIXES = ("arrow", "keypad", "numpad", "page")

# Single-character keys that are also the separator, so the cell must not be split.
UNSPLITTABLE_CELLS = {"/", ",", ".", ";", "'"}


def _strip_markdown(cell: str) -> str:
    """Unwrap code spans and bold. ``` `` ` `` ``` is the grave key and must survive."""
    cell = re.sub(r"``\s*(.+?)\s*``", r"\1", cell)
    cell = re.sub(r"`([^`]*)`", r"\1", cell)
    return cell.replace("**", "").strip()


def _resolve_key(token: str) -> str | None:
    """Doc spelling -> Key enumerator name, or None if it is not a keyboard key."""
    token = token.strip()
    if not token:
        return None
    lowered = token.lower()
    if lowered in KEY_ALIASES:
        return KEY_ALIASES[lowered]
    if re.fullmatch(r"f([1-9]|1[0-2])", lowered):
        return token.upper()
    if re.fullmatch(r"[a-z]", lowered):
        return lowered.upper()
    if re.fullmatch(r"[0-9]", lowered):
        return f"Num{lowered}"
    if match := re.fullmatch(r"(?:keypad|numpad)\s*([0-9])", lowered):
        return f"Numpad{match.group(1)}"
    for name in ("Space", "Enter", "Tab", "Backspace", "Delete", "Escape", "Home", "End", "Insert"):
        if lowered == name.lower():
            return name
    return None


def _tokenize_key_cell(cell: str) -> tuple[list[str], list[str]]:
    """Split a key cell into (resolved Key enumerators, unrecognised tokens)."""
    text = _strip_markdown(cell)
    if text in UNSPLITTABLE_CELLS:
        parts = [text]
    else:
        text = re.sub(r"\*?\bor\b\*?", "/", text)
        parts = [p.strip() for p in text.split("/") if p.strip()]

    prefix = ""
    if parts:
        first = parts[0].lower()
        for candidate in KEY_PREFIXES:
            if first.startswith(candidate):
                prefix = candidate
                break

    keys: list[str] = []
    unknown: list[str] = []
    for part in parts:
        # The prefixed reading wins: in "Keypad 8 / 2 / 4 / 6" a bare "2" resolves on its own to
        # the digit key Num2, so trying it first would quietly check the wrong key.
        resolved = None
        if prefix and not part.lower().startswith(prefix):
            resolved = _resolve_key(f"{prefix} {part}")
        if resolved is None:
            resolved = _resolve_key(part)
        if resolved is not None:
            keys.append(resolved)
        elif part.lower() not in NON_KEYBOARD_INPUTS:
            unknown.append(part)
    return keys, unknown


def _markdown_tables(doc: str) -> list[tuple[list[str], list[list[str]]]]:
    """Every pipe table in a document, as (header cells, data rows)."""

    def cells(line: str) -> list[str]:
        return [c.strip() for c in line.strip().strip("|").split("|")]

    tables: list[tuple[list[str], list[list[str]]]] = []
    lines = doc.splitlines()
    i = 0
    while i < len(lines):
        is_table_head = (
            lines[i].lstrip().startswith("|")
            and i + 1 < len(lines)
            and re.fullmatch(r"\|[\s:|-]+\|", lines[i + 1].strip())
        )
        if is_table_head:
            header = cells(lines[i])
            rows = []
            i += 2
            while i < len(lines) and lines[i].lstrip().startswith("|"):
                rows.append(cells(lines[i]))
                i += 1
            tables.append((header, rows))
            continue
        i += 1
    return tables


def _default_keyboard_bindings(src: str) -> dict[str, set[str]]:
    """InputAction -> the Key enumerators applyDefaults() binds it to."""
    bindings: dict[str, set[str]] = {}
    for action, key in re.findall(r"\{InputAction::(\w+),\s*kb\(Key::(\w+)\)\}", src):
        bindings.setdefault(action, set()).add(key)
    return bindings


def check_input_keys() -> CheckResult:
    """Every key the user guide prints must be what applyDefaults() actually binds.

    input-actions proves the key map NAMES every action. It cannot prove the key beside the name
    is right, and it reads one page. This check pairs the Key column against the Binding column
    and resolves both against the shipped defaults, across the whole user guide.

    A user-guide table with a Key column and real keys in it must carry a Binding column, so a
    new table cannot opt out of the check by omitting the one column that makes it checkable.
    """
    result = CheckResult("input-keys: applyDefaults() vs docs/user-guide/*.md key columns")
    defaults = _default_keyboard_bindings(read("engine/input/InputBindings.cpp"))
    actions = _input_action_names(read("engine/input/InputAction.h"))
    if not defaults:
        result.errors.append(
            "no {InputAction::X, kb(Key::Y)} defaults found in engine/input/InputBindings.cpp"
        )
        return result

    guide = sorted(p for p in (REPO_ROOT / USER_GUIDE_DIR).glob("*.md"))
    checked = 0
    for path in guide:
        rel = path.relative_to(REPO_ROOT).as_posix()
        for header, rows in _markdown_tables(path.read_text(encoding="utf-8")):
            if not header or header[0].lower() not in KEY_COLUMN_HEADERS:
                continue
            lowered = [h.lower() for h in header]
            if "binding" not in lowered:
                # Only a table that actually prints keyboard keys needs one: the mouse-only
                # camera tables and the user.toml settings tables share the "Key" header.
                if any(_tokenize_key_cell(row[0])[0] for row in rows if row):
                    shown = " | ".join(header)
                    result.errors.append(f"{rel}: a key table names real keys but has no `Binding` column: {shown}")
                continue
            bind_col = lowered.index("binding")

            for row in rows:
                if len(row) <= bind_col:
                    continue
                named = [n for n in re.findall(r"`(\w+)`", row[bind_col]) if n in actions]
                if not named:
                    continue  # "—", or a row whose binding cell names no action
                keys, unknown = _tokenize_key_cell(row[0])
                if unknown:
                    result.errors.append(f"{rel}: unrecognised key {unknown!r} in row: {row[0]}")
                    continue
                if not keys:
                    result.errors.append(f"{rel}: row binds {named} but names no key: {row[0]}")
                    continue
                if len(keys) == len(named):
                    pairs = list(zip(keys, named))
                elif len(named) == 1:
                    pairs = [(k, named[0]) for k in keys]
                else:
                    result.errors.append(
                        f"{rel}: cannot pair {len(keys)} key(s) with {len(named)} action(s): {row[0]}"
                    )
                    continue
                for key, action in pairs:
                    checked += 1
                    actual = defaults.get(action, set())
                    if key not in actual:
                        shown = ", ".join(sorted(actual)) or "nothing"
                        result.doc_only.add(f"{rel}: {action} documented on {key}, actually bound to {shown}")

    result.code_count = len(defaults)
    result.doc_count = checked
    floor_check(result, "documented key/action pairs", checked, 60)
    return result


# --------------------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------------------

CHECKS = {
    "config-keys": check_config_keys,
    "msg-ids": check_msg_ids,
    "lua-names": check_lua_names,
    "commands": check_commands,
    "tools-list": check_tools_list,
    "input-actions": check_input_actions,
    "input-keys": check_input_keys,
}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Check documentation against the code it describes.",
        epilog="Exit status is non-zero when any selected check finds drift.",
    )
    parser.add_argument(
        "check",
        choices=[*CHECKS, "all"],
        help="which check to run",
    )
    parser.add_argument("--verbose", action="store_true", help="also list waived names")
    args = parser.parse_args(argv)

    selected = list(CHECKS) if args.check == "all" else [args.check]
    failures = 0
    for name in selected:
        try:
            result = CHECKS[name]()
        except FileNotFoundError as exc:
            print(f"[FAIL] {name}\n    error: {exc}")
            failures += 1
            continue
        result.report(verbose=args.verbose)
        if not result.ok:
            failures += 1

    if failures:
        print(f"\n{failures} of {len(selected)} check(s) found drift.")
        return 1
    print(f"\nAll {len(selected)} check(s) clean.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
