# SPDX-FileCopyrightText: 2026 John McKenzie
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/gen_terrain_chunks.py — pure-Python logic, no GDAL required."""

from __future__ import annotations

import importlib.util
from pathlib import Path

import numpy as np
import pytest

# ---------------------------------------------------------------------------
# Load the module without installing it as a package.
# The guarded GDAL import in gen_terrain_chunks.py means this succeeds even
# when GDAL is not installed (gdal/osr are set to None, _HAS_GDAL=False).
# ---------------------------------------------------------------------------
_spec = importlib.util.spec_from_file_location(
    "gen_terrain_chunks",
    Path(__file__).parent.parent / "tools" / "gen_terrain_chunks.py",
)
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)

_downsample = _mod._downsample
LOD_STRIDES = _mod.LOD_STRIDES
CHUNK_PIXELS = _mod.CHUNK_PIXELS
TERRAIN_ID_RE = _mod.TERRAIN_ID_RE


# ---------------------------------------------------------------------------
# LOD dimensions
# ---------------------------------------------------------------------------


class TestLodDimensions:
    """Strided subsampling must produce the exact pixel counts for each LOD."""

    def _base(self) -> np.ndarray:
        return np.zeros((CHUNK_PIXELS, CHUNK_PIXELS), dtype=np.uint16)

    def test_lod0_identity(self) -> None:
        out = _downsample(self._base(), LOD_STRIDES[0])
        assert out.shape == (513, 513)

    def test_lod1_is_257x257(self) -> None:
        out = _downsample(self._base(), LOD_STRIDES[1])
        assert out.shape == (257, 257)

    def test_lod2_is_129x129(self) -> None:
        out = _downsample(self._base(), LOD_STRIDES[2])
        assert out.shape == (129, 129)

    def test_lod3_is_65x65(self) -> None:
        out = _downsample(self._base(), LOD_STRIDES[3])
        assert out.shape == (65, 65)

    def test_edge_pixels_preserved(self) -> None:
        """First and last pixels (0 and 512) must both appear in every LOD."""
        arr = np.zeros((CHUNK_PIXELS, CHUNK_PIXELS), dtype=np.uint16)
        arr[0, 0] = 1
        arr[512, 512] = 2
        for lod in range(4):
            out = _downsample(arr, LOD_STRIDES[lod])
            assert out[0, 0] == 1, f"LOD{lod}: first pixel lost"
            assert out[-1, -1] == 2, f"LOD{lod}: last pixel lost"

    def test_stride_values(self) -> None:
        assert LOD_STRIDES == [1, 2, 4, 8]


# ---------------------------------------------------------------------------
# Height encoding arithmetic
# ---------------------------------------------------------------------------


class TestHeightEncoding:
    """Encoding: uint16 = clamp(elev * scale + offset, 0, 65535)."""

    @staticmethod
    def _encode(elev_m: float, scale: float = 1.0, offset: float = 32768.0) -> int:
        arr = np.array([[elev_m]], dtype=np.float64)
        arr = arr * scale + offset
        np.clip(arr, 0, 65535, out=arr)
        return int(arr.astype(np.uint16)[0, 0])

    def test_sea_level_default(self) -> None:
        assert self._encode(0.0) == 32768

    def test_everest_no_clip(self) -> None:
        assert self._encode(8849.0) == 32768 + 8849   # 41617

    def test_dead_sea_no_clip(self) -> None:
        assert self._encode(-430.0) == 32768 - 430    # 32338

    def test_extreme_positive_clamps(self) -> None:
        assert self._encode(100_000.0) == 65535

    def test_extreme_negative_clamps(self) -> None:
        assert self._encode(-40_000.0) == 0

    def test_custom_scale(self) -> None:
        # 0.5 scale, 0 offset: 100 m → 50
        assert self._encode(100.0, scale=0.5, offset=0.0) == 50

    def test_nodata_zero_with_default_offset(self) -> None:
        # Nodata path sets float value to 0.0 before encoding.
        # With default offset=32768, result is 32768 (sea level), NOT raw 0.
        assert self._encode(0.0) == 32768

    def test_nodata_zero_with_zero_offset(self) -> None:
        # With offset=0 the user explicitly wants 0 to mean 0.
        assert self._encode(0.0, scale=1.0, offset=0.0) == 0


# ---------------------------------------------------------------------------
# Terrain ID validation
# ---------------------------------------------------------------------------


class TestTerrainIdValidation:
    """Terrain ID regex: ^[a-z][a-z0-9_-]*$"""

    def test_simple_word(self) -> None:
        assert TERRAIN_ID_RE.match("world")

    def test_with_trailing_numbers(self) -> None:
        assert TERRAIN_ID_RE.match("world2")

    def test_hyphenated(self) -> None:
        assert TERRAIN_ID_RE.match("black-sea")

    def test_underscored(self) -> None:
        assert TERRAIN_ID_RE.match("black_sea")

    def test_mixed(self) -> None:
        assert TERRAIN_ID_RE.match("fl-base-pack2")

    def test_reject_uppercase(self) -> None:
        assert not TERRAIN_ID_RE.match("World")

    def test_reject_leading_digit(self) -> None:
        assert not TERRAIN_ID_RE.match("1world")

    def test_reject_space(self) -> None:
        assert not TERRAIN_ID_RE.match("my world")

    def test_reject_empty(self) -> None:
        assert not TERRAIN_ID_RE.match("")

    def test_reject_dot(self) -> None:
        assert not TERRAIN_ID_RE.match("world.v2")


# ---------------------------------------------------------------------------
# Chunk pixel constant
# ---------------------------------------------------------------------------


class TestChunkPixels:
    def test_chunk_pixels_value(self) -> None:
        assert CHUNK_PIXELS == 513

    def test_lod1_formula(self) -> None:
        # (513 - 1) / 2 + 1 == 257 — verify the formula matches the stride result
        base = np.zeros((CHUNK_PIXELS, CHUNK_PIXELS), dtype=np.uint16)
        lod1 = _downsample(base, 2)
        expected = (CHUNK_PIXELS - 1) // 2 + 1
        assert lod1.shape[0] == expected
        assert lod1.shape[1] == expected

    def test_lod2_formula(self) -> None:
        base = np.zeros((CHUNK_PIXELS, CHUNK_PIXELS), dtype=np.uint16)
        lod2 = _downsample(base, 4)
        expected = (CHUNK_PIXELS - 1) // 4 + 1
        assert lod2.shape[0] == expected
        assert lod2.shape[1] == expected
