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
| `Hello` | `0x00` | server→client | reliable | 4 bytes | Protocol version handshake; first message on every new connection |
| `ConnectAck` | `0x01` | server→client | reliable | 12 + N×196 bytes | Handshake on connect; assigns entity slot and delivers type registry |
| `WorldSnapshot` | `0x02` | server→client | unreliable | 12 + N×68 bytes | Per-tick entity state broadcast |
| `ClientInput` | `0x03` | client→server | reliable | 44 bytes | Per-frame flight inputs |
| `WeatherState` | `0x04` | server→client | unreliable | 20 bytes | Weather and time-of-day; broadcast every 10 ticks (~6 Hz). Additive ID — old clients silently discard. |
| `LanBeacon` | `0x10` | server→LAN | raw UDP (not ENet) | 74 bytes | LAN server presence broadcast |

## Struct Definitions

### MsgHello — 4 bytes

Sent by the server on every new connection (reliable channel 0) **before** `MsgConnectAck`. The
client must compare `protocolVersion` against its own compiled `kProtocolVersion` and call
`disconnect()` immediately on mismatch. If `protocolVersion` matches, the client ignores this
message and waits for `MsgConnectAck`.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x00` |
| 1 | 1 | `_pad` | `uint8_t` | Reserved, always 0 |
| 2 | 2 | `protocolVersion` | `uint16_t` | Server's `kProtocolVersion`; client disconnects if this != its own `kProtocolVersion` |

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
| 1 | 1 | `protocolVersion` | `uint8_t` | Server's `kProtocolVersion`; defense-in-depth echo — per-packet version stamp |
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
| 66 | 1 | `throttle` | `uint8_t` | Actual throttle [0, 100] = 0%–100% (`FlightState::throttle_actual × 100`); 0 for non-player entities |
| 67 | 1 | `fuelPct` | `uint8_t` | Fuel remaining [0, 100] = 0%–100% of max fuel; 0 for non-player entities |

### MsgClientInput — 44 bytes

Sent by the client each render frame on the reliable channel (channel 0).

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x03` |
| 1 | 1 | `buttons` | `uint8_t` | Bit 0 = weaponTrigger, bit 1 = afterburner |
| 2 | 2 | `protocolVersion` | `uint16_t` | Client's `kProtocolVersion`; server discards packet and logs a warning on mismatch |
| 4 | 4 | `seqNum` | `uint32_t` | Client-incremented wrapping sequence counter |
| 8 | 8 | `tickIndex` | `uint64_t` | Client's last-received server `tickIndex` (reserved for lag compensation — #142) |
| 16 | 4 | `throttle` | `float` | `[0.0, 1.0]` |
| 20 | 4 | `elevator` | `float` | `[-1.0, +1.0]` nose-up positive |
| 24 | 4 | `aileron` | `float` | `[-1.0, +1.0]` right-roll positive |
| 28 | 4 | `rudder` | `float` | `[-1.0, +1.0]` right-yaw positive |
| 32 | 12 | `viewAxis[3]` | `float[3]` | Normalized camera look direction (world space) |

The server clamps all control surface inputs to their valid ranges and normalises
`viewAxis` to unit length. Packets smaller than 44 bytes are silently discarded.

### MsgWeatherState — 20 bytes

Unreliable, server→client. Broadcast every 10 sim ticks (~6 Hz at 60 Hz sim) after the `MsgWorldSnapshot`.
`MsgId::WeatherState = 0x04` is an additive message ID — clients that do not recognize it silently
discard without error. `kProtocolVersion` is **not** bumped.

`timeOfDayTenths` encodes the time of day as `hours × 10` in a `uint16_t` to avoid placing a `float`
at offset 2 (ARM64 alignment constraint). Decode: `timeOfDay = timeOfDayTenths / 10.f`.

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 1 | `msgId` | `uint8_t` | `0x04` |
| 1 | 1 | `preset` | `uint8_t` | `WeatherPreset` enum: 0=Clear, 1=PartlyCloudy, 2=Overcast, 3=Rain, 4=Storm |
| 2 | 2 | `timeOfDayTenths` | `uint16_t` | hours × 10; range [0, 239]; decode: / 10.f |
| 4 | 4 | `fogDensity` | `float` | exponential fog coefficient (0 = no fog) |
| 8 | 4 | `fogStartDist` | `float` | fog start distance (metres) |
| 12 | 4 | `windX` | `float` | world-frame wind x component (m/s), includes gust |
| 16 | 4 | `windZ` | `float` | world-frame wind z component (m/s), includes gust |

Wind convention: `windX` and `windZ` are the **blowing-toward** direction. A westerly wind (FROM 270°) has `windX > 0`.
`windY` is always zero (horizontal wind only).

### MsgLanBeacon — 74 bytes

Broadcast by `fl-server` on `255.255.255.255:<port>` (IPv4) and `[ff02::1]:<port>` (IPv6
link-local multicast) every `discovery.interval_ms` milliseconds (default: 2000 ms) using a
**raw UDP socket separate from ENet**. Clients on the same LAN receive this packet without
establishing a connection. See issue #91 for the server-side implementation; client-side server
browser is issue #143.

This packet is **not** sent over ENet and must not be injected into an ENet connection.
`MsgId::LanBeacon = 0x10` is outside the ENet message range (`0x00`–`0x03`).

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x10` |
| 1 | 1 | `_pad` | `uint8_t` | Reserved, always 0 |
| 2 | 2 | `protocolVersion` | `uint16_t` | Server's `kProtocolVersion`; clients may filter on this |
| 4 | 2 | `gamePort` | `uint16_t` | ENet game port to connect to |
| 6 | 1 | `playerCount` | `uint8_t` | Current connected player count |
| 7 | 1 | `maxPlayers` | `uint8_t` | Maximum allowed peers |
| 8 | 1 | `gameModeFlags` | `uint8_t` | Bit 0 = campaign (`kGameModeCampaign`), bit 1 = mission (`kGameModeMission`), bit 2 = sandbox (`kGameModeSandbox`) |
| 9 | 1 | `_pad2` | `uint8_t` | Reserved, always 0 |
| 10 | 64 | `name[64]` | `char[64]` | Null-terminated server name (UTF-8) |

**IPv6 multicast:** The sender broadcasts to `ff02::1` (all-nodes link-local); receivers join
via `IPV6_JOIN_GROUP`. No join is required by the sender. `IPV6_MULTICAST_HOPS` is set to 1
(link-local scope only — does not traverse routers).

**Address preference:** When the same server is found via both IPv4 and IPv6 beacons,
`DiscoveryListener` uses the IPv4 address for the `ServerInfo::address` field. IPv6 link-local
addresses carry a scope ID that is interface-specific and may not survive an `INetwork::connect`
call.

**Deduplication:** `DiscoveryListener` merges beacons with the same `(gamePort, name)` into one
`ServerInfo` entry regardless of source address family.

---

## Connection Flow

```
Client                              Server (fl-server sim thread)
  |                                     |
  |--- ENet connect ------------------>|
  |                                     | onConnect(peerId):
  |<-- MsgHello (reliable) ------------|   protocolVersion = kProtocolVersion
  | [disconnect if version mismatch]    |
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

Implemented in #92. The protocol uses a two-level version check:

**Initial handshake (`MsgHello`):** The server sends `MsgHello` as the very first reliable
packet on every new connection, before `MsgConnectAck`. The client compares
`MsgHello::protocolVersion` against its compiled `kProtocolVersion` constant. On mismatch the
client logs an error and calls `disconnect()` immediately, before processing any further packets.
On match the client continues normally and waits for `MsgConnectAck`.

**Per-packet echo:** Every `MsgWorldSnapshotHeader` carries `protocolVersion` at offset 1 (server
→ client); every `MsgClientInput` carries `protocolVersion` at offsets 2–3 (client → server). The
server discards `MsgClientInput` packets whose `protocolVersion` does not match `kProtocolVersion`
and logs a warning. These fields serve as a defense-in-depth sanity check — the primary
negotiation happens via `MsgHello`.

**`kProtocolVersion`** is defined as `constexpr uint16_t kProtocolVersion = 1` in
`engine/net/GameProtocol.h`. It must be incremented whenever the wire format changes in a
backward-incompatible way.

External tools (replay readers per #41, spectator clients, LAN discovery tools) built against
this spec are **protocol version 1** and must implement `MsgHello` handling to interoperate.

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
