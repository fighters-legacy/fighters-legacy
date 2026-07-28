# Dynamic World & Agentic AI — Architecture

Design record for the **Dynamic World & Agentic AI** cross-cutting initiative (Epics M–P;
[roadmap](../roadmap.md), [decision record 2026-07-01](architecture.md#decision-records)). It covers
the provider seam, the agent-facing surfaces of `fl-server`, the four epics, and the normative
security, degradation, and testing rules every AI feature must satisfy.

| Epic | Issue | Theme |
|---|---|---|
| M | [#589](https://github.com/fighters-legacy/fighters-legacy/issues/589) | Agentic AI substrate — provider seam, world-state API, MCP surface |
| N | [#590](https://github.com/fighters-legacy/fighters-legacy/issues/590) | Dynamic campaign director — `fl-director` |
| O | [#591](https://github.com/fighters-legacy/fighters-legacy/issues/591) | Conversational crew & command — wingman NL, GCI/AWACS, radio chatter |
| P | [#592](https://github.com/fighters-legacy/fighters-legacy/issues/592) | Agentic server operations — `fl-ops` |

## 1. Principles (normative)

1. **LLMs never run in the 60 Hz tick.** Agents operate at human timescale (seconds to minutes)
   against a ~1 Hz world-state snapshot and an event stream, both assembled off the sim thread.
   Sim tick p99 must be unchanged with agents attached (validated under the Epic I load harness).
2. **Validated-paths-only actuation.** Agents act exclusively through surfaces that already
   validate their inputs: the admin/MCP command surface (server-side allowlist), mission YAML
   gated by `validate-mission`, and the `AiControllerFactory`/`StateMachineController` behaviour
   grammar. There is no direct state-mutation API for agents, by design.
3. **Graceful degradation is the default.** With no provider configured, every feature falls back
   to scripted behaviour, and that fallback is the CI-tested path. Intelligence is an amplifier,
   never a dependency.
4. **Tiered ops autonomy.** observe → recommend → act-with-allowlist. The act tier is limited to
   an operator-configured command allowlist; observe/recommend are provably side-effect-free.
5. **Player text is untrusted agent input.** Chat messages, callsigns, and server names entering a
   prompt are treated as data, never instructions (templated prompts, schema-validated outputs,
   grammar allowlists, audit logging).
6. **Generative AI never produces a shipped creative asset.** Art, music, story campaigns and voice
   packs are human-authored or CC0. Runtime-generated content (briefings, debriefs, chatter, TTS
   speech) is **ephemeral**: opt-in per server *and* per client, labeled, advertised by the server,
   and never baked back into a shipped pack. Contributors declare asset provenance in PRs. The
   distinction that decides any given case is the artifact's **lifetime**, not how it was made:
   a thing a player keeps is an asset, a thing that exists for one session is not.
   Ratified 2026-07-27 (#932); see the decision record in `architecture.md`.

## 2. Provider seam

Any **OpenAI-compatible HTTP endpoint**; configured under `[ai.provider]` in the fl-server config
(`base_url`, `model`, `api_key_env` — the key is read from the environment, never stored in the
TOML). The section is namespaced to avoid the existing `[ai]` difficulty-policy section. Absent
section = fully scripted behaviour.

- **Reference deployment is local:** Ollama or `llama-server` (llama.cpp). Operators may point at
  vLLM or a hosted API — never required. Self-host posture preserved.
- **Endpoint-only:** the engine and companion services never manage the inference-server
  lifecycle. No model files ship with the game.
- **Engine-side client is minimal:** a thin OpenAI-compatible client over `IHttpClient` (#490);
  no vendor SDK. Most inference consumers are the Go services, which talk to the endpoint
  directly.
- **Model-size guidance (measured, spike #599 — see [ai-provider-evaluation.md](decisions/ai-provider-evaluation.md)):**
  a **~9B–14B instruct model**, reference `qwen2.5-coder:14b`. 9B is the floor at which the workloads
  start working (96 % intent accuracy, 100 % validate-clean mission generation); below ~7B they
  collapse. This **revises the earlier "7–8B" assumption upward** — 8B is the edge of viable, not the
  comfortable default. Prefer an *instruct* model over a *reasoning* model wherever the output has a
  strict schema: reasoning traces leak into responses and cost schema validity. **Screen candidates
  for prompt-injection resistance** before using them on the chat path — one 9B model in the sweep
  obeyed an injected instruction embedded in a pilot utterance, and team chat is untrusted data.
  Latency budgets (§9) were met with wide margin on a GPU. **On the 8-core/16 GB CPU-only reference
  instance, two of the three fail** — quality is identical (accuracy belongs to the model, not the
  host), the models are simply too slow. **Intent:** no model is both ≥ 90 % accurate and inside 2 s
  (9B = 92 % at 4.8 s p95; 14B = 96 % at 3.3 s; the only model inside budget is a 3B at 81 %), so the
  natural-language wingman **requires a GPU-backed provider** and CPU-only servers degrade to the
  scripted wingman — decided in §9 (#769); the budget was kept, not relaxed. **Mission:** 9B/14B stay
  100 % validate-clean but take p95 71 s / 95 s against a 60 s budget — the director must generate
  ahead rather than block. **Ops** fits on CPU. See §9 and
  [ai-provider-evaluation.md](decisions/ai-provider-evaluation.md#cpu-only-reference-instance--the-box-the-acceptance-gate-names).
  **Deployment requirement:** pin the model in memory (`OLLAMA_KEEP_ALIVE` or equivalent) — a cold
  14B costs **55 s** to load on that box and idle models are evicted after 5 minutes by default.

## 3. World-state API & event stream (Epic M, #600)

A structured, read-only, out-of-band surface:

- **Snapshot (~1 Hz):** JSON document — entities (id/type/faction/position/velocity/damage),
  faction relationships and alert levels, active mission/objective state, weather, peer summary
  (count, per-peer latency class). Assembled off the sim thread from the same published state the
  render bridge uses; deterministic given a fixed entity set (golden-JSON schema-stability tests).
- **Event stream:** append-only match events — kills (with attribution), spawns/despawns,
  objective transitions, weather changes, chat (flagged untrusted), admin/agent commands (the
  audit trail).
- Consumers: MCP tools (below), `fl-director`, `fl-ops`, and the replay recorder (epic #588
  captures the same events, making agent behaviour reviewable offline).

> **Status (2026-07-26):** built, except for the network transport.
>
> - `WorldStateSnapshot` (`engine/net/WorldState.h`) now carries the faction table with per-faction
>   alert levels and the full relationship matrix, mission/objective state, and wind — the blocks this
>   section always specified but the #861 GM-map core did not yet need.
> - `MatchEventLog` (`engine/net/MatchEventLog.h`) is the append-only event stream: a bounded ring of
>   typed, tick-stamped records covering kills (with attribution and weapon class), spawns, damage
>   transitions, joins/leaves, chat, admin commands and alert-level changes. It reports `droppedCount`
>   and `hasGapBefore`, so a consumer that fell behind learns it has a gap instead of receiving a
>   partial history it believes is complete. `EntityEventType::Spawned` was added to raise the spawn
>   half — before #600 a spawn was observable nowhere.
> - JSON lives in `engine/net/WorldStateJson.h`, hand-rolled in the `ServerTickReport` style so
>   engine-net gains no JSON dependency. It **escapes strings**, unlike `MissionReport::toJson`, because
>   it carries chat lines and admin commands.
> - `WorldStatePublisher` is the off-thread handoff: the sim publishes an immutable snapshot and any
>   thread takes a `shared_ptr` to whatever was current, so a serializer never holds a lock across a
>   multi-thousand-entity document. `worldState()` remains the sim-thread-only fast path for the GM feed.
> - Reachable today through the `worldstate` and `events [after_seq] [max]` admin commands (console,
>   RCON, `MsgAdminCommand`). **The out-of-band HTTP surface is #233**, which fronts exactly these two
>   reads — REST is a frontend over this, not a second assembly path.
> - `test_world_state` and `test_match_event_log` join the TSan target set, per §7.

## 4. MCP surface (Epic M, #601)

> **Status: SHIPPED (v0.3.13).** Revision **`2025-06-18`**, pinned in
> `fl::mcp::kProtocolRevision`. Streamable HTTP on the #233 listener — `POST <path>` for calls,
> `GET <path>` for the notification stream. Tools `world_state`/`events` (observe),
> `submit_mission` (recommend) and `admin_command` (act, allowlisted). Config, the three
> authorization gates, and the audit story are documented in
> [`fl-server-config.md` `[ai.mcp]`](../server-ops/server-config.md).
>
> Two things worth stating because they are easy to assume otherwise: the autonomy tier is a
> **ceiling, not a bypass** — an `act` token still meets the #945 capability mask inside
> `CommandRegistry::dispatch`, so an `act`-tier `moderator` is refused `shutdown` — and
> `2025-06-18` **removed JSON-RPC batching**, so a batch is an explicit error rather than an
> unimplemented feature.

`fl-server` exposes a **Model Context Protocol** server — a **first-class operator and modding
surface**, not merely the agent door. It is the single command/read path behind the campaign
director, the ops agent, the game-master overview map (#861), the Epic G web admin (#550), the
fl-lobby listing (#143), and community spectator tooling (Sneaker-style web GCI falls out of the
same world-state resource). MCP standardised sharply after this design was first written (donated
to the Linux Foundation, Dec 2025); the surface below tracks the current spec.

- **Tool catalog:** `world_state` (snapshot read), `events` (stream tail), `admin_command`
  (allowlisted subset of the existing console command set — the #233 REST substrate; MCP and the
  Epic G web admin are two frontends over one command path), `submit_mission` (YAML → runs
  `validate-mission` server-side before acceptance).
- **Spec conformance:** pin a dated MCP protocol revision; **Streamable HTTP** transport (SSE-only
  is deprecated); **structured tool-output schemas** (the world-state snapshot already carries a
  golden-JSON schema — expose it as the tool's output schema); expose **world-state as an MCP
  resource with subscriptions** and the event stream as notifications (rather than only a polled
  tool); per-token **read-only default tier** + rate limiting.
- **Authz:** token-authenticated (reuses the operator-password/AuthTracker patterns); per-token
  autonomy tier (read-only default); per-token command allowlist. Every invocation is audit-logged
  and mirrored into the event stream/replay.
- **Config:** `[ai.mcp]` — `enabled` (default false), `bind`/`port`, `autonomy`
  (`observe|recommend|act`), `allowlist` (command names).
- Threat model coordinates with the Epic D anti-cheat threat model (#545). Sockets follow the
  RconServer cross-platform patterns (`WSAPoll`/`poll`, `MSG_NOSIGNAL`/`SO_NOSIGPIPE`, `#ifdef`s
  confined to the `.cpp`).

## 5. Epic designs

### Epic N — dynamic campaign director (`fl-director`, Go)

Consumes the world-state API + campaign/theater memory (single-session first; Epic H persistence
deepens it). Generates missions through **generate → `validate-mission` → repair → submit**;
writes narrative briefings/debriefs; adapts OPFOR composition by choosing among validated
unit/behaviour templates. Engine-side hooks: the `IWorldAiProvider` seam (#163) and the
deterministic `AlertSystem` (#162), both landing AI-free. The deterministic campaign engine
(#635, mission & campaign runtime epic #584) must run scripted/random standalone — the director
drives the same machinery through the same interfaces.

### Epic O — conversational crew & command

Text-first: team chat (#646) → provider maps free text to a structured intent → executed only
through the scripted wingman command grammar (#610, the zero-AI fallback satisfying the Phase 4
"six commands" acceptance). GCI/AWACS per coalition reads only its own side's track picture
(Epic F datalink #528 — no omniscience) and issues advisory calls. Ambient chatter is generated
from match events at low rate. Voice later: Epic J Opus capture (#531) + whisper.cpp STT +
optional Piper TTS; voice never bypasses the grammar.

**The natural-language path requires a GPU-backed provider (§9, #769).** Intent inference runs
**server-side, not client-local** (measured, #609): the models that clear the ≥ 90 % accuracy gate
are 9–14B, and a 55 s cold load against bursty, minutes-apart radio calls makes per-client hosting
the expensive way to do it — server-side also keeps the grammar allowlist on the authority that
enforces it, and keeps the LLM off the client's frame budget. On a CPU-only server the chat path is
not offered and Epic O ships its scripted half (#610), which stands on its own.

**Crew seats are Epic O's embodiment surface.** The multi-crew system (Epic #966) gives each
`ControlledEntity` a `CrewState` of capability-partitioned seats, one of which carries the `Command`
capability. That seat + the replicated crew roster are the structural hook Epic O binds to: a
conversational crewman (an AI-occupied `Command`/`Radar` seat, or NL orders issued *from* a
`Command` seat) is a seat bot whose `SeatCommand` output is produced by the provider-mapped intent
rather than a scripted controller — the same masked-merge tick composition, no new authority path.
The `Command` capability is reserved by #966 for exactly this and has no runtime consumer until
Epic O lands.

#### The scripted wingman (#610) — shipped, and the zero-AI path

The deterministic half of Epic O is built and is what a server runs with no provider configured. Its
vocabulary is six parameterless commands — `attack_my_target`, `engage_bandits`, `rejoin`,
`cover_me`, `hold_fire`, `return_to_base` — defined once in **`engine/ai/WingmanCommand.h`**, which
is the single source of truth: the radio menu, the admin console, the wire ordinal, and the eval
suite's grammar all derive from it (`tests/test_ai_eval.py` fails if the suite and the header drift).

Two properties matter to the LLM path that sits on top of it:

- **The grammar is the security boundary.** A provider maps free text to one of these ordinals, and
  the ordinal executes through the same code the menu drives. That is what bounds a successful prompt
  injection to *a real command at the wrong time* rather than arbitrary actuation. `unknown` — the
  mapper's decline sentinel — is deliberately **not** an executable command; `parseWingmanCommand`
  rejects it, so a model that correctly refuses an out-of-grammar utterance cannot accidentally order
  the wingman.
- **The grammar is also the latency lever (§9).** Intent latency on CPU is *prompt-eval dominated*,
  so the vocabulary's length is the cost. Keeping it a flat, parameterless enum keeps the prompt as
  short as it can be, makes constrained decoding a one-line alternation, and lets the eval score by
  string equality with no harness change. A command that needs a target does not get one from the
  model: `attack_my_target` is resolved **server-side** from the commander's boresight.

**Orders are issued against a formation, not "your wingman".** The underlying model is a command
*tree* (`engine/world/Formation.h`): a formation is `{anchor, commander, members, children}`, the
commander is a **role** rather than a seat (an AWACS or game master commands a flight it is not in,
and command cascades down to sub-formations), and a member is an **aircraft** — which may be AI (the
server retasks its controller) or **another player** (the server cannot retask a person, so the order
is *relayed* to them as a radio call and compliance is theirs). That distinction is visible on the
wire, and it is the honest model of a human wingman.

### Epic P — agentic server operations (`fl-ops`, Go)

Consumes Epic G Prometheus metrics + structured logs (#546/#547); acts through the MCP surface
under the autonomy tiers. **Runbooks** are structured prompt+policy documents that declare their
allowed actions (tick-overrun triage from TickGovernor signals, congestion tuning, abuse/ban
review from AuthTracker summaries); anything outside the declaration is rejected by the policy
engine. Incident digests go to operator webhooks. LLM-assisted `fl-review` triage (#620)
summarizes and prioritizes — verdicts remain statistical/deterministic. Fleet-level scale/drain
recommendations integrate with the Epic K operator (#621).

**Autonomy is capped at `recommend` until triage quality improves (measured, #599).** In the spike's
ops suite no local model exceeded 75 % root-cause accuracy, and the errors were systematic in both
directions — over-eager models recommended action against healthy servers, under-eager ones called
an active auth-abuse incident "healthy", and *every* model misread network congestion as either a
server fault or nothing at all. `act` is not justified by that data. The likely fix is better inputs
rather than a bigger model: derived/labelled signals instead of raw counters, few-shot examples, and
one runbook per failure class rather than a single open-ended triage prompt. The `ops` suite in
`tools/ai_eval/` is the regression test for that work. (Action-allowlist compliance *was* 100 % —
no model invented a command outside its declared runbook — but the policy engine remains mandatory:
that is one sweep on one endpoint.)

## 6. Degradation matrix

| Feature | With provider | Without provider (CI-tested path) |
|---|---|---|
| Campaign | Director-generated missions, narrative briefings, adaptive OPFOR | Scripted/random campaign engine (#635), template briefings |
| Wingman commands | Natural language over chat/voice | Command menu + scripted grammar (#610) |
| GCI/AWACS | Conversational vectors/bogey-dope/picture | Templated calls or silence (operator-configurable) |
| Radio chatter | Event-driven generated lines | Canned line pools (fl-base-pack) |
| Server ops | Runbook triage, digests, recommendations | Dashboards + alerts only (Epic G) |
| fl-review | Ranked triage + evidence summaries | Raw statistical detector output |

**A configured provider is not the same as a *fast enough* provider.** The right-hand column is also
the behaviour of a **CPU-only** deployment on the two rows with a real-time budget: the wingman NL
path needs a GPU to hold its 2 s budget and degrades to the scripted grammar without one (§9, #769),
and GCI/AWACS shares that path. The other rows have between-mission or advisory timescales and are
served by a CPU-only provider — the director simply has to generate ahead (§9).

## 7. Testing & CI policy (normative)

- **CI never requires a model.** Default CI runs zero LLM calls; ctest/go-test suites must pass on
  a runner with no network inference access.
- The no-provider fallback of every feature is the CI-tested path (unit + integration).
- Model-dependent metrics (intent accuracy ≥ 90 %, director ≤ 60 s validate-clean generation, ops
  triage correctness) are measured by **reusable eval harnesses produced by each epic's spike**,
  run locally or on the reference environment. The first of these is
  [`tools/ai_eval/`](https://github.com/fighters-legacy/fighters-legacy/blob/main/tools/ai_eval/README.md) (#599): one suite per workload (`intent`,
  `mission`, `ops`), scored against the real `validate-mission` binary and the declared
  grammars/allowlists. It is a developer tool — not wired into `ctest`; only its pure scoring logic
  is unit-tested (`tests/test_ai_eval.py`, zero network calls).
- Go services test their pipelines against a **fake provider** (canned completions) so
  generate/validate/repair and runbook logic are deterministic in `go test`; `validate-mission`
  is invoked as a subprocess fixture.
- Engine-side: world-state assembly gets golden-JSON schema-stability tests and joins the TSan
  target set (SDL/ENet-free); MCP framing/authz gets injectable-clock unit tests; the provider
  client gets `NullHttpClient`/`TrackingHttpClient` shared mocks (`tests/mock_http.h`).

## 8. Platform support matrix

| Concern | Windows | macOS | Linux |
|---|---|---|---|
| Local inference backends | CUDA / Vulkan (llama.cpp), Ollama | Metal (Apple Silicon — best-in-class local inference) | CUDA / ROCm / Vulkan, Ollama |
| Go services (`fl-director`/`fl-ops`) | native binary | native binary | native binary + container image |
| STT/TTS (Epic O voice) | whisper.cpp (CPU/CUDA); capture via WASAPI (SDL3) | whisper.cpp (Metal); CoreAudio | whisper.cpp (CPU/CUDA/ROCm); PipeWire |
| Intent inference host | Server-side (#609) — a **GPU-backed** provider; CPU-only servers ship the scripted wingman (#769) | same | same |
| MCP/world-state sockets | WSAPoll, no SIGPIPE concern | poll + `SO_NOSIGPIPE` | poll + `MSG_NOSIGNAL` |

**STT and TTS are CPU-real-time and sit outside the 9B/GPU model floor** (which governs *LLM*
inference only). This matters for the voice feature ladder: between the scripted radio menu and the
GPU-only LLM intent tier is a **deterministic voice-command tier** — push-to-talk whisper.cpp STT
fuzzy-matched onto the six-command `WingmanCommand.h` grammar, no LLM at all — that runs on *every*
server and is what the community bolts on manually (WhisperAttack). Piper TTS likewise voices
wingman acks, GCI, ATC clearances (deterministic template text — no LLM in ATC logic) and radio
chatter on CPU, preferring recorded content-pack lines where they exist and always with a subtitle
fallback. Only the *LLM intent tier* (#611) and *LLM-generated text* (director, narrative) are
gated by the 9B floor and the #769 GPU decision.

Client-local intent inference — the original assumption — is **not** the plan (#609), which is why
this table no longer carries a per-OS row for it. The question that row implied, *how much does
local inference contend with Vulkan for the GPU on each OS*, was parked in **#782** rather than
deleted, because it was a hardware-availability gap and not a settled question. It has since been
measured: the harness is `tools/gpu_contention/` and the per-OS results are in
`docs/developer/decisions/ai-provider-evaluation.md` ("GPU contention"). The measurement does not change the hosting
decision — that rests on accuracy, keep-warm cost and where the data already is — but it means a
player who *does* run a model locally is no longer doing so on an unmeasured assumption.

## 9. Latency & timescale budgets

| Workload | Budget | Notes |
|---|---|---|
| Sim tick with agents attached | p99 unchanged (≤ 16.6 ms at 128 clients) | Out-of-tick guarantee; Epic I gate |
| World-state snapshot assembly | ≤ 1 ms off-thread per publish | ~1 Hz cadence |
| Wingman intent mapping | ≤ 2 s utterance → acknowledged command | Human radio-comms timescale. **Held, and scoped to a GPU-backed provider** — see the decision below (#769) |
| GCI calls / chatter | 5–30 s cadence | Advisory only |
| Director mission generation | ≤ 60 s from campaign state | Between-mission timescale. **Not met on CPU** (p95 71 s at 9B, 95 s at 14B) — generate ahead of time |
| Ops triage | ≤ 60 s from alert to recommendation | Digest delivery may batch. Met on CPU (p95 ≤ 24 s) |

**These budgets hold on a GPU. On the CPU-only reference instance, two of the three fail** (spike
#599 follow-up, measured on the 8-core box). In both cases *quality is unaffected* — accuracy is a
property of the model, not the host — so this is purely a latency wall:

- **Intent (2 s) — the budget is kept and the feature is scoped to the hardware that can serve it
  (decision, #769).** The accurate models are 1.7–2.4× over budget on CPU (9B = 92 % at 4.8 s p95;
  14B = 96 % at 3.3 s); the only model inside budget is a 3B at 81 %. Latency is *prompt-eval*
  dominated — the cost is ingesting the command grammar, not generating the ~12-token answer — so
  the lever is a shorter prompt, not a faster decoder. See the decision record below.
- **Mission (60 s) — fails, but scheduling fixes it.** 9B and 14B still generate **100 %
  validate-clean** missions; they just take p95 71 s / 95 s. Mission generation is not inherently
  synchronous — a director that generates the *next* mission while the current one is flown hides
  this entirely. **Epic N must generate ahead, not block on a 60 s call.**

### Decision (#769) — the conversational wingman is a GPU feature

The 2 s budget is **not relaxed**. It exists because it is the human radio-comms timescale, and a
wingman that answers in five seconds is not a wingman — degrading the *number* would quietly degrade
the *feature* everywhere, including on the hardware where it works fine. Instead the feature is
scoped to the hardware that can serve it, and the hardware that cannot gets the fallback that
already exists:

- **A GPU-backed provider is a stated requirement of the natural-language wingman.** On CPU-only
  servers the chat path is not offered and the wingman degrades to the **scripted command menu and
  grammar (#610)** — already the zero-provider fallback, already the CI-tested path, and already
  sufficient for Phase 4's "six commands" acceptance on its own. This costs nothing to build. It has
  to be *stated*, so an operator knows what a GPU buys them and is not left diagnosing a wingman
  that merely feels slow. `fl-server` should say so at startup rather than let it be discovered:
  a provider configured for the intent path on a host that cannot meet the budget is a
  **misconfiguration to warn about**, not a performance characteristic to absorb.
- **Epic O's ≥ 90 % intent-accuracy gate is measured on a GPU-backed provider deployment.** It was
  always an accuracy gate; the CPU wall is a latency question and does not move it (9B/14B clear
  ≥ 90 % on *both* machine classes — see
  [ai-provider-evaluation.md](decisions/ai-provider-evaluation.md#cpu-only-reference-instance--the-box-the-acceptance-gate-names)).

**Bringing the LLM wingman back to CPU is a small-model-accuracy problem, not a big-model-speed
problem.** 3B is the only size that clears the budget (0.7 s p95) and it sits at 81 % — one command
in five wrong. Every lever that would close that gap acts on the *model and the prompt*, not the
host: a tighter grammar, few-shot examples, and constrained/grammar-guided decoding (which also
removes a whole class of schema-invalid answers outright).

**That work belongs to #610**, because the first lever *is* #610: latency is prompt-eval bound, so a
materially shorter grammar moves it directly — and #610 owns the real wingman vocabulary. The
`intent` suite in [`tools/ai_eval/`](https://github.com/fighters-legacy/fighters-legacy/blob/main/tools/ai_eval/README.md) is the regression test, and suites
are data — the re-run is a file edit, not new engineering.

**Status after #610 shipped the grammar:** the vocabulary is final (six flat, parameterless commands
in `engine/ai/WingmanCommand.h`), the suite is re-pointed at it, and the system prompt is down from
~176 to ~136 tokens — **a 23 % cut**. That is real movement on the dominant cost, but it is **not
enough on its own**: a 23 % shorter prompt does not turn a 3.3 s p95 into 2 s. **The remaining
levers — few-shot examples and constrained/grammar-guided decoding — are untried, and the number has
not been re-measured on the reference VM.** So the position is unchanged: **do not assume a CPU-only
server can serve a 2 s conversational loop.** If the remaining levers do not close the gap, that is a
real result — the GPU requirement above becomes the permanent answer rather than a provisional one.

**Prompt-injection resistance remains a model-selection criterion on this path** (§1, §2): the
grammar allowlist bounds the blast radius of a successful injection to a real command fired at the
wrong time, and it is load-bearing precisely because a capable 9B model in the sweep obeyed an
injected instruction. Anything done to make a *small* model accurate must not be assumed to preserve
that — re-screen it.

**Deployment requirement, whichever path wins:** pin the model in memory (`OLLAMA_KEEP_ALIVE` or
equivalent). A cold 14B costs **55 s** to load on that box, and idle models are evicted after 5
minutes by default — the first command after a lull would miss any budget on model load alone.

## 10. References

- Model Context Protocol — <https://modelcontextprotocol.io> (spec + SDKs, MIT)
- Ollama — <https://github.com/ollama/ollama> (MIT)
- llama.cpp + `llama-server` (OpenAI-compatible) — <https://github.com/ggml-org/llama.cpp> (MIT)
- OpenAI API specification (the provider lingua franca) — <https://platform.openai.com/docs/api-reference>
- whisper.cpp — <https://github.com/ggml-org/whisper.cpp> (MIT)
- Piper TTS — <https://github.com/rhasspy/piper> (MIT)
- Prior art: the *Left 4 Dead* AI Director (scripted pacing director) — see
  [prior-art.md](prior-art.md#dynamic-ai)
