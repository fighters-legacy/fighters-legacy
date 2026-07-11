# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Generate fighters-legacy cube-sphere terrain tile PNGs from a global elevation dataset.

Unlike the planar/UTM gen_terrain_chunks.py, this generates the #472 cube-sphere quadtree
tiles: for each TileKey (face, level, i, j) it samples a GLOBAL lat/lon source raster at the
tile's cube-sphere lattice (129x129, matching kTileHeightmapSize) and writes a 16-bit
grayscale PNG at the canonical pack path:

    terrain/<id>/f<face>/l<level>/tile_<i>_<j>.png       (height)
    terrain/<id>/f<face>/l<level>/tile_<i>_<j>_lc.png    (land cover, --landcover-source)

Resolution comes from the quadtree LEVEL (level L => 2^L tiles per face axis), not per-tile LOD
pyramids. Face 2 (+Y) is the north pole; the sphere maps to Earth via the engine's geodetic
convention (Geodetic.h): lat = asin(dir.y), lon = atan2(dir.x, dir.z), longitude positive east.

The cube-sphere math is ported from engine/render/CubeSphere.h; verify against
tests/test_cube_sphere.cpp if that warp changes.

Usage:
    python3 tools/gen_terrain_tiles.py \\
        --input world_dem.vrt \\
        --terrain-id world \\
        --output-dir mods/fl-base-pack/ \\
        --min-level 0 --max-level 5 \\
        --landcover-source worldcover.vrt

Height encoding (matches generateProceduralTile so pack + procedural tiles are consistent):
    uint16 = clamp(elevation_m * height_scale + height_offset, 0, 65535)   (default offset 32768)
Land cover: nearest-neighbour class value stored directly as uint16 (engine reads the low byte).
Nodata / out-of-bounds samples encode as sea level (height) or class 0 (land cover).
"""

from __future__ import annotations

import argparse
import math
import os
import re
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

# Guard GDAL import so --help and pytest unit tests work without GDAL installed.
try:
    from osgeo import gdal
    gdal.UseExceptions()
    _HAS_GDAL = True
except ImportError:
    gdal = None  # type: ignore[assignment]
    _HAS_GDAL = False

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

TILE_PIXELS = 129            # kTileHeightmapSize (129x129 uniform tiles, #472)
TERRAIN_ID_RE = re.compile(r'^[a-z][a-z0-9_-]*$')
QUARTER_PI = math.pi / 4.0
CUBE_FACE_COUNT = 6

# ---------------------------------------------------------------------------
# Cube-sphere math (ported from engine/render/CubeSphere.h)
# ---------------------------------------------------------------------------


def cube_warp(c: "np.ndarray") -> "np.ndarray":
    """Tangent-adaptive face warp: tan(c * pi/4)."""
    return np.tan(np.asarray(c, dtype=np.float64) * QUARTER_PI)


def face_uv_to_direction(face: int, u: "np.ndarray", v: "np.ndarray") -> tuple:
    """Face uv in [0,1]^2 -> unit direction (x, y, z) arrays from the planet centre.

    Mirrors cubePoint()/faceUvToDirection() in CubeSphere.h exactly (per-face axis table).
    """
    a = cube_warp(2.0 * np.asarray(u, dtype=np.float64) - 1.0)
    b = cube_warp(2.0 * np.asarray(v, dtype=np.float64) - 1.0)
    ones = np.ones_like(a)
    if face == 0:      # +X: U=+Y V=+Z   -> ( 1, a, b)
        px, py, pz = ones, a, b
    elif face == 1:    # -X: U=+Z V=+Y   -> (-1, b, a)
        px, py, pz = -ones, b, a
    elif face == 2:    # +Y: U=+Z V=+X   -> ( b, 1, a)  (centre -> north pole)
        px, py, pz = b, ones, a
    elif face == 3:    # -Y: U=+X V=+Z   -> ( a,-1, b)
        px, py, pz = a, -ones, b
    elif face == 4:    # +Z: U=+X V=+Y   -> ( a, b, 1)
        px, py, pz = a, b, ones
    elif face == 5:    # -Z: U=+Y V=+X   -> ( b, a,-1)
        px, py, pz = b, a, -ones
    else:
        raise ValueError(f"face must be 0..5, got {face}")
    inv = 1.0 / np.sqrt(px * px + py * py + pz * pz)
    return px * inv, py * inv, pz * inv


def direction_to_latlon(x: "np.ndarray", y: "np.ndarray", z: "np.ndarray") -> tuple:
    """Unit direction -> (lat_deg, lon_deg), engine convention (Geodetic.h).

    lat = asin(y), lon = atan2(x, z); longitude positive east, +Y = north pole.
    """
    lat = np.degrees(np.arcsin(np.clip(y, -1.0, 1.0)))
    lon = np.degrees(np.arctan2(x, z))
    return lat, lon


def tile_latlon_grid(face: int, level: int, i: int, j: int,
                     tile_px: int = TILE_PIXELS) -> tuple:
    """(lat, lon) degree arrays of shape (tile_px, tile_px) for the tile's lattice.

    Matches generateProceduralTile's layout: out[row, col] with s = col/(N-1) (face u),
    t = row/(N-1) (face v), u = (i + s)/2^level, v = (j + t)/2^level. No vertical flip.
    """
    n = float(1 << level)
    st = np.linspace(0.0, 1.0, tile_px)
    s, t = np.meshgrid(st, st)   # s varies along columns (u), t along rows (v)
    u = (i + s) / n
    v = (j + t) / n
    dx, dy, dz = face_uv_to_direction(face, u, v)
    return direction_to_latlon(dx, dy, dz)


def encode_heights(elev_m: "np.ndarray", height_scale: float,
                   height_offset: float) -> "np.ndarray":
    """uint16 = clamp(elev_m * scale + offset, 0, 65535); NaN -> sea level before encode."""
    arr = np.array(elev_m, dtype=np.float64, copy=True)
    arr[np.isnan(arr)] = 0.0
    arr = arr * height_scale + height_offset
    np.clip(arr, 0, 65535, out=arr)
    return arr.astype(np.uint16)


def tile_rel_path(terrain_id: str, face: int, level: int, i: int, j: int,
                  layer: str = "height") -> str:
    """Canonical pack-relative tile path (matches FolderContentPack::resolveTilePath)."""
    suffix = {"height": ".png", "landcover": "_lc.png", "satellite": "_sat.ktx2"}[layer]
    return f"terrain/{terrain_id}/f{face}/l{level}/tile_{i}_{j}{suffix}"


def enumerate_tiles(faces, min_level: int, max_level: int):
    """Yield (face, level, i, j) for every tile in the face x level range."""
    for level in range(min_level, max_level + 1):
        n = 1 << level
        for face in faces:
            for j in range(n):
                for i in range(n):
                    yield (face, level, i, j)


# ---------------------------------------------------------------------------
# GDAL sampling (worker side)
# ---------------------------------------------------------------------------

# Per-worker globals (avoid pickling gdal.Dataset).
_g_dem = None
_g_lc = None
_g_common = None


def _sample_raster(band, gt, width: int, height: int, nodata,
                   lat: "np.ndarray", lon: "np.ndarray", nearest: bool) -> "np.ndarray":
    """Sample a north-up EPSG:4326 raster at (lat, lon) arrays.

    Reads only the covering latitude band across the full width (so pole tiles that span all
    longitudes and antimeridian-crossing tiles are handled uniformly), then interpolates —
    bilinear for height, nearest for land-cover class. Longitude wraps modulo width; latitude
    clamps. NaN nodata is preserved for the caller to map to a sentinel.
    """
    # Fractional source pixel coords. North-up 4326: gt[1] > 0 (deg/px lon), gt[5] < 0 (lat).
    fx = (lon - gt[0]) / gt[1]                      # fractional column (may be <0 or >=width)
    fy = (lat - gt[3]) / gt[5]                      # fractional row
    y0 = int(max(0, math.floor(np.min(fy)) - 1))
    y1 = int(min(height, math.ceil(np.max(fy)) + 2))
    if y1 <= y0:
        y0, y1 = 0, min(height, 1)
    band_arr = band.ReadAsArray(0, y0, width, y1 - y0).astype(np.float64)
    if nodata is not None and not math.isnan(nodata):
        band_arr[band_arr == nodata] = np.nan

    ry = fy - y0
    if nearest:
        xi = np.mod(np.rint(fx).astype(np.int64), width)
        yi = np.clip(np.rint(ry).astype(np.int64), 0, band_arr.shape[0] - 1)
        return band_arr[yi, xi]

    x0 = np.floor(fx)
    wx = fx - x0
    xi0 = np.mod(x0.astype(np.int64), width)
    xi1 = np.mod(xi0 + 1, width)
    ry_floor = np.floor(ry)
    wy = np.clip(ry - ry_floor, 0.0, 1.0)
    yi0 = np.clip(ry_floor.astype(np.int64), 0, band_arr.shape[0] - 1)
    yi1 = np.clip(yi0 + 1, 0, band_arr.shape[0] - 1)
    top = band_arr[yi0, xi0] * (1.0 - wx) + band_arr[yi0, xi1] * wx
    bot = band_arr[yi1, xi0] * (1.0 - wx) + band_arr[yi1, xi1] * wx
    return top * (1.0 - wy) + bot * wy


def _write_png_u16(path: Path, data_u16: "np.ndarray") -> None:
    """Write a 2-D uint16 array as a 16-bit grayscale PNG via GDAL."""
    h, w = data_u16.shape
    mem = gdal.GetDriverByName("MEM").Create("", w, h, 1, gdal.GDT_UInt16)
    mem.GetRasterBand(1).WriteArray(data_u16)
    gdal.GetDriverByName("PNG").CreateCopy(str(path).replace("\\", "/"), mem)
    mem = None


def _worker_init(dem_path: str, lc_path, common: tuple) -> None:
    global _g_dem, _g_lc, _g_common
    gdal.UseExceptions()
    _g_dem = gdal.Open(dem_path, gdal.GA_ReadOnly)
    _g_lc = gdal.Open(lc_path, gdal.GA_ReadOnly) if lc_path else None
    _g_common = common


def _worker_tile(key: tuple) -> tuple:
    """Sample + write one tile's PNG(s). Returns (key, ok)."""
    face, level, i, j = key
    (output_dir, terrain_id, tile_px, height_scale, height_offset, skip_existing) = _g_common

    height_path = Path(output_dir) / tile_rel_path(terrain_id, face, level, i, j, "height")
    if skip_existing and height_path.exists():
        return key, True
    try:
        lat, lon = tile_latlon_grid(face, level, i, j, tile_px)
        dem_band = _g_dem.GetRasterBand(1)
        dem_nodata = dem_band.GetNoDataValue()
        elev = _sample_raster(dem_band, _g_dem.GetGeoTransform(), _g_dem.RasterXSize,
                              _g_dem.RasterYSize, dem_nodata, lat, lon, nearest=False)
        height_path.parent.mkdir(parents=True, exist_ok=True)
        _write_png_u16(height_path, encode_heights(elev, height_scale, height_offset))

        if _g_lc is not None:
            lc_band = _g_lc.GetRasterBand(1)
            lc_nodata = lc_band.GetNoDataValue()
            cover = _sample_raster(lc_band, _g_lc.GetGeoTransform(), _g_lc.RasterXSize,
                                   _g_lc.RasterYSize, lc_nodata, lat, lon, nearest=True)
            cover = np.nan_to_num(cover, nan=0.0)
            lc_u16 = np.clip(cover, 0, 65535).astype(np.uint16)
            _write_png_u16(Path(output_dir) / tile_rel_path(terrain_id, face, level, i, j, "landcover"),
                           lc_u16)
        return key, True
    except Exception as exc:  # noqa: BLE001 - report + continue, never abort the batch
        print(f"  Warning: tile f{face} l{level} {i},{j} failed: {exc}", file=sys.stderr)
        return key, False


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _parse_faces(spec: str) -> list:
    if spec.strip().lower() in ("all", "*"):
        return list(range(CUBE_FACE_COUNT))
    faces = []
    for tok in spec.split(","):
        tok = tok.strip()
        if not tok:
            continue
        f = int(tok)
        if not 0 <= f < CUBE_FACE_COUNT:
            raise ValueError(f"face {f} out of range 0..5")
        faces.append(f)
    if not faces:
        raise ValueError("no faces selected")
    return faces


def _parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="gen_terrain_tiles.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--input", required=True, metavar="PATH",
                   help="Global elevation source (GDAL-readable, lat/lon EPSG:4326)")
    p.add_argument("--terrain-id", required=True, metavar="SLUG",
                   help="Terrain identifier — must match ^[a-z][a-z0-9_-]*$")
    p.add_argument("--output-dir", required=True, metavar="DIR",
                   help="Output root (mod directory); tiles land under terrain/<id>/")
    p.add_argument("--faces", default="all", metavar="LIST",
                   help="Comma-separated face indices 0..5, or 'all' (default: all)")
    p.add_argument("--min-level", type=int, default=0, metavar="N",
                   help="Lowest quadtree level to generate (default: 0)")
    p.add_argument("--max-level", required=True, type=int, metavar="N",
                   help="Highest quadtree level to generate (2^level tiles per face axis)")
    p.add_argument("--landcover-source", default=None, metavar="PATH",
                   help="Optional global land-cover source; also emits _lc.png tiles")
    p.add_argument("--height-scale", type=float, default=1.0, metavar="FLOAT",
                   help="Multiply elevation before uint16 encode (default: 1.0)")
    p.add_argument("--height-offset", type=float, default=32768.0, metavar="FLOAT",
                   help="Add to elevation after scale before uint16 encode (default: 32768)")
    p.add_argument("--tile-pixels", type=int, default=TILE_PIXELS, metavar="N",
                   help=f"Tile edge in pixels (default: {TILE_PIXELS} = kTileHeightmapSize)")
    p.add_argument("--workers", type=int, default=None, metavar="N",
                   help="Parallel worker processes (default: os.cpu_count())")
    p.add_argument("--skip-existing", action="store_true",
                   help="Skip tiles whose height PNG already exists (resume interrupted run)")
    return p.parse_args(argv)


def _validate_args(args: argparse.Namespace) -> None:
    if not TERRAIN_ID_RE.match(args.terrain_id):
        sys.exit(f"Error: --terrain-id '{args.terrain_id}' must match ^[a-z][a-z0-9_-]*$")
    if not Path(args.input).exists():
        sys.exit(f"Error: input file not found: {args.input}")
    if args.landcover_source and not Path(args.landcover_source).exists():
        sys.exit(f"Error: land-cover source not found: {args.landcover_source}")
    if args.min_level < 0 or args.max_level < args.min_level:
        sys.exit("Error: require 0 <= --min-level <= --max-level")
    if args.max_level > 20:
        sys.exit("Error: --max-level > 20 would generate an astronomical tile count")
    if args.tile_pixels < 2:
        sys.exit("Error: --tile-pixels must be >= 2")


def main(argv=None) -> None:
    args = _parse_args(argv)
    if not _HAS_GDAL:
        sys.exit(
            "Error: GDAL Python bindings not found.\n"
            "  Linux:   sudo apt install python3-gdal\n"
            "  macOS:   brew install gdal\n"
            "  Windows: conda install -c conda-forge gdal"
        )
    if int(gdal.VersionInfo()) < 3000000:
        sys.exit("Error: GDAL 3.0+ required")
    _validate_args(args)

    try:
        faces = _parse_faces(args.faces)
    except ValueError as exc:
        sys.exit(f"Error: --faces: {exc}")

    tiles = list(enumerate_tiles(faces, args.min_level, args.max_level))
    total = len(tiles)
    workers = args.workers if args.workers is not None else os.cpu_count()
    dem_path = str(Path(args.input).resolve())
    lc_path = str(Path(args.landcover_source).resolve()) if args.landcover_source else None
    common = (str(args.output_dir), args.terrain_id, args.tile_pixels,
              args.height_scale, args.height_offset, args.skip_existing)

    print(f"[1/2] Generating {total} tile(s) across faces {faces}, "
          f"levels {args.min_level}..{args.max_level} ({workers} worker(s))"
          f"{' + land cover' if lc_path else ''}")

    done = 0
    failures = 0
    report_every = max(1, total // 200)
    with ProcessPoolExecutor(max_workers=workers, initializer=_worker_init,
                             initargs=(dem_path, lc_path, common)) as pool:
        chunksize = max(1, total // (workers * 10))
        for _key, ok in pool.map(_worker_tile, tiles, chunksize=chunksize):
            done += 1
            if not ok:
                failures += 1
            if done % report_every == 0 or done == total:
                suffix = f"  [{failures} failed]" if failures else ""
                print(f"  {done}/{total} ({done * 100 // total}%){suffix}")

    dest = Path(args.output_dir) / "terrain" / args.terrain_id
    print(f"\n[2/2] Done. Wrote tiles to {dest}/")
    if failures:
        print(f"WARNING: {failures} tile(s) failed — see stderr.", file=sys.stderr)


if __name__ == "__main__":
    main()
