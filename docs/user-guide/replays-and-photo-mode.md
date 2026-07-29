# Replays & photo mode

Every match a server runs is recorded by default, and you can watch it back from any camera with a
scrubbable timeline.

## Replay playback (#41)

Every match is recorded by the server (`[replay]` in
[fl-server-config.md](../server-ops/server-config.md)). Open one from **Replays** in the main menu, or straight
from the command line:

    fighters-legacy --replay ~/.local/share/mkzsystems/fighters-legacy/replays/20260727-140233.flrep

Playback is a session with no server and no socket: the recorded world is published through the same
path a live server's snapshots take, so **every camera mode, the HUD and the terrain work exactly as
they do in flight**. There is no ownship — a recording is of the world — so a replay uses the
spectator controls: **Num1 / Num2** cycle which entity the camera views, and **F1 / F2 / F4** frame it
in Cockpit / Chase / Free.

| Action | Key |
|---|---|
| Pause / resume | Space |
| Scrub ±5 s | Left / Right arrow |
| Scrub ±30 s | Down / Up arrow |
| Jump to start / end | Home / End |
| Slower / faster (0.25× … 2×) | `-` / `=` |
| Toggle photo mode | P |

The transport bar along the bottom shows elapsed / total time, the current speed, and tick marks at
each **keyframe** — a scrub always lands on the keyframe at or before the target and rolls forward
from it, so the marks show where a seek will actually land.

### Photo mode (P)

Photo mode pauses playback (a still of a moving world is a screenshot, not a photograph), switches to
the free camera, and adds manual control of the shot:

| Action | Key |
|---|---|
| Zoom in / out (FOV 20°–120°) | Page Up / Page Down (hold Left Shift for 1° steps) |
| Roll the camera | `;` / `'` — not Q / E, which move the free camera you frame the shot with |
| Exposure −/+ (¼-stop, ±4 stops) | `-` / `=` |
| Export a PNG | Enter |

Exposure compensation is applied to linear HDR *before* the tonemap curve, so raising it recovers
real highlight detail rather than washing out an already-compressed image. Exports land in
`<user data>/screenshots/photo-<timestamp>.png`. Photo mode is available during live single-player
too, via the local pause.

---

