# SPDX-FileCopyrightText: 2026 Fighters Legacy contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure-logic tests for tools/gen_terrain_color.py (#488). No network; GDAL not required."""
import importlib.util
import os

import pytest

np = pytest.importorskip("numpy")

_SPEC = importlib.util.spec_from_file_location(
    "gen_terrain_color", os.path.join(os.path.dirname(__file__), "..", "tools", "gen_terrain_color.py")
)
gtc = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(gtc)


def test_s2_to_srgb_monotonic_and_clamped():
    refl = np.array([[[0, 0, 0], [1500, 1500, 1500], [3000, 3000, 3000], [9000, 9000, 9000]]], dtype=np.float64)
    srgb = gtc.s2_to_srgb(refl, scale=3000.0)
    assert srgb.dtype == np.uint8
    # 0 -> 0, ceiling+ -> 255, monotonic non-decreasing.
    vals = [int(srgb[0, k, 0]) for k in range(4)]
    assert vals[0] == 0
    assert vals[-1] == 255
    assert vals == sorted(vals)
    # Gamma lifts the mid-tone above the linear fraction (1500/3000 = 0.5 -> >0.5*255).
    assert vals[1] > 127


def test_scl_cloud_mask_flags_clouds_not_ground():
    scl = np.array([[4, 5, 8, 9, 3, 10, 11, 0]], dtype=np.uint8)  # 4/5 veg/bare, 11 snow = ground
    mask = gtc.scl_cloud_mask(scl)
    assert mask.tolist() == [[False, False, True, True, True, True, False, True]]


def test_select_scenes_filters_and_sorts_by_cloud():
    feats = [
        {"properties": {"eo:cloud_cover": 40.0}, "id": "a"},
        {"properties": {"eo:cloud_cover": 5.0}, "id": "b"},
        {"properties": {"eo:cloud_cover": 12.0}, "id": "c"},
        {"properties": {"s2:cloudy_pixel_percentage": 8.0}, "id": "d"},  # alt key
    ]
    picked = gtc.select_scenes(feats, max_cloud=20.0)
    ids = [f["id"] for f in picked]
    assert ids == ["b", "d", "c"]  # 5, 8, 12; the 40% scene dropped


def test_tile_rel_path_satellite_matches_engine_convention():
    # Must match FolderContentPack::resolveTilePath / the streamer's Satellite layer.
    assert gtc.tile_rel_path("world", 2, 8, 130, 44, "satellite") == "terrain/world/f2/l8/tile_130_44_sat.ktx2"


def test_bbox_tiles_is_the_shared_implementation():
    # bbox -> tile coverage moved into gen_terrain_tiles (#1107) so both generators run one
    # quadtree descent rather than two filters over the full 4^level enumeration. This module
    # re-exports it; the coverage/pruning contract is tested in tests/test_gen_terrain_tiles.py.
    import gen_terrain_tiles

    assert gtc.bbox_tiles is gen_terrain_tiles.bbox_tiles


def test_bbox_tiles_selects_a_small_non_empty_subset():
    bbox = (0.0, 0.0, 1.0, 1.0)  # a tiny box near the equator
    tiles = gtc.bbox_tiles([0, 1, 2, 3, 4, 5], 0, 3, bbox)
    assert isinstance(tiles, list) and tiles
    # Deliberately a conservative superset (the bounds are padded), so assert it stays a small
    # fraction of the full range rather than an exact set.
    assert len(tiles) < 6 * (1 + 4 + 16 + 64) / 4
    assert all(0 <= face < 6 and 0 <= level <= 3 for (face, level, _i, _j) in tiles)
