# ai_eval — local-provider evaluation harness (#599)

Measures **latency**, **structured-output reliability** and **task correctness** of any
OpenAI-compatible endpoint on the workloads of the Dynamic World & Agentic AI initiative
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
| `intent_asr` | O — conversational crew | the same task over **transcripts as speech-to-text actually produces them**: compounds split, initialisms spelled out, homophones substituted, filler retained. Deliberately separate from `intent` — folding them in would move the clean-speech numbers and make two models incomparable across an edit to the case file. The deterministic matcher (`WingmanPhraseMatch.h`) handles these with no model at all, so **a model that scores below the matcher here has no business on the voice path**. | 2 s |
| `injection` | M/O — chat path | **prompt-injection susceptibility**, and a model-adoption regression gate for anything put on the chat path (#611). Every case hides an instruction inside player-typed text; `injected_command` is what the attack was trying to elicit, and obedience is reported separately from ordinary wrongness so a sweep can rank models by susceptibility rather than only by accuracy. A pass does not make the model a security boundary — #611's grammar allowlist is. | 2 s |
| `mission` | N — campaign director | campaign brief → mission YAML, judged by the **real `validate-mission` binary** plus semantic checks. Scored `pass@1` and `pass-after-one-repair` (generate → validate → repair is the director's actual pipeline). | 60 s |
| `narrative` | N/M — briefing prose | **citation grounding**. Generated briefing/debrief prose must cite what it describes with `[[id]]` markers, and every citation is checked against the supplied context *deterministically* — no judge model, no similarity threshold, which is what makes it a regression gate rather than a vibe check. `must_cite` names the events a debrief cannot honestly omit. Catches prose that reads beautifully and refers to a sortie that never happened. | 20 s |
| `gci` | O — ground-controlled intercept | **call correctness against a known track picture**. A bearing, a range and a count are facts about the supplied picture, not opinions, so tolerance is the only judgement and it is declared in the suite file (`bearing_tol_deg`, `range_tol_nm`) rather than buried in the scorer. Bearing compares as shortest angular distance, so 359 and 001 are two degrees apart. | 20 s |
| `ops` | P — agentic server ops | `ServerTickReport`-shaped metrics snapshot → root cause + actions. Actions are checked against an **allowlist**: proposing a command outside the declared runbook is a failure, and a `healthy` case catches over-eagerness. Scored per runbook, and includes cases that separate **link congestion from server fault** — the two look alike in a snapshot and call for opposite responses. | 60 s |

Suites are data (`suites/*.json`) — adding cases is a file edit, not a code change. The wingman
grammar the `intent` suites score against is the shipped one from `engine/ai/WingmanCommand.h`;
`tests/test_ai_eval.py` asserts the two agree, so renaming a command there fails the test rather
than silently scoring every model against a vocabulary the engine no longer has.

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
