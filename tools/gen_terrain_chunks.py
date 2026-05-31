# SPDX-FileCopyrightText: 2026 John McKenzie
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Generate fighters-legacy terrain chunk PNGs from a GeoTIFF elevation dataset.

Reads any GDAL-supported raster (GeoTIFF, VRT, SRTM, Copernicus GLO-30, …),
reprojects to a flat metric CRS (auto-detected UTM zone or --srs override),
slices into 513×513 px chunks (512 intervals + 1 shared overlap pixel), and
writes 16-bit grayscale PNG files at the canonical path:

    terrain/<id>/lod<n>/chunk_<xxxx>_<yyyy>.png

Optionally writes terrain/<id>.json with chunk_size_m, grid_width, grid_height,
and lod_levels for the engine's terrain manifest.

Usage:
    python3 tools/gen_terrain_chunks.py \\
        --input merged.vrt \\
        --terrain-id world \\
        --chunk-size-m 15360 \\
        --grid-width 64 --grid-height 64 \\
        --output-dir mods/fl-base-pack/ \\
        --write-manifest

Height encoding (default):
    uint16 = clamp(elevation_m * height_scale + height_offset, 0, 65535)
    Default offset=32768 centres sea level at 32768; range ±32767 m covers all
    Earth terrain. Nodata / out-of-bounds areas are stored as 0.

LOD downsampling:
    LOD 0: 513×513 (native), LOD 1: 257×257 (stride 2), LOD 2: 129×129 (stride 4),
    LOD 3: 65×65 (stride 8). Strided subsampling preserves exact values and
    edge-pixel alignment for seamless cross-chunk stitching.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

# Guard GDAL import so --help and pytest unit tests work without GDAL installed.
try:
    from osgeo import gdal, osr
    gdal.UseExceptions()
    _HAS_GDAL = True
except ImportError:
    gdal = osr = None  # type: ignore[assignment]
    _HAS_GDAL = False

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

CHUNK_PIXELS = 513          # 512 intervals + 1 shared edge pixel (seamless)
TERRAIN_ID_RE = re.compile(r'^[a-z][a-z0-9_-]*$')
LOD_STRIDES = [1, 2, 4, 8]  # index = LOD level; stride into LOD-0 array

# ---------------------------------------------------------------------------
# Per-worker globals set by _worker_init (avoids pickling large objects)
# ---------------------------------------------------------------------------

_g_src_ds = None   # gdal.Dataset opened once per worker process
_g_common = None   # tuple of common parameters shared across all chunks

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------


def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="gen_terrain_chunks.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--input", required=True, metavar="PATH",
                   help="Input GeoTIFF or VRT (any GDAL-readable raster)")
    p.add_argument("--terrain-id", required=True, metavar="SLUG",
                   help="Terrain identifier — must match ^[a-z][a-z0-9_-]*$")
    p.add_argument("--output-dir", required=True, metavar="DIR",
                   help="Output root (mod directory); chunks land under terrain/<id>/")
    p.add_argument("--chunk-size-m", type=float, default=15360.0, metavar="FLOAT",
                   help="Physical chunk size in metres (default: 15360)")
    p.add_argument("--grid-width", required=True, type=int, metavar="N",
                   help="Grid width in chunks (columns)")
    p.add_argument("--grid-height", required=True, type=int, metavar="N",
                   help="Grid height in chunks (rows)")
    p.add_argument("--origin-x", type=float, default=None, metavar="FLOAT",
                   help="Grid X origin in target CRS metres (default: auto from SW corner)")
    p.add_argument("--origin-y", type=float, default=None, metavar="FLOAT",
                   help="Grid Y origin in target CRS metres (default: auto from SW corner)")
    p.add_argument("--srs", default=None, metavar="SRS",
                   help="Target CRS as EPSG code (e.g. EPSG:32637) or WKT "
                        "(default: auto UTM zone from raster centroid)")
    p.add_argument("--height-scale", type=float, default=1.0, metavar="FLOAT",
                   help="Multiply elevation before uint16 encode (default: 1.0)")
    p.add_argument("--height-offset", type=float, default=32768.0, metavar="FLOAT",
                   help="Add to elevation after scale before uint16 encode (default: 32768)")
    p.add_argument("--lod-levels", type=int, default=3, metavar="N",
                   help="Number of LOD levels to generate: 1–4 (default: 3)")
    p.add_argument("--workers", type=int, default=None, metavar="N",
                   help="Parallel worker processes (default: os.cpu_count())")
    p.add_argument("--skip-existing", action="store_true",
                   help="Skip chunks whose LOD-0 PNG already exists (resume interrupted run)")
    p.add_argument("--write-manifest", action="store_true",
                   help="Write terrain/<id>.json with chunk_size_m, grid dimensions, lod_levels")
    return p.parse_args()


def _validate_args(args: argparse.Namespace) -> None:
    if not TERRAIN_ID_RE.match(args.terrain_id):
        sys.exit(f"Error: --terrain-id '{args.terrain_id}' must match ^[a-z][a-z0-9_-]*$")
    if not Path(args.input).exists():
        sys.exit(f"Error: input file not found: {args.input}")
    if not 1 <= args.lod_levels <= 4:
        sys.exit("Error: --lod-levels must be 1–4")
    if args.grid_width < 1 or args.grid_height < 1:
        sys.exit("Error: --grid-width and --grid-height must be >= 1")
    if args.chunk_size_m <= 0:
        sys.exit("Error: --chunk-size-m must be > 0")
    if (args.origin_x is None) != (args.origin_y is None):
        sys.exit("Error: --origin-x and --origin-y must both be provided or both omitted")

# ---------------------------------------------------------------------------
# CRS helpers
# ---------------------------------------------------------------------------


def _parse_srs(srs_str: str) -> "osr.SpatialReference":
    tgt = osr.SpatialReference()
    upper = srs_str.strip().upper()
    if upper.startswith("EPSG:"):
        code = int(upper.split(":", 1)[1])
        tgt.ImportFromEPSG(code)
    else:
        tgt.ImportFromWkt(srs_str)
    return tgt


def _detect_target_srs(src_ds: "gdal.Dataset") -> "osr.SpatialReference":
    """Auto-select UTM zone from the source raster centroid."""
    src_srs = osr.SpatialReference(wkt=src_ds.GetProjection())
    gt = src_ds.GetGeoTransform()
    cx_src = gt[0] + (src_ds.RasterXSize / 2.0) * gt[1]
    cy_src = gt[3] + (src_ds.RasterYSize / 2.0) * gt[5]

    wgs84 = osr.SpatialReference()
    wgs84.ImportFromEPSG(4326)
    wgs84.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    src_srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    ct = osr.CoordinateTransformation(src_srs, wgs84)
    lon, lat, _ = ct.TransformPoint(cx_src, cy_src)

    zone = int((lon + 180.0) / 6.0) + 1
    epsg = 32600 + zone if lat >= 0 else 32700 + zone
    print(f"  Auto-detected CRS: EPSG:{epsg} (UTM zone {zone}{'N' if lat >= 0 else 'S'})")

    tgt = osr.SpatialReference()
    tgt.ImportFromEPSG(epsg)
    return tgt


def _compute_origin(src_ds: "gdal.Dataset",
                    tgt_srs: "osr.SpatialReference") -> tuple[float, float]:
    """Reproject the SW corner of src_ds to tgt_srs; return (origin_x, origin_y)."""
    gt = src_ds.GetGeoTransform()
    xmin_src = gt[0]
    ymin_src = gt[3] + src_ds.RasterYSize * gt[5]   # gt[5] < 0 for north-up rasters

    src_srs = osr.SpatialReference(wkt=src_ds.GetProjection())
    src_srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    tgt_srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    ct = osr.CoordinateTransformation(src_srs, tgt_srs)
    x_tgt, y_tgt, _ = ct.TransformPoint(xmin_src, ymin_src)
    return x_tgt, y_tgt

# ---------------------------------------------------------------------------
# Chunk processing (called inside each worker)
# ---------------------------------------------------------------------------


def _process_chunk(src_ds: "gdal.Dataset", tgt_srs_wkt: str,
                   cx: int, cy: int,
                   origin_x: float, origin_y: float,
                   chunk_size_m: float,
                   height_scale: float, height_offset: float) -> "np.ndarray":
    """
    Warp a 513×513 bounding box for chunk (cx, cy) from src_ds into the target
    CRS and return a height-encoded uint16 numpy array.
    """
    pixel_size = chunk_size_m / 512.0          # metres per pixel (30 m for Copernicus)
    xmin = origin_x + cx * chunk_size_m
    xmax = xmin + chunk_size_m + pixel_size    # +1 pixel for seamless edge overlap
    ymin = origin_y + cy * chunk_size_m
    ymax = ymin + chunk_size_m + pixel_size

    warped = gdal.Warp(
        "",
        src_ds,
        options=gdal.WarpOptions(
            format="MEM",
            outputBounds=(xmin, ymin, xmax, ymax),
            width=CHUNK_PIXELS,
            height=CHUNK_PIXELS,
            dstSRS=tgt_srs_wkt,
            resampleAlg=gdal.GRA_Bilinear,
            dstNodata=float("nan"),
        ),
    )
    if warped is None:
        raise RuntimeError(f"gdal.Warp returned None for chunk ({cx}, {cy})")

    arr = warped.GetRasterBand(1).ReadAsArray().astype(np.float64)
    arr[np.isnan(arr)] = 0.0          # nodata / out-of-bounds → sea level before encode
    arr = arr * height_scale + height_offset
    np.clip(arr, 0, 65535, out=arr)
    return arr.astype(np.uint16)


def _downsample(arr_u16: "np.ndarray", stride: int) -> "np.ndarray":
    """Return arr[::stride, ::stride] — strided subsampling for LOD generation."""
    return arr_u16[::stride, ::stride]


def _write_png(path: Path, data_u16: "np.ndarray") -> None:
    """Write a 2-D uint16 numpy array as a 16-bit grayscale PNG via GDAL."""
    h, w = data_u16.shape
    mem_ds = gdal.GetDriverByName("MEM").Create("", w, h, 1, gdal.GDT_UInt16)
    mem_ds.GetRasterBand(1).WriteArray(data_u16)
    # GDAL expects forward-slash paths on all platforms including Windows.
    gdal.GetDriverByName("PNG").CreateCopy(str(path).replace("\\", "/"), mem_ds)
    mem_ds = None

# ---------------------------------------------------------------------------
# Directory and manifest helpers
# ---------------------------------------------------------------------------


def _prepare_dirs(output_dir: Path, terrain_id: str, lod_levels: int) -> None:
    for lod in range(lod_levels):
        (output_dir / "terrain" / terrain_id / f"lod{lod}").mkdir(
            parents=True, exist_ok=True)


def _write_manifest(output_dir: Path, terrain_id: str,
                    args: argparse.Namespace) -> None:
    manifest = {
        "chunk_size_m": args.chunk_size_m,
        "grid_width": args.grid_width,
        "grid_height": args.grid_height,
        "lod_levels": args.lod_levels,
    }
    path = output_dir / "terrain" / f"{terrain_id}.json"
    path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"  Wrote terrain/{terrain_id}.json")

# ---------------------------------------------------------------------------
# Worker (must be module-level for pickle; reads _g_src_ds / _g_common globals)
# ---------------------------------------------------------------------------


def _worker_init(src_path: str, tgt_srs_wkt: str,
                 origin_x: float, origin_y: float,
                 chunk_size_m: float, height_scale: float, height_offset: float,
                 output_dir_str: str, terrain_id: str,
                 lod_levels: int, skip_existing: bool) -> None:
    """Called once per worker process; opens the source dataset and caches params."""
    global _g_src_ds, _g_common
    gdal.UseExceptions()
    _g_src_ds = gdal.Open(src_path, gdal.GA_ReadOnly)
    _g_common = (tgt_srs_wkt, origin_x, origin_y, chunk_size_m,
                 height_scale, height_offset, output_dir_str, terrain_id,
                 lod_levels, skip_existing)


def _worker_chunk(cx_cy: tuple[int, int]) -> tuple[int, int, bool]:
    """
    Process one chunk: warp, encode, write all LOD PNGs.
    Returns (cx, cy, ok) — ok=False means warp failed and a zero-fill was written.
    """
    cx, cy = cx_cy
    (tgt_srs_wkt, origin_x, origin_y, chunk_size_m,
     height_scale, height_offset, output_dir_str, terrain_id,
     lod_levels, skip_existing) = _g_common

    lod0_path = (Path(output_dir_str) / "terrain" / terrain_id
                 / "lod0" / f"chunk_{cx:04d}_{cy:04d}.png")
    if skip_existing and lod0_path.exists():
        return cx, cy, True

    try:
        arr_lod0 = _process_chunk(
            _g_src_ds, tgt_srs_wkt, cx, cy,
            origin_x, origin_y, chunk_size_m, height_scale, height_offset,
        )
        ok = True
    except Exception as exc:
        print(f"  Warning: chunk ({cx},{cy}) warp failed: {exc}", file=sys.stderr)
        arr_lod0 = np.zeros((CHUNK_PIXELS, CHUNK_PIXELS), dtype=np.uint16)
        ok = False

    for lod in range(lod_levels):
        arr = _downsample(arr_lod0, LOD_STRIDES[lod])
        path = (Path(output_dir_str) / "terrain" / terrain_id
                / f"lod{lod}" / f"chunk_{cx:04d}_{cy:04d}.png")
        _write_png(path, arr)

    return cx, cy, ok

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> None:
    args = _parse_args()

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

    terrain_id = args.terrain_id
    output_dir = Path(args.output_dir)
    total_chunks = args.grid_width * args.grid_height
    workers = args.workers if args.workers is not None else os.cpu_count()

    print(f"[1/5] Opening source dataset: {args.input}")
    src_ds = gdal.Open(str(args.input), gdal.GA_ReadOnly)
    if src_ds is None:
        sys.exit(f"Error: GDAL could not open '{args.input}'")

    print(f"[2/5] Resolving target CRS")
    if args.srs:
        tgt_srs = _parse_srs(args.srs)
        print(f"  Using --srs: {args.srs}")
    else:
        tgt_srs = _detect_target_srs(src_ds)
    tgt_srs_wkt = tgt_srs.ExportToWkt()

    print("[3/5] Computing grid origin")
    if args.origin_x is not None:
        origin_x, origin_y = args.origin_x, args.origin_y
        print(f"  Using --origin-x/y: ({origin_x:.1f}, {origin_y:.1f})")
    else:
        origin_x, origin_y = _compute_origin(src_ds, tgt_srs)
        print(f"  Auto-detected origin: ({origin_x:.1f}, {origin_y:.1f})")

    print("[4/5] Creating output directories")
    _prepare_dirs(output_dir, terrain_id, args.lod_levels)

    src_path_abs = str(Path(args.input).resolve())  # VRT relative paths break in workers

    initargs = (
        src_path_abs, tgt_srs_wkt, origin_x, origin_y,
        args.chunk_size_m, args.height_scale, args.height_offset,
        str(output_dir), terrain_id, args.lod_levels, args.skip_existing,
    )

    tasks = ((cx, cy)
             for cy in range(args.grid_height)
             for cx in range(args.grid_width))
    chunksize = max(1, total_chunks // (workers * 10))

    print(
        f"[5/5] Generating {total_chunks} chunk(s) "
        f"× {args.lod_levels} LOD(s) = {total_chunks * args.lod_levels} PNG(s) "
        f"({workers} worker{'s' if workers != 1 else ''})"
    )

    done = 0
    failures = 0
    report_every = max(1, total_chunks // 200)

    with ProcessPoolExecutor(
        max_workers=workers,
        initializer=_worker_init,
        initargs=initargs,
    ) as pool:
        for _cx, _cy, ok in pool.map(_worker_chunk, tasks, chunksize=chunksize):
            done += 1
            if not ok:
                failures += 1
            if done % report_every == 0 or done == total_chunks:
                pct = done * 100 // total_chunks
                suffix = f"  [{failures} failed]" if failures else ""
                print(f"  {done}/{total_chunks} ({pct}%){suffix}")

    if args.write_manifest:
        _write_manifest(output_dir, terrain_id, args)

    total_pngs = done * args.lod_levels
    dest = output_dir / "terrain" / terrain_id
    print(f"\nDone. Wrote {total_pngs} PNG file(s) to {dest}/")
    if args.write_manifest:
        print(f"Manifest: {output_dir / 'terrain' / f'{terrain_id}.json'}")
    if failures:
        print(f"WARNING: {failures} chunk(s) failed — see stderr for details.",
              file=sys.stderr)


if __name__ == "__main__":
    main()
