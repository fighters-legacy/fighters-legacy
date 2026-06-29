# Snapshot Quantization & Bit-Packing

This document describes the quantized, bit-packed `MsgWorldSnapshot` entity encoding introduced in
#515 (Epic B — wire-state quantization & snapshot scaling). It is a decision-record-style design
note; the normative wire spec lives in [network-protocol.md](network-protocol.md).

## Motivation

The reference-environment characterisation (#505) measured per-client downstream at **~480 KB/s at
128 idle clients — 3.2× the ≤150 KB/s scale gate**. The dominant cost was the fixed, uncompressed
per-entity record: an 88-byte `MsgEntityEntry` (new/baseline) and a 64-byte `MsgEntityUpdate`
(steady state), broadcast for every visible entity every tick. A modern netcode quantizes these
fields; this epic does the same, targeting a ~3–4× reduction while staying transport-agnostic
(unchanged on the current `enet6`).

## Design

Each `MsgWorldSnapshot` is `header(40) → quantized bitstream(bitstreamBytes) → TLV block`. The
header carries a **per-snapshot `double frameOrigin[3]`** (the receiving peer's authoritative
position) and a `recordCount`. The body is a single bit-packed stream of `recordCount` entity
records, MSB-first.

### Record layout (one entity)

| Field | Encoding |
|---|---|
| `idxDelta` | unsigned varint of `entityIdx − previousIdx` (records are sorted by idx ascending) |
| `full` | 1 bit — full record (carries typeIndex + gen) vs. delta |
| `genPresent` | 1 bit — generation is on the wire (else the client reuses its cache) |
| `omegaPresent` | 1 bit — angular rates present (set only for the receiving peer's own entity) |
| `gen` | 16 bits, only if `genPresent` |
| `typeIndex` | varint, only if `full` |
| position | 3 × `kPosBitsPerAxis` (22), signed offset from `frameOrigin` at `kPosStepM` (0.125 m) |
| orientation | 2-bit dropped-component index + 3 × `kQuatBits` (10) — smallest-three |
| velocity | 3 × `kVelBits` (18), range ± `kVelMaxMps` (2000) |
| omega | 3 × `kOmegaBits` (12), range ± `kOmegaMaxRadS` (20), only if `omegaPresent` |
| byte fields | `damageLevel`(3) + `engineFailFlags`(5) + `throttle`(7) + `fuelPct`(7) + `abEngaged`(1) + `playerOwned`(1) |

Constants live in `engine/net/SnapshotCodec.h` and are tuned against the bot_swarm
`downstream_kbs_per_client` metric. Representative sizes at the current budget: a steady-state delta
record is **24 bytes** (vs. the old 64), a full own-entity record (typeIndex + gen + omega) is
**31 bytes** (vs. 88). These exact sizes are locked by a golden-bytes test in `test_snapshot_codec`.

### Why these choices

- **Frame-origin-relative position.** Storing a `double` per entity is wasteful; storing absolute
  `float` loses precision at planet scale. Encoding the offset from a per-snapshot `double` origin
  keeps fine resolution everywhere within the interest radius. `kPosBitsPerAxis = 22` at
  `kPosStepM = 0.125` covers ±262 km — comfortably beyond the default 200 km draw distance. Offsets
  beyond the encodable range are clamped (interest culling removes them first in practice).
- **Smallest-three quaternion.** A unit quaternion's largest component is reconstructed from the
  other three (each in ±1/√2), so 32 bits replaces 16 bytes with imperceptible error after
  renormalization.
- **Omega only for the own entity.** Body-frame angular rates are consumed solely by client-side
  prediction reconciliation, which only runs for the player's own entity. Every other record omits
  them.
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
  40-byte header is read via `WireCodec readMsg` (memcpy), with `frameOrigin` 8-aligned within it.

## Validation

- `tests/test_snapshot_codec.cpp` — bitstream round-trip (incl. truncated-buffer fail-closed),
  smallest-three for all four largest-component cases, clamp paths, planet-scale position, golden
  byte sizes, and a bandwidth-regression guard (records strictly smaller than the old 64 B).
- `tests/test_world_broadcaster.cpp` — server encode + full/delta/baseline/respawn behaviour, the
  TLV-after-bitstream offset, and the 3D (XYZ) interest cull (#402).
- `tests/test_client_net_event_handler.cpp` — client decode, the typeIndex/gen cache, and the
  stale-gen guard.
- **Bandwidth acceptance** is measured end-to-end by the bot_swarm `downstream_kbs_per_client`
  metric against the ≤150 KB/s gate (see [load-testing.md](load-testing.md)).

## Relationship to the rest of Epic B

This change is the encoding layer only. The per-client priority/budget snapshot scheduler (#516 —
landed) builds directly on top: it reuses `estimateRecordBytes()` (added here, mirroring the encoder's
bit layout) to fit the highest-relevance records into a per-client byte budget, and adds the
`SnapshotDespawn` TLV + client-side entity retention so budget-deferred entities don't flicker. See
`engine/net/SnapshotScheduler.{h,cpp}` and [network-protocol.md](network-protocol.md). Client-acked
delta baselines (#517 — landed) build on this codec's per-record `full` bit: the server keys
full-vs-delta off the snapshot tick each client echoes in `MsgClientInput`/`MsgHeartbeat` (the ack),
re-sending a full every tick until the peer confirms it — no wire change to this codec. Adaptive
send-rate/congestion response (#518) builds on the codec and the scheduler, and remains a separate
sub-task of #495.
