# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Scale-gate driver for the bot_swarm load harness (issue #520).

Reads a threshold *profile* from ``scale-gate.json``, runs ``run_loadtest.sh`` / ``run_loadtest.ps1``
once per flight pattern with the profile's ``--assert-*`` flags wired in, evaluates each resulting
JSON report, compares the machine-independent ``downstream_kbs_per_client`` metric against a
committed baseline, prints a Markdown summary (to ``$GITHUB_STEP_SUMMARY`` when set, else stdout),
and exits nonzero on any runner failure or baseline regression.

Division of responsibility (see docs/load-testing.md):
  * Absolute ceilings (max-kbs / max-tick-ms / min-tick-hz) are enforced by ``bot_swarm`` itself via
    the ``--assert-*`` flags this driver forwards — that stays the single source of truth and is
    unit-tested in tests/test_bot_swarm.cpp.
  * This driver adds the *relative* KB/s-per-client baseline-regression check and the human summary.

Pure logic (profile loading, flag assembly, report evaluation, baseline diff, summary rendering) is
factored into small functions and unit-tested in tests/test_scale_gate.py without sockets or binaries.
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_CONFIG = SCRIPT_DIR / "scale-gate.json"
DEFAULT_BASELINE = SCRIPT_DIR / "scale-gate-baseline.json"
RESULTS_DIR = SCRIPT_DIR / "results"

# Keys every profile carries, with defaults applied when absent.
PROFILE_DEFAULTS = {
    "clients": 64,
    "duration_s": 30,
    "patterns": ["weave"],
    "sim_worker_threads": 0,
    "assert_max_kbs": 0.0,
    "assert_min_tick_hz": 0.0,
    "assert_max_tick_ms": 0.0,
    # Soak leak gate (#707): max allowed RSS growth over the run (rss_kb - rss_startup_kb). 0 = disabled.
    "assert_max_rss_growth_kb": 0,
    # Overrun-governor gate (#574). Both use a NEGATIVE-disabled sentinel (0 is a real value for each).
    # governor=true flips FL_LOADTEST_GOVERNOR=1 so the run exercises the governor ON.
    "assert_max_load_factor": -1.0,
    "assert_max_dropped_ticks": -1,
    "governor": False,
    # Congestion gate (#714): the bot_swarm lossy-proxy degrade window (forwarded as --degrade-*
    # flags; degrade_duration_s 0 = proxy off) plus the engaged/recovered watermark asserts
    # (0 = disabled — a 0 Hz threshold is meaningless for either, so no negative sentinel).
    "degrade_start_s": 0.0,
    "degrade_duration_s": 0.0,
    "degrade_loss": 0.0,
    "degrade_delay_ms": 0,
    "assert_congestion_engaged_hz": 0.0,
    "assert_congestion_recovered_hz": 0.0,
    # Entity-scale sweep (#573). Empty lists => a normal one-run-per-pattern profile. When populated,
    # the profile sweeps the cartesian product of patterns x entity_spawn_counts x
    # sim_worker_threads_sweep, driving FL_TEST_SPAWN_AI / FL_SIM_WORKER_THREADS per run.
    "entity_spawn_counts": [],
    "sim_worker_threads_sweep": [],
    # Heavier AI mix + projectile churn (#580): ai_mix is the weighted controller mix for the
    # pre-spawned entities ("loiter:70,pursuit:20,patrol:10"; "" = all loiter, the #573 baseline);
    # projectile_rate/_ttl_s drive the spawn/reap churn generator (0 = disabled). Forwarded per run
    # as FL_TEST_SPAWN_MIX / FL_TEST_PROJECTILE_RATE / FL_TEST_PROJECTILE_TTL_S.
    "ai_mix": "",
    "projectile_rate": 0,
    "projectile_ttl_s": 3.0,
    # entity-scale is advisory characterisation, not a bandwidth gate: it must NOT read or write the
    # committed downstream_kbs_per_client baseline (its sweep collapses many KB/s values onto one key).
    "baselined": True,
}


# --------------------------------------------------------------------------------------------------
# Pure logic (unit-tested)
# --------------------------------------------------------------------------------------------------
def load_config(config_path):
    """Load the gate config JSON. Raises ValueError on a missing/garbled file."""
    try:
        with open(config_path, encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError as e:
        raise ValueError(f"config not found: {config_path}") from e
    except json.JSONDecodeError as e:
        raise ValueError(f"config is not valid JSON: {config_path}: {e}") from e


def load_profile(config, name):
    """Return the named profile with PROFILE_DEFAULTS filled in. Raises ValueError if unknown."""
    profiles = config.get("profiles", {})
    if name not in profiles:
        known = ", ".join(sorted(profiles)) or "(none)"
        raise ValueError(f"unknown profile '{name}'; known: {known}")
    merged = dict(PROFILE_DEFAULTS)
    merged.update({k: v for k, v in profiles[name].items() if not k.startswith("_")})
    return merged


def assert_flags(profile, strict):
    """Assemble the bot_swarm --assert-* flags for a profile.

    A threshold of 0 means "disabled" and the flag is omitted. ``assert_max_tick_ms`` is only
    emitted when ``strict`` is true — on shared CI runners the CPU-timing gate is advisory.
    """
    flags = []
    if profile["assert_max_kbs"] > 0:
        flags += ["--assert-max-kbs", _num(profile["assert_max_kbs"])]
    if profile["assert_min_tick_hz"] > 0:
        flags += ["--assert-min-tick-hz", _num(profile["assert_min_tick_hz"])]
    if strict and profile["assert_max_tick_ms"] > 0:
        flags += ["--assert-max-tick-ms", _num(profile["assert_max_tick_ms"])]
    if profile["assert_max_rss_growth_kb"] > 0:
        flags += ["--assert-max-rss-growth-kb", _num(profile["assert_max_rss_growth_kb"])]
    # Overrun-governor gate (#574): negative-disabled, so >= 0 enables the flag (0 is a real value).
    if profile["assert_max_load_factor"] >= 0:
        flags += ["--assert-max-load-factor", _num(profile["assert_max_load_factor"])]
    if profile["assert_max_dropped_ticks"] >= 0:
        flags += ["--assert-max-dropped-ticks", str(int(profile["assert_max_dropped_ticks"]))]
    # Congestion gate (#714).
    if profile["assert_congestion_engaged_hz"] > 0:
        flags += ["--assert-congestion-engaged-hz", _num(profile["assert_congestion_engaged_hz"])]
    if profile["assert_congestion_recovered_hz"] > 0:
        flags += ["--assert-congestion-recovered-hz", _num(profile["assert_congestion_recovered_hz"])]
    return flags


def degrade_flags(profile):
    """Assemble the bot_swarm lossy-proxy --degrade-* flags (#714). Empty when the profile has no
    degrade window (degrade_duration_s 0), so every other profile runs proxy-free."""
    if profile["degrade_duration_s"] <= 0:
        return []
    flags = ["--degrade-duration", _num(profile["degrade_duration_s"]),
             "--degrade-start", _num(profile["degrade_start_s"])]
    if profile["degrade_loss"] > 0:
        flags += ["--degrade-loss", _num(profile["degrade_loss"])]
    if profile["degrade_delay_ms"] > 0:
        flags += ["--degrade-delay-ms", str(int(profile["degrade_delay_ms"]))]
    return flags


def _num(x):
    """Render a threshold as a compact string (ints without a trailing .0)."""
    f = float(x)
    return str(int(f)) if f.is_integer() else repr(f)


def evaluate_report(report, profile, strict):
    """Evaluate a bot_swarm report dict against a profile. Returns {passed: bool, checks: [...]}.

    Mirrors bot_swarm's own gate so the summary explains *why* a run passed/failed. The runner's
    exit code remains authoritative for the absolute asserts; this is the human-readable view plus
    the admission check. tick-ms is reported but only counts toward pass/fail when ``strict``.
    """
    checks = []

    requested = report.get("clients_requested", 0)
    connected = report.get("clients_connected", 0)
    disconnected = report.get("clients_disconnected", 0)
    admission_ok = connected == requested and disconnected == 0
    checks.append({
        "name": "admission",
        "ok": admission_ok,
        "detail": f"connected={connected}/{requested} disconnected={disconnected}",
        "advisory": False,
    })

    kbs_max = report.get("downstream_kbs_per_client", {}).get("max", 0.0)
    if profile["assert_max_kbs"] > 0:
        ok = kbs_max <= profile["assert_max_kbs"]
        checks.append({
            "name": "downstream_kbs_per_client.max",
            "ok": ok,
            "detail": f"{kbs_max:.1f} <= {profile['assert_max_kbs']:.1f} KB/s",
            "advisory": False,
        })

    tick_hz_min = report.get("observed_server_tick_hz", {}).get("min", 0.0)
    if profile["assert_min_tick_hz"] > 0:
        ok = tick_hz_min >= profile["assert_min_tick_hz"]
        checks.append({
            "name": "observed_server_tick_hz.min",
            "ok": ok,
            "detail": f"{tick_hz_min:.1f} >= {profile['assert_min_tick_hz']:.1f} Hz",
            "advisory": False,
        })

    if profile["assert_max_tick_ms"] > 0:
        server = report.get("server_tick")
        if server is None:
            ok = False
            detail = "no server_tick block (cannot evaluate)"
        else:
            p99 = server.get("tick_ms", {}).get("p99", 0.0)
            ok = p99 <= profile["assert_max_tick_ms"]
            detail = f"{p99:.2f} <= {profile['assert_max_tick_ms']:.2f} ms"
        checks.append({
            "name": "server_tick.tick_ms.p99",
            "ok": ok,
            "detail": detail + ("" if strict else " (advisory)"),
            "advisory": not strict,
        })

    if profile["assert_max_rss_growth_kb"] > 0:
        server = report.get("server_tick")
        if server is None:
            ok = False
            detail = "no server_tick block (cannot evaluate)"
        else:
            growth = server.get("rss_kb", 0) - server.get("rss_startup_kb", 0)
            ok = growth <= profile["assert_max_rss_growth_kb"]
            detail = f"{growth} <= {profile['assert_max_rss_growth_kb']} KiB growth"
        checks.append({
            "name": "server_tick.rss_growth_kb",
            "ok": ok,
            "detail": detail,
            "advisory": False,
        })

    # Overrun governor engaged (#574): load_factor <= threshold means it shed; > means it never fired.
    if profile["assert_max_load_factor"] >= 0:
        server = report.get("server_tick")
        if server is None:
            ok = False
            detail = "no server_tick block (cannot evaluate)"
        else:
            lf = server.get("load_factor", 1.0)
            ok = lf <= profile["assert_max_load_factor"]
            detail = f"{lf:.3f} <= {profile['assert_max_load_factor']:.3f} (governor engaged)"
        checks.append({
            "name": "server_tick.load_factor",
            "ok": ok,
            "detail": detail,
            "advisory": False,
        })

    # Graceful-not-spiral (#574): the governor should keep GameLoop catch-up drops at ~0 under overload.
    if profile["assert_max_dropped_ticks"] >= 0:
        server = report.get("server_tick")
        if server is None:
            ok = False
            detail = "no server_tick block (cannot evaluate)"
        else:
            dropped = server.get("dropped_ticks", 0)
            ok = dropped <= profile["assert_max_dropped_ticks"]
            detail = f"{dropped} <= {profile['assert_max_dropped_ticks']} dropped ticks"
        checks.append({
            "name": "server_tick.dropped_ticks",
            "ok": ok,
            "detail": detail,
            "advisory": False,
        })

    # Congestion controller engaged (#714): the run-long min send rate must have fallen to the
    # threshold or below (stuck at 60 = the controller never responded to the degraded link).
    if profile["assert_congestion_engaged_hz"] > 0:
        server = report.get("server_tick")
        if server is None:
            ok = False
            detail = "no server_tick block (cannot evaluate)"
        else:
            min_hz = server.get("congestion_min_send_hz", 60.0)
            ok = min_hz <= profile["assert_congestion_engaged_hz"]
            detail = f"{min_hz:.1f} <= {profile['assert_congestion_engaged_hz']:.1f} Hz (engaged)"
        checks.append({
            "name": "server_tick.congestion_min_send_hz",
            "ok": ok,
            "detail": detail,
            "advisory": False,
        })

    # Congestion controller recovered (#714): after the min, the send rate must have climbed back.
    if profile["assert_congestion_recovered_hz"] > 0:
        server = report.get("server_tick")
        if server is None:
            ok = False
            detail = "no server_tick block (cannot evaluate)"
        else:
            rec_hz = server.get("congestion_recovered_send_hz", 60.0)
            ok = rec_hz >= profile["assert_congestion_recovered_hz"]
            detail = f"{rec_hz:.1f} >= {profile['assert_congestion_recovered_hz']:.1f} Hz (recovered)"
        checks.append({
            "name": "server_tick.congestion_recovered_send_hz",
            "ok": ok,
            "detail": detail,
            "advisory": False,
        })

    passed = all(c["ok"] for c in checks if not c["advisory"])
    return {"passed": passed, "checks": checks}


def compare_baseline(report, baseline_entry, tolerance_pct):
    """Compare a report's downstream KB/s/client mean against a baseline value.

    Returns {regressed: bool, detail: str}. A missing baseline entry (None) is a no-op (not a
    regression) — newly added (profile, pattern) pairs simply have no baseline yet. Only increases
    beyond the tolerance band count as a regression; improvements never fail.
    """
    if baseline_entry is None:
        return {"regressed": False, "detail": "no baseline (skipped)"}
    current = report.get("downstream_kbs_per_client", {}).get("mean", 0.0)
    limit = baseline_entry * (1.0 + tolerance_pct / 100.0)
    regressed = current > limit
    return {
        "regressed": regressed,
        "detail": f"{current:.1f} vs baseline {baseline_entry:.1f} KB/s "
                  f"(+{tolerance_pct:.0f}% = {limit:.1f})",
    }


def baseline_key(profile_name, pattern):
    return f"{profile_name}/{pattern}"


def expand_runs(profile):
    """Expand a profile into a list of run specs. Pure (no I/O).

    Normal profile: one run per pattern, no extra env, participates in the KB/s baseline.
    Entity-scale sweep (entity_spawn_counts / sim_worker_threads_sweep non-empty): the cartesian
    product of patterns x counts x workers, each carrying FL_TEST_SPAWN_AI / FL_SIM_WORKER_THREADS
    and a per-run --assert-min-entities flag. Labels are unique so report files never collide.
    """
    counts = profile.get("entity_spawn_counts") or [None]
    workers = profile.get("sim_worker_threads_sweep") or [None]
    # Overrun profile (#574): flip the governor ON for every run in the profile.
    profile_env = {"FL_LOADTEST_GOVERNOR": "1"} if profile.get("governor") else {}
    # AI mix + projectile churn (#580): profile-constant, applied to every run in the sweep.
    if profile.get("ai_mix"):
        profile_env["FL_TEST_SPAWN_MIX"] = profile["ai_mix"]
    if profile.get("projectile_rate"):
        profile_env["FL_TEST_PROJECTILE_RATE"] = str(profile["projectile_rate"])
        profile_env["FL_TEST_PROJECTILE_TTL_S"] = str(profile["projectile_ttl_s"])
    runs = []
    for pattern in profile["patterns"]:
        for c in counts:
            for w in workers:
                env = dict(profile_env)
                flags = []
                label = pattern
                if c is not None:
                    env["FL_TEST_SPAWN_AI"] = str(c)
                    label += f"_e{c}"
                    if c > 0:
                        flags += ["--assert-min-entities", str(c)]
                if w is not None:
                    env["FL_SIM_WORKER_THREADS"] = str(w)
                    label += f"_w{w}"
                runs.append({"pattern": pattern, "label": label, "env": env, "flags": flags})
    return runs


def render_summary(profile_name, results):
    """Render a Markdown summary table from a list of per-pattern result dicts."""
    lines = [f"## Scale gate — profile `{profile_name}`", ""]
    lines.append("| Pattern | Result | Checks | Baseline (KB/s) |")
    lines.append("|---|---|---|---|")
    for r in results:
        status = "✅ pass" if r["passed"] else "❌ FAIL"
        checks = "<br>".join(
            f"{'✅' if c['ok'] else '❌'} {c['name']}: {c['detail']}" for c in r["checks"]
        )
        bl = r["baseline"]["detail"]
        if r["baseline"]["regressed"]:
            bl = "❌ REGRESSED " + bl
        lines.append(f"| {r['pattern']} | {status} | {checks} | {bl} |")
    lines.append("")
    return "\n".join(lines)


def runner_for_platform(platform):
    """Return the runner script name for a platform string (sys.platform style)."""
    return "run_loadtest.ps1" if platform.startswith("win") else "run_loadtest.sh"


# --------------------------------------------------------------------------------------------------
# I/O orchestration (thin; exercised by CI, not unit tests)
# --------------------------------------------------------------------------------------------------
def run_pattern(build_dir, clients, duration_s, pattern, flags, runner, port, report_path, extra_env=None):
    """Invoke run_loadtest.sh/.ps1 for one pattern. Returns (exit_code, report_path | None).

    The report path is pinned via FL_LOADTEST_REPORT (deterministic — no glob/mtime guessing, and
    nothing buffered for a multi-hour soak), and a distinct FL_LOADTEST_PORT per run avoids the UDP
    rebind race when servers are launched back-to-back. `extra_env` (entity-scale FL_TEST_SPAWN_AI /
    FL_SIM_WORKER_THREADS) is merged into the child environment. Output streams live to the console.
    """
    runner_path = SCRIPT_DIR / runner
    env = dict(os.environ)
    env["FL_LOADTEST_PORT"] = str(port)
    env["FL_LOADTEST_REPORT"] = str(report_path)
    if extra_env:
        env.update({k: str(v) for k, v in extra_env.items()})
    if runner.endswith(".ps1"):
        cmd = ["pwsh", "-File", str(runner_path), build_dir, str(clients), str(duration_s), pattern]
    else:
        cmd = ["bash", str(runner_path), build_dir, str(clients), str(duration_s), pattern]
    if flags:
        cmd += ["--", *flags]
    print(f"[scale_gate] $ FL_LOADTEST_PORT={port} {' '.join(cmd)}", flush=True)
    proc = subprocess.run(cmd, env=env, check=False)
    report = Path(report_path)
    if proc.returncode != 0 or not report.is_file():
        return proc.returncode, None
    return proc.returncode, report


def write_summary(text):
    """Echo the Markdown summary to stdout, and append it to $GITHUB_STEP_SUMMARY when set."""
    print(text)
    dest = os.environ.get("GITHUB_STEP_SUMMARY")
    if dest:
        with open(dest, "a", encoding="utf-8") as f:
            f.write(text + "\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description="bot_swarm scale-gate driver (#520)")
    parser.add_argument("--profile", required=True, help="profile name in scale-gate.json")
    parser.add_argument("--build-dir", required=True, help="build tree with fl-server + bot_swarm")
    parser.add_argument("--config", default=str(DEFAULT_CONFIG))
    parser.add_argument("--baseline", default=str(DEFAULT_BASELINE))
    parser.add_argument("--strict", action="store_true",
                        help="enforce the tick-ms p99 gate (reference/self-hosted runner)")
    parser.add_argument("--update-baseline", action="store_true",
                        help="rewrite the baseline file from this run instead of gating")
    args = parser.parse_args(argv)

    config = load_config(args.config)
    profile = load_profile(config, args.profile)
    tolerance = config.get("kbs_baseline_tolerance_pct", 10)

    baseline = {}
    if Path(args.baseline).is_file():
        baseline = load_config(args.baseline).get("kbs", {})

    flags = assert_flags(profile, args.strict) + degrade_flags(profile)
    runner = runner_for_platform(sys.platform)
    baselined = profile.get("baselined", True)
    if args.update_baseline and not baselined:
        print(f"[scale_gate] ERROR: profile '{args.profile}' is not baselined (advisory "
              "characterisation); nothing to update", file=sys.stderr)
        return 1

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    base_port = int(os.environ.get("FL_LOADTEST_PORT", "4793"))

    runs = expand_runs(profile)
    results = []
    any_runner_failed = False
    new_baseline = dict(baseline)
    for idx, run in enumerate(runs):
        pattern, label = run["pattern"], run["label"]
        # Distinct port per run dodges the UDP rebind race between back-to-back servers.
        port = base_port + idx
        report_path = RESULTS_DIR / f"loadtest_{profile['clients']}c_{label}_{args.profile}.json"
        code, report_path = run_pattern(args.build_dir, profile["clients"], profile["duration_s"],
                                        pattern, flags + run["flags"], runner, port, report_path,
                                        extra_env=run["env"])
        if report_path is None:
            any_runner_failed = True
            print(f"[scale_gate] ERROR: '{label}' run failed (exit {code}, no report)",
                  file=sys.stderr)
            results.append({"pattern": label, "passed": False,
                            "checks": [{"name": "runner", "ok": False,
                                        "detail": f"no report (exit {code})", "advisory": False}],
                            "baseline": {"regressed": False, "detail": "n/a"}})
            continue
        report = json.loads(report_path.read_text(encoding="utf-8"))
        evaluation = evaluate_report(report, profile, args.strict)
        if baselined:
            key = baseline_key(args.profile, pattern)
            cmp = compare_baseline(report, baseline.get(key), tolerance)
            new_baseline[key] = report.get("downstream_kbs_per_client", {}).get("mean", 0.0)
        else:
            # Advisory characterisation: report entity count + tick p99, never touch the baseline.
            srv = report.get("server_tick") or {}
            detail = (f"entities={srv.get('entities', 0)} "
                      f"tick_p99={srv.get('tick_ms', {}).get('p99', 0.0):.2f} ms (advisory)")
            cmp = {"regressed": False, "detail": detail}
        if code != 0 or cmp["regressed"]:
            evaluation["passed"] = False
        results.append({"pattern": label, "passed": evaluation["passed"],
                        "checks": evaluation["checks"], "baseline": cmp})

    if args.update_baseline:
        if any_runner_failed:
            print("[scale_gate] ERROR: a run failed; refusing to write a partial baseline",
                  file=sys.stderr)
            return 1
        Path(args.baseline).write_text(
            json.dumps({"kbs": new_baseline}, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"[scale_gate] wrote baseline {args.baseline}")
        return 0

    write_summary(render_summary(args.profile, results))

    ok = not any_runner_failed and all(r["passed"] for r in results)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
