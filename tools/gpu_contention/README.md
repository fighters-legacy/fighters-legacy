<!--
SPDX-FileCopyrightText: 2026 MKZ Systems LLC
SPDX-License-Identifier: GPL-3.0-or-later
-->

# GPU contention harness (#782)

Measures what a **resident local LLM does to the renderer's frame time** — the question spike #609
asked on all three platforms and could only answer on Linux/CUDA, and the reason
`docs/ai-architecture.md` §8 carried an unmeasured risk next to its client-local-inference row.

The answer matters to one decision: whether intent inference could ever run **client-local**. #769
chose server-side hosting, which takes the LLM off the client's frame budget entirely. If that is
ever revisited, "how much does inference cost the frame" is the gate, and no amount of accuracy
data answers it.

## How it works

Three pieces, joined by wall-clock time:

1. **The game records frames.** `--frame-stats-json <path>` makes the client write one sample per
   rendered Flight frame — CPU frame ms, GPU ms (timestamp queries), and device-local VRAM usage
   and budget (`VK_EXT_memory_budget`) — each stamped with an epoch-millisecond timestamp.
   Implementation: `engine/perf/FrameStatsRecorder.h`, the render-side sibling of fl-server's
   `--metrics-json`.
2. **`driver.py` drives the model** on a phased schedule against any OpenAI-compatible endpoint,
   recording each phase's window on the same clock:

   ```
   |<--- idle 60s --->|<-burst 20s->|<-gap 20s->|<-burst 20s->| ... |<--- tail 30s --->|
        baseline          load          recover      load              back to baseline
   ```

3. **`analyze.py` joins them.** Every frame is attributed to the phase running when it rendered,
   and the report compares the burst distribution against the *same session's* idle baseline.

The epoch timestamps are the mechanism, not a detail. A monotonic clock's epoch is per-process, so
two processes' monotonic timestamps cannot be put on one timeline; wall-clock can. Burst windows
are tens of seconds long, so millisecond-scale clock agreement is far more precision than the join
needs.

Frames within 5 s of any phase boundary are discarded (`--settle-s`). A boundary is not
instantaneous for the renderer — the first requests are still being scheduled and the GPU is still
draining the previous phase — and attributing those frames to either side smears the difference
being measured.

## Running it

Each OS has a runner that does the whole sequence: preflight, system/VRAM probe, boot `fl-server`,
attach the game as a recording observer, drive the bursts, probe VRAM again, analyze.

```bash
# Linux
tools/gpu_contention/measure_linux.sh --model qwen2.5-coder:14b

# macOS (Metal)
tools/gpu_contention/measure_macos.sh --model qwen2.5-coder:14b

# Windows — run twice, once per inference backend (#782 names both)
.\tools\gpu_contention\measure_windows.ps1 -Model qwen2.5-coder:14b -Label cuda
.\tools\gpu_contention\measure_windows.ps1 -BaseUrl http://127.0.0.1:8081 -Model local -Label vulkan
```

**On Windows, address the endpoint as `127.0.0.1`, never `localhost` (#1021).** Both inference
servers bind IPv4 only, but `localhost` resolves to `::1` first; the IPv6 connect has to time out
before the client falls back, costing **~2.1 s of dead wait per request**. It never errors, so the
run looks clean — but the driver loops requests until a deadline, so the stall does not slow the
burst, it *empties* it: ~45 requests instead of ~350, leaving each burst ~85 % idle and comparing a
nearly-idle window against the idle baseline. Every Windows run before #1021 was measured this way.

Options are the same across all three (`--help` on the shell scripts). Defaults: `builtin:sandbox`
as the scene, the `intent` workload, 5 × 20 s bursts, one request in flight. Results land in
`tools/gpu_contention/results/` (git-ignored) as `frames_*.json`, `driver_*.json`,
`sysinfo_*.txt`, and the analyzed `<os>_<stamp>.{json,md}`. **`analyze.py` exits non-zero when it
emits warnings** — a run whose numbers are not trustworthy must not look like a clean pass.

### One run is not a measurement — use `--repeat` (#1016)

A single run gives one number per metric, which *looks* like a measurement. For the mean it nearly
is. For **p99 — the statistic the whole finding rests on — it is not**: re-running the identical
Vulkan cell on an RTX 5080 (same binary, model, scene, machine, minutes apart) moved the p99 ratio
from 1.49× to 2.48×. The run-to-run spread within one cell exceeded the gap between backends, so no
backend ordering could be read off single runs.

So for anything you intend to conclude from, repeat the cell and let the harness aggregate:

```bash
tools/gpu_contention/measure_linux.sh --model qwen2.5-coder:14b --label cuda --repeat 5
```

`--repeat N` boots the server once, runs the measurement N times against the same warm model, and
calls `aggregate.py` over the N reports. The aggregate reports each metric as **median [min–max]
across the runs**, so a cell's spread sits next to its value, and prints a p99-stability verdict —
whether the spread is small enough for the median to be a defensible point value, or large enough
that only the range is honest. It refuses to blend runs from different cells (different
model/GPU/OS/label), which would manufacture a spread out of real differences.

`aggregate.py` also runs standalone over any set of same-cell report JSONs:

```bash
python3 tools/gpu_contention/aggregate.py --reports results/windows_vulkan_*_run*.json
```

### On a vsync-pinned client, the divisor is part of the cell (#1019)

The first `--repeat 5` run (Intel iGPU, 60 Hz panel) returned an *unstable* verdict for a reason the
refuse-to-blend guard cannot see: the client could not hold 60 fps, so it sat on an integer divisor
of the refresh interval — 30 fps for four runs, 15 fps for one — and those are two operating points,
not one noisy one. Averaging across them manufactured the spread.

Two practical consequences when reading a `--repeat` aggregate from any vsync-limited client:

- **Check the idle frame interval per run before trusting the aggregate.** Each report records
  `idle_frame_interval_median_ms`, and the aggregate reports it as a row and **warns when the runs
  disagree by more than 20 %** — that is the divisor showing up. Aggregate the runs that share a
  cadence (`--reports` takes an explicit list for exactly this). Same-divisor runs from that machine
  agreed to within 0.05× on the p99 ratio.
- **Prefer Δ p95 to Δ p99 on a quantized frame clock.** Δ p95 held within 1.3 ms across all five
  runs *including* the odd one; Δ p99 ranged over 34 ms, because p99 lands wherever the next divisor
  is. On that machine the real effect was exactly one missed refresh interval.

  **That preference is conditional, not general, and the condition is not load (#1021).** On Windows
  under full burst load, p99 was the *stable* statistic on both backends (spreads 0.18× and 0.06×
  against 1.61× and 1.50× effects, both verdicts "defensible point value") while Δ p95 was the
  noisier one — the reverse of the iGPU cell above.

  The obvious explanation is that the p95 preference came from lightly-loaded cells, and it is wrong:
  **the Linux runs were at full load too** (373 and 540 requests/run, against 351 and 535 on the
  corrected Windows runs and 45 on the defective ones), and Linux still had noisy p99 with a cleanly
  separating Δ p95. Two OSes, one physical card, comparable load, opposite answers about which
  statistic is trustworthy. Whatever drives it is not how hard the model was driven.

  So do not carry a rule of thumb between platforms. **Read the harness's own per-cell stability
  verdict**, which is computed from that cell's actual spread, and quote the range when it says to.

### The hitch counter is relative to the run's own baseline (#1022)

A frame counts as a hitch when it exceeds **2× that run's median idle frame interval**, and the
threshold is calibrated once off the idle phase and then applied to every phase (calibrating per
phase would define the burst's hitches in terms of the burst's own slowness and could never show
contention at all). Both numbers are in the report as `hitch_threshold_ms` and
`idle_frame_interval_median_ms`, and the per-run markdown states them above the phase table.

This replaced a fixed `>33.3 ms` threshold, which was degenerate on any client whose ordinary cadence
already sat at or past it — on the #1019 iGPU it scored 7757 of 14200 *idle* frames as hitches. It
also means **hitch counts from before this change are not comparable** with ones after it, which is
why report `schema_version` went to 2 and `aggregate.py` refuses to blend the two.

`stalls_over_50ms` is deliberately still absolute: it is the band a player reads as a stutter rather
than as general slowness, which is a property of perception, not of the client's frame rate.

With no idle phase there is nothing to calibrate against, so the hitch metrics report `null` / `n/a`
rather than falling back to a fixed number — "could not measure" and "measured zero" are different
findings.

### Split the delta before interpreting it (#1025)

A frame time cannot tell you whether inference made the renderer's **work slower** or made it
**wait** — the two are the same number in the frame column and want opposite fixes. Every sample
also carries `gpu_ms`, the renderer's own timestamp-query span, so each frame splits:

    Δ frame  =  Δ renderer-GPU-execution  +  Δ residual        (residual = frame_ms − gpu_ms)

The report carries `residual_ms` per phase and `residual_mean/p95/p99_ms` + `gpu_p95/p99_ms` in the
delta block, and the per-run markdown prints them as a *Where the delta lives* table. A delta in the
GPU column is contention *inside* the renderer's execution; a delta in the residual is the renderer
not executing — CPU work, queue wait, or present.

Two companions decide between "waited" and "got slower" when the delta is in the residual:

- **`short_frames_pct`** — frames under half the idle cadence. A blocked frame's queued successor
  returns immediately, so a wait-then-drain *manufactures* short frames. Slower work cannot.
- **`drain_after_hitch_pct`** — of the frames over the run's hitch threshold, the share whose
  successor was one of those short frames. The unconditional share says both became common; this
  says they are the **same event**. `null` when the phase carries no hitch to condition on, which is
  not the same as zero.
- **`fps` / `fps_ratio`** — throughput. A genuine slowdown lowers it; a wait followed by a drain
  does not.

This is what resolved #1025 from artifacts already on disk instead of a tracing session: on Windows
the GPU column was +0.01 ms while the residual carried the whole +4.4 ms at p95, with throughput at
0.99× and catch-up frames up 15–27 points — the renderer waiting. On Linux the same split put the
delta in the GPU column with the residual *negative* — the renderer executing longer inside a frame
that had slack. Same card, opposite mechanisms.

The columns are additive **at the mean only**. Means are additive; percentiles are not, so the
p95/p99 rows localise the tail rather than forming an identity. The residual is nonetheless computed
per frame, which is what makes it more than a subtraction of two summaries. When the device does not
support timestamp queries `gpu_ms` is 0, the split degenerates to `frame == residual`, and the
markdown omits the table rather than presenting a missing measurement as a finding.

### Pin the model first

```bash
export OLLAMA_KEEP_ALIVE=-1     # or your server's equivalent
```

Ollama evicts an idle model after five minutes by default, and a cold 14B costs **~55 s** to load
on the reference instance (#769). An unpinned model reloads *inside* the first burst, and that
load is then measured as contention rather than as what it is. The driver's warm-up probe pays and
separately records the load (`model_load_probe_s`) precisely so it does not land in a burst.

### Choices the runners make, and why

- **Observer, not pilot.** A spectator ghost holds a fixed camera over a streamed-in scene, so the
  render load is repeatable. A pilot aircraft flies away, changes what is on screen, and eventually
  hits the ground — none of which is a controlled baseline.
- **Windowed, not `--headless`.** Present and the compositor are part of what contends for the GPU.
- **Release build.** Debug frame times are dominated by things this measurement is not about.
- **`intent` workload by default.** It is the real wingman workload and it is *prompt-eval
  dominated* (#769): the grammar is ingested every call and the answer is ~12 tokens. `--workload
  mission` gives the generation-dominated profile instead. These stress a GPU differently, and a
  synthetic prompt would represent neither.

## Reading the VRAM numbers

Two different measurements, and they are not interchangeable:

- **The game's usage/budget** comes from `VK_EXT_memory_budget` on device-local heaps. It is what
  the *renderer* occupies and what the driver says is available to it.
- **The model's own footprint** lives in the inference server's process, which the game's Vulkan
  view cannot see. `driver.py` reads it from Ollama's `/api/ps` (`size_vram`) where available; the
  runners also capture `nvidia-smi --query-compute-apps` (Linux/Windows NVIDIA) or the
  `\GPU Process Memory(*)\Dedicated Usage` counter (other Windows GPUs) into the sysinfo file.
  llama-server and LM Studio expose nothing equivalent — use the OS-level numbers there.

**macOS caveat.** Apple Silicon has unified memory: there is no discrete VRAM pool and no
per-process GPU memory API, so the model and the renderer compete for the same system RAM and the
ceiling is the machine's RAM, not a card's. What MoltenVK reports through `VK_EXT_memory_budget` is
a Metal heap's *recommended working set*, not a dedicated allocation — read it as a trend across
the run's phases, never as an absolute.

## CI

This never runs in CI, and **CI never requires a model** (`docs/ai-architecture.md` §7). Only the
pure logic — schedule arithmetic, the frame/phase join, the summary math — is unit-tested, in
`tests/test_gpu_contention.py`, with no network, GPU, model, or game binary.

## Recording results

Findings go in `docs/ai-provider-evaluation.md` under "GPU contention (#782)". Paste the analyzer's
Markdown, then add the run's row to the summary table.
