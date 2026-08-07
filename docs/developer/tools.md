# Tools

Everything in `tools/`. Each C++ binary is built by the standard presets and answers `--help`; the
usage lines below are taken from the binaries themselves.

Build one on its own with `cmake --build --preset release --target <name>`.

## Content validators

Every asset class has a validator, and the pack CI gates run them. Each one delegates to the
**engine's own parser**, so a file that passes cannot be rejected by the engine for a schema reason
— the validator and the runtime cannot drift into disagreeing.

| Tool | Usage |
|---|---|
| `validate-mission` | `validate-mission [--pack <dir>] <file.yaml> [file2.yaml ...]` |
| `validate-campaign` | `validate-campaign [--pack <dir>] <campaign.yaml> [more.yaml ...]` |
| `validate-entity` | `validate-entity <file.toml> [file2.toml ...]` |
| `validate-flight-model` | `validate-flight-model <file.toml> [file2.toml ...]` |
| `validate-weapon` | `validate-weapon <file.toml> [file2.toml ...]` |
| `validate-sensor` | `validate-sensor <file.toml> [file2.toml ...]` |
| `validate-livery` | `validate-livery <file.toml> [file2.toml ...]` |
| `validate-mode` | `validate-mode <file.toml> [file2.toml ...]` |
| `validate-playlist` | `validate-playlist --playlist <path/to/data/playlist.toml> [--pack <dir>]` |
| `validate-mesh` | `validate-mesh <file.glb> [file2.gltf ...]` |
| `validate-mod` | `validate-mod [--no-licenses] [--allow <spdx-id>]... <pack-dir>` |
| `validate-licenses` | `validate-licenses [--dir <path>] [--licenses-dir <path>] [--allow <id>] ...` |

**`--pack <dir>` is the flag that matters.** Without it a validator checks the file's schema in
isolation; with it, cross-references are resolved against a real pack — that every sensor id
exists, every weapon a hardpoint allows is defined, every mesh a definition names is present. Most
real content bugs are reference bugs, and they are invisible without `--pack`.

## Asset pipeline

| Tool | Usage / purpose |
|---|---|
| `tex-compress` | `tex-compress [options] <input.png> [<output.ktx2>]` — Basis Universal compression |
| `terrain-chunk-io` | `terrain-chunk-io <subcommand> [options]` — decode terrain PNGs, generate procedural tiles |
| `fl-viewer` | Model preview in the game renderer |
| `locale-extract` | Extract translatable strings |
| `gen_terrain_tiles.py` | Cube-sphere quadtree tiles from a global DEM (**the current terrain path**); `--bbox` scopes a run to one theater, full faces otherwise |
| `gen_terrain_chunks.py` | Planar chunk grid — **legacy**, superseded by the above |
| `gen_terrain_color.py` | Sentinel-2 satellite imagery tiles |
| `gen_wind_profile.py` | Altitude wind profile TOML from gridded wind data |
| `blender_gen.py` | Parametric aircraft mesh generation, headless Blender |
| `gen_builtin_glb.py` · `gen_unifont_header.py` | Regenerate compiled-in geometry and the HUD font |

The Python tools guard heavy imports (GDAL, netCDF) behind `try/except ImportError`, so `--help`
and the unit tests work without the system packages installed.

## Measurement and analysis

| Tool | Usage / purpose |
|---|---|
| `bot_swarm` | `bot_swarm [host] [port] [options]` — headless multi-client load generator |
| `scale_gate.py` | Drives `bot_swarm` against `scale-gate.json` thresholds; the CI scale gate |
| `net_check` | `net_check [host] [port] [--count N] [--interval MS]` — ENet smoke test and latency bench |
| `fm-trim` | `fm-trim <flight-model.toml> [options]` — derives performance from a flight model |
| `audio_check` | `audio_check [--check-ogg <file.ogg>]` — audio device and OGG decode check |
| `input_check` | Gamepad, joystick and HOTAS axis inspection |
| `latency_analysis/` | Per-platform loopback RTT measurement and comparison |
| `gpu_contention/` | GPU contention harness |
| `ai_eval/` | Local AI provider evaluation ([README](https://github.com/fighters-legacy/fighters-legacy/blob/main/tools/ai_eval/README.md)) |

`fm-trim --expect` is the flight-model acceptance instrument: it derives stall speed, turn rates,
climb and specific range from the model and gates them against the published chart. It runs in CI,
which also makes it the engine's guard against a change to the aerodynamics silently altering every
aircraft's performance.

## Development and CI helpers

| Tool | Purpose |
|---|---|
| `code_stats.py` | Application statistics for a release: composition by category plus the product surface. Attached to every release and appended to minor-release bodies — see [the release process](project-management.md#application-statistics-milestone-gates) |
| `docs_drift.py` | Checks documentation against the code it describes ([the drift gate](https://github.com/fighters-legacy/fighters-legacy/blob/main/.github/workflows/docs-drift.yml)) |
| `coverage_gate.py` | Runs `gcovr` once and gates on the result, separating "below threshold" from "no number was produced" ([code coverage](development.md#code-coverage)) |
| `check_deps.py` | Reports which pinned dependencies have newer upstream releases |
| `lint_workflow_expressions.py` | Catches `${{ }}` interpolation into `run:` blocks |
| `lint_test_names.py` | Catches non-ASCII in Catch2 test names and tags, which ctest cannot select on Windows |
| `mission_test/` | Mission harness assertions |
| `record_demo/` | Headless cinematic capture ([demo recording](demo-recording.md)) |
| `visual_check.sh` / `.ps1` | Launch the game into a known scene for visual verification |
| `build_global_base.sh` | Build the coarse global base terrain bundle |

Repository scripts live in `scripts/`: `cut-release.sh` and `tag-release.sh` (the release process
in [project management](project-management.md)), `roadmap-status.sh`, and the
`commit-msg` / `pre-commit` hooks.

## See also

- [Development](development.md) — building, presets and prerequisites
- [Load testing](load-testing.md) — using `bot_swarm` and the scale gate properly
- [Modding Guide](../modding/index.md) — what the validators are validating
