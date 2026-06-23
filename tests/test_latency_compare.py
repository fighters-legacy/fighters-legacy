# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/latency_analysis/compare.py — pure-Python logic, no subprocess."""

import importlib.util
import json
import os
import sys
import types

import pytest

# ---------------------------------------------------------------------------
# Load compare module from its actual path (not installed as a package)
# ---------------------------------------------------------------------------

_SCRIPT = os.path.join(
    os.path.dirname(__file__), "..", "tools", "latency_analysis", "compare.py"
)


def _load_compare():
    spec = importlib.util.spec_from_file_location("compare", _SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


compare = _load_compare()

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

_BENCH_OUTPUT_TEMPLATE = (
    "--- bench results ---\n"
    "ENet RTT   samples={samples}  "
    "min={min:.2f}ms  mean={mean:.2f}ms  max={max:.2f}ms  "
    "p95={p95:.2f}ms  p99={p99:.2f}ms  stddev={stddev:.2f}ms\n"
    "Round dt   samples={samples}  "
    "min=15.00ms  mean=16.00ms  max=19.00ms  "
    "p95=17.00ms  p99=18.00ms  stddev=0.60ms\n"
    "---\n"
)


def _make_result(platform, timestamp, rtt_stats=None):
    """Build a JSON fixture dict as written by the measurement scripts."""
    bench_output = ""
    if rtt_stats:
        bench_output = _BENCH_OUTPUT_TEMPLATE.format(
            samples=rtt_stats.get("samples", 600),
            min=rtt_stats.get("min", 0.0),
            mean=rtt_stats.get("mean", 1.0),
            max=rtt_stats.get("max", 3.0),
            p95=rtt_stats.get("p95", 2.0),
            p99=rtt_stats.get("p99", 3.0),
            stddev=rtt_stats.get("stddev", 0.4),
        )
    return {
        "timestamp": timestamp,
        "platform": platform,
        "bench_samples": 600,
        "bench_rate_hz": 60,
        "icmp_summary": "",
        "bench_output": bench_output,
    }


def _write_fixture(tmp_path, filename, data):
    p = tmp_path / filename
    p.write_text(json.dumps(data), encoding="utf-8")
    return p


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_single_result(tmp_path, capsys):
    """One JSON fixture produces a table with the expected platform row."""
    data = _make_result(
        "linux",
        "20260624T120000Z",
        {"min": 0.0, "mean": 1.23, "max": 3.00, "p95": 2.0, "p99": 3.0, "stddev": 0.4},
    )
    _write_fixture(tmp_path, "linux_20260624T120000Z.json", data)

    rc = compare.main([str(tmp_path)])
    assert rc == 0

    out = capsys.readouterr().out
    assert "linux" in out
    assert "20260624T120000Z" in out
    assert "0.00 ms" in out   # min
    assert "1.23 ms" in out   # mean
    assert "3.00 ms" in out   # max (also p99 here)


def test_multi_platform(tmp_path, capsys):
    """Three fixtures (linux/macos/windows) all appear in the output."""
    fixtures = [
        ("linux",   "20260624T120000Z", {"min": 0.0, "mean": 1.0, "max": 3.0, "p95": 2.0, "p99": 3.0, "stddev": 0.4}),
        ("macos",   "20260624T130000Z", {"min": 0.0, "mean": 1.1, "max": 4.0, "p95": 2.5, "p99": 4.0, "stddev": 0.5}),
        ("windows", "20260624T140000Z", {"min": 1.0, "mean": 2.0, "max": 6.0, "p95": 4.0, "p99": 5.0, "stddev": 0.8}),
    ]
    for platform, ts, stats in fixtures:
        _write_fixture(tmp_path, f"{platform}_{ts}.json", _make_result(platform, ts, stats))

    rc = compare.main([str(tmp_path)])
    assert rc == 0

    out = capsys.readouterr().out
    for platform, ts, _ in fixtures:
        assert platform in out
        assert ts in out


def test_empty_results_dir(tmp_path, capsys):
    """Empty results directory prints a helpful message and returns 0."""
    rc = compare.main([str(tmp_path)])
    assert rc == 0

    out = capsys.readouterr().out
    assert "No results found" in out or "measure" in out.lower()


def test_missing_bench_output_field(tmp_path, capsys):
    """JSON fixture without bench_output shows N/A without crashing."""
    data = {
        "timestamp": "20260624T120000Z",
        "platform": "linux",
        "bench_samples": 600,
        "bench_rate_hz": 60,
        # bench_output intentionally absent
    }
    _write_fixture(tmp_path, "linux_20260624T120000Z.json", data)

    rc = compare.main([str(tmp_path)])
    assert rc == 0

    out = capsys.readouterr().out
    assert "N/A" in out
    assert "linux" in out
