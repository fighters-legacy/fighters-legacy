# Client-Favored Turret Hit Registration — Design Record and Trigger Criterion

> **Frozen decision record.** This page records a decision as it was made and is not
> maintained against current behaviour. For how the engine works today, see the
> [Developer Guide](../index.md).

Design record for fair hit registration of a **human turret gunner** on a multi-crew aircraft
(Epic [#966]). Resolves the design spike [#973]. A gunner (the seat UI lands in [#975]/[#979],
seat-scoped input in [#972], seat join/handoff in [#974]) aims a rotating turret and fires guns
(hitscan) or projectiles at a moving target while separated from the server by network latency.
This is the turret analogue of the pilot's own-ship `ClientPrediction` — but the two problems are
*not* the same, and the difference is what this document is about.

**Recommendation: implement-now, in [#979], as an EXTENSION of the machinery that already exists —
not a new subsystem.** The server already (a) slews every turret authoritatively via
`commandTurretWorld` + `stepTurret` (`engine/weapon/Turret.h`), and (b) lag-compensates hitscan by
rewinding target positions through `TransformHistory` (`engine/net/TransformHistory.h`, #425). The
work owed to a *human* gunner is three bounded deltas on top of that spine: key the existing rewind
off the **gunner's** peer delay rather than the airframe's; predict the turret pose client-side so
the reticle is not latency-lagged (reusing `stepTurret` verbatim, exactly as `ClientPrediction`
reuses `FlightIntegrator`); and — only if reticle-vs-server bore divergence at the fire instant is
shown to cost hits — accept a client-asserted fire bore that the server clamps to the reachable arc
by replaying `stepTurret` over the bounded window. The third is the "client-favored" escalation and
is **deferred behind a measured trigger**; the first two ship in [#979].

[#966]: https://github.com/fighters-legacy/fighters-legacy/issues/966
[#969]: https://github.com/fighters-legacy/fighters-legacy/issues/969
[#970]: https://github.com/fighters-legacy/fighters-legacy/issues/970
[#971]: https://github.com/fighters-legacy/fighters-legacy/issues/971
[#972]: https://github.com/fighters-legacy/fighters-legacy/issues/972
[#973]: https://github.com/fighters-legacy/fighters-legacy/issues/973
[#974]: https://github.com/fighters-legacy/fighters-legacy/issues/974
[#975]: https://github.com/fighters-legacy/fighters-legacy/issues/975
[#979]: https://github.com/fighters-legacy/fighters-legacy/issues/979
[#425]: https://github.com/fighters-legacy/fighters-legacy/issues/425
[#427]: https://github.com/fighters-legacy/fighters-legacy/issues/427
[#566]: https://github.com/fighters-legacy/fighters-legacy/issues/566
[#625]: https://github.com/fighters-legacy/fighters-legacy/issues/625
[#630]: https://github.com/fighters-legacy/fighters-legacy/issues/630

## Problem statement

A defensive tail-gunner (`builtin:bomber`, [#977]) or any crewed turret seat leads a crossing
target and pulls the trigger. Between the moment the gunner *saw* the target and the moment the
server *resolves* the shot, three separate latencies stack up, and each one, handled naively,
steals hits from a gunner who aimed correctly:

1. **Target-position staleness (the classic lag-comp problem).** The gunner aims at where the
   target appeared in the last `MsgWorldSnapshot` they decoded — a world that is
   `estimatedDelayTicks` old (`PeerInputState::estimatedDelayTicks`, one-way delay in sim ticks).
   If the server tests the shot against *current* positions, a target crossing at 300 m/s has moved
   ~1.6 m per 100 ms of delay past the point the gunner put their reticle on. The gunner leads
   perfectly for the world they can see and misses the world the server has. This is exactly the
   problem `resolveHitscan` already solves for the pilot's nose gun by rewinding through
   `TransformHistory` — but it currently keys the rewind off `peerIdForEntity(shooter->id)`, the
   **airframe's** owning peer, which for a crewed aircraft is the *pilot*, not the gunner.

2. **Turret-pose staleness (new, turret-specific).** The turret is a rate-limited servo:
   `TurretLimits::slewRateRadS` defaults to 60 deg/s, and `stepTurret` moves the pose toward the
   commanded direction one tick at a time, clamped to `[azMin,azMax]×[elMin,elMax]`. The gunner's
   aim command (`SeatCommand::aimDirWorld`, or a human gunner's `viewAxis` via [#972]) travels to
   the server, is turned into a commanded az/el by `commandTurretWorld`, and only *then* does the
   pose begin catching up at the servo rate. A gunner tracking a fast crosser sees their local
   reticle where they are pointing; the server's authoritative bore
   (`turretWorldDir(TurretState, mountRest, airframeQuat)`) lags behind it by the round-trip plus
   the servo's own catch-up time. Fire at that instant and the shot leaves along a bore that is
   *behind* where the gunner was aiming.

3. **Airframe motion under the mount.** The turret's world bore is
   `airframeWorldQuat · mountRestQuat · bore_mount(az,el)`. The airframe is itself moving and
   rotating (the pilot is flying it, predicted independently by `ClientPrediction`), so even a
   perfectly-tracked az/el points somewhere different a few ticks later. The gunner cannot be asked
   to compensate for their own aircraft's motion on top of leading the target.

Naive server-only registration — resolve the shot against current target positions along the
server's current authoritative bore, no rewind — makes a competent gunner feel like the game is
eating their hits, because all three latencies push the same way: the target has moved on, and the
bore has not yet arrived. The pilot got `ClientPrediction` + `TransformHistory` rewind for exactly
this reason; the gunner needs the turret-shaped equivalent.

[#977]: https://github.com/fighters-legacy/fighters-legacy/issues/977

## What already exists (and why this is an extension, not a subsystem)

Grounding the design in the actual code, three pieces are already in place:

- **The turret servo is pure and server-authoritative.** `engine/weapon/Turret.h` is deliberately
  allocation-free and deterministic: `stepTurret(TurretState&, const TurretLimits&, float dt)` slews
  the pose; `commandTurretWorld` sets the clamped commanded az/el from a world aim direction; and
  `turretWorldDir` reads the bore back out. The header comment states the reuse contract directly:
  *"WorldBroadcaster steps it in the integrate pass … and the client replays it verbatim as the
  turret predictor — the same reason evaluateFire is pure."* The predictor half of that sentence is
  what [#979] builds; the primitive is ready.

- **The fire path already carries a per-shot world bore.** `FireRequest` (`engine/weapon/FireControl.h`)
  has `bool hasAimDir` + `float aimDir[3]`. `runCrewedFire` (`WorldBroadcaster.cpp`) stamps it from
  `turretWorldDir` for a turret seat, and both `resolveHitscan` and `ProjectileSystem::launch`
  already fire along `req.aimDir` when present (else the airframe nose, bit-identical). No new fire
  plumbing is needed — the shot already leaves along the turret bore.

- **Lag compensation already rewinds targets.** `TransformHistory` records every live entity's
  post-integrate position each tick (`beginTick`/`add` at the top of the weapons phase),
  `kHistoryTicks = 32` (≈533 ms at 60 Hz — the rewind clamp bound). `resolveHitscan` rewinds to
  `tickIndex − min(estimatedDelayTicks, kHistoryTicks−1)`, broadphases on current positions inflated
  by the max drift (`kVelMaxMps/60` per rewound tick), and tests the exact ray against each
  candidate's **rewound** position, with a generation check so a recycled pool slot is never hit
  through history.

The gap is narrow and specific: the existing rewind uses the **airframe** peer's delay, the turret
pose is **not predicted client-side** (so a human gunner's reticle is laggy), and there is **no
client-asserted fire bore** (so any residual servo-prediction error at the fire instant is a lost
hit rather than a validated claim). This document designs those three deltas.

## The chosen model

The design is a spine plus a gated escalation. The spine is what [#979] implements; the escalation
is deferred behind the trigger in the Decision section.

### Spine (implement in #979): server-authoritative slew + gunner-keyed rewind + client pose prediction

1. **The server remains the slew authority.** A human gunner's aim command arrives as the
   seat-scoped `viewAxis` from [#972] (see Wire section), is fed to `commandTurretWorld` exactly
   where a bot's `SeatCommand::aimDirWorld` is fed today in `sampleCrewSeats`, and the pose advances
   via `stepTurret` in the integrate pass. The bore the shot leaves along is the server's own
   `turretWorldDir`, never a number the client asserts. This is the safest possible starting point
   and it is *already how bot turrets work* — the human gunner simply supplies the aim command.

2. **Hit registration rewinds targets by the GUNNER's delay.** `resolveHitscan` is changed so that
   for a turret-mounted request (`req.hasAimDir`, i.e. `seat.turretIndex >= 0`) the rewind reads the
   **firing seat's occupant** peer delay, not the airframe peer's. `CrewSeat::occupantPeer` ([#972]/
   [#974]) gives the gunner's peer id; its `PeerInputState::estimatedDelayTicks` is the correct
   rewind depth. A bot gunner (`occupantPeer == kNoSeatPeer`) rewinds 0, identical to today's AI
   shooter. This closes latency (1) — the target is tested where the gunner *saw* it.

3. **The client predicts the turret pose for the reticle only.** The gunner's client runs a local
   `TurretState` and calls `stepTurret` every frame with its own aim command, exactly as
   `ClientPrediction::onInput` steps a local `FlightIntegrator`. This makes the reticle/tracer track
   the gunner's input at frame rate instead of at snapshot rate. On each snapshot the client
   **reconciles**: the authoritative `TurretState` (see Wire — the pose must reach the client) resets
   the local pose, and the last `estimatedDelayTicks` aim commands are replayed via `stepTurret` —
   the turret-shaped mirror of `ClientPrediction::reconcile`'s snap-vs-blend. Because the servo is
   rate-limited and pure, replay is cheap and exact. This hides latency (2)/(3) *visually* without
   giving the client any authority.

Under the spine, "client-favored" means the *target rewind* is favored to the gunner's view (as it
already is for the pilot) and the *reticle* is predicted; the **bore is still the server's**. For a
60 deg/s servo tracking a target the gunner has already settled on, the server bore and the
predicted reticle agree to within the per-tick slew step, and the rewind does the heavy lifting.

### Escalation (deferred): client-asserted fire bore, clamped to the reachable arc

The residual error the spine does *not* remove is the bore mismatch at the exact fire instant when
the gunner is still slewing hard (a fast target just entering the arc). Here the server bore trails
the reticle by up to the round-trip in servo travel. If measurement shows this costs hits, the
escalation lets the fire message carry the **client's asserted bore** (the predicted
`turretWorldDir` at the fired client tick), and the server *validates* it rather than trusting it:

- Replay `stepTurret` from the **last server-acked** `TurretState` (the pose the client last
  confirmed decoding) forward over the bounded window `[ackedTick, fireTick]`, driven by the aim
  commands the server has for that seat, to obtain the **envelope of poses the physical turret could
  have reached**. Because `stepTurret` is rate- and arc-limited, this envelope is exactly the set of
  legitimate bores.
- Accept the claimed bore iff it lies within `kBoreAcceptRad` of a pose in that reachable envelope
  (equivalently: the claimed az/el is within the arc limits *and* within `slewRateRadS × window` of
  the acked pose). Otherwise clamp to the nearest reachable bore — the client is favored up to what
  the servo could physically do and no further.
- Resolve the shot (hitscan rewind or `launch`) along the **validated** bore.

This is the literal "client-favored bounded by the server slew servo" model: the client may claim
any aim the physical turret could have reached in the elapsed time from a state both sides agree on,
and cannot claim one it could not. It reuses `stepTurret` verbatim on the server for validation and
on the client for prediction — one function, three call sites (server slew, server validation,
client predictor), no second implementation to drift.

## Wire / protocol implications

**House rule: additive ids only, no `kProtocolVersion` bump during primary development** (see
`CLAUDE.md`, the memory `feedback_no_version_bumps_primary_dev`). Everything below is additive.

### Aim command and fire (seat-scoped, via #972 — no new client→server message)

[#972] makes `MsgClientInput` seat-scoped: a human occupant of a non-Fly seat sends the same 80-byte
`MsgClientInput`, and the server routes it to that seat instead of the airframe. The existing fields
already carry everything the gunner needs:

| Field (offset) | Turret meaning |
|---|---|
| `viewAxis[3]` @32 | The gunner's world-space aim direction → the seat's `SeatCommand::aimDirWorld` → `commandTurretWorld`. Already normalized + finite-guarded server-side. |
| `buttons` bit 0 @… | Gun trigger (level) → `WeaponControls::trigger`. |
| `buttons` bit 2 | Fire selected store (edge-detected) → `WeaponControls::release`. |
| `selectedStation` @48 | Absolute station within the **seat's disjoint loadout partition** (`CrewState` one-owner-per-channel invariant); 255 = keep. |
| `tickIndex` @8 / `ackMask` @44 | Already the snapshot ack ([#566]); doubles as the gunner's rewind-depth signal (`estimatedDelayTicks = currentTick − tickIndex`) and, for the escalation, the `ackedTick` the pose replay starts from. |

So the **spine needs no new client→server message at all** — it is `MsgClientInput` routed to the
seat by [#972], plus a server-side change to which peer's delay `resolveHitscan` reads.

The **escalation** needs the client to assert its predicted bore at the fired tick. Two options,
both additive:

- **Preferred: a TLV on `MsgClientInput`** carrying `claimedBore[3]` (world-space, or the more
  compact `az`/`el` mount-frame `int16` pair since the server knows `mountRest`/`airframeQuat`),
  present only on ticks where bit 0/bit 2 is set. TLV keeps it out of the hot 80-byte struct and
  invisible to non-gunner clients. The escalation is the *only* thing that needs this; the spine
  ships without it.
- **Alternative:** a dedicated `MsgSeatFire` reliable message. Rejected for the spine (fire on the
  reliable channel adds a round-trip of head-of-line blocking to a 60 Hz action) and unnecessary for
  the escalation (the TLV rides the unreliable input the gunner is already sending).

### Turret pose on the wire (server→client, needed for client prediction reconcile)

The client predictor must reconcile against the authoritative `TurretState`, so the pose has to
reach the gunner's client. This is a **new `MsgWorldSnapshot` TLV**, in the `0x01xx` snapshot-ext
range alongside `SnapshotPeerLatency`/`SnapshotPeerDelayTicks`:

| Tag (proposed) | Payload | Notes |
|---|---|---|
| `SnapshotCrewTurret` `0x0106` | per-owned-turret `{turretIndex uint8, azRad, elRad}` (quantized; the servo pose only) | Emitted **only to the peer occupying a turret seat on that aircraft**, only for that peer's own turret(s) — the "own-record only" precedent the snapshot already uses for `omega`. A spectator or the pilot does not need another crew's turret pose predicted; the visible bore already rides the airframe transform. |

Quantization follows `docs/developer/decisions/snapshot-quantization.md`: az/el are bounded angles, a 12–14 bit
fixed-point per axis is ample (finer than the 60 deg/s servo moves in a tick). This mirrors how
`omega` is present only on the receiving peer's own record — the pose is prediction state for its
owner and dead weight for everyone else.

Everything is additive: old clients (and every non-gunner client) ignore the TLV; the pose is
belt-and-suspenders for reconciliation, since the visible turret already tracks the airframe
transform and the *authoritative* bore is what the server fires along regardless.

## Server validation algorithm

The spine changes exactly one thing in `resolveHitscan` — the rewind peer. The escalation adds the
bore-validation prologue. Pseudocode against the real functions (compare `resolveHitscan`,
`runCrewedFire`, `sampleCrewSeats` in `WorldBroadcaster.cpp`):

### Spine: gunner-keyed rewind (the whole of #979's hit-reg change)

    // in resolveHitscan(req, def, tickIndex), replacing the peerIdForEntity(shooter->id) lookup:
    uint64_t rewindTicks = 0;
    uint32_t rewindPeer = kNoOwningPeer;
    if (req.hasAimDir) {
        // Turret shot (#970): rewind by the FIRING SEAT's occupant, not the airframe's pilot.
        const CrewSeat* seat = crewSeatForRequest(req);          // ce.crew.seats[req.seat]
        if (seat && seat->occupantPeer != kNoSeatPeer)
            rewindPeer = seat->occupantPeer;
    } else {
        rewindPeer = peerIdForEntity(shooter->id);               // pilot nose gun — unchanged
    }
    if (rewindPeer != kNoOwningPeer)
        if (auto pit = m_peerInputs.find(rewindPeer); pit != m_peerInputs.end())
            rewindTicks = std::min<uint64_t>(pit->second.estimatedDelayTicks,
                                             TransformHistory::kHistoryTicks - 1);
    rewindTicks = std::min(rewindTicks, tickIndex);
    // ... the existing broadphase-inflate-by-drift + per-candidate queryAt(rewTick, idx, gen)
    //     rewound ray test is UNCHANGED from here down.

A bot gunner (`occupantPeer == kNoSeatPeer`) yields `rewindTicks == 0` — byte-identical to today's AI
turret shot. A single-seat fighter never sets `req.hasAimDir`, so its nose gun is byte-identical too.

### Escalation: validate the claimed bore against the reachable arc

    // Runs in the serial weapons pass, before resolveHitscan/launch, when a claimedBore TLV is present.
    glm::vec3 validateFireBore(const CrewSeat& seat, const CrewTurret& tr,
                               const glm::quat& airQ, uint64_t ackedTick, uint64_t fireTick,
                               const glm::vec3& claimedBoreWorld) {
        // 1. Bound the window to the history clamp — a claim older than the ring is not honored.
        const uint64_t window = std::min<uint64_t>(fireTick - ackedTick,
                                                    TransformHistory::kHistoryTicks - 1);
        // 2. Replay the servo from the pose the client last acked, driving it with the aim commands
        //    the server holds for this seat over [ackedTick, fireTick]. stepTurret is pure + arc/rate
        //    limited, so the resulting pose is precisely "the farthest the physical turret could reach".
        TurretState replay = seat.ackedTurretState;              // pose at ackedTick (retained per seat)
        for (uint64_t t = 0; t < window; ++t) {
            commandTurretMount(replay, tr.limits, seatAimAt(seat, ackedTick + t)); // clamped command
            stepTurret(replay, tr.limits, kSimDt);
        }
        const glm::vec3 reachableBore = turretWorldDir(replay, tr.mountRest, airQ);
        // 3. Client is favored UP TO the reachable envelope; a claim beyond it is clamped, not trusted.
        const glm::vec3 claimMount = worldToMountDir(claimedBoreWorld, tr.mountRest, airQ);
        if (angleBetween(reachableBore, claimedBoreWorld) <= kBoreAcceptRad
            && withinArc(claimMount, tr.limits))
            return claimedBoreWorld;                             // accept the client's bore
        return reachableBore;                                    // clamp to what the servo could do
    }

The validated bore then flows into the *existing* `req.aimDir` slot, so `resolveHitscan` (rewound as
above) and `ProjectileSystem::launch(..., &aimDirWorld)` consume it with no further change.

## Anti-cheat bounds

Every degree of freedom the client is favored on is bounded by a server-authoritative limit; the
table is the contract:

| Vector | Bound | Enforced by |
|---|---|---|
| **Rewind depth** | `min(gunner.estimatedDelayTicks, kHistoryTicks−1)` = ≤ 31 ticks ≈ 533 ms | `TransformHistory` ring size; a claim to rewind further has no data and rewinds 0. The gunner cannot pick the rewind — it is the server's own measured one-way delay for that peer. |
| **Claimed bore** | within `slewRateRadS × window` of the last-**acked** `TurretState`, and within `[azMin,azMax]×[elMin,elMax]` | replaying `stepTurret`/`commandTurretMount` from a pose the client already confirmed. A client asserting a snap-to-target bore the 60 deg/s servo could not have reached is clamped to the reachable bore. |
| **Fire rate** | `FireState::nextGunTick` (gun `rate_of_fire_rpm`), `nextReleaseTick` (store cooldown), ammo in the seat's disjoint `LoadoutState` | `evaluateFire` — unchanged; per-seat because the [#966] one-owner-per-channel partition makes ammo per-seat. A gunner cannot out-fire the servo or the magazine. |
| **Input rate** | 60 × `floodMultiplier` packets/s | the existing `m_peerFloodState` flood gate in `onReceive`. |
| **Target identity** | generation check on every `TransformHistory::queryAt` | a recycled pool slot can never be hit through history — the entity that occupied that index at the rewound tick is not the one living there now. |

What a malicious client **cannot** achieve, stated plainly:

- **Cannot hit a target the gunner could not have been aiming at.** The bore is clamped to the
  reachable servo arc from an acked pose; there is no "aimbot snap" the physical turret could not
  perform in the elapsed time.
- **Cannot rewind arbitrarily far** to shoot a target long since moved on — the ring caps it at
  ~533 ms and the depth is the server's own delay measurement, not a client number.
- **Cannot fire faster than the weapon**, cannot fire an empty station, cannot fire a station in
  another seat's partition (the request's `seat`/`station` are validated against
  `CrewState`/`FireState`).
- **Cannot hit a stale/recycled entity** through the generation check.
- **Cannot manufacture a release** via loss concealment — the jitter buffer masks bit 2 on a
  stale-repeat, the same guard `MsgClientInput` already documents.

The residual, deliberate concession is the standard lag-comp "shot around the corner": a target that
has just broken line of sight on the *victim's* screen can still be hit at its rewound position, up
to ≈533 ms. This is the identical trade already accepted for the pilot's gun ([#425],
`docs/developer/network-protocol.md`); the turret inherits it rather than inventing a new policy.

## Determinism & serial-equivalence

The weapons phase is **serial and must stay byte-identical across worker counts** — `runWeaponsPass`
sorts `m_fireRequests` by `(shooterIdx, seat, station)` and executes them serially, and
`ProjectileSystem` steps serially by design. Nothing here changes that; the rules to preserve it:

- **Pose prediction is client-only.** `stepTurret` on the client touches no server state and is
  never on the wire. The server's slew (`sampleCrewSeats` → `commandTurretWorld` → `stepTurret` in
  the integrate pass) is unchanged and already per-entity-disjoint (each aircraft's crew is stepped
  inside its own worker slice, writes confined to that entity — see `CrewState`'s "parallel access is
  confined to the owning entity's worker slice").
- **Validation and rewind run in the serial weapons pass**, reading only frozen pre-step state
  (`TransformHistory` recorded at the top of the phase, `PeerInputState` read-only there,
  `seat.ackedTurretState` a per-seat scalar). No worker reads another entity's turret state.
- **The claimed-bore validation is a pure function** of `(ackedTurretState, aim-command history,
  airframeQuat, claimedBore, window)` — `stepTurret`/`commandTurretMount`/`turretWorldDir` are the
  same allocation-free deterministic primitives the header guarantees. Given identical inputs it
  produces an identical validated bore on every worker count and every platform (float math is
  identical; no RNG, no `from_chars`, no order-dependence).
- **The retained `ackedTurretState` is per-seat and written only by its owning entity's slice** —
  it joins `lastCommand`/`lastCommandValid` in `CrewSeat`, the same disjoint-write discipline
  `lastInput` already follows.

The enforcing test is the existing one, extended: pin a crewed aircraft with a human-occupied turret
seat (mock peer with a fixed `estimatedDelayTicks`) firing at a scripted crosser, and assert
bit-identical `FireRequest` streams + resolved hits + damage for `workerCount ∈ {1, 4}` in
`test_world_broadcaster` under the TSan `tsan.yml` target. The gun-dispersion hash
(`shooterIdx, tickIndex`) is unchanged and already replay-stable.

## Interaction with the existing pilot `ClientPrediction`

The two prediction systems are **independent by construction and do not interact**:

- `ClientPrediction` (`game/fighters-legacy/ClientPrediction.h`) predicts **only the player's own
  aircraft** — it is init'd with `playerIdx`/`playerGen` and `reconcile()` mutates only that entry;
  every other entity stays server-authoritative. It replays *flight inputs* through a
  `FlightIntegrator`.
- Turret prediction predicts **only a gunner seat's own turret pose** — a separate local
  `TurretState` replayed through `stepTurret`. It touches no `FlightIntegrator` and no other entity.

Their only contact point is that the turret's world bore is
`airframeWorldQuat · mountRest · bore_mount`, so a client that is *both* pilot and gunner would want
the predicted airframe orientation under the mount. But a crewed aircraft has **one Fly seat**:
whoever occupies it runs `ClientPrediction` for the airframe; a *different* peer in the turret seat
does **not** predict the airframe (they receive it as a normal server-authoritative snapshot
entity) and predicts only the pose *relative* to that received transform. The single-occupant edge
case (one human flying and gunning, if [#974] ever allows it) simply composes the two local
predictors — airframe from `ClientPrediction`, pose from the turret predictor — with no shared
state. The masked merge that keeps flight (Fly seat) and each fire channel (owning seat) disjoint on
the server (`ISeatController.h`, `runCrewedFire`) is mirrored on the client: each predictor owns its
own slice.

## Decision

**Implement-now for the spine, defer the escalation behind a trigger.**

The spine — gunner-keyed rewind + client turret-pose prediction — is the concrete design [#979]
should build. It is small, it reuses `TransformHistory` and `stepTurret` verbatim, it needs **no new
client→server message** (seat-scoped `MsgClientInput` from [#972] carries the aim + fire) and one
additive server→client snapshot TLV for the pose, and it brings the human gunner to parity with the
pilot's already-shipped hit-reg. There is no reason to wait: multi-crew turrets are landing in this
same epic and a gunner without lag comp will feel broken on day one.

The **client-asserted-bore escalation** is deferred. It exists only to recover the residual bore
mismatch at the fire instant *while the gunner is still slewing hard*, and whether that residual
actually costs meaningful hits is an empirical question the spine's own telemetry answers. Building
the bore-claim TLV + validation replay before that evidence exists is speculative complexity on the
serial weapons hot path.

### Fallback and trigger

**Fallback if even the spine's client pose predictor proves too costly or divergent** (e.g. the
snapshot TLV pose reconciliation fights airframe prediction and produces reticle jitter): drop the
client pose predictor and ship **server-authoritative-slew-only** — the reticle is drawn at the last
server bore (snapshot-rate), and *only* the gunner-keyed rewind remains. The rewind alone already
fixes the dominant latency (target staleness) and is the same mechanism the pilot uses; the
predicted reticle is a smoothness nicety, not a correctness requirement. This fallback is a strict
subset of the spine and is always available.

**Escalate to the client-asserted-bore model when, on the 8-core reference environment ([#569]) at a
crewed-turret workload with a human gunner at a representative delay (≥ 6 ticks ≈ 100 ms one-way), a
sustained window shows:**

> the fraction of gunner trigger-pulls whose **predicted-reticle bore diverges from the
> server-authoritative bore by more than `kBoreAcceptRad` at the fire instant** exceeds a set
> threshold (proposed: > 10% of shots while the servo `|cmd − pose|` is non-trivial), **and** the
> resulting felt miss-rate is confirmed in playtest —

i.e. escalate only when the servo lag at the fire instant is measurably eating hits the rewind
cannot recover. Instrument it by logging, per turret shot, `angleBetween(clientReticleBore,
serverBore)` and the servo error at fire; the metric rides the same `--metrics-json` path the
overrun profile uses. No trigger, no escalation — this section exists so the escalation starts from a
worked plan the day the metric fires.

[#569]: https://github.com/fighters-legacy/fighters-legacy/issues/569

## Follow-on issue sketch (the #979 spine)

Title: `feat(netcode): lag-compensated turret hit registration for human gunners`
(Feature, Epic [#966], references this document + [#973]; the [#979] hit-reg slice)

- **Rewind by the firing seat's occupant** in `resolveHitscan`: for `req.hasAimDir`, read
  `ce.crew.seats[req.seat].occupantPeer`'s `estimatedDelayTicks` instead of
  `peerIdForEntity(shooter->id)`; bot seat (`kNoSeatPeer`) rewinds 0. Single-seat + pilot nose gun
  byte-identical.
- **Client turret pose predictor**: a local `TurretState` stepped each frame via `stepTurret` from
  the gunner's aim command; reconcile against the new `SnapshotCrewTurret` TLV
  (snap-vs-blend mirror of `ClientPrediction::reconcile`); lands in the [#975]/[#979] gunner station.
- **Additive `SnapshotCrewTurret` TLV** (`0x0106`, own-turret-only, quantized az/el per
  `docs/developer/decisions/snapshot-quantization.md`); documented in `docs/developer/network-protocol.md`. No `kProtocolVersion`
  bump.
- **Seat-scoped aim/fire** consumed from [#972]'s seat-routed `MsgClientInput`
  (`viewAxis`→`commandTurretWorld`, `buttons` bit 0/2 → `WeaponControls`); no new client→server
  message.
- **Tests**: `test_world_broadcaster` — gunner-keyed rewind (human gunner at fixed delay hits the
  rewound crosser; bot gunner rewinds 0); cross-worker-count bit-equivalence with a human-occupied
  turret seat (`workerCount ∈ {1,4}`, TSan); generation-check on a recycled target slot.
- **Docs**: this document's status and the `docs/developer/network-protocol.md` TLV table. No
  changelog entry — the commit subject is the entry (#1123).

Escalation (client-asserted bore + `validateFireBore` replay) is a *separate* follow-on filed only
on the trigger above, from the escalation pseudocode in this document.
