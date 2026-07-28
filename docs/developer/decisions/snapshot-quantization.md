# Snapshot Quantization & Bit-Packing

> **Frozen decision record.** This page records a decision as it was made and is not
> maintained against current behaviour. For how the engine works today, see the
> [Developer Guide](../index.md).

This document describes the quantized, bit-packed `MsgWorldSnapshot` entity encoding introduced in
#515 (Epic B — wire-state quantization & snapshot scaling). It is a decision-record-style design
note; the normative wire spec lives in [network-protocol.md](../network-protocol.md).

## Motivation

The reference-environment characterisation (#505) measured per-client downstream at **~480 KB/s at
128 idle clients — 3.2× the ≤150 KB/s scale gate**. The dominant cost was the fixed, uncompressed
per-entity record: an 88-byte `MsgEntityEntry` (new/baseline) and a 64-byte `MsgEntityUpdate`
(steady state), broadcast for every visible entity every tick. A modern netcode quantizes these
fields; this epic does the same, targeting a ~3–4× reduction while staying transport-agnostic
(unchanged on the current `enet6`).

## Design

Each `MsgWorldSnapshot` is
`header(24) → origin table(originCount × double[3]) → stitched record stream(bitstreamBytes) → TLV
block`. Since #775 the whole body after the header may additionally be one **zstd frame**
(`MsgWorldSnapshotHeader::flags` bit 0 + `uncompressedBytes`; engine-layer, transport-agnostic, raw
fallback when it does not strictly win) — everything below describes the *decompressed* body; the
codec seam is `engine/net/SnapshotCompression.h` and the framing is specified in
[network-protocol.md](../network-protocol.md). Positions are quantized relative to a **shared per-region origin** — the floor of the
entity's position onto a fixed `kOriginGridM` (~65 km) grid (`SnapshotCodec::originForPos`) — instead
of the receiving peer's position, so the sim encodes each entity **once per tick** (a full and a delta
blob) and each peer's snapshot is assembled by *stitching* the pre-encoded blobs (memcpy), not
re-quantizing per peer (#725). The header carries `recordCount` + `originCount`; the origin table holds
the distinct grid origins this snapshot's records reference (deduped); each record is prefixed with an
origin-index varint into that table.

### Record layout (one stitched entity, byte-aligned)

| Field | Encoding |
|---|---|
| `originIndex` | unsigned varint into the origin table (written at stitch time, not baked into the blob) |
| `idx` | unsigned varint of the **absolute** entityIdx (peer-independent → blobs stitch in any order) |
| `full` | 1 bit — full record (carries typeIndex + factionIndex + gen) vs. delta |
| `genPresent` | 1 bit — generation is on the wire (else the client reuses its cache) |
| `omegaPresent` | 1 bit — angular rates present (set only for the receiving peer's own entity) |
| `gen` | 16 bits, only if `genPresent` |
| `typeIndex` | varint, only if `full` |
| `factionIndex` | 16 bits, only if `full` (#860) — `FactionRegistry` index, client-cached alongside `typeIndex` |
| position | 3 × `kPosBitsPerAxis` (22), signed offset from the record's shared grid origin at `kPosStepM` (0.125 m) |
| orientation | 2-bit dropped-component index + 3 × `kQuatBits` (10) — smallest-three |
| velocity | 3 × `kVelBits` (18), range ± `kVelMaxMps` (2000) |
| omega | 3 × `kOmegaBits` (12), range ± `kOmegaMaxRadS` (20), only if `omegaPresent` |
| loadout block | 64 bits, only if `omegaPresent` (#625): `selectedStation`(8) + `stationRounds`(16) + `weaponFlags`(8, bit 0 = seeker locked) + `payloadMassKg`(16, 1 kg steps, clamped [0, 65535]) + `payloadCd0`(16, 1e-5 steps, clamped [0, 0.65535]) |
| byte fields | `damageLevel`(3) + `engineFailFlags`(5) + `throttle`(7) + `fuelPct`(7) + `abEngaged`(1) + `playerOwned`(1) |
| padding | zero bits to the next byte boundary (so the next record's `originIndex` starts on a byte) |

Constants live in `engine/net/SnapshotCodec.h` and are tuned against the bot_swarm
`downstream_kbs_per_client` metric. Representative blob sizes: a steady-state delta blob is
**24 bytes**, a full own-entity blob (typeIndex + factionIndex + gen + omega + loadout) is **41 bytes**; the stitched record
adds the origin-index varint (1 byte for a small index). These blob sizes are locked by a golden-bytes
test in `test_snapshot_codec`. The encode-once trade adds a small per-record overhead (origin index +
byte alignment) plus the per-snapshot origin table in exchange for O(entities) encode instead of
O(peers × visible).

### Why these choices

- **Shared-grid-origin-relative position (#725).** Storing a `double` per entity is wasteful; storing
  absolute `float` loses precision at planet scale. Encoding the offset from a shared **grid-cell**
  `double` origin keeps fine resolution everywhere while making a record peer-independent: the same
  quantized blob is valid for every peer, so it is encoded once per tick and stitched (memcpy) into
  each peer's stream. `kPosBitsPerAxis = 22` at `kPosStepM = 0.125` covers ±262 km — far beyond the
  ~65 km grid cell, so the offset always fits. The grid origins actually referenced by a snapshot are
  carried (deduped) in its origin table as exact `double`s, so precision is uniform at planet scale.
  (The old per-peer `frameOrigin` gave the peer's *own* entity a near-exact position as an artifact of
  `frameOrigin == own position`; under the shared origin the own entity is quantized to 0.125 m like
  every other entity.)
- **Smallest-three quaternion.** A unit quaternion's largest component is reconstructed from the
  other three (each in ±1/√2), so 32 bits replaces 16 bytes with imperceptible error after
  renormalization.
- **Omega only for the own entity.** Body-frame angular rates are consumed solely by client-side
  prediction reconciliation, which only runs for the player's own entity. Every other record omits
  them.
- **Loadout rides the same own-record bit (#625).** Selection, rounds, weapon flags, and the LIVE
  payload (mass + drag of what is still on the rails) matter only to the owning client — the HUD
  weapon line and `ClientPrediction`, which re-resolves its `PayloadEffect` from the record so a
  released store changes client-predicted physics the same tick it changes the server's. Other
  peers see stores as spawned projectile entities, not as loadout state. Reusing the `omegaPresent`
  gate costs no new flag bit; the per-type static payload in `MsgEntityTypeDef` remains only as the
  pre-first-snapshot fallback.
- **Generation only when it changes.** In steady state an entity's generation never changes, so the
  16-bit field is replaced by a single `genPresent = 0` bit; the client reuses its cache. A
  generation change is, by construction, classified as a `full` record on the server.

## Portability

The codec is **byte-identical on every supported target** (a Linux server and a Windows client must
agree bit-for-bit):

- Bits are assembled MSB-first with shifts/masks into a `uint8_t` buffer — never `memcpy` of a
  native multi-byte int — so byte order never reaches the wire.
- Quantization is arithmetic (`value × scale`, rounded with `std::lround`), never an IEEE bit-cast,
  so float representation never reaches the wire.
- Signed values use **offset-binary**, so only unsigned shifts/masks are ever applied (no
  implementation-defined signed-shift behaviour).
- The accumulator is `uint64_t` and each read/write moves ≤ 32 bits with ≤ 7 leftover bits, so the
  running width stays ≤ 39 bits — no shift-by-≥width UB (enforced by the ASan/UBSan CI job).
- NaN/Inf inputs are clamped before any float→int cast.
- The bitstream is byte-addressed, so there are no misaligned multi-byte loads on ARM64; only the
  24-byte header is read via `WireCodec readMsg` (memcpy); the origin table's `double`s are 8-aligned after it.

## Validation

- `tests/test_snapshot_codec.cpp` — bitstream round-trip (incl. truncated-buffer fail-closed),
  smallest-three for all four largest-component cases, clamp paths, planet-scale position, golden
  byte sizes, and a bandwidth-regression guard (records strictly smaller than the old 64 B).
- `tests/test_world_broadcaster.cpp` — server encode + full/delta/baseline/respawn behaviour, the
  TLV-after-bitstream offset, and the 3D (XYZ) interest cull (#402).
- `tests/test_client_net_event_handler.cpp` — client decode, the typeIndex/gen cache, and the
  stale-gen guard.
- **Bandwidth acceptance** is measured end-to-end by the bot_swarm `downstream_kbs_per_client`
  metric against the ≤150 KB/s gate (see [load-testing.md](../load-testing.md)).

## Relationship to the rest of Epic B

This change is the encoding layer only. The per-client priority/budget snapshot scheduler (#516 —
landed) builds directly on top: it reuses `estimateRecordBytes()` (added here, mirroring the encoder's
bit layout) to fit the highest-relevance records into a per-client byte budget, and adds the
`SnapshotDespawn` TLV + client-side entity retention so budget-deferred entities don't flicker. See
`engine/net/SnapshotScheduler.{h,cpp}` and [network-protocol.md](../network-protocol.md). Client-acked
delta baselines (#517 — landed) build on this codec's per-record `full` bit: the server keys
full-vs-delta off the snapshot tick each client echoes in `MsgClientInput`/`MsgHeartbeat` (the ack),
re-sending a full every tick until the peer confirms it — no wire change to this codec. Selective-ack
identity precision (#566 — landed) tightens that confirmation: the client→server ack carries a 32-bit
bitmask (`ackMask`, `engine/net/AckWindow.h`) of recently **decoded** ticks, so the server confirms the
*specific* tick a full was sent in rather than a high-water mark — closing the residual where acking a
later tick could falsely confirm a full the client never decoded, and retiring the #517 deferral guard.
Adaptive send-rate/congestion response (#518) builds on the codec and the scheduler, and remains a
separate sub-task of #495.
