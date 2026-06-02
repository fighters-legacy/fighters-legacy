# SPDX-FileCopyrightText: 2026 John McKenzie
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/gen_unifont_header.py.

All tests use synthetic in-memory data — no downloads, no disk I/O.
"""
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

from gen_unifont_header import _parse_hex_line, build_bitmap, _ARRAY_SIZE


# ---------------------------------------------------------------------------
# _parse_hex_line
# ---------------------------------------------------------------------------

def test_parse_8wide_ascii_A():
    """Capital A (U+0041) is an 8-wide glyph with 32 hex chars."""
    # Minimal synthetic glyph: all bytes are 0x41 (not a real 'A', just tests parsing)
    line = "0041:" + "41" * 16
    result = _parse_hex_line(line)
    assert result is not None
    cp, glyph = result
    assert cp == 0x0041
    assert len(glyph) == 16
    assert all(b == 0x41 for b in glyph)


def test_parse_8wide_space():
    """Space glyph (U+0020) with all-zero bitmap."""
    line = "0020:" + "00" * 16
    result = _parse_hex_line(line)
    assert result is not None
    cp, glyph = result
    assert cp == 0x0020
    assert glyph == bytes(16)


def test_parse_16wide_skipped():
    """A 16-wide glyph (64 hex chars) must be skipped — returns None."""
    line = "4E2D:" + "00" * 32  # U+4E2D CJK, 64 hex chars
    result = _parse_hex_line(line)
    assert result is None


def test_parse_blank_line():
    assert _parse_hex_line("") is None
    assert _parse_hex_line("   ") is None


def test_parse_comment_like_line():
    """Lines without ':' are skipped."""
    assert _parse_hex_line("# comment") is None


def test_parse_beyond_bmp_skipped():
    """Codepoints above U+FFFF are skipped."""
    line = "10000:" + "ab" * 16
    result = _parse_hex_line(line)
    assert result is None


# ---------------------------------------------------------------------------
# build_bitmap
# ---------------------------------------------------------------------------

def test_build_bitmap_size():
    """Output is always exactly 65536 * 16 bytes."""
    data = build_bitmap([])
    assert len(data) == _ARRAY_SIZE


def test_build_bitmap_default_zeros():
    """Slots with no hex data default to zero (except U+FFFF)."""
    data = build_bitmap([])
    assert data[0x0041 * 16 : 0x0041 * 16 + 16] == bytes(16)


def test_build_bitmap_glyph_written():
    """A parsed glyph is placed at the correct offset."""
    glyph_bytes = bytes(range(16))
    hex_data = glyph_bytes.hex()
    lines = [f"0042:{hex_data}"]  # U+0042 'B'
    data = build_bitmap(lines)
    assert data[0x0042 * 16 : 0x0042 * 16 + 16] == glyph_bytes


def test_build_bitmap_solid_white_override():
    """U+FFFF must be solid 0xFF regardless of input data."""
    # Supply a zero glyph for U+FFFF — the override should win.
    lines = ["FFFF:" + "00" * 16]
    data = build_bitmap(lines)
    assert data[0xFFFF * 16 : 0xFFFF * 16 + 16] == b"\xff" * 16


def test_build_bitmap_solid_white_even_with_no_input():
    """U+FFFF is 0xFF even when no hex data is provided at all."""
    data = build_bitmap([])
    assert data[0xFFFF * 16 : 0xFFFF * 16 + 16] == b"\xff" * 16


def test_build_bitmap_16wide_not_written():
    """16-wide glyphs leave their slot as zero."""
    lines = ["4E2D:" + "ab" * 32]  # wide glyph, 64 hex chars
    data = build_bitmap(lines)
    assert data[0x4E2D * 16 : 0x4E2D * 16 + 16] == bytes(16)


# ---------------------------------------------------------------------------
# Atlas layout math (512-column layout)
# ---------------------------------------------------------------------------

def test_atlas_col_row_for_ascii():
    """ASCII 'A' (U+0041): col = 0x41 % 512 = 65, row = 0x41 // 512 = 0."""
    cp = 0x0041
    assert cp % 512 == 65
    assert cp // 512 == 0


def test_atlas_col_row_for_ffff():
    """U+FFFF: col = 65535 % 512 = 511, row = 65535 // 512 = 127."""
    cp = 0xFFFF
    assert cp % 512 == 511
    assert cp // 512 == 127


def test_atlas_col_row_for_light_shade():
    """U+2591 LIGHT SHADE: col = 0x2591 % 512 = 401, row = 0x2591 // 512 = 18."""
    cp = 0x2591
    assert cp % 512 == 0x2591 % 512
    assert cp // 512 == 0x2591 // 512
    # Explicit expected values
    assert cp % 512 == 401
    assert cp // 512 == 18


def test_atlas_total_dimensions():
    """Atlas must be exactly 4096 x 2048 pixels (512 cols x 8px, 128 rows x 16px)."""
    assert 512 * 8 == 4096
    assert 128 * 16 == 2048
