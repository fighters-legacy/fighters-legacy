# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/gen_terrain_tiles.py — pure-Python logic, no GDAL required."""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np
import pytest

from conftest import load_tool

# Load the module without installing it as a package. The guarded GDAL import means this
# succeeds even when GDAL is absent (gdal=None, _HAS_GDAL=False).
_mod = load_tool("gen_terrain_tiles", "tools", "gen_terrain_tiles.py")

face_uv_to_direction = _mod.face_uv_to_direction
direction_to_latlon = _mod.direction_to_latlon
tile_latlon_grid = _mod.tile_latlon_grid
encode_heights = _mod.encode_heights
merge_bathymetry = _mod.merge_bathymetry
tile_rel_path = _mod.tile_rel_path
enumerate_tiles = _mod.enumerate_tiles
bbox_tiles = _mod.bbox_tiles
tile_latlon_bounds = _mod.tile_latlon_bounds
_parse_faces = _mod._parse_faces
_plan_read = _mod._plan_read
_sample_raster = _mod._sample_raster
_worst_read_bytes = _mod._worst_read_bytes
_default_workers = _mod._default_workers
TILE_PIXELS = _mod.TILE_PIXELS
TERRAIN_ID_RE = _mod.TERRAIN_ID_RE


# ---------------------------------------------------------------------------
# Cube-sphere math (ported from CubeSphere.h) — face centres map to known points
# ---------------------------------------------------------------------------


class TestCubeSphereMath:
    def _center_latlon(self, face):
        x, y, z = face_uv_to_direction(face, np.array([0.5]), np.array([0.5]))
        lat, lon = direction_to_latlon(x, y, z)
        return float(lat[0]), float(lon[0])

    def test_plus_y_face_centre_is_north_pole(self):
        lat, _ = self._center_latlon(2)  # +Y
        assert lat == pytest.approx(90.0, abs=1e-9)

    def test_minus_y_face_centre_is_south_pole(self):
        lat, _ = self._center_latlon(3)  # -Y
        assert lat == pytest.approx(-90.0, abs=1e-9)

    def test_plus_z_face_centre_is_lon_zero_equator(self):
        lat, lon = self._center_latlon(4)  # +Z
        assert lat == pytest.approx(0.0, abs=1e-9)
        assert lon == pytest.approx(0.0, abs=1e-9)

    def test_plus_x_face_centre_is_lon_ninety_equator(self):
        lat, lon = self._center_latlon(0)  # +X
        assert lat == pytest.approx(0.0, abs=1e-9)
        assert lon == pytest.approx(90.0, abs=1e-9)

    def test_minus_x_face_centre_is_lon_minus_ninety(self):
        lat, lon = self._center_latlon(1)  # -X
        assert lat == pytest.approx(0.0, abs=1e-9)
        assert abs(lon) == pytest.approx(90.0, abs=1e-9)

    def test_directions_are_unit_length(self):
        u = np.array([0.0, 0.25, 0.5, 0.75, 1.0])
        for face in range(6):
            x, y, z = face_uv_to_direction(face, u, u)
            assert np.allclose(np.sqrt(x * x + y * y + z * z), 1.0, atol=1e-12)

    def test_bad_face_raises(self):
        with pytest.raises(ValueError):
            face_uv_to_direction(6, np.array([0.5]), np.array([0.5]))


# ---------------------------------------------------------------------------
# Tile lattice grid
# ---------------------------------------------------------------------------


class TestTileLatLonGrid:
    def test_shape_matches_tile_pixels(self):
        lat, lon = tile_latlon_grid(2, 0, 0, 0, tile_px=TILE_PIXELS)
        assert lat.shape == (TILE_PIXELS, TILE_PIXELS)
        assert lon.shape == (TILE_PIXELS, TILE_PIXELS)

    def test_level0_plus_y_tile_reaches_the_pole_at_its_centre(self):
        # Level-0 +Y tile is the whole face; its centre sample is the north pole.
        lat, _ = tile_latlon_grid(2, 0, 0, 0, tile_px=TILE_PIXELS)
        mid = TILE_PIXELS // 2
        assert lat[mid, mid] == pytest.approx(90.0, abs=1e-6)

    def test_latitudes_within_range(self):
        for face in range(6):
            lat, lon = tile_latlon_grid(face, 1, 0, 0)
            assert lat.min() >= -90.0 - 1e-9 and lat.max() <= 90.0 + 1e-9
            assert lon.min() >= -180.0 - 1e-9 and lon.max() <= 180.0 + 1e-9

    def test_child_tiles_partition_parent_columns(self):
        # A level-1 tile's u-range is half the parent's; the (i=0) and (i=1) children of a
        # +Z face abut at u=0.5 -> lon 0. Sample the shared edge column.
        _, lon_left = tile_latlon_grid(4, 1, 0, 0)
        _, lon_right = tile_latlon_grid(4, 1, 1, 0)
        # left tile's last column == right tile's first column (shared edge, lon continuous).
        assert np.allclose(lon_left[:, -1], lon_right[:, 0], atol=1e-9)


# ---------------------------------------------------------------------------
# Height encoding (matches generateProceduralTile: elev + 32768)
# ---------------------------------------------------------------------------


class TestHeightEncoding:
    def test_sea_level_maps_to_offset(self):
        out = encode_heights(np.array([[0.0]]), 1.0, 32768.0)
        assert out[0, 0] == 32768

    def test_known_elevations(self):
        out = encode_heights(np.array([[8849.0, -430.0]]), 1.0, 32768.0)
        assert out[0, 0] == 41617   # Everest
        assert out[0, 1] == 32338   # Dead Sea

    def test_clamps_extremes(self):
        out = encode_heights(np.array([[-40000.0, 40000.0]]), 1.0, 32768.0)
        assert out[0, 0] == 0
        assert out[0, 1] == 65535

    def test_nan_encodes_to_sea_level(self):
        out = encode_heights(np.array([[float("nan")]]), 1.0, 32768.0)
        assert out[0, 0] == 32768

    def test_scale_applied(self):
        out = encode_heights(np.array([[100.0]]), 2.0, 0.0)
        assert out[0, 0] == 200

    def test_output_dtype_is_uint16(self):
        out = encode_heights(np.array([[550.0]]), 1.0, 32768.0)
        assert out.dtype == np.uint16
        assert out[0, 0] == 33318   # 550 + 32768, matches the streamer's pack-tile test


# ---------------------------------------------------------------------------
# Path convention (must match FolderContentPack::resolveTilePath)
# ---------------------------------------------------------------------------


class TestTilePath:
    def test_height_path(self):
        assert tile_rel_path("world", 2, 1, 3, 7, "height") == "terrain/world/f2/l1/tile_3_7.png"

    def test_landcover_path(self):
        assert tile_rel_path("world", 0, 3, 4, 5, "landcover") == "terrain/world/f0/l3/tile_4_5_lc.png"

    def test_satellite_path(self):
        assert tile_rel_path("world", 0, 3, 4, 5, "satellite") == "terrain/world/f0/l3/tile_4_5_sat.ktx2"


# ---------------------------------------------------------------------------
# Tile enumeration + face parsing
# ---------------------------------------------------------------------------


class TestEnumerateTiles:
    def test_counts_per_level(self):
        tiles = list(enumerate_tiles([0, 1, 2, 3, 4, 5], 0, 1))
        # level 0: 6 faces x 1; level 1: 6 faces x 4 = 24; total 30.
        assert len(tiles) == 6 + 24

    def test_single_face_single_level(self):
        tiles = list(enumerate_tiles([2], 2, 2))
        n = 1 << 2
        assert len(tiles) == n * n
        assert all(f == 2 and level == 2 for (f, level, _i, _j) in tiles)
        assert (2, 2, 3, 3) in tiles


ALL_FACES = [0, 1, 2, 3, 4, 5]


def _lattice_overlaps(face, level, i, j, bbox, tile_px=17):
    """Does the tile's raw (unpadded) sample lattice meet the bbox? The reference the descent must
    never miss — bbox_tiles is allowed to return MORE than this, never less."""
    lat_min, lon_min, lat_max, lon_max = bbox
    lat, lon = tile_latlon_grid(face, level, i, j, tile_px)
    if float(lat.max()) < lat_min or float(lat.min()) > lat_max:
        return False
    if float(lon.max()) < lon_min or float(lon.min()) > lon_max:
        return False
    return True


class TestBboxTiles:
    """bbox-limited enumeration (#1107). The contract is a conservative SUPERSET found by
    descending the quadtree — never a filter over the full 4^level range."""

    BOX = (30.0, 32.0, 38.0, 42.0)  # an 8 x 10 degree theater

    def test_selects_a_small_subset_of_the_full_enumeration(self):
        scoped = bbox_tiles(ALL_FACES, 0, 5, self.BOX)
        full = list(enumerate_tiles(ALL_FACES, 0, 5))
        assert 0 < len(scoped) < len(full) / 10

    def test_misses_nothing_a_brute_force_filter_would_find(self):
        # The property that makes pruning safe: a parent whose padded bounds miss the box cannot
        # have a descendant that hits it. Checked against the exhaustive filter at a level small
        # enough to enumerate.
        for max_level in (3, 5, 6):
            got = set(bbox_tiles(ALL_FACES, 0, max_level, self.BOX))
            for key in enumerate_tiles(ALL_FACES, 0, max_level):
                if _lattice_overlaps(*key, self.BOX):
                    assert key in got, f"{key} overlaps the bbox but was pruned"

    def test_over_inclusion_stays_marginal(self):
        # The pad admits a few near-miss tiles. That is the safe direction, but it should be a
        # rounding effect, not a doubling — a bbox option that quietly returns half the globe is
        # the problem it was added to solve.
        got = bbox_tiles(ALL_FACES, 0, 6, self.BOX)
        overlapping = [k for k in got if _lattice_overlaps(*k, self.BOX)]
        assert len(got) <= len(overlapping) * 1.5 + 6

    def test_a_global_bbox_returns_the_full_enumeration(self):
        assert set(bbox_tiles(ALL_FACES, 0, 3, (-90.0, -180.0, 90.0, 180.0))) == \
            set(enumerate_tiles(ALL_FACES, 0, 3))

    def test_min_level_is_descended_through_but_not_emitted(self):
        # Levels below min_level are the path to the wanted tiles; they must not appear in output.
        got = bbox_tiles(ALL_FACES, 4, 5, self.BOX)
        assert got, "descending from level 0 must still reach level 4"
        assert {level for (_f, level, _i, _j) in got} == {4, 5}

    def test_deep_levels_are_reachable_without_enumerating_the_face(self):
        # The point of the issue: level 12 is 16.7 M tiles per face. A descent visits only the
        # covering subtree, so this returns in seconds instead of never.
        got = bbox_tiles([0, 4], 12, 12, (35.0, 36.0, 35.2, 36.2))
        assert 0 < len(got) < 20000

    def test_output_is_coarse_first_and_matches_the_unfiltered_ordering(self):
        got = bbox_tiles(ALL_FACES, 0, 4, self.BOX)
        levels = [level for (_f, level, _i, _j) in got]
        assert levels == sorted(levels)  # coarse-first, so --skip-existing resumes identically
        order = {key: n for n, key in enumerate(enumerate_tiles(ALL_FACES, 0, 4))}
        for level in set(levels):
            at_level = [order[k] for k in got if k[1] == level]
            assert at_level == sorted(at_level)

    def test_enumerate_tiles_delegates_when_given_a_bbox(self):
        assert list(enumerate_tiles(ALL_FACES, 0, 3, self.BOX)) == bbox_tiles(ALL_FACES, 0, 3, self.BOX)
        # No bbox = the full faces, unchanged.
        assert list(enumerate_tiles(ALL_FACES, 0, 2, None)) == list(enumerate_tiles(ALL_FACES, 0, 2))

    def test_reversed_bounds_are_rejected(self):
        with pytest.raises(ValueError):
            bbox_tiles(ALL_FACES, 0, 2, (38.0, 32.0, 30.0, 42.0))

    def test_bounds_are_a_superset_of_the_sampled_lattice(self):
        lat_min, lon_min, lat_max, lon_max = tile_latlon_bounds(4, 3, 3, 4)
        lat, lon = tile_latlon_grid(4, 3, 3, 4, 17)
        assert lat_min <= float(lat.min()) and lat_max >= float(lat.max())
        assert lon_min <= float(lon.min()) and lon_max >= float(lon.max())


class TestParseFaces:
    def test_all(self):
        assert _parse_faces("all") == [0, 1, 2, 3, 4, 5]

    def test_subset(self):
        assert _parse_faces("0,2,4") == [0, 2, 4]

    def test_out_of_range_raises(self):
        with pytest.raises(ValueError):
            _parse_faces("7")

    def test_empty_raises(self):
        with pytest.raises(ValueError):
            _parse_faces("")


class TestTerrainIdRegex:
    @pytest.mark.parametrize("tid", ["world", "fa-korea", "theater_01", "a"])
    def test_accepts_valid(self, tid):
        assert TERRAIN_ID_RE.match(tid)

    @pytest.mark.parametrize("tid", ["World", "1world", "-world", "wörld", ""])
    def test_rejects_invalid(self, tid):
        assert not TERRAIN_ID_RE.match(tid)


class TestMergeBathymetry:
    """merge_bathymetry fills a land DEM's ocean/nodata gaps with GEBCO depth (#476)."""

    def test_ocean_gaps_filled_with_bathymetry(self):
        nan = float("nan")
        dem = np.array([[100.0, nan], [nan, 50.0]])       # land DEM, nodata over water
        bathy = np.array([[-5.0, -1200.0], [-30.0, -8.0]]) # GEBCO everywhere
        out = merge_bathymetry(dem, bathy)
        # Land cells keep the DEM; ocean/nodata cells take the (negative) bathymetry.
        assert out[0, 0] == 100.0
        assert out[1, 1] == 50.0
        assert out[0, 1] == -1200.0
        assert out[1, 0] == -30.0

    def test_land_is_never_overwritten(self):
        dem = np.array([[10.0, 20.0]])
        bathy = np.array([[-99.0, -99.0]])
        out = merge_bathymetry(dem, bathy)
        assert list(out[0]) == [10.0, 20.0]

    def test_gap_without_bathymetry_stays_nan(self):
        nan = float("nan")
        dem = np.array([[nan]])
        bathy = np.array([[nan]])
        out = merge_bathymetry(dem, bathy)
        assert math.isnan(out[0, 0])  # still nodata -> encode_heights maps to sea level

    def test_does_not_mutate_input(self):
        dem = np.array([[float("nan")]])
        bathy = np.array([[-42.0]])
        merge_bathymetry(dem, bathy)
        assert math.isnan(dem[0, 0])  # original DEM untouched

    def test_merged_ocean_encodes_below_datum(self):
        # End-to-end with encode_heights: a −1000 m ocean cell encodes below the 32768 datum.
        nan = float("nan")
        merged = merge_bathymetry(np.array([[nan]]), np.array([[-1000.0]]))
        enc = encode_heights(merged, 1.0, 32768.0)
        assert int(enc[0, 0]) == 31768


# ---------------------------------------------------------------------------
# Decimated reads + memory-bounded workers (#1217)
# ---------------------------------------------------------------------------

# GEBCO_2024-shaped raster: 86,400 x 43,200, 15 arc-second global grid.
_GEBCO_GT = (-180.0, 1.0 / 240.0, 0.0, 90.0, 0.0, -1.0 / 240.0)
_GEBCO_W = 86400
_GEBCO_H = 43200


class TestPlanRead:
    """_plan_read keeps a read O(tile), not O(raster) — the #1217 outage was a level-0 tile
    reading half the globe at native resolution (14.9 GB) to produce 129x129 samples, times
    os.cpu_count() workers."""

    def _plan_for(self, face, level, i, j):
        lat, lon = tile_latlon_grid(face, level, i, j, TILE_PIXELS)
        return _plan_read(_GEBCO_GT, _GEBCO_W, _GEBCO_H, lat, lon)

    def test_level0_read_is_tile_sized_not_raster_sized(self):
        # Before the fix this tile's strip was ~21,600 rows x 86,400 cols as float64 = 14.9 GB.
        y0, y1, buf_x, buf_y = self._plan_for(4, 0, 0, 0)
        native_bytes = (y1 - y0) * _GEBCO_W * 8
        buf_bytes = buf_x * buf_y * 4
        assert native_bytes > 10 * 2**30      # the read this replaces really was ~15 GB
        assert buf_bytes < 32 * 2**20         # and what is read now is megabytes
        # Still enough resolution: at least ~2 buffer px between adjacent output samples.
        assert buf_x >= 2 * TILE_PIXELS
        assert buf_y >= 2 * TILE_PIXELS // 2

    def test_pole_tile_is_also_bounded(self):
        # Face 2's tile spans every longitude; the full-width read must still decimate.
        _y0, _y1, buf_x, buf_y = self._plan_for(2, 0, 0, 0)
        assert buf_x * buf_y * 4 < 32 * 2**20

    def test_every_level_is_bounded(self):
        # The peak sits at the decimation boundary (level 6 against GEBCO: full native width,
        # ~157 MiB as float32), NOT at level 0 — coarser tiles decimate harder. Nothing anywhere
        # in the pyramid may approach the old level-0 gigabytes.
        for level in range(9):
            _y0, _y1, buf_x, buf_y = self._plan_for(4, level, 0, 0)
            assert buf_x * buf_y * 4 < 256 * 2**20, f"level {level}"

    def test_never_upsamples(self):
        for level in (0, 3, 8, 12):
            y0, y1, buf_x, buf_y = self._plan_for(4, level, 0, 0)
            assert buf_x <= _GEBCO_W
            assert buf_y <= (y1 - y0)

    def test_fine_tile_reads_native_resolution(self):
        # A level-12 tile's sample spacing is well under a source pixel: no decimation, exactly
        # the pre-#1217 read window.
        n = 1 << 12
        y0, y1, buf_x, buf_y = self._plan_for(4, 12, n // 2, n // 2)
        assert buf_x == _GEBCO_W
        assert buf_y == y1 - y0


class _FakeBand:
    """A GDAL band double: ReadAsArray over a numpy array, with GDAL's default nearest-neighbour
    decimation when buf_xsize/buf_ysize are passed. Lets the sampling path run without GDAL."""

    def __init__(self, arr):
        self.arr = arr

    def ReadAsArray(self, x0, y0, w, h, buf_xsize=None, buf_ysize=None):
        block = self.arr[y0:y0 + h, x0:x0 + w]
        if buf_xsize is None:
            return block
        yi = np.clip(np.round((np.arange(buf_ysize) + 0.5) * h / buf_ysize - 0.5).astype(int), 0, h - 1)
        xi = np.clip(np.round((np.arange(buf_xsize) + 0.5) * w / buf_xsize - 0.5).astype(int), 0, w - 1)
        return block[np.ix_(yi, xi)]


class TestSampleRasterDecimated:
    """The decimated read must return the same terrain the full-resolution read returned."""

    # 0.1 deg/px synthetic globe: small enough to hold, coarse enough that a level-1 tile
    # decimates (its sample spacing is several source pixels).
    _W, _H = 3600, 1800
    _GT = (-180.0, 0.1, 0.0, 90.0, 0.0, -0.1)

    def _lat_value_raster(self):
        # value == latitude of the pixel centre, so the expected sample value is analytic.
        rows = 90.0 - (np.arange(self._H) + 0.5) * 0.1
        return np.repeat(rows[:, None], self._W, axis=1)

    def test_coarse_tile_matches_the_analytic_field(self):
        band = _FakeBand(self._lat_value_raster())
        lat, lon = tile_latlon_grid(4, 0, 0, 0, TILE_PIXELS)
        _y0, _y1, buf_x, _buf_y = _plan_read(self._GT, self._W, self._H, lat, lon)
        assert buf_x < self._W  # the case actually decimates, or this test asserts nothing
        out = _mod._sample_raster(band, self._GT, self._W, self._H, None, lat, lon, nearest=False)
        # Bilinear over a nearest-decimated grid: error bounded by ~one decimated pixel (0.1 deg
        # native, factor 2 -> ~0.3 deg worst case).
        assert float(np.nanmax(np.abs(out - lat))) < 0.5

    def test_fine_tile_is_bit_identical_to_the_native_read(self):
        # A tile whose sample spacing is sub-pixel decimates by 1: the new path must reproduce
        # the old read exactly (same window, same interpolation).
        band = _FakeBand(self._lat_value_raster())
        lat, lon = tile_latlon_grid(4, 6, 31, 31, TILE_PIXELS)
        y0, y1, buf_x, buf_y = _plan_read(self._GT, self._W, self._H, lat, lon)
        assert buf_x == self._W and buf_y == y1 - y0
        out = _mod._sample_raster(band, self._GT, self._W, self._H, None, lat, lon, nearest=False)
        assert float(np.nanmax(np.abs(out - lat))) < 0.1  # within one native pixel of analytic

    def test_nearest_mode_returns_source_values(self):
        # Land-cover classes must arrive as exact source values, never blended.
        arr = np.full((self._H, self._W), 7.0)
        arr[: self._H // 2] = 3.0
        band = _FakeBand(arr)
        lat, lon = tile_latlon_grid(4, 1, 0, 0, TILE_PIXELS)
        out = _mod._sample_raster(band, self._GT, self._W, self._H, None, lat, lon, nearest=True)
        assert set(np.unique(out)) <= {3.0, 7.0}


class TestWorkerBound:
    def test_worst_read_bytes_is_megabytes_after_the_fix(self):
        tiles = list(enumerate_tiles(range(6), 0, 2))
        worst = _worst_read_bytes([(_GEBCO_GT, _GEBCO_W, _GEBCO_H)], tiles, TILE_PIXELS)
        assert 0 < worst < 32 * 2**20

    def test_no_rasters_means_no_bound(self):
        assert _worst_read_bytes([], [(4, 0, 0, 0)], TILE_PIXELS) == 0

    def test_default_workers_unbounded_without_memory_info(self):
        assert _default_workers(24, None, 10 * 2**30) == 24

    def test_default_workers_clamps_to_what_fits(self):
        # 8 GiB free, 1 GiB per read, 2x headroom -> 4 workers, not 24.
        assert _default_workers(24, 8 * 2**30, 1 * 2**30) == 4

    def test_default_workers_never_below_one(self):
        assert _default_workers(24, 1 * 2**20, 10 * 2**30) == 1

    def test_default_workers_caps_at_cpu(self):
        assert _default_workers(4, 64 * 2**30, 1 * 2**20) == 4
