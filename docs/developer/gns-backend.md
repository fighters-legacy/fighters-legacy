# GameNetworkingSockets Backend

Implementation notes for the **GameNetworkingSockets (GNS)** transport behind the `INetwork` HAL
([#507], Epic L). GNS is the encrypted, congestion-controlled UDP transport selected for the 128+
multiplayer target; `enet6` is retained as the LAN / single-player / low-count backend. The
selection rationale is in [transport-selection.md](decisions/transport-selection.md); this document covers the
*implementation*.

[#507]: https://github.com/fighters-legacy/fighters-legacy/issues/507
[#519]: https://github.com/fighters-legacy/fighters-legacy/issues/519

## Backend selection

`platform/net` builds three targets:

- **`platform-enet`** — the `enet6` backend (`ENetNetwork`), unchanged.
- **`platform-gns`** — the GNS backend (`GnsNetwork`), compiled only when `FL_ENABLE_GNS=ON`.
- **`platform-net`** — a thin **facade** holding `createNetwork(TransportKind)`
  (`platform/net/NetworkFactory.h`). Game and server link this and never include a concrete backend
  header.

```cpp
enum class TransportKind : uint8_t { Enet, Gns };
std::unique_ptr<INetwork> createNetwork(TransportKind kind, ILogger* log = nullptr);
TransportKind parseTransportKind(std::string_view s, TransportKind fallback);
```

`FL_ENABLE_GNS` defaults **ON**; `-DFL_ENABLE_GNS=OFF` yields a lean enet6-only build (tools, and CI
legs that don't need GNS). When GNS is requested in an enet6-only build, `createNetwork(Gns)` logs a
warning and returns the enet6 backend rather than nullptr.

**Who uses which transport:** `fl-server` selects via `[network] transport = "gns"|"enet"` (default
`gns`) or `--transport <gns|enet>`. The game client uses **GNS** for internet multiplayer and
**enet6** for single-player (the embedded `LocalServer` spawns `fl-server` with `--transport enet`).
`net_check` and `bot_swarm` **stay on enet6** — `bot_swarm` is the cross-swap regression instrument
([#519]), so the load-test runners force `--transport enet`.

## `INetwork` → GNS mapping

`GnsNetwork` implements the existing `INetwork` contract with no interface change beyond three
optional server-tuning virtuals (`setBandwidthLimit`, `setPreHandshakeRateLimit`, `setAllowInsecure`,
all default no-op on the base, so the factory return type stays honest and `fl-server` no longer
down-casts to a concrete type).

| `INetwork` | GameNetworkingSockets |
|---|---|
| `init` / `shutdown` | refcounted `GameNetworkingSockets_Init` / `_Kill` (see below) |
| `bind(addr, port, max)` | `CreateListenSocketIP` + a `CreatePollGroup`; `maxClients` enforced in the accept path |
| `connect(host, port)` | `ConnectByIPAddress` |
| `peerId` | `HSteamNetConnection` ↔ small stable `uint32_t` via two maps |
| `send` / `broadcast` (reliable flag) | `SendMessageToConnection` with `k_nSteamNetworkingSend_Reliable`/`_Unreliable` |
| `service(timeoutMs)` | `RunCallbacks()` + `ReceiveMessagesOnPollGroup`/`OnConnection` → `INetworkEventHandler` |
| `getPeerLinkStats` / `getPeerRtt` | `GetConnectionRealTimeStatus` (`m_nPing`, quality, `m_cbSentUnackedReliable`) |
| `getPeerAddress` / `getPeerState` | `GetConnectionInfo` (`m_addrRemote.ToString`, `m_eState`) |
| `disconnectPeer` / `disconnect` | `CloseConnection` (linger to flush reliable) |
| connect/disconnect events | `SteamNetConnectionStatusChangedCallback_t` → `onConnect` / `onDisconnect` |

### Multi-instance refcount ([#519])

`GameNetworkingSockets_Init` / `_Kill` are process-global and not ref-counted by GNS. Like
`ENetNetwork`'s `g_enetRefCount`, a static mutex + counter inits GNS on the first `GnsNetwork::init()`
and kills it on the last `shutdown()`, so many instances (the `bot_swarm`-style harness) coexist in
one process. Each `GnsNetwork` owns its own listen socket, poll group, and peer maps over the shared
global interface.

### Per-instance callback routing (no global registry)

The connection-status callback is set as a **per-connection config value**
(`k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged`) on the listen socket / outbound
connection, with `k_ESteamNetworkingConfig_ConnectionUserData` = the owning `GnsNetwork*`. Accepted
connections inherit both from the listen socket. A static trampoline recovers the owner from the
connection's user data and dispatches — the clean analogue of ENet's intercept registry without a
process-global map.

### Threading

Single-thread `service()`, same contract as ENet: the server host is **sim-thread-owned**, the
client host **main-thread-owned**, never crossed. GNS runs no internal service thread when pumped
manually via `RunCallbacks()`. `service(timeoutMs>0)` sleeps the timeout after draining (so per-frame
pumps spread `RunCallbacks` over wall time for handshake progress); `service(0)` — the production
sim-tick path — never sleeps.

## Encryption ([#508])

GNS negotiates **curve25519 + AES-GCM** in its handshake — transport encryption is on by default and
never disabled. Standalone (self-host, no Steam PKI) servers run with
`k_ESteamNetworkingConfig_IP_AllowWithoutAuth = 1` → **encrypted but unauthenticated** (opportunistic,
like TLS-without-cert), exposed as `[network] allow_insecure` (default true) via
`INetwork::setAllowInsecure`. Epic C's identity/auth token is designed to travel **in-band as a wire
message** (like the existing `MsgAdminCommand` token), not through the HAL. LAN discovery
(`MsgLanBeacon`, raw UDP) stays plaintext outside the transport; RCON stays a separate TCP channel —
both unchanged.

## Dependencies & build (crypto = OpenSSL, protobuf system-preferred)

Two decisions here **reverse** the [transport-selection.md](decisions/transport-selection.md) spike, for
concrete reasons discovered during implementation:

- **Crypto = OpenSSL, not libsodium.** GNS's `CMakeLists.txt` makes `USE_CRYPTO=libsodium` a
  `FATAL_ERROR` on non-x86 CPUs (libsodium's AES is x86-only), and the macOS CI runner is Apple
  Silicon (arm64). OpenSSL (Apache-2.0, GPL-3-compatible) builds on every target.
- **protobuf = system-preferred + FetchContent fallback** (the same pattern as SDL3 / OpenAL /
  Catch2). GNS builds its own `.proto` files with `protobuf_generate_cpp`, which is only available
  from protobuf's installed CMake config; the pure-FetchContent handoff is blocked because CMake
  refuses to `include()` protobuf's `export()`-generated build-tree targets file within the same
  build. System protobuf (GNS's own BUILDING.md recommendation) sidesteps this cleanly.

GNS is pinned to **v1.6.0**, built **static** with **ICE/WebRTC off** (dedicated server, no P2P) — so
no abseil/webrtc submodules (`GIT_SUBMODULES ""`). Declared in `cmake/dependencies.cmake` (gated on
`FL_ENABLE_GNS`); `platform/net/CMakeLists.txt` links it. GNS/protobuf headers are `SYSTEM` includes
so their warnings don't fail our `-Werror` build.

Per-platform dependency sourcing. **GNS builds on all three CI platforms** ([#653]); the common
constraint is that GNS v1.6.0 needs the **pre-abseil protobuf 3.21.x line** (the abseil-based
4.x/5.x protobuf does not build against it):

| Platform | Status | OpenSSL | protobuf |
|---|---|---|---|
| Linux | **GNS on** | `libssl-dev` (apt) | `libprotobuf-dev` + `protobuf-compiler` (apt — Ubuntu ships 3.21.x) |
| macOS | **GNS on** | `brew openssl@3` (`OPENSSL_ROOT_DIR`) | pinned formula `protobuf@21` (= 3.21.12, keg-only), surfaced via `CMAKE_PREFIX_PATH` — the main `protobuf` formula is the abseil-based 5.x line |
| Windows | **GNS on** | runner-provided | vcpkg manifest (repo-root `vcpkg.json` pins `protobuf` 3.21.12#4) under the vcpkg toolchain; `x64-windows-static-md` triplet matches the presets' /MD(d) CRT |
| Fedora (reference env) | **GNS on** | `openssl-devel` (dnf) | `protobuf-devel` + `protobuf-compiler` (dnf — Fedora ships 3.19.x) **plus `-DFL_ALLOW_SHARED_PROTOBUF=ON`**, see below |

protobuf is linked **statically**: `Protobuf_USE_STATIC_LIBS ON` is set before the seeding
`find_package`, because a release `fl-server` dynamically linked against `libprotobuf.so.32` will
not load on a machine without that exact private build ([#905]).

**`FL_ALLOW_SHARED_PROTOBUF`** (default OFF) exists because some distributions — Fedora among them —
ship protobuf as a shared library only. There the static-only find fails with every package
correctly installed, and GNS gets force-disabled on a machine that could build it perfectly well
([#1136]). Turning the option ON retries the find against the shared library and warns loudly, every
configure, that the resulting binary must not be shipped. It is for binaries that never leave the
machine that built them: the `bot_swarm` reference environment sets it, and nothing that ships does.

`cmake/dependencies.cmake` auto-disables `FL_ENABLE_GNS` (with a warning naming *which* of the two
causes applies) when OpenSSL or usable system protobuf is absent, so any build/CI leg without the
deps configures cleanly as enet6-only. Because of that graceful fallback, the CI legs and
`run-benchmark.sh` **assert `FL_ENABLE_GNS:BOOL=ON` in the CMakeCache after configure** — a broken
dependency setup fails the leg instead of silently passing as enet6-only.
It uses `find_package(Protobuf)` to both gate and seed the module cache that GNS's own
`find_package` reuses. On macOS, pinning `protobuf@21` first in `CMAKE_PREFIX_PATH` also avoids
the mixed module/config double-`find_package` clash the abseil-based Homebrew `protobuf` config
would cause ("some but not all targets already defined" — protobuf 5.x adds `libupb`). The
repo-root `vcpkg.json` only takes effect under the vcpkg toolchain — local non-vcpkg builds
ignore it.

[#508]: https://github.com/fighters-legacy/fighters-legacy/issues/508
[#649]: https://github.com/fighters-legacy/fighters-legacy/issues/649
[#653]: https://github.com/fighters-legacy/fighters-legacy/issues/653
[#773]: https://github.com/fighters-legacy/fighters-legacy/issues/773
[#905]: https://github.com/fighters-legacy/fighters-legacy/issues/905
[#1136]: https://github.com/fighters-legacy/fighters-legacy/issues/1136

## Scale validation ([#649], GNS-primary since [#773])

GNS ships as the default internet transport, so it is gated at scale, not just built — and since
[#773] it is the **primary** scale-gate profile, not a side leg: the `reference` profile runs 128
clients with **both ends on GNS** (`FL_LOADTEST_TRANSPORT=gns` → `fl-server --transport gns` +
`bot_swarm --transport gns`) on the 8-core reference runner, and the `soak`/`overrun`/`congestion`/
`entity-scale`/`entity-churn` characterisation profiles all pin GNS too. enet6 keeps two regression
legs (`pr` on every PR, `reference-enet` at full 128-client scale) as the LAN/single-player backend.
See [load-testing.md](load-testing.md#the-transport-the-gate-measures-649-773) for the runbook, the
measured enet6-vs-GNS comparison, and the three independent guards that stop a GNS run from silently
degrading into an enet6 run.

Headline: at 128 clients GNS admits all 128 and holds 60 Hz, and **server tick p99 is ~7–8× lower
than enet6** (1.5–1.7 ms vs 5.5–12.6 ms on the same box) — ENet does its per-packet send work inline
on the sim thread (inside the serialize phase) while GNS hands off to its own service thread.

**The tick win used to cost 1.3×–4.3× more wire bytes** than enet6 for identical application
payload ([#772]): `ENetNetwork` enables ENet's range coder, **GNS does not compress at all** (it
encrypts), and it sent ~1.8× the datagrams (MTU fragmentation of ~1.2 KB snapshots). [#775] closed
the gap with engine-layer zstd snapshot compression (`[network] compress_snapshots`, default on):
measured at 128 clients, GNS idle wire drops 75.5 → 16.9 KB/s/client (now *below* enet6's 17.6),
active patterns drop ~15 % to ~1.1× enet6 (the residual is AES-GCM + framing no codec can remove),
and the datagram rate collapses to the 60 Hz data floor because compressed snapshots fit one MTU.
Wire bytes — not the transport-independent payload figure — are what an operator's bandwidth bill
is denominated in, so the gate's hard 150 KB/s/client ceiling is enforced on them. Full tables:
[load-testing.md](load-testing.md#wire-bytes-vs-payload-bytes-772--read-this-before-quoting-a-bandwidth-number).

[#772]: https://github.com/fighters-legacy/fighters-legacy/issues/772
[#775]: https://github.com/fighters-legacy/fighters-legacy/issues/775

## Testing

`tests/test_gns_network.cpp` holds `GnsNetwork` to the same contract as `test_network.cpp`: loopback
connect, reliable + unreliable send, multi-client broadcast, disconnect callback, link-stats, the
multi-instance refcount, and the before-connect/out-of-range guards. Built only when
`FL_ENABLE_GNS`. `tests/test_network_factory.cpp` covers `parseTransportKind` and `createNetwork`.
