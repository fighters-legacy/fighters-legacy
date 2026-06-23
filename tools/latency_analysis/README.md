# Loopback Latency Analysis

Per-platform ENet loopback RTT measurement scripts for fighters-legacy.
See [docs/loopback-latency-analysis.md](../../docs/loopback-latency-analysis.md) for
the analysis methodology, decision record, and guidance on interpreting results.

## Prerequisites

Install the OS-level baseline tools before running (not required for the ENet bench itself):

| Platform        | Command                       |
|-----------------|-------------------------------|
| Fedora / RHEL   | `sudo dnf install sockperf`   |
| Ubuntu / Debian | `sudo apt install sockperf`   |
| macOS           | `brew install iperf3`         |
| Windows         | *(no extra tools required)*   |

See [docs/development.md](../../docs/development.md) — "Loopback latency analysis" section.

## Build first

```bash
cmake --build --preset debug        # Linux / macOS
cmake --build --preset debug-msvc   # Windows
```

## Run

```bash
# Fedora Linux (primary platform)
bash tools/latency_analysis/measure_linux.sh

# macOS
bash tools/latency_analysis/measure_macos.sh

# Windows (PowerShell)
.\tools\latency_analysis\measure_windows.ps1
```

Results are written to `tools/latency_analysis/results/` as timestamped JSON files.

## Compare across platforms

```bash
python3 tools/latency_analysis/compare.py
```

Prints a Markdown table of ENet RTT statistics (min / mean / max / p99) across all
result files. Copy the table into [docs/loopback-latency-analysis.md](../../docs/loopback-latency-analysis.md)
to update the baseline results.
