# Reduced-Rate / LOD Physics — Design Record and Trigger Criterion

Design record for reduced-rate ("LOD") physics integration of distant AI entities (Epic A).
Resolves the design spike [#575]. The overrun governor ([#514]) sheds with snapshot send-rate
decimation, byte-budget scaling, and AI-sample decimation, but deliberately never decimates the
integrate pass — `TickGovernor.h` states the invariant directly: *"the integrate pass is never
decimated — fixed-dt stability"*. When a server is **integrate-bound**, the governor therefore has
no lever left and the `GameLoop` catch-up cap absorbs the overrun as bounded time dilation (rising
`dropped_ticks`). This document designs the missing lever — integrating low-relevance AI entities
every Nth tick — grounds its stability envelope in the actual integrator, and defines the trigger
that says "build it". It is ladder item 4 of the pre-sharding ladder in
[spatial-sharding-design.md](spatial-sharding-design.md), which routes an *integrate-dominant*
overrun here.

**Recommendation: conditional go — design approved, implementation deferred behind an
integrate-bound trigger.** The integrate pass measures ~0.21 µs/entity today (1.038 ms at 5000
entities on one worker, [entity-scale-characterization.md](entity-scale-characterization.md)); it
is nowhere near the ceiling at the 128+ product target. The lever becomes real when mission-scale
AI populations (Epic N, the dynamic campaign director) push live entity counts into the 10⁴–10⁵
range. The contingent design below — full-stride integration with exact per-entity time
accounting, a stride ceiling of 4×, a serially-frozen eligibility predicate, and a fourth governor
lever — is ready to implement when the trigger fires.

[#380]: https://github.com/fighters-legacy/fighters-legacy/issues/380
[#402]: https://github.com/fighters-legacy/fighters-legacy/issues/402
[#511]: https://github.com/fighters-legacy/fighters-legacy/issues/511
[#512]: https://github.com/fighters-legacy/fighters-legacy/issues/512
[#514]: https://github.com/fighters-legacy/fighters-legacy/issues/514
[#516]: https://github.com/fighters-legacy/fighters-legacy/issues/516
[#517]: https://github.com/fighters-legacy/fighters-legacy/issues/517
[#520]: https://github.com/fighters-legacy/fighters-legacy/issues/520
[#566]: https://github.com/fighters-legacy/fighters-legacy/issues/566
[#569]: https://github.com/fighters-legacy/fighters-legacy/issues/569
[#572]: https://github.com/fighters-legacy/fighters-legacy/issues/572
[#573]: https://github.com/fighters-legacy/fighters-legacy/issues/573
[#574]: https://github.com/fighters-legacy/fighters-legacy/issues/574
[#575]: https://github.com/fighters-legacy/fighters-legacy/issues/575
[#588]: https://github.com/fighters-legacy/fighters-legacy/issues/588
[#590]: https://github.com/fighters-legacy/fighters-legacy/issues/590
[#725]: https://github.com/fighters-legacy/fighters-legacy/issues/725
[#726]: https://github.com/fighters-legacy/fighters-legacy/issues/726

## Problem and evidence

The [#514] governor's three levers (send-rate, byte budget, AI stride) all attack the Serialize
and AI phases. The entity-scale characterisation ([#573]) showed the Serialize phase is the
dominant cost at scale and the integrate pass is cheap: 1.038 ms at 5000 entities on a single
worker, parallelising further with worker count ([#511]). Extrapolating ~0.21 µs/entity, integrate
alone exhausts the 16.67 ms budget only around ~8×10⁴ entities serial, several × that at 8
workers. So today there is no integrate-bound overrun to shed — which is why this is a contingent
design, not an implementation.

The contingency is real, though: the Phase 4+ dynamic-world initiative ([#590]) wants persistent
mission-scale AI populations spread over a planet-sized world, most of it hundreds of kilometres
from any player. Those entities are exactly the ones whose full-fidelity 60 Hz aerodynamics buys
nothing — and exactly the ones the [#514] AI-stride lever already treats as decimatable.

## The integrator under a larger dt

Everything below is read from `engine/flight/FlightIntegrator.cpp`,
`engine/flight/FixedWingForceModel.cpp`, and `engine/flight/AeroForces.cpp`.

**The scheme is semi-implicit (symplectic) Euler throughout.** Step 11 updates `omega` from the
moments, step 12 updates `vel_body` from the forces, and only then step 13 integrates the
quaternion from the *updated* omega and step 14 integrates `pos_world` from the *updated*
velocity. Symplectic Euler is conditionally stable on oscillatory dynamics (stable for
`ω·dt < 2`, unlike explicit Euler which is unconditionally unstable on an undamped oscillator) —
this is what makes a moderate dt increase thinkable at all.

The stiff and dt-sensitive terms, in decreasing order of concern:

| Term | Where | Behaviour at larger dt |
|---|---|---|
| Short-period pitch mode (`cm_alpha` stiffness + `cm_q·(q·mac/2V)` damping) | `computeMoments` | The stiffest oscillator. ω_sp ≈ √(q_dyn·S·mac·abs(cm_alpha)/Iyy) ≈ 5–7 rad/s for a fighter at 300 m/s sea level, ~2× that at high dynamic pressure. Symplectic bound `ω·dt < 2` gives dt < ~150–300 ms in principle, but phase/amplitude error is large well before; at dt = 133 ms (8×), `ω·dt` reaches ~0.8–1.6 — accurate integration is gone even where formal stability holds. At dt ≤ 67 ms (4×), `ω·dt ≤ 0.8` at the extreme and ≤ 0.4 in the eligible envelope (see below) — acceptable. Roll (`cl_p`) and yaw (`cn_r`) damping are the same class, less stiff. |
| Quaternion small-angle integration | `integrateRotation` (`dq = 0.5·ω·dt`, renormalised) | Valid only for small `ω·dt`. At loiter/cruise rates (0.05–0.5 rad/s) even 133 ms gives ≤ 0.07 rad per step — fine. A hard-maneuvering entity (omega up to the `kMaxOmega = 50 rad/s` clamp) at stride 4 would take 3.3 rad per step — nonsense. Handled by the eligibility predicate's omega gate, not by the integrator. |
| Ground contact + parking hold | steps 14b/14c | Discrete per-step penetration check; larger dt tunnels deeper before the snap-back, and the friction/impact constants (the "~0.16 m/s/frame floor-tickle" comment) are tuned at 60 Hz. Also the contact margin is 0.5 m — a 10 m/s descent moves 1.3 m per 133 ms step, quantising contact detection. Handled by the eligibility predicate's AGL floor: entities near the ground are never reduced-rate. |
| First-order actuator lags (spool, sweep, TVC) | `advanceSpool` (explicit relaxation `x += (cmd−x)/τ·dt`), `advanceSweep`/`advanceTvc` (rate-limited, unconditionally stable) | The spool relaxation overshoots for dt > τ and oscillates for dt > 2τ. Typical spool times are seconds, so dt ≤ 133 ms is safe for sane content; a pack setting `spool_time_s = 0.05` would misbehave — the clamp to [0,1] bounds it. Not an eligibility concern at stride ≤ 4. |
| Aero forces ∝ airspeed² | `computeForces` (`q_dyn = ½ρV²`) | Not stiff by itself, but the alpha feedback loop (velocity → `atan2` alpha → lift/moment → velocity) sampled too coarsely reproduces the known AoA-saturation artifact (multi-step from degenerate states). Bounded by the `kMaxBodySpeed`/`kMaxOmega` clamps — the sim cannot NaN-spiral, but a clamped trajectory is garbage. Again an eligibility question: cruise/loiter states are near aero equilibrium. |
| Turbulence impulse | `stepFlightSim` (per-`(entityIdx, tickIndex)` seeded LCG; `Δv = turb·dt`) | Applied over dt_eff the velocity kick grows N× while firing N× less often: same mean drift, N× the variance — the gust spectrum audibly changes. Resolution: **suppress turbulence entirely for reduced-rate entities** (distant AI needs no buffeting; also removes any question of RNG cadence). Steady wind is unaffected (relative-airspeed model, dt-independent). |
| Fuel burn, gravity | steps 8/15 | Linear in dt; exact at any stride. |

**Stability ceiling: dt_eff ≤ 4× (66.7 ms).** 2× is comfortably inside every bound; 4× keeps
`ω·dt < ~0.5` across the *eligible* envelope (low omega, away from the ground, near trim) with the
clamps as a backstop; 8× crosses the small-angle validity for anything that starts maneuvering and
approaches the symplectic bound at high dynamic pressure. The stride ceiling is therefore capped
at 4 in the design, matching `overrun_max_ai_stride`'s default.

## Integration-scheme comparison

| Scheme | Sketch | Verdict |
|---|---|---|
| **Coalesced substepping** | step every Nth tick, running N × `step(1/60)` back-to-back | Physics-identical, zero stability risk — and zero CPU win: the same number of `step()` calls, only cache locality improves. Rejected as the primary scheme (it is, however, the correct *re-entry catch-up* form if an implementation ever needs one — see below). |
| **Full-stride integration** (recommended) | step every Nth tick with `dt_eff = (tick − lastSteppedTick) × simDt`; entity state frozen between steps | True ×N reduction of force-model + integrator work. No `FlightIntegrator` changes at all — the caller passes a larger dt, which the API already accepts. Position staleness between steps is ≤ (N−1) ticks ≈ 50 ms ≈ 15 m at 300 m/s — negligible against 10 km spatial cells and 200 km interest radii, and invisible by construction (eligible entities are beyond every peer's interest radius). Time accounting is exact across stride changes via `lastSteppedTick`. |
| **Split-rate kinematic coast** | evaluate forces/moments every Nth tick (dt_eff), advance pos/quat kinematically every tick (1/60) | Smooth per-tick trajectory and per-tick ground-contact checks — but requires cracking `step()` into force-eval and kinematic halves, keeps a per-tick per-entity cost (eroding the win), and the smoothness buys nothing when no peer can see the entity. Worth revisiting only if eligibility is ever loosened to *visible* entities. Rejected for now. |

**Pick: full-stride integration.** Entities are stepped on ticks where
`(tickIndex + idx) % stride == 0` — the same pure-function-of-`(idx, tick, stride)` phase
spreading the [#514] AI lever uses, so per-tick integrate cost is smoothed rather than bunched.
Each `ControlledEntity` records `lastSteppedTick`; the step uses
`dt_eff = (tickIndex − lastSteppedTick) × simDt`, which makes stride transitions (governor ramps,
re-entry) exactly time-conserving — no entity ever loses or double-counts sim time, and dt_eff is
automatically 1 tick on the first step after returning to full rate.

## Eligibility: who may be reduced-rate

The predicate must guarantee that reduced-rate state is never *observed* and never *unstable*.
Recommended predicate, evaluated **serially** (Maintenance phase) and frozen for the tick:

    lodEligible(e) =
        ce.decimatable                        // reuses the #514 flag: registerController() sets it
                                              // for AI/scripted entities; PeerController entities
                                              // (players) are created with decimatable = false
        && nearestPeerDist(e) > lodRadius     // lodRadius = physics_lod_radius_scale × configured
                                              // draw_distance_km (default 1.5 × 200 km = 300 km)
        && agl(e) > physics_lod_min_agl_m     // no ground contact / landing / terrain masking
                                              // (default 1000 m; uses the same groundQuery as
                                              // stepFlightSim)
        && |omega| < kLodMaxOmega             // ~1 rad/s: not mid-maneuver, keeps ω·dt_eff small
                                              // (read from FlightState — free)

Notes:

- **Players are never reduced-rate** — `decimatable == false` short-circuits, same as the AI
  lever. Their `FlightIntegrator` runs 60 Hz unconditionally.
- **The radius margin is the pop-prevention mechanism.** Because `lodRadius` is strictly greater
  than the interest radius (the [#402] XYZ gate at `drawDistM`), any entity a peer can possibly
  receive in a snapshot is full-rate. The coarse trajectory is *never on the wire*. The [#726]
  interest-radius governor lever only ever *shrinks* the effective draw distance, so scaling from
  the **configured** distance keeps eligibility a superset under overrun too.
- **`nearestPeerDist` is cheap if inverted**: rather than E×P distance checks, run one
  `SpatialIndex::queryRadius(peerPos, lodRadius)` per peer marking entities as near — the same
  shape as the interest queries the Serialize phase already runs. Cheaper still: recompute
  eligibility every ~30 ticks with hysteresis (engage at 1.5×, disengage at 1.25×); at Mach 2 an
  entity moves ~340 m in 30 ticks against a ≥ 50 km hysteresis band, so staleness is harmless.
- AI `sample()` of *other* far entities may read positions up to stride−1 ticks (≤ 50 ms) stale —
  the same staleness class as the sharding ghost analysis, far below anything guidance math at
  100+ km ranges can resolve.

## Re-entry

When the predicate flips (a player closes, the entity descends, or it starts maneuvering), the
entity simply steps on the next tick with `dt_eff = ticksSinceLastStep × simDt` and thereafter
every tick. There is no blend and no state fix-up: full-stride state is always a *valid physics
state* — the handoff is a cadence change, not a trajectory splice.

Wire-side, nothing new is needed:

- The [#516]/[#517] machinery already handles an entity entering a peer's interest set:
  `kSnapshotRetentionTicks` (180) force-fulls any entity not sent within the window, and the
  [#566] selective-ack confirms the full before deltas resume. Because the eligibility radius
  exceeds the interest radius, an entity is *already full-rate for ≥ (lodRadius −
  drawDist)/closing-speed seconds* before its first snapshot record — at 1.5× scale and a 600 m/s
  head-on closure, ≥ 2.7 minutes of full-rate integration precede visibility.
- Defense in depth even if the margin were violated: the client extrapolates cached entities by
  velocity (`SceneRenderer`), and a frozen-then-stepped position is exactly what constant-velocity
  extrapolation reconstructs — near-ballistic gaps are invisible.
- Client prediction is untouched **by construction**: `ClientPrediction` replays inputs only for
  the *player's own* entity (init'd with `playerIdx/playerGen`; `reconcile()` mutates only the
  player's entry), and players are never reduced-rate. Reduced-rate state reaches the client only
  through snapshots of other entities, whose cadence, quantization, and omega policy (own-record
  only) are unchanged. The 60 Hz replay assumption is not implicated.

## Determinism and serial-equivalence

The [#511]/[#512] guarantee — bit-identical transforms and per-peer packet streams across worker
counts — **holds under per-entity strides**, provided two rules the design already follows:

1. The step/skip decision is a pure function of frozen inputs: `(tickIndex + idx) % stride` with
   `stride` and the eligibility set computed serially in Maintenance and frozen for the parallel
   region — exactly how the [#514] levers are frozen today. No worker reads another entity's
   step decision.
2. All writes stay per-entity disjoint: `lastSteppedTick` joins `lastInput`/`lastInputValid` in
   `ControlledEntity` (each worker writes only its own entity's record), and a skipped entity is
   simply not written this tick.

What must be **explicitly relaxed** is a different equivalence: strided physics is *not*
trajectory-identical to unstrided physics (the whole point is a larger dt). That is the same class
of relaxation the governor already made — `TickGovernor.h` notes loadFactor float math is "disabled
in the serial-equivalence determinism tests". The enforcing tests are therefore:

- **Cross-worker-count equivalence WITH LOD engaged**: extend `test_world_broadcaster` to pin a
  fixed stride (governor forced to floor or a test hook) and assert bit-identical entity
  transforms + per-peer packet bytes for `workerCount ∈ {1, 4}`. This is the guarantee that
  matters and it must hold exactly.
- **Time conservation**: with a zero-gravity `IGravityField` stub and no weather, a
  constant-velocity entity's position after T ticks is bit-identical under any stride schedule
  (including mid-run stride changes) — proves the `lastSteppedTick` dt accounting drops no time.
- **Trajectory-error bound (not bit-equality)**: a loitering AI at stride 4 vs stride 1 for 3600
  ticks stays within an RMS position divergence bound (e.g. < 1% of loiter radius) with no
  NaN/clamp hits — the stability-envelope regression test.

## Governor-driven vs always-on

**Recommendation: governor-driven**, as the fourth `TickGovernor` lever
(`physicsStride() = clamp(round(1/loadFactor), 1, maxPhysicsStride)`, the `aiSampleStride`
mapping), *not* always-on.

- Always-on buys nothing today: integrate is ~1 ms at 5000 entities. Permanently lowering far-world
  fidelity for a phase that is not the bottleneck is a pure loss — and it would silently
  complicate every future consumer of far-entity state (replay [#588], honest sensing, the ~1 Hz
  agentic world snapshot).
- Governor-driven preserves the [#514] invariant that made the governor reviewable: **healthy /
  disabled / never-overrun ⇒ byte-for-byte pre-change behaviour** (`loadFactor == 1` ⇒ stride 1 ⇒
  every entity steps every tick with dt = 1/60). The determinism-test story above depends on it.
- The single-loadFactor AIMD deliberately engages all levers together; the *eligibility predicate*
  (never observed, never unstable), not engage ordering, is what bounds the fidelity cost. This
  keeps the lever consistent with the existing three rather than inventing a second controller.

Operator knobs follow the `[world] overrun_*` precedent in `server_config.h` (range-validated,
hot-reloadable via `reload_config`, mapped through `makeTickGovernorParams` into
`WorldBroadcasterConfig::governor`):

| Knob | Range | Default | Meaning |
|---|---|---|---|
| `overrun_max_physics_stride` | [1, 4] | 2 | deepest integrate decimation; 1 = lever disabled (ship-conservative: 2 already halves the eligible integrate cost; 4 is the stability-ceiling opt-in) |
| `physics_lod_radius_scale` | [1.1, 8.0] | 1.5 | eligibility distance = scale × configured `draw_distance_km`; the > 1 floor preserves the never-observed guarantee |
| `physics_lod_min_agl_m` | [0, 50000] | 1000 | AGL floor below which entities are never reduced-rate |

Observability mirrors the other levers: `physicsStride` joins `OverrunStatus` and the
`status`/`tickstats` output, and `ServerTickReport` gains a `physics_stride` field (schema bump)
so the overrun profile ([#574]) and scale gate ([#520]) can see it.

Interaction note ([#725]): the shared-origin encode builds each entity's snapshot blob from
`EntityState` every tick; a reduced-rate entity's state is unchanged between its steps, so its
delta blob is recomputed identically. Caching encodes for un-stepped entities is a possible
follow-on coupling — deliberately out of scope here (Serialize has its own ladder).

## Test plan

1. `test_tick_governor`: `physicsStride()` mapping, cap, disabled/healthy ⇒ 1 (mirror the
   `aiSampleStride` cases).
2. `test_world_broadcaster` `[overrun]`: stride engages at loadFactor floor and recovers to 1;
   eligibility gates (player never strided; near-peer entity never strided; low-AGL entity never
   strided; high-omega entity never strided); `lastSteppedTick` dt_eff across a stride change.
3. Cross-worker-count serial-equivalence with LOD pinned on (`workerCount ∈ {1, 4}`,
   bit-identical transforms + packet bytes) — the TSan `tsan.yml` targets already cover these
   test binaries.
4. Time-conservation and trajectory-error-bound tests as specified above (new
   `test_physics_lod` or folded into `test_world_broadcaster`).
5. `bot_swarm` overrun profile ([#574]) re-run on the 8-core reference env with
   `test_spawn_ai_count` large enough to integrate-bind, before/after — the acceptance
   measurement for the lever actually moving `integrate_ms` and `load_factor`.

## ⚠ Measurement caveat: the phase numbers here were taken on enet6 ([#649])

The [#572] measurements this document builds on were taken on **enet6**; the default internet
transport is **GameNetworkingSockets** ([#507]). On GNS the `serialize` phase collapses from 8.03 ms
to 0.81 ms (128 clients, weave, same box) because ~90 % of it was ENet's inline per-packet send, not
the snapshot pipeline. **`integrate` is unaffected** (0.06 → 0.05 ms) — it is transport-independent,
as it must be.

For *this* document that is mostly good news, and one thing to watch:

- The trigger's clause 3 is a **ratio** (`integrate_ms > 0.5 × tick_ms`). A cheaper serialize phase
  shrinks `tick_ms`, so the same integrate cost is a *larger fraction* of it — the ratio moves toward
  firing without integrate having gotten any more expensive. That is **not** a false alarm, because
  clauses 1 and 2 gate it: the ratio only routes an overrun that is *already happening*. It cannot
  fire on a healthy server. The router is sound; do not "fix" it by re-tuning 0.5.
- With the tick running ~16× under budget on GNS (1.0 ms vs 16.6 ms), clauses 1 and 2 are far from
  firing at all — so this remains correctly deferred.

Re-derive the absolute figures on GNS before acting ([#649] follow-up).

## Trigger criterion

All quantities come from `fl-server --metrics-json` (`ServerTickReport`) via the scale gate
([#520]) / overrun profile ([#574]) on the 8-core reference environment ([#569]) — the same
instruments as the [#572] trigger, with the phase test inverted:

> **Implement reduced-rate physics only when, on the 8-core reference environment at a
> product-target workload, with `sim_worker_threads = 8` and the overrun governor enabled, a
> sustained window (≥ 60 s) shows *all* of:**
>
> 1. `server_tick.load_factor` pinned at its configured floor — the existing three levers have
>    shed everything they can;
> 2. `server_tick.dropped_ticks` still rising monotonically — the `GameLoop` catch-up cap is
>    engaged despite the shedding;
> 3. `server_tick.integrate_ms.mean > 0.5 × server_tick.tick_ms.mean` — the tick is
>    **integrate-bound** (a serialize-dominant overrun routes to the [#572] ladder instead);
> 4. the workload is a *product* workload (mission/campaign AI population), not only the
>    `test_spawn_ai_count` synthetic — the affordance may be used to reproduce, not to justify.

No trigger, no implementation — like [#572], this document exists so the implementation starts
from a worked plan the day the trigger fires.

## Recommendation

**Conditional go.** Adopt the contingent design — full-stride integration with exact
`lastSteppedTick` time accounting, stride ceiling 4, turbulence suppressed at LOD, the
never-observed eligibility predicate (decimatable ∧ beyond `physics_lod_radius_scale ×`
draw-distance of every peer ∧ AGL ∧ omega gates, serially frozen), governor-driven as the fourth
`TickGovernor` lever with `overrun_*`-style knobs — but **do not implement now**. Integrate is
~0.21 µs/entity; the trigger above (integrate-bound overrun at governor floor on the reference
env, on a product workload) is the go signal. File the follow-on Feature issue from the sketch
below when it fires.

## Follow-on issue sketch

Title: `feat(engine): reduced-rate physics integration for distant AI under overrun`
(Feature, Epic A, references this document + [#575]; file only on trigger)

- `TickGovernor::physicsStride()` fourth lever: `clamp(round(1/loadFactor), 1, maxPhysicsStride)`;
  `TickGovernorParams::maxPhysicsStride` + `makeTickGovernorParams` plumbing.
- `ControlledEntity::lastSteppedTick`; integrate pass skips on
  `(tickIndex + idx) % stride != 0` for eligible entities; step uses
  `dt_eff = (tickIndex − lastSteppedTick) × simDt`; turbulence suppressed when `dt_eff > simDt`.
- Serial eligibility pass (Maintenance, every ~30 ticks with engage/disengage hysteresis):
  `decimatable` ∧ nearest-peer distance > `physics_lod_radius_scale × drawDist` (inverted
  per-peer `SpatialIndex::queryRadius` marking) ∧ AGL > `physics_lod_min_agl_m` ∧
  `|omega| < 1 rad/s`; frozen for the parallel region.
- `[world]` config: `overrun_max_physics_stride` [1,4] default 2, `physics_lod_radius_scale`
  [1.1,8] default 1.5, `physics_lod_min_agl_m` [0,50000] default 1000 — range-validated,
  hot-reloadable, documented in `fl-server-config.md` + default TOML template.
- Observability: `physicsStride` in `OverrunStatus`, `status`/`tickstats` lines,
  `ServerTickReport` `physics_stride` field (schema bump) + `bot_swarm`/`scale_gate.py` pass-through.
- Tests: governor mapping; eligibility gates; dt_eff across stride changes; cross-worker-count
  bit-equivalence with LOD pinned; zero-gravity time-conservation; loiter trajectory-error bound.
- Measurement: overrun-profile before/after on the 8-core reference env demonstrating
  `integrate_ms` reduction and `load_factor` recovery at the trigger workload; CHANGELOG + this
  document's status updated.
[#649]: https://github.com/fighters-legacy/fighters-legacy/issues/649
[#507]: https://github.com/fighters-legacy/fighters-legacy/issues/507
