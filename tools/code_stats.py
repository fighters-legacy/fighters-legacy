#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Application statistics for a milestone gate: what this release actually is.

Two halves.

**Composition** — every tracked file sorted into a category (production code, tests, test
fixtures, documentation, configuration, build system, media). Text is measured in lines;
binary is measured in files and bytes, because a line count of a PNG is a lie.

**Surface** — the numbers that say what the software *does* rather than how much of it there
is: wire messages, server configuration keys, admin commands, Lua bindings, CLI tools, test
cases. "41 admin commands and 4,177 test cases" tells a reader something; "279,147 lines"
mostly tells them C++ is verbose.

Every file must classify. An unrecognised extension is a hard error rather than a silent
omission — the failure mode of a statistics tool is under-reporting that looks like a real
number, so a new file type has to be classified deliberately (`--allow-unclassified` exists
for local exploration, never for a release).

Usage:
    tools/code_stats.py                     # markdown to stdout
    tools/code_stats.py --format json
    tools/code_stats.py --format both --out-dir dist/
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# --------------------------------------------------------------------------------------
# Classification
# --------------------------------------------------------------------------------------

# extension -> (category, language). Categories are the report's top-level grouping.
EXT_MAP: dict[str, tuple[str, str]] = {
    ".cpp": ("code", "C++"),
    ".h": ("code", "C++"),
    ".hpp": ("code", "C++"),
    ".c": ("code", "C"),
    ".py": ("code", "Python"),
    ".sh": ("code", "Shell"),
    ".ps1": ("code", "PowerShell"),
    ".lua": ("code", "Lua"),
    ".vert": ("code", "GLSL"),
    ".frag": ("code", "GLSL"),
    ".comp": ("code", "GLSL"),
    ".cmake": ("build", "CMake"),
    ".md": ("docs", "Markdown"),
    ".yml": ("config", "YAML"),
    ".yaml": ("config", "YAML"),
    ".json": ("config", "JSON"),
    ".toml": ("config", "TOML"),
    ".conf": ("config", "Config"),
    ".supp": ("config", "Config"),
    ".in": ("build", "Template"),
    ".csv": ("data", "CSV"),
    ".txt": ("build", "CMake"),  # refined below: CMakeLists.txt vs a real text file
    # Binary — counted as files and bytes, never lines.
    ".bin": ("fixtures", "Fuzz corpus"),
    ".dict": ("fixtures", "Fuzz dictionary"),
    ".png": ("media", "Image"),
    ".jpg": ("media", "Image"),
    ".jpeg": ("media", "Image"),
    ".ktx2": ("media", "Texture"),
    ".glb": ("media", "Model"),
    ".gltf": ("media", "Model"),
    ".ogg": ("media", "Audio"),
    ".wav": ("media", "Audio"),
    ".ttf": ("media", "Font"),
    ".otf": ("media", "Font"),
    ".mp4": ("media", "Video"),
    ".COF": ("data", "Coefficient table"),
}

# Known by name: extensionless files, and dotfiles whose leading dot is not a suffix
# (`Path(".gitignore").suffix` is empty, so these can only match here).
NAME_MAP: dict[str, tuple[str, str]] = {
    ".clang-format": ("config", "Config"),
    ".clangd": ("config", "Config"),
    ".editorconfig": ("config", "Config"),
    ".gitignore": ("config", "Config"),
    ".gitattributes": ("config", "Config"),
    "CODEOWNERS": ("config", "Config"),
    "AUTHORS": ("docs", "Plain text"),
    "LICENSE": ("docs", "Plain text"),
    "NOTICE": ("docs", "Plain text"),
    "commit-msg": ("code", "Shell"),
    "pre-commit": ("code", "Shell"),
    "Containerfile": ("build", "Container"),
    "Vagrantfile": ("build", "Container"),
    "CMakeLists.txt": ("build", "CMake"),
}

# Measured by files and bytes rather than lines. Binary because a line count is meaningless,
# and `data` because a 134k-row reference dataset is not 134k lines of authored work -- counting
# it as "lines" would triple the headline and describe nothing anyone did.
BINARY_CATEGORIES = {"fixtures", "media", "data"}

CATEGORY_ORDER = ["code", "tests", "build", "config", "docs", "data", "fixtures", "media"]
CATEGORY_LABEL = {
    "code": "Production code",
    "tests": "Test code",
    "build": "Build system",
    "config": "Configuration",
    "docs": "Documentation",
    "data": "Data",
    "fixtures": "Test fixtures",
    "media": "Media & assets",
}


def classify(rel_path: str) -> tuple[str, str]:
    """Return (category, language) for a repo-relative path."""
    p = Path(rel_path)
    category, language = NAME_MAP.get(p.name, (None, None))
    if category is None:
        ext = p.suffix
        # `.txt` is CMakeLists.txt often enough to be mapped that way; a real text file is not.
        if ext == ".txt" and p.name != "CMakeLists.txt":
            category, language = "docs", "Plain text"
        else:
            category, language = EXT_MAP.get(ext, (None, None))
    if category is None:
        return ("", "")

    # Test code is its own category regardless of language: separating the test tree from the
    # shipped tree is the distinction a reader of a release actually wants.
    if category == "code" and is_test_path(rel_path):
        category = "tests"
    return (category, language)


def is_test_path(rel_path: str) -> bool:
    parts = rel_path.split("/")
    if parts[0] in ("tests", "fuzz"):
        return True
    name = parts[-1]
    return name.startswith("test_") or name.endswith(("_test.cpp", "_tests.cpp"))


# --------------------------------------------------------------------------------------
# Composition
# --------------------------------------------------------------------------------------


@dataclass
class Bucket:
    files: int = 0
    lines: int = 0
    bytes_: int = 0


@dataclass
class Composition:
    by_category: dict[str, Bucket] = field(default_factory=dict)
    by_language: dict[str, dict[str, Bucket]] = field(default_factory=dict)
    by_area: dict[str, Bucket] = field(default_factory=dict)
    unclassified: list[str] = field(default_factory=list)


AREA_ROOTS = {
    "engine", "game", "server", "platform", "tools", "tests", "fuzz",
    "docs", "cmake", "scripts", ".github",
}


def area_of(rel_path: str) -> str:
    top = rel_path.split("/")[0]
    return top if top in AREA_ROOTS else "(root)"


def tracked_files() -> list[str]:
    out = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "ls-files"], capture_output=True, text=True, check=True
    )
    return out.stdout.split("\n")


def measure(paths: list[str]) -> Composition:
    comp = Composition()
    for rel in paths:
        if not rel:
            continue
        full = REPO_ROOT / rel
        if not full.is_file():
            continue  # submodule entry, or a broken symlink
        category, language = classify(rel)
        if not category:
            comp.unclassified.append(rel)
            continue

        cat = comp.by_category.setdefault(category, Bucket())
        lang = comp.by_language.setdefault(category, {}).setdefault(language, Bucket())
        area = comp.by_area.setdefault(area_of(rel), Bucket())

        size = full.stat().st_size
        lines = 0
        if category == "data":
            with open(full, encoding="utf-8", errors="ignore") as fh:
                lines = sum(1 for _ in fh)  # rows, reported separately from the line total
        elif category not in BINARY_CATEGORIES:
            with open(full, encoding="utf-8", errors="ignore") as fh:
                lines = sum(1 for _ in fh)

        for b in (cat, lang, area):
            b.files += 1
            b.lines += lines
            b.bytes_ += size
    return comp


# --------------------------------------------------------------------------------------
# Surface
# --------------------------------------------------------------------------------------


def read(rel: str) -> str:
    return (REPO_ROOT / rel).read_text(encoding="utf-8")


def count_enum_rows(src: str, enum_name: str) -> int:
    m = re.search(rf"enum class {enum_name}\s*:\s*\w+\s*\{{(.*?)^\}};", src, re.S | re.M)
    return len(re.findall(r"^\s*\w+\s*=\s*0x[0-9A-Fa-f]+\s*,", m.group(1), re.M)) if m else 0


def grep_count(pattern: str, paths: list[str]) -> int:
    total = 0
    for rel in paths:
        p = REPO_ROOT / rel
        if p.is_file():
            total += len(re.findall(pattern, p.read_text(encoding="utf-8", errors="ignore"), re.M))
    return total


def glob_count(pattern: str) -> int:
    return len(list(REPO_ROOT.glob(pattern)))


def _load_docs_drift():
    """Import the drift checker so both tools count the same things the same way.

    These metrics already have one careful extractor each in `docs_drift.py` -- including the
    exclusions that took a round of debugging to get right (sub-table roots, array-of-table row
    fields, loop-registered command names, per-entry Lua globals). Re-implementing them here
    would guarantee the release notes and the drift gate eventually disagree about how many
    config keys the server has, which is the exact failure this repository keeps finding.
    """
    import importlib.util

    spec = importlib.util.spec_from_file_location("docs_drift", REPO_ROOT / "tools" / "docs_drift.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["docs_drift"] = module
    spec.loader.exec_module(module)
    return module


def surface() -> dict[str, int]:
    """Product-surface metrics, all derived statically from source."""
    dd = _load_docs_drift()
    protocol = read("engine/net/GameProtocol.h")
    commands = read("server/fl-server/ServerCommands.cpp")
    lua = read("engine/script/LuaController.cpp")
    tools_cmake = read("tools/CMakeLists.txt")

    test_cpp = [p for p in tracked_files() if p.startswith("tests/") and p.endswith(".cpp")]
    test_py = [p for p in tracked_files() if p.startswith("tests/") and p.endswith(".py")]

    config_result = dd.check_config_keys()

    return {
        "wire_messages": count_enum_rows(protocol, "MsgId"),
        "wire_extension_tags": count_enum_rows(protocol, "ExtTag"),
        "server_config_keys": config_result.code_count,
        "admin_commands": len(dd._registered_command_names(commands)),
        "lua_bindings": len(dd._lua_binding_names(lua)),
        "cli_tools": len(re.findall(r"add_executable\(\s*[A-Za-z0-9_-]+", tools_cmake)),
        "test_cases": grep_count(r"\bTEST_CASE\(", test_cpp),
        "test_assertions": grep_count(
            r"\b(?:CHECK|REQUIRE)(?:_FALSE|_THAT|_THROWS[A-Z_]*|_NOTHROW)?\(", test_cpp
        ),
        "python_tests": grep_count(r"^def test_", test_py),
        "fuzz_harnesses": glob_count("fuzz/fuzz_*.cpp"),
    }


SURFACE_LABEL = {
    "wire_messages": "Wire message types",
    "wire_extension_tags": "Wire extension tags",
    "server_config_keys": "Server configuration keys",
    "admin_commands": "Admin commands",
    "lua_bindings": "Lua API bindings",
    "cli_tools": "CLI tools",
    "test_cases": "Test cases",
    "test_assertions": "Test assertions",
    "python_tests": "Python test functions",
    "fuzz_harnesses": "Fuzz harnesses",
}


# --------------------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------------------


def human_bytes(n: int) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:,.0f} {unit}" if unit == "B" else f"{n / 1:,.1f} {unit}".replace(".0 ", " ")
        n /= 1024.0
    return f"{n} B"


def render_markdown(comp: Composition, surf: dict[str, int], version: str | None) -> str:
    out: list[str] = []
    title = f"Application statistics — {version}" if version else "Application statistics"
    out.append(f"### {title}\n")

    text_lines = sum(b.lines for c, b in comp.by_category.items() if c not in BINARY_CATEGORIES)
    text_files = sum(b.files for c, b in comp.by_category.items() if c not in BINARY_CATEGORIES)
    out.append(f"**{text_lines:,} lines** across **{text_files:,} text files**, "
               f"plus {sum(comp.by_category[c].files for c in BINARY_CATEGORIES if c in comp.by_category):,} "
               "binary files.\n")

    out.append("| Category | Files | Lines | Size |")
    out.append("|---|---:|---:|---:|")
    for cat in CATEGORY_ORDER:
        b = comp.by_category.get(cat)
        if not b:
            continue
        lines = "—" if cat in BINARY_CATEGORIES else f"{b.lines:,}"
        out.append(f"| {CATEGORY_LABEL[cat]} | {b.files:,} | {lines} | {human_bytes(b.bytes_)} |")
    out.append("")

    for cat in CATEGORY_ORDER:
        langs = comp.by_language.get(cat)
        if not langs or len(langs) < 2:
            continue
        out.append(f"**{CATEGORY_LABEL[cat]}** by language:\n")
        out.append("| Language | Files | Lines |")
        out.append("|---|---:|---:|")
        for lang, b in sorted(langs.items(), key=lambda kv: -kv[1].lines):
            lines = "—" if cat in BINARY_CATEGORIES else f"{b.lines:,}"
            out.append(f"| {lang} | {b.files:,} | {lines} |")
        out.append("")

    out.append("**Surface**\n")
    out.append("| Metric | Count |")
    out.append("|---|---:|")
    for key, label in SURFACE_LABEL.items():
        if key in surf:
            out.append(f"| {label} | {surf[key]:,} |")
    out.append("")

    if "media" not in comp.by_category:
        out.append("_No media or content assets: the engine is content-agnostic and ships none. "
                   "Aircraft, terrain, audio and missions live in content packs._\n")
    return "\n".join(out)


def to_dict(comp: Composition, surf: dict[str, int], version: str | None) -> dict:
    return {
        "version": version,
        "composition": {
            "categories": {
                cat: {
                    "files": b.files,
                    "lines": None if cat in BINARY_CATEGORIES else b.lines,
                    "bytes": b.bytes_,
                    "languages": {
                        lang: {
                            "files": lb.files,
                            "lines": None if cat in BINARY_CATEGORIES else lb.lines,
                            "bytes": lb.bytes_,
                        }
                        for lang, lb in sorted(comp.by_language.get(cat, {}).items())
                    },
                }
                for cat, b in ((c, comp.by_category[c]) for c in CATEGORY_ORDER if c in comp.by_category)
            },
            "areas": {
                a: {"files": b.files, "lines": b.lines, "bytes": b.bytes_}
                for a, b in sorted(comp.by_area.items())
            },
        },
        "surface": surf,
    }


# --------------------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Application statistics for a release.")
    ap.add_argument("--format", choices=["md", "json", "both"], default="md")
    ap.add_argument("--out-dir", type=Path, help="write stats.md / stats.json here instead of stdout")
    ap.add_argument("--version", help="version label for the report heading")
    ap.add_argument(
        "--allow-unclassified",
        action="store_true",
        help="report unclassified files instead of failing (local exploration only)",
    )
    args = ap.parse_args(argv)

    comp = measure(tracked_files())
    if comp.unclassified and not args.allow_unclassified:
        print(
            f"error: {len(comp.unclassified)} tracked file(s) have no category, so they would be "
            "silently missing from the report. Classify them in EXT_MAP/NAME_MAP:",
            file=sys.stderr,
        )
        for rel in sorted(comp.unclassified)[:25]:
            print(f"    {rel}", file=sys.stderr)
        return 1

    surf = surface()
    md = render_markdown(comp, surf, args.version)
    data = to_dict(comp, surf, args.version)

    if args.out_dir:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        if args.format in ("md", "both"):
            (args.out_dir / "stats.md").write_text(md, encoding="utf-8")
        if args.format in ("json", "both"):
            (args.out_dir / "stats.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {args.format} to {args.out_dir}")
    else:
        if args.format == "json":
            print(json.dumps(data, indent=2))
        elif args.format == "both":
            print(md)
            print(json.dumps(data, indent=2))
        else:
            print(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
