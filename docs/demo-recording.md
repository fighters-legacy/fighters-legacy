# Cinematic Demo Recording

Fighters Legacy can record scripted demo missions to video **headless** — no display and, paired with a
software Vulkan driver, no GPU. The built-in AI bots fly the action; camera shots authored in the
mission YAML catch it; the recorder pipes frames to `ffmpeg`. This is the pipeline behind the phase-gate
demo videos (epic #909).

## Pieces

| Piece | What it does |
|---|---|
| `cameras:` in the mission YAML | Scripted camera shots — `static` / `orbit` / `chase` / `move` (see [modding/missions.md](modding/missions.md#cameras--cinematic-shots-optional)). Parsed by the single schema owner, so `validate-mission` covers them. |
| `ShotDirector` (engine-mission) | Turns the shot list + live entity poses into a camera pose at a given sim time. |
| `--headless` (game client) | A swapchain-free renderer that draws into owned images — no window, no surface, no present. |
| `--record` (game client) | Drives the camera from the shots and pipes frames to ffmpeg (mp4) or a PNG sequence. |
| `fl-server --time-rate` | Runs the sim at a reduced **wall-clock** rate (sim content unchanged) so a slow software-rendered client never misses a capture boundary. |
| `MsgMissionRoster` (0x1B) | Maps mission object ids (`bandit1`) to network entities, so entity-relative shots resolve. |
| `tools/record_demo/record_demo.py` | Orchestrates the whole thing per demo, and `--all` over `missions/demos/demos.json`. |

## Dependencies

- **ffmpeg** with libx264 (`apt install ffmpeg`) — a record-time tool dependency only; it is never
  linked. Use `--record-png-dir` to skip it and write a PNG sequence instead.
- **Mesa lavapipe** for no-GPU rendering (`apt install mesa-vulkan-drivers`). It provides a software
  Vulkan ICD at `/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`. A real GPU works too — headless does not
  require lavapipe, only a display-less Vulkan device.

## Quick start

Record one demo (headless, lavapipe, mp4):

    tools/record_demo/record_demo.py \
      --game   build/release/game/fighters-legacy/fighters-legacy \
      --server build/release/server/fl-server/fl-server \
      --assets . \
      --mission demo-dogfight \
      --out-dir demo-videos \
      --headless

Record the whole v0.4.0 set:

    tools/record_demo/record_demo.py --game … --server … --assets . --all --headless

No ffmpeg / libx264 (PNG sequence instead of mp4):

    tools/record_demo/record_demo.py --game … --server … --assets . --mission demo-dogfight --headless --png

The driver launches an `fl-server` at `--time-rate quarter`, connects a headless observer recorder,
waits for it to finish, stops the server, and `ffprobe`s the output as a smoke check. A non-zero exit
means a demo exceeded its duplicated-frame cap or the encoder failed — **bad video is loud, never
silently shipped**.

## The honesty mechanism

The recorder abandons wall-clock pacing: it emits exactly one video frame per **capture boundary**
(`60 / --record-fps` ticks of the snapshot stream). If the client falls behind — the sim advances past
more than one boundary between renders — the missed boundaries are filled with duplicated frames and
counted. `--record-max-dup` fails the run when duplicates exceed the cap. The fix is not to hide the
drop but to **slow the server**: `--time-rate quarter` or `eighth` gives a slow (lavapipe) client more
wall time per boundary.

## Verify by hand

The milestone proof (no window, no GPU):

    DISPLAY= VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
      build/release/game/fighters-legacy/fighters-legacy \
      --headless --mission builtin:sandbox --auto --screenshot out.png

produces a correct PNG. For a full recording, run `record_demo.py` as above, then **watch the mp4**:
cuts at the authored shot times, aircraft tracked through the action.

## Authoring shots

Shot `start`/`duration` are sim-seconds from mission start. To place them around real events, run the
mission through the deterministic report first and read the event ticks:

    fl-server --mission missions/demos/demo-dogfight.yaml --mission-report /tmp/r.json

The sim is deterministic, so shots authored against the report stay in sync run to run. If a sim change
drifts a demo, re-run the report and adjust. Iterate a shot list without touching the shared mission via
`--shot-track <cameras-only.yaml>` (a YAML doc with just a `cameras:` block).

## Troubleshooting

| Symptom | Fix |
|---|---|
| `openFfmpeg: popen failed` | ffmpeg not on `PATH`. Install it, or use `--record-png-dir`. Override the binary with `FL_FFMPEG`. |
| `Unknown encoder 'libx264'` | Your ffmpeg lacks libx264. Install a full build, or use `--record-png-dir`. |
| Many duplicated frames / non-zero exit | The client can't keep up. Lower `--time-rate` (`eighth`), drop `--record-res`, or lower `--record-fps`. |
| No Vulkan device headless | Install `mesa-vulkan-drivers` and set `VK_ICD_FILENAMES` to the lavapipe ICD; unset `DISPLAY`. |
| Aircraft look like placeholder shapes | Expected zero-pack — the builtin silhouettes (#886). Pack-variant demos with real aircraft follow once fl-base-pack records well. |

## The v0.4.0 demo set

`missions/demos/` (zero content pack, builtin AI + placeholder meshes):

1. **demo-dogfight** — 2v2 `builtin:fighter` guns + IR; establishing static → orbit at the merge →
   chase the blue lead → move flyby → static tracking a survivor.
2. **demo-sam-strike** — a strike pair vs a SAM + AAA site; ground static at the site, chase inbound,
   orbit the engagement.
3. **demo-formation-tour** — a 4-ship on a waypoint route at dawn under a partly-cloudy sky; move slide
   along the formation, wide landscape static.
4. **demo-sensors-intercept** — a patrol → detect → intercept honest-sensing chain.
5. **demo-gallery-flyover** — a museum row of the builtin placeholder silhouettes (#886), a slow move
   camera touring the asset catalog.
