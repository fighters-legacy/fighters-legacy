# SPDX-FileCopyrightText: 2026 Fighters Legacy contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/lint_teardown_order.py (#1084).

The gate reads source order to check a contract no C++ test can observe, so the parser is the thing
that can silently stop checking. These pin that it finds members, that it fails on a real inversion,
and that it refuses to pass vacuously when the parser stops matching.
"""
import importlib.util
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("lint_teardown_order", ROOT / "tools" / "lint_teardown_order.py")
lto = importlib.util.module_from_spec(spec)
sys.modules["lint_teardown_order"] = lto
spec.loader.exec_module(lto)

IMPL = """
struct ServerRuntime::Impl {
    struct ChurnState {
        double spawnAccum{0.0};
    };

    explicit Impl(ServerRuntime::Options& o) : opts(o) {}

    ServerRuntime::Options& opts;
    int exitCode{0};

    Platform p;
    ILogger* log{nullptr};
    INetwork* net{nullptr};
    std::unique_ptr<fl::WorldBroadcaster> p_broadcaster;
    char listeningMsg[192]{};
    std::unique_ptr<GameLoop> p_gameLoop;
    fl::StdinCommandReader stdinReader;
};
"""


def test_member_order_reads_declaration_order():
    order = lto.impl_member_order(IMPL)
    assert order == ["p", "log", "net", "broadcaster", "listeningMsg", "gameLoop", "stdinReader"]


def test_member_order_unwraps_the_unique_ptr_prefix():
    """`p_broadcaster` is the handle; `broadcaster` is what the rules name."""
    assert "broadcaster" in lto.impl_member_order(IMPL)
    assert "p_broadcaster" not in lto.impl_member_order(IMPL)


def test_member_order_skips_the_nested_struct_and_the_options_reference():
    order = lto.impl_member_order(IMPL)
    assert "spawnAccum" not in order  # a ChurnState field, not an Impl member
    assert "opts" not in order        # a reference to the caller's options, not an owned object


def test_the_real_source_satisfies_every_rule():
    assert lto.main() == 0


def test_a_reordered_source_fails(monkeypatch, tmp_path, capsys):
    """The inversion #1038 was: the stdin reader outliving the sim thread."""
    text = lto.SOURCE.read_text()
    moved = text.replace("    fl::StdinCommandReader stdinReader;\n", "", 1)
    anchor = "    std::unique_ptr<GameLoop> p_gameLoop;"
    moved = moved.replace(anchor, "    fl::StdinCommandReader stdinReader;\n" + anchor, 1)
    broken = tmp_path / "ServerRuntime.cpp"
    broken.write_text(moved)
    monkeypatch.setattr(lto, "SOURCE", broken)
    assert lto.main() == 1
    assert "stdinReader" in capsys.readouterr().err


def test_a_parser_that_stops_matching_fails_rather_than_passing(monkeypatch, tmp_path):
    """A gate that silently matches nothing reports success for the wrong reason."""
    stub = tmp_path / "ServerRuntime.cpp"
    stub.write_text("struct ServerRuntime::Impl {\n    int exitCode{0};\n};\n")
    monkeypatch.setattr(lto, "SOURCE", stub)
    assert lto.main() == 1
