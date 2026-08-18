# Bundled base terrain — production and hosting

> **Frozen decision record.** This page records a decision as it was made and is not
> maintained against current behaviour. For how the engine works today, see the
> [Developer Guide](../index.md).

**Resolved 2026-08-18 (#1199, #1202). Route C: build once by hand, publish as a versioned release
asset in `fighters-legacy/fl-base-terrain`, pin it by sha256, and fetch it during the release.**

## The question

`tools/build_global_base.sh` (#474) produces the six cube-sphere face roots that
`loadBundledBaseTerrain()` mounts as `builtin:base-terrain` at `INT_MIN` priority, so a zero-pack
launch shows a recognisable Earth instead of the procedural horizon. The mount is gated on one
sentinel tile, `base-terrain/terrain/<id>/f0/l0/tile_0_0.png`.

The packaging seam already existed and was already wired: `release.yml` copied `base-terrain/` into
`dist/` **if the directory was present**, under a comment reading "produced out-of-band by
`tools/build_global_base.sh`". It never was present. Nothing in the repository produced it and
nothing committed it, so the conditional was silently false on every release ever cut and every
release shipped without a base terrain.

"Out-of-band" was the open question and had never been answered. `fighters-legacy/fl-base-pack#9`
is one of the nine gating M4.0 content issues, but its *output* is an engine bundle, not pack
content — the pack lane can produce the bytes, it cannot decide where they live, because they ship
inside the engine release. That made it the only gating pack item with a dependency outside the
pack, and therefore the only one that could stall on a decision nobody had been asked to make.

## What shapes the answer

- **Input is huge, output is small.** GEBCO_2024 is ~7.5 GB (free anonymous download, no API); the
  level-5 output is **~165 MB** (measured on the first bundle, `gebco2024-r1`: 8,190 tiles,
  164 MB on disk, 150 MB zipped). This page originally estimated 15–50 MB; that was optimistic
  by about 3x, and it does not compress away — `ZLEVEL=9` against GDAL's PNG default saves 0.0%.
  The estimate being wrong does not change the decision: it strengthens it, since committing
  165 MB of regenerated PNGs to a source repo is worse than committing 50 MB.
- **It changes almost never.** The source grids are annual at best, and the coarse level is a floor
  for the horizon, not gameplay detail. This is a rarely-regenerated artifact, unlike anything else
  in the release.
- **Attribution is mandatory.** GEBCO requires attribution; ESA WorldCover is CC BY 4.0. REUSE
  covers source files, not a generated binary tree.
- **#1098 wants byte-reproducible distribution** (contentHash verified at join), so whichever route
  wins should pin a checksum.

## Options considered

| | Route | Cost | Downside |
|---|---|---|---|
| A | Commit `base-terrain/` into the engine repo | zero new infra | a **165 MB** binary tree in a source repo, forever, in every clone; every regeneration rewrites it |
| B | Build it in `release.yml` on each tag | always fresh | fetches ~7.5 GB per release job and adds GDAL to the release runner, to regenerate bytes that are identical each time; makes the release long, network-dependent, and newly able to fail on an external download |
| **C** | **Build once, publish as a versioned release asset in its own repo, fetch + verify + stage at release time** | **source repos stay clean; pinned and checksummed; regeneration is an explicit act with its own version** | **one more repo, and a network fetch in the release job** |
| D | Don't ship it — users run the script | nothing to host | a zero-pack launch keeps the procedural horizon, which is the state #474 exists to fix |

**C was chosen.** It matches the artifact's actual lifecycle — produced rarely, consumed every
release — keeps a 50 MB blob out of both source repos, and gives #1098 a checksum to pin. B is the
tempting one to avoid: a 7.5 GB download on every tag to regenerate bytes that do not change.

## The mechanism

**The pin.** `tools/base_terrain.lock.json` names the source repo, the release tag, the asset, its
sha256 and byte size, the terrain id and the max level, plus the source grids the bundle was built
from. An empty `tag` means *no bundle is pinned yet* — a declared state, not a silent one.

**The fetcher.** `tools/fetch_base_terrain.py` reads the pin, downloads the asset, verifies size and
sha256, extracts it (refusing any member that escapes the output directory), and stages
`base-terrain/`. It runs on all three release runners because it is Python with no third-party
imports, which is why the Windows leg needs no separate implementation.

**The release.** `release.yml` runs the fetcher before packaging. The two `if [ -d base-terrain ]` /
`if (Test-Path base-terrain)` copies remain as the staging step, but they are now downstream of
something that actually produces the directory.

**What the fetcher refuses to do.** A release must never *quietly* ship without terrain it was
supposed to carry, so the tool fails the release — non-zero exit — on:

- a download error, or a payload too small to be a terrain bundle (a GitHub error page);
- a size or sha256 mismatch against the pin;
- an archive that is neither a valid zip nor a valid tar, or that contains a link or an escaping
  member;
- a staged tree with fewer than six cube-sphere face roots;
- a staged tree **without the sentinel tile** — the tree would extract cleanly and mount nothing,
  which is the failure mode that looks exactly like success;
- a staged tree **without `ATTRIBUTION.md`**.

The one case that exits 0 without staging anything is the unpinned state, and it says so on stdout.

## Where attribution lives

**Inside the bundle, as `base-terrain/ATTRIBUTION.md`.** REUSE covers source files, and the bundle
leaves this repo entirely — it is built on a workstation and published as a release asset in another
repository — so the only place the notices can reliably travel is inside the tree itself.
`build_global_base.sh` now writes that file as part of every build, naming the elevation,
bathymetry and land-cover grids it was given, and the fetcher refuses to stage a bundle that lacks
it. That is what makes an unattributed tree unable to ship, rather than a rule somebody has to
remember.

## Publish runbook

Once, on a workstation with GDAL and ~10 GB free:

1. Download the GEBCO_2024 sub-ice grid (free anonymous download, <https://www.gebco.net/>), and
   optionally an ESA WorldCover VRT for land-cover tiles.
2. `tools/build_global_base.sh --elevation gebco_2024.tif [--landcover worldcover.vrt]`
3. **Edit `base-terrain/ATTRIBUTION.md`** — the generated file hedges between the grids it might
   have been given; replace the conditional wording with the line that applies.
4. Sanity-check the tree: six `f*` roots, `terrain/world/f0/l0/tile_0_0.png` present, and a size in
   the region of 165 MB at level 5. Decode the sentinel tile and check the elevations are
   Earth-shaped — `gebco2024-r1` reads −9,322 m … +6,095 m, mean −1,999 m, 32% above sea level.
   A tree of the right size and shape can still be noise.
5. **Zip from INSIDE the tree**, so `terrain/` and `ATTRIBUTION.md` are at the archive root:
   `cd base-terrain && zip -r ../base-terrain-<version>.zip .`
   The obvious `zip -r base-terrain-<version>.zip base-terrain/` nests everything one level
   deeper, and the fetcher then stages a tree whose sentinel is at `base-terrain/terrain/...`
   and refuses it. That is the fetcher working — it caught exactly this on the first bundle —
   but the wording above sent it there, so it is fixed here.
6. Publish it as a release of `fighters-legacy/fl-base-terrain`, tagged by the *data* version
   (the source grid year plus a revision — e.g. `gebco2024-r1`), not by an engine version. The
   bundle's lifecycle is the source grids', and tying it to an engine tag would imply it changes
   when the engine does.
7. Update `tools/base_terrain.lock.json` with the tag, asset name, `sha256sum`, byte size, and the
   grids used. That commit is the act of adopting a new bundle, and it is reviewable as a diff.

Regeneration is the same list. It is deliberately manual: this artifact is expected to change on
the order of once a year, and automating a 7.5 GB download for that cadence buys nothing.

## Consequences

- `fighters-legacy/fl-base-terrain` does not exist yet, and the first bundle has not been
  published. Until it is, the pin's `tag` stays empty and releases continue to ship the procedural
  horizon — the same behaviour as every release to date, but now stated in the build log instead of
  being an unnoticed false conditional.
- `fighters-legacy/fl-base-pack#9` becomes a *production* task with a runbook, not an open design
  question. It is no longer blocked on a cross-lane decision.
- #1098 gets its checksum: the pinned sha256 is the byte-reproducibility anchor for the bundle.
- The release job gains a network dependency. It is one small pinned asset from GitHub's own release
  CDN, in a job that already clones submodules and fetches CMake dependencies over the network.
