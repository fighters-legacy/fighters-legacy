# SPDX-FileCopyrightText: 2026 John McKenzie
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate platform/vulkan/UnifontBitmap.{h,cpp} from GNU Unifont .hex data.

The generated files contain a 1 MB (65536 x 16 bytes) bitmap array for the
full Unicode BMP.  Run this tool once and commit the output; re-run only when
upgrading the Unifont version.

Usage:
    python3 tools/gen_unifont_header.py [--input unifont-XX.hex] \
        [--output-dir platform/vulkan]

When --input is omitted the tool downloads unifont-16.0.02.hex.gz from the
GNU FTP mirror automatically.
"""
import argparse
import gzip
import io
import sys
import urllib.request

__version__ = "1.0.0"

_UNIFONT_URL = (
    "https://unifoundry.com/pub/unifont/unifont-16.0.02/"
    "font-builds/unifont-16.0.02.hex.gz"
)

_ARRAY_SIZE = 65536 * 16  # 1 048 576 bytes


def _parse_hex_line(line: str) -> tuple[int, bytes] | None:
    """Parse one line of GNU Unifont .hex format.

    Returns (codepoint, 16-byte glyph) for 8-wide glyphs, or None for
    16-wide glyphs (CJK etc.) which are skipped — they don't fit an
    8-pixel-wide atlas.
    """
    line = line.strip()
    if not line or ":" not in line:
        return None
    cp_str, data_str = line.split(":", 1)
    if len(data_str) != 32:
        # 64 hex chars = 16-wide glyph; skip.
        return None
    cp = int(cp_str, 16)
    if cp > 0xFFFF:
        return None
    return cp, bytes.fromhex(data_str)


def build_bitmap(lines) -> bytearray:
    """Build the 65536 x 16-byte bitmap from an iterable of .hex lines."""
    data = bytearray(_ARRAY_SIZE)

    for raw in lines:
        if isinstance(raw, bytes):
            raw = raw.decode("ascii", errors="ignore")
        result = _parse_hex_line(raw)
        if result is None:
            continue
        cp, glyph = result
        offset = cp * 16
        data[offset : offset + 16] = glyph

    # Reserve codepoint U+FFFF (col=511, row=127 in the 512-col atlas) as a
    # solid-white block used by VkRenderer for HudElement::Rect/Line fills.
    data[0xFFFF * 16 : 0xFFFF * 16 + 16] = b"\xff" * 16

    return data


def _bytes_to_c_array(data: bytearray) -> str:
    """Render a bytearray as a comma-separated hex initializer list."""
    parts = []
    for i, b in enumerate(data):
        if i % 16 == 0:
            parts.append("\n   ")
        parts.append(f" 0x{b:02x},")
    return "".join(parts)


# Split to prevent REUSE from treating these as file-level annotations.
_SPDX_GPL2 = "// SPDX" "-License-Identifier: GPL-2.0-or-later\n"
_GENERATED_COMMENT = (
    "// Generated from GNU Unifont -- do not edit by hand.\n"
    "// Re-run tools/gen_unifont_header.py to update.\n"
)


def emit_header(output_dir: str) -> None:
    h_path = f"{output_dir}/UnifontBitmap.h"
    with open(h_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(
            _SPDX_GPL2
            + _GENERATED_COMMENT
            + "#pragma once\n"
            "#include <cstdint>\n"
            "\n"
            "// GNU Unifont 8x16 bitmap: 65536 glyphs x 16 bytes each = 1048576 bytes.\n"
            "// Codepoint cp occupies bytes [cp*16, cp*16+16).\n"
            "// Atlas layout: 512 columns x 128 rows of 8x16-pixel glyphs (4096x2048 px).\n"
            "// Codepoint U+FFFF is overridden with 0xFF (solid white) for rect/line fills.\n"
            "extern const uint8_t kUnifontBitmap[1048576];\n"
        )
    print(f"Wrote {h_path}", file=sys.stderr)


def emit_cpp(data: bytearray, output_dir: str) -> None:
    cpp_path = f"{output_dir}/UnifontBitmap.cpp"
    array_body = _bytes_to_c_array(data)
    with open(cpp_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(
            _SPDX_GPL2
            + _GENERATED_COMMENT
            + '#include "UnifontBitmap.h"\n'
            "\n"
            "// clang-format off\n"
            f"const uint8_t kUnifontBitmap[1048576] = {{{array_body}\n}};\n"
            "// clang-format on\n"
        )
    print(f"Wrote {cpp_path}", file=sys.stderr)


def _open_input(input_path: str | None):
    """Return an iterable of text lines from the hex source."""
    if input_path:
        with open(input_path, "r", encoding="ascii") as f:
            yield from f
        return

    print(f"Downloading {_UNIFONT_URL} ...", file=sys.stderr)
    with urllib.request.urlopen(_UNIFONT_URL) as resp:
        compressed = resp.read()
    print("Download complete. Decompressing ...", file=sys.stderr)
    with gzip.open(io.BytesIO(compressed)) as gz:
        yield from io.TextIOWrapper(gz, encoding="ascii")


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate UnifontBitmap.{h,cpp} from GNU Unifont hex data."
    )
    parser.add_argument(
        "--input",
        metavar="FILE",
        help="Path to unifont .hex file (omit to auto-download)",
    )
    parser.add_argument(
        "--output-dir",
        default="platform/vulkan",
        metavar="DIR",
        help="Directory to write UnifontBitmap.{h,cpp} (default: platform/vulkan)",
    )
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    args = parser.parse_args(argv)

    lines = _open_input(args.input)
    print("Parsing glyph data ...", file=sys.stderr)
    data = build_bitmap(lines)
    print(f"Parsed {_ARRAY_SIZE} bytes for 65536 glyphs.", file=sys.stderr)

    emit_header(args.output_dir)
    emit_cpp(data, args.output_dir)
    print("Done.", file=sys.stderr)


if __name__ == "__main__":
    main()
