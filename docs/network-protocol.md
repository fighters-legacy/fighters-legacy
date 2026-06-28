# Network Wire Protocol

This document describes the binary message format used between `fl-server` and game clients
over ENet UDP. All structs are defined in `engine/net/GameProtocol.h`.

## Channels

| Channel | Constant | Delivery | Use |
|---------|----------|----------|-----|
| 0 | `kNetChReliable` | Ordered, guaranteed | Handshake messages |
| 1 | `kNetChUnreliable` | Best-effort datagram | World-state snapshots, client input frames |

ENet enforces ordering within each channel; reliable packets are retransmitted until
acknowledged. Unreliable packets may be dropped or arrive out of order — clients tolerate
this via dead-reckoning (`rendered_pos = pos + vel × alpha × kTickDt`).

## Implementation Rules

- Wire structs are **unpacked** and use natural field alignment — fields ordered large→small with
  explicit `reserved` padding; no implicit compiler padding is inserted. `static_assert`s on
  `sizeof`/`offsetof`/`alignof` lock the layout across MSVC/GCC/Clang.
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
| `ConnectAck` | `0x01` | server→client | reliable | 16 + N×196 bytes | Handshake on connect; assigns entity slot and delivers type registry |
| `WorldSnapshot` | `0x02` | server→client | unreliable | 16 + F×88 + U×64 bytes | Per-tick entity state, unicast per peer; F = full entries (new or baseline), U = compact updates (known entities) |
| `ClientInput` | `0x03` | client→server | unreliable | 48 bytes | Per-frame flight inputs |
| `WeatherState` | `0x04` | server→client | unreliable | 20 bytes | Weather and time-of-day; broadcast every 10 ticks (~6 Hz). Additive ID — old clients silently discard. |
| `ServerNotice` | `0x05` | server→client | reliable | 64 bytes | Shutdown countdown notification; sent at each warning interval and at T=0. Additive ID — old clients silently discard. |
| `AdminCommand` | `0x06` | client→server | reliable | 128 bytes | Operator-authenticated admin command. Additive ID — old servers silently discard. |
| `AdminResponse` | `0x07` | server→client | reliable | 128 bytes | Fast-path command result (≤ 123 chars), unicast to the requesting peer. Additive ID — old clients silently discard. |
| `AdminResponseChunk` | `0x0A` | server→client | reliable | 512 bytes | Streaming chunk for long admin command output (> 123 chars). Additive ID — old clients silently discard. |
| `Motd` | `0x08` | server→client | reliable | 4 + len(text) bytes | MOTD delivered once per connection after `MsgConnectAck`; variable-length. Additive ID — old clients silently discard. |
| `ConnectRefusal` | `0x09` | server→client | reliable | 64 bytes | Rejection reason sent before `disconnectPeer()` on every `onConnect` rejection (ban, allowlist, rate-limit, per-IP limit, admin auth lockout). Additive ID — old clients silently discard and fall back to the generic "Connection refused by server." message. |
| `Heartbeat` | `0x0B` | client→server | unreliable | 16 bytes | Liveness signal sent at ~1 Hz when flying; carries the client's last received `tickIndex` so the server can refresh `estimatedDelayTicks`. Only sent after at least one `MsgWorldSnapshot` has been received. Additive ID — old servers silently discard. |
| `PeerDelay` | `0x0C` | server→client | unreliable | 4 bytes | Reply to `MsgHeartbeat`; delivers `estimatedDelayTicks` for this peer. Client converts to ms: `delayTicks × 1000 / 60`. `delayTicks == 0` means no valid estimate yet (client ignores). Additive ID — old clients silently discard. |
| `LanBeacon` | `0x10` | server→LAN | raw UDP (not ENet) | 74 bytes | LAN server presence broadcast |

## Struct Definitions

> **Layout & versioning (primary development).** Wire structs are **unpacked** and laid out for
> natural field alignment — fields ordered large→small with explicit `reserved` padding, and array
> records padded to a multiple of their alignment so the i-th record stays aligned. Using only
> fixed-width types makes the layout byte-identical across MSVC/GCC/Clang, and `static_assert`s on
> `sizeof`/`offsetof`/`alignof` lock it. A naturally-aligned received buffer may therefore be read in
> place via `fl::viewMsg` (zero-copy); `fl::readMsg` (`memcpy`) is the portable default. The wire
> format may change freely while `kProtocolVersion` stays at **1** — the client always runs the
> same-tree server in primary development. The version field only begins to bind at the Phase 7
> (Platform Release) public-release freeze.

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

### MsgConnectAck — 16 bytes

Sent once per peer on connect (reliable channel 0), immediately followed by
`typeCount` × `MsgEntityTypeDef` records.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x01` |
| 1 | 1 | `tickRateHz` | `uint8_t` | Server tick rate (60) |
| 2 | 2 | `typeCount` | `uint16_t` | Number of `MsgEntityTypeDef` records that follow |
| 4 | 4 | `assignedEntityIdx` | `uint32_t` | Pool slot of the entity assigned to this peer (0 if none) |
| 8 | 4 | `assignedEntityGen` | `uint32_t` | Entity generation; 0 = no entity assigned |
| 12 | 4 | `planetRadiusKm` | `float32` | Planet sphere radius in km; Earth default = 6371.0 |

### MsgEntityTypeDef — 196 bytes

Appended N times after `MsgConnectAck` (one per registered entity type).

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 4 | `typeIndex` | `uint32_t` | Index into server-side `EntityTypeRegistry` |
| 4 | 64 | `id[64]` | `char[64]` | Null-terminated type ID, e.g. `"builtin:debug-entity"` |
| 68 | 64 | `mesh[64]` | `char[64]` | Null-terminated mesh asset name; empty = builtin tetrahedron |
| 132 | 64 | `dmgMesh[64]` | `char[64]` | Null-terminated damage mesh; empty = none |

### MsgWorldSnapshotHeader — 16 bytes

Sent unreliably per-peer every sim tick (channel 1). Followed by `fullEntityCount` ×
`MsgEntityEntry` records (new entities or baseline-tick resync), then `updateCount` ×
`MsgEntityUpdate` records (compact state for entities the peer already knows), then the TLV
extension block. Sized to 16 (a multiple of 8) so each trailing `MsgEntityEntry` stays
8-aligned for in-place reads.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x02` |
| 1 | 1 | `protocolVersion` | `uint8_t` | Server's `kProtocolVersion`; defense-in-depth echo — per-packet version stamp |
| 2 | 2 | `fullEntityCount` | `uint16_t` | Number of `MsgEntityEntry` records that follow (new or baseline-tick entities) |
| 4 | 2 | `updateCount` | `uint16_t` | Number of `MsgEntityUpdate` records following the full entries (compact deltas for known entities) |
| 6 | 2 | `_reserved` | `uint16_t` | Padding so `tickIndex` is 8-aligned; always 0 |
| 8 | 8 | `tickIndex` | `uint64_t` | Monotonically increasing server tick counter (naturally 8-aligned) |

### MsgEntityEntry — 88 bytes

Per-entity state appended N times after `MsgWorldSnapshotHeader`. Laid out large→small (`pos` first,
8-aligned) and padded to 88 (a multiple of 8) so record `i` at `16 + i×88` stays 8-aligned.

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
| 70 | 2 | `reserved[2]` | `uint8_t[2]` | Padding (layout stability); always 0 |
| 72 | 12 | `omega[3]` | `float[3]` | Body-frame angular rates p, q, r (rad/s); used by client-side prediction to seed the local `FlightIntegrator` at reconciliation time |
| 84 | 4 | `reserved2[4]` | `uint8_t[4]` | Pad to 88 (multiple of 8 for record-alignment); always 0 |

### MsgEntityUpdate — 64 bytes

Compact per-tick state for entities already known to the receiving peer. Appended in the update
section of `MsgWorldSnapshot` (after `fullEntityCount × MsgEntityEntry`). Uses `float` positions
(absolute world coordinates; precision ~1.6 cm at 200 km from origin — sufficient for rendering).
Omits static fields (`typeIndex`, padding) that the client caches from the last full
`MsgEntityEntry` for this entity. `alignof(MsgEntityUpdate) == 4`; 64 is a multiple of 4.

`entityGen` is `uint16_t` (truncated from `EntityId::generation` `uint32_t`). Practical pool-slot
reuse rate makes overflow impossible within a session.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 4 | `entityIdx` | `uint32_t` | Pool slot index (same as `MsgEntityEntry`) |
| 4 | 2 | `entityGen` | `uint16_t` | Generation counter (truncated); mismatch vs. client cache → entity treated as new |
| 6 | 1 | `damageLevel` | `uint8_t` | 0=Intact … 3=Critical |
| 7 | 1 | `engineFailFlags` | `uint8_t` | `fl::kEngineFail*` bitmask (same encoding as `MsgEntityEntry`) |
| 8 | 12 | `pos[3]` | `float[3]` | Absolute world position XYZ (metres), `float32` |
| 20 | 12 | `vel[3]` | `float[3]` | World velocity XYZ (m/s) for dead-reckoning |
| 32 | 16 | `ori[4]` | `float[4]` | Orientation quaternion **`[x, y, z, w]`** (same wire order as `MsgEntityEntry`) |
| 48 | 1 | `throttle` | `uint8_t` | Actual throttle [0, 100] |
| 49 | 1 | `fuelPct` | `uint8_t` | Fuel remaining [0, 100] |
| 50 | 1 | `abEngaged` | `uint8_t` | `1` when afterburner physically lit |
| 51 | 1 | `flags` | `uint8_t` | Bit 0 = playerOwned |
| 52 | 12 | `omega[3]` | `float[3]` | Body-frame angular rates p, q, r (rad/s); used by client-side prediction |

### MsgClientInput — 48 bytes

Sent by the client each render frame on the **unreliable channel (channel 1)**. Padded to 48 (a
multiple of 8) for the 8-aligned `tickIndex`. For a continuous 60 Hz control stream, unreliable
delivery is correct: a dropped packet is superseded by the next frame's input; retransmission would
delay all subsequent inputs behind the ACK round-trip.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x03` |
| 1 | 1 | `buttons` | `uint8_t` | Bit 0 = weaponTrigger, bit 1 = afterburner |
| 2 | 2 | `protocolVersion` | `uint16_t` | Client's `kProtocolVersion`; server discards packet and logs a warning on mismatch |
| 4 | 4 | `seqNum` | `uint32_t` | Monotonically increasing per-client sequence counter; server applies a half-window comparison to discard out-of-order and duplicate packets |
| 8 | 8 | `tickIndex` | `uint64_t` | Server `tickIndex` from the client's last received `MsgWorldSnapshot`; server computes `estimatedDelayTicks = currentTick − tickIndex` for diagnostics |
| 16 | 4 | `throttle` | `float` | `[0.0, 1.0]` |
| 20 | 4 | `elevator` | `float` | `[-1.0, +1.0]` nose-up positive |
| 24 | 4 | `aileron` | `float` | `[-1.0, +1.0]` right-roll positive |
| 28 | 4 | `rudder` | `float` | `[-1.0, +1.0]` right-yaw positive |
| 32 | 12 | `viewAxis[3]` | `float[3]` | Normalized camera look direction (world space) |
| 44 | 4 | `reserved[4]` | `uint8_t[4]` | Padding to 48; always 0 |

The server clamps all control surface inputs to their valid ranges and normalises `viewAxis` to unit
length. Packets smaller than 48 bytes are silently discarded. ENet's sequenced unreliable delivery
provides a first layer of ordering; the application-level `seqNum` guard adds defense-in-depth.

After passing validation the server enqueues each accepted input into a per-peer ring buffer
(`JitterBuffer`, depth ≤ `[world].jitter_buffer_depth`, default 4 ticks). The buffer is drained
exactly once per sim tick before the flight integrator is stepped; when the buffer runs empty the
last drained input is repeated (stale repeat) rather than zeroing controls, preventing coasting
under transient packet loss. The initial buffer depth per peer is `min(estimatedDelayTicks, maxDepth)`
seeded at first input. The depth is then continuously adjusted each tick: an EWMA of
`estimatedDelayTicks` and an RFC 3550-style inter-arrival jitter estimate drive
`target = ceil(ewma_delay + k × jitter)`, clamped to `[1, jitter_buffer_depth]`; resize fires only
when `|target − current| > hysteresis`. Configurable via `[world].jitter_buffer_adapt_window`,
`jitter_buffer_hysteresis`, and `jitter_buffer_jitter_multiplier`.

### MsgWeatherState — 20 bytes

Unreliable, server→client. Broadcast every 10 sim ticks (~6 Hz at 60 Hz sim) after the `MsgWorldSnapshot`.
`MsgId::WeatherState = 0x04` is an additive message ID — clients that do not recognize it silently
discard without error. `kProtocolVersion` is **not** bumped.

`timeOfDayTenths` encodes the time of day as `hours × 10` in a `uint16_t` to avoid placing a `float`
at offset 2 (ARM64 alignment constraint). Decode: `timeOfDay = timeOfDayTenths / 10.f`.

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 1 | `msgId` | `uint8_t` | `0x04` |
| 1 | 1 | `preset` | `uint8_t` | `WeatherPreset` enum: 0=Clear, 1=PartlyCloudy, 2=Overcast, 3=Rain, 4=Storm, 5=Snow, 6=Blizzard |
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
| 2 | 2 | `reqId` | `uint16_t` | client-generated correlation ID; echoed in every response packet for this command |
| 4 | 30 | `token` | `char[30]` | null-terminated operator password; 29 usable chars |
| 34 | 94 | `command` | `char[94]` | null-terminated command text; 93 usable chars |

### MsgAdminResponse — 128 bytes

Reliable, server→client unicast. Fast path for command results ≤ 123 chars. Results longer
than 123 chars are streamed as `MsgAdminResponseChunk` (0x0A) packets instead — see below.
Always sent back to the requesting peer after a successful `MsgAdminCommand`, even when
the result string is empty (fire-and-forget commands return empty; clients may skip printing).

`MsgId::AdminResponse = 0x07` is an additive message ID — clients that do not recognize it
silently discard without error. `kProtocolVersion` is **not** bumped.

The `text` field is null-terminated UTF-8. Response bodies may contain embedded `\n`
characters; clients should split on `\n`, strip trailing `\r` for CRLF compatibility, and
display each non-empty line separately. Results ≤ 123 chars are delivered in a single
`MsgAdminResponse`; longer results arrive as a sequence of `MsgAdminResponseChunk` packets
terminated by `kChunkFlagEnd`. `text[0] == '\0'` indicates an empty result (queued
asynchronously; clients may skip printing). `reqId` echoes the triggering
`MsgAdminCommand::reqId` for request/response correlation.

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 1 | `msgId` | `uint8_t` | `0x07` |
| 1 | 1 | `reserved` | `uint8_t` | reserved, always 0 |
| 2 | 2 | `reqId` | `uint16_t` | echoed from the triggering `MsgAdminCommand::reqId` |
| 4 | 124 | `text` | `char[124]` | null-terminated UTF-8 response; 123 usable chars |

**Deferred confirmation:** commands that enqueue a sim-thread mutation (e.g. `spawn`, `kill`,
`tp`, `ban`, `kick`, `peers`) deliver their result in two stages:

1. **Synchronous ack** — `MsgAdminResponse` or `MsgAdminResponseChunk` sent immediately;
   may be empty (`text[0] == '\0'`) or a brief status such as `"spawn: queued …"`.
2. **Deferred confirmation** — one additional `MsgAdminResponseChunk` (or `MsgAdminResponse`)
   carrying the actual mutation result (e.g. `"[admin] spawned builtin:debug-entity entity=1/1"`)
   arrives with the **same `reqId`** approximately one sim tick (~16 ms) later.

Clients that display admin output should append every response packet that matches a pending
`reqId`, not just the first. Deferred output is absent when the mutation produces no shell
output (e.g. `set_weather`).

### MsgAdminResponseChunk — 512 bytes

Reliable, server→client unicast. Streaming path for command results longer than 123 chars
(the `MsgAdminResponse` fast-path limit). The server splits the result into sequential chunks
and sends them on the reliable channel; ENet guarantees in-order delivery, so `seqNum` is
diagnostic only.

`MsgId::AdminResponseChunk = 0x0A` is an additive message ID — clients that do not
recognize it silently discard without error. `kProtocolVersion` is **not** bumped.

**Client reassembly:** append each `body` string in order. When a chunk with `kChunkFlagEnd`
(bit 0 of `flags`) arrives, the response body is complete. Split it on `\n` (strip trailing
`\r` for CRLF compatibility) and display each non-empty line separately in the client UI.
Discard streams that exceed 64 KB (implementation-defined safety cap).

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 1 | `msgId` | `uint8_t` | `0x0A` |
| 1 | 1 | `flags` | `uint8_t` | bit 0 = `kChunkFlagEnd` (set on the final chunk) |
| 2 | 2 | `reqId` | `uint16_t` | echoed from the triggering `MsgAdminCommand::reqId` |
| 4 | 2 | `seqNum` | `uint16_t` | 0-based chunk index; diagnostic only |
| 6 | 506 | `body` | `char[506]` | null-terminated chunk body; 505 usable chars |

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

### MsgHeartbeat — 16 bytes

Unreliable, **client→server**, channel 1. Sent by the client at ~1 Hz while in the flight screen
(only after the first `MsgWorldSnapshot` has been received). Carries the client's last received
`tickIndex` so the server can refresh `estimatedDelayTicks` for idle peers. The server replies with
`MsgPeerDelay`.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x0B` |
| 1 | 7 | `reserved[7]` | `uint8_t[7]` | padding to 8-align `tickIndex` |
| 8 | 8 | `tickIndex` | `uint64_t` | last received `MsgWorldSnapshot::tickIndex`; same semantic as `MsgClientInput::tickIndex` |

`MsgId::Heartbeat = 0x0B` is an additive message ID — old servers silently discard.

### MsgPeerDelay — 4 bytes

Unreliable, **server→client unicast**, channel 1. Reply to `MsgHeartbeat`; delivers the server's
`estimatedDelayTicks` for this peer so the client can display "Ping: N ms".

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x0C` |
| 1 | 1 | `reserved` | `uint8_t` | |
| 2 | 2 | `delayTicks` | `uint16_t` | `estimatedDelayTicks` capped at 65535 (≈18 min at 60 Hz). `0` = estimate not yet valid; client ignores. Convert to ms: `delayTicks × 1000 / 60`. |

`MsgId::PeerDelay = 0x0C` is an additive message ID — old clients silently discard.

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

## Extension Blocks (TLV)

Optional TLV (Type-Length-Value) extension blocks may be appended after the fixed struct section of
any message packet. Each extension entry:

```
[tag: uint16_t LE][len: uint16_t LE][data: len bytes]
[tag: uint16_t LE][len: uint16_t LE][data: len bytes]
... (more; parsing stops when fewer than 4 bytes remain in the extension region)
```

The extension block begins at:
- **Single-struct messages**: offset `sizeof(FixedStruct)`
- **Array messages** (header + N records): offset `sizeof(Header) + N × sizeof(Record)`
- **`MsgWorldSnapshot`** (two record types): offset
  `sizeof(MsgWorldSnapshotHeader) + fullEntityCount × sizeof(MsgEntityEntry) + updateCount × sizeof(MsgEntityUpdate)`

**Backward compatibility**: old receivers that call `readMsg<T>()` or iterate records up to
`fullEntityCount` naturally stop at the right byte count and ignore trailing extension bytes — no code
change is required for old receivers to remain correct. New receivers call `fl::readExtValue()` for
known tags and skip unknown tags via their `len` field.

Helpers: `fl::findExt`, `fl::readExtValue<T>`, `fl::appendExt<T>`, `fl::appendExtRaw` in
`engine/net/WireCodec.h`. Tag registry: `fl::ExtTag` enum in `engine/net/GameProtocol.h`.

### Defined Extension Tags

| Tag | Value | Type | Message | Description |
|-----|-------|------|---------|-------------|
| `SnapshotPeerCount` | `0x0100` | `uint16_t` | `MsgWorldSnapshot` | Active connected peer count at the time the snapshot was built. Emitted by `WorldBroadcaster` every tick; stored by `ClientNetEventHandler::serverPeerCount()`. |
| `SnapshotPeerLatency` | `0x0101` | `uint16_t` | `MsgWorldSnapshot` | Receiving peer's estimated one-way latency in ms (`estimatedDelayTicks × 1000 / 60`), capped at 65535. Absent when `estimatedDelayTicks == 0` (e.g. single-player localhost). Stored by `ClientNetEventHandler::snapshotLatencyMs()`; displayed in `FlightHud` as a compact `"42 ms"` indicator. |
| `SnapshotPeerDelayTicks` | `0x0102` | `uint16_t` | `MsgWorldSnapshot` | Raw `estimatedDelayTicks` (tick count, not ms). Companion to `SnapshotPeerLatency`; avoids the ms-rounding loss inherent in `ticks → ms → ticks` conversion. Used by `ClientPrediction` as the replay-depth signal for client-side prediction. Absent when `estimatedDelayTicks == 0`. |

**Reserved ranges:**
- `0x0000`: reserved
- `0x0100–0x01FF`: `MsgWorldSnapshot` extensions
- `0x0200–0x02FF`: `MsgConnectAck` extensions (reserved for future use)
- `0x0300–0x03FF`: `MsgClientInput` extensions (reserved for future use)
- `0x0400–0x04FF`: `MsgWeatherState` extensions (reserved for future use)
- All other values: reserved; must not be sent

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
  |--- MsgClientInput (unreliable) --->|   onReceive: seqNum guard + store PeerInputState
  |                                     |   onTick:
  |                                     |     applyPeerInput → update entity transform
  |                                     |     EntityManager::onTick
  |<-- MsgWorldSnapshot (unreliable, unicast) |  interest filter + delta compress → per-peer
  |    fullEntityCount × MsgEntityEntry       |  (new or baseline-tick entities)
  |    updateCount × MsgEntityUpdate          |  (compact updates for known entities)
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
begins to bind (incremented on any backward-incompatible change) only at the Phase 7 (Platform
Release) public-release freeze.

External tools (replay readers per #41, spectator clients, LAN discovery tools) built against
this spec are **protocol version 1** and must implement `MsgHello` handling to interoperate.

## Bandwidth and Scalability

### Snapshot packet size

`MsgWorldSnapshot` is fixed-cost per entity: **88 bytes/entity** (full entry) or **64 bytes** (compact update) plus a 16-byte header.

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

### Interest management and delta compression (#346)

As of #346, `WorldBroadcaster::onTick()` sends a **per-peer unicast** `MsgWorldSnapshot`
containing only entities within `draw_distance_km` of the peer's own entity position (via
`SpatialIndex::queryRadius()`). `MsgWeatherState` and `MsgServerNotice` remain global
broadcasts.

**Delta compression**: entities already known to a peer appear as compact `MsgEntityUpdate`
records (64 bytes, float positions) rather than full `MsgEntityEntry` records (88 bytes).
A full re-sync (all `MsgEntityEntry`) fires every `baseline_interval_ticks` ticks (default
120 = 2 s at 60 Hz) for UDP packet-loss recovery.

Typical bandwidth with 20 peers, 10 visible entities each, default baseline:
- Baseline tick: 16 + 10×72 + TLV ≈ 742 bytes/peer (~45 KB/s per peer)
- Update ticks: 16 + 10×52 + TLV ≈ 542 bytes/peer (~32 KB/s per peer)
- Both are single-fragment (< 1,400-byte MTU threshold). ✓

Configure via `[world] draw_distance_km` and `[world] baseline_interval_ticks` in
`server.toml`; hot-reloadable via `reload_config`.

### Known overhead in this format

The following are documented as future optimization candidates:

- **Double-precision positions in full entries**: `pos[3]` is 24 bytes (double) in
  `MsgEntityEntry` vs. 12 bytes (float) in `MsgEntityUpdate`. Required for planet-scale
  precision in full entries; compact updates use float32 (~1.6 cm at 200 km).
- **Double-precision positions**: `pos[3]` is 24 bytes vs. 12 bytes for float. Required
  for planet-scale precision; float32 degrades to ~24 cm accuracy at 2,000 km from origin.
- **`typeIndex` as uint32_t**: 4 bytes; a uint16_t would support 65,535 entity types and
  save 2 bytes/entity. Not changed here because `EntityTypeRegistry` and `EntityState`
  both use uint32_t throughout the engine — narrowing the wire type is deferred.

### Scaling to 128+ (planned — Epics B & L)

The fixed 64-byte compact update and radius-only unicast above are sized for ~32 players. The
128+ re-target (decision record 2026-06-28) replaces them. The per-client bandwidth figures in
this section are measured empirically by the `bot_swarm` load generator — see
[docs/load-testing.md](load-testing.md). This is a **forward-looking spec**;
the wire changes land with the epics, and the protocol can change freely until the
`kProtocolVersion` freeze.

- **Quantized / bit-packed state (Epic B).** The fixed `MsgEntityUpdate` becomes a quantized
  bitstream: position relative to a per-snapshot frame origin, **smallest-three** quaternion
  encoding for orientation, and quantized velocity/omega. Target ~3–4× size reduction over the
  current 64 bytes, pulling the single-fragment safe threshold well above 20 entities.
- **Priority/budget snapshot scheduler (Epic B).** Instead of "everything within
  `draw_distance_km`," each client gets a **per-tick byte budget**; entities are ranked by
  relevance (distance, threat, recency) and the highest-priority set that fits is sent. Keeps
  per-client bandwidth bounded as population grows.
- **Client-acked delta baselines (Epic B).** Replace the fixed `baseline_interval_ticks`
  re-sync with client-acknowledged baselines, so full re-sends happen only when a client
  actually needs recovery.
- **3D interest management (#402, Epic B).** Extend `SpatialIndex::queryRadius()` interest
  from XZ to full XYZ distance culling.
- **Transport replacement (Epic L).** `enet6` is being replaced (GameNetworkingSockets lean)
  behind the `INetwork` HAL for higher peer counts, built-in congestion control, and
  encryption. The MTU/fragmentation discussion above is transport-specific and will be
  revised when the backend is selected. `MsgLanBeacon` (raw UDP) and RCON (TCP) are unaffected.

### Authenticated connect handshake (planned — Epic C)

Server-side identity adds a **signed-token** field to the connect flow: the client presents an
offline-verifiable access token (e.g. Ed25519/JWT) issued by a pluggable identity provider;
`fl-server` verifies the signature locally against the issuer's published public key (no live
callback per connect). The verified account ID — not the client-generated `PilotProfile::guid`
— keys persistent stats, ranking, and bans. Guest connections remain possible when the server
permits them. A **voice channel** (Epic J — positional + team, Opus over an unreliable channel)
is also reserved here. Exact message layout is specified when Epic C/J land.

## Notes

- **World coordinate system**: right-handed, Y-up, metres (matches glTF). Entity body `+X`
  axis is forward.
- **Position precision**: `double` throughout the engine and wire format. At 2,000 km from
  origin, float32 precision degrades to ~24 cm; double is accurate to sub-millimetre.
- **Snapshot tolerance**: `WorldSnapshot` is unreliable — dropped packets are tolerated via
  dead-reckoning. Clients extrapolate `rendered_pos = pos + vel × alpha × kTickDt` where
  `alpha = GameLoop::shellTick()` ∈ [0, 1] and `kTickDt = 1/60 s`.
- **Input channel**: `MsgClientInput` uses the unreliable channel (channel 1). The server
  applies a half-window `seqNum` staleness guard to discard out-of-order and duplicate
  packets. Per-peer one-way delay is estimated from `tickIndex` and exposed via the `peers`
  admin command.

## Client-Side Prediction

Client-side prediction (`ClientPrediction`, `game/fighters-legacy/`) reduces perceived input
latency by running a local `FlightIntegrator` that mirrors the server's physics:

1. **On each sent `MsgClientInput`**: the input is pushed into a 128-slot history ring and
   the local integrator is stepped one tick (steady wind from `MsgWeatherState` only —
   turbulence is stochastic server-side and excluded to prevent compound divergence).

2. **On each received `MsgWorldSnapshot`**: the snapshot callback (`ClientNetEventHandler::
   snapshotCallback`) is invoked before `publishExternal()`. The integrator is reset to the
   server's authoritative `FlightState` (reconstructed from the player's `EntityRenderEntry`
   including the new `omega` field), then the last `estimatedDelayTicks` history inputs are
   replayed forward. The `SnapshotPeerDelayTicks` TLV (0x0102) carries the raw tick count as
   the replay depth signal; `SnapshotPeerLatency` (0x0101, ms) continues to serve the HUD
   indicator.

3. **The player's `EntityRenderEntry` is mutated in-place** with the predicted position,
   velocity, orientation, and angular rates before the snapshot is published to
   `SimRenderBridge`. All other entities remain server-authoritative.

4. **Reconciliation**: if the new predicted position diverges from the previous prediction by
   more than `snap_threshold_m` (default 5 m), the correction is applied immediately (hard
   snap). Otherwise it is blended at `blend_rate` per reconciliation for visual smoothness.
   Both parameters are configurable via `[prediction]` in `user.toml`.

**Known limitation**: server-side turbulence is not replicated client-side (requires a
future seed-broadcast mechanism). The resulting small positional divergence is corrected each
reconciliation. Server-side lag compensation / hit-detection rewind is a separate follow-on
that builds on this infrastructure.
