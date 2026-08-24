# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/fetch_base_terrain.py.

The part worth pinning is the REFUSAL set. This tool decides whether a release ships with the
bundled base terrain it was supposed to carry, and every interesting failure looks like success
from the outside: a tree that extracts cleanly but has no sentinel tile mounts nothing, and a tree
without ATTRIBUTION.md ships GEBCO-derived bytes with no notice. Neither raises anything on its
own. So the tests here are mostly "does it still say no", plus one end-to-end pass over a synthetic
bundle served from a local file so the checksum path is exercised for real.
"""

import hashlib
import json
import os
import sys
import zipfile
from pathlib import Path

import pytest

from conftest import load_tool

REPO_ROOT = Path(__file__).resolve().parent.parent
fbt = load_tool("fetch_base_terrain", "tools", "fetch_base_terrain.py")


def _lock(tmp_path: Path, **overrides) -> Path:
    data = {
        "repo": "fighters-legacy/fl-base-terrain",
        "tag": "",
        "asset": "",
        "sha256": "",
        "size_bytes": 0,
        "terrain_id": "world",
    }
    data.update(overrides)
    p = tmp_path / "lock.json"
    p.write_text(json.dumps(data), encoding="utf-8")
    return p


def _run(monkeypatch, *argv) -> int:
    monkeypatch.setattr(sys, "argv", ["fetch_base_terrain.py", *argv])
    return fbt.main()


def _bundle(tmp_path: Path, *, sentinel=True, faces=6, attribution=True, pad=1 << 20) -> Path:
    """A synthetic base-terrain archive with the tree at its root, as the runbook specifies."""
    src = tmp_path / "src"
    for f in range(faces):
        level = "l0" if sentinel else "l1"
        d = src / "terrain" / "world" / f"f{f}" / level
        d.mkdir(parents=True, exist_ok=True)
        (d / "tile_0_0.png").write_bytes(os.urandom(pad // max(faces, 1) + 1))
    if attribution:
        (src / "ATTRIBUTION.md").write_text("# attribution\nGEBCO_2024 Grid.\n", encoding="utf-8")
    archive = tmp_path / "bundle.zip"
    with zipfile.ZipFile(archive, "w") as zf:
        for root, _, files in os.walk(src):
            for fn in files:
                p = Path(root) / fn
                zf.write(p, p.relative_to(src))
    return archive


def _pin_local(tmp_path: Path, monkeypatch, archive: Path, **overrides) -> Path:
    """Pin `archive` by its real digest and serve it from the filesystem instead of GitHub."""
    raw = archive.read_bytes()
    lock = _lock(
        tmp_path,
        tag="gebco2024-r1",
        asset=archive.name,
        sha256=hashlib.sha256(raw).hexdigest(),
        size_bytes=len(raw),
        **overrides,
    )
    monkeypatch.setattr(fbt, "_asset_url", lambda _lockdata: archive.as_uri())
    return lock


# ---- the unpinned state is declared, not silent ------------------------------------------------


def test_unpinned_succeeds_and_says_so(tmp_path, monkeypatch, capsys):
    lock = _lock(tmp_path)
    assert _run(monkeypatch, "--lock", str(lock), "--output-dir", str(tmp_path / "bt")) == 0
    out = capsys.readouterr().out
    assert "no base-terrain bundle is pinned" in out
    assert "procedural horizon" in out


def test_unpinned_with_require_is_an_error(tmp_path, monkeypatch):
    lock = _lock(tmp_path)
    with pytest.raises(SystemExit) as exc:
        _run(monkeypatch, "--lock", str(lock), "--output-dir", str(tmp_path / "bt"), "--require")
    assert exc.value.code == 1


# ---- a malformed pin never reaches the network -------------------------------------------------


@pytest.mark.parametrize("sha", ["", "zz", "abc", "0" * 63, "g" * 64])
def test_pinned_tag_needs_a_real_digest(tmp_path, monkeypatch, sha):
    lock = _lock(tmp_path, tag="gebco2024-r1", asset="b.zip", sha256=sha)
    with pytest.raises(SystemExit):
        _run(monkeypatch, "--lock", str(lock), "--output-dir", str(tmp_path / "bt"))


def test_missing_lock_file_is_an_error(tmp_path, monkeypatch):
    with pytest.raises(SystemExit):
        _run(monkeypatch, "--lock", str(tmp_path / "nope.json"), "--output-dir", str(tmp_path / "bt"))


def test_malformed_lock_file_is_an_error(tmp_path, monkeypatch):
    lock = tmp_path / "lock.json"
    lock.write_text("{not json", encoding="utf-8")
    with pytest.raises(SystemExit):
        _run(monkeypatch, "--lock", str(lock), "--output-dir", str(tmp_path / "bt"))


def test_tag_without_asset_is_an_error(tmp_path, monkeypatch):
    lock = _lock(tmp_path, tag="gebco2024-r1", asset="", sha256="a" * 64)
    with pytest.raises(SystemExit):
        _run(monkeypatch, "--lock", str(lock), "--output-dir", str(tmp_path / "bt"))


# ---- the happy path ----------------------------------------------------------------------------


def test_verified_bundle_is_staged(tmp_path, monkeypatch, capsys):
    archive = _bundle(tmp_path)
    lock = _pin_local(tmp_path, monkeypatch, archive)
    out_dir = tmp_path / "bt"
    assert _run(monkeypatch, "--lock", str(lock), "--output-dir", str(out_dir)) == 0
    assert (out_dir / "terrain" / "world" / "f0" / "l0" / "tile_0_0.png").is_file()
    assert (out_dir / "ATTRIBUTION.md").is_file()
    out = capsys.readouterr().out
    assert "verified" in out
    assert "gebco2024-r1" in out


def test_a_stale_output_dir_is_replaced_not_merged(tmp_path, monkeypatch):
    out_dir = tmp_path / "bt"
    (out_dir / "terrain" / "world" / "f0" / "l0").mkdir(parents=True)
    stale = out_dir / "terrain" / "world" / "f0" / "l0" / "tile_9_9.png"
    stale.write_bytes(b"stale")
    archive = _bundle(tmp_path)
    lock = _pin_local(tmp_path, monkeypatch, archive)
    assert _run(monkeypatch, "--lock", str(lock), "--output-dir", str(out_dir)) == 0
    assert not stale.exists()  # a previous bundle's tiles must not survive into the new one


# ---- the refusals that a release depends on ----------------------------------------------------


def test_sha_mismatch_fails(tmp_path, monkeypatch):
    archive = _bundle(tmp_path)
    lock = _pin_local(tmp_path, monkeypatch, archive)
    data = json.loads(lock.read_text())
    data["sha256"] = "0" * 64
    lock.write_text(json.dumps(data), encoding="utf-8")
    with pytest.raises(SystemExit):
        _run(monkeypatch, "--lock", str(lock), "--output-dir", str(tmp_path / "bt"))


def test_size_mismatch_fails(tmp_path, monkeypatch):
    archive = _bundle(tmp_path)
    lock = _pin_local(tmp_path, monkeypatch, archive)
    data = json.loads(lock.read_text())
    data["size_bytes"] = data["size_bytes"] + 1
    lock.write_text(json.dumps(data), encoding="utf-8")
    with pytest.raises(SystemExit):
        _run(monkeypatch, "--lock", str(lock), "--output-dir", str(tmp_path / "bt"))


def test_a_tiny_payload_is_rejected_before_it_is_trusted(tmp_path, monkeypatch):
    """A GitHub error page or an LFS pointer hashes fine; it is just not a terrain bundle."""
    archive = _bundle(tmp_path, pad=16)
    lock = _pin_local(tmp_path, monkeypatch, archive)
    with pytest.raises(SystemExit):
        _run(monkeypatch, "--lock", str(lock), "--output-dir", str(tmp_path / "bt"))


def test_missing_sentinel_tile_fails(tmp_path, monkeypatch):
    """The tree extracts cleanly and the engine would mount NOTHING — the silent failure."""
    archive = _bundle(tmp_path, sentinel=False)
    lock = _pin_local(tmp_path, monkeypatch, archive)
    with pytest.raises(SystemExit):
        _run(monkeypatch, "--lock", str(lock), "--output-dir", str(tmp_path / "bt"))


def test_wrong_terrain_id_fails(tmp_path, monkeypatch):
    """The sentinel is per terrain id; a bundle built as 'earth' does not answer for 'world'."""
    archive = _bundle(tmp_path)
    lock = _pin_local(tmp_path, monkeypatch, archive, terrain_id="earth")
    with pytest.raises(SystemExit):
        _run(monkeypatch, "--lock", str(lock), "--output-dir", str(tmp_path / "bt"))


def test_incomplete_face_set_fails(tmp_path, monkeypatch):
    archive = _bundle(tmp_path, faces=4)
    lock = _pin_local(tmp_path, monkeypatch, archive)
    with pytest.raises(SystemExit):
        _run(monkeypatch, "--lock", str(lock), "--output-dir", str(tmp_path / "bt"))


def test_missing_attribution_fails(tmp_path, monkeypatch):
    """GEBCO requires attribution and ESA WorldCover is CC BY 4.0 — an unattributed tree cannot ship."""
    archive = _bundle(tmp_path, attribution=False)
    lock = _pin_local(tmp_path, monkeypatch, archive)
    with pytest.raises(SystemExit):
        _run(monkeypatch, "--lock", str(lock), "--output-dir", str(tmp_path / "bt"))


def test_archive_member_escaping_the_output_dir_is_refused(tmp_path, monkeypatch):
    archive = tmp_path / "evil.zip"
    with zipfile.ZipFile(archive, "w") as zf:
        zf.writestr("../escaped.png", os.urandom(1 << 20).hex())
    lock = _pin_local(tmp_path, monkeypatch, archive)
    with pytest.raises(SystemExit):
        _run(monkeypatch, "--lock", str(lock), "--output-dir", str(tmp_path / "bt"))
    assert not (tmp_path / "escaped.png").exists()


def test_a_payload_that_is_not_an_archive_fails(tmp_path, monkeypatch):
    archive = tmp_path / "junk.zip"
    archive.write_bytes(os.urandom(2 << 20))
    lock = _pin_local(tmp_path, monkeypatch, archive)
    with pytest.raises(SystemExit):
        _run(monkeypatch, "--lock", str(lock), "--output-dir", str(tmp_path / "bt"))


# ---- the shipped pin ---------------------------------------------------------------------------


def test_the_repo_pin_is_valid_json_and_internally_consistent():
    """The pin committed to the repo must always parse, and must be complete or empty — never half.

    This ASSERTS ON THE PIN, and deliberately does not run the fetcher against it. The earlier
    version did, which was free while the pin was empty (the fetcher exits 0 and says so) and became
    a **156 MB download on every CI run** the moment a real bundle was pinned — a unit test reaching
    out to a release CDN to re-prove what the twenty tests above already prove against synthetic
    bundles. What is worth checking here is the thing those tests cannot see: that the committed pin
    is coherent.

    A half-filled pin is the failure this guards. `tag` set with an empty `sha256` would download an
    asset and verify nothing; `sha256` set with an empty `tag` would silently skip the fetch and ship
    a release with no terrain while looking pinned.
    """
    lock = REPO_ROOT / "tools" / "base_terrain.lock.json"
    data = json.loads(lock.read_text(encoding="utf-8"))
    assert data["repo"] == "fighters-legacy/fl-base-terrain"
    assert data["terrain_id"] == "world"
    assert isinstance(data["max_level"], int) and data["max_level"] >= 0

    pinned = [bool(data[k]) for k in ("tag", "asset", "sha256", "size_bytes")]
    assert all(pinned) or not any(pinned), (
        f"the pin is half-filled: {dict((k, data[k]) for k in ('tag', 'asset', 'sha256', 'size_bytes'))}"
    )

    if all(pinned):
        assert len(data["sha256"]) == 64 and all(c in "0123456789abcdef" for c in data["sha256"])
        assert data["size_bytes"] > (1 << 20), "a terrain bundle is megabytes, not bytes"
        assert data["asset"].endswith((".zip", ".tar", ".tar.gz", ".tgz"))
        assert data["sources"], "a pinned bundle must record the source grids it was built from"
