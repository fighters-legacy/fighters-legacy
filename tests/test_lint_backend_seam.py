# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/lint_backend_seam.py (pure logic over a synthetic tree)."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import lint_backend_seam as lint  # noqa: E402


def make_tree(tmp_path, *, link_libraries, product_source, platform_headers=None):
    """A minimal repo shaped like the real one: one product dir + a platform/ tree."""
    game = tmp_path / "game" / "fighters-legacy"
    game.mkdir(parents=True)
    (game / "CMakeLists.txt").write_text(
        "add_library(game-client STATIC Thing.cpp)\n"
        f"target_link_libraries(game-client PUBLIC\n{link_libraries})\n"
    )
    (game / "Thing.cpp").write_text(product_source)
    (game / "main.cpp").write_text('#include <AL/al.h>\n#include "openal/OALAudio.h"\n')
    for rel, body in (platform_headers or {}).items():
        p = tmp_path / "platform" / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(body)
    return tmp_path


OAL_HEADERS = {"openal/OALAudio.h": "#include <AL/al.h>\n", "openal/OALAudioFactory.h": '#include "IAudio.h"\n'}


def test_the_1067_defect_is_rejected(tmp_path):
    """The real break: a concrete backend header whose dependency the library does not link."""
    root = make_tree(
        tmp_path,
        link_libraries="    platform-openal\n    platform-hal\n",
        product_source='#include "openal/OALAudio.h"\n',
        platform_headers=OAL_HEADERS,
    )
    violations = lint.check(root)
    assert len(violations) == 1
    assert "OALAudio.h" in violations[0] and "AL/al.h" in violations[0]


def test_the_factory_header_is_accepted(tmp_path):
    """The fix: the thin factory header reaches no third-party header at all."""
    root = make_tree(
        tmp_path,
        link_libraries="    platform-openal\n    platform-hal\n",
        product_source='#include "openal/OALAudioFactory.h"\n',
        platform_headers=OAL_HEADERS,
    )
    assert lint.check(root) == []


def test_a_linked_dependency_is_permitted(tmp_path):
    """SDL3 in game-client and httplib in fl-server-lib: linked, so inherited on every platform."""
    root = make_tree(
        tmp_path,
        link_libraries="    platform-sdl3\n    SDL3::SDL3\n",
        product_source="#include <SDL3/SDL.h>\n",
    )
    assert lint.check(root) == []


def test_the_same_include_is_rejected_when_the_dependency_is_dropped(tmp_path):
    """The allowlist is derived from the link line, so removing the dep flips the verdict."""
    root = make_tree(
        tmp_path,
        link_libraries="    platform-sdl3\n",  # platform-sdl3 links SDL3::SDL3 PRIVATE
        product_source="#include <SDL3/SDL.h>\n",
    )
    violations = lint.check(root)
    assert len(violations) == 1
    assert "SDL3/SDL.h" in violations[0]


def test_the_composition_root_is_exempt(tmp_path):
    """main.cpp names backends by design; the fixture's main.cpp violates both rules at once."""
    root = make_tree(
        tmp_path,
        link_libraries="    platform-hal\n",
        product_source="// nothing\n",
        platform_headers=OAL_HEADERS,
    )
    assert lint.check(root) == []


def test_a_comment_in_the_link_list_does_not_truncate_it(tmp_path):
    """A trailing "(#41)" comment closes a paren; a naive scan would read an empty dependency list."""
    root = make_tree(
        tmp_path,
        link_libraries="    engine-replay  # .flrep playback (#41); the reader only\n    SDL3::SDL3\n",
        product_source="#include <SDL3/SDL.h>\n",
    )
    assert lint.check(root) == []


def test_an_engine_header_sharing_a_platform_subdir_name_is_not_probed(tmp_path):
    """engine/net/ and platform/net/ collide by name; only a real platform/ file is followed."""
    root = make_tree(
        tmp_path,
        link_libraries="    engine-net\n",
        product_source='#include "net/GameProtocol.h"\n',  # engine/net/, absent from platform/
    )
    assert lint.check(root) == []


@pytest.mark.parametrize("header", ["vulkan/vulkan.h", "curl/curl.h", "httplib.h"])
def test_every_guarded_dependency_is_rejected_unlinked(tmp_path, header):
    root = make_tree(tmp_path, link_libraries="    platform-hal\n", product_source=f"#include <{header}>\n")
    assert len(lint.check(root)) == 1


def test_the_real_repository_is_clean():
    """The rule holds on the tree as committed — this is the check CI runs."""
    assert lint.check(Path(__file__).resolve().parent.parent) == []
