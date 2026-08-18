# Decision records

Resolutions of spikes and design investigations, kept as written.

**These are history, not guides.** Each one records what was decided, when, and on what evidence.
They are deliberately *not* maintained against the current code: a record that gets quietly edited
to match today's implementation stops being evidence of anything, and the reasoning it preserves —
including the options that were rejected and why — is the whole reason to keep it.

If you want to know how the engine behaves today, use the [Developer Guide](../index.md). If you
want to know why it behaves that way, read these.

| Record | Decision | Resolved |
|---|---|---|
| [Transport selection](transport-selection.md) | GameNetworkingSockets for scale, enet6 retained for LAN and single-player | #506 |
| [GNS backend notes](../gns-backend.md) | (living implementation notes, not a record — kept in the guide) | — |
| [Congestion control](congestion-control-design.md) | Per-client AIMD send-rate response | #518 |
| [Snapshot quantization](snapshot-quantization.md) | Bit-packed quantized entity records | #515 |
| [Server job system](server-job-system-design.md) | Data-parallel two-phase sim tick | Epic A |
| [Spatial sharding](spatial-sharding-design.md) | **Deferred**, behind a stated trigger criterion | #572 |
| [Physics LOD](physics-lod-design.md) | **Deferred**, behind an integrate-bound trigger criterion | #575 |
| [Entity scale characterization](entity-scale-characterization.md) | Measured pool and index scaling on the reference environment | #573 |
| [Turret hit registration](turret-hit-registration.md) | Client-favoured hit registration with anti-cheat bounds | #973 |
| [Loopback latency](loopback-latency-analysis.md) | Accept and compensate | #179 |
| [AI provider evaluation](ai-provider-evaluation.md) | Measured model-size guidance for the agentic surface | #599 |
| [Distribution strategy](distribution.md) | GPL-compatible monetization and channel strategy | — |
| [Base terrain hosting](base-terrain-hosting.md) | Built once by hand, published as a pinned release asset in its own repo | #1199 |

Two of these — spatial sharding and physics LOD — are **deferred with a trigger**, which means the
design is worked out and the decision was not to build it yet. Each names the measurement that
would change the answer. Check the trigger before reopening either.
