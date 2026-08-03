#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Fighters Legacy contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate satellite terrain textures (tile_i_j_sat.ktx2) from Sentinel-2 imagery (#488).

Produces the `_sat.ktx2` colour layer the terrain renderer samples (`TileLayer::Satellite`), so a
theater can carry real cloud-free HD imagery instead of the procedural biomes. For each cube-sphere
tile intersecting a bounding box it:

  1. discovers cloud-free Sentinel-2 L2A scenes via the earth-search STAC API (AWS Open Data,
     ANONYMOUS — no account),
  2. reads the B04/B03/B02 (RGB) + SCL (scene-classification) bands via GDAL /vsicurl,
  3. builds an SCL-cloud-masked median composite over the scenes,
  4. converts surface reflectance to sRGB (`s2_to_srgb`),
  5. resamples to the tile's cube-sphere lattice, writes a temp PNG, and Basis-UASTC-encodes it to
     `tile_<i>_<j>_sat.ktx2` via `tex-compress` (toktx) — transcodable to BC7/ASTC per GPU at load.

Copernicus Sentinel data terms require attribution ("Contains modified Copernicus Sentinel data
<year>") on a player-reachable credits surface when the derived tiles ship — see NOTICE.

The cube-sphere lattice math is shared with gen_terrain_tiles.py. GDAL / requests are guarded so
--help and the pure-logic unit tests (tests/test_gen_terrain_color.py) run without them.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

# Reuse the cube-sphere + tile helpers (they guard their own GDAL import).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_terrain_tiles import bbox_tiles, tile_latlon_grid, tile_rel_path  # noqa: E402

try:
    import numpy as np  # type: ignore

    _HAVE_NUMPY = True
except ImportError:
    _HAVE_NUMPY = False

try:
    from osgeo import gdal  # type: ignore

    _HAVE_GDAL = True
except ImportError:
    _HAVE_GDAL = False

STAC_URL = "https://earth-search.aws.element84.com/v1/search"
S2_COLLECTION = "sentinel-2-l2a"
TILE_SIZE = 256  # satellite texture resolution per tile

# SCL (scene classification) values that are NOT usable ground: 0 no-data, 1 saturated, 3 cloud
# shadow, 8 cloud medium, 9 cloud high, 10 thin cirrus, 11 snow (kept as ground here since terrain
# snow is legitimate imagery).
SCL_MASKED = {0, 1, 3, 8, 9, 10}


def scl_cloud_mask(scl):
    """Boolean mask (True = MASKED / unusable) for a Sentinel-2 SCL band array."""
    if not _HAVE_NUMPY:
        raise RuntimeError("numpy required")
    m = np.zeros(scl.shape, dtype=bool)
    for v in SCL_MASKED:
        m |= scl == v
    return m


def s2_to_srgb(rgb, scale=3000.0, gamma=2.2):
    """Sentinel-2 surface reflectance (B04,B03,B02 stacked, HxWx3) -> 8-bit sRGB.

    Reflectance is scaled to [0,1] against `scale` (a bright-scene ceiling ~0.3 reflectance in the
    L2A 10000x integer encoding), gamma-corrected, and clamped. Monotonic + saturating."""
    if not _HAVE_NUMPY:
        raise RuntimeError("numpy required")
    x = np.clip(np.asarray(rgb, dtype=np.float64) / scale, 0.0, 1.0)
    x = np.power(x, 1.0 / gamma)
    return (np.clip(x, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def select_scenes(features, max_cloud=20.0, limit=6):
    """Pick usable scenes from STAC features: cloud cover <= max_cloud, ascending cloudiness."""
    scored = []
    for f in features:
        props = f.get("properties", {})
        cloud = props.get("eo:cloud_cover", props.get("s2:cloudy_pixel_percentage", 100.0))
        if cloud is None or cloud > max_cloud:
            continue
        scored.append((cloud, f))
    scored.sort(key=lambda t: t[0])
    return [f for _, f in scored[:limit]]


# bbox -> tile coverage now lives in gen_terrain_tiles.bbox_tiles (#1107), which BOTH generators
# call. It used to be here, filtering the full 4^level enumeration; a level-12 theater would test
# a hundred million keys to select a few hundred. It is a quadtree descent now, so this module gets
# the fix for free and there is one answer to "which tiles cover this box".


def _stac_search(bbox, max_cloud):  # pragma: no cover - network
    """POST an earth-search STAC query for Sentinel-2 L2A scenes over the bbox."""
    body = json.dumps({
        "collections": [S2_COLLECTION],
        "bbox": [bbox[1], bbox[0], bbox[3], bbox[2]],  # STAC bbox = [west, south, east, north]
        "query": {"eo:cloud_cover": {"lte": max_cloud}},
        "limit": 50,
    }).encode()
    req = urllib.request.Request(STAC_URL, data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return json.load(resp).get("features", [])


def _band_href(feature, name):  # pragma: no cover - network
    assets = feature.get("assets", {})
    a = assets.get(name) or assets.get(name.lower())
    return a.get("href") if a else None


def _read_tile_rgb(scenes, lat_deg, lon_deg):  # pragma: no cover - needs GDAL + network
    """Median cloud-masked RGB (HxWx3 reflectance) over `scenes` at the tile lat/lon lattice."""
    gdal.SetConfigOption("AWS_NO_SIGN_REQUEST", "YES")
    gdal.SetConfigOption("GDAL_HTTP_UNSAFESSL", "NO")
    h, w = lat_deg.shape
    stacks = []
    for feat in scenes:
        try:
            bands = []
            for bname in ("red", "green", "blue"):
                href = _band_href(feat, bname)
                ds = gdal.Open(f"/vsicurl/{href}")
                bands.append(_warp_to_latlon(ds, lat_deg, lon_deg))
            scl_ds = gdal.Open(f"/vsicurl/{_band_href(feat, 'scl')}")
            scl = _warp_to_latlon(scl_ds, lat_deg, lon_deg, nearest=True)
        except Exception as exc:  # noqa: BLE001
            print(f"  scene skipped: {exc}", file=sys.stderr)
            continue
        rgb = np.dstack(bands).astype(np.float64)
        mask = scl_cloud_mask(scl)
        rgb[mask] = np.nan
        stacks.append(rgb)
    if not stacks:
        return None
    return np.nanmedian(np.stack(stacks), axis=0)


def _warp_to_latlon(ds, lat_deg, lon_deg, nearest=False):  # pragma: no cover - needs GDAL
    """Bilinear (or nearest) sample of a projected COG at lat/lon points via a per-tile warp."""
    # For simplicity, warp the source window to EPSG:4326 over the tile bbox then sample.
    lat_min, lat_max = float(lat_deg.min()), float(lat_deg.max())
    lon_min, lon_max = float(lon_deg.min()), float(lon_deg.max())
    alg = gdal.GRA_NearestNeighbour if nearest else gdal.GRA_Bilinear
    warped = gdal.Warp("", ds, format="MEM", dstSRS="EPSG:4326", resampleAlg=alg,
                       outputBounds=[lon_min, lat_min, lon_max, lat_max],
                       width=lon_deg.shape[1], height=lat_deg.shape[0])
    arr = warped.GetRasterBand(1).ReadAsArray()
    return np.flipud(arr)  # north-up


def _encode_ktx2(srgb, out_path):  # pragma: no cover - needs toktx
    """Write an sRGB uint8 HxWx3 array to a portable Basis KTX2 via tex-compress (toktx).

    Uses UASTC (high quality, transcodes to BC7/ASTC at load) rather than ETC1S so the satellite
    imagery keeps its fidelity on close approach; the runtime transcodes it per-GPU.
    """
    from PIL import Image  # optional, only when actually encoding

    with tempfile.TemporaryDirectory() as td:
        png = Path(td) / "tile.png"
        Image.fromarray(srgb, "RGB").save(png)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["tex-compress", "--format", "uastc", "-o", str(out_path), str(png)],
                       check=True)


def main(argv=None):  # pragma: no cover - CLI wrapper over tested helpers
    ap = argparse.ArgumentParser(description="Generate Sentinel-2 satellite terrain tiles (#488).")
    ap.add_argument("--terrain-id", default="world")
    ap.add_argument("--faces", default="all")
    ap.add_argument("--min-level", type=int, default=8)
    ap.add_argument("--max-level", type=int, default=10)
    ap.add_argument("--bbox", nargs=4, type=float, required=True,
                    metavar=("LATMIN", "LONMIN", "LATMAX", "LONMAX"))
    ap.add_argument("--max-cloud", type=float, default=15.0)
    ap.add_argument("--output-dir", default="satellite-terrain")
    ap.add_argument("--tex-compress", default="tex-compress")
    args = ap.parse_args(argv)

    if not (_HAVE_GDAL and _HAVE_NUMPY):
        print("error: GDAL + numpy required (dnf install gdal python3-gdal python3-numpy)", file=sys.stderr)
        return 2

    from gen_terrain_tiles import _parse_faces  # local import; guarded module
    faces = _parse_faces(args.faces)
    tiles = bbox_tiles(faces, args.min_level, args.max_level, tuple(args.bbox))
    print(f"{len(tiles)} candidate tiles over the bbox")

    scenes = select_scenes(_stac_search(tuple(args.bbox), args.max_cloud), args.max_cloud)
    print(f"{len(scenes)} usable Sentinel-2 scenes (cloud <= {args.max_cloud}%)")
    if not scenes:
        print("no cloud-free scenes found for the bbox", file=sys.stderr)
        return 1

    written = 0
    for (face, level, i, j) in tiles:
        lat_deg, lon_deg = tile_latlon_grid(face, level, i, j, tile_px=TILE_SIZE)  # already degrees
        rgb = _read_tile_rgb(scenes, lat_deg, lon_deg)
        if rgb is None or np.all(np.isnan(rgb)):
            continue
        rgb = np.nan_to_num(rgb)
        srgb = s2_to_srgb(rgb)
        out = Path(args.output_dir) / tile_rel_path(args.terrain_id, face, level, i, j, "satellite")
        _encode_ktx2(srgb, out)
        written += 1
        print(f"  wrote {out}")
    print(f"done: {written} satellite tiles. Attribution: 'Contains modified Copernicus Sentinel data'.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
