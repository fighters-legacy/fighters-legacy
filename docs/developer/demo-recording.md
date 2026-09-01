# Cinematic Demo Recording

Fighters Legacy can record scripted demo missions to video **headless** — no display and, paired with a
software Vulkan driver, no GPU. The built-in AI bots fly the action; camera shots authored in the
mission YAML catch it; the recorder pipes frames to `ffmpeg`. This is the pipeline behind the phase-gate
demo videos (epic #909).

## Pieces

| Piece | What it does |
|---|---|
| `cameras:` in the mission YAML | Scripted camera shots — `static` / `orbit` / `chase` / `move` (see [modding/missions.md](../modding/missions.md#cameras--cinematic-shots-optional)). Parsed by the single schema owner, so `validate-mission` covers them. |
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
waits for it to finish, stops the server, and checks the output. A non-zero exit means a demo
exceeded its duplicated-frame cap, was truncated by its wall-clock cap, produced a video with no
picture in it, or the encoder failed — **bad video is loud, never silently shipped**.

### What the acceptance check actually checks

Four things, because for a while it was one ([#1347], [#1378]):

| Check | What fails it |
|---|---|
| **Manifest** — the shot list vs `demos.json`, *before anything is recorded* | a mission whose shots end at a different second than its `expected_duration`, or that declares no shots |
| **Envelope** — `ffprobe` container duration | zero-length, far shorter than the shot list, or longer than it |
| **Payload** — one `ffmpeg` decode pass: `blackdetect` + per-frame `signalstats` YDIF | more than half the run black, or more than half of it a still picture |
| **Attribution** — each frozen stretch against the shot that produced it | a still picture lasting ≥ 2 s on a shot that is not `static` |

A duration bound cannot fail on an empty video, and it did not: a windowed recording that captured
**pure black for its whole length** printed `[OK] … 22.6s within [3.0, N]` and reported "1/1 demo(s)
recorded successfully". A gate that checks the envelope and never the payload is not evidence. The
PNG path has no image dependency, so it counts **distinct frame hashes** instead — one repeated black
frame hashes to one value however many files it wrote.

**The manifest check runs first and fails the whole run**, because the two numbers are load-bearing
in *different* places: the recorder derives `--record-max-sec` from the **shot list**, while the
acceptance window comes from **`demos.json`**. When they drift, a demo is capped by one number and
then judged against the other — `demo-atc-scramble` ran to 128 s of shots while the manifest still
declared the 44 s it had carried since [#909], so a complete recording would have failed its own
duration gate. `tests/test_record_demo.py` asserts the committed set agrees, so drift fails in CI
rather than on the next hands-on pass. It needs PyYAML; without it the recorder warns and records
anyway.

**Attribution is what the payload check cannot see.** "More than half the run frozen" is an
envelope too: it passes a demo whose orbit shot rendered one still image for its entire length, and
that is a defect every time — an orbiting or moving camera changes the picture *by definition*, so a
frozen one means the shot frames nothing, its target is gone ([#1381]), or the scene is not lit. A
`static` shot is exempt: holding a quiet scene still is what it is for. The recorder's timeline is
the shot list's, so a freeze timestamp indexes straight into the shot that produced it.

**The still-picture floor is the check, and it is measured directly.** The gate used ffmpeg's
`freezedetect`, whose response to its own noise threshold is **not monotone** on real footage —
sweeping one `demo-sam-strike` recording gave `0.0s` at `n=0.1`, `25.7s` at `n=0.01`, `0.0s` at
`n=0.001`, `6.6s` at `n=0.0002` and `0.0s` at `n=0.000005`. A tighter threshold reporting *more*
frozen time, twice, in both directions: whatever value you pick you cannot say what it means. The
payload check now measures the per-frame difference itself (`signalstats` YDIF) and calls a run of
frames at or below `FREEZE_YDIF` for ≥ 2 s a still stretch, which is monotone by construction.

The shipped `freezedetect` default (`0.001`) also sat **above even `demo-dogfight`'s median frame
difference** — above the busiest demo in the set — so every quieter demo was reported frozen while
its picture was moving:

    footage                                                  YDIF median   normalised
    demo-atc-scramble, cameras 2 km out over empty ground       0.000094      3.7e-7   genuinely dead
    demo-night-patrol, a deliberate static hold                 0.00014       5.6e-7   genuinely still
    demo-atc-scramble, cameras 60 m off the measured track      0.0035        1.4e-5   MOVING
    demo-dogfight, the busiest demo in the set                  0.21          8.3e-4   moving, loudly

The tell was that the verdict depended on **`--time-rate`**, a recording *speed* knob: the identical
shot list measured **0.0 s frozen at `normal` and 93.8 s at `quarter`**, because the sky animates on
wall clock and advances four times further per video frame at the faster rate. A content check whose
answer changes with how fast you recorded is not measuring the content.

`FREEZE_YDIF = 0.0005` sits ~5× above the dead footage and ~7× below real motion, and a recording of
one repeated frame — the [#1347] failure the check exists for — measures exactly `0` and is caught
with the whole margin to spare. Verified both ways on the same clips: **40.0 s** still on the old
`demo-atc-scramble` recording (attributed to its `move` shot), **0.0 s** on the re-framed one.

⚠ **None of the four can see COMPOSITION.** A demo can pass all of them while its subject is a speck,
its carrier is out of frame, or its aircraft is one dot against an empty sky. Extract a frame and
look before believing a green demo gate.

### The recording cap is wall clock; the shot list is sim time

`--record-max-sec` is a **wall-clock** safety stop and a shot list is **sim** seconds;
`fl-server --time-rate` is exactly the ratio between them. A flat 180 s cap at `--time-rate eighth`
buys 22.5 s of shot list, which is why every demo once recorded to the same 22.6 s whatever its shots
said ([#1347]). Two things follow, and both are now true:

- `record_demo.py --max-sec` defaults to **0 = derive it** from the demo's shot total and
  `--time-rate` (`--timeout` follows it). Pass a number to override.
- Tripping the cap **fails the run**: the recorder logs `recorder: TRUNCATED — the N s wall-clock cap
  stopped the run at X s of sim time, short of the Y s shot list` and exits non-zero. Stopping early
  is not the same fact as a short demo, and only one of them is a successful recording.

[#1347]: https://github.com/fighters-legacy/fighters-legacy/issues/1347
[#1378]: https://github.com/fighters-legacy/fighters-legacy/issues/1378
[#1381]: https://github.com/fighters-legacy/fighters-legacy/issues/1381
[#909]: https://github.com/fighters-legacy/fighters-legacy/issues/909

## The honesty mechanism

The recorder abandons wall-clock pacing: it emits exactly one video frame per **capture boundary**
(`60 / --record-fps` ticks of the snapshot stream). If the client falls behind — the sim advances past
more than one boundary between renders — the missed boundaries are filled with duplicated frames and
counted. `--record-max-dup` fails the run when duplicates exceed the cap. The fix is not to hide the
drop but to **slow the server**: `--time-rate quarter` or `eighth` gives a slow (lavapipe) client more
wall time per boundary.

⚠ **A duplicated frame is a PACING duplicate, not a visually identical one.** It means the client did
not render in time, never that the picture did not change. A long `static` shot over a quiet scene
therefore costs nothing against the cap — the renderer keeps producing frames whether or not the
image moves — and no shot length is unrecordable "by construction" ([#1378] argued otherwise, from a
run whose duplicates were the 2.6 FPS capture sink of [#1375]). A still *picture* is what the
attribution check above is for; the two failure modes look alike in a video and share nothing.

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
| `recorder: TRUNCATED` | The wall-clock cap stopped the run inside the shot list. Raise `--max-sec` (or leave it at 0 and let the driver derive it) or use a faster `--time-rate`. |
| `x.xs of y.ys is BLACK` / `is a FROZEN frame` | The recording has no picture in it. Check the recorder's size warning first (below), then that the mission is actually rendering (`--screenshot`). |
| `recorder: recording at AxB — CxD was requested` | Windowed only, and not an error: the compositor gave the window a different drawable size (HiDPI / fractional scaling — a 960x540 request lands as 1200x675 at 125%). The video is recorded at what the renderer actually delivers. Use `--headless` for an exact frame size. |
| `recorder: captured frame is AxB but the encoder is open at CxD` | A window resized mid-recording. The run aborts rather than write black frames; re-record without touching the window. |
| `openFfmpeg: popen failed` | ffmpeg not on `PATH`. Install it, or use `--record-png-dir`. Override the binary with `FL_FFMPEG`. |
| `Unknown encoder 'libx264'` | Your ffmpeg lacks libx264. Install a full build, or use `--record-png-dir`. |
| Many duplicated frames / non-zero exit | The client can't keep up. Lower `--time-rate` (`eighth`), drop `--record-res`, or lower `--record-fps`. |
| No Vulkan device headless | Install `mesa-vulkan-drivers` and set `VK_ICD_FILENAMES` to the lavapipe ICD; unset `DISPLAY`. |
| Aircraft look like placeholder shapes | Expected zero-pack — the builtin silhouettes (#886). Pack-variant demos with real aircraft follow once fl-base-pack records well. |

## The demo set

`missions/demos/` (zero content pack, builtin AI + placeholder meshes). The first five are the
original set; the last five each showcase one engine epic that landed for v0.4.0.

1. **demo-dogfight** — 2v2 `builtin:fighter` guns + IR; establishing static → orbit at the merge →
   chase the blue lead → move flyby → static tracking a survivor.
2. **demo-sam-strike** — a `builtin:striker` pair vs a SAM + AAA site: roll-in, a shallow dive to the
   release height, a level bombing run, and the site goes up at ~40 s (#1339). Ground static at the
   site, chase inbound, orbit the release, wide for the impact.
3. **demo-formation-tour** — a 4-ship on a waypoint route at dawn under a partly-cloudy sky; move slide
   along the formation, wide landscape static.
4. **demo-sensors-intercept** — a patrol → detect → intercept honest-sensing chain.
5. **demo-gallery-flyover** — a museum row of the builtin placeholder silhouettes (#886), a slow move
   camera touring the asset catalog.
6. **demo-bomber-defense** (multi-crew, #966) — two `builtin:bomber` hold a slow racetrack; each tail
   seat auto-crews a defensive gunner. Two fighters bore in and the turrets fire tracer down the rear
   quarter as they press the envelope.
7. **demo-carrier-swarm** (advanced vehicles, #585) — a `builtin:carrier` on its vessel force model
   with two naval escorts, and a six-ship drone **boids swarm** (`ai: "swarm …"`) flocking over the
   group; bow static → carrier orbit → move through the flock → drone chase → wide.
8. **demo-atc-scramble** (ATC, #673) — timed `atc_scramble` triggers launch AI departures from the
   builtin airfield in sequence; the cameras use fixed world points along the runway and climbout
   (scrambled aircraft are not mission-roster objects, so they can't be a `target`/`look_at` id).
9. **demo-ejection** (mission runtime, #584) — a walking `detonate` flak barrage wounds the lead into
   the eject band; the AI pilot auto-ejects and a replicating parachute appears where the airframe was
   lost. The camera pushes in on the hanging chute.
10. **demo-night-patrol** (spherical Earth, #468) — a two-ship night CAP over the builtin airfield
    under a geographically-correct star field + phase-lit Moon, with a low pass along the flattened,
    procedurally-marked runway.

Two authoring notes these demos exercise:

- **Camera targets must be mission-roster ids.** `orbit`/`chase` `target` and `look_at`-by-id resolve
  through the mission roster. Entities that appear at runtime — `atc_scramble` departures, an ejection
  parachute — have no mission id, so frame them with a fixed `[x, y, z]` `look_at`.
- **Effect-driven demos need faithful timing.** Trigger `do:` actions that mutate the world
  (`detonate`, `atc_scramble`, `spawn`) run on the sim callback queue; `fl-server --mission-report`
  drains that queue each tick, so a report run reproduces them deterministically — place burst/shot
  coords against a report run and they stay in sync.

### Carrier launch/recovery and rotorcraft are out of the zero-pack set

`demo-carrier-swarm` shows the vessel + swarm, not catapult/arrested-recovery flight ops: there is no
zero-pack way to author an aircraft **on** a moving deck. (Builtin helicopter and multirotor models
DO exist since #1335 — `builtin:helicopter` / `builtin:multirotor`, on display in
`builtin:shape-gallery` — but no rotorcraft-aware AI controller flies them beyond a loiter, so a
rotorcraft showcase demo waits on that.) Deck-ops demos follow once a content pack provides
carrier-capable aircraft.

[#1375]: https://github.com/fighters-legacy/fighters-legacy/issues/1375
