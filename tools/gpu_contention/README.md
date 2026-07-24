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
.\tools\gpu_contention\measure_windows.ps1 -BaseUrl http://localhost:8080 -Model local -Label vulkan
```

Options are the same across all three (`--help` on the shell scripts). Defaults: `builtin:sandbox`
as the scene, the `intent` workload, 5 × 20 s bursts, one request in flight. Results land in
`tools/gpu_contention/results/` (git-ignored) as `frames_*.json`, `driver_*.json`,
`sysinfo_*.txt`, and the analyzed `<os>_<stamp>.{json,md}`. **`analyze.py` exits non-zero when it
emits warnings** — a run whose numbers are not trustworthy must not look like a clean pass.

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
