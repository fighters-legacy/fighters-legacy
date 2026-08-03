<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Satellite Terrain Textures (Sentinel-2)

A content pack can drape real cloud-free satellite imagery over the terrain instead of the
procedural biomes (#488). Each cube-sphere tile carries an optional `_sat.ktx2` colour layer that the
renderer samples in place of the biome shading — and it works over the procedural terrain too, not
only over pack DEM (height) tiles.

## Generating tiles — `tools/gen_terrain_color.py`

The tool builds satellite tiles from **Sentinel-2 L2A** imagery on AWS Open Data, fetched
**anonymously** (no account). It never bundles data — it reads scenes directly at build time.

```bash
tools/gen_terrain_color.py \
    --terrain-id world --faces 2 \
    --min-level 8 --max-level 10 \
    --bbox 37.40 -122.52 37.85 -122.05 \
    --max-cloud 10 \
    --output-dir mypack
```

For each cube-sphere tile intersecting the bounding box it:

1. discovers cloud-free scenes via the earth-search STAC API,
2. reads the B04/B03/B02 (RGB) + SCL (scene classification) bands through GDAL `/vsicurl`,
3. builds an **SCL-cloud-masked median composite** over several scenes (fills gaps, removes clouds),
4. converts surface reflectance to sRGB, and
5. BC7-encodes the result to `terrain/<id>/f<face>/l<level>/tile_<i>_<j>_sat.ktx2` via `tex-compress`
   (**mipmapped** — do not skip mipmaps; minification without them aliases into colour noise).

`gen_terrain_tiles.py` takes the **same `--bbox`**, so the height and land-cover tiles under the
imagery are built with the identical scoping — one box, one theater, three layers:

```bash
tools/gen_terrain_tiles.py \
    --input world_dem.vrt --terrain-id world --output-dir mypack \
    --bbox 37.40 -122.52 37.85 -122.05 \
    --min-level 8 --max-level 10 \
    --landcover-source worldcover.vrt --skip-existing
```

**Prerequisites:** GDAL Python bindings + numpy (`apt install python3-gdal python3-numpy` /
`dnf install python3-gdal python3-numpy`), `tex-compress` (which shells out to `toktx`), and outbound
HTTPS to AWS. Choose a tile level whose tile size is close to the imagery footprint you want per tile
(too fine a level over a wide composite tiles/aliases; too coarse squeezes the scene into a small
area). `--local-source` (offline) reads a local GDAL raster instead of Sentinel-2 when you already
have imagery.

## How the engine consumes it

`TerrainStreamer` probes the `Satellite` tile layer for every resident tile (client-only; a headless
server has no renderer and skips it). When a tile has a `_sat.ktx2`, it is uploaded as a per-tile
texture + material and the tile's `RenderItem` carries `kRenderFlagTerrainSatellite`; the terrain
shader (`mesh.frag`, shadingMode 4) samples the imagery as albedo (with a gentle detail bump) instead
of the biome arrays. Tiles without a `_sat.ktx2` fall back to the land-cover biomes (#475).

## Attribution (required)

Copernicus Sentinel data terms require the attribution **"Contains modified Copernicus Sentinel
data \<year\>"** on a player-reachable credits surface whenever the derived tiles ship in a release
(see `NOTICE`). The tool prints the reminder when it finishes.
