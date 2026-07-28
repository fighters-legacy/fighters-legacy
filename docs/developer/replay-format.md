# `.flrep` — Replay File Format

**Status:** implemented as of v0.3.12. The writer and reader are `engine/replay/`
(`ReplayWriter`/`ReplayReader`, #643), the server records through `WorldBroadcaster`'s replay tap
into `server/fl-server/ReplayRecorder`, the client plays back through `ReplayPlayer` (#41), and the
determinism gate compares state hashes computed from the file (#644). This document was written
first, deliberately, because a replay file is the first thing this engine produces that **outlives
the build that wrote it** — it remains the contract, not a description of the code.

Related: [`network-protocol.md`](network-protocol.md) (the snapshot codec this reuses),
[`snapshot-quantization.md`](decisions/snapshot-quantization.md) (the per-entity bit layout),
`engine/net/InputTraceFormat.h` (FLIT, the on-disk precedent).

---

## 1. What a replay is, and what it is not

A `.flrep` file is a **recording of what the server broadcast**, not a recording of what any client
drew and not a script the sim re-runs. Playback feeds the stored entity states into the existing client
presentation path, so the renderer cannot tell a replay from a live session.

That choice is what makes the format tractable. The alternative — storing inputs and re-simulating —
would make every replay a hostage to the flight model: a fix to `AeroForces.cpp` would silently change
what a year-old replay *shows*, and the file would stop being a record of anything. Inputs are already
recorded separately as FLIT traces for the load harness, and those two formats answer different
questions: FLIT asks "what did the pilot do", `.flrep` asks "what happened".

Consequences worth stating up front:

- A replay is **not** a determinism proof on its own. #644 gets that by recording a session, replaying
  it, and comparing per-tick state hashes — the file is the instrument, not the claim.
- A replay contains only what was **broadcast**. Anything the server never put on the wire (a peer's
  raw input, server-internal AI state) is not in the file and cannot be recovered from it.
- A replay is not authoritative for anti-cheat. It records the server's own view, so it proves what the
  server believed, which is a different thing from proving what was true.

---

## 2. Compatibility — the rule FLIT does not need

FLIT deliberately froze `version = 1` and documents why: its producer and consumer live in this repo
and land together, so a version bump would be a compatibility promise to a party that does not exist.

**That reasoning does not transfer.** Players keep replays and share them. A `.flrep` written by
v0.4.0 will be opened by v0.6.0, by a build the recorder had never heard of. So the format carries a
real two-part version and real rules:

| Change | Version bump | Reader behaviour |
|---|---|---|
| A new optional section, or new fields appended to an existing section's tail | **minor** | Reads it. Skips sections it does not recognise via their declared length. |
| A field's meaning changes under an unchanged name; a section is removed; the record layout changes | **major** | **Refuses the file** with an error naming both versions. |

Two rules make this enforceable rather than aspirational:

1. **Every section is length-prefixed.** A reader that does not recognise a section id seeks past it.
   This is what makes "additive is minor" true rather than hopeful.
2. **A reader refuses a *newer major* outright** and says so — "this replay was recorded by
   Fighters Legacy 0.7 (format 3.0); this build reads format 2.x". A silent partial read that renders a
   plausible-but-wrong flight is worse than a clear refusal, because nobody would notice.

A reader accepts any **older** major it still ships support for, or refuses with the same clarity.

---

## 3. Byte-level conventions

- **Little-endian, always**, on every field, written byte by byte. Follow FLIT's `putU32LE`/`getU32LE`
  helpers (`engine/net/InputTraceFormat.h`): never `memcpy` a native word into the file. A replay
  recorded on one machine must play on any other, and the wire structs' "naturally aligned so memcpy is
  safe" reasoning does not apply to a file that crosses machines and years.
- **Floats** are IEEE-754, stored via their bit pattern with the same explicit helpers.
- **Strings** are length-prefixed UTF-8, never NUL-padded fixed arrays. The wire uses `char[64]` because
  a packet wants a fixed stride; a file wants to not truncate a callsign.
- **All file paths use `std::filesystem::path`.** `InputTraceWriter` takes a `std::string` path, which
  is precisely the Windows-UTF-8 bug `engine/config/ConfigFile.h` exists to avoid — a player whose
  profile directory contains non-ASCII cannot write a trace. `.flrep` must not repeat it, and #643
  should fix FLIT in passing.

---

## 4. File layout

```
+--------------------------------------------------+
| File header (fixed prefix, then length-prefixed)  |
+--------------------------------------------------+
| Section: entity-type manifest                     |
| Section: roster / name table                      |
| Section: (optional) camera track                  |
| ... unknown sections are skipped by length ...    |
+--------------------------------------------------+
| Frame stream: zstd-framed chunks                  |
|   chunk = [keyframe tick][delta tick]...          |
|   each tick = entity records + event records      |
+--------------------------------------------------+
| Index (trailer): keyframe tick -> byte offset     |
+--------------------------------------------------+
```

### 4.1 File header

| Field | Type | Notes |
|---|---|---|
| `magic` | `char[4]` | `"FLRP"` |
| `formatMajor` | `u16` | Refuse a file whose major exceeds what this build reads |
| `formatMinor` | `u16` | Additive-only within a major |
| `engineVersion` | string | The recorder's version, e.g. `"0.4.0"` — for diagnostics and the refusal message |
| `tickRateHz` | `u32` | Sim ticks per second (60 today); playback timing derives from this, never a hardcoded constant |
| `planetRadiusM` | `f64` | **Load-bearing.** See below. |
| `startUnixSeconds` | `u64` | Wall-clock recording start, for display and for ACMI export |
| `missionId` | string | Mission/map identity, empty for a free-flight session |
| `sessionFlags` | `u32` | Reserved; bit 0 = the session ran a mission, bit 1 = a match was scored |

**Why `planetRadiusM` is in the header and not assumed.** The planet radius is session-configurable
(`[world] planet_radius_m`, and it reaches clients in `MsgConnectAck`). Every geodetic conversion —
`worldToGeodetic` in `engine/flight/Geodetic.h`, and therefore every latitude, longitude and altitude a
replay reports — is a function of it. A replay that assumed Earth would not fail loudly on a
non-Earth session; it would render an aircraft at a *plausible wrong altitude*, and the ACMI export
built on it (#923) would produce a track file that looks entirely reasonable and is wrong. The radius is
one `f64`. Store it.

### 4.2 Sections

Each section is `[id: u16][byteLength: u32][payload]`. A reader dispatches on `id` and seeks
`byteLength` past anything it does not know.

| Id | Section | Required | Contents |
|---|---|---|---|
| `0x0001` | Entity-type manifest | yes | One record per entity type present: type index, id string, display name, category, projectile kind. The replay's equivalent of the `MsgEntityTypeDef` set a live client receives in ConnectAck — without it a stored `typeIndex` is a number with no meaning. |
| `0x0002` | Faction table | yes | Faction index → id, display name (the `MsgFactionDef` mapping). |
| `0x0003` | Roster / name table | yes | Participant id → callsign, and the mission-object-id → entity mapping (`MsgMissionRoster`). This is what lets a replay say "Maverick" rather than "entity 7", and what ACMI export needs for its per-object metadata. |
| `0x0004` | Camera track | no | Recorded or authored camera shots (the `cameras:` / `ShotDirector` vocabulary), so a cinematic pass can be replayed rather than re-flown. |

Sections are written **before** the frame stream so a reader can populate its tables in one forward
pass, without seeking.

### 4.3 Frame stream

The body is a sequence of **zstd frames** produced through `engine/net/SnapshotCompression.h`
(`engine-compress`), which is already documented as deterministic for identical input. Compression is
per *chunk*, not per tick: a chunk holds one keyframe tick plus the delta ticks that follow it, so a
seek decompresses exactly one chunk.

`engine-protocol` must stay stdlib-only, so the codec that writes entity records stays in
`engine-protocol` and the zstd framing lives one layer out — the same split `engine-net` already uses.

Within a tick:

| Field | Type | Notes |
|---|---|---|
| `tickIndex` | `u64` | Absolute sim tick |
| `flags` | `u16` | bit 0 = keyframe |
| `recordCount` | `u16` | Entity records following |
| `eventCount` | `u16` | Match-event records following |
| entity records | — | `SnapshotCodec` `QuantEntity` records, exactly the wire encoding |
| event records | — | `MatchEventLog` records (#600) — kills, spawns, chat, admin actions, alert-level changes |

**A keyframe tick is a tick whose entity records are all `full`.** `SnapshotCodec` has no separate
keyframe concept — full-vs-delta is caller policy expressed by a per-record presence bit — so a replay
keyframe needs no new codec, only the discipline of emitting every entity full on a cadence. Keyframes
are the seek points: scrubbing means "find the keyframe at or before the target tick, decompress its
chunk, roll forward". That is the whole of scrub support, and it is why keyframe cadence is a
recording-time knob rather than a playback problem.

Reusing the wire encoding verbatim is deliberate: the recorder serialises what the broadcaster already
built, so there is one quantization implementation rather than a second one that can disagree about
what 0.125 m means.

Interleaving events with entity state in the same tick (rather than in a separate stream) keeps a kill
and the frame it happened on inseparable — which is what a killcam, a debrief, and an ACMI event line
all need.

### 4.4 Index trailer

A trailing table of `keyframe tickIndex → byte offset`, plus the offset of the table itself in the last
8 bytes. Written last because keyframe offsets are not known until the stream is complete; found first
by a reader seeking to the end.

A file whose trailer is missing or corrupt (an interrupted recording — a crash, a killed server) is
**still playable from the start**: the reader falls back to a forward scan and rebuilds the index,
losing seek performance but not the recording. A replay of the session that crashed the server is
exactly the replay someone most wants.

---

## 5. Reading untrusted files

A `.flrep` is a file a player downloads from a stranger. The reader treats it as hostile input:

- Every length is bounds-checked against the remaining file size before allocation. A declared
  `recordCount` is never trusted enough to reserve on.
- Decompressed chunk size is capped; a zstd frame declaring a huge output is rejected, not honoured.
- Malformed input produces a clear error and no partial state — the `BitReader` "fail closed on
  truncation" contract that `engine/net/BitStream.h` already provides.
- The reader joins `fuzz/` with a seed corpus, as FLIT's does. It parses attacker-controlled bytes;
  that is the rule.

---

## 6. What this format is designed to carry later

The fields above are not all needed by #643/#41. Several exist because designing them in now costs a
header field and designing them in later costs a format major.

**ACMI export (#923, M5).** Tacview's format wants, per object per sample: latitude, longitude,
altitude, attitude, plus object metadata and discrete events. Every one of those is derivable from what
is specified here — geodetic position from `worldToGeodetic` with the **stored** planet radius, object
names and coalitions from the roster and faction sections, and weapon/kill events from the interleaved
event records. #923 is an exporter over this file, not a second recording path.

**Match-log shipping (#547, Phase 5).** Reads the same `MatchEventLog` records this file already
interleaves.

**The determinism gate (#644).** Needs a per-tick state hash over the canonical `QuantEntity` stream.
The hash is not stored in the file — it is *computed* from it on both the record and replay side, and
compared. Storing it would let a recorder assert its own correctness, which is not a test.
`engine/net/ReplayStateHash.h` is that primitive, and it hashes the **quantized integer domain**:
quantization is lossy, so a record that goes through the codec can never equal its input in float
space, but it must be exactly equal after quantization — and that comparison is also free of the
float-ordering ambiguity that would make the gate flaky across workers and platforms.

---

## 7. Items left open by the spec, and how they were settled

- **Keyframe cadence** is `[replay] keyframe_interval_ticks`, default **120** (2 s at 60 Hz), range
  `[15, 3600]`. The spec said to measure it rather than guess, so it was measured: on a 40-entity
  world with everything moving, a 25-second recording produced 701 bytes/tick at a 60-tick cadence,
  700 at 300 and 700 at 600 — **0.4 % across a tenfold range**. The expected size/seek tradeoff
  barely exists at this workload, because a delta record for a moving entity is nearly the size of a
  full one and zstd absorbs the repeated keyframes. Cadence is therefore chosen for seek feel, and
  set low. A world of mostly-static entities is the case where the tradeoff is real.
- **Rotation bounds** are `[replay] max_file_mb` (default 256) and `max_files` (default 20). `max_files`
  bounds the **directory**, not one session — a server recording one file per match would otherwise
  fill a disk one perfectly-rotated file at a time. Rotation happens at a chunk boundary, so every
  rotated file is independently playable.
- **Section `0x0004` (camera track)** remains reserved and unwritten. #41's playback drives the live
  camera path (`makeCameraView`) rather than a stored track, so nothing needs it yet; the id stays
  claimed so nothing else takes it.
- **Two things the implementation added**, both additive within format 1.0: the header carries
  `keyframeIntervalTicks` (a reader shows seek granularity without scanning), and the index trailer
  carries the recording's first and last tick (a replay browser shows a duration without
  decompressing anything). A file with no trailer recovers both from the forward scan.
