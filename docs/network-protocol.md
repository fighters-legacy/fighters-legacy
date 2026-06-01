# Network Wire Protocol

This document describes the binary message format used between `fl-server` and game clients
over ENet UDP. All structs are defined in `engine/net/GameProtocol.h`.

## Channels

| Channel | Constant | Delivery | Use |
|---------|----------|----------|-----|
| 0 | `kNetChReliable` | Ordered, guaranteed | Handshake messages, client input frames |
| 1 | `kNetChUnreliable` | Best-effort datagram | World-state snapshots |

ENet enforces ordering within each channel; reliable packets are retransmitted until
acknowledged. Unreliable packets may be dropped or arrive out of order — clients tolerate
this via dead-reckoning (`rendered_pos = pos + vel × alpha × kTickDt`).

## Implementation Rules

- All wire structs use `#pragma pack(1)` — no implicit padding.
- All multi-byte fields are **little-endian**. All supported targets (x86-64, arm64,
  Apple Silicon) are natively little-endian; no byte-swapping is performed at the sender
  or receiver.
- Always use `std::memcpy` to read/write struct fields from/to raw buffers. Direct pointer
  casting of unaligned wire data is undefined behaviour caught by UBSAN.
- The first byte of every packet is the `MsgId` discriminator.

## Messages

| MsgId | Value | Direction | Channel | Size | Purpose |
|-------|-------|-----------|---------|------|---------|
| `ConnectAck` | `0x01` | server→client | reliable | 12 + N×196 bytes | Handshake on connect; assigns entity slot and delivers type registry |
| `WorldSnapshot` | `0x02` | server→client | unreliable | 12 + N×68 bytes | Per-tick entity state broadcast |
| `ClientInput` | `0x03` | client→server | reliable | 44 bytes | Per-frame flight inputs |

## Struct Definitions

### MsgConnectAck — 12 bytes

Sent once per peer on connect (reliable channel 0), immediately followed by
`typeCount` × `MsgEntityTypeDef` records.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x01` |
| 1 | 1 | `tickRateHz` | `uint8_t` | Server tick rate (60) |
| 2 | 2 | `typeCount` | `uint16_t` | Number of `MsgEntityTypeDef` records that follow |
| 4 | 4 | `assignedEntityIdx` | `uint32_t` | Pool slot of the entity assigned to this peer (0 if none) |
| 8 | 4 | `assignedEntityGen` | `uint32_t` | Entity generation; 0 = no entity assigned |

### MsgEntityTypeDef — 196 bytes

Appended N times after `MsgConnectAck` (one per registered entity type).

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 4 | `typeIndex` | `uint32_t` | Index into server-side `EntityTypeRegistry` |
| 4 | 64 | `id[64]` | `char[64]` | Null-terminated type ID, e.g. `"builtin:debug-entity"` |
| 68 | 64 | `mesh[64]` | `char[64]` | Null-terminated mesh asset name; empty = builtin tetrahedron |
| 132 | 64 | `dmgMesh[64]` | `char[64]` | Null-terminated damage mesh; empty = none |

### MsgWorldSnapshotHeader — 12 bytes

Broadcast unreliably every sim tick (channel 1), immediately followed by
`entityCount` × `MsgEntityEntry` records.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x02` |
| 1 | 1 | `_pad` | `uint8_t` | Reserved; candidate for protocol version field per #92 — write 0 until defined |
| 2 | 2 | `entityCount` | `uint16_t` | Number of `MsgEntityEntry` records that follow |
| 4 | 8 | `tickIndex` | `uint64_t` | Monotonically increasing server tick counter; at wire offset 4 (4-byte aligned, not 8-byte aligned) — **always use `memcpy`**; ARM64 (Linux arm64, Apple Silicon macOS) will SIGBUS on a direct pointer dereference; x86-64 handles it in hardware but UBSAN catches it |

### MsgEntityEntry — 68 bytes

Per-entity state appended N times after `MsgWorldSnapshotHeader`.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 4 | `entityIdx` | `uint32_t` | Pool slot index |
| 4 | 4 | `entityGen` | `uint32_t` | Generation counter (stale-handle detection) |
| 8 | 4 | `typeIndex` | `uint32_t` | Index into entity type registry |
| 12 | 24 | `pos[3]` | `double[3]` | World position XYZ (metres); double for planet-scale precision; at wire offset 12 (4-byte aligned, not 8-byte aligned) — **always use `memcpy`**; ARM64 (Linux arm64, Apple Silicon macOS) will SIGBUS on a direct pointer dereference; x86-64 handles it in hardware but UBSAN catches it |
| 36 | 12 | `vel[3]` | `float[3]` | World velocity XYZ (m/s), used for dead-reckoning |
| 48 | 16 | `ori[4]` | `float[4]` | Orientation quaternion **`[x, y, z, w]`** wire order; GLM constructor is `(w, x, y, z)` |
| 64 | 1 | `damageLevel` | `uint8_t` | 0=Intact, 1=Minor, 2=Severe, 3=Critical |
| 65 | 1 | `flags` | `uint8_t` | Bit 0 = playerOwned |
| 66 | 2 | `_pad[2]` | `uint8_t[2]` | Reserved, always 0 |

### MsgClientInput — 44 bytes

Sent by the client each render frame on the reliable channel (channel 0).

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x03` |
| 1 | 1 | `buttons` | `uint8_t` | Bit 0 = weaponTrigger, bit 1 = afterburner |
| 2 | 2 | `_pad[2]` | `uint8_t[2]` | Reserved; candidate for protocol version field per #92 — write 0 until defined |
| 4 | 4 | `seqNum` | `uint32_t` | Client-incremented wrapping sequence counter |
| 8 | 8 | `tickIndex` | `uint64_t` | Client's last-received server `tickIndex` (reserved for lag compensation — #142) |
| 16 | 4 | `throttle` | `float` | `[0.0, 1.0]` |
| 20 | 4 | `elevator` | `float` | `[-1.0, +1.0]` nose-up positive |
| 24 | 4 | `aileron` | `float` | `[-1.0, +1.0]` right-roll positive |
| 28 | 4 | `rudder` | `float` | `[-1.0, +1.0]` right-yaw positive |
| 32 | 12 | `viewAxis[3]` | `float[3]` | Normalized camera look direction (world space) |

The server clamps all control surface inputs to their valid ranges and normalises
`viewAxis` to unit length. Packets smaller than 44 bytes are silently discarded.

## Connection Flow

```
Client                              Server (fl-server sim thread)
  |                                     |
  |--- ENet connect ------------------>|
  |                                     | onConnect(peerId):
  |                                     |   spawn "builtin:debug-entity" → EntityId
  |<-- MsgConnectAck (reliable) --------|   assignedEntityIdx/Gen in ack
  |    + N × MsgEntityTypeDef           |
  |                                     |
  |  [each render frame]                | [each sim tick, 60 Hz]
  |--- MsgClientInput (reliable) ----->|   onReceive: validate + store PeerInputState
  |                                     |   onTick:
  |                                     |     applyPeerInput → update entity transform
  |                                     |     EntityManager::onTick
  |<-- MsgWorldSnapshot (unreliable) ---|     serialize all entities → broadcast
  |    + N × MsgEntityEntry             |
  |                                     |
  |--- ENet disconnect --------------->|
  |                                     | onDisconnect(peerId):
  |                                     |   kill assigned entity, clear maps
```

## Version Negotiation

Not yet implemented — tracked in #92. Phase 2 `fl-server` accepts all connecting peers
regardless of the client version.

`MsgConnectAck` has no protocol version field. `tickRateHz` at offset 1 carries the server
tick rate (always 60); it is not a version identifier. The `_pad` bytes in
`MsgWorldSnapshotHeader` (offset 1) and `MsgClientInput` (offsets 2–3) are reserved for a
future version field per #92.

External tools (replay readers per #41, spectator clients, LAN discovery tools) should
treat this spec as **protocol version 0** and be prepared to handle version negotiation once
#92 ships.

## Bandwidth and Scalability

### Snapshot packet size

`MsgWorldSnapshot` is fixed-cost per entity: **68 bytes/entity** plus a 12-byte header.

| Visible entities | Packet size | Per-client outbound (60 Hz) |
|-----------------|-------------|-----------------------------|
| 8 | 556 bytes | ~33 KB/s |
| 20 | 1,372 bytes | ~83 KB/s |
| 32 | 2,188 bytes | ~131 KB/s |
| 64 | 4,364 bytes | ~262 KB/s |
| 128 | 8,716 bytes | ~523 KB/s |

### MTU fragmentation

ENet fragments unreliable datagrams that exceed the path MTU (~1,400 bytes on standard
Ethernet). Fragmented unreliable packets are reconstructed only if **all fragments arrive**.
A single dropped fragment discards the whole snapshot — dead-reckoning absorbs occasional
losses, but high fragment counts multiply the effective loss rate:

> At 128 entities (8,716 bytes ≈ 6 fragments), a 5% per-fragment drop rate produces an
> ~26% effective snapshot delivery rate — significantly degrading simulation fidelity.

**The single-fragment safe threshold is approximately 20 visible entities per client**
(12 + 20×68 = 1,372 bytes < typical 1,400-byte MTU).

### Clustered / 128-player deployments

The current `WorldBroadcaster` sends all live entities to all connected peers (broadcast
all). For 128 simultaneous players this produces:

- ~8.7 KB snapshots per client per tick
- ~65 MB/s total server outbound at 60 Hz
- 6+ ENet fragments per snapshot, significantly raising effective packet loss

**Required mitigation: server-side interest management.** Each zone server must maintain
a per-peer visible entity list (culled by position and draw distance) and only serialize
entities on that list into each client's snapshot. With 128 players spread across a large
theater and a 100 km draw distance, the typical per-client visible count is 10–20 entities
— well within the single-fragment threshold.

Interest management is not yet implemented. It is required before any public multiplayer
test with more than ~20 simultaneous players in the same zone.

### Known overhead in this format

The following are documented as future optimization candidates; they do not block the
Phase 2 spec:

- **Full state every tick**: no delta compression — each snapshot is a complete entity
  state. Replay readers and spectator clients can rely on every snapshot being
  self-contained.
- **Double-precision positions**: `pos[3]` is 24 bytes vs. 12 bytes for float. Required
  for planet-scale precision; float32 degrades to ~24 cm accuracy at 2,000 km from origin.
- **`typeIndex` as uint32_t**: 4 bytes; a uint16_t would support 65,535 entity types and
  save 2 bytes/entity. Not changed here because `EntityTypeRegistry` and `EntityState`
  both use uint32_t throughout the engine — narrowing the wire type is deferred.

## Notes

- **World coordinate system**: right-handed, Y-up, metres (matches glTF). Entity body `+X`
  axis is forward.
- **Position precision**: `double` throughout the engine and wire format. At 2,000 km from
  origin, float32 precision degrades to ~24 cm; double is accurate to sub-millimetre.
- **Snapshot tolerance**: `WorldSnapshot` is unreliable — dropped packets are tolerated via
  dead-reckoning. Clients extrapolate `rendered_pos = pos + vel × alpha × kTickDt` where
  `alpha = GameLoop::shellTick()` ∈ [0, 1] and `kTickDt = 1/60 s`.
- **Input channel**: `MsgClientInput` uses the reliable channel for Phase 2. Full
  client-side prediction with reconciliation (which would switch inputs to unreliable +
  sequence-based) is deferred (#142).
