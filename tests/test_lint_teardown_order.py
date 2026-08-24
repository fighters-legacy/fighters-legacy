# SPDX-FileCopyrightText: 2026 Fighters Legacy contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/lint_teardown_order.py (#1084).

The gate reads source order to check a contract no C++ test can observe, so the parser is the thing
that can silently stop checking. These pin that it finds members, that it fails on a real inversion,
and that it refuses to pass vacuously when the parser stops matching.
"""
from pathlib import Path

import pytest

from conftest import load_tool

lto = load_tool("lint_teardown_order", "tools", "lint_teardown_order.py")

IMPL = """
struct ServerRuntime::Impl {
    struct ChurnState {
        double spawnAccum{0.0};
    };

    explicit Impl(ServerRuntime::Options& o) : opts(o) {}

    ServerRuntime::Options& m_opts;
    int m_exitCode{0};

    Platform m_p;
    ILogger* m_log{nullptr};
    INetwork* m_net{nullptr};
    std::unique_ptr<fl::WorldBroadcaster> m_broadcaster;
    char m_listeningMsg[192]{};
    std::unique_ptr<GameLoop> m_gameLoop;
    fl::StdinCommandReader m_stdinReader;
};
"""


def test_member_order_reads_declaration_order():
    order = lto.impl_member_order(IMPL)
    assert order == ["p", "log", "net", "broadcaster", "listeningMsg", "gameLoop", "stdinReader"]


def test_member_order_strips_the_member_prefix():
    """`m_broadcaster` is the member; `broadcaster` is what the rules name."""
    assert "broadcaster" in lto.impl_member_order(IMPL)
    assert "m_broadcaster" not in lto.impl_member_order(IMPL)


def test_member_order_skips_the_nested_struct_and_the_options_reference():
    order = lto.impl_member_order(IMPL)
    assert "spawnAccum" not in order  # a ChurnState field, not an Impl member
    assert "opts" not in order        # a reference to the caller's options, not an owned object


def test_the_real_source_satisfies_every_rule():
    assert lto.main() == 0


def test_a_reordered_source_fails(monkeypatch, tmp_path, capsys):
    """The inversion #1038 was: the stdin reader outliving the sim thread."""
    text = lto.SOURCE.read_text()
    moved = text.replace("    fl::StdinCommandReader m_stdinReader;\n", "", 1)
    anchor = "    std::unique_ptr<GameLoop> m_gameLoop;"
    moved = moved.replace(anchor, "    fl::StdinCommandReader m_stdinReader;\n" + anchor, 1)
    broken = tmp_path / "ServerRuntime.cpp"
    broken.write_text(moved)
    monkeypatch.setattr(lto, "SOURCE", broken)
    assert lto.main() == 1
    assert "stdinReader" in capsys.readouterr().err


def test_a_parser_that_stops_matching_fails_rather_than_passing(monkeypatch, tmp_path):
    """A gate that silently matches nothing reports success for the wrong reason."""
    stub = tmp_path / "ServerRuntime.cpp"
    stub.write_text("struct ServerRuntime::Impl {\n    int m_exitCode{0};\n};\n")
    monkeypatch.setattr(lto, "SOURCE", stub)
    assert lto.main() == 1
