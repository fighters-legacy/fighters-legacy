#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# build_global_base.sh — build the bundled coarse global base terrain (#474).
#
# Produces the six cube-sphere face roots to a coarse quadtree level from a global elevation
# source (land + ocean bathymetry) and an optional land-cover source, by invoking the cube-sphere
# tile generator (tools/gen_terrain_tiles.py). The result is the terrain/<id>/... tree that
# loadBundledBaseTerrain() mounts at lowest priority so a zero-user-pack launch shows a
# recognizable Earth (generateProceduralTile fills finer near-camera detail; user packs override).
#
# This produces an engine BUNDLE (staged next to the binary + shaders/), not a user content pack;
# it is intentionally coarse (~15-50 MB at level 5) so it can ship with the release.
#
# Sources (bring your own; all GDAL-readable, ideally global lat/lon EPSG:4326):
#   --elevation   Global topo grid. Two recipes for oceans:
#                   (a) GEBCO alone (land + ocean, ~15 arc-sec) as --elevation — simplest.
#                   (b) A land DEM (e.g. Copernicus GLO-30) as --elevation PLUS --bathymetry GEBCO,
#                       which fills the DEM's ocean/nodata cells with real sea-floor depth (#476).
#   --bathymetry  Optional GEBCO grid merged UNDER the land DEM (recipe b). Without it (or without a
#                 GEBCO --elevation), oceans sit flat at the datum.
#   --landcover   Optional ESA WorldCover / any class raster -> nearest-neighbour _lc.png tiles.
#
# Usage:
#   tools/build_global_base.sh --elevation gebco_2024.tif \
#       [--bathymetry gebco_2024.tif] [--landcover worldcover.vrt] \
#       [--output-dir base-terrain] [--max-level 5]
#
# Prereqs: GDAL Python bindings + CLI (apt install gdal-bin python3-gdal python3-numpy).
# GEBCO_2024 sub-ice grid: free anonymous download from https://www.gebco.net/ (~7.5 GB).

set -euo pipefail

MAX_LEVEL=5
OUTPUT_DIR="base-terrain"
ELEVATION=""
BATHYMETRY=""
LANDCOVER=""
TERRAIN_ID="world"

die() { echo "error: $*" >&2; exit 1; }

usage() {
    sed -n '5,35p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --elevation)  ELEVATION="${2:-}"; shift 2 ;;
        --bathymetry) BATHYMETRY="${2:-}"; shift 2 ;;
        --landcover)  LANDCOVER="${2:-}"; shift 2 ;;
        --output-dir) OUTPUT_DIR="${2:-}"; shift 2 ;;
        --max-level)  MAX_LEVEL="${2:-}"; shift 2 ;;
        --terrain-id) TERRAIN_ID="${2:-}"; shift 2 ;;
        -h|--help)    usage 0 ;;
        *) echo "unknown argument: $1" >&2; usage 1 ;;
    esac
done

[[ -n "$ELEVATION" ]] || { echo "error: --elevation is required" >&2; usage 1; }
[[ -f "$ELEVATION" ]] || die "elevation source not found: $ELEVATION"
[[ -z "$BATHYMETRY" || -f "$BATHYMETRY" ]] || die "bathymetry source not found: $BATHYMETRY"
[[ -z "$LANDCOVER" || -f "$LANDCOVER" ]] || die "land-cover source not found: $LANDCOVER"
command -v python3 >/dev/null || die "python3 not found"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GEN="$SCRIPT_DIR/gen_terrain_tiles.py"
[[ -f "$GEN" ]] || die "generator not found: $GEN"

echo "Building coarse global base '$TERRAIN_ID' (levels 0..$MAX_LEVEL) -> $OUTPUT_DIR"
echo "  elevation: $ELEVATION"
[[ -n "$BATHYMETRY" ]] && echo "  bathymetry: $BATHYMETRY"
[[ -n "$LANDCOVER" ]] && echo "  land cover: $LANDCOVER"

# Empty-array expansion is written set -u / bash 3.2 (macOS) safe.
EXTRA_ARGS=()
[[ -n "$BATHYMETRY" ]] && EXTRA_ARGS+=(--bathymetry-source "$BATHYMETRY")
[[ -n "$LANDCOVER" ]] && EXTRA_ARGS+=(--landcover-source "$LANDCOVER")

python3 "$GEN" \
    --input "$ELEVATION" \
    ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"} \
    --terrain-id "$TERRAIN_ID" \
    --faces all \
    --min-level 0 --max-level "$MAX_LEVEL" \
    --output-dir "$OUTPUT_DIR"

echo "Done. Stage '$OUTPUT_DIR' next to the game/fl-server binary so loadBundledBaseTerrain()"
echo "finds it (the release workflow copies base-terrain/ alongside shaders/)."
