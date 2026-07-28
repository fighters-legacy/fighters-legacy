# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/code_stats.py.

The classification rules are the part worth pinning: a file landing in the wrong category, or
in no category at all, produces a number that looks right and is not. The report goes into a
GitHub release, where nobody re-derives it.
"""

import importlib.util
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location("code_stats", REPO_ROOT / "tools" / "code_stats.py")
cs = importlib.util.module_from_spec(_spec)
sys.modules["code_stats"] = cs
_spec.loader.exec_module(cs)


# ---- classification ----------------------------------------------------------------------------


@pytest.mark.parametrize(
    "path,category,language",
    [
        ("engine/net/WorldBroadcaster.cpp", "code", "C++"),
        ("engine/net/GameProtocol.h", "code", "C++"),
        ("tools/docs_drift.py", "code", "Python"),
        ("scripts/cut-release.sh", "code", "Shell"),
        ("platform/vulkan/shaders/mesh.frag", "code", "GLSL"),
        ("cmake/layering.cmake", "build", "CMake"),
        ("CMakeLists.txt", "build", "CMake"),
        ("docs/index.md", "docs", "Markdown"),
        (".github/workflows/ci.yml", "config", "YAML"),
        ("scale-gate.json", "config", "JSON"),
        ("data/airports.csv", "data", "CSV"),
        ("data/WMM.COF", "data", "Coefficient table"),
        ("fuzz/corpus/fuzz_mcp/seed-01.bin", "fixtures", "Fuzz corpus"),
    ],
)
def test_classify_known_types(path, category, language):
    assert cs.classify(path) == (category, language)


def test_test_code_is_its_own_category_not_a_language():
    """A test .cpp must not inflate the production C++ count."""
    assert cs.classify("tests/test_entity.cpp") == ("tests", "C++")
    assert cs.classify("fuzz/fuzz_mcp.cpp") == ("tests", "C++")
    assert cs.classify("tests/test_docs_drift.py") == ("tests", "Python")


def test_dotfiles_classify_by_name_not_suffix():
    """`Path('.gitignore').suffix` is empty, so these only match through NAME_MAP."""
    for name in (".gitignore", ".clang-format", ".editorconfig", ".clangd", ".gitattributes"):
        assert cs.classify(name)[0] == "config", name


def test_plain_txt_is_not_mistaken_for_cmake():
    assert cs.classify("CMakeLists.txt") == ("build", "CMake")
    assert cs.classify("tools/notes.txt") == ("docs", "Plain text")


def test_unknown_extension_is_unclassified_rather_than_guessed():
    assert cs.classify("weird/thing.qqq") == ("", "")


def test_is_test_path():
    assert cs.is_test_path("tests/test_x.cpp")
    assert cs.is_test_path("fuzz/fuzz_x.cpp")
    assert cs.is_test_path("engine/foo_test.cpp")
    assert not cs.is_test_path("engine/net/WorldBroadcaster.cpp")
    assert not cs.is_test_path("tools/latest.cpp")


# ---- measurement -------------------------------------------------------------------------------


def test_binary_categories_are_not_line_counted():
    """A line count of a PNG or a fuzz seed is noise; those report files and bytes."""
    assert "fixtures" in cs.BINARY_CATEGORIES
    assert "media" in cs.BINARY_CATEGORIES
    # Bulk reference data is excluded for a different reason: 134k CSV rows are not authored work.
    assert "data" in cs.BINARY_CATEGORIES
    assert "code" not in cs.BINARY_CATEGORIES
    assert "docs" not in cs.BINARY_CATEGORIES


def test_every_category_has_a_label_and_an_order_slot():
    for cat in cs.CATEGORY_LABEL:
        assert cat in cs.CATEGORY_ORDER
    for cat in cs.CATEGORY_ORDER:
        assert cat in cs.CATEGORY_LABEL


def test_every_mapped_category_is_known():
    known = set(cs.CATEGORY_ORDER)
    for cat, _lang in list(cs.EXT_MAP.values()) + list(cs.NAME_MAP.values()):
        assert cat in known, cat


# ---- the real tree -----------------------------------------------------------------------------


def test_no_tracked_file_is_unclassified():
    """The guard that matters: an unmapped file type silently shrinks every number."""
    comp = cs.measure(cs.tracked_files())
    assert comp.unclassified == [], (
        f"{len(comp.unclassified)} file(s) need a category in EXT_MAP/NAME_MAP: "
        f"{sorted(comp.unclassified)[:10]}"
    )


def test_measure_produces_plausible_totals():
    comp = cs.measure(cs.tracked_files())
    assert comp.by_category["code"].lines > 10_000
    assert comp.by_category["tests"].lines > 1_000
    assert comp.by_category["docs"].lines > 1_000
    # Fuzz corpus is binary: files counted, lines not.
    assert comp.by_category["fixtures"].files > 0
    assert comp.by_category["fixtures"].lines == 0


def test_surface_agrees_with_the_drift_checker():
    """Two tools reporting different counts for one thing is the bug this repo keeps finding."""
    dd = cs._load_docs_drift()
    surf = cs.surface()
    assert surf["server_config_keys"] == dd.check_config_keys().code_count
    assert surf["lua_bindings"] == len(
        dd._lua_binding_names((REPO_ROOT / "engine/script/LuaController.cpp").read_text())
    )


def test_surface_metrics_are_all_non_zero():
    """A zero here means an extractor stopped matching, not that the feature vanished."""
    surf = cs.surface()
    for key, value in surf.items():
        assert value > 0, f"{key} extracted 0 — the pattern has probably broken"


def test_render_markdown_and_json_round_trip():
    import json

    comp = cs.measure(cs.tracked_files())
    surf = cs.surface()
    md = cs.render_markdown(comp, surf, "v9.9.9")
    assert "v9.9.9" in md
    assert "Production code" in md and "Test code" in md
    data = json.loads(json.dumps(cs.to_dict(comp, surf, "v9.9.9")))
    assert data["version"] == "v9.9.9"
    assert data["composition"]["categories"]["code"]["lines"] > 0
    # Binary categories report null lines rather than a misleading zero.
    assert data["composition"]["categories"]["fixtures"]["lines"] is None
