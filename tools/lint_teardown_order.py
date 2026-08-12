# SPDX-FileCopyrightText: 2026 Fighters Legacy contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check fl-server's teardown contract (#1084).

`ServerRuntime::Impl` holds every owned server object, and C++ destroys members in REVERSE
declaration order. That makes the declaration order the teardown contract -- which is the point of
#1084, because the order it replaced was a comment, and teardown order in this file has already
produced two bugs (#1054, a double-free from voice capture being destroyed after `SDL_Quit()`, and
#1038, a stdin reader that deadlocked exit).

A C++ test cannot assert this: `Impl` is a pimpl, and "these two destructors ran in this order" is
not observable from outside without instrumenting the destructors themselves. So the gate reads the
source, the same way `docs_drift.py` and `lint_backend_seam.py` do. Each rule below is a hazard
somebody already hit, written as "A is declared before B", i.e. "B is destroyed before A".

Run: python3 tools/lint_teardown_order.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

SOURCE = Path(__file__).resolve().parent.parent / "server" / "fl-server" / "ServerRuntime.cpp"

# (earlier, later, why) -- `earlier` is declared first, so `later` is destroyed first.
RULES: list[tuple[str, str, str]] = [
    ("gameLoop", "stdinReader",
     "the stdin reader must stop before the sim thread: #1038's deadlock was exit waiting on a "
     "reader that was itself waiting on the C-stdio lock"),
    ("gameLoop", "replayRecorder",
     "the recorder's thread must stop before the sim thread stops feeding it"),
    ("broadcaster", "gameLoop",
     "the game loop steps the broadcaster every tick, so the loop's thread must be joined first"),
    ("adminRegistry", "adminShell",
     "the shell dispatches into the registry"),
    ("adminShell", "stdinChannel",
     "a channel's drain reads the shell's ring buffer"),
    ("stdinChannel", "rconServer",
     "the RCON I/O thread dispatches through its AdminChannel (#1079)"),
    ("adminRegistry", "enetChannel",
     "the channel's dispatcher calls into the command registry"),
    ("enetChannel", "broadcaster",
     "the broadcaster holds the ENet admin channel as a raw pointer from construction (#1082), so "
     "the channel has to outlive it"),
    ("httpChannel", "httpAdminServer",
     "the HTTP listener threads dispatch through their AdminChannel (#1079)"),
    ("entityManager", "broadcaster",
     "the broadcaster reads and mutates the entity pool"),
    ("entityRegistry", "entityManager",
     "the entity manager resolves types through the registry"),
    ("assets", "terrainStreamer",
     "the streamer reads chunks through the AssetManager"),
    ("p", "net",
     "the network backend lives in the Platform and must outlive everything that sends on it"),
]


def impl_member_order(text: str) -> list[str]:
    """Names of the OWNED members of `struct ServerRuntime::Impl`, in declaration order.

    Skips nested type definitions (their fields are not Impl members) and the two bookkeeping members
    above the object list, so what comes back is exactly the set the teardown rules talk about.
    """
    start = text.index("struct ServerRuntime::Impl {")
    end = text.index("\n};", start)
    body = text[start:end].split("\n")[1:]  # drop the `struct ... {` line itself

    names: list[str] = []
    depth = 0  # brace depth INSIDE Impl: > 0 means we are in a nested type or a function body
    for line in body:
        stripped = line.strip()
        opens, closes = line.count("{"), line.count("}")
        was_nested = depth > 0
        # A one-line `T x{};` or `T x{0};` opens and closes on the same line: not a nesting.
        if opens != closes:
            depth += opens - closes
        if was_nested or depth > 0:
            continue
        if stripped.startswith(("//", "explicit ", "[[nodiscard]]")) or not stripped:
            continue
        if "(" in stripped.split(";")[0]:
            continue  # a member function declaration
        # `std::unique_ptr<fl::WorldBroadcaster> m_broadcaster;` -> broadcaster
        # `char m_listeningMsg[192]{};`                          -> listeningMsg
        m = re.match(r"^[\w:<>,\s\*&]+?\bm_([a-zA-Z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:\{[^}]*\})?\s*;", stripped)
        if not m:
            continue
        name = m.group(1)
        if name in ("opts", "exitCode"):
            continue  # bookkeeping, not an owned object
        names.append(name)
    return names


def main() -> int:
    text = SOURCE.read_text()
    order = impl_member_order(text)
    if len(order) < 40:
        print(f"lint_teardown_order: only found {len(order)} members in ServerRuntime::Impl -- the "
              f"parser is broken, not the source. Refusing to pass vacuously.", file=sys.stderr)
        return 1

    failures = []
    for earlier, later, why in RULES:
        for name in (earlier, later):
            if name not in order:
                failures.append(f"  member '{name}' is not in ServerRuntime::Impl any more; the rule "
                                f"naming it needs updating, not deleting")
        if earlier in order and later in order and order.index(earlier) > order.index(later):
            failures.append(
                f"  '{later}' is declared before '{earlier}', so it is destroyed AFTER it.\n"
                f"    {why}")

    if failures:
        print("lint_teardown_order: fl-server's teardown contract is violated\n", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"lint_teardown_order: {len(RULES)} teardown rules hold across "
          f"{len(order)} ServerRuntime::Impl members")
    return 0


if __name__ == "__main__":
    sys.exit(main())
