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
| `WorldSnapshot` | `0x02` | server→client | unreliable | 16 + N×72 bytes | Per-tick entity state broadcast |
| `ClientInput` | `0x03` | client→server | reliable | 48 bytes | Per-frame flight inputs |
| `WeatherState` | `0x04` | server→client | unreliable | 20 bytes | Weather and time-of-day; broadcast every 10 ticks (~6 Hz). Additive ID — old clients silently discard. |
| `ServerNotice` | `0x05` | server→client | reliable | 64 bytes | Shutdown countdown notification; sent at each warning interval and at T=0. Additive ID — old clients silently discard. |
| `AdminCommand` | `0x06` | client→server | reliable | 128 bytes | Operator-authenticated admin command. Additive ID — old servers silently discard. |
| `AdminResponse` | `0x07` | server→client | reliable | 128 bytes | Command result text, unicast to the requesting peer. Additive ID — old clients silently discard. |
| `Motd` | `0x08` | server→client | reliable | 4 + len(text) bytes | MOTD delivered once per connection after `MsgConnectAck`; variable-length. Additive ID — old clients silently discard. |
| `ConnectRefusal` | `0x09` | server→client | reliable | 64 bytes | Rejection reason sent before `disconnectPeer()` on every `onConnect` rejection (ban, allowlist, rate-limit, per-IP limit, admin auth lockout). Additive ID — old clients silently discard and fall back to the generic "Connection refused by server." message. |
| `LanBeacon` | `0x10` | server→LAN | raw UDP (not ENet) | 74 bytes | LAN server presence broadcast |

## Struct Definitions

> **Layout & versioning (primary development).** Wire structs are **unpacked** and laid out for
> natural field alignment — fields ordered large→small with explicit `reserved` padding, and array
> records padded to a multiple of their alignment so the i-th record stays aligned. Using only
> fixed-width types makes the layout byte-identical across MSVC/GCC/Clang, and `static_assert`s on
> `sizeof`/`offsetof`/`alignof` lock it. A naturally-aligned received buffer may therefore be read in
> place via `fl::viewMsg` (zero-copy); `fl::readMsg` (`memcpy`) is the portable default. The wire
> format may change freely while `kProtocolVersion` stays at **1** — the client always runs the
> same-tree server in primary development. The version field only begins to bind at the Phase 6
> public-release freeze.

### MsgHello — 4 bytes

Sent by the server on every new connection (reliable channel 0) **before** `MsgConnectAck`. The
client must compare `protocolVersion` against its own compiled `kProtocolVersion` and call
`disconnect()` immediately on mismatch. If `protocolVersion` matches, the client ignores this
message and waits for `MsgConnectAck`.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x00` |
| 1 | 1 | `reserved` | `uint8_t` | Reserved, always 0 |
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

### MsgWorldSnapshotHeader — 16 bytes

Broadcast unreliably every sim tick (channel 1), immediately followed by
`entityCount` × `MsgEntityEntry` records. Sized to 16 (a multiple of 8) so each trailing
`MsgEntityEntry` stays 8-aligned for in-place reads.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x02` |
| 1 | 1 | `protocolVersion` | `uint8_t` | Server's `kProtocolVersion`; defense-in-depth echo — per-packet version stamp |
| 2 | 2 | `entityCount` | `uint16_t` | Number of `MsgEntityEntry` records that follow |
| 4 | 4 | `reserved` | `uint32_t` | Padding so `tickIndex` is 8-aligned; always 0 |
| 8 | 8 | `tickIndex` | `uint64_t` | Monotonically increasing server tick counter (naturally 8-aligned) |

### MsgEntityEntry — 72 bytes

Per-entity state appended N times after `MsgWorldSnapshotHeader`. Laid out large→small (`pos` first,
8-aligned) and padded to 72 (a multiple of 8) so record `i` at `16 + i×72` stays 8-aligned.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 24 | `pos[3]` | `double[3]` | World position XYZ (metres); double for planet-scale precision; 8-aligned |
| 24 | 12 | `vel[3]` | `float[3]` | World velocity XYZ (m/s), used for dead-reckoning |
| 36 | 16 | `ori[4]` | `float[4]` | Orientation quaternion **`[x, y, z, w]`** wire order; GLM constructor is `(w, x, y, z)` |
| 52 | 4 | `entityIdx` | `uint32_t` | Pool slot index |
| 56 | 4 | `entityGen` | `uint32_t` | Generation counter (stale-handle detection) |
| 60 | 4 | `typeIndex` | `uint32_t` | Index into entity type registry |
| 64 | 1 | `damageLevel` | `uint8_t` | 0=Intact, 1=Minor, 2=Severe, 3=Critical |
| 65 | 1 | `flags` | `uint8_t` | Bit 0 = playerOwned |
| 66 | 1 | `throttle` | `uint8_t` | Actual throttle [0, 100] = 0%–100% (`FlightState::throttle_actual × 100`); 0 for non-player entities |
| 67 | 1 | `fuelPct` | `uint8_t` | Fuel remaining [0, 100] = 0%–100% of max fuel; 0 for non-player entities |
| 68 | 1 | `abEngaged` | `uint8_t` | `1` when afterburner physically lit (`FlightState::ab_engaged`); `0` otherwise |
| 69 | 1 | `engineFailFlags` | `uint8_t` | Engine failure bitmask: bit `0x01` = generic thrust impairment (`damageLevel ≥ Heavy`); bit `0x02` = left-engine failure (Phase 6+); bit `0x04` = right-engine failure (Phase 6+); bit `0x08` = compressor stall (Phase 6+); bit `0x10` = flameout (Phase 6+) |
| 70 | 2 | `reserved[2]` | `uint8_t[2]` | Padding to 72 (multiple of 8); always 0 |

### MsgClientInput — 48 bytes

Sent by the client each render frame on the reliable channel (channel 0). Padded to 48 (a multiple
of 8) for the 8-aligned `tickIndex`.

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
| 44 | 4 | `reserved[4]` | `uint8_t[4]` | Padding to 48; always 0 |

The server clamps all control surface inputs to their valid ranges and normalises
`viewAxis` to unit length. Packets smaller than 48 bytes are silently discarded.

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

### MsgServerNotice — 64 bytes

Reliable, server→client. Sent at each countdown interval during a graceful shutdown sequence and
once more at T=0 immediately before the server disconnects all peers.
`MsgId::ServerNotice = 0x05` is an additive message ID — clients that do not recognize it silently
discard without error. `kProtocolVersion` is **not** bumped.

`secondsRemaining == 0` indicates the server is shutting down immediately. The `text` field is
null-terminated UTF-8 (maximum 60 bytes including the NUL terminator); always read with
`sn.text[59] = '\0'` as a defensive guard.

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 1 | `msgId` | `uint8_t` | `0x05` |
| 1 | 1 | `reserved` | `uint8_t` | reserved, always 0 |
| 2 | 2 | `secondsRemaining` | `uint16_t` | seconds until shutdown; 0 = shutting down now |
| 4 | 60 | `text` | `char[60]` | null-terminated UTF-8 operator message |

**Sending cadence (fl-server):** first notice fires immediately when `shutdown --in <dur>` is
issued; subsequent notices fire every `shutdown.warning_interval_s` seconds (default 300 s); a
T-60s notice is always injected if the configured interval would skip past it. At T=0 a final
`secondsRemaining=0` notice is sent before graceful disconnect. If `--reason <text>` was provided
to the shutdown command, each `text` value is prefixed with the reason followed by ` -- ` (e.g.
`"Server restarting for patch 1.2 -- shutting down in 5 minutes."`); the field is always
null-terminated and safely truncated to 59 chars if the combined string exceeds that limit.

### MsgAdminCommand — 128 bytes

Reliable, client→server. Carries an operator token and a command string. The server performs
a constant-time comparison of the `token` field against the configured `operator_password`
(or the per-session `--admin-token` for single-player). On authentication success the command
is dispatched through the server's admin registry. On failure the packet is silently discarded
and a Warn is logged.

**Per-IP brute-force protection:** consecutive authentication failures from the same IP
are counted. After `admin_auth_max_failures` consecutive failures (default 5) the peer is
kicked and reconnections from that IP are refused for `admin_auth_lockout_s` seconds (default
300). The failure counter is IP-keyed and persists across disconnect/reconnect. A successful
authentication clears the counter for that IP. See `docs/fl-server-config.md` for the
configurable thresholds.

`MsgId::AdminCommand = 0x06` is an additive message ID — servers that do not recognize it
silently discard without error. `kProtocolVersion` is **not** bumped.

**Security note:** The token travels over UDP (ENet). Use this channel only on trusted networks
or behind a VPN. Passwords longer than 29 characters are silently truncated by the client (the
`token` field is 30 bytes including the NUL terminator).

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 1 | `msgId` | `uint8_t` | `0x06` |
| 1 | 1 | `reserved` | `uint8_t` | reserved, always 0 |
| 2 | 30 | `token` | `char[30]` | null-terminated operator password; 29 usable chars |
| 32 | 96 | `command` | `char[96]` | null-terminated command text; 95 usable chars |

### MsgAdminResponse — 128 bytes

Reliable, server→client unicast. Carries the text result of a dispatched admin command.
Always sent back to the requesting peer after a successful `MsgAdminCommand`, even when
the result string is empty (fire-and-forget commands return empty; clients may skip printing).

`MsgId::AdminResponse = 0x07` is an additive message ID — clients that do not recognize it
silently discard without error. `kProtocolVersion` is **not** bumped.

The `text` field is null-terminated UTF-8; guaranteed within 125 bytes by the server.
`text[0] == '\0'` indicates an empty result (command queued asynchronously).

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 1 | `msgId` | `uint8_t` | `0x07` |
| 1 | 1 | `reserved` | `uint8_t` | reserved, always 0 |
| 2 | 126 | `text` | `char[126]` | null-terminated UTF-8 response; 125 usable chars |

**Known limitation:** response text is capped at 125 characters. Commands with long output
(e.g. `peers` on a full server) will be silently truncated on the wire.

### MsgMotd — variable length

Reliable, server→client unicast. Sent once per connection immediately after `MsgConnectAck`,
only when `[server].motd` is non-empty in `server.toml`. The 4-byte `MsgMotdHeader` is followed by
the null-terminated text payload at offset 4.

The text is null-terminated UTF-8; the server caps the payload at `kMaxMotdBytes = 65535`
usable characters. Multi-line MOTDs use `\n` or `\r\n` line endings — the client splits on
newlines, prints each non-empty line to the game console prefixed `[server]`, and shows the
first line in the server notice banner.

The `displaySeconds` field lets the server specify how long its MOTD banner should remain
visible. `displaySeconds = 0` means the client uses its own `[client].motd_display_s` setting
(default 15 s); a non-zero value overrides the client setting for this connection.

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 1 | `msgId` | `uint8_t` | `0x08` |
| 1 | 1 | `reserved` | `uint8_t` | Reserved, always 0 |
| 2 | 2 | `displaySeconds` | `uint16_t` | banner duration (s); 0 = use client's `motd_display_s`; little-endian |
| 4 | ≤ 65535 | `text` | `char[]` | null-terminated UTF-8 MOTD; server caps at `kMaxMotdBytes` usable chars |

Packet size = `4 + strlen(text) + 1`. The packet has no fixed trailing padding; ENet
fragments automatically if the text exceeds the MTU.

`MsgId::Motd = 0x08` is an additive message ID — clients that do not recognize it silently
discard without error.

### MsgConnectRefusal — 64 bytes

Reliable, server→client unicast. Sent immediately before `disconnectPeer()` on every
`onConnect` rejection:

- **Ban**: `"You are banned from this server."`
- **Allowlist**: `"Access denied."`
- **Rate-limit**: `"Connection rate limit exceeded. Try again later."`
- **Per-IP connection limit**: `"Too many connections from your address."`
- **Admin auth lockout**: `"Access denied."`

ENet's graceful disconnect flushes all pending reliable packets before completing the
disconnect sequence, so the client receives this packet before the ENet disconnect event fires.
The client stores the reason via CAS into `connectFailMsg`, which the `onDisconnect` fallback
CAS then fails to overwrite, surfacing the specific reason in the `LoadingScreen`.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x09` |
| 1 | 1 | `code` | `uint8_t` | `ConnectRefusalCode`: 0=Generic, 1=Banned, 2=AccessDenied, 3=RateLimited, 4=TooManyConnections, 5=AdminLockout — machine-readable reason paired with the text below |
| 2 | 62 | `reason` | `char[62]` | Null-terminated UTF-8 rejection reason; 61 usable chars |

`MsgId::ConnectRefusal = 0x09` is an additive message ID — old clients that do not recognize
it silently discard and fall back to the generic "Connection refused by server." message from
the existing `onDisconnect` CAS path.

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
| 1 | 1 | `reserved` | `uint8_t` | Reserved, always 0 |
| 2 | 2 | `protocolVersion` | `uint16_t` | Server's `kProtocolVersion`; clients may filter on this |
| 4 | 2 | `gamePort` | `uint16_t` | ENet game port to connect to |
| 6 | 1 | `playerCount` | `uint8_t` | Current connected player count |
| 7 | 1 | `maxPlayers` | `uint8_t` | Maximum allowed peers |
| 8 | 1 | `gameModeFlags` | `uint8_t` | Bit 0 = campaign (`kGameModeCampaign`), bit 1 = mission (`kGameModeMission`), bit 2 = sandbox (`kGameModeSandbox`) |
| 9 | 1 | `reserved2` | `uint8_t` | Reserved, always 0 |
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
  |                                     |   [if rejected — ban/allowlist/rate/limit/lockout:]
  |<-- MsgConnectRefusal (reliable) ---|     reason string (e.g. "You are banned from this server.")
  |<-- ENet disconnect -----------------|
  | [onReceive CAS sets specific reason;|
  |  onDisconnect CAS fails; specific   |
  |  message shown in LoadingScreen]    |
  |                                     |   [if accepted:]
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
`engine/net/GameProtocol.h`. During primary development it stays at **1** — the game client always
runs the same-tree `fl-server`, so the wire format may change freely without a bump. The constant
begins to bind (incremented on any backward-incompatible change) only at the Phase 6 public-release
freeze.

External tools (replay readers per #41, spectator clients, LAN discovery tools) built against
this spec are **protocol version 1** and must implement `MsgHello` handling to interoperate.

## Bandwidth and Scalability

### Snapshot packet size

`MsgWorldSnapshot` is fixed-cost per entity: **72 bytes/entity** plus a 16-byte header.

| Visible entities | Packet size | Per-client outbound (60 Hz) |
|-----------------|-------------|-----------------------------|
| 8 | 592 bytes | ~36 KB/s |
| 20 | 1,456 bytes | ~87 KB/s |
| 32 | 2,320 bytes | ~139 KB/s |
| 64 | 4,624 bytes | ~277 KB/s |
| 128 | 9,232 bytes | ~554 KB/s |

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
