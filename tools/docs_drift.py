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
    return result


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


def check_msg_ids() -> CheckResult:
    result = CheckResult("msg-ids: GameProtocol.h vs docs/developer/network-protocol.md")
    src = read("engine/net/GameProtocol.h")
    doc = read("docs/developer/network-protocol.md")

    code: set[str] = set()
    for enum_name in ("MsgId", "ExtTag"):
        for name, value in _enum_entries(src, enum_name).items():
            code.add(f"{name}={value}")

    # Doc tables spell both as | `Name` | `0xNN` | ...
    documented = {
        f"{name}={value.lower()}"
        for name, value in re.findall(r"^\|\s*`(\w+)`\s*\|\s*`(0x[0-9A-Fa-f]+)`\s*\|", doc, re.M)
    }

    result.code_count = len(code)
    result.doc_count = len(documented)
    if not floor_check(result, "GameProtocol.h enum rows", len(code), 30):
        return result
    if not floor_check(result, "network-protocol.md table rows", len(documented), 30):
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
# Driver
# --------------------------------------------------------------------------------------

CHECKS = {
    "config-keys": check_config_keys,
    "msg-ids": check_msg_ids,
    "lua-names": check_lua_names,
    "commands": check_commands,
    "tools-list": check_tools_list,
    "input-actions": check_input_actions,
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
