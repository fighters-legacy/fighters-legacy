# Multiplayer

Finding a server, joining it, and what to expect once you are on one.

## Why you can trust the netcode

The server is **authoritative**: it owns the world, and your client shows you its view of it. That
is what stops one player's modified client from deciding where everyone else's aircraft are.

Your own aircraft is **predicted locally** so it responds immediately to your stick rather than
after a round trip, and the prediction is reconciled against the server's view every snapshot. When
the two agree — the normal case — you see nothing. When they disagree slightly, the correction is
blended in rather than snapped, so a busy connection reads as softness rather than teleporting.

Other aircraft are interpolated between the snapshots you receive. Under packet loss the server
adapts: it lowers your snapshot rate and shrinks the per-snapshot budget rather than dropping you,
which is why a bad connection degrades into coarser updates instead of a disconnection.

If the *server* is overloaded rather than your link, it says so — the HUD distinguishes server load
from link congestion, because the two look identical from the outside and mean opposite things.

## Multiplayer connection

Pass `--connect` to join a remote `fl-server` instead of spawning a local single-player session.

| Flag | Description |
|---|---|
| `--connect <host[:port]>` | Connect to a remote fl-server. Port defaults to `4778` if omitted. IPv6 literals must be bracketed: `--connect [::1]:4778`. |
| `--aircraft <type-id>` | Request a specific aircraft in the connect handshake (#834), e.g. `--aircraft fl-base:f5e`. The server clamps to a registered type and falls back to its `[world] player_entity_type` default if the id is unknown. Omit to let the server choose. |
| `--observer` | Join as an **observer** (spectator) with no aircraft (#857) — a free ghost camera (press **F4**). The server must have `[world] allow_observers = true` (default). |
| `--operator-password <pw>` | Operator password for admin console commands on the remote server. Enables the in-game console commands (`spawn`, `kill`, `tp`, etc.) over the network. Takes precedence over the env var and user.toml. |
| `--mission <id>` | **Skip the menu** and launch straight into a single-player session with this mission — any builtin id (`builtin:sandbox`, `builtin:shape-gallery`) or pack mission stem. The id is forwarded to the embedded fl-server exactly as Instant Action forwards `builtin:sandbox`. |
| `--auto` | **Skip the menu** and enter the session the other flags describe: Free Flight alone, Join Server with `--connect` (composes with `--observer`). |
| `--screenshot <path>` | Write one PNG to `<path>` a few seconds into the Flight session, then exit — the in-engine capture (#909; no external screenshot tool needed). Pair with `--auto`/`--mission` for an unattended shot. |
| `--screenshot-frames <N>` | Frames after Flight starts before the `--screenshot` capture fires (default 600 ≈ 10 s at 60 fps) — raise it to let terrain/airports stream in first. |
| `--headless` | Run with **no window and no display** (#913): a swapchain-free renderer draws into owned images instead of presenting. Paired with a software Vulkan ICD (lavapipe) it renders with **no GPU** — the recorder's foundation. Combine with `--auto`/`--observer`/`--connect` and `--screenshot`/`--record`. Example: `DISPLAY= VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json fighters-legacy --headless --mission builtin:sandbox --auto --screenshot out.png`. |
| `--no-audio` | Run **silent**: skip opening an audio device entirely (#1117). For a deliberately quiet run — CI, an unattended `--screenshot`, a VM — where probing a driver you know is absent only costs warnings and startup time. It is **not** required on a machine with no sound: a failed device open already degrades to a silent game with a warning, because sound is not a prerequisite for flying an aircraft. |
| `--record <out.mp4>` | **Cinematic recorder** (#916): drive the camera from the mission's `cameras:` shots (via `ShotDirector`) and pipe frames to an ffmpeg H.264 mp4. Connect as an `--observer` (the camera is shot-driven, not flown). See `docs/developer/demo-recording.md`. |
| `--record-fps <n>` / `--record-res <WxH>` | Recording frame rate (default 30) and resolution (default 1280×720; also sets the headless framebuffer size). |
| `--record-png-dir <dir>` | Record a PNG sequence to `<dir>` instead of an mp4 — no ffmpeg needed (the fallback path). |
| `--shot-track <yaml>` | A cameras-only (or full-mission) YAML whose `cameras:` block drives the recorder — iterate shots without editing the shared mission. Defaults to the `--mission` file. |
| `--exit-on-mission-end` | Stop recording when the mission objective ends (consumes `MsgMissionOutcome`). |
| `--record-max-sec <n>` | Wall-clock recording cap (safety stop). |
| `--record-max-dup <n>` | Fail the run (non-zero exit) if duplicated frames — capture boundaries the sim didn't reach in time — exceed this. Bad video is loud, never silently shipped (default 300). Slow the server with `--time-rate quarter\|eighth` if you see dups. |

To avoid exposing the password in the process listing, use the `FL_OPERATOR_PASSWORD` environment variable instead of the CLI flag. Merge precedence: `--operator-password` CLI arg > `FL_OPERATOR_PASSWORD` env var > `[client].operator_password` in user.toml.

When `--connect` is given the main menu shows **Join Server** instead of **Sandbox (Instant Action)**, and the loading screen displays "Connecting to remote server…".

