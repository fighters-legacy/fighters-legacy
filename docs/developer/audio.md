<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Audio Subsystem

Everything the player hears, and the one rule that shapes all of it: **the engine ships no audio
content**, so every sound has a compiled-in procedural fallback and the whole subsystem is provable
with zero content packs.

The HAL is `platform/IAudio.h`, implemented by `platform/openal/OALAudio` (OpenAL Soft). Engine-side
managers consume `IAudio*` by injection and never see an OpenAL header.

## A null `IAudio` is a clean no-op

Every manager takes `IAudio*` and no-ops against a null one, which is what makes the whole subsystem
testable headless — **CI never opens an audio device.** Nothing here is a hard dependency on a sound
card: the managers hold buffers and sources they simply never hear.

## Builtin PCM is byte-stable, never random

`generateBuiltinSfx` (`engine/audio/SfxBuiltinSounds.h`) and `generateBuiltinMusic`
(`engine/audio/MusicBuiltinTracks.h`) synthesise their PCM from **a fixed hash and IEEE float math —
never `rand()` and never the clock**. The determinism is deliberate: it makes the zero-pack sandbox
sound the same on every machine and every run, which is what lets a test assert on the output at all.

## Managers

| Manager | Header | What it owns |
|---|---|---|
| `SfxManager` | `engine/audio/SfxManager.h` | Positional weapon SFX (#631): a 16-voice steal-oldest pool, a full-decode buffer cache, and **camera-relative** 3D positioning — `play(preset, worldPos, cameraOrigin, settings)` subtracts the camera origin and puts the listener at the origin, the same invariant the renderer uses. Preset vocabulary `sfx.gunfire` / `launch` / `release` / `impact` / `explosion`; a content-pack asset name overrides the compiled-in procedural fallback. |
| `MusicManager` | `engine/audio/MusicManager.h` | Streams OGG music through two OpenAL sources (primary + crossfade). `loadPlaylist` / `setState(GameState)` / `update(dt, masterVol, musicVol)`. A `shuffle` state Fisher-Yates shuffles on entry and re-shuffles each loop cycle; `setRng` injects a deterministic RNG for tests. `openSlot` resolves a `builtin:` track **before** `AssetManager` and streams its looping PCM through the same crossfade buffers as a real OGG. |
| `WarningToneManager` | `engine/audio/WarningToneManager.h` | Cockpit stall + overspeed tones (#957): activation, exit hysteresis, live gain, the in-flight gate, and channel independence. |
| `EngineAudioManager` | `game/fighters-legacy/EngineAudioManager.h` | Continuous engine + aerodynamic layers (#959): the throttle/airspeed → pitch/gain mapping, own-ship head-locked audio, and positional flyby Doppler. |
| `VoiceCalloutManager` | `engine/audio/VoiceCalloutManager.h` | Crew/GCI callout playback with the subtitle queue. |
| `VoiceMixer` / `VoiceChat` | `engine/voice/` | In-game radio comms (Epic J). See [Voice](voice.md). |

## Content-pack audio

Music playlists are `data/playlist.toml` in a pack — see
[Asset formats](../modding/formats.md#music-playlist--toml). When no pack provides one, the client
uses `builtinDefaultPlaylist()`, which wires `builtin:music-menu` / `-patrol` / `-combat` to the game
states, so **music plays with zero content**.

Effects routing is the game layer's `ClientEffectRouter`, which drives `SfxManager` alongside the
particle effects and `HapticController` (own-ship launch → `notifyOrdnanceRelease`; the
missile-approach warning is deliberately withheld — it would be a wallhack, tracked on #529).

## Testing

Audio is fully testable headless: **no device is ever opened in CI.** Every audio test drives a
no-op or tracking `IAudio` double instead.

Sound is *not* verified by listening; the assertions are on buffer/source lifecycle, gain and
position, state transitions, and the byte-stability of the builtin PCM. `test_music_manager` carries
a design note worth reading before adding cases: a no-op `processedBufferCount` returning 0 means the
EOF handler only fires when `openSlot` fails, so track-advance logic is driven with a pack that
returns `nullopt`, and a pack returning valid OGG is used only for `setState()`-entry cases.
