# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare ENet loopback latency results across platforms.

Reads all JSON result files produced by measure_linux.sh / measure_macos.sh /
measure_windows.ps1 from the given results directory (default: the 'results/'
subdirectory next to this script) and prints a Markdown comparison table.

Usage:
    python3 tools/latency_analysis/compare.py [results_dir]
"""

import json
import os
import re
import sys


def _parse_bench_output(text):
    """Extract ENet RTT stats from net_check --bench output lines.

    Looks for a line like:
        ENet RTT   samples=600  min=0.00ms  mean=1.23ms  max=3.00ms  ...
    Returns a dict with keys: samples, min, mean, max, p95, p99, stddev,
    or None if the line is not found.
    """
    pattern = re.compile(
        r"ENet RTT\s+samples=(\d+)\s+"
        r"min=([\d.]+)ms\s+mean=([\d.]+)ms\s+max=([\d.]+)ms\s+"
        r"p95=([\d.]+)ms\s+p99=([\d.]+)ms\s+stddev=([\d.]+)ms"
    )
    for line in text.splitlines():
        m = pattern.search(line)
        if m:
            return {
                "samples": int(m.group(1)),
                "min":     float(m.group(2)),
                "mean":    float(m.group(3)),
                "max":     float(m.group(4)),
                "p95":     float(m.group(5)),
                "p99":     float(m.group(6)),
                "stddev":  float(m.group(7)),
            }
    return None


def _load_results(results_dir):
    """Load all *.json files from results_dir. Returns list of dicts."""
    results = []
    if not os.path.isdir(results_dir):
        return results
    for fname in sorted(os.listdir(results_dir)):
        if not fname.endswith(".json"):
            continue
        path = os.path.join(results_dir, fname)
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
            results.append((fname, data))
        except (json.JSONDecodeError, OSError):
            pass
    return results


def _ms(value):
    return f"{value:.2f} ms"


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]

    script_dir = os.path.dirname(os.path.abspath(__file__))
    results_dir = argv[0] if argv else os.path.join(script_dir, "results")

    entries = _load_results(results_dir)
    if not entries:
        print(f"No results found in: {results_dir}")
        print("Run a measurement script first:")
        print("  bash tools/latency_analysis/measure_linux.sh")
        print("  bash tools/latency_analysis/measure_macos.sh")
        print("  .\\tools\\latency_analysis\\measure_windows.ps1")
        return 0

    rows = []
    for fname, data in entries:
        platform  = data.get("platform", "unknown")
        timestamp = data.get("timestamp", fname.removesuffix(".json"))
        bench_out = data.get("bench_output", "")
        stats = _parse_bench_output(bench_out) if bench_out else None
        rows.append((platform, timestamp, stats))

    # Header
    col_w = [10, 18, 9, 9, 9, 9]
    header = ["Platform", "Run", "RTT min", "RTT mean", "RTT max", "RTT p99"]
    sep = ["-" * w for w in col_w]

    def fmt_row(cols):
        return "| " + " | ".join(c.ljust(w) for c, w in zip(cols, col_w)) + " |"

    print(fmt_row(header))
    print(fmt_row(sep))
    for platform, timestamp, stats in rows:
        if stats:
            row = [
                platform,
                timestamp,
                _ms(stats["min"]),
                _ms(stats["mean"]),
                _ms(stats["max"]),
                _ms(stats["p99"]),
            ]
        else:
            row = [platform, timestamp, "N/A", "N/A", "N/A", "N/A"]
        print(fmt_row(row))

    return 0


if __name__ == "__main__":
    sys.exit(main())
