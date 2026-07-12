# ai_eval — local-provider evaluation harness (#599)

Measures **latency**, **structured-output reliability** and **task correctness** of any
OpenAI-compatible endpoint on the three workloads of the Dynamic World & Agentic AI initiative
(`docs/ai-architecture.md`). It is the reusable harness that spike #599 owes the follow-on epics,
and the instrument behind the model-size guidance in
[`docs/ai-provider-evaluation.md`](../../docs/ai-provider-evaluation.md).

**CI never requires a model.** This harness is a developer / reference-environment tool — it is not
wired into `ctest`, and nothing in the default CI path calls an inference endpoint. Only the pure
scoring logic is unit-tested (`tests/test_ai_eval.py`, no network).

## Suites

| Suite | Epic | What it measures | Budget |
|---|---|---|---|
| `intent` | O — conversational crew | free-text radio call → one wingman command, JSON. Includes out-of-grammar calls where **`unknown` is the correct answer** — an agent that maps an unsupported request onto a real command is worse than one that declines, because execution goes through the grammar unattended. | 2 s |
| `mission` | N — campaign director | campaign brief → mission YAML, judged by the **real `validate-mission` binary** plus semantic checks. Scored `pass@1` and `pass-after-one-repair` (generate → validate → repair is the director's actual pipeline). | 60 s |
| `ops` | P — agentic server ops | `ServerTickReport`-shaped metrics snapshot → root cause + actions. Actions are checked against an **allowlist**: proposing a command outside the declared runbook is a failure, and a `healthy` case catches over-eagerness. | 60 s |

Suites are data (`suites/*.json`) — adding cases, or re-pointing `intent` at the final wingman
grammar once [#610](https://github.com/fighters-legacy/fighters-legacy/issues/610) lands, is a file
edit, not a code change. The grammar shipped today is **provisional**; #610 owns the real one.

## Running

The mission suite shells out to `validate-mission`, so build it first:

    cmake --build --preset release --target validate-mission

Against a local Ollama directly (no key needed):

    python3 tools/ai_eval/ai_eval.py \
        --base-url http://localhost:11434 \
        --models qwen2.5-coder:14b \
        --suite all

Against LiteLLM (adds logging/Grafana visibility; the key is read from the environment, never a
flag — mirroring the `[ai.provider] api_key_env` seam):

    export FL_AI_API_KEY=<virtual-key>
    python3 tools/ai_eval/ai_eval.py \
        --base-url http://localhost:4000 \
        --api-key-env FL_AI_API_KEY \
        --models ollama/qwen2.5-coder-14b,ollama/gemma2-9b \
        --suite all --repeat 3

Useful flags: `--suite intent,ops` (subset), `--repeat N` (stability across runs), `--timeout`,
`--validate-mission <path>`, `--out <dir>`, `--merge-system` (below).

### `--merge-system` — models with no system role

Some chat templates have **no system turn**, and Ollama drops the message *silently* instead of
erroring. `gemma2` is the one that bites: served directly it never sees the system prompt, so it
never sees the command grammar, answers `unknown` to every case, and scores **35 %** when it is
really a 92 % model. Gateways hide this — LiteLLM merges the turn for gemma, which is why the same
model scores differently on two endpoints.

    python3 tools/ai_eval/ai_eval.py --models gemma2:9b --suite intent --merge-system

**If a model scores far below its reputation and the failures are a wall of identical "I don't know"
answers, suspect the prompt before you believe the score.**

Each run writes `results/ai-eval-<stamp>.json` (full per-case detail, including the raw response of
every failure) and `results/ai-eval-<stamp>.md` (the comparison table). `results/` is git-ignored.

## Interpreting the output

- **Parse** — a JSON object (or YAML document) could be recovered at all. Fenced output and leading
  prose are tolerated; this measures capability, not prompt obedience.
- **Schema** — the value is inside the declared enum/grammar. The gap between *parse* and *schema*
  is the hallucination rate, and it is the number that decides whether a model can be trusted behind
  a validated execution path.
- **Accuracy** — schema-valid **and** the expected answer.
- **p95 / in-budget** — against the per-workload budgets in `docs/ai-architecture.md` §9.
