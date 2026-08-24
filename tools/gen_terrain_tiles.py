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

Usage — a coarse GLOBAL base (full faces, every tile at each level):
    python3 tools/gen_terrain_tiles.py \\
        --input world_dem.vrt \\
        --terrain-id world \\
        --output-dir mods/fl-base-pack/ \\
        --min-level 0 --max-level 5 \\
        --landcover-source worldcover.vrt

Usage — a THEATER (only the tiles covering a region, #1107):
    python3 tools/gen_terrain_tiles.py \\
        --input world_dem.vrt \\
        --terrain-id world \\
        --output-dir mods/my-theater/ \\
        --bbox 30.0 32.0 38.0 42.0 \\
        --min-level 10 --max-level 12 \\
        --skip-existing

Without --bbox the run covers FULL faces — 4^level tiles each, so level 12 is millions of tiles
per face. --bbox scopes the run geographically (the same option gen_terrain_color.py takes) and
composes with --skip-existing for a resumable build.

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
    """uint16 = clamp(elev_m * scale + offset, 0, 65535); NaN -> sea level before encode.

    With a bathymetry source merged in (merge_bathymetry, #476), ocean cells carry real NEGATIVE
    elevation here, so they encode below the datum (offset) instead of flattening to sea level.
    """
    arr = np.array(elev_m, dtype=np.float64, copy=True)
    arr[np.isnan(arr)] = 0.0
    arr = arr * height_scale + height_offset
    np.clip(arr, 0, 65535, out=arr)
    return arr.astype(np.uint16)


def merge_bathymetry(dem_elev: "np.ndarray", bathy_elev: "np.ndarray") -> "np.ndarray":
    """Fill ocean / nodata gaps in a land DEM with GEBCO bathymetry (#476).

    Land DEMs (e.g. Copernicus GLO-30) are nodata over water, so ocean samples arrive as NaN and
    would otherwise flatten to sea level (encode_heights). Where the DEM is NaN and the bathymetry
    grid has a value, take the bathymetry elevation (real sea-floor depth, typically negative); land
    always keeps the DEM. Pure numpy — unit-tested without GDAL. Returns a new array.
    """
    dem = np.array(dem_elev, dtype=np.float64, copy=True)
    bathy = np.asarray(bathy_elev, dtype=np.float64)
    fill = np.isnan(dem) & ~np.isnan(bathy)
    dem[fill] = bathy[fill]
    return dem


def tile_rel_path(terrain_id: str, face: int, level: int, i: int, j: int,
                  layer: str = "height") -> str:
    """Canonical pack-relative tile path (matches FolderContentPack::resolveTilePath)."""
    suffix = {"height": ".png", "landcover": "_lc.png", "satellite": "_sat.ktx2"}[layer]
    return f"terrain/{terrain_id}/f{face}/l{level}/tile_{i}_{j}{suffix}"


def enumerate_tiles(faces, min_level: int, max_level: int, bbox=None):
    """Yield (face, level, i, j) for the requested tiles.

    Without `bbox`: every tile in the face x level range — 4^level per face, which is the whole
    globe and is what a coarse global base wants.

    With `bbox` = (lat_min, lon_min, lat_max, lon_max) in degrees: only the tiles covering that
    region. See `bbox_tiles` for why this is a quadtree DESCENT and not a filter over the full
    enumeration (#1107).
    """
    if bbox is not None:
        yield from bbox_tiles(faces, min_level, max_level, bbox)
        return
    for level in range(min_level, max_level + 1):
        n = 1 << level
        for face in faces:
            for j in range(n):
                for i in range(n):
                    yield (face, level, i, j)


# Lattice used to bound a tile's lat/lon extent. Odd, so it samples the tile's centre lines: a
# cube-sphere tile's latitude extremum in u sits at the face-axis zero (u = 0.5), not on an edge,
# so an even grid can miss it.
_BOUNDS_LATTICE = 17


def tile_latlon_bounds(face: int, level: int, i: int, j: int) -> tuple:
    """Conservative (lat_min, lon_min, lat_max, lon_max) degree bounds for one tile.

    Sampled, then PADDED by the widest adjacent-sample step. A cube-sphere tile is a smooth curved
    patch, so its true extremum can fall between samples; the pad is what makes these bounds a
    superset rather than an estimate, which is the property the descent below relies on.

    A tile spanning the antimeridian or containing a pole reports a near-global longitude span
    (samples land at both +180 and -180). That is over-inclusive, never under-inclusive, so it
    costs a few extra tiles and cannot drop a wanted one.
    """
    lat, lon = tile_latlon_grid(face, level, i, j, _BOUNDS_LATTICE)
    lat_pad = max(float(np.abs(np.diff(lat, axis=0)).max()), float(np.abs(np.diff(lat, axis=1)).max()))
    lon_pad = max(float(np.abs(np.diff(lon, axis=0)).max()), float(np.abs(np.diff(lon, axis=1)).max()))
    return (float(lat.min()) - lat_pad, float(lon.min()) - lon_pad,
            float(lat.max()) + lat_pad, float(lon.max()) + lon_pad)


def _bbox_intersects(bounds: tuple, bbox: tuple) -> bool:
    """Do a tile's (lat_min, lon_min, lat_max, lon_max) bounds overlap the requested bbox?"""
    t_lat_min, t_lon_min, t_lat_max, t_lon_max = bounds
    lat_min, lon_min, lat_max, lon_max = bbox
    if t_lat_max < lat_min or t_lat_min > lat_max:
        return False
    if t_lon_max < lon_min or t_lon_min > lon_max:
        return False
    return True


def bbox_tiles(faces, min_level: int, max_level: int, bbox) -> list:
    """Tiles covering a lat/lon bbox, found by DESCENDING the quadtree.

    `bbox` = (lat_min, lon_min, lat_max, lon_max) in degrees.

    The descent is the whole point. Filtering the full `enumerate_tiles` range instead — which is
    what a naive bbox option would do — still visits 4^level keys per face: a starter theater at
    level 12 would test 100 million tiles to select a few hundred, so the tool would appear to hang
    before it wrote anything. Children nest strictly inside their parent's uv square, so a parent
    whose (padded, superset) bounds miss the bbox cannot have a descendant that hits it, and
    pruning there discards 4^k tiles for the cost of one test.

    Levels below `min_level` are still descended through — they are the path to the tiles that are
    wanted — but only `min_level..max_level` is emitted. Output is ordered coarse-first (by level,
    then face, then j, then i) to match the unfiltered enumeration, so `--skip-existing` resumes a
    partial run the same way either way.

    Pure numpy — no GDAL, unit-tested.
    """
    lat_min, lon_min, lat_max, lon_max = (float(v) for v in bbox)
    if lat_min > lat_max or lon_min > lon_max:
        raise ValueError("bbox must be (lat_min, lon_min, lat_max, lon_max) with min <= max")
    box = (lat_min, lon_min, lat_max, lon_max)

    by_level = {level: [] for level in range(min_level, max_level + 1)}

    def descend(face: int, level: int, i: int, j: int) -> None:
        if not _bbox_intersects(tile_latlon_bounds(face, level, i, j), box):
            return
        if level >= min_level:
            by_level[level].append((face, level, i, j))
        if level < max_level:
            for cj in (2 * j, 2 * j + 1):
                for ci in (2 * i, 2 * i + 1):
                    descend(face, level + 1, ci, cj)

    for face in faces:
        descend(face, 0, 0, 0)

    out = []
    for level in range(min_level, max_level + 1):
        out.extend(sorted(by_level[level], key=lambda k: (k[0], k[3], k[2])))
    return out


# ---------------------------------------------------------------------------
# GDAL sampling (worker side)
# ---------------------------------------------------------------------------

# Per-worker globals (avoid pickling gdal.Dataset).
_g_dem = None
_g_lc = None
_g_bathy = None
_g_common = None


# Buffer resolution kept per output sample: the decimated read stays at least this many buffer
# pixels between adjacent output samples, so bilinear interpolation still has real detail to work
# with. 2.0 = classic 2x oversampling.
_READ_OVERSAMPLE = 2.0


def _plan_read(gt, width: int, height: int,
               lat: "np.ndarray", lon: "np.ndarray") -> tuple:
    """Plan the source read for sampling (lat, lon): (y0, y1, buf_x, buf_y).

    The native rows [y0, y1) are read across the full width into a (buf_y, buf_x) buffer, letting
    GDAL decimate on read. This is the #1217 fix: a coarse tile's latitude strip is O(raster) at
    native resolution — a level-0 tile against GEBCO_2024 read half the globe at full resolution
    (14.9 GB as float64, times os.cpu_count() workers) to produce 129x129 samples. The buffer is
    sized from the FINEST spacing between adjacent output samples in source-pixel space (Euclidean,
    so a warped lattice can't alias an axis to zero), keeping _READ_OVERSAMPLE buffer pixels per
    sample step; memory becomes O(tile), not O(raster). Never upsamples: buf_x <= width,
    buf_y <= rows, and a tile already at source resolution reads exactly what it read before.

    Pure numpy — no GDAL, unit-tested.
    """
    fx = (lon - gt[0]) / gt[1]
    fy = (lat - gt[3]) / gt[5]
    y0 = int(max(0, math.floor(np.min(fy)) - 1))
    y1 = int(min(height, math.ceil(np.max(fy)) + 2))
    if y1 <= y0:
        y0, y1 = 0, min(height, 1)
    rows = y1 - y0

    # Finest adjacent-sample displacement in source pixels, across both lattice axes. Longitude
    # wraps, so column differences are modular.
    min_step = math.inf
    for axis in (0, 1):
        dfx = np.abs((np.diff(fx, axis=axis) + width / 2.0) % width - width / 2.0)
        dfy = np.abs(np.diff(fy, axis=axis))
        step = np.sqrt(dfx * dfx + dfy * dfy)
        if step.size:
            min_step = min(min_step, float(step.min()))
    if not math.isfinite(min_step):
        min_step = 1.0

    factor = max(1, int(min_step / _READ_OVERSAMPLE))
    buf_x = max(1, width // factor)
    buf_y = max(1, rows // factor)
    return y0, y1, buf_x, buf_y


def _worst_read_bytes(rasters, tiles, tile_px: int, sample_count: int = 24) -> int:
    """Largest single read (bytes, float32) any of the first `sample_count` tiles will make against
    any configured raster. The enumeration is coarse-first, so the head of the list holds the most
    expensive reads of the whole run — the level-0 tiles that all started together are exactly what
    took a 62 GB machine down (#1217). `rasters` is a list of (gt, width, height) triples.

    Pure numpy — no GDAL, unit-tested.
    """
    worst = 0
    for face, level, i, j in tiles[:sample_count]:
        lat, lon = tile_latlon_grid(face, level, i, j, tile_px)
        for gt, width, height in rasters:
            _y0, _y1, buf_x, buf_y = _plan_read(gt, width, height, lat, lon)
            worst = max(worst, buf_x * buf_y * 4)
    return worst


def _available_memory_bytes():
    """Available physical memory in bytes, or None where the platform does not say (Windows has no
    os.sysconf; the worker default then falls back to cpu_count alone)."""
    try:
        return os.sysconf("SC_AVPHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")
    except (AttributeError, ValueError, OSError):
        return None


def _default_workers(cpu: int, avail_bytes, worst_read_bytes: int) -> int:
    """The default worker count, bounded by memory as well as cores (#1217): each worker's peak is
    roughly one strip read, so admit only as many as fit with 2x headroom. With the decimated read
    the bound rarely binds — it is the backstop that turns a future variant of the O(raster) read
    into a slow build instead of an outage.

    Pure — unit-tested.
    """
    if not avail_bytes or worst_read_bytes <= 0:
        return max(1, cpu)
    fit = int(avail_bytes // (2 * worst_read_bytes))
    return max(1, min(cpu, fit))


def _sample_raster(band, gt, width: int, height: int, nodata,
                   lat: "np.ndarray", lon: "np.ndarray", nearest: bool) -> "np.ndarray":
    """Sample a north-up EPSG:4326 raster at (lat, lon) arrays.

    Reads only the covering latitude band across the full width (so pole tiles that span all
    longitudes and antimeridian-crossing tiles are handled uniformly), decimated on read to the
    resolution the tile can actually use (_plan_read, #1217), then interpolates — bilinear for
    height, nearest for land-cover class. Longitude wraps modulo the buffer width; latitude clamps.
    NaN nodata is preserved for the caller to map to a sentinel. The decimating read uses GDAL's
    default nearest resampling, so nodata values arrive exact, never averaged into real data.
    """
    y0, y1, buf_x, buf_y = _plan_read(gt, width, height, lat, lon)
    band_arr = band.ReadAsArray(0, y0, width, y1 - y0,
                                buf_xsize=buf_x, buf_ysize=buf_y).astype(np.float32)
    if nodata is not None and not math.isnan(nodata):
        band_arr[band_arr == nodata] = np.nan

    # Fractional coords in the (possibly decimated) buffer. North-up 4326: gt[1] > 0 (deg/px lon),
    # gt[5] < 0 (lat). Pixel-centre aligned: buffer pixel k covers native [k/s, (k+1)/s).
    sx = buf_x / float(width)
    sy = buf_y / float(y1 - y0)
    fx = ((lon - gt[0]) / gt[1] + 0.5) * sx - 0.5   # fractional buffer column (may wrap)
    ry = (((lat - gt[3]) / gt[5]) - y0 + 0.5) * sy - 0.5

    if nearest:
        xi = np.mod(np.rint(fx).astype(np.int64), buf_x)
        yi = np.clip(np.rint(ry).astype(np.int64), 0, band_arr.shape[0] - 1)
        return band_arr[yi, xi]

    x0 = np.floor(fx)
    wx = fx - x0
    xi0 = np.mod(x0.astype(np.int64), buf_x)
    xi1 = np.mod(xi0 + 1, buf_x)
    ry_floor = np.floor(ry)
    wy = np.clip(ry - ry_floor, 0.0, 1.0)
    yi0 = np.clip(ry_floor.astype(np.int64), 0, band_arr.shape[0] - 1)
    yi1 = np.clip(yi0 + 1, 0, band_arr.shape[0] - 1)
    top = band_arr[yi0, xi0] * (1.0 - wx) + band_arr[yi0, xi1] * wx
    bot = band_arr[yi1, xi0] * (1.0 - wx) + band_arr[yi1, xi1] * wx
    return top * (1.0 - wy) + bot * wy


def write_png_u16(path: Path, data_u16: "np.ndarray") -> None:
    """Write a 2-D uint16 array as a 16-bit grayscale PNG via GDAL.

    Public because gen_terrain_chunks.py writes the same format (#1265); it had a byte-identical
    copy, down to the forward-slash path rewrite GDAL needs on Windows.
    """
    h, w = data_u16.shape
    mem = gdal.GetDriverByName("MEM").Create("", w, h, 1, gdal.GDT_UInt16)
    mem.GetRasterBand(1).WriteArray(data_u16)
    gdal.GetDriverByName("PNG").CreateCopy(str(path).replace("\\", "/"), mem)
    mem = None


def _worker_init(dem_path: str, lc_path, bathy_path, common: tuple) -> None:
    global _g_dem, _g_lc, _g_bathy, _g_common
    gdal.UseExceptions()
    _g_dem = gdal.Open(dem_path, gdal.GA_ReadOnly)
    _g_lc = gdal.Open(lc_path, gdal.GA_ReadOnly) if lc_path else None
    _g_bathy = gdal.Open(bathy_path, gdal.GA_ReadOnly) if bathy_path else None
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
        # Bathymetry (#476): fill the land DEM's ocean/nodata gaps with GEBCO sea-floor depth.
        if _g_bathy is not None:
            b_band = _g_bathy.GetRasterBand(1)
            bathy = _sample_raster(b_band, _g_bathy.GetGeoTransform(), _g_bathy.RasterXSize,
                                   _g_bathy.RasterYSize, b_band.GetNoDataValue(), lat, lon, nearest=False)
            elev = merge_bathymetry(elev, bathy)
        height_path.parent.mkdir(parents=True, exist_ok=True)
        write_png_u16(height_path, encode_heights(elev, height_scale, height_offset))

        if _g_lc is not None:
            lc_band = _g_lc.GetRasterBand(1)
            lc_nodata = lc_band.GetNoDataValue()
            cover = _sample_raster(lc_band, _g_lc.GetGeoTransform(), _g_lc.RasterXSize,
                                   _g_lc.RasterYSize, lc_nodata, lat, lon, nearest=True)
            cover = np.nan_to_num(cover, nan=0.0)
            lc_u16 = np.clip(cover, 0, 65535).astype(np.uint16)
            write_png_u16(Path(output_dir) / tile_rel_path(terrain_id, face, level, i, j, "landcover"),
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
    p.add_argument("--bbox", nargs=4, type=float, default=None,
                   metavar=("LATMIN", "LONMIN", "LATMAX", "LONMAX"),
                   help="Restrict output to the tiles covering this lat/lon box (degrees), the same "
                        "scoping gen_terrain_color.py takes. Without it the run covers the FULL "
                        "faces — 4^level tiles each, which is a global build. Composes with "
                        "--skip-existing for a resumable theater build")
    p.add_argument("--landcover-source", default=None, metavar="PATH",
                   help="Optional global land-cover source; also emits _lc.png tiles")
    p.add_argument("--bathymetry-source", default=None, metavar="PATH",
                   help="Optional global bathymetry source (e.g. GEBCO); fills the DEM's ocean/nodata "
                        "gaps with real negative sea-floor depth (#476)")
    p.add_argument("--height-scale", type=float, default=1.0, metavar="FLOAT",
                   help="Multiply elevation before uint16 encode (default: 1.0)")
    p.add_argument("--height-offset", type=float, default=32768.0, metavar="FLOAT",
                   help="Add to elevation after scale before uint16 encode (default: 32768)")
    p.add_argument("--tile-pixels", type=int, default=TILE_PIXELS, metavar="N",
                   help=f"Tile edge in pixels (default: {TILE_PIXELS} = kTileHeightmapSize)")
    p.add_argument("--workers", type=int, default=None, metavar="N",
                   help="Parallel worker processes (default: os.cpu_count(), bounded by available "
                        "memory so the coarse levels cannot exhaust the machine, #1217)")
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
    if args.bathymetry_source and not Path(args.bathymetry_source).exists():
        sys.exit(f"Error: bathymetry source not found: {args.bathymetry_source}")
    if args.min_level < 0 or args.max_level < args.min_level:
        sys.exit("Error: require 0 <= --min-level <= --max-level")
    if args.max_level > 20:
        sys.exit("Error: --max-level > 20 would generate an astronomical tile count")
    if args.tile_pixels < 2:
        sys.exit("Error: --tile-pixels must be >= 2")
    if args.bbox is not None:
        lat_min, lon_min, lat_max, lon_max = args.bbox
        if not (-90.0 <= lat_min <= lat_max <= 90.0):
            sys.exit("Error: --bbox requires -90 <= LATMIN <= LATMAX <= 90")
        if not (-180.0 <= lon_min <= lon_max <= 180.0):
            sys.exit("Error: --bbox requires -180 <= LONMIN <= LONMAX <= 180 "
                     "(a box crossing the antimeridian must be built as two runs)")


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

    bbox = tuple(args.bbox) if args.bbox is not None else None
    tiles = list(enumerate_tiles(faces, args.min_level, args.max_level, bbox))
    total = len(tiles)
    if total == 0:
        sys.exit("Error: no tiles selected — check --bbox against --faces "
                 "(a bbox outside the selected faces produces nothing)")
    dem_path = str(Path(args.input).resolve())
    lc_path = str(Path(args.landcover_source).resolve()) if args.landcover_source else None
    bathy_path = str(Path(args.bathymetry_source).resolve()) if args.bathymetry_source else None

    workers = args.workers
    if workers is None:
        cpu = os.cpu_count() or 1
        rasters = []
        for path in (dem_path, lc_path, bathy_path):
            if path:
                ds = gdal.Open(path, gdal.GA_ReadOnly)
                rasters.append((ds.GetGeoTransform(), ds.RasterXSize, ds.RasterYSize))
                ds = None
        worst = _worst_read_bytes(rasters, tiles, args.tile_pixels)
        avail = _available_memory_bytes()
        workers = _default_workers(cpu, avail, worst)
        if workers < cpu:
            print(f"  workers bounded by memory: {workers} of {cpu} cores — worst read "
                  f"~{worst / 2**30:.2f} GiB/worker against {avail / 2**30:.2f} GiB available "
                  f"(--workers overrides)")
    common = (str(args.output_dir), args.terrain_id, args.tile_pixels,
              args.height_scale, args.height_offset, args.skip_existing)

    scope = (f"bbox lat {bbox[0]}..{bbox[2]}, lon {bbox[1]}..{bbox[3]}" if bbox
             else f"full faces {faces}")
    print(f"[1/2] Generating {total} tile(s) across {scope}, "
          f"levels {args.min_level}..{args.max_level} ({workers} worker(s))"
          f"{' + land cover' if lc_path else ''}{' + bathymetry' if bathy_path else ''}")

    done = 0
    failures = 0
    report_every = max(1, total // 200)
    with ProcessPoolExecutor(max_workers=workers, initializer=_worker_init,
                             initargs=(dem_path, lc_path, bathy_path, common)) as pool:
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
