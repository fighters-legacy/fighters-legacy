#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Aggregate several single-run contention reports into an across-run distribution (#1016).

A single `analyze.py` report is one number per metric, and the results table has one row per cell,
so a single run *looks* like a measurement. For the mean it nearly is. For **p99 — the statistic the
whole finding rests on — it is not**: on an RTX 5080, re-running the identical Vulkan cell (same
binary, model, scene, machine, minutes apart) moved the p99 ratio from 1.49× to 2.48×. The
run-to-run spread within one cell exceeded the gap between backends, so no backend ordering could be
read off single runs.

This tool takes N reports from the SAME cell and reports each headline metric as a distribution —
median and min–max across the runs — so a cell's spread sits next to its value. It refuses to blend
runs from different cells (different model/GPU/OS/label), which would manufacture a spread out of
real differences.

Pure and unit-tested (tests/test_gpu_contention.py); file I/O only in main(). Never in CI.

    python3 tools/gpu_contention/aggregate.py --reports results/windows_vulkan_*_run*.json
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path

# The headline metrics, all from a report's `delta_burst_vs_idle` block (burst minus idle baseline).
# Order is display order. `frame_p99_ratio` is the one the #1016 finding is about; `frame_mean_ms`
# is included precisely for the contrast — it is the metric that stays put.
DELTA_METRICS = [
    ("frame_p99_ratio", "Frame p99 ratio", "x"),
    ("frame_p99_ms", "Frame p99 delta", "ms"),
    ("frame_p95_ms", "Frame p95 delta", "ms"),
    ("frame_mean_ms", "Frame mean delta", "ms"),
    ("worst_1pct_ms", "Worst 1% delta", "ms"),
    ("gpu_mean_ms", "GPU mean delta", "ms"),
    ("hitches_per_min", "Hitches/min delta", ""),
]

# VRAM metrics (from the `vram` block) — usually stable across runs, reported for completeness.
VRAM_METRICS = [
    ("model_vram_mb", "Model VRAM", "MB"),
    ("budget_mb", "Game VRAM budget", "MB"),
]

# The cell-identity fields. Two reports with different values here are different cells and must not
# be aggregated together.
IDENTITY_FIELDS = ("os", "gpu_info", "model", "label")


# ---- pure aggregation --------------------------------------------------------------------------


def across_run_stats(values):
    """median / min / max / mean / spread over per-run values. Empty -> zeros, None entries dropped."""
    vals = [v for v in values if v is not None]
    if not vals:
        return {"median": None, "min": None, "max": None, "mean": None, "spread": None, "n": 0}
    return {
        "median": round(statistics.median(vals), 4),
        "min": round(min(vals), 4),
        "max": round(max(vals), 4),
        "mean": round(statistics.fmean(vals), 4),
        # Spread is max-min, not stddev: at n=5 the range is the honest, legible summary of "how
        # much did this wander", and it is what the #1016 verdict compares against the effect size.
        "spread": round(max(vals) - min(vals), 4),
        "n": len(vals),
    }


def identity_warnings(reports):
    """Refuse-to-blend guard: flag any cell-identity field that is not constant across the reports."""
    warns = []
    for field in IDENTITY_FIELDS:
        seen = sorted({str(r.get(field, "")) for r in reports})
        if len(seen) > 1:
            warns.append(
                f"Reports disagree on `{field}` ({', '.join(seen)}) — these are different cells and "
                "should not be aggregated into one distribution."
            )
    return warns


def p99_stability_verdict(ratio_stats, mean_stats):
    """The #1016 conclusion, computed: is a single run's p99 a measurement on this hardware?

    Compares the run-to-run p99-ratio spread against the EFFECT it is trying to resolve (how far the
    median ratio sits above 1.0). A spread that rivals or exceeds the effect means the number is
    noise dressed as signal — which is exactly what the Windows Vulkan runs showed.
    """
    if not ratio_stats or ratio_stats["n"] < 2 or ratio_stats["median"] is None:
        return {"level": "insufficient", "text": "Fewer than 2 runs — no spread to assess. Run more (#1016 asks for 5+)."}
    effect = ratio_stats["median"] - 1.0
    spread = ratio_stats["spread"]
    # effect can be ~0 (inference barely moved p99); guard the ratio.
    rel = spread / effect if effect > 0.01 else float("inf")
    mean_note = ""
    if mean_stats and mean_stats.get("spread") is not None:
        mean_note = f" (mean delta spread across the same runs: {mean_stats['spread']:.2f} ms)"
    if rel >= 1.0:
        level = "noise"
        text = (
            f"p99 run-to-run spread ({spread:.2f}×) EXCEEDS the effect it measures "
            f"(median {ratio_stats['median']:.2f}× = {effect:.2f}× above baseline). A single run's p99 "
            f"is not a measurement on this hardware — report the range.{mean_note}"
        )
    elif rel >= 0.5:
        level = "wide"
        text = (
            f"p99 spread ({spread:.2f}×) is a large fraction of the effect ({effect:.2f}× above "
            f"baseline) — quote the min–max, not a point.{mean_note}"
        )
    else:
        level = "stable"
        text = (
            f"p99 is stable across runs (spread {spread:.2f}× vs {effect:.2f}× effect); "
            f"the median is a defensible point value.{mean_note}"
        )
    return {"level": level, "text": text, "relative_spread": round(rel, 3) if rel != float("inf") else None}


def aggregate_reports(reports):
    """Fold N single-run reports (same cell) into an across-run distribution report."""
    if not reports:
        raise ValueError("no reports to aggregate")
    first = reports[0]

    deltas = {}
    for key, _label, _unit in DELTA_METRICS:
        deltas[key] = across_run_stats([r.get("delta_burst_vs_idle", {}).get(key) for r in reports])
    vram = {}
    for key, _label, _unit in VRAM_METRICS:
        vram[key] = across_run_stats([r.get("vram", {}).get(key) for r in reports])

    warns = identity_warnings(reports)
    # A run that itself warned (early exit, clock skew, dirty burst) should not silently dilute the
    # aggregate — surface the count so the reader knows some inputs were flagged.
    dirty = sum(1 for r in reports if r.get("warnings"))
    if dirty:
        warns.append(f"{dirty} of {len(reports)} input runs carried their own warnings; inspect those before trusting the aggregate.")

    verdict = p99_stability_verdict(deltas.get("frame_p99_ratio"), deltas.get("frame_mean_ms"))

    return {
        "schema_version": 1,
        "runs": len(reports),
        "os": first.get("os", ""),
        "gpu_info": first.get("gpu_info", ""),
        "model": first.get("model", ""),
        "label": first.get("label", ""),
        "endpoint": first.get("endpoint", ""),
        "workload": first.get("workload", ""),
        "scene": first.get("scene", ""),
        "delta_burst_vs_idle": deltas,
        "vram": vram,
        "p99_verdict": verdict,
        "warnings": warns,
    }


def _fmt(stat, unit):
    """A `median [min–max]` cell, unit-suffixed. Blank stat -> em dash."""
    if not stat or stat.get("median") is None:
        return "—"
    u = unit
    return f"{stat['median']:.2f}{u} [{stat['min']:.2f}–{stat['max']:.2f}]"


def render_aggregate_markdown(agg):
    """Markdown in the docs style: one row per metric, `median [min–max]` across the runs."""
    lines = []
    cell = f"{agg['label'] or agg['model']}"
    lines.append(f"### GPU contention (aggregate, {agg['runs']} runs) — {agg['os']}")
    lines.append("")
    lines.append(f"- GPU: `{agg['gpu_info'] or 'unknown'}`  scene: `{agg['scene'] or 'n/a'}`")
    lines.append(f"- Cell: `{cell}` — model `{agg['model']}` via `{agg['endpoint']}` (workload `{agg['workload']}`)")
    lines.append("")
    lines.append("| Metric | Median [min–max] across runs |")
    lines.append("|---|---:|")
    for key, label, unit in DELTA_METRICS:
        lines.append(f"| {label} | {_fmt(agg['delta_burst_vs_idle'].get(key), unit)} |")
    for key, label, unit in VRAM_METRICS:
        lines.append(f"| {label} | {_fmt(agg['vram'].get(key), unit)} |")
    lines.append("")

    v = agg["p99_verdict"]
    lines.append(f"**p99 stability:** {v['text']}")
    lines.append("")

    if agg["warnings"]:
        lines.append("**Warnings**")
        lines.append("")
        for w in agg["warnings"]:
            lines.append(f"- {w}")
        lines.append("")
    return "\n".join(lines)


# ---- main --------------------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(description="Aggregate repeated GPU-contention runs into a distribution (#1016)")
    ap.add_argument("--reports", nargs="+", required=True, help="analyze.py report JSONs from the SAME cell")
    ap.add_argument("--out", default="", help="output path prefix (writes <prefix>.json and <prefix>.md)")
    args = ap.parse_args(argv)

    reports = [json.loads(Path(p).read_text(encoding="utf-8")) for p in args.reports]
    agg = aggregate_reports(reports)
    md = render_aggregate_markdown(agg)
    print(md)

    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.with_suffix(".json").write_text(json.dumps(agg, indent=2) + "\n", encoding="utf-8")
        out.with_suffix(".md").write_text(md + "\n", encoding="utf-8")
        print(f"[gpu-contention] wrote {out.with_suffix('.json')} and {out.with_suffix('.md')}", file=sys.stderr)
    # A cross-cell mix or dirty inputs make the aggregate untrustworthy — do not exit 0 on it.
    return 1 if agg["warnings"] else 0


if __name__ == "__main__":
    sys.exit(main())
