# Dynamic World & Agentic AI — Architecture

Design record for the **Dynamic World & Agentic AI** cross-cutting initiative (Epics M–P;
[roadmap](roadmap.md), [decision record 2026-07-01](architecture.md#decision-records)). It covers
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
- **Model-size guidance** comes from the Epic M provider spike (#599); the initiative acceptance
  gate assumes a local 7–8B instruct model on the 8-core/16 GB reference instance.

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

## 4. MCP surface (Epic M, #601)

`fl-server` exposes a **Model Context Protocol** server so any agent runtime can operate it — the
same surface serves the campaign director, the ops agent, and interactive operator tooling.

- **Tool catalog:** `world_state` (snapshot read), `events` (stream tail), `admin_command`
  (allowlisted subset of the existing console command set — the #233 REST substrate; MCP and the
  Epic G web admin are two frontends over one command path), `submit_mission` (YAML → runs
  `validate-mission` server-side before acceptance).
- **Authz:** token-authenticated (reuses the operator-password/AuthTracker patterns); per-token
  autonomy tier; per-token command allowlist. Every invocation is audit-logged and mirrored into
  the event stream/replay.
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

### Epic P — agentic server operations (`fl-ops`, Go)

Consumes Epic G Prometheus metrics + structured logs (#546/#547); acts through the MCP surface
under the autonomy tiers. **Runbooks** are structured prompt+policy documents that declare their
allowed actions (tick-overrun triage from TickGovernor signals, congestion tuning, abuse/ban
review from AuthTracker summaries); anything outside the declaration is rejected by the policy
engine. Incident digests go to operator webhooks. LLM-assisted `fl-review` triage (#620)
summarizes and prioritizes — verdicts remain statistical/deterministic. Fleet-level scale/drain
recommendations integrate with the Epic K operator (#621).

## 6. Degradation matrix

| Feature | With provider | Without provider (CI-tested path) |
|---|---|---|
| Campaign | Director-generated missions, narrative briefings, adaptive OPFOR | Scripted/random campaign engine (#635), template briefings |
| Wingman commands | Natural language over chat/voice | Command menu + scripted grammar (#610) |
| GCI/AWACS | Conversational vectors/bogey-dope/picture | Templated calls or silence (operator-configurable) |
| Radio chatter | Event-driven generated lines | Canned line pools (fl-base-pack) |
| Server ops | Runbook triage, digests, recommendations | Dashboards + alerts only (Epic G) |
| fl-review | Ranked triage + evidence summaries | Raw statistical detector output |

## 7. Testing & CI policy (normative)

- **CI never requires a model.** Default CI runs zero LLM calls; ctest/go-test suites must pass on
  a runner with no network inference access.
- The no-provider fallback of every feature is the CI-tested path (unit + integration).
- Model-dependent metrics (intent accuracy ≥ 90 %, director ≤ 60 s validate-clean generation, ops
  triage correctness) are measured by **reusable eval harnesses produced by each epic's spike**,
  run locally or on the reference environment.
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
| Client-side intent models | CPU inference by default (avoid Vulkan GPU contention — measured in the O spike) | CPU/ANE | CPU |
| MCP/world-state sockets | WSAPoll, no SIGPIPE concern | poll + `SO_NOSIGPIPE` | poll + `MSG_NOSIGNAL` |

## 9. Latency & timescale budgets

| Workload | Budget | Notes |
|---|---|---|
| Sim tick with agents attached | p99 unchanged (≤ 16.6 ms at 128 clients) | Out-of-tick guarantee; Epic I gate |
| World-state snapshot assembly | ≤ 1 ms off-thread per publish | ~1 Hz cadence |
| Wingman intent mapping | ≤ 2 s utterance → acknowledged command | Human radio-comms timescale |
| GCI calls / chatter | 5–30 s cadence | Advisory only |
| Director mission generation | ≤ 60 s from campaign state | Between-mission timescale |
| Ops triage | ≤ 60 s from alert to recommendation | Digest delivery may batch |

## 10. References

- Model Context Protocol — <https://modelcontextprotocol.io> (spec + SDKs, MIT)
- Ollama — <https://github.com/ollama/ollama> (MIT)
- llama.cpp + `llama-server` (OpenAI-compatible) — <https://github.com/ggml-org/llama.cpp> (MIT)
- OpenAI API specification (the provider lingua franca) — <https://platform.openai.com/docs/api-reference>
- whisper.cpp — <https://github.com/ggml-org/whisper.cpp> (MIT)
- Piper TTS — <https://github.com/rhasspy/piper> (MIT)
- Prior art: the *Left 4 Dead* AI Director (scripted pacing director) — see
  [prior-art.md](prior-art.md#dynamic-ai)
