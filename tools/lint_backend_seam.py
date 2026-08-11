#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject a product-library source that reaches third-party headers its library does not link.

`platform/` publishes a thin factory header per backend — `VkRendererFactory.h`, `OALAudioFactory.h`,
`ImGuiGui.h`, `NetworkFactory.h` — each stating in a comment that consumers "are never exposed to" the
backend's headers. The mechanism behind that promise is that every `platform-*` target links its
third-party dependency **PRIVATE**, so a consumer never inherits the include path.

The trap: a system package puts those headers on the compiler's DEFAULT include path anyway. So a
source that includes a concrete backend header compiles on a developer machine with the package
installed and fails on every CI platform, which builds the dependency through FetchContent instead.
That is exactly what happened to `game/fighters-legacy/Game.cpp` (#1067): it included the concrete
`openal/OALAudio.h` rather than `openal/OALAudioFactory.h`, and the break landed on
Build (ubuntu-latest), Build (macos-latest), Build (windows-latest) and Fuzz (fuzz_client_msg)
simultaneously, having passed a clean local build and a full 4657-test ctest run.

The rule is therefore not "no third-party headers" — some are legitimate. It is: **a product-library
TU may reach a third-party header only when the product library itself links that dependency.**
`game-client` lists `SDL3::SDL3` and `fl-server-lib` lists `httplib::httplib`, so those includes are
inherited on every platform and are fine; neither lists `OpenAL::OpenAL`, so `AL/al.h` was not. The
allowlist is DERIVED from `target_link_libraries` rather than written here, so it cannot drift from
what the build actually links.

**`main.cpp` is exempt, and that is the point.** Each product's `main.cpp` is its composition root —
the one place that names concrete backends and injects them (`ClientBackends`, `createNetwork`) — and
each executable links the backend targets the library deliberately does not. The exemption is what
makes the rule enforceable everywhere else.

Usage:  python3 tools/lint_backend_seam.py [repo-root]
Exit 0 when clean, 1 when a violation is found.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# Each product: its source directory and the static library built from it. The composition root is
# `main.cpp` in both, and it is exempt.
PRODUCTS = (("game/fighters-legacy", "game-client"), ("server/fl-server", "fl-server-lib"))
COMPOSITION_ROOT = "main.cpp"

# Third-party dependency target -> the header prefixes reaching it implies. A product library that
# links the target inherits its include path on every platform, so those includes are legitimate.
DEP_HEADERS = {
    "SDL3::SDL3": ("SDL3/",),
    "OpenAL::OpenAL": ("AL/",),
    "Vulkan::Vulkan": ("vulkan/", "vk_mem_alloc.h"),
    "httplib::httplib": ("httplib.h",),
    "CURL::libcurl": ("curl/",),
    "imgui": ("imgui",),
    "enet6": ("enet6/", "enet/"),
}

# Third-party headers worth checking at all. Whether reaching one is a violation depends on the
# product library's link line — see DEP_HEADERS and allowed_prefixes().
THIRD_PARTY = re.compile(
    r"#\s*include\s*<((?:AL|SDL3|vulkan|GL|curl|google|openssl|steam)/[\w./+-]+|imgui[\w.]*\.h|"
    r"vk_mem_alloc\.h|httplib\.h|enet6?/[\w./+-]+)>"
)

# An #include of a platform/ header, e.g. `#include "openal/OALAudio.h"`. The subdirectory name is
# required so this does not match engine/ headers, which are reached the same way.
PLATFORM_INCLUDE = re.compile(r'#\s*include\s*"([\w-]+/[\w.]+)"')

SOURCE_SUFFIXES = (".h", ".hpp", ".cpp", ".inl")


def platform_header_third_party(root: Path, rel: str, seen: set[str] | None = None) -> list[str]:
    """Third-party headers `platform/<rel>` reaches, following its own platform/ includes.

    Returns [] when `platform/<rel>` does not exist — the include named an engine/ header (both
    `engine/net/` and `platform/net/` exist, so resolving by existence is the only honest test).
    """
    seen = seen if seen is not None else set()
    if rel in seen:
        return []
    seen.add(rel)
    path = root / "platform" / rel
    if not path.is_file():
        return []
    body = path.read_text(encoding="utf-8", errors="replace")
    found = [m.group(1) for m in THIRD_PARTY.finditer(body)]
    for m in PLATFORM_INCLUDE.finditer(body):
        found += platform_header_third_party(root, m.group(1), seen)
    return found


def allowed_prefixes(root: Path, product: str, library: str) -> tuple[set[str], set[str]]:
    """(header prefixes this product library may reach, dependency targets it links).

    Read out of `target_link_libraries(<library> ...)` in the product's CMakeLists.txt, so the
    allowlist is whatever the build links and cannot drift from it.
    """
    cmake = (root / product / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
    # Strip comments BEFORE balancing parens: these link lists carry trailing comments holding issue
    # references like "(#41)", and a non-greedy match to the first ")" would stop inside one and read
    # a truncated dependency list as an empty one.
    cmake = re.sub(r"#[^\n]*", "", cmake)
    body = ""
    m = re.search(r"target_link_libraries\s*\(\s*" + re.escape(library) + r"(?![\w-])", cmake)
    if m:
        depth, i = 1, m.end()
        while i < len(cmake) and depth:
            depth += (cmake[i] == "(") - (cmake[i] == ")")
            i += 1
        body = cmake[m.end() : i - 1]
    linked = {dep for dep in DEP_HEADERS if re.search(r"(?<![\w:-])" + re.escape(dep) + r"(?![\w:-])", body)}
    prefixes = {p for dep in linked for p in DEP_HEADERS[dep]}
    return prefixes, linked


def check(root: Path) -> list[str]:
    violations: list[str] = []
    for product, library in PRODUCTS:
        directory = root / product
        if not directory.is_dir():
            continue
        allowed, linked = allowed_prefixes(root, product, library)
        for src in sorted(directory.iterdir()):
            if src.suffix not in SOURCE_SUFFIXES or src.name == COMPOSITION_ROOT:
                continue
            body = src.read_text(encoding="utf-8", errors="replace")
            for m in THIRD_PARTY.finditer(body):
                if not _permitted(m.group(1), allowed):
                    violations.append(
                        f"{product}/{src.name}: includes <{m.group(1)}>, but {library} does not link "
                        f"the dependency that provides it (links: {sorted(linked) or 'none'}). Construct "
                        f"the backend in {product}/{COMPOSITION_ROOT} and inject it."
                    )
            for m in PLATFORM_INCLUDE.finditer(body):
                rel = m.group(1)
                for reached in sorted(set(platform_header_third_party(root, rel))):
                    if not _permitted(reached, allowed):
                        violations.append(
                            f'{product}/{src.name}: includes "{rel}", which pulls <{reached}>, but '
                            f"{library} does not link the dependency that provides it. Use that "
                            f"backend's thin factory header instead, or construct it in "
                            f"{product}/{COMPOSITION_ROOT}."
                        )
                        break
    return violations


def _permitted(header: str, allowed: set[str]) -> bool:
    return any(header.startswith(prefix) for prefix in allowed)


def main(argv: list[str]) -> int:
    root = Path(argv[1]) if len(argv) > 1 else Path(__file__).resolve().parent.parent
    violations = check(root)
    if violations:
        print("Backend-seam violation(s) detected:", file=sys.stderr)
        for v in violations:
            print(f"  {v}", file=sys.stderr)
        print(
            "\nThese compile wherever a system package puts the backend headers on the default\n"
            'include path, and fail on every CI platform, which builds them via FetchContent.\n'
            'See "Product libraries" in docs/developer/architecture.md.',
            file=sys.stderr,
        )
        return 1
    print("lint_backend_seam: product libraries reach no backend third-party headers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
