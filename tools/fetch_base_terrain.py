# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Fetch and stage the bundled coarse global base terrain (#474, decided by #1199/#1202).

The bundle is an ENGINE artifact, not a content pack: loadBundledBaseTerrain() mounts it as
`builtin:base-terrain` at INT_MIN priority so a zero-pack launch shows a recognisable Earth. It is
produced rarely (build_global_base.sh, from a ~7.5 GB GEBCO grid on a workstation with GDAL) and
consumed by every release, so it is published once as a versioned release asset in its own
repository and pinned here by sha256 -- route C of the #1199 decision record.

    tools/fetch_base_terrain.py [--lock FILE] [--output-dir DIR] [--require]

Exit status is 0 when the bundle was staged AND verified, and 0 when the lock declares no pinned
bundle (the release then ships the procedural horizon, as every release to date has). It is
non-zero for every other outcome -- a bad download, a checksum mismatch, a malformed archive, or a
tree that extracts without the sentinel tile the engine gates the mount on.

THE POINT OF THE EXIT CODES: a release must never *quietly* ship without the terrain it was
supposed to carry. "Not pinned" is a declared state that says so on stdout; anything else is a
failure that stops the release. Pass --require to make even the unpinned state an error (used once
the bundle is expected to exist).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

# Read in chunks so a 50 MB asset does not land in memory twice.
_CHUNK = 1 << 20
# A download this small is a GitHub error page or an LFS pointer, never a terrain bundle.
_MIN_PLAUSIBLE_BYTES = 1 << 20


def _fail(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    raise SystemExit(1)


def _load_lock(path: Path) -> dict:
    try:
        with path.open(encoding="utf-8") as fh:
            lock = json.load(fh)
    except FileNotFoundError:
        _fail(f"pin file not found: {path}")
    except json.JSONDecodeError as exc:
        _fail(f"pin file is not valid JSON: {path}: {exc}")
    if not isinstance(lock, dict):
        _fail(f"pin file must be a JSON object: {path}")
    return lock


def _asset_url(lock: dict) -> str:
    repo = str(lock.get("repo", "")).strip()
    tag = str(lock.get("tag", "")).strip()
    asset = str(lock.get("asset", "")).strip()
    if not repo:
        _fail("pin file names a tag but no repo")
    if not asset:
        _fail("pin file names a tag but no asset")
    return f"https://github.com/{repo}/releases/download/{tag}/{asset}"


def _download(url: str, dest: Path) -> None:
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    req = urllib.request.Request(url, headers={"User-Agent": "fighters-legacy-release"})
    if token:
        # A private source repo still resolves in CI; a public one ignores the header.
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=120) as resp, dest.open("wb") as out:
            while True:
                chunk = resp.read(_CHUNK)
                if not chunk:
                    break
                out.write(chunk)
    except urllib.error.HTTPError as exc:
        _fail(f"download failed: {url}: HTTP {exc.code} {exc.reason}")
    except urllib.error.URLError as exc:
        _fail(f"download failed: {url}: {exc.reason}")
    except OSError as exc:
        _fail(f"download failed: {url}: {exc}")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        while True:
            chunk = fh.read(_CHUNK)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _safe_extract(archive: Path, dest: Path) -> None:
    """Extract a .zip or .tar.* to `dest`, refusing any member that escapes it."""
    dest.mkdir(parents=True, exist_ok=True)
    root = dest.resolve()

    def _checked(name: str) -> Path:
        target = (root / name).resolve()
        if target != root and root not in target.parents:
            _fail(f"archive member escapes the output directory: {name}")
        return target

    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as zf:
            for member in zf.namelist():
                _checked(member)
            zf.extractall(root)
        return
    try:
        with tarfile.open(archive) as tf:
            for member in tf.getmembers():
                if member.islnk() or member.issym():
                    _fail(f"archive contains a link member: {member.name}")
                _checked(member.name)
            # `data` filter is 3.12+; it is the stricter path where available.
            if sys.version_info >= (3, 12):
                tf.extractall(root, filter="data")
            else:
                tf.extractall(root)
    except tarfile.TarError as exc:
        _fail(f"archive is neither a valid zip nor a valid tar: {archive.name}: {exc}")


def _verify_tree(out_dir: Path, terrain_id: str) -> int:
    """The engine gates the mount on ONE sentinel tile; a tree without it is inert.

    Returns the tile count, so the caller can report what was staged rather than just "ok".
    """
    sentinel = out_dir / "terrain" / terrain_id / "f0" / "l0" / "tile_0_0.png"
    if not sentinel.is_file():
        _fail(
            f"staged tree has no sentinel tile at {sentinel.relative_to(out_dir)} -- "
            f"loadBundledBaseTerrain() would find nothing and mount nothing. Check that the "
            f"archive's root is the base-terrain/ tree itself and that terrain_id is correct."
        )
    faces = sorted(p.name for p in (out_dir / "terrain" / terrain_id).glob("f*") if p.is_dir())
    if len(faces) != 6:
        _fail(f"staged tree has {len(faces)} cube-sphere face roots, expected 6: {faces}")
    return sum(1 for _ in (out_dir / "terrain" / terrain_id).rglob("tile_*.png"))


def main() -> int:
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lock", type=Path, default=here / "base_terrain.lock.json", help="pin file")
    ap.add_argument("--output-dir", type=Path, default=Path("base-terrain"), help="where to stage the tree")
    ap.add_argument("--require", action="store_true", help="treat an unpinned bundle as an error")
    args = ap.parse_args()

    lock = _load_lock(args.lock)
    tag = str(lock.get("tag", "")).strip()
    if not tag:
        msg = f"no base-terrain bundle is pinned in {args.lock.name}"
        if args.require:
            _fail(msg + " (--require)")
        print(f"base-terrain: {msg} -- this build ships the procedural horizon (#474).")
        return 0

    want_sha = str(lock.get("sha256", "")).strip().lower()
    if len(want_sha) != 64 or any(c not in "0123456789abcdef" for c in want_sha):
        _fail(f"pin file names tag {tag} but its sha256 is not a 64-char hex digest")
    want_size = int(lock.get("size_bytes", 0) or 0)
    terrain_id = str(lock.get("terrain_id", "world")).strip() or "world"
    url = _asset_url(lock)

    print(f"base-terrain: fetching {url}")
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / "bundle"
        _download(url, archive)
        got_size = archive.stat().st_size
        if got_size < _MIN_PLAUSIBLE_BYTES:
            _fail(f"downloaded {got_size} bytes -- too small to be a terrain bundle (an error page?)")
        if want_size and got_size != want_size:
            _fail(f"size mismatch: pinned {want_size} bytes, downloaded {got_size}")
        got_sha = _sha256(archive)
        if got_sha != want_sha:
            _fail(f"sha256 mismatch:\n  pinned     {want_sha}\n  downloaded {got_sha}")
        print(f"base-terrain: sha256 {got_sha} verified ({got_size} bytes)")

        if args.output_dir.exists():
            shutil.rmtree(args.output_dir)
        _safe_extract(archive, args.output_dir)

    tiles = _verify_tree(args.output_dir, terrain_id)
    print(f"base-terrain: staged {tag} to {args.output_dir}/ -- {tiles} tiles, terrain id '{terrain_id}'")
    attribution = args.output_dir / "ATTRIBUTION.md"
    if not attribution.is_file():
        _fail(
            "staged tree carries no ATTRIBUTION.md -- GEBCO requires attribution and ESA WorldCover "
            "is CC BY 4.0, so a bundle without its notices must not ship. Rebuild it with a "
            "build_global_base.sh new enough to emit one."
        )
    print(f"base-terrain: attribution present ({attribution.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
