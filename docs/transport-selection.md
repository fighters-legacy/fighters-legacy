# Network Transport Selection

Decision record for **[#506]** — the transport-selection spike of **Epic L** (network transport
replacement, [#493]) in the 128+ multiplayer re-target. It evaluates candidate transports against
the `INetwork` HAL contract and **selects one** so the implementation issues proceed against a
settled decision: the backend ([#507]), transport encryption/DTLS ([#508]), and the build/CI
integration ([#509]).

This is a **writeup spike** — no backend is implemented and no dependency is pulled here. It
mirrors how the sibling spikes resolved: [#505] (enet6 ceiling characterisation) and [#510]
(server job-system design).

> **Implementation outcome ([#507]/[#508]/[#509], landed).** GameNetworkingSockets was implemented
> behind `INetwork` as selected. Two build-level choices below were **reversed** during
> implementation, for concrete reasons — see [gns-backend.md](gns-backend.md):
> 1. **Crypto backend = OpenSSL, not libsodium.** GNS makes `USE_CRYPTO=libsodium` a `FATAL_ERROR`
>    on non-x86 CPUs (its libsodium AES path is x86-only); the macOS CI runner is Apple-Silicon
>    arm64. OpenSSL (Apache-2.0) is GPL-3-compatible and builds on every target.
> 2. **protobuf = system-preferred + FetchContent fallback**, not pure FetchContent. CMake refuses
>    to `include()` protobuf's `export()`-generated build-tree targets file within the same build, so
>    the pure-FetchContent handoff is blocked; system protobuf (GNS's own recommendation) is clean
>    and matches how this repo already sources SDL3 / OpenAL / Catch2. No abseil, since GNS is built
>    with ICE/WebRTC off.
>
> Unchanged as selected: GNS behind `INetwork` with **no interface change** to the transport surface
> (three optional server-tuning virtuals were added), `enet6` retained via `createNetwork(kind)`, the
> wire format (`kProtocolVersion = 1`), and the game-client HAL-leak closure.

[#493]: https://github.com/fighters-legacy/fighters-legacy/issues/493
[#505]: https://github.com/fighters-legacy/fighters-legacy/issues/505
[#506]: https://github.com/fighters-legacy/fighters-legacy/issues/506
[#507]: https://github.com/fighters-legacy/fighters-legacy/issues/507
[#508]: https://github.com/fighters-legacy/fighters-legacy/issues/508
[#509]: https://github.com/fighters-legacy/fighters-legacy/issues/509
[#510]: https://github.com/fighters-legacy/fighters-legacy/issues/510
[#518]: https://github.com/fighters-legacy/fighters-legacy/issues/518
[#402]: https://github.com/fighters-legacy/fighters-legacy/issues/402

## Problem & prior data

`enet6` (v6.1.3, MIT, `SirLynix/enet6`) was a locked decision sized for ~32 peers. It ships **no
transport-level encryption** and only basic congestion handling. Epic L reverses that decision for
the 128+ target.

Crucially, the completed sibling spike [#505] (8-core / 16 GB, Release, `bot_swarm`) proved the
transport is **not** the player-count ceiling in the 96–256 range: tick-Hz collapsed while the load
harness stayed idle, so the wall is **CPU-bound on the single-threaded sim + per-peer snapshot
build**, not on `enet6`. Epic L was therefore **decoupled from foundational** and re-cast as a
*later* optimisation. So this selection is **not** justified on throughput/tick-Hz — Epics A
(parallel sim) and B (quantization / interest / priority-budget / congestion, all landed on
`enet6`) own that. It is justified on the three things `enet6` cannot give the 128+ target:

1. **Transport encryption** — needed to carry Epic C identity/auth tokens over the open internet.
2. **Mature congestion control** — a battle-tested AIMD/pacing layer beneath the application-level
   per-client response already built in [#518].
3. **Connection-count headroom** — a transport proven well past 128 concurrent connections.

Any replacement must deliver those **without regressing A/B** and **without forcing a heavy new
dependency onto every build configuration** (single-player, LAN, tools, CI).

## Requirements — the `INetwork` contract

The transport abstraction already exists: **`platform/INetwork.h`**. A backend must satisfy it
without interface changes wherever possible:

| Capability | `INetwork` surface |
|---|---|
| Listen / connect | `bind(addr, port, maxClients)` · `connect(host, port)` · IPv4 + IPv6 + `::` dual-stack |
| Reliable **and** unreliable ordered delivery | `send(peerId, …, reliable)` · `broadcast(…, reliable)` — ≥2 logical channels |
| Per-frame pump | `service(timeoutMs)` drives I/O + dispatches `INetworkEventHandler` callbacks |
| Peer model | `uint32_t peerId` · `getPeerState` · `getPeerAddress` · graceful `disconnectPeer` + host `disconnect` |
| Link quality for congestion | `getPeerLinkStats` → `PeerLinkStats{rttMs, rttVarianceMs, packetLoss, reliableBytesInFlight}` (feeds the [#518] `CongestionController`) · `getPeerRtt` |
| Threading | single-thread service; the server host is **sim-thread-owned**, the client host **main-thread-owned** — never crossed |
| Portability | Win (MSVC 2026) / Linux (GCC/Clang) / macOS (Apple Clang); FetchContent build with the static-link + `ldd` CI guard |
| License | GPL-3-compatible |

## Candidate set

Three primary candidates are scored below. To avoid a false GNS-vs-bespoke dichotomy, the
off-the-shelf UDP stacks were also considered and briefly rejected:

- **netcode.io + reliable.io** (Glenn Fiedler) — netcode.io gives an encrypted, token-authenticated
  connection layer and reliable.io adds an ACK/reliability layer, but they are *components*: you
  still assemble and maintain the stack (channels, fragmentation policy, congestion) yourself. That
  is bespoke-with-a-head-start, and there is **no built-in congestion control** — one of the three
  reasons we are replacing `enet6` at all.
- **KCP** — an ARQ reliability layer only; no crypto, no congestion parity, no connection
  management. Would need the same surrounding stack as bespoke.
- **yojimbo** — a client/server framework built *on* netcode.io + reliable.io; convenient, but
  niche, less actively maintained, and inherits the "no congestion control" gap.
- **`enet6` + a DTLS/libsodium shim** — this is exactly the third scored column ("harden / defer").

The primary candidates are therefore **GameNetworkingSockets**, **bespoke UDP + libsodium**, and
**harden `enet6` (defer)**.

## Weighted evaluation matrix

Scores are 1 (poor) – 5 (excellent). Weight reflects how much the criterion matters to *this*
selection (encryption / congestion / headroom / build-cost dominate; raw throughput does not,
per [#505]).

| Criterion | Wt | GameNetworkingSockets | Bespoke UDP + libsodium | Harden `enet6` (defer) |
|---|--:|:--:|:--:|:--:|
| License / GPL-3 compat | 3 | 5 · BSD-3 | 5 · GPL-3 + ISC | 5 · MIT |
| Reliable + unreliable delivery | 4 | 5 · message lanes | 2 · must build | 5 · 2 channels today |
| Congestion control | 4 | 5 · Valve-production | 1 · must build | 2 · basic |
| Encryption / DTLS (Epic C auth) | 5 | 5 · built-in (25519 + AES-GCM) | 3 · libsodium, must wire | 1 · none (shim TBD) |
| Connection headroom @128+ | 4 | 5 · Steam/Dota/CS-proven | 2 · unproven | 2 · unproven >~128 |
| Cross-platform **build cost** | 4 | 2 · **heavy (protobuf)** | 4 · libsodium only | 5 · already in tree |
| FetchContent + static-link + `ldd` fit | 3 | 3 · protobuf complicates | 4 | 5 · proven |
| `INetwork` fit (no HAL change) | 3 | 5 | 4 | 5 |
| Maintenance / maturity | 3 | 5 · Valve, active | 2 · we own all bugs | 4 · upstream + us |
| Dependency weight | 2 | 2 · protobuf + crypto | 4 · one lib | 5 · none |
| **Weighted total** | | **~161** | **~103** | **~124** |

(Weighted total = Σ weight × score; indicative, not to two significant figures — the *shape* is the
point.)

### Reading the scores

- **Bespoke UDP is decisively last.** Its only wins are license and dependency weight; it loses
  every capability criterion because those capabilities do not exist yet and would take
  **man-months** to build to production quality — a reinvention of a solved, BSD-3-licensed problem.
  Rejected.
- **Harden-`enet6`/defer beats bespoke** purely on "it already works and pulls nothing," but it
  scores 1–2 on the three criteria that *motivate the epic* (encryption, congestion, headroom). It
  is a genuine fallback, not a solution.
- **GameNetworkingSockets wins clearly**, and wins specifically on the high-weight criteria we are
  replacing `enet6` *for*. Its one real weakness is **build cost** — it requires Google Protobuf
  (2.6.1+) plus a crypto backend (OpenSSL / libsodium / bcrypt). (WebRTC is a GNS dependency **only**
  for P2P NAT piercing, which a self-host dedicated-server model does not use.)

## Recommendation

**Select GameNetworkingSockets (BSD-3) as the strategic transport, behind the existing `INetwork`
HAL. Retain `enet6` as a selectable backend for LAN / single-player / low-count servers. Crypto
backend = libsodium (ISC).**

Rationale:

1. GNS is the only candidate that delivers all three motivating capabilities — encryption, mature
   congestion control, connection headroom — as a maintained, production-proven, GPL-compatible
   library, with **no `INetwork` interface change** required.
2. **Retaining `enet6` behind the factory is the key architectural move**: it keeps the heavy
   protobuf/crypto dependency chain **out of** the single-player, LAN, and CI-tool build paths, and
   preserves a lightweight fallback. The two backends coexist behind `createNetwork(kind)`; the
   dedicated 128+ server defaults to GNS, everything else can stay on `enet6`.
3. **libsodium over OpenSSL** for the crypto backend — lighter, more uniform cross-platform build
   (matches the Epic L direction; ISC license).

**The honest fallback is documented, not hidden:** if the protobuf FetchContent integration proves
untenable across MSVC / Apple Clang in [#509] (the single largest execution risk), the fallback is
**defer-and-harden** — keep `enet6` and add only a DTLS/libsodium encryption shim, revisiting GNS
when connection/congestion headroom is *proven* insufficient by the Epic I scale gate. That path
buys encryption but not congestion/headroom, and is strictly the second choice.

## `INetwork` → GameNetworkingSockets mapping (de-risks #507)

The GNS standalone (non-Steam) open-source library exposes `ISteamNetworkingSockets` (and a flat C
API). The mapping is direct:

| `INetwork` | GameNetworkingSockets |
|---|---|
| `init()` / `shutdown()` | `GameNetworkingSockets_Init(nullptr, errMsg)` / `_Kill()` |
| `bind(addr, port, max)` | `CreateListenSocketIP` on the bound address; a **poll group** for the accepted connections |
| `connect(host, port)` | `ConnectByIPAddress` |
| `peerId` | `HSteamNetConnection` (mapped to/from the `uint32_t` peerId) |
| `send(peerId, …, reliable)` | `SendMessageToConnection` with `k_nSteamNetworkingSend_Reliable` / `_Unreliable` |
| channels 0/1 (reliable/unreliable) | the two send flags (GNS lanes replace ENet channels) |
| `service(timeoutMs)` | `RunCallbacks()` + `ReceiveMessagesOnPollGroup` / `…OnConnection`, draining into `INetworkEventHandler` |
| `getPeerLinkStats` | `GetConnectionRealTimeStatus` → ping (`m_nPing`), quality, `m_cbPendingReliable`/`m_cbSentUnackedReliable` |
| `disconnectPeer` / `disconnect` | `CloseConnection` (graceful, linger to flush reliable) |
| connect/disconnect events | `SteamNetConnectionStatusChangedCallback` → `onConnect` / `onDisconnect` |

Gaps to design in [#507], not here: server **poll-group** management; confirming GNS's manual
`RunCallbacks()` pump respects the **sim-thread-owned host** rule (no internal service thread when
driven manually — it must be); and mapping GNS connection-status transitions cleanly onto the
existing two-callback (`onConnect`/`onDisconnect`) model.

## Does `INetwork` itself need to grow?

**Expected answer: no new methods.** Keep `INetwork` transport-only:

- **Encryption is backend-internal** (GNS handles the handshake; configuration, not interface).
- The **Epic C identity/auth token travels in-band as a wire message** (like the existing admin
  token), not through the HAL.
- `getPeerLinkStats` already exposes everything the [#518] congestion controller needs, and GNS's
  `GetConnectionRealTimeStatus` fills every field.

If [#507]/[#508] discover a genuine need (e.g. a `configureSecurity()` seam to hand the backend a
key/cert or an identity-provider hook), it will be added there as a **narrow, explicit** addition —
but the analysis does not anticipate one.

## Backend selection & the game-client HAL leak (de-risks #507)

Today the seam is `createENetNetwork()` (`platform/net/ENetNetworkFactory.h`), consumed by
`server/fl-server/main.cpp`, `tools/net_check/main.cpp`, and `tools/bot_swarm/BotClient.cpp`.
**But the game client bypasses it:** `game/fighters-legacy/Game.cpp:669` does
`std::make_unique<ENetNetwork>()` directly, leaking the concrete ENet type into `game/` and
violating the HAL boundary.

[#507] should introduce a backend-selecting factory — e.g. `createNetwork(TransportKind)` (or a
config-driven `createNetwork(NetworkConfig)`) returning `std::unique_ptr<INetwork>` — and **route
the game client through it**, closing the leak. `TransportKind::Enet` / `TransportKind::Gns`, with
the dedicated server defaulting to GNS and single-player/LAN able to stay on `enet6`.

## Encryption / DTLS + discovery + RCON reconciliation (de-risks #508)

- **Game channel:** GNS provides the transport encryption; pairs with Epic C auth tokens.
- **LAN discovery (`MsgLanBeacon`, `MsgId 0x10`):** stays a **raw UDP broadcast / IPv6 multicast
  outside the transport** and **plaintext** — it advertises presence only (name, port, player
  count, game-mode flags), no PII. It is not injected into a GNS connection. Unchanged.
- **RCON:** stays a **separate TCP** channel with its own `AuthTracker` per-IP lockout; an optional
  TLS wrapper is a later, independent hardening step. Unchanged by this selection.

## Build plan sketch (de-risks #509)

- FetchContent GameNetworkingSockets + its Protobuf dependency + libsodium; gate GNS behind the
  same pattern `enet6` uses (declared in `cmake/dependencies.cmake`, made available by
  `platform/net`).
- Set `COMPILE_WARNING_AS_ERROR OFF` on the third-party targets (they will not survive our
  `-Werror`), specify `LANGUAGES C CXX`, and keep the static-link + `ldd` CI regression guard.
- `find_package` version floors must match the FetchContent `GIT_TAG` exactly (all three deps) —
  the established convention.
- Add the apt package names + boolean inputs to `.github/actions/install-linux-deps/action.yml`;
  keep the build path buildable **without** GNS when only `enet6` is selected (so single-player /
  tools / most CI do not pay the protobuf cost).
- Platform notes: GNS documents possible link errors under LLVM 10+ (prefer GCC on Linux CI where
  relevant); validate MSVC 2026 + Apple Clang builds early — this is the largest execution risk.

## Rollout / retirement

- GNS becomes the **default** for the dedicated 128+ server; `enet6` is **retained** behind the
  factory for LAN / single-player / low-count and as the fallback backend.
- Do **not** retire `enet6` until the GNS backend ([#507]) passes the Epic I scale gate (128
  clients @ 60 Hz, ≤ ~150 KB/s/client, 2 h soak). The wire format (`kProtocolVersion = 1`) is
  **unchanged** by the transport swap.

## Follow-on issues this spike feeds

- **[#507]** — implement the GNS backend behind `INetwork`; add the `createNetwork(kind)` factory
  and route the game client through it (closing the `Game.cpp:669` HAL leak).
- **[#508]** — transport encryption/DTLS; reconcile LAN beacon + RCON (as above).
- **[#509]** — FetchContent (GNS + protobuf + libsodium) + composite CI deps + static-link/`ldd`
  guard; per-backend build gating.

### Deferred documentation updates (land with the code, not here)

- `docs/development.md` — Dependencies table + `platform/net/` description + prerequisites gain
  protobuf/libsodium → **[#509]**.
- `docs/fl-server-config.md` — a `[network] transport =` selector knob + "ENet"→"transport"
  phrasing → **[#507]/[#508]**.
- `docs/load-testing.md` — re-validate the transport ceiling post-swap with `bot_swarm` →
  **[#507]** validation.

## Sources

- GameNetworkingSockets `BUILDING.md` (deps: Protobuf 2.6.1+; crypto OpenSSL/libsodium/bcrypt;
  WebRTC for P2P NAT only) and repository (BSD-3-Clause; standalone `GameNetworkingSockets_Init`) —
  <https://github.com/ValveSoftware/GameNetworkingSockets>.
- libsodium (ISC) — <https://doc.libsodium.org>.
- `enet6` (MIT, `SirLynix/enet6`) — `cmake/dependencies.cmake`.
- Reference-env ceiling data — [#505]; application-level congestion response —
  [docs/congestion-control-design.md](congestion-control-design.md) ([#518]).
