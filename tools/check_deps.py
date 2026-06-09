# SPDX-FileCopyrightText: 2026 John McKenzie
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Check FetchContent dependency versions against the latest GitHub releases.
Reads cmake/dependencies.cmake, queries GitHub for each dep, and prints a
colour-coded status table.

Usage:
    python3 tools/check_deps.py [--no-color]

Requires the GitHub CLI (gh) to be installed and authenticated.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

# ---------------------------------------------------------------------------
# Colour helpers — respect NO_COLOR env var and --no-color flag
# ---------------------------------------------------------------------------
_USE_COLOUR = (
    sys.stdout.isatty()
    and "--no-color" not in sys.argv
    and not os.environ.get("NO_COLOR")
)


def _c(code: str, text: str) -> str:
    return f"{code}{text}\033[0m" if _USE_COLOUR else text


def red(t: str)    -> str: return _c("\033[1;31m", t)
def yellow(t: str) -> str: return _c("\033[1;33m", t)
def green(t: str)  -> str: return _c("\033[1;32m", t)
def bold(t: str)   -> str: return _c("\033[1m",    t)
def dim(t: str)    -> str: return _c("\033[2m",    t)


# ---------------------------------------------------------------------------
# Human-readable display names for FetchContent targets
# ---------------------------------------------------------------------------
DISPLAY_NAMES: dict[str, str] = {
    "SDL3":                  "SDL3",
    "openal-soft":           "OpenAL Soft",
    "enet6":                 "enet6",
    "Catch2":                "Catch2",
    "tinygltf":              "tinygltf",
    "yaml-cpp":              "yaml-cpp",
    "VulkanMemoryAllocator": "VulkanMemoryAllocator",
    "ktx":                   "KTX-Software",
    "glm":                   "GLM",
    "stb":                   "stb",
    "lua_src":               "Lua",
    "tomlplusplus":          "tomlplusplus",
}

DEPS_FILE = Path(__file__).parent.parent / "cmake" / "dependencies.cmake"


# ---------------------------------------------------------------------------
# Parse FetchContent_Declare entries from cmake/dependencies.cmake
# ---------------------------------------------------------------------------
def parse_deps(path: Path) -> list[dict]:
    text = path.read_text()
    deps = []
    for m in re.finditer(
        r'FetchContent_Declare\s*\(\s*(\S+)(.*?)\)',
        text, re.DOTALL,
    ):
        name = m.group(1)
        body = m.group(2)
        repo_m = re.search(r'GIT_REPOSITORY\s+(\S+)', body)
        tag_m  = re.search(r'GIT_TAG\s+(\S+)',        body)
        if not repo_m or not tag_m:
            continue
        repo_url = repo_m.group(1)
        if repo_url.endswith('.git'):
            repo_url = repo_url[:-4]
        gh_m = re.search(r'github\.com/([^/]+/[^/]+)$', repo_url)
        if not gh_m:
            continue
        deps.append({
            "name":    name,
            "display": DISPLAY_NAMES.get(name, name),
            "repo":    gh_m.group(1),
            "pinned":  tag_m.group(1),
        })
    return deps


# ---------------------------------------------------------------------------
# GitHub API helpers
# ---------------------------------------------------------------------------
def _gh_api(endpoint: str, jq: str) -> str | None:
    try:
        r = subprocess.run(
            ["gh", "api", endpoint, "--jq", jq],
            capture_output=True, text=True, timeout=15,
        )
        out = r.stdout.strip()
        return out if r.returncode == 0 and out not in ("", "null") else None
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return None


def _is_commit_hash(tag: str) -> bool:
    return bool(re.fullmatch(r'[0-9a-f]{40}', tag, re.IGNORECASE))


def fetch_latest(dep: dict) -> dict:
    repo   = dep["repo"]
    pinned = dep["pinned"]

    if _is_commit_hash(pinned):
        sha = _gh_api(f"repos/{repo}/commits/HEAD", ".sha")
        latest = sha[:40] if sha else None
    else:
        # Prefer releases/latest — skips pre-releases and draft releases
        latest = _gh_api(f"repos/{repo}/releases/latest", ".tag_name")
        if not latest:
            # Fall back to the most recent tag
            latest = _gh_api(f"repos/{repo}/tags?per_page=1", ".[0].name")

    if latest is None:
        return {**dep, "latest": "error", "status": "error"}
    return {**dep, "latest": latest, "status": _classify(pinned, latest)}


# ---------------------------------------------------------------------------
# Version comparison
# ---------------------------------------------------------------------------
def _parse_version(tag: str) -> tuple[int, ...] | None:
    """Extract the first dotted integer sequence from a tag string."""
    m = re.search(r'(\d+)(?:\.(\d+))?(?:\.(\d+))?', tag)
    if not m:
        return None
    return tuple(int(x or 0) for x in m.groups())


def _classify(pinned: str, latest: str) -> str:
    if _is_commit_hash(pinned):
        return "current" if pinned[:40].lower() == latest[:40].lower() else "commit"
    if pinned == latest:
        return "current"
    pv = _parse_version(pinned)
    lv = _parse_version(latest)
    if pv is None or lv is None:
        return "unknown"
    if pv == lv:
        return "current"
    if lv > pv:
        if lv[0] > pv[0]: return "major"
        if lv[1] > pv[1]: return "minor"
        return "patch"
    return "current"  # pinned is ahead of latest release (pre-release, etc.)


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------
_STATUS_CONFIG: dict[str, tuple[str, object]] = {
    "current": ("✓  current",      green),
    "patch":   ("↑  patch",        yellow),
    "minor":   ("↑  minor",        yellow),
    "major":   ("↑  MAJOR",        red),
    "commit":  ("↑  new commits",  yellow),
    "unknown": ("?  unknown",      yellow),
    "error":   ("!  error",        red),
}


def _short_tag(tag: str) -> str:
    """Shorten 40-char commit hashes to 8 chars + ellipsis."""
    if _is_commit_hash(tag):
        return tag[:8] + "…"
    return tag


def print_table(results: list[dict]) -> None:
    disp = [(_short_tag(r["pinned"]), _short_tag(r["latest"])) for r in results]

    col_dep    = max(len(r["display"]) for r in results)        + 2
    col_pinned = max(len(p) for p, _ in disp)                   + 2
    col_latest = max(len(l) for _, l in disp)                   + 2

    bar = "─" * (col_dep + col_pinned + col_latest + 16)

    hdr_dep    = f"{'Dependency':<{col_dep}}"
    hdr_pinned = f"{'Pinned':<{col_pinned}}"
    hdr_latest = f"{'Latest':<{col_latest}}"

    print()
    print(f"  {bold('Dependency Check — fighters-legacy')}")
    print(f"  {bar}")
    print(f"  {bold(hdr_dep)}{bold(hdr_pinned)}{bold(hdr_latest)}{bold('Status')}")
    print(f"  {bar}")

    n_current = n_updates = n_errors = 0

    for r, (disp_pinned, disp_latest) in zip(results, disp):
        label, colour_fn = _STATUS_CONFIG.get(r["status"], ("? unknown", yellow))
        dep_col    = f"{r['display']:<{col_dep}}"
        pinned_col = f"{disp_pinned:<{col_pinned}}"
        latest_col = f"{disp_latest:<{col_latest}}"

        if r["status"] == "current":
            n_current += 1
            row = f"  {dep_col}{dim(pinned_col)}{dim(latest_col)}{green(label)}"
        elif r["status"] in ("major", "error"):
            n_errors += (r["status"] == "error")
            n_updates += (r["status"] == "major")
            row = f"  {dep_col}{pinned_col}{red(latest_col)}{red(label)}"
        else:
            n_updates += 1
            row = f"  {dep_col}{pinned_col}{yellow(latest_col)}{yellow(label)}"

        print(row)

    print(f"  {bar}")

    parts = [green(f"{n_current} up to date")]
    if n_updates:
        parts.append(yellow(f"{n_updates} update{'s' if n_updates != 1 else ''} available"))
    if n_errors:
        parts.append(red(f"{n_errors} error{'s' if n_errors != 1 else ''}"))
    print(f"  {', '.join(parts)}")
    print()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main() -> int:
    if "--help" in sys.argv or "-h" in sys.argv:
        print(__doc__)
        return 0

    try:
        subprocess.run(["gh", "--version"], capture_output=True, check=True)
    except (FileNotFoundError, subprocess.CalledProcessError):
        print("error: 'gh' CLI not found or not working — see https://cli.github.com/", file=sys.stderr)
        return 1

    deps = parse_deps(DEPS_FILE)
    if not deps:
        print(f"error: no FetchContent_Declare entries found in {DEPS_FILE}", file=sys.stderr)
        return 1

    print(f"  Checking {len(deps)} dependencies", end="", flush=True)

    results: list[dict | None] = [None] * len(deps)
    with ThreadPoolExecutor(max_workers=8) as pool:
        futures = {pool.submit(fetch_latest, dep): i for i, dep in enumerate(deps)}
        for future in as_completed(futures):
            results[futures[future]] = future.result()
            print(".", end="", flush=True)

    print()
    print_table(results)  # type: ignore[arg-type]
    return 0


if __name__ == "__main__":
    sys.exit(main())
