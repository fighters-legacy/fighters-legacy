# Network Wire Protocol

This document describes the binary message format used between `fl-server` and game clients
over ENet UDP. All structs are defined in `engine/net/GameProtocol.h`.

## Why you can trust the netcode (for players)

`fl-server` is **authoritative**: it owns the simulation, and a client sends only *inputs*, never
positions or hits. The server integrates every aircraft, resolves every weapon, and validates state
in-tick — an input that would imply an impossible speed, position, or fire rate is rejected, not
trusted. The client *predicts* your own aircraft locally (so controls feel instant on a ~100 ms
connection) and *reconciles* to the server's truth; other players are interpolated from the
authoritative snapshot stream, and hit detection is lag-compensated on the server. On the 128+
dedicated backend the whole stream is **encrypted** (curve25519 + AES-GCM, on by default). What this
buys you: a cheating *client* cannot teleport, shoot through walls of the rules, or fabricate kills —
the server simply won't accept it. What it does not buy (and no self-hosted open game can): kernel-
level attestation of the machine on the other end; competitive integrity on a community server rests
on server-authoritative validation, offline statistical review, and community trust/reputation, not
a spyware driver. The remainder of this document is the implementor's reference.

## Transport

The wire protocol rides on a UDP transport behind the `platform/INetwork.h` HAL, so the message
format is **transport-agnostic** (`kProtocolVersion` does not change with the transport). Two
backends coexist behind the `createNetwork(TransportKind)` factory (both landed, #507/#508/#509):

- **`enet6`** (MIT) — the LAN / single-player / low-count option; also the load-test transport.
- **GameNetworkingSockets** (BSD-3, v1.6.0) — the 128+ dedicated-server default (Epic L), adding
  **transport encryption** (curve25519 + AES-GCM, on by default), mature congestion control, and
  connection-count headroom. See [gns-backend.md](gns-backend.md).

`fl-server` selects the backend via `[network] transport = "gns"|"enet"` (default `gns`) or
`--transport`; the game client uses GNS for internet multiplayer and enet6 for single-player. The two
logical channels below map to the transport's reliable / unreliable ordered lanes (ENet channels /
GNS `Reliable`/`Unreliable` send flags). **LAN discovery** (`MsgLanBeacon`, raw UDP broadcast / IPv6
multicast) and **RCON** (TCP) sit *outside* the game transport (both plaintext) and are unaffected by
the backend choice.

## Channels

| Channel | Constant | Delivery | Use |
|---------|----------|----------|-----|
| 0 | `kNetChReliable` | Ordered, guaranteed | Handshake messages |
| 1 | `kNetChUnreliable` | Best-effort datagram | World-state snapshots, client input frames |
| 2 | `kNetChVoice` | Best-effort datagram | Voice frames (Epic J, #532) |

ENet enforces ordering within each channel; reliable packets are retransmitted until
acknowledged. Unreliable packets may be dropped or arrive out of order — clients tolerate
this via dead-reckoning (`rendered_pos = pos + vel × alpha × kTickDt`).

**Why voice gets its own channel.** ENet sequences *unreliable* packets **per channel** and
discards one that arrives older than the last received on that channel. Two independent unreliable
streams sharing a channel therefore knock each other out: at ~50 voice frames/s against 60
snapshots/s, each stream's packets look stale relative to the other's and both lose frames. The
channel number is a **transport** index, not a protocol constant — `INetwork::sendChannel` routes it
and a backend with no channel concept ignores it (GNS's send flags carry the reliability, and its
lanes are not configured). Callers must not assume ordering *between* channels.

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
- **Not on the wire:** the server tick-budget instrumentation (#513) is exported as a JSON file
  (`fl-server --metrics-json`) and over the admin command channel (`tickstats`), **not** as a new
  network message — no `MsgId` and no protocol-version bump. See
  [docs/developer/load-testing.md](load-testing.md#authoritative-server-tick-budget-server_tick).

## Messages

| MsgId | Value | Direction | Channel | Size | Purpose |
|-------|-------|-----------|---------|------|---------|
| `Hello` | `0x00` | server→client | reliable | 4 bytes | Protocol version handshake; first message the **server** sends on every new connection |
| `ConnectRequest` | `0x11` | client→server | reliable | 72 + N×128 bytes | The **client**'s join request (#853): role, requested entity type, and mounted-pack manifest. Sent first on connect; the server replies `ConnectAck` or `ConnectRefusal`. |
| `ConnectAck` | `0x01` | server→client | reliable | 20 + N×336 bytes | Reply to `ConnectRequest`: granted role + assigned entity slot, then the type registry |
| `WorldSnapshot` | `0x02` | server→client | unreliable | 24 + origin table + record stream + TLV | Per-tick entity state, unicast per peer; 24-byte header + shared-origin table + a byte-aligned stitched record stream (each record: origin index + a `full` bit) + TLV extension block — see *Quantized entity record* below |
| `ClientInput` | `0x03` | client→server | unreliable | 80 bytes | Per-frame flight inputs + fire intents + selected weapon station + camera eye (observer interest) |
| `WeatherState` | `0x04` | server→client | unreliable | 32 bytes | Weather and time-of-day (+ turbulence amplitude #426, + UTC Julian Day for the geographic sun #481); broadcast every 10 ticks (~6 Hz). Additive ID — old clients silently discard. |
| `ServerNotice` | `0x05` | server→client | reliable | 64 bytes | Shutdown countdown notification; sent at each warning interval and at T=0. Additive ID — old clients silently discard. |
| `AdminCommand` | `0x06` | client→server | reliable | 128 bytes | Operator-authenticated admin command. Additive ID — old servers silently discard. |
| `AdminResponse` | `0x07` | server→client | reliable | 128 bytes | Fast-path command result (≤ 123 chars), unicast to the requesting peer. Additive ID — old clients silently discard. |
| `AdminResponseChunk` | `0x0A` | server→client | reliable | 512 bytes | Streaming chunk for long admin command output (> 123 chars). Additive ID — old clients silently discard. |
| `Motd` | `0x08` | server→client | reliable | 4 + len(text) bytes | MOTD delivered once per connection after `MsgConnectAck`; variable-length. Additive ID — old clients silently discard. |
| `ConnectRefusal` | `0x09` | server→client | reliable | 64 bytes | Rejection reason sent before `disconnectPeer()` on every `onConnect` rejection (ban, allowlist, rate-limit, per-IP limit, admin auth lockout) and on every admission failure in `handleConnectRequest` (role denied, missing pack, team full, bad password, and — #1049 — a pilot the server cannot give an aircraft). Additive ID — old clients silently discard and fall back to the generic "Connection refused by server." message. |
| `Heartbeat` | `0x0B` | client→server | unreliable | 16 bytes | Liveness signal sent at ~1 Hz when flying; carries the client's last received `tickIndex` so the server can refresh `estimatedDelayTicks`. Only sent after at least one `MsgWorldSnapshot` has been received. Additive ID — old servers silently discard. |
| `PeerDelay` | `0x0C` | server→client | unreliable | 4 bytes | Reply to `MsgHeartbeat`; delivers `estimatedDelayTicks` for this peer. Client converts to ms: `delayTicks × 1000 / 60`. `delayTicks == 0` means no valid estimate yet (client ignores). Additive ID — old clients silently discard. |
| `WingmanCommand` | `0x0D` | client→server | reliable | 16 bytes | Order a formation (#610). Authorized by **commanding the formation**, never by anything in the packet. Additive ID — old servers silently discard. |
| `WingmanAck` | `0x0E` | server→client | reliable | 16 bytes | Outcome of an order, the on-connect flight check-in, or a radio call **relayed** to a human member of someone's flight. Carries a result **code**, never server-authored text. Additive ID. |
| `CombatEvent` | `0x0F` | server→client | reliable | 4 + n×32 bytes | Kill feed (broadcast) + the receiving peer's own combat stats (unicast). A multiplexed record stream — this took the **last free ENet id**, so future gameplay events extend the record vocabulary, not the id space. Additive ID. |
| `FactionDef` | `0x10` | server→client | reliable | n×132 bytes | Faction index→id/name table, sent once after `MsgConnectAck`. Lets the client name the faction behind each entity's snapshot `factionIndex` (observer picker; future friend/foe colouring). Additive ID. |
| `Datalink` | `0x12` | server→client | unreliable | 40 + t×40 + s×28 bytes | The peer's **fused team track picture + RWR** (#528), sent per-peer at ~6 Hz. Fuses the peer's own sensor contacts with every same-faction teammate's, deduplicated by target; carries each track's `Identification` (the display-safe IFF fact, not the raw faction) and RWR strobes. Positions are float, relative to a header origin. Loss-tolerant — refreshed every send. Additive ID. |
| `CrewRoster` | `0x13` | server→client | reliable | 12 + s×44 bytes | One **crewed aircraft's seat roster** (#972): per seat the role name, capability mask, turret index, per-instance skill, and occupant (`Empty` / `Bot` / `Human(peerId)`). Sent after `MsgFactionDef` for every crewed aircraft in the world, and re-broadcast on any occupancy change. A single-seat aircraft (the implicit-single-pilot fast path) sends none. Additive ID. |
| `SeatRequest` | `0x14` | client→server | reliable | 12 bytes | Claim a non-fly crew seat, or leave the current seat (#974). A join names `{entityIdx, entityGen, seatIndex}`; the `leave` flag (bit 0) vacates whatever seat the peer holds. Free-form policy — any peer may request any non-human-held seat, including hopping aircraft mid-flight. Additive ID. |
| `SeatResult` | `0x15` | server→client | reliable | 12 bytes | Outcome of a `MsgSeatRequest` (`SeatResultCode`: Granted / NoSuchEntity / NotCrewed / NoSuchSeat / SeatOccupiedByHuman / FlySeatNotJoinable / NotInSeat). On a grant the client also receives a fresh `MsgConnectAck` (assigned entity = host aircraft) and the roster delta. Additive ID. |
| `MusicState` | `0x16` | server→client | reliable | 4 bytes | Music-state transition request (#413/#166). Broadcast when a Lua/mission script calls `world.set_music_state()`; carries a `GameState` ordinal (`Menu`/`FlightPatrol`/`FlightCombat`/`MissionSuccess`/`Debrief`), which the client maps back and drives `MusicManager::setState`. Additive ID, old clients discard. |
| `Haptic` | `0x17` | server→client | reliable | 12 bytes | Scripted rumble (#128). Broadcast when a Lua script calls `rumble()` / `rumble_triggers()` / `stop_rumble()`; carries a `HapticKind` ordinal + two intensities (`a`, `b`, `[0,1]`) + `durationMs` (`[0, 5000]`, engine-clamped). Each client plays it on its local (current-player) gamepad. Additive ID, old clients discard. |
| `MissionOutcome` | `0x18` | server→client | reliable | 8 bytes | The mission's terminal outcome (#584). Broadcast once when the objective evaluator drives the mission to Complete/Failed; carries a `MissionResultCode` (`Incomplete`/`Success`/`Failure`) + `triggersFired` + `elapsedSeconds`, so the client debrief reports the real result instead of a hardcoded success. Additive ID, old clients discard. |
| `RadioCommand` | `0x19` | client→server | reliable | 64 bytes | A player radio command (#703). Verb-routed like the admin channel — `atc request_takeoff\|request_landing\|inbound\|cancel [facility]` — never a direct state mutation; the server dispatches to the ATC service (#702) and replies with `RadioTransmission`(s), rate-limited per peer. The `wing` verb namespace is reserved for #610. **Note:** `0x0D`/`0x0E` (the ids #703 originally reserved) were already taken by the wingman channel, so the radio channel took the next free ENet ids `0x19`/`0x1A`. Additive ID, old servers discard. |
| `RadioTransmission` | `0x1A` | server→client | reliable | 224 bytes | One spoken radio line (#703): `speaker` (28), `voiceKey` (32, a stable TTS/pack-OGG key — empty = subtitle only), `text` (160, server-rendered + localizable), `displaySeconds`. Unicast to the addressed pilot, or broadcast (an AI flight's clearance / an undirected line). The client prints `[radio] speaker: text` to the console and feeds the comms-menu subtitle/voice pipeline (#704). Additive ID, old clients discard. |
| `MissionRoster` | `0x1B` | server→client | reliable | n×72 bytes | Entity idx/gen → mission object id table (#914). Concatenated self-describing records sent once after `MsgConnectAck` (with the current spawned mission objects + bound player slots), plus single-record deltas as a player slot binds. Lets the cinematic recorder (#909) resolve an entity-relative camera shot's `target`/`look_at` (a mission object id) to a live network entity. Additive ID, old clients discard. |
| `PlayerRoster` | `0x1C` | server→client | reliable | 4 + n×40 bytes | Match roster upsert/leave stream (#996): participant id → callsign / faction / role. Broadcast on join / role change / leave; the full roster is chunked to a late joiner after `MsgConnectAck`. The single name source for chat, kill feed and scoreboard. Establishes the participant-id model (humans = peerId, bots = `kBotParticipantBase + n`). Additive ID. |
| `MatchState` | `0x1D` | server→client | reliable | 80 + n×8 bytes | Match phase + per-team scores + limits + phase clock (#523). Broadcast on change and unicast to a late joiner. Client renders remaining = `phaseEndTick − tickIndex`. Additive ID. |
| `Scoreboard` | `0x1E` | server→client | unreliable | 8 + n×16 bytes | Per-participant kills/deaths/score/ping (#523), every ~2 s + on admit. Self-describing; a dropped one is replaced. Additive ID. |
| `TeamRequest` | `0x1F` | client→server | reliable | 4 bytes | Request a mid-match team switch (#522). Guarded against unbalancing server-side. Additive ID. |
| `AlertLevelChange` | `0x26` | server→client | reliable | 4 bytes | A faction's airspace readiness posture changed (#162). Sent once per faction after `MsgConnectAck` and again on every change, so a late joiner starts from the live value. Carries an `AlertLevel` ordinal (`Peacetime`/`Elevated`/`Conflict`/`WarState`) — gate it with `isAlertLevelOrdinal()` before casting. Additive ID, old clients discard. |
| `GmWorldState` | `0x22` | server→client | reliable | variable | The game-master overview-map aggregate (#861): the whole-world picture a `gm`-granted peer sees, independent of their own interest set. Sent only to peers holding the capability. Additive ID. |
| `VoiceNetDef` | `0x23` | server→client | reliable | 4 + n×68 bytes | The server's radio-net table (id / name / kind / profile), Epic J (#532). Sent once so the client can label which net a push-to-talk key transmits on. Field table [below](#msgvoicenetdef--4--n68-bytes). Additive ID. |
| `VoiceFrame` | `0x24` | client→server | unreliable (voice channel) | 8 + payload | One 20 ms Opus frame from a transmitting client (#532). The server never decodes it. Field table [below](#msgvoiceframe--8--payload-bytes). Additive ID. |
| `VoiceRelay` | `0x25` | server→client | unreliable (voice channel) | 16 + payload | The same opaque payload relayed to listeners on the net, plus who sent it (#532). Field table [below](#msgvoicerelay--16--payload-bytes). Additive ID. |
| `Chat` | `0x20` | client→server | reliable | 4-byte `MsgChatHeader` + NUL-terminated UTF-8 text (≤ `kMaxChatBytes` = 240) | In-match chat line (#646). Header: `channel` @1 (`ChatChannel` All/Team). The server sanitizes (BMP UTF-8, control chars stripped, codepoint-boundary truncation), per-peer rate-limits, applies mute + a moderation hook, then routes a `ChatEvent`. Additive ID. |
| `ChatEvent` | `0x21` | server→client | reliable | 8-byte `MsgChatEventHeader` + NUL-terminated UTF-8 text | Routed chat line (#646). Header: `channel` @1, `senderPeerId` @4 (participant id; `kNoOwningPeer` = a system line with no sender name). All-channel lines reach every handshake-complete peer incl. the sender's own echo; Team-channel lines reach only the sender's faction. Additive ID. |
| `LanBeacon` | `0x40` | server→LAN | raw UDP (not ENet) | 76 bytes | LAN server presence broadcast. The ENet id space is `0x00–0x3F`; `0x40+` is reserved for raw-UDP/non-ENet ids (the boundary was raised from `0x20` in #996 to free ENet ids for the Epic E messages). Carries `gameModeFlags` (incl. `kGameModeShuttingDown` #226 and `kGameModePassworded` #998) + `shutdownSeconds`. |
| `ServerQuery` | `0x41` | client→server | raw UDP | 192 bytes | A2S-style server-info request (#997), sent to the query port rather than the game port. Answered by the query responder, which runs by default (`[discovery] query_enabled`); the port is advertised in `MsgLanBeacon`, and defaults to game port + 1. |
| `ServerInfo` | `0x42` | server→client | raw UDP | variable | Reply to `MsgServerQuery` (#997): live name, player count and mode flags for a browser's details and ping columns. |

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

### Connect handshake (#853)

The join flow is a round trip. On connect the **server** runs its rejection gauntlet and sends
`MsgHello`; the **client** sends `MsgConnectRequest` (role + requested entity type + mounted-pack
manifest). The two cross on independent reliable streams — neither waits for the other. The server
then admits the peer and replies `MsgConnectAck` (granting a role and, for a pilot, an entity), or
`MsgConnectRefusal`. A peer that never sends a request is never admitted (no entity, no snapshots).

### MsgHello — 4 bytes

Sent by the server on every new connection (reliable channel 0). The client must compare
`protocolVersion` against its own compiled `kProtocolVersion` and call `disconnect()` immediately on
mismatch.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x00` |
| 1 | 1 | `reserved` | `uint8_t` | Reserved, always 0 |
| 2 | 2 | `protocolVersion` | `uint16_t` | Server's `kProtocolVersion`; client disconnects if this != its own `kProtocolVersion` |

### MsgConnectRequest — 72 bytes (+ N×128-byte manifest records)

The client's first packet on connect (reliable channel 0). Followed by `packCount` ×
`PackManifestEntry` records, then an optional TLV extension block (ExtTag `0x0500–0x05FF`, reserved
for the RFC #871 entitlement token). `requestedEntityType` empty = let the server pick its
`[world] player_entity_type` default (server-clamped to a registered type).

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x11` |
| 1 | 1 | `requestedRole` | `uint8_t` | `PeerRole`: 0 = Pilot, 1 = Observer (server may clamp/deny) |
| 2 | 2 | `protocolVersion` | `uint16_t` | Client's `kProtocolVersion` |
| 4 | 2 | `packCount` | `uint16_t` | Number of trailing `PackManifestEntry` records |
| 6 | 2 | `reserved` | `uint16_t` | Reserved, always 0 |
| 8 | 64 | `requestedEntityType[64]` | `char[64]` | Null-terminated type id to fly; empty = server default |

#### PackManifestEntry — 128 bytes

One mounted content pack, reported for content-consistency negotiation (#872). `contentHash` is
reserved (zero-filled) until a pack-hashing pass lands.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 64 | `id[64]` | `char[64]` | Null-terminated pack id, e.g. `"fl-base"` |
| 64 | 32 | `version[32]` | `char[32]` | Null-terminated version string |
| 96 | 32 | `contentHash[32]` | `uint8_t[32]` | Reserved; all-zero = not computed |

### MsgConnectAck — 20 bytes

Reply to `MsgConnectRequest` (reliable channel 0), immediately followed by `typeCount` ×
`MsgEntityTypeDef` records. Also re-sent on a mid-session role change (#857). `grantedRole` may
differ from the requested role, and is load-bearing: an observer's valid ack carries
`assignedEntityGen == 0` (no entity), so the client keys "rejected before ack" on **whether a
ConnectAck arrived**, not on `assignedEntityIdx == 0`.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x01` |
| 1 | 1 | `tickRateHz` | `uint8_t` | Server tick rate (60) |
| 2 | 2 | `typeCount` | `uint16_t` | Number of `MsgEntityTypeDef` records that follow |
| 4 | 4 | `assignedEntityIdx` | `uint32_t` | Pool slot of the entity assigned to this peer (0 = none, e.g. observer) |
| 8 | 4 | `assignedEntityGen` | `uint32_t` | Entity generation; 0 = no entity assigned |
| 12 | 4 | `planetRadiusKm` | `float32` | Planet sphere radius in km; Earth default = 6371.0 |
| 16 | 1 | `grantedRole` | `uint8_t` | `PeerRole` granted by the server (0 = Pilot, 1 = Observer) |
| 17 | 3 | `reserved2[3]` | `uint8_t[3]` | Padding to keep trailing records 4-aligned |

### MsgEntityTypeDef — 348 bytes

Appended N times after `MsgConnectAck` (one per registered entity type).

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 4 | `typeIndex` | `uint32_t` | Index into server-side `EntityTypeRegistry` |
| 4 | 64 | `id[64]` | `char[64]` | Null-terminated type ID, e.g. `"builtin:debug-entity"` |
| 68 | 64 | `mesh[64]` | `char[64]` | Null-terminated mesh asset name; empty = builtin placeholder shape |
| 132 | 64 | `dmgMesh[64]` | `char[64]` | Null-terminated damage mesh; empty = none |
| 196 | 64 | `flightModel[64]` | `char[64]` | Null-terminated flight-model **asset name** (not a def id); empty = builtin model |
| 260 | 4 | `payloadMassKg` | `float32` | Default-loadout store mass (kg); 0 = clean airframe |
| 264 | 4 | `payloadCd0` | `float32` | Default-loadout parasite-drag delta; 0 = clean airframe |
| 268 | 64 | `name[64]` | `char[64]` | Null-terminated friendly display name (`EntityDef::name`), e.g. `"F-16C"`; empty = client falls back to `id` (#860) |
| 332 | 1 | `category` | `uint8_t` | `ObjectCategory` ordinal; client gates via `isObjectCategoryOrdinal` before the cast, invalid → AirVehicle (#886) |
| 333 | 1 | `projectileKind` | `uint8_t` | `ProjectileKind` ordinal (Projectile types only); gated, invalid → None (#886) |
| 334 | 2 | `reservedCat[2]` | `uint8_t[2]` | Padding (kept so the #886 offsets stay frozen) |
| 336 | 4 | `deckLengthM` | `float32` | Flight-deck footprint along the keel (m); 0 = the type has no deck (#38) |
| 340 | 4 | `deckWidthM` | `float32` | Flight-deck footprint abeam (m) |
| 344 | 4 | `deckHeightM` | `float32` | Deck plane height above the ship origin (m) |

`flightModel` (#811) exists because the client must integrate the **same** aircraft the server does.
Without it the client had no way to learn an entity type's flight model, silently fell back to the
builtin model, and diverged from the server permanently. `payloadMassKg` / `payloadCd0` (#812) are the
aggregate cost of the type's default loadout: the client has no hardpoints and no weapon registry, so
it receives the two numbers rather than the data to derive them. `name` (#860) is the human-readable
label the observer entity picker shows. `category` / `projectileKind` (#886) drive the per-category
builtin placeholder silhouettes (and future picker grouping / map icons) — without them every
client-side def read back as an AirVehicle. `deckLengthM/WidthM/HeightM` (#38) carry a carrier's
flight-deck footprint so client prediction composes its ground floor as max(terrain, moving deck)
exactly as the server does; the catapult/arrest parameters stay server-side (server-authoritative
events). All were **appended at the tail**, so every pre-existing field offset is unchanged and
`kProtocolVersion` stays at 1.

### MsgFactionDef — 132 bytes

Concatenated one-per-faction into a single reliable packet (leading `msgId` = `FactionDef`) sent once
after `MsgConnectAck`, when the server has a faction registry. The client reads `size / 132` records,
each self-describing via `factionIndex`, and builds an index→name table so it can label the faction
behind each entity's snapshot `factionIndex` (the observer picker; future friend/foe HUD colouring).
Skipped entirely when the server has no faction registry — the client then shows the index alone.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x10` |
| 1 | 1 | `reserved` | `uint8_t` | Zero |
| 2 | 2 | `factionIndex` | `uint16_t` | `FactionRegistry` index this record describes |
| 4 | 64 | `id[64]` | `char[64]` | Null-terminated faction id, e.g. `"blue"` |
| 68 | 64 | `name[64]` | `char[64]` | Null-terminated display name, e.g. `"Blue Coalition"`; empty = fall back to `id` |

### MsgAlertLevelChange — 4 bytes

Server→client, **reliable**. One faction's airspace readiness posture (#162), which selects which
dwell row every airspace zone that faction owns applies to intruders. Sent on two paths through the
same message, so the client has one decode branch rather than two: once per faction in the
`MsgConnectAck` sequence (beside `MsgFactionDef`), and again whenever `AlertSystem::setAlertLevel`
moves a faction — from a mission Lua `world.set_alert_level()`, an admin action, or a #163
`WorldEvolutionDelta`. Reliable, because a posture change that goes missing leaves a client
displaying peacetime during a war.

`level` is an `AlertLevel` ordinal, and those ordinals are a wire contract — renumbering the enum
would silently change what a posture means to every connected client. Gate the byte with
`fl::isAlertLevelOrdinal()` before casting it.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x26` |
| 1 | 1 | `level` | `uint8_t` | `AlertLevel`: 0 `Peacetime`, 1 `Elevated`, 2 `Conflict`, 3 `WarState` |
| 2 | 2 | `factionIndex` | `uint16_t` | `FactionRegistry` index; matches `MsgFactionDef::factionIndex` |

### MsgMissionRoster — 72 bytes

Concatenated one-per-mapping into a single reliable packet (leading `msgId` = `MissionRoster`) sent
once after `MsgConnectAck`, plus single-record deltas as a player slot binds its aircraft. The client
reads `size / 72` records and builds a `missionObjectId → {entityIdx, entityGen}` table so the cinematic
recorder (#909) can resolve an entity-relative camera shot's `target`/`look_at` to a live network
entity. Only entries with a valid entity are sent (an unbound player slot is omitted until it binds). No
mission ⇒ no packet.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x1B` |
| 1 | 1 | `reserved` | `uint8_t` | Zero |
| 2 | 2 | `entityGen` | `uint16_t` | `EntityId` generation (guards against a pool-slot reuse) |
| 4 | 4 | `entityIdx` | `uint32_t` | `EntityId` pool index |
| 8 | 64 | `objectId[64]` | `char[64]` | Null-terminated mission object id, e.g. `"bandit1"` |

### MsgDatalink — 40 + trackCount×40 + threatCount×28 bytes

The fused **team track picture + RWR** (#528), sent per-peer unreliably at ~6 Hz (`kDatalinkIntervalTicks`).
The server fuses the peer's own sensor contacts with every same-faction teammate's, deduplicated by
target — so you see the bandit your wingman locked even if your own radar never found it. Track/threat
positions are `float`, **relative to `origin`** (the peer's aircraft), so the picture stays float-precise
at any world scale; the client reconstructs absolute world positions as `origin + relPos`. A faction-0
(neutral) peer forms no team and receives only its own contacts. Each track carries the display-safe
`Identification` (Friend/Foe/Unknown — see [IFF, #527]), never the raw faction. Loss-tolerant: a dropped
packet is one stale refresh. `MsgDatalinkHeader`:

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x12` |
| 1 | 1 | `flags` | `uint8_t` | Reserved |
| 2 | 2 | `trackCount` | `uint16_t` | `DatalinkTrack` records following the header (≤ `kMaxDatalinkTracks` = 48) |
| 4 | 2 | `threatCount` | `uint16_t` | `DatalinkThreat` records following the tracks (≤ `kMaxDatalinkThreats` = 16) |
| 6 | 2 | `reserved` | `uint16_t` | Zero |
| 8 | 8 | `tickIndex` | `uint64_t` | Server tick the picture was built on |
| 16 | 24 | `origin[3]` | `double[3]` | World position the relative track/threat positions are measured from |

`DatalinkTrack` (40 bytes, one per fused target):

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 4 | `targetIdx` | `uint32_t` | Target entity index |
| 4 | 4 | `typeIndex` | `uint32_t` | Target entity type |
| 8 | 2 | `targetGen` | `uint16_t` | Target entity generation |
| 10 | 2 | `factionIndex` | `uint16_t` | Target's actual faction (the client colours by `ident`, not this) |
| 12 | 1 | `state` | `uint8_t` | `ContactState` (Lost/Detected/Locked/Coasting) |
| 13 | 1 | `ident` | `uint8_t` | `Identification` (Unknown/Friend/Foe) — the display-safe fact |
| 14 | 1 | `sensorTypeMask` | `uint8_t` | `1<<SensorType` — which kinds of sensor hold it across the team |
| 15 | 1 | `flags` | `uint8_t` | bit 0 = firing-quality (STT) lock; bit 1 = this peer's own sensors hold it |
| 16 | 12 | `relPos[3]` | `float[3]` | Position relative to `origin` |
| 28 | 12 | `relVel[3]` | `float[3]` | Velocity (world frame) |

`DatalinkThreat` (28 bytes, one per RWR strobe):

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 4 | `emitterIdx` | `uint32_t` | Emitter entity index |
| 4 | 4 | `emitterTypeIndex` | `uint32_t` | Emitter entity type |
| 8 | 2 | `emitterGen` | `uint16_t` | Emitter entity generation |
| 10 | 2 | `emitterFactionIndex` | `uint16_t` | Emitter's faction |
| 12 | 1 | `channel` | `uint8_t` | `SensorType` (radar / laser) |
| 13 | 1 | `level` | `uint8_t` | `ThreatLevel` — 0 search strobe, 1 lock tone, 2 launch (a radar-guided missile guiding on you, #960) |
| 14 | 1 | `ident` | `uint8_t` | `Identification` of the emitter (a friendly emitter reads benign) |
| 15 | 1 | `flags` | `uint8_t` | Reserved |
| 16 | 12 | `relPos[3]` | `float[3]` | Emitter position relative to `origin` |

### MsgCrewRoster — 12 + seatCount×44 bytes

Reliable, server→client (#972). One crewed aircraft's full seat roster. Sent after `MsgFactionDef` for
every crewed aircraft in the world, and re-broadcast on any occupancy change (a human joining/leaving a
seat). A single-seat aircraft never sends one. `MsgCrewRosterHeader`:

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x13` |
| 1 | 1 | `seatCount` | `uint8_t` | Number of trailing `CrewRosterSeat` records |
| 2 | 1 | `turretCount` | `uint8_t` | Turret mounts on the aircraft (sizes the client's pose arrays) |
| 3 | 1 | `reserved` | `uint8_t` | Pad |
| 4 | 4 | `entityIdx` | `uint32_t` | Aircraft entity index |
| 8 | 4 | `entityGen` | `uint32_t` | Aircraft entity generation (guards a pool-slot reuse) |

Then `seatCount` × `CrewRosterSeat` (44 bytes each, align 4):

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `seatIndex` | `uint8_t` | Seat ordinal |
| 1 | 1 | `occupancy` | `uint8_t` | `SeatOccupancy`: 0 Empty, 1 Bot, 2 Human (`isSeatOccupancyOrdinal` guards it) |
| 2 | 2 | `capabilities` | `uint16_t` | `CrewCapabilityMask` — Fly / Fire / Radar / Countermeasures / Command |
| 4 | 4 | `occupantPeerId` | `uint32_t` | Human peer id when `occupancy == Human`, else `kNoSeatPeer` (`0xFFFFFFFF`) |
| 8 | 1 | `skillPct` | `uint8_t` | Per-instance skill × 100, `[0,100]` |
| 9 | 1 | `turretIndex` | `uint8_t` | Turret this seat aims; `255` = none |
| 10 | 1 | `knockedOut` | `uint8_t` | `1` = the seat is knocked out (silent, #978); orthogonal to occupancy |
| 11 | 1 | `reserved` | `uint8_t` | Pad (keeps `role` 4-aligned) |
| 12 | 32 | `role` | `char[32]` | Null-terminated display string (roles-as-data, #944) |

### MsgSeatRequest — 12 bytes

Reliable, client→server (#974). Claim a non-fly crew seat or leave the current one.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x14` |
| 1 | 1 | `flags` | `uint8_t` | bit 0 (`kSeatRequestFlagLeave`) = vacate current seat (entity/seat ignored) |
| 2 | 1 | `seatIndex` | `uint8_t` | Seat to claim |
| 3 | 1 | `reserved` | `uint8_t` | Pad |
| 4 | 4 | `entityIdx` | `uint32_t` | Target aircraft index |
| 8 | 4 | `entityGen` | `uint32_t` | Target aircraft generation |

### MsgSeatResult — 12 bytes

Reliable, server→client (#974). The outcome of a `MsgSeatRequest`; echoes the target for correlation.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x15` |
| 1 | 1 | `code` | `uint8_t` | `SeatResultCode`: 0 Granted, 1 NoSuchEntity, 2 NotCrewed, 3 NoSuchSeat, 4 SeatOccupiedByHuman, 5 FlySeatNotJoinable, 6 NotInSeat (`isSeatResultOrdinal` guards it) |
| 2 | 1 | `seatIndex` | `uint8_t` | Echoed requested seat |
| 3 | 1 | `reserved` | `uint8_t` | Pad |
| 4 | 4 | `entityIdx` | `uint32_t` | Echoed target index |
| 8 | 4 | `entityGen` | `uint32_t` | Echoed target generation |

### MsgWorldSnapshotHeader — 24 bytes

Sent unreliably per-peer every sim tick (channel 1). The body after the header is (#725):
**origin table** of `originCount` × `double[3]` grid-cell quantization origins, then a **byte-aligned
stitched record stream** of `recordCount` entity records occupying `bitstreamBytes` bytes, then the
TLV extension block. The record stream begins at `24 + originCount×24`; the TLV block begins at
`24 + originCount×24 + bitstreamBytes`. See [snapshot-quantization.md](decisions/snapshot-quantization.md) for
the record bit-layout and the codec. Sized to 24 (a multiple of 8) so the origin table's `double`s are
8-aligned and the header reads in place.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x02` (byte-0 dispatch position unchanged) |
| 1 | 1 | `protocolVersion` | `uint8_t` | Server's `kProtocolVersion`; per-packet version stamp |
| 2 | 2 | `recordCount` | `uint16_t` | Number of stitched entity records in the record stream |
| 4 | 4 | `bitstreamBytes` | `uint32_t` | Byte length of the record stream (after the origin table); the TLV block starts at `24 + originCount×24 + bitstreamBytes` |
| 8 | 8 | `tickIndex` | `uint64_t` | Monotonically increasing server tick counter (8-aligned) |
| 16 | 2 | `originCount` | `uint16_t` | Number of `double[3]` shared-origin entries in the table that follows this header |
| 18 | 2 | `flags` | `uint16_t` | `kSnapshotFlag*` bits (#775). Bit 0 = `kSnapshotFlagCompressed`: the whole body after this header is one zstd frame. 0 = raw body |
| 20 | 4 | `uncompressedBytes` | `uint32_t` | Decompressed body length when bit 0 of `flags` is set; 0 otherwise |

Immediately after the header: `originCount` entries of `double[3]` (24 bytes each) — the distinct
grid-cell quantization origins (`floor(pos / kOriginGridM) * kOriginGridM`) the records reference.

#### Compressed snapshot body (#775)

When `flags & 0x0001` (`kSnapshotFlagCompressed`), everything after the 24-byte header — origin
table, record stream, and TLV block — is a single **zstd** frame whose decompressed length is
exactly `uncompressedBytes`; `recordCount`/`originCount`/`bitstreamBytes` always describe the
**decompressed** layout. The header itself is never compressed, so byte-0 dispatch, the
protocol-version stamp, and the receiver's out-of-order tick guard work without a decompress. The
sender uses compression only when it strictly wins: bodies under `kMinSnapshotCompressBytes`
(128) or that do not shrink are sent raw with `flags == 0`, byte-identical to a
compression-disabled server. Receivers must bound `uncompressedBytes` by
`kMaxSnapshotPayloadBytes` (4 MiB) before allocating, and must reject a frame whose decoded
length differs from the claim (`fl::decompressSnapshotPayload` in
`engine/net/SnapshotCompression.h` — the single audited path, used by the game client and the
tests alike). Compression is transport-agnostic and server-controlled (`[network]
compress_snapshots`, default on): enet6's range coder used to compress on the wire, GNS (the
default internet transport) does not compress at all, so the engine owns the codec and both
backends carry identical bytes.

**Version decision (#775):** `kProtocolVersion` stays 1. A pre-#775 client that received a
compressed snapshot would mis-parse the zstd frame as an origin table and fail record decode
(fail-closed in `decodeStandaloneRecord`, no crash) — per the standing primary-development
convention, both sides update together and the version bump is reserved for the 1.0 wire freeze.

### Quantized entity record (byte-aligned, stitched)

The entity body is **not** a fixed struct array — it is a byte-aligned stream of `recordCount` records
produced by `engine/net/SnapshotCodec`. Each record is peer-independent (absolute idx, position
relative to a shared origin), so the sim encodes it once per tick and stitches the blob into every
peer's stream. Full field semantics and the quantization constants are in
[snapshot-quantization.md](decisions/snapshot-quantization.md). The fixed `MsgEntityEntry` (88 B) and
`MsgEntityUpdate` (64 B) structs were removed in #515.

| Field | Bits | Notes |
|---|---|---|
| `originIndex` | varint | index into the origin table (written at stitch time) |
| `idx` | varint | **absolute** `entityIdx` (peer-independent; blobs stitch in any order) |
| `full` | 1 | full record (carries typeIndex + factionIndex + gen) vs. delta |
| `genPresent` | 1 | generation on the wire; else client reuses its cache |
| `omegaPresent` | 1 | angular rates present (set only for the receiving peer's own entity) |
| `gen` | 16 | only if `genPresent`; truncated `EntityId::generation` |
| `typeIndex` | varint | only if `full` |
| `factionIndex` | 16 | only if `full`; `FactionRegistry` index, client-cached like `typeIndex` (#860) |
| position | 3 × 22 | signed offset from the record's shared grid origin, 0.125 m resolution, ±262 km range |
| orientation | 2 + 3 × 10 | smallest-three quaternion (dropped-component index + 3 components) |
| velocity | 3 × 18 | ± 2000 m/s range |
| omega | 3 × 12 | only if `omegaPresent`; ± 20 rad/s range, body-frame p,q,r |
| byte fields | 3 + 5 + 7 + 7 + 1 + 1 | damageLevel, engineFailFlags, throttle [0–100], fuelPct [0–100], abEngaged, playerOwned |

`engineFailFlags` bits: `0x01` generic thrust impairment (`damageLevel ≥ Severe`), `0x02` left-engine,
`0x04` right-engine, `0x08` compressor stall, `0x10` flameout (last four Phase 6+). Orientation wire
order is `[x, y, z, w]`; the GLM constructor is `(w, x, y, z)`.

### MsgClientInput — 80 bytes

Sent by the client each render frame on the **unreliable channel (channel 1)**. Padded to a
multiple of 8 for the 8-aligned `tickIndex` and the trailing `cameraEye` doubles. For a continuous
60 Hz control stream, unreliable
delivery is correct: a dropped packet is superseded by the next frame's input; retransmission would
delay all subsequent inputs behind the ACK round-trip.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x03` |
| 1 | 1 | `buttons` | `uint8_t` | Bit 0 = gun trigger (level), bit 1 = afterburner, bit 2 = fire selected store, **bit 3 = dispense chaff/flare** (level; server edge-detects — holding it is one pop, #529), **bit 4 = ECM jammer on** (level), **bit 5 = eject** (level; server edge-detects — a held key is one ejection, #672). Fire/EW bits are **intents**: the server validates station/ammo/rate/weapons-hold, edge-detects the store-release, dispense, and eject bits, and applies ECM to the entity |
| 2 | 2 | `protocolVersion` | `uint16_t` | Client's `kProtocolVersion`; server discards packet and logs a warning on mismatch |
| 4 | 4 | `seqNum` | `uint32_t` | Monotonically increasing per-client sequence counter; server applies a half-window comparison to discard out-of-order and duplicate packets |
| 8 | 8 | `tickIndex` | `uint64_t` | Server `tickIndex` from the client's last received `MsgWorldSnapshot`. Server computes `estimatedDelayTicks = currentTick − tickIndex` for diagnostics, and also uses it as the **snapshot ack** high-water mark (clamped to the current tick), paired with `ackMask` below, that drives client-acked delta baselines — see *Scaling to 128+* |
| 16 | 4 | `throttle` | `float` | `[0.0, 1.0]` |
| 20 | 4 | `elevator` | `float` | `[-1.0, +1.0]` nose-up positive |
| 24 | 4 | `aileron` | `float` | `[-1.0, +1.0]` right-roll positive |
| 28 | 4 | `rudder` | `float` | `[-1.0, +1.0]` right-yaw positive |
| 32 | 12 | `viewAxis[3]` | `float[3]` | Normalized camera look direction (world space) |
| 44 | 4 | `ackMask` | `uint32_t` | Selective-ack bitmask of recently **decoded** snapshot ticks below `tickIndex`: bit `b` = tick `tickIndex − 1 − b` was decoded (`tickIndex` itself is implicitly decoded). Lets the server confirm the specific tick a full record was sent in rather than a high-water mark — see *Scaling to 128+* |
| 48 | 1 | `selectedStation` | `uint8_t` | **Absolute** selected weapon station index; `255` = keep the current (server-default) selection. Absolute rather than cycle-edges so selection converges under loss on this unreliable channel; the client computes Next/Prev cycling locally and sends the result. Server clamps to the entity's station count |
| 49 | 1 | `radarMode` | `uint8_t` | **Absolute** radar operating mode (#526): `0` Silent (EMCON), `1` Search (bearing only), `2` TWS (soft multi-track), `3` STT (one firing-quality lock); `255` = keep the current server-side mode (unaware clients / load bots leave the spawned TWS mode). Absolute for the same reason as `selectedStation` — it converges under loss. Applied to the peer's own aircraft observer before the sensing pass each tick |
| 50 | 1 | `flaps` | `uint8_t` | **Absolute** commanded flap position, `0..255` ⇒ `0..1` (#843). The server slews the actuator toward it at the model's `flap_transit_s`; drag follows the resulting POSITION |
| 51 | 1 | `speedbrake` | `uint8_t` | **Absolute** commanded speed-brake, `0..255` ⇒ `0..1`. The player's speed-brake was not on the wire at all before #843 — only Lua AI could deploy one |
| 52 | 1 | `artButtons` | `uint8_t` | Articulation state bits (#843): bit 0 gear down, bit 1 hook down, bit 2 canopy open. **Absolute state, not edges** — an edge lost on this unreliable channel never converges, an absolute value does on the very next packet. A separate byte because `buttons` ran out at bit 7 |
| 53 | 3 | `reservedC[3]` | `uint8_t[3]` | Zero (explicit padding keeping `cameraEye` 8-aligned) |
| 56 | 24 | `cameraEye[3]` | `double[3]` | Camera eye world-position (absolute metres). The server centers interest management on this for an **entity-less peer** (an observer ghost camera, or a dead peer awaiting respawn) that has no aircraft transform to key interest on; ignored for a pilot, whose aircraft transform wins. Finite-guarded server-side |

The server clamps all control surface inputs to their valid ranges and normalises `viewAxis` to unit
length. Packets smaller than 80 bytes are silently discarded. ENet's sequenced unreliable delivery
provides a first layer of ordering; the application-level `seqNum` guard adds defense-in-depth.

After passing validation the server enqueues each accepted input into a per-peer ring buffer
(`JitterBuffer`, depth ≤ `[world].jitter_buffer_depth`, default 4 ticks). The buffer is drained
exactly once per sim tick before the flight integrator is stepped; when the buffer runs empty the
last drained input is repeated (stale repeat) rather than zeroing controls, preventing coasting
under transient packet loss — with **bit 2 masked off** in the repeated copy, so a fire-store
intent is never manufactured by the loss-concealment path. The initial buffer depth per peer is `min(estimatedDelayTicks, maxDepth)`
seeded at first input. The depth is then continuously adjusted each tick: an EWMA of
`estimatedDelayTicks` and an RFC 3550-style inter-arrival jitter estimate drive
`target = ceil(ewma_delay + k × jitter)`, clamped to `[1, jitter_buffer_depth]`; resize fires only
when `|target − current| > hysteresis`. Configurable via `[world].jitter_buffer_adapt_window`,
`jitter_buffer_hysteresis`, and `jitter_buffer_jitter_multiplier`.

### MsgWeatherState — 32 bytes

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
| 20 | 4 | `turbulenceAmp` | `float` | turbulence amplitude (m/s), #426. The client feeds it to the same deterministic `weatherTurbulence(entityIdx, tickIndex, amp)` the server uses, so client-side prediction reproduces the per-tick turbulence exactly. Tail-append (grew 20→24); additive, no version bump. |
| 24 | 8 | `utcJulianDay` | `double` | shared UTC clock (calendar date + fractional time-of-day) as a Julian Day, #481. The client combines it with its own camera latitude/longitude (`worldToGeodetic` of the eye) to compute the **geographic** sun each frame — so the day/night terminator tracks longitude and two players far apart see different local suns. `timeOfDayTenths` remains the coarse HUD clock. Tail-append (grew 24→32, alignment 4→8); additive, no version bump. |

Wind convention: `windX` and `windZ` are the **blowing-toward** direction. A westerly wind (FROM 270°) has `windX > 0`.

Turbulence (#426): the server's per-tick turbulence perturbation is a pure function of
`(entityIdx, tickIndex, turbulenceAmp)` — no shared PRNG state — so broadcasting only the scalar
amplitude lets `ClientPrediction` reproduce it **exactly** for the player's own entity (the same
one-function-two-callers discipline as the stall buffet), rather than approximating per-tick vectors.
Previously prediction excluded turbulence entirely and diverged by the full amplitude every gusty
tick, leaving visible reconciliation jitter.
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
authentication clears the counter for that IP. See `docs/server-ops/server-config.md` for the
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
- **Missing required pack** (#872, `required_policy = "refuse"`): `"Missing required pack(s): <list>"`

ENet's graceful disconnect flushes all pending reliable packets before completing the
disconnect sequence, so the client receives this packet before the ENet disconnect event fires.
The client stores the reason via CAS into `connectFailMsg`, which the `onDisconnect` fallback
CAS then fails to overwrite, surfacing the specific reason in the `LoadingScreen`.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x09` |
| 1 | 1 | `code` | `uint8_t` | `ConnectRefusalCode`: 0=Generic, 1=Banned, 2=AccessDenied, 3=RateLimited, 4=TooManyConnections, 5=AdminLockout, 6=RoleDenied (#857), 7=MissingRequiredPack (#872 — `reason` names the missing pack(s)), 8=EntitlementRequired (RFC #871, reserved), 9=MatchFull (#522 — every team at capacity), 10=BadPassword (#998), 11=ServerFull (#1049 — the world is at `[world] entity_soft_cap`; **retryable**), 12=NoAirframe (#1049 — no spawnable `player_entity_type`; an operator configuration fault, not retryable) — machine-readable reason paired with the text below |
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
| 1 | 3 | `reserved[3]` | `uint8_t[3]` | padding to 4-align `ackMask` |
| 4 | 4 | `ackMask` | `uint32_t` | selective-ack bitmask; same semantic as `MsgClientInput::ackMask` |
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

### MsgWingmanCommand — 16 bytes

Reliable, **client→server**, channel 0. Orders a formation — the scripted wingman command grammar
(#610). See `engine/ai/WingmanCommand.h` for the six commands; the ordinal on the wire is
`fl::ai::WingmanCommand`.

**Authority never comes out of the packet.** The server resolves the addressed formation and checks
that the *sender* commands it (directly, or via an ancestor — command cascades down the formation
tree). A peer that does not command it receives `NoFlight` — deliberately the **same** code an
unknown formation returns, so the order channel cannot be used to enumerate which formations exist
or who leads them.

This is *not* `MsgAdminCommand`: that is a password-gated shell tunnel, and a gameplay order is
authorized by a role in the world, not by a token.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x0D` |
| 1 | 1 | `command` | `uint8_t` | `fl::ai::WingmanCommand` ordinal. `>= 6` is rejected with `Rejected`. |
| 2 | 2 | `protocolVersion` | `uint16_t` | `kProtocolVersion`; mismatch is discarded. |
| 4 | 4 | `memberIdx` | `uint32_t` | Entity pool index of one addressed member, or `kFlightAll` (`0xFFFFFFFF`) for the whole formation. |
| 8 | 4 | `seqNum` | `uint32_t` | Client-monotonic; the server discards packets that are not newer (dup/reorder guard). |
| 12 | 2 | `flightId` | `uint16_t` | Formation to order, or `kOwnFlight` (`0xFFFF`) = "the one I command". A commander of *several* formations (an AWACS, a package commander) must name one — `kOwnFlight` is refused as ambiguous. |
| 14 | 1 | `flags` | `uint8_t` | Bit 0 = `kFlightFlagCascade`: apply to every sub-formation beneath the addressed one. |
| 15 | 1 | `reserved` | `uint8_t` | |

Rate-limited per peer (`[flight] command_rate_limit_per_s`, default 4/s). The limit is acked **once
per window**, never once per packet — an ack for every rejected packet would turn a flood into an
amplifier pointed back at the sender.

### MsgWingmanAck — 16 bytes

Reliable, **server→client**. Three things arrive on this ID:

1. **An order outcome**, to the commander.
2. **The flight check-in** (`CheckIn`), unsolicited, once after `MsgConnectAck` when the peer's
   flight is formed. This is how a client learns it *has* a flight and what id to address it by.
   (`MsgConnectAck` cannot carry it: that packet is immediately followed by `MsgEntityTypeDef`
   records, so appending a field there would shift them — breaking, not additive.)
3. **A relayed radio call** (`Relayed`), to a **human** member of someone's flight. The server cannot
   retask a person, so an order aimed at a player is delivered as a call they may choose to obey.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x0E` |
| 1 | 1 | `command` | `uint8_t` | Echoed `WingmanCommand` ordinal (`rejoin` for a check-in). |
| 2 | 1 | `result` | `uint8_t` | `WingmanResult` (below). |
| 3 | 1 | `flightSize` | `uint8_t` | Live members in the addressed formation. |
| 4 | 4 | `memberIdx` | `uint32_t` | Member the order applied to, or `kFlightAll`. On a **relayed** call this is the *caller's* entity index — i.e. who is ordering you. |
| 8 | 4 | `targetIdx` | `uint32_t` | Designated target for `attack_my_target`, else `kNoTarget` (`0xFFFFFFFF`). |
| 12 | 2 | `flightId` | `uint16_t` | The formation this ack refers to. |
| 14 | 2 | `reserved` | `uint16_t` | |

**`WingmanResult`** — a machine-readable code, **not** server-authored text. Brevity calls ("Two,
engaging.") are UI strings that must be localizable; the client already needs the same table to label
its radio menu; and it keeps a server-controlled string off the HUD text path entirely. This is the
same reasoning as `ConnectRefusalCode`.

| Value | Name | Meaning |
|-------|------|---------|
| 0 | `Acknowledged` | An AI member's behavior changed. |
| 1 | `NoFlight` | You command no formation with live members, **or** you named one that is not yours. Deliberately the same code for both. |
| 2 | `NoTarget` | `attack_my_target`: nothing hostile in the commander's boresight cone. **Behavior is unchanged** — the wingman does not pick its own target. |
| 3 | `Unavailable` | The addressed member is dead. |
| 4 | `Rejected` | Unknown command ordinal. |
| 5 | `RateLimited` | Too many orders this window (acked once per window). |
| 6 | `CheckIn` | Unsolicited: your flight is formed; this is its size and id. |
| 7 | `Relayed` | Sent **to a human member**: your commander has ordered `command`. Advisory — compliance is the player's choice. Also returned to the commander when every addressed member was human. |
| 8 | `NotLead` | Reserved: you are in a formation but do not command it. |

Both IDs are additive — old peers discard them, and `kProtocolVersion` stays **1**.

### MsgCombatEvent — 4 + n×32 bytes

Reliable, **server→client** (#626). The gameplay-event stream: a 4-byte header followed by
`count` 32-byte records. Kill records are **broadcast**; a `Stats` record is **unicast** and
always describes the *receiving* peer's own tallies. Cosmetic effects (tracers, impacts) do not
ride here — they are unreliable snapshot TLVs, because a lost muzzle flash is nothing and a lost
kill credit is a bug.

Header:

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x0F` |
| 1 | 1 | `count` | `uint8_t` | Records following the header |
| 2 | 2 | `reserved` | `uint16_t` | |

`CombatEventRecord` (each):

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `type` | `uint8_t` | `0` = Kill, `1` = Stats; unknown types must be skipped, not rejected |
| 1 | 1 | `weaponClass` | `uint8_t` | `WeaponType` ordinal of the credited weapon; `0xFF` = none/unknown |
| 2 | 2 | `reserved` | `uint16_t` | |
| 4 | 4 | `subjectIdx` | `uint32_t` | Kill: the destroyed entity |
| 8 | 2 | `subjectGen` | `uint16_t` | |
| 10 | 2 | `pad0` | `uint16_t` | |
| 12 | 4 | `instigatorIdx` | `uint32_t` | Kill: the credited entity; `0xFFFFFFFF` = environment |
| 16 | 2 | `instigatorGen` | `uint16_t` | |
| 18 | 2 | `pad1` | `uint16_t` | |
| 20 | 4 | `a` | `uint32_t` | Kill: instigator's owning **peer id** (`kNoOwningPeer` = AI/server — peer id 0 is a real player). Stats: kills |
| 24 | 4 | `b` | `uint32_t` | Kill: subject's owning peer id. Stats: losses |
| 28 | 4 | `c` | `int32_t` | Stats: score |

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

### Voice comms — `MsgVoiceNetDef` / `MsgVoiceFrame` / `MsgVoiceRelay` (Epic J, #532)

Three messages, and **the server understands exactly one thing about the audio: how many bytes it
is.** Frames are relayed *opaque* to a recipient set derived from the net's kind — no decode, no
mix, no transcode. That is what makes voice for 128 players cost the server almost nothing, and it
keeps the codec (48 kHz mono Opus, 20 ms frames — see `engine/voice/VoiceCodec.h`) a
**client-to-client contract** that can change without a protocol change.

Routing is by **radio net**: a named channel with a membership rule, defined in server config
(`[[voice.nets]]`) and replicated to clients once after `MsgConnectAck`. There is deliberately **no
frequency dial** — tuning 251.000 to hear the tanker is ceremony rather than gameplay, and a new
player cannot discover it. A net's **index** in the server's table is its wire `netId`, so net
strings never travel per frame. Kinds: `global`, `team` (same faction), `flight` (the speaker's
formation, #610's command tree), `proximity` (within `rangeM`, side-agnostic), `atc`.

The sender is **always excluded** from the recipient set: a network round trip of your own voice is
the most disorienting thing a voice system can do. Sidetone belongs on the client.

#### MsgVoiceNetDef — 4 + n×68 bytes

Server→client, **reliable**, sent once after `MsgConnectAck` (beside `MsgFactionDef`). A client
cannot key a mic until it arrives.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x23` |
| 1 | 1 | `netCount` | `uint8_t` | Records following at offset 4 |
| 2 | 1 | `flags` | `uint8_t` | Bit 0 = `kVoiceServerEnabled`; clear + `netCount == 0` = voice off |
| 3 | 1 | `reserved` | `uint8_t` | |

Each `MsgVoiceNetRecord` (68 bytes, align 4):

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `netId` | `uint8_t` | Index in the table; the wire net id on every frame |
| 1 | 1 | `kind` | `uint8_t` | `RadioNetKind` ordinal — gate with `isRadioNetKindOrdinal` before casting |
| 2 | 1 | `flags` | `uint8_t` | `kVoiceNetFlagPositional` / `…RadioEffect` / `…Default` |
| 3 | 1 | `reserved` | `uint8_t` | |
| 4 | 4 | `rangeM` | `float` | Proximity radius / positional rolloff ceiling; 0 = unlimited |
| 8 | 4 | `gain` | `float` | Authored per-net trim |
| 12 | 24 | `id[24]` | `char[24]` | Stable machine id (23 usable) |
| 36 | 32 | `name[32]` | `char[32]` | Display label (31 usable) |

#### MsgVoiceFrame — 8 + payload bytes

Client→server, **unreliable, channel 2**. The opaque Opus payload follows the header at offset 8.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x24` |
| 1 | 1 | `netId` | `uint8_t` | Index into the table received in `MsgVoiceNetDef` |
| 2 | 2 | `seq` | `uint16_t` | Per-(speaker, net) counter; wraps, compared half-window |
| 4 | 2 | `payloadBytes` | `uint16_t` | ≤ `kMaxVoiceFrameBytes` (400) |
| 6 | 1 | `flags` | `uint8_t` | `kVoiceFlagStart` / `kVoiceFlagEnd` |
| 7 | 1 | `reserved` | `uint8_t` | |

An **empty payload with `kVoiceFlagEnd`** is a pure end-of-transmission marker: it is what the
receiver turns into a squelch tail, and deriving that boundary from a receive timeout instead would
put the squelch a timeout late.

The server validates the length, the sender's net membership, and a per-peer **frames/second** cap
(`[voice] frame_rate_limit`, default 60). That cap is a *bandwidth* bound, not anti-spam: a frame is
fanned out to every recipient on the net, so an unbounded sender costs (recipients × bytes). An
over-rate frame is dropped **silently** — replying to a flood amplifies it.

#### MsgVoiceRelay — 16 + payload bytes

Server→client, **unreliable, channel 2**. The same payload, plus who said it.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 1 | `msgId` | `uint8_t` | `0x25` |
| 1 | 1 | `netId` | `uint8_t` | |
| 2 | 2 | `seq` | `uint16_t` | Forwarded unchanged |
| 4 | 4 | `senderPeerId` | `uint32_t` | Participant id (matches `MsgPlayerRoster`) |
| 8 | 4 | `senderEntityIdx` | `uint32_t` | Entity pool index, or `kNoVoiceEntity` |
| 12 | 2 | `payloadBytes` | `uint16_t` | ≤ `kMaxVoiceFrameBytes` |
| 14 | 1 | `flags` | `uint8_t` | |
| 15 | 1 | `reserved` | `uint8_t` | |

`senderEntityIdx` carries the speaker's **entity index rather than a position**: the receiving
client already has entity transforms from its snapshot, and 24 bytes of double position on every
20 ms frame would cost more than the audio. A speaker the client cannot resolve (interest-culled, or
an observer with no aircraft) is mixed **head-locked**, which is the right fallback for a radio
anyway.

`MsgRadioTransmission` (#703) gained a `netId` at offset 1 for the same reason: synthetic traffic
(ATC, AWACS, Epic O TTS) rides the **same net** as human voice, so the presentation layer (#925)
applies the same DSP, ducking and gain to both. A human and a synthetic transmission must be
indistinguishable in presentation.

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
- **`MsgWorldSnapshot`** (origin table + record stream): offset `sizeof(MsgWorldSnapshotHeader) + originCount×24 + bitstreamBytes`

**Backward compatibility**: old receivers that call `readMsg<T>()` or consume exactly `bitstreamBytes`
of the record stream naturally stop at the right byte count and ignore trailing extension bytes — no code
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
| `SnapshotDespawn` | `0x0103` | `uint32_t[]` | `MsgWorldSnapshot` | Indices of entities the receiving peer *knew* that were removed from the sim entirely (kills/despawns) — **not** entities that merely left the interest radius (those rely on the client retention timeout). Variable length = `4 × count`, little-endian; read **per element via `memcpy`** (the payload is unaligned). Omitted when empty. Repeated for `kDespawnRepeatTicks` (≈4) ticks for drop tolerance on the unreliable channel. The client (`ClientNetEventHandler`) applies despawns *before* upserting the same packet's records, so a kill-then-reuse-same-idx resolves to the new entity. Priority/budget scheduler (#516). |
| `SnapshotEffects` | `0x0104` | packed records | `MsgWorldSnapshot` | Cosmetic weapon effects (#625): tracers, muzzle flashes, launches, impacts, detonations. `kEffectRecordBytes` (22) per record, unaligned little-endian: `type u8` (`EffectType`: 0=WeaponFired, 1=MissileLaunch, 2=Impact, 3=Detonation, 4=NuclearFlash) + `weaponClass u8` (`WeaponType` ordinal) + `srcIdx u32` + `tgtIdx u32` (`0xFFFFFFFF` = none) + `pos f32[3]` (float32 world position — particle precision, not sim precision). Interest-filtered per peer, capped at `kMaxEffectsPerSnapshot` (16) per snapshot. **Unreliable by design**: a dropped packet loses cosmetics, never state — anything that must arrive (kills, stats) travels on the reliable `MsgCombatEvent`. Unknown `type` values must be skipped, never rejected. Omitted when empty. |
| `SnapshotLastAckedSeqNum` | `0x0105` | `uint32_t` | `MsgWorldSnapshot` | The `seqNum` of the last `MsgClientInput` the server **drained from the jitter buffer and applied** for the receiving peer (#427). Lets `ClientPrediction` replay *exactly* the inputs the server has not yet reflected (history `seqNum > this`), rather than approximating the replay window from `SnapshotPeerDelayTicks` — exact under high delay variance, where the tick count over- or under-replays. Absent until the server has applied one of the peer's inputs (its first snapshots); the client falls back to the delay-ticks approximation when absent. |
| `SnapshotArticulation` | `0x0107` | records | `MsgWorldSnapshot` | **Actuator positions** (#843) for the articulated entities in the receiving peer's interest set, so a remote aircraft's gear and flaps move. Payload: repeated `{ uint32 entityIdx (LE), uint16 channelMask (LE), uint8 value[popcount(mask)] }`. Bit *n* of the mask is `ArtChannel` *n* (`engine/render/ArtChannel.h`; the enum order is the wire order and is append-only). Unsigned channels encode `round(v × 255)`, signed ones `round(v × 127) + 128` (offset binary). **Quantization is cosmetic-grade on purpose:** 1/255 on a 0..1 actuator is 0.4% — visually exact, and it cannot affect flight because *a peer never reads its own entity's channels from the wire* (client prediction overwrites them with its own full-precision integration). Sent on **change**, plus a periodic refresh every `kArtRefreshTicks` (30) for drop tolerance and interest re-entry. An entity at all-default channels is **absent**, so a world of unarticulated aircraft emits no TLV and its snapshot is byte-identical to pre-#843. Absence means HOLD-LAST, not neutral. Little-endian, unaligned; read per-field via memcpy. |
| `SnapshotCrew` | `0x0106` | `uint8 count` + records | `MsgWorldSnapshot` | Live **crew turret pose** (#972) for the CREWED aircraft in the receiving peer's interest set. Payload: `uint8 entryCount`, then per entry `{ uint32 entityIdx (LE), uint8 turretCount, turretCount × { int16 azQ (LE), int16 elQ (LE) } }`. Azimuth is quantized over `[-π, π]` and elevation over `[-π/2, π/2]` to `int16` (mount frame). **Single-seat aircraft never appear** — occupancy lives in the reliable `MsgCrewRoster`, so a world of only single-seat entities emits no `SnapshotCrew` TLV and its snapshot is byte-identical to pre-#972. Unreliable/interest-filtered: a dropped packet loses one tick of turret aim. Little-endian, unaligned; read per-field via memcpy. |
| `SnapshotServerThrottle` | `0x0108` | `uint8[2]` | `MsgWorldSnapshot` | The **server** is intentionally decimating this peer's snapshots because the tick-overrun governor (#514) is shedding work — as distinct from the peer's own link being congested (#518), which the client already infers from RTT and loss. The two are indistinguishable from the client (snapshots arrive late) and mean **opposite** things, so the HUD must not blame a player's connection for a server problem. Payload: `uint8 loadPct` (`round(loadFactor × 100)`, clamped to 1–100 — 0 would read as *no load* on a server at its floor) + `uint8 intervalTicks` (the governor's snapshot spacing; ≥ 2 whenever the tag is present at all). **Omitted entirely** when the governor is not the binding lever for this peer, so the healthy path is byte-identical to pre-#576 — the `SnapshotCrew`/`SnapshotArticulation` rule. **Presence is the signal**; absence is both the healthy case and what an old server sends, so nothing is assumed from silence. The client latches it for ~3 s past the last tagged snapshot, because the channel is unreliable and one dropped packet must not flicker the indicator (#576). |

| `ConnectAckAuthority` | `0x0201` | `{u64 caps, u16 factionIndex}` (10 B) | `MsgConnectAck` | Granted authority (#949), appended after the entity-type records. Present only when the peer holds non-zero granted capabilities (`CapabilityMask`, `engine/net/Capability.h`); re-sent on a mid-session grant/revoke so the client can show/hide game-master, moderator, and faction-leader UI affordances. **Cosmetic/UX only — the server remains the enforcement point.** Old clients iterate the type records by `typeCount` and skip the unknown tag. Little-endian, unaligned. Parsed by `ClientNetEventHandler::grantedCaps()` / `grantedFactionIndex()`. |
| `WeatherWindProfile` | `0x0400` | `uint8 count` + `count × {f32 altM, f32 windX, f32 windZ}` (12 B each) | `MsgWeatherState` | Altitude wind profile (#489), appended after the 32-byte fixed struct. Knots ascending by altitude, absolute world-frame wind (m/s) at each. The client interpolates it by altitude (`WindProfile.h`) in parity with the server's per-entity wind. Omitted when no profile is set; old clients read the 32-byte struct and ignore the tail, keeping the datum-level `windX/windZ`. Little-endian, unaligned; read per-record via memcpy. |
| `ConnectSeatClaim` | `0x0500` | `{u32 entityIdx, u32 entityGen, u8 seatIndex}` (9 B) | `MsgConnectRequest` | Join-at-connect seat claim (#974): the client asks to occupy a non-fly seat of an existing crewed aircraft instead of spawning its own. The server binds the seat when it is joinable and falls back to a normal pilot spawn otherwise (so a pilot always gets in). Appended after the pack manifest. Little-endian, unaligned. |
| `ConnectIdentity` | `0x0501` | ASCII UUID (≤ 40 B) | `MsgConnectRequest` | The client's stable pilot identity (#524), taken from `PilotProfile::guid`. Lets the server restore a reconnecting player's team and score tallies inside the `[match] reconnect_grace_s` window, so a dropped connection is not a fresh join. Absent means no reconnect matching is attempted. Little-endian, unaligned. |
| `ConnectJoinPassword` | `0x0502` | raw UTF-8 bytes (1–64) | `MsgConnectRequest` | The join password (#998) when `[server] password` is set. Absent or wrong is answered with `MsgConnectRefusal`, so a passworded server never admits and then disconnects. Unaligned. |

**Reserved ranges:**
- `0x0000`: reserved
- `0x0100–0x01FF`: `MsgWorldSnapshot` extensions
- `0x0200–0x02FF`: `MsgConnectAck` extensions (`0x0201` = `ConnectAckAuthority`)
- `0x0300–0x03FF`: `MsgClientInput` extensions (`0x0400` used by `WeatherWindProfile`)
- `0x0400–0x04FF`: `MsgWeatherState` extensions (`0x0400` = `WeatherWindProfile`)
- `0x0500–0x05FF`: `MsgConnectRequest` extensions (`0x0500` = `ConnectSeatClaim`)
- All other values: reserved; must not be sent

**Future: requested-authority claim.** A client will later be able to *request* authority at connect
time by riding a signed, offline-verifiable entitlement token in the `0x0500–0x05FF`
`MsgConnectRequest` extension range (alongside RFC #871's token and Epic C's identity binding, #950) —
the same shape as the identity-bound role table. This is **not implemented**: today authority is
granted only server-side (operator console / RCON `grant` command, rung 2 of the #944 grant ladder),
and `ConnectAckAuthority` merely reports the result to the client. The requested-authority claim is
documented here so the reserved range is not reused.

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
  |<-- MsgWorldSnapshot (unreliable, unicast) |  3D interest filter + per-peer quantized bitstream
  |    header(40) + bit-packed records        |  (full vs delta per-record `full` bit)
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

As of #515 the entity body is a quantized bitstream (see
[snapshot-quantization.md](decisions/snapshot-quantization.md)), so the per-entity cost is **~24 bytes**
(steady-state delta) or **~31 bytes** (full own-entity record) plus a 1-byte origin index, plus the
24-byte header, the shared-origin table, and the TLV block — roughly a 2.5–3× reduction from the
previous fixed 64/88-byte records.

| Visible entities | Approx. packet size (delta) | Per-client outbound (60 Hz) |
|-----------------|-----------------------------|-----------------------------|
| 8 | ~238 bytes | ~14 KB/s |
| 20 | ~526 bytes | ~32 KB/s |
| 32 | ~814 bytes | ~49 KB/s |
| 64 | ~1,582 bytes | ~95 KB/s |
| 128 | ~3,118 bytes | ~187 KB/s |

(Indicative; actual bytes depend on the tuned bit budget and idx-delta varint sizes. The authoritative
before/after numbers come from the bot_swarm `downstream_kbs_per_client` metric — see
[load-testing.md](load-testing.md).)

### MTU fragmentation

ENet fragments unreliable datagrams that exceed the path MTU (~1,400 bytes on standard
Ethernet). Fragmented unreliable packets are reconstructed only if **all fragments arrive**.
A single dropped fragment discards the whole snapshot — dead-reckoning absorbs occasional
losses, but high fragment counts multiply the effective loss rate. The quantized encoding pushes the
single-fragment safe threshold from ~20 entities up to **~55 visible entities per client**
(40 + 55×24 ≈ 1,360 bytes < typical 1,400-byte MTU); beyond that, the priority/budget scheduler
(#516) bounds the per-client byte cost.

### Interest management and delta compression

`WorldBroadcaster::onTick()` sends a **per-peer unicast** `MsgWorldSnapshot` containing only
entities within `draw_distance_km` of the peer's own entity position (via
`SpatialIndex::queryRadius()`, then an exact **3D (XYZ) distance gate** added in #402).
`MsgWeatherState` and `MsgServerNotice` remain global broadcasts.

**Delta compression**: each record carries a `full` bit. A `full` record (typeIndex + generation)
is sent the first time a peer sees an entity, on generation change, until the client **acknowledges**
the full (client-acked delta baselines, #517 — see *Scaling to 128+*), and when the entity has not
been sent to that peer within `kSnapshotRetentionTicks` (the budget-deferral re-entry case, #516);
every other tick the entity is a delta record that omits typeIndex (and the generation when
unchanged).

**Priority/budget cap (#516)**: with `[world] snapshot_budget_bytes` set (default 1200, 0 = unlimited),
the per-peer record set is no longer "everything within `draw_distance_km`" — it is the
highest-relevance subset that fits the budget (see *Scaling to 128+* above), with the rest deferred to
later ticks. The client retains deferred entities and is told of true removals via the `SnapshotDespawn`
TLV.

Configure via `[world] draw_distance_km` and `[world] snapshot_budget_bytes` in `server.toml`; both
hot-reloadable via `reload_config`. (Delta-baseline recovery is automatic and client-acked — see
*Scaling to 128+* below — so there is no baseline-interval knob.)

### Position precision

Record positions are quantized to a fixed-point offset from a **shared grid-cell `double` origin**
(`floor(pos / kOriginGridM) * kOriginGridM`, ~65 km cells; #725) at 0.125 m resolution over a ±262 km
range — planet-scale accurate without storing a `double` per entity. The distinct origins a snapshot
references are carried (deduped) in its origin table. Sharing the origin across peers makes each record
peer-independent, so the sim encodes each entity once per tick and stitches the blob into every peer's
stream. The double-precision engine state is preserved on the server; only the wire representation is
quantized.

- **(historical)** The former fixed format stored `pos[3]` as 24 bytes (double) in full entries
  for planet-scale precision; float32 degrades to ~24 cm accuracy at 2,000 km from origin.
- **`typeIndex` as uint32_t**: 4 bytes; a uint16_t would support 65,535 entity types and
  save 2 bytes/entity. Not changed here because `EntityTypeRegistry` and `EntityState`
  both use uint32_t throughout the engine — narrowing the wire type is deferred.

### Scaling to 128+ (Epics B & L)

The original fixed 64/88-byte records and radius-only unicast were sized for ~32 players. The
128+ re-target (decision record 2026-06-28) replaces them. The per-client bandwidth figures in
this section are measured empirically by the `bot_swarm` load generator — see
[docs/developer/load-testing.md](load-testing.md). The protocol can change freely until the
`kProtocolVersion` freeze.

- **Quantized / bit-packed state (Epic B, #515 — landed).** The entity body is a quantized bitstream
  (position relative to a per-snapshot frame origin, **smallest-three** quaternion, quantized
  velocity/omega; full vs delta per-record `full` bit). See
  [snapshot-quantization.md](decisions/snapshot-quantization.md). ~2.5–3× reduction over the former 64-byte
  record, raising the single-fragment safe threshold from ~20 to ~55 entities.
- **3D interest management (#402, Epic B — landed).** `WorldBroadcaster` applies an exact full-XYZ
  squared-distance gate over the conservative XZ cells returned by `SpatialIndex::queryRadius()`.
- **Priority/budget snapshot scheduler (Epic B, #516 — landed).** Instead of "everything within
  `draw_distance_km`," each client gets a **per-tick byte budget** (`[world] snapshot_budget_bytes`,
  default 1200, 0 = unlimited; hot-reloadable). `engine/net/SnapshotScheduler` ranks the visible
  entities by relevance (distance, closing-speed, recency, player-owned) and sends only the
  highest-priority set that fits; a recency term guarantees eventual inclusion of every visible
  entity, and the peer's own entity is always sent. Keeps per-client bandwidth bounded as population
  grows. Because a budgeted snapshot omits low-priority entities, the **client retains entity state
  across snapshots** (`ClientNetEventHandler`), evicting an entry only on an explicit `SnapshotDespawn`
  TLV (confirmed kill/removal) or after `kSnapshotRetentionTicks` (~3 s) with no update (the
  interest-out / lost-despawn backstop). The server force-sends a *full* record when it has not sent a
  known entity within that window, so a returning entity is decodable after the client evicted it.
- **Client-acked delta baselines (Epic B, #517 — landed).** Full-vs-delta is driven by what each
  client has acknowledged, not a fixed timer. The client already echoes the last snapshot tick it
  processed in `MsgClientInput`/`MsgHeartbeat` (`tickIndex`); the server treats that as the snapshot
  **ack** (clamped to the present, monotonic). An entity is re-sent as a *full* record every tick
  until the peer confirms it decoded the tick its full streak started on, then it converges to
  *deltas*. This removes the globally-synchronized periodic full-resync spike (the former
  `baseline_interval_ticks` fired on the same tick for every peer) and recovers a dropped full in
  ~1 RTT instead of up to 2 s. The client ignores out-of-order/duplicate snapshots so its echoed tick
  stays a monotonic high-water mark. The per-entity `kSnapshotRetentionTicks` force-full (the
  interest-out / client-evicted re-entry case) is retained as an ack-independent backstop.
- **Selective-ack identity precision (Epic B, #566 — landed).** A single high-water-mark ack cannot
  distinguish "the client decoded the full sent at tick S" from "the client received a *later* tick
  ≥ S but missed S", so a full dropped on a couple of consecutive ticks could be briefly mis-confirmed
  and the entity turns invisible until the retention backstop heals it. The client→server ack is
  therefore paired with a 32-bit **selective-ack bitmask** (`MsgClientInput`/`MsgHeartbeat` `ackMask`,
  TCP-SACK style) reporting which of the ticks just below the high-water mark it actually **decoded**
  (`engine/net/AckWindow.h`). The server confirms delivery of the *specific* `fullStreakTick` rather
  than a high-water mark, closing the residual at the root and retiring the #517 "deferral guard"
  workaround. No wire-size or protocol-version change — `ackMask` reuses the messages' former reserved
  padding. **Limitation:** a snapshot the client receives but does not fully decode (mid-bitstream
  truncation) still sets its ack bit; the per-client byte budget keeps snapshots within a single MTU
  fragment so truncation is rare, and the retention force-full remains the backstop for it.
- **Adaptive send-rate / congestion response (Epic B, #518).** Each connected peer owns an AIMD
  congestion controller (`engine/net/CongestionController`) that the broadcaster steps every tick from
  the peer's ENet link quality (`INetwork::getPeerLinkStats` → packet loss, RTT, reliable bytes in
  flight). A peer is judged congested when loss exceeds a threshold (default 2 %), RTT rises a margin
  above its running baseline (default 40 ms), or the reliable backlog grows large. On congestion the
  controller multiplicatively lowers a per-peer `throttle ∈ [floor, 1]`; when healthy it additively
  ramps it back toward 1. The throttle drives **two levers**: it stretches the snapshot **send
  interval** (60 Hz down to a configurable floor, default 10 Hz — the broadcaster simply skips a
  peer's per-tick snapshot until enough ticks elapse) and **scales the per-client byte budget** (the
  #516 scheduler then defers more low-relevance entities). There is **no wire-format change** — the
  client already tolerates a variable snapshot rate (render alpha is wall-clock; prediction uses
  `estimatedDelayTicks`). A healthy peer, the mock/loopback case (zero link stats), or a disabled
  controller all hold `throttle == 1`, i.e. the full 60 Hz / full-budget behaviour.

  **Signal note (anti-feedback):** the delay term uses **ENet RTT**, *not* the application-level
  snapshot one-way delay (`estimatedDelayTicks`). That metric inflates when we decimate — a staler
  "last received tick" looks like more delay — which would drive *more* decimation: a self-reinforcing
  collapse. ENet keeps RTT fresh via periodic reliable pings, independent of our snapshot cadence. See
  [docs/developer/decisions/congestion-control-design.md](decisions/congestion-control-design.md). Config: `[world]
  congestion_enabled` / `congestion_min_send_hz` / `congestion_loss_threshold` /
  `congestion_budget_floor_bytes` (all hot-reloadable via `reload_config`); per-peer send rate and loss
  are visible in the `peers` admin command.
- **Transport replacement (Epic L).** **GameNetworkingSockets** is selected (#506) as the 128+
  backend behind the `INetwork` HAL for higher peer counts, built-in congestion control, and
  encryption; `enet6` is retained as the LAN/low-count backend (see
  [transport-selection.md](decisions/transport-selection.md)). The MTU/fragmentation discussion above is
  transport-specific and will be revised when the GNS backend lands (#507). `MsgLanBeacon` (raw
  UDP) and RCON (TCP) are unaffected.

### Authenticated connect handshake (planned — Epic C)

Server-side identity adds a **signed-token** field to the connect flow: the client presents an
offline-verifiable access token (e.g. Ed25519/JWT) issued by a pluggable identity provider;
`fl-server` verifies the signature locally against the issuer's published public key (no live
callback per connect). The verified account ID — not the client-generated `PilotProfile::guid`
— keys persistent stats, ranking, and bans. Guest connections remain possible when the server
permits them. Exact message layout is specified when Epic C lands. (The Epic J voice channel that
was reserved here has landed — see **Voice comms** above.)

## Notes

- **World coordinate system**: right-handed, Y-up, metres (matches glTF). Entity body `+X`
  axis is forward.
- **Position precision**: `double` throughout the engine. On the wire, the snapshot's shared grid
  origins are `double` (sub-millimetre) and per-entity positions are quantized to a fixed-point offset
  from them at 0.125 m resolution (±262 km range) — see [snapshot-quantization.md](decisions/snapshot-quantization.md).
  `MsgClientInput`/`MsgConnectAck` and other non-snapshot positions remain full `double`/`float`.
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
   the local integrator is stepped one tick — with steady wind AND the deterministic weather
   turbulence reproduced from the broadcast `turbulenceAmp` (#426), plus the stall buffet (#816).
   The turbulence is a pure `(entityIdx, tickIndex, amp)` function shared with the server, so it
   matches exactly rather than diverging.

2. **On each received `MsgWorldSnapshot`**: the snapshot callback (`ClientNetEventHandler::
   snapshotCallback`) is invoked before `publishExternal()`. The integrator is reset to the
   server's authoritative `FlightState` (reconstructed from the player's `EntityRenderEntry`
   including the new `omega` field), then the un-reflected history inputs are replayed forward.
   When the `SnapshotLastAckedSeqNum` TLV (0x0105) is present, the client replays *exactly* the
   history inputs whose `seqNum` is newer than the acked value — the precise window, robust to
   delay variance (#427). Absent (a peer's first snapshots, or an older server), it falls back to
   replaying the last `estimatedDelayTicks` inputs, whose raw tick count arrives in the
   `SnapshotPeerDelayTicks` TLV (0x0102). `SnapshotPeerLatency` (0x0101, ms) continues to serve
   the HUD indicator.

3. **The player's `EntityRenderEntry` is mutated in-place** with the predicted position,
   velocity, orientation, and angular rates before the snapshot is published to
   `SimRenderBridge`. All other entities remain server-authoritative.

4. **Reconciliation**: if the new predicted position diverges from the previous prediction by
   more than `snap_threshold_m` (default 5 m), the correction is applied immediately (hard
   snap). Otherwise it is blended at `blend_rate` per reconciliation for visual smoothness.
   Both parameters are configurable via `[prediction]` in `user.toml`.

**Known limitation**: server-side turbulence is not replicated client-side (requires a
future seed-broadcast mechanism). The resulting small positional divergence is corrected each
reconciliation.

## Server-Side Lag Compensation (Hit-Detection Rewind)

A player aiming a gun aims at where targets were `estimatedDelayTicks` ago — the world their
last snapshot showed. Without compensation every shot must *lead* the target by the shooter's
own latency, which punishes exactly the players a 128-player internet server has most of.

The server keeps a rolling **`TransformHistory`** ring (`engine/net/TransformHistory.h`): the
post-integrate position of every live entity for the last 32 ticks (≈533 ms at 60 Hz). When a
**player's** hitscan gun fires at tick `T`, targets are ray-tested at their positions from tick
`T − clamp(estimatedDelayTicks, 0, 31)`; damage is applied to the entity as it is *now*. Rules:

- **Players only.** AI shooters have no latency and rewind 0 ticks. Missiles, rockets, and
  bombs fly in real time and never rewind — a projectile is a physical object in the current
  world, not an instantaneous ray. Both are deliberate.
- **Keyed off the shooting SEAT's occupant (#979).** A turret gunner's gun rewinds by the
  *gunner's* latency, not the pilot's — the rewind reads `occupantPeerFor(airframe, seat)`, which
  resolves to the gunner for a crew seat and the pilot for the Fly seat; the airframe-owner
  fallback keeps the single-seat case unchanged. The turret slew stays server-authoritative, so a
  gunner cannot claim a bore its physical turret could not have reached within its slew/arc limits.
- **Generation-checked.** Each history entry stores the entity generation; a recycled pool slot
  can never be hit through history. An entity that did not exist at the rewound tick is tested
  at its current position instead (the shooter could not have seen it, but it is physically in
  the bullet's path).
- **Broadphase inflation.** The spatial index holds current positions, so the broadphase radius
  is inflated by the maximum possible drift since the rewound tick (bounded by the snapshot
  codec's ±2000 m/s velocity cap); the exact ray test then uses each candidate's rewound
  position.
- **Bounded unfairness.** The 32-tick clamp bounds the classic "shot from around the corner"
  effect to ≈533 ms: a victim can be hit at most that long after they, in their own view,
  broke line of sight. High-latency shooters past the clamp are back to leading their targets.
