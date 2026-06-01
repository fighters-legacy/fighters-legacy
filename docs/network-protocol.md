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

| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0 | `msgId` | `uint8_t` | `0x01` |
| 1 | `tickRateHz` | `uint8_t` | Server tick rate (60) |
| 2 | `typeCount` | `uint16_t` | Number of `MsgEntityTypeDef` records that follow |
| 4 | `assignedEntityIdx` | `uint32_t` | Pool slot of the entity assigned to this peer (0 if none) |
| 8 | `assignedEntityGen` | `uint32_t` | Entity generation; 0 = no entity assigned |

### MsgEntityTypeDef — 196 bytes

Appended N times after `MsgConnectAck` (one per registered entity type).

| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0 | `typeIndex` | `uint32_t` | Index into server-side `EntityTypeRegistry` |
| 4 | `id[64]` | `char[64]` | Null-terminated type ID, e.g. `"builtin:debug-entity"` |
| 68 | `mesh[64]` | `char[64]` | Null-terminated mesh asset name; empty = builtin tetrahedron |
| 132 | `dmgMesh[64]` | `char[64]` | Null-terminated damage mesh; empty = none |

### MsgWorldSnapshotHeader — 12 bytes

Broadcast unreliably every sim tick (channel 1), immediately followed by
`entityCount` × `MsgEntityEntry` records.

| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0 | `msgId` | `uint8_t` | `0x02` |
| 1 | `_pad` | `uint8_t` | Reserved, always 0 |
| 2 | `entityCount` | `uint16_t` | Number of `MsgEntityEntry` records that follow |
| 4 | `tickIndex` | `uint64_t` | Monotonically increasing server tick counter; **at wire offset 4 (misaligned) — always use `memcpy`** |

### MsgEntityEntry — 68 bytes

Per-entity state appended N times after `MsgWorldSnapshotHeader`.

| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0 | `entityIdx` | `uint32_t` | Pool slot index |
| 4 | `entityGen` | `uint32_t` | Generation counter (stale-handle detection) |
| 8 | `typeIndex` | `uint32_t` | Index into entity type registry |
| 12 | `pos[3]` | `double[3]` | World position XYZ (metres) — **double for planet-scale precision** |
| 36 | `vel[3]` | `float[3]` | World velocity XYZ (m/s), used for dead-reckoning |
| 48 | `ori[4]` | `float[4]` | Orientation quaternion **`[x, y, z, w]`** wire order; GLM constructor is `(w, x, y, z)` |
| 64 | `damageLevel` | `uint8_t` | 0=Intact, 1=Minor, 2=Severe, 3=Critical |
| 65 | `flags` | `uint8_t` | Bit 0 = playerOwned |
| 66 | `_pad[2]` | `uint8_t[2]` | Reserved, always 0 |

### MsgClientInput — 44 bytes

Sent by the client each render frame on the reliable channel (channel 0).

| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0 | `msgId` | `uint8_t` | `0x03` |
| 1 | `buttons` | `uint8_t` | Bit 0 = weaponTrigger, bit 1 = afterburner |
| 2 | `_pad[2]` | `uint8_t[2]` | Reserved, always 0 |
| 4 | `seqNum` | `uint32_t` | Client-incremented wrapping sequence counter |
| 8 | `tickIndex` | `uint64_t` | Client's last-received server `tickIndex` (reserved for lag compensation — #142) |
| 16 | `throttle` | `float` | `[0.0, 1.0]` |
| 20 | `elevator` | `float` | `[-1.0, +1.0]` nose-up positive |
| 24 | `aileron` | `float` | `[-1.0, +1.0]` right-roll positive |
| 28 | `rudder` | `float` | `[-1.0, +1.0]` right-yaw positive |
| 32 | `viewAxis[3]` | `float[3]` | Normalized camera look direction (world space) |

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
