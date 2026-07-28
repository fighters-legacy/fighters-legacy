# Pack manifest reference

`manifest.toml` is the file that makes a directory a mod. Everything else in a pack is optional;
this is not.

```toml
[mod]
name       = "Fighters Legacy Base Pack"
id         = "fl-base-pack"
version    = "0.3.0"
engine-api = "1.0"
priority   = 0

# Optional
namespace  = "fl-base"
depends    = ["some-other-pack"]

[mod.trust]
signature  = ""
signed-by  = "community"
```

Validate a manifest with `validate-mod` before publishing; the pack CI gates run it.

## Required fields

A manifest missing **any** of these fails to load, and the pack is skipped:

| Field | Type | Notes |
|---|---|---|
| `name` | string | Human-readable pack name |
| `id` | string | Machine identifier — see the rules below |
| `version` | string | Pack version. Free-form; used for dependency reporting and the join-time manifest |
| `engine-api` | string | Engine API version this pack targets, e.g. `"1.0"` |
| `priority` | integer | Load order. Higher wins when two packs provide the same asset |

### `id` and `name` validity

Both are used to build filesystem paths, so both are constrained:

- Non-empty, at most 128 characters
- No `/` or `\`, no embedded NUL
- No drive-letter prefix (`C:`…)
- Not a Windows reserved device name (`CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`)

These rules apply on every platform, not just Windows. A pack that loads on Linux and cannot exist
on Windows is a portability bug you would rather find at authoring time.

### `engine-api`

Only the **major** version is compared. `"1.0"`, `"1.3"` and `"1.99"` are all compatible with
engine major `1`; `"2.0"` is not, and the pack is rejected with an explicit message rather than
being loaded and failing later in some unrelated way.

### `priority`

Packs stack. When two provide an asset with the same name, the higher `priority` wins. A theater
pack that overrides some of the base pack's terrain tiles sits above it; a pack that only adds new
content can sit anywhere.

`0` is a reasonable default for a general content pack. The bundled base terrain loads at the
lowest possible priority so that any user pack overrides it.

## Optional fields

### `namespace`

**Defaults to `id`, and the two are not interchangeable.** The namespace prefixes the *definition
ids* this pack declares — sensors, weapons, entity types — so another pack can reference
`fl-base:apq159` unambiguously.

This is the sharp edge of the content system, and it is worth being precise about:

- A field naming a **file** (`mesh`, `flight_model`, `ai_script`, `damage_mesh`, `cockpit`,
  `manual`) takes an **asset name** — a bare stem, no namespace.
- A field naming a **definition** (`entity.sensors`, `hardpoints.allowed` / `default`) takes a
  **namespaced id**.

Getting these backwards does not produce a clean error. A namespaced id fed to the asset loader
becomes a path that cannot exist and is not even a legal Windows filename — which once meant every
aircraft in a pack flew with no radar and nothing said so.

Set `namespace` explicitly when your pack id is long or ugly; leave it alone otherwise.

### `depends`

An array of pack ids this pack needs.

```toml
depends = ["fl-base-pack"]
```

### `[mod.trust]`

| Field | Type | Notes |
|---|---|---|
| `signature` | string | Detached signature. Non-empty marks the pack as signed |
| `signed-by` | string | `community` or `maintainer` |

The engine parses and records the trust tier today; **signature verification itself is not
implemented yet**, so a signature is a declaration rather than a proof. Do not treat a signed pack
as verified. An unknown `signed-by` value warns and falls back to `Unsigned`.

The trust tier is what drives the game's prompt before loading an unsigned pack, and native-code
packs always prompt regardless of tier.

## Directory layout

The manifest sits at the pack root, and the engine looks for assets in well-known subdirectories
beside it:

```
mods/my-pack/
├── manifest.toml
├── aircraft/        entity definitions
├── flight_models/   aerodynamics TOML
├── meshes/          glTF models
├── textures/        KTX2 art
├── liveries/        livery TOML
├── sensors/         sensor definitions
├── weapons/         weapon definitions
├── missions/        mission YAML
├── campaigns/       campaign definitions
├── ai/              Lua behaviour scripts
├── audio/           OGG music and effects
├── terrain/         cube-sphere tiles
├── zones/           airspace escalation policies
└── airports/        airport and runway definitions
```

Every directory is optional. See [asset formats](formats.md) for what goes in each.

## See also

- [Asset formats](formats.md) — the format of everything a pack contains
- [Architecture](../developer/architecture.md) — how packs are discovered, stacked and mounted
- [Tools](../developer/tools.md) — `validate-mod` and the rest of the validator set
