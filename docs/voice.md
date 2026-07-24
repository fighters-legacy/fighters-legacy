<!--
SPDX-FileCopyrightText: 2026 Fighters Legacy contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->

# In-Game Voice Comms (Epic J)

Voice in a combat flight sim is **radio**, not lobby chat. This document is the design record and
the operator/player reference for the radio-net model: what it is, why it is shaped this way, and
where each piece lives.

Implemented across [#531](https://github.com/fighters-legacy/fighters-legacy/issues/531) (capture,
codec, playback, positional mix), [#532](https://github.com/fighters-legacy/fighters-legacy/issues/532)
(transport, routing, server relay) and
[#925](https://github.com/fighters-legacy/fighters-legacy/issues/925) (the presentation layer),
under epic [#499](https://github.com/fighters-legacy/fighters-legacy/issues/499).

## The model: nets, not frequencies

The unit of routing is a **net**: a named channel with a membership rule and a presentation
profile. Nets are **data** — a server operator or a theater pack adds `tanker` or `awacs` without an
engine change (see `[[voice.nets]]` in [fl-server-config.md](fl-server-config.md#voice--in-game-voice-comms-epic-j-532)).

There is deliberately **no frequency dial**. Tuning 251.000 to hear the tanker is ceremony, not
gameplay: it is undiscoverable for a new player, and it fights the arcade-to-sim pillar (which says
depth should be opt-in, never a prerequisite). Named nets are what SRS's frequency simulation is
actually *used* for, with none of its setup cost — and a mission or campaign that wants a tanker net
gets one by naming it.

| Kind | Who hears it |
|---|---|
| `global` | Every admitted peer |
| `team` | Same faction as the speaker |
| `flight` | The speaker's formation (the [#610](https://github.com/fighters-legacy/fighters-legacy/issues/610) element → flight → package tree) |
| `proximity` | Every peer within `range_m`, **regardless of side** |
| `atc` | Everyone, including teamless spectators — it is how a player who is not flying talks to the tower |

The default (compiled-in) stack is `team` (the default PTT net), `flight`, `atc`, and a positional
`proximity` net at 3 km, so **voice works with zero server configuration**.

### Rules that are easy to get wrong

- **The sender is always excluded from the recipient set.** A network round trip of your own voice
  is the single most disorienting thing a voice system can do. Sidetone, if we ever add it, belongs
  on the client where it costs no round trip.
- **A flight lead is a formation's *anchor*, not a member.** Both readings are checked, or every
  lead is silently missing from their own flight net.
- **A peer with no aircraft has no team and no proximity**, so it may not transmit on those nets.
  "My team" with no referent would reach either nobody or everybody, and neither is a good guess. It
  can still *listen*, and it can always use `atc`.

## Architecture

    microphone ──► IAudioCapture ──► VoiceActivityGate ──► VoiceEncoder ──► MsgVoiceFrame
                   (platform/sdl3)     (PTT / VOX)          (Opus)              │
                                                                                ▼
                                                                    fl-server: VoiceRouter
                                                                    (membership → recipients;
                                                                     NEVER decodes the audio)
                                                                                │
    speakers ◄── IAudio ◄── RadioFilter ◄── VoiceDecoder ◄── VoiceJitterBuffer ◄─┘
                            (#925 DSP)                        (reorder + PLC)     MsgVoiceRelay

| Piece | Lives in |
|---|---|
| Net vocabulary, table, builtin stack, radio DSP | `engine/voice/RadioNet.*`, `engine/voice/RadioDsp.*` (`engine-radio`) |
| Opus wrapper, jitter buffer, keying gate, mixer, client facade | `engine/voice/` (`engine-voice`) |
| Microphone capture HAL + SDL3 backend | `platform/IAudioCapture.h`, `platform/sdl3/SDL3AudioCapture.*` |
| Routing rules (pure) | `engine/net/VoiceRouter.h` |
| Server relay, rate limit, mute | `engine/net/WorldBroadcaster.cpp` |
| Wire messages | `engine/net/GameProtocol.h`, documented in [network-protocol.md](network-protocol.md) |
| HUD net indicator | `game/fighters-legacy/VoiceOverlay.h` |

### The server never decodes audio

`fl-server` understands exactly one thing about a voice frame: **how many bytes it is**. It checks
the length, checks the sender is on the net, and copies the bytes to that net's recipients. No
decode, no mix, no transcode.

Two consequences, both deliberate:

1. Voice at 128 players costs the server almost nothing — the reason the epic could be pulled
   forward into the 128+ re-target at all.
2. The codec is a **client-to-client contract**. The 48 kHz / 20 ms / VOIP operating point can
   change without a protocol change, because nothing on the server has an opinion about it.

### Why voice is not on the snapshot channel

ENet sequences *unreliable* packets **per channel** and discards one that arrives older than the
channel's last. Two independent unreliable streams sharing a channel therefore knock each other
out: at ~50 voice frames/s against 60 snapshots/s, each stream's packets look stale relative to the
other's and both lose frames. Voice rides `kNetChVoice` (channel 2) via `INetwork::sendChannel`,
whose base implementation forwards to `send()` so no other backend and no mock had to change.

### Why voice needs its own jitter buffer

`engine/net/JitterBuffer.h` is depth-only: it holds control samples and stale-repeats the last one
on underrun, because a repeated stick position is a perfectly good guess. Audio needs three things
it does not do:

1. **Reordering** by sequence number. A late control packet is worthless and gets discarded; a late
   voice frame is a *syllable*.
2. An explicit **loss signal**. Underrun must reach the decoder as "conceal this", not "repeat the
   last one" — repeating an Opus frame produces a robotic stutter, while Opus's own PLC extrapolates
   the glottal pulse and is nearly inaudible for a frame or two.
3. **Prefill**, or the first jitter spike underruns immediately and every transmission opens with a
   stutter.

## Presentation (#925)

What separates a flight sim's radio from lobby voice is almost entirely presentation — the bytes are
the same Opus either way:

- **Band-limiting and compression.** A 300–3000 Hz biquad pair into a `tanh` soft clipper (not a
  hard clamp, whose odd harmonics read as digital distortion rather than a compressed channel), plus
  an optional carrier hiss so a live net does not sound dead.
- **A key-down click and a squelch tail.** Generated, not sampled: deterministic byte-stable
  procedural PCM (the `SfxBuiltinSounds` contract — a fixed hash, never `rand()` or a clock), so the
  radio sounds like a radio in the zero-content-pack sandbox and identical on every machine. These
  cues are why the wire carries an **explicit end-of-transmission marker** rather than leaving the
  boundary to a receive timeout, which would put the squelch a timeout late.
- **Ducking.** One smoothed envelope for the whole radio, not per net — the ear does not care which
  net is live, only that someone is talking. It ducks the **music** and not the flight audio: the
  engine note and the RWR are information the pilot is flying on, and burying them under a radio
  call would trade one kind of deafness for another.
- **Subtitles.** Each new transmission pushes a `[NET] Callsign…` line onto the *same*
  `SubtitleQueue` the ATC callouts use. Human speech cannot be transcribed client-side, so the line
  names the speaker and the net — which is exactly what a player who cannot hear the audio needs,
  and what a player who can needs when a radio-filtered voice is hard to place.

### Human and synthetic traffic are the same path

ATC, AWACS and (later) Epic O TTS transmissions carry a `netId` on `MsgRadioTransmission` and are
played through the **same** `RadioFilter`, the same click and squelch, the same net gain and the
same ducking envelope as human voice. There is one implementation, not two that drift, and the
requirement it satisfies is that a human and a synthetic transmission are **indistinguishable in
presentation**.

## Player reference

| Action | Default | Notes |
|---|---|---|
| `PushToTalkPrimary` | `V` | Transmits on the selected net (TEAM by default) |
| `PushToTalkSecondary` | `B` | Transmits on the flight net; wins if both keys are held |
| `VoiceNetCycle` | `M` | Re-points the primary key at the next net |

All three are **held, never latched** — a latched mic is how a lobby ends up listening to someone's
kitchen. Two PTT keys rather than one because the pair of nets a pilot uses constantly is "my
flight" and "everyone on my side", and reaching a menu to switch between them mid-merge is exactly
the ceremony the model exists to avoid.

The bottom-left HUD shows the net the PTT key will use, a `TX` indicator whose brightness follows
the live mic level (the first thing to check when nobody answers), and who is currently on the air.

### Client settings (`[voice]` in `config/user.toml`)

| Key | Default | Notes |
|---|---|---|
| `enabled` | `true` | Master switch: neither transmit nor receive |
| `transmit` | `true` | `false` = listen-only; no capture device is opened at all |
| `input_device` | *(empty)* | Capture device **name**; empty = system default |
| `key_mode` | `"ptt"` | `ptt` / `vox` / `open`. A held PTT key always overrides VOX, never locks it out |
| `vox_threshold` | `0.03` | Linear RMS, `[0, 1]` |
| `mic_gain` | `1.0` | Pre-encode trim, `[0, 4]`; 100 % = unity |
| `bitrate` | `24000` | Encoder target bits/s, `[6000, 128000]` |
| `jitter_frames` | `3` | Playback de-jitter depth in 20 ms frames — 3 = 60 ms |
| `radio_effect` | `true` | Apply the DSP the server's net profile asks for |
| `subtitles` | `true` | Show the transmission line on the radio subtitle path |
| `ducking` | `0.55` | How far music drops while a net is live; 0 = off |
| `net_volume` | all `1.0` | Per-**net-index** receive volume, `[0, 2]`; 0 = muted |

Graphics/audio settings screen exposes the common subset (voice on/off, mic mode, mic device, mic
gain, radio effect, ducking, radio volume).

### Degradation

Every failure is soft and independent, and none of them is an error a player must resolve before
flying:

| Missing | Result |
|---|---|
| Capture device / permission | Listen-only |
| Opus encoder | Listen-only |
| Audio device (`IAudio` null, headless, CI) | Send-only; no `#ifdef` anywhere |
| Server voice disabled | Client is told at connect and the HUD says so |
| `voice.enabled = false` (client) | Clean no-op |

## Operator reference

See [fl-server-config.md](fl-server-config.md#voice--in-game-voice-comms-epic-j-532) for `[voice]`
and `[[voice.nets]]`. Admin commands:

- `voice` — the net table and the currently voice-muted peers, as the clients see them.
- `voice_mute <peerId>` / `voice_unmute <peerId>` — session-scoped **transmit** mute. A muted peer
  still hears every net: muting is a moderation action against what someone broadcasts, not a
  punishment that also blinds them to their own team.

The per-peer `frame_rate_limit` is a **bandwidth** bound, not anti-spam: a frame is fanned out to
every recipient on the net, so an unbounded sender costs *(recipients × bytes)*. Over-rate frames
are dropped **silently** — a reply to a flood is amplification.

## Security notes

- A client decodes bytes that reached it through a server which never inspected them. The **payload
  length cap** (`kMaxVoiceFrameBytes` = 400) is the only validation available, and it is enforced on
  both ends before anything reaches libopus.
- Frames are relayed only to peers the sender's net actually includes, so voice cannot be used as an
  amplification or intel channel across teams (`proximity` is side-agnostic **by design** — you can
  hear a nearby aircraft's transmission, which is a gameplay feature, not a leak).
- Voice inherits the transport's encryption: on the GNS backend the whole stream is
  curve25519 + AES-GCM. On enet6 (LAN / single-player) it is plaintext, like every other message.

## See also

- [network-protocol.md](network-protocol.md) — the wire messages and channel assignments
- [fl-server-config.md](fl-server-config.md) — `[voice]` / `[[voice.nets]]`
- [sandbox.md](sandbox.md) — the full key map
- [ai-architecture.md](ai-architecture.md) — the TTS / voice-line service that shares this path
