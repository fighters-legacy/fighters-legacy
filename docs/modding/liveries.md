<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Liveries — re-skinning an aircraft (#845)

A **livery** re-skins an aircraft by swapping its textures — nothing else. It never touches geometry,
nodes, or UVs. That single invariant is what makes a stranger's livery safe to accept in
multiplayer: two peers can disagree about pixels, never about the shape of an aircraft. Every shipping
combat sim converged on this same texture-set-indirection design for exactly that reason.

A livery is an ordinary content pack, so pack priority, the mod loader, and the distribution
machinery all apply with no new mechanism. A livery pack can contain nothing but a `.toml` and a
couple of `.ktx2` skins (built from committed PNG masters — see
[`textures.md`](textures.md)) — it does **not** ship a mesh.

## The file

Liveries live in `liveries/<id>.toml`. The file stem is the livery id.

```toml
# liveries/f5e_aggressor_blue.toml
[livery]
name     = "Aggressor Blue"
aircraft = "fl-base:f5e"        # a namespaced DEF ID (never a filename)

[textures]                      # material slot . map  ->  replacement texture ASSET NAME
f5e_skin.diffuse = "f5e_aggressor_blue_diffuse"
f5e_skin.orm     = "f5e_aggressor_blue_orm"
# any map omitted -> the base aircraft's texture for that slot
```

### The two-vocabulary rule (do not mix these up)

- `aircraft` is a **def id** — `"<namespace>:<local>"`, the same id the aircraft's `entities/*.toml`
  declares. It is resolved through the id index, never the filesystem.
- The values under `[textures]` are **asset names** — bare stems resolved through `kAssetPaths`
  (`Texture → textures/*.ktx2`). `"f5e_aggressor_blue_diffuse"` resolves to
  `textures/f5e_aggressor_blue_diffuse.ktx2`.

### The keys: `<slot>.<map>`

Each key names a **material slot** and a **map** on that slot. The map vocabulary is `diffuse`
(baseColor), `orm`, and `normal` — the renderer skins only those. The slot name matches the base
aircraft's texture-naming convention: the base textures are named `<slot>_<map>` (e.g.
`f5e_skin_diffuse`), so the key `f5e_skin.diffuse` re-skins exactly that map of that slot.

Because `f5e_skin.diffuse` is a TOML *dotted key*, `[textures]` is really a table of slot sub-tables.
The explicit form is identical:

```toml
[textures.f5e_skin]
diffuse = "f5e_aggressor_blue_diffuse"
orm     = "f5e_aggressor_blue_orm"
```

## Resolution and fallback

The renderer resolves each of the base aircraft's texture references through the livery table:

- **Slot map present in the livery** → the replacement texture is loaded.
- **Slot map omitted** → the base aircraft's texture for that slot is used (per-map fallback).
- **A present override that fails to load** (missing/broken file) → the base texture is used.
- **No livery installed for the aircraft** → the base scheme, unchanged.

So a partial livery (diffuse only) or a broken one degrades gracefully; it never fails an aircraft to
spawn. Geometry, nodes, and UVs are read only from the base aircraft's mesh — a livery cannot change
them, which is what makes user skins riskless.

When two peers have different livery packs installed, each sees its own livery and they never disagree
about geometry (the mesh is always the base pack's).

## Selecting a livery

Today a livery is selected per aircraft **type**: the highest-priority installed livery whose
`aircraft` matches an entity's def id is applied. (Per-entity / per-mission / per-loadout livery
selection carried on the wire is a planned extension; the runtime seam is already in place.)

## Authoring checklist (base-pack side)

- Give each aircraft **stable, documented material slots** (`f5e_skin` liverable; `f5e_canopy`,
  `f5e_nozzle` not). Treat the slot names as ABI once released.
- Name base textures `<slot>_<map>` so livery keys extend the convention predictably.
- Bake **no** markings or insignia into the base diffuse (also required by the likeness policy).
- Ship each aircraft's UV layout template so a painter can work without opening Blender.

## Validating

`validate-livery` shares the engine's own `parseLiveryDef`, so a livery it passes is one the engine
loads:

```
validate-livery liveries/f5e_aggressor_blue.toml     # schema + plausibility
validate-livery --pack path/to/livery-pack           # resolve texture files + aircraft def id
```

In `--pack` mode a texture asset name that does not resolve to a file the pack ships is an **error**;
an `aircraft` def id that is not in the pack is a **warning** (it legitimately lives in a base pack).
Single-file mode also warns about a no-op (empty) livery and an override whose map is outside
`diffuse`/`normal`/`orm` (which the renderer would silently ignore).
