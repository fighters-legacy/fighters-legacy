# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/gen_terrain_tiles.py — pure-Python logic, no GDAL required."""

from __future__ import annotations

import importlib.util
import math
from pathlib import Path

import numpy as np
import pytest

# Load the module without installing it as a package. The guarded GDAL import means this
# succeeds even when GDAL is absent (gdal=None, _HAS_GDAL=False).
_spec = importlib.util.spec_from_file_location(
    "gen_terrain_tiles",
    Path(__file__).parent.parent / "tools" / "gen_terrain_tiles.py",
)
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)

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
