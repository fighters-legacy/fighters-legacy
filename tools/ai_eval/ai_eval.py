#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MKZ Systems LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Local-provider evaluation harness for the Dynamic World & Agentic AI initiative (#599).

Measures latency, structured-output reliability and task correctness of any
**OpenAI-compatible** endpoint (Ollama, llama-server, vLLM, LiteLLM, a hosted API) across the
three initiative workloads:

    intent   — chat utterance -> structured wingman command (Epic O; budget 2 s)
    mission  — campaign brief -> mission YAML, validated by the real `validate-mission`
               binary, with one generate->validate->repair round (Epic N; budget 60 s)
    ops      — server metrics snapshot -> triage recommendation over an action allowlist
               (Epic P; budget 60 s)

This harness NEVER runs in ctest and CI never requires a model (docs/ai-architecture.md §7).
It is a developer/reference-environment tool: run it locally against a local endpoint, then
record the numbers in docs/ai-provider-evaluation.md.

    python3 tools/ai_eval/ai_eval.py --models ollama/qwen2.5-coder-14b --suite all \
        --base-url http://localhost:4000 --api-key-env FL_AI_API_KEY

Stdlib only — no third-party client. The API key is read from the environment, never a flag and
never a file (mirrors the `[ai.provider] api_key_env` seam).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

SUITE_DIR = Path(__file__).resolve().parent / "suites"
DEFAULT_RESULT_DIR = Path(__file__).resolve().parent / "results"
SUITE_NAMES = ("intent", "mission", "ops")

# ---- suite loading (pure) ----------------------------------------------------------------------


def load_suite(path):
    """Load a suite JSON file. Raises ValueError on a malformed/missing suite."""
    p = Path(path)
    if not p.is_file():
        raise ValueError(f"suite not found: {p}")
    suite = json.loads(p.read_text(encoding="utf-8"))
    for key in ("name", "system_prompt", "budget_s", "cases"):
        if key not in suite:
            raise ValueError(f"suite {p.name}: missing required key '{key}'")
    if not suite["cases"]:
        raise ValueError(f"suite {p.name}: no cases")
    return suite


def suite_path(name):
    return SUITE_DIR / f"{name}.json"


# ---- response extraction (pure) ----------------------------------------------------------------

_FENCE_RE = re.compile(r"```(?:[a-zA-Z0-9_+-]*)\n(.*?)```", re.S)


def strip_code_fence(text):
    """Return the first fenced block's body, or the whole text when unfenced.

    Small models fence their output constantly even when told not to; treating that as a parse
    failure would measure prompt compliance, not structured-output capability.
    """
    if text is None:
        return ""
    m = _FENCE_RE.search(text)
    return m.group(1).strip() if m else text.strip()


def extract_json_object(text):
    """Best-effort parse of a single JSON object out of a model response. None on failure."""
    body = strip_code_fence(text)
    if not body:
        return None
    try:
        obj = json.loads(body)
        return obj if isinstance(obj, dict) else None
    except json.JSONDecodeError:
        pass
    # Fall back to the first balanced {...} span (handles leading prose / trailing chatter).
    start = body.find("{")
    while start != -1:
        depth = 0
        in_str = False
        esc = False
        for i in range(start, len(body)):
            c = body[i]
            if in_str:
                if esc:
                    esc = False
                elif c == "\\":
                    esc = True
                elif c == '"':
                    in_str = False
                continue
            if c == '"':
                in_str = True
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    try:
                        obj = json.loads(body[start : i + 1])
                        return obj if isinstance(obj, dict) else None
                    except json.JSONDecodeError:
                        break
        start = body.find("{", start + 1)
    return None


def extract_yaml_document(text):
    """Return the YAML body of a response (fence-stripped). Empty string when there is none."""
    body = strip_code_fence(text)
    # A reasoning model may prefix prose; YAML mappings we want always start at a top-level key.
    lines = body.splitlines()
    for i, line in enumerate(lines):
        if re.match(r"^[A-Za-z_][A-Za-z0-9_]*\s*:", line):
            return "\n".join(lines[i:]).strip()
    return ""


# ---- scoring (pure) ----------------------------------------------------------------------------


def score_intent(case, response_text):
    """Score one intent case: parsed / schema-valid / correct."""
    grammar = case["_grammar"]
    obj = extract_json_object(response_text)
    if obj is None:
        return {"parsed": False, "schema_valid": False, "correct": False, "got": None}
    cmd = obj.get("command")
    schema_valid = isinstance(cmd, str) and cmd in grammar
    correct = schema_valid and cmd == case["expect"]["command"]
    return {
        "parsed": True,
        "schema_valid": schema_valid,
        "correct": bool(correct),
        "got": cmd,
    }


def score_ops(case, response_text):
    """Score one ops-triage case: root cause + action allowlist compliance."""
    causes = case["_causes"]
    allowlist = case["_allowlist"]
    obj = extract_json_object(response_text)
    if obj is None:
        return {
            "parsed": False,
            "schema_valid": False,
            "correct": False,
            "actions_allowed": False,
            "got": None,
        }
    cause = obj.get("root_cause")
    actions = obj.get("actions")
    if not isinstance(actions, list):
        actions = []
    schema_valid = isinstance(cause, str) and cause in causes
    # An agent that invents a command it is not permitted to run is the failure mode Epic P's
    # policy engine exists to reject — measure it explicitly.
    actions_allowed = all(isinstance(a, str) and a in allowlist for a in actions)
    correct = schema_valid and cause == case["expect"]["root_cause"] and actions_allowed
    return {
        "parsed": True,
        "schema_valid": schema_valid,
        "correct": bool(correct),
        "actions_allowed": bool(actions_allowed),
        "got": cause,
    }


def parse_yaml_sequence(yaml_text, key):
    """Scalar items of a top-level YAML sequence, accepting BOTH styles:

        sides: [nato, russia]        # flow
        sides:                       # block
          - nato
          - russia

    Models pick either style freely (they copy whichever the prompt's example used), so a
    block-only check would score a perfectly valid document as wrong. Not a general YAML parser —
    just enough to read a flat sequence of scalars without taking a PyYAML dependency (these tools
    are stdlib-only).
    """
    # `[ \t]*` not `\s*`: \s matches newlines, so a greedy \s* after the colon would swallow the
    # line break and capture the first block item as if it were an inline value.
    m = re.search(rf"^{re.escape(key)}[ \t]*:[ \t]*(.*)$", yaml_text, re.M)
    if not m:
        return []
    inline = m.group(1).strip()
    if inline.startswith("["):
        body = inline[1 : inline.rindex("]")] if "]" in inline else inline[1:]
        return [i.strip().strip("\"'") for i in body.split(",") if i.strip()]
    items = []
    for line in yaml_text[m.end() :].splitlines():
        if not line.strip():
            continue
        entry = re.match(r"^\s+-\s*(.+?)\s*$", line)
        if not entry:
            break  # dedented to the next top-level key -> sequence is over
        items.append(entry.group(1).strip().strip("\"'"))
    return items


def check_mission_semantics(case, yaml_text):
    """Cheap semantic checks beyond `validate-mission` (which only enforces the schema)."""
    errors = []
    expect = case["expect"]
    declared_sides = parse_yaml_sequence(yaml_text, "sides")
    for side in expect.get("sides", []):
        if side not in declared_sides:
            errors.append(f"side '{side}' not declared")
    min_objects = expect.get("min_objects", 0)
    n_objects = len(re.findall(r"^\s*-\s*type\s*:", yaml_text, re.M))
    if n_objects < min_objects:
        errors.append(f"{n_objects} objects < required {min_objects}")
    preset = expect.get("weather_preset")
    if preset and not re.search(rf"preset\s*:\s*{re.escape(preset)}\b", yaml_text):
        errors.append(f"weather preset '{preset}' missing")
    return errors


def summarize_latencies(latencies):
    """min/mean/p50/p95/max over a list of seconds. Empty -> zeros."""
    if not latencies:
        return {"min": 0.0, "mean": 0.0, "p50": 0.0, "p95": 0.0, "max": 0.0}
    s = sorted(latencies)
    p50 = statistics.median(s)
    # Nearest-rank p95 (small n; percentile interpolation would over-promise precision).
    p95 = s[min(len(s) - 1, max(0, int(round(0.95 * len(s))) - 1))]
    return {
        "min": round(s[0], 3),
        "mean": round(statistics.fmean(s), 3),
        "p50": round(p50, 3),
        "p95": round(p95, 3),
        "max": round(s[-1], 3),
    }


def aggregate(suite_name, budget_s, case_results):
    """Roll per-case results into the suite-level metrics the spike reports."""
    n = len(case_results)
    lat = [c["latency_s"] for c in case_results if c.get("latency_s") is not None]
    ok = lambda key: sum(1 for c in case_results if c.get(key))  # noqa: E731
    metrics = {
        "suite": suite_name,
        "cases": n,
        "errors": sum(1 for c in case_results if c.get("error")),
        "parse_rate": round(ok("parsed") / n, 3) if n else 0.0,
        "schema_valid_rate": round(ok("schema_valid") / n, 3) if n else 0.0,
        "accuracy": round(ok("correct") / n, 3) if n else 0.0,
        "latency_s": summarize_latencies(lat),
        "budget_s": budget_s,
    }
    metrics["within_budget"] = bool(lat) and metrics["latency_s"]["p95"] <= budget_s
    if suite_name == "mission":
        metrics["pass_at_1"] = round(ok("valid_first_try") / n, 3) if n else 0.0
        metrics["pass_after_repair"] = metrics["accuracy"]
    if suite_name == "ops":
        metrics["action_allowlist_rate"] = (
            round(ok("actions_allowed") / n, 3) if n else 0.0
        )
    return metrics


def render_markdown(all_metrics):
    """Render the cross-model comparison table (the spike's headline deliverable)."""
    lines = [
        "| Model | Suite | Cases | Parse | Schema | Accuracy | p50 s | p95 s | Budget | In budget |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|:-:|",
    ]
    for model, metrics in all_metrics:
        lat = metrics["latency_s"]
        lines.append(
            f"| `{model}` | {metrics['suite']} | {metrics['cases']} | "
            f"{metrics['parse_rate']:.0%} | {metrics['schema_valid_rate']:.0%} | "
            f"{metrics['accuracy']:.0%} | {lat['p50']:.1f} | {lat['p95']:.1f} | "
            f"{metrics['budget_s']:.0f} | {'yes' if metrics['within_budget'] else 'NO'} |"
        )
    return "\n".join(lines)


# ---- I/O edges ---------------------------------------------------------------------------------


def chat_completion(base_url, api_key, model, system, user, timeout, json_mode):
    """POST /v1/chat/completions. Returns (text, latency_s). Raises RuntimeError on failure."""
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": system},
            {"role": "user", "content": user},
        ],
        "temperature": 0,
        "stream": False,
    }
    if json_mode:
        payload["response_format"] = {"type": "json_object"}
    req = urllib.request.Request(
        base_url.rstrip("/") + "/v1/chat/completions",
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}" if api_key else "",
        },
        method="POST",
    )
    start = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:  # pragma: no cover - network edge
        raise RuntimeError(f"HTTP {e.code}: {e.read()[:200].decode('utf-8', 'replace')}") from e
    except Exception as e:  # pragma: no cover - network edge
        raise RuntimeError(str(e)) from e
    elapsed = time.monotonic() - start
    try:
        text = body["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError) as e:  # pragma: no cover - network edge
        raise RuntimeError(f"malformed completion: {json.dumps(body)[:200]}") from e
    return text, elapsed


def run_validate_mission(binary, yaml_text):
    """Run the real validate-mission binary over a YAML doc. Returns (ok, stderr)."""
    with tempfile.TemporaryDirectory() as td:
        path = Path(td) / "mission.yaml"
        path.write_text(yaml_text, encoding="utf-8")
        proc = subprocess.run(
            [str(binary), str(path)],
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )
    return proc.returncode == 0, proc.stderr.strip()


# ---- per-suite drivers -------------------------------------------------------------------------


def run_intent_case(cfg, suite, case):
    text, latency = chat_completion(
        cfg["base_url"], cfg["api_key"], cfg["model"],
        suite["system_prompt"], case["utterance"], cfg["timeout"], json_mode=True,
    )
    case = dict(case, _grammar=suite["grammar"])
    result = score_intent(case, text)
    result.update({"id": case["id"], "latency_s": round(latency, 3), "raw": text[:400]})
    return result


def run_ops_case(cfg, suite, case):
    user = json.dumps(case["snapshot"], indent=2)
    text, latency = chat_completion(
        cfg["base_url"], cfg["api_key"], cfg["model"],
        suite["system_prompt"], user, cfg["timeout"], json_mode=True,
    )
    case = dict(case, _causes=suite["root_causes"], _allowlist=suite["action_allowlist"])
    result = score_ops(case, text)
    result.update({"id": case["id"], "latency_s": round(latency, 3), "raw": text[:400]})
    return result


def run_mission_case(cfg, suite, case):
    """generate -> validate -> (one) repair. Mirrors the Epic N director pipeline."""
    start = time.monotonic()
    text, _ = chat_completion(
        cfg["base_url"], cfg["api_key"], cfg["model"],
        suite["system_prompt"], case["brief"], cfg["timeout"], json_mode=False,
    )
    yaml_text = extract_yaml_document(text)
    valid, stderr = (False, "empty response")
    if yaml_text:
        valid, stderr = run_validate_mission(cfg["validate_mission"], yaml_text)
    semantic = check_mission_semantics(case, yaml_text) if valid else ["schema invalid"]
    first_try = bool(valid and not semantic)

    repaired = False
    if not first_try:
        # One repair round, fed the real validator's stderr — the director's error-feedback loop.
        problems = stderr or "; ".join(semantic)
        repair_user = (
            f"{case['brief']}\n\nYour previous mission was rejected:\n{problems}\n\n"
            f"Previous attempt:\n{yaml_text or text}\n\nEmit a corrected mission YAML."
        )
        text, _ = chat_completion(
            cfg["base_url"], cfg["api_key"], cfg["model"],
            suite["system_prompt"], repair_user, cfg["timeout"], json_mode=False,
        )
        yaml_text = extract_yaml_document(text)
        valid, stderr = (False, "empty response")
        if yaml_text:
            valid, stderr = run_validate_mission(cfg["validate_mission"], yaml_text)
        semantic = check_mission_semantics(case, yaml_text) if valid else ["schema invalid"]
        repaired = bool(valid and not semantic)

    latency = time.monotonic() - start
    ok = first_try or repaired
    return {
        "id": case["id"],
        "parsed": bool(yaml_text),
        "schema_valid": bool(valid),
        "valid_first_try": first_try,
        "correct": ok,
        "repaired": repaired and not first_try,
        "errors_detail": (stderr or "; ".join(semantic))[:300] if not ok else "",
        "latency_s": round(latency, 3),
        "raw": text[:400],
    }


RUNNERS = {"intent": run_intent_case, "ops": run_ops_case, "mission": run_mission_case}


def run_suite(cfg, suite):
    runner = RUNNERS[suite["name"]]
    results = []
    for case in suite["cases"]:
        for rep in range(cfg["repeat"]):
            try:
                r = runner(cfg, suite, case)
            except RuntimeError as e:
                r = {
                    "id": case["id"],
                    "error": str(e),
                    "parsed": False,
                    "schema_valid": False,
                    "correct": False,
                    "latency_s": None,
                }
            r["rep"] = rep
            results.append(r)
            mark = "." if r.get("correct") else ("!" if r.get("error") else "x")
            print(mark, end="", flush=True)
    print()
    return results


# ---- main --------------------------------------------------------------------------------------


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Evaluate an OpenAI-compatible provider on the agentic-AI workloads (#599)."
    )
    parser.add_argument("--models", required=True, help="comma-separated model ids to sweep")
    parser.add_argument("--base-url", default=os.environ.get("FL_AI_BASE_URL", "http://localhost:11434"))
    parser.add_argument("--api-key-env", default="FL_AI_API_KEY", help="env var holding the key")
    parser.add_argument("--suite", default="all", help=f"{'|'.join(SUITE_NAMES)}|all")
    parser.add_argument("--repeat", type=int, default=1, help="repetitions per case")
    parser.add_argument("--timeout", type=float, default=180.0, help="per-request timeout (s)")
    parser.add_argument(
        "--validate-mission",
        default="build/release/tools/validate-mission",
        help="path to the validate-mission binary (mission suite only)",
    )
    parser.add_argument("--out", default=str(DEFAULT_RESULT_DIR), help="result directory")
    args = parser.parse_args(argv)

    suites = SUITE_NAMES if args.suite == "all" else tuple(args.suite.split(","))
    for s in suites:
        if s not in SUITE_NAMES:
            print(f"[ai_eval] ERROR: unknown suite '{s}'", file=sys.stderr)
            return 2

    api_key = os.environ.get(args.api_key_env, "")
    if "mission" in suites and not Path(args.validate_mission).is_file():
        print(
            f"[ai_eval] ERROR: validate-mission not found at {args.validate_mission}\n"
            "  build it: cmake --build --preset release --target validate-mission",
            file=sys.stderr,
        )
        return 2

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    table = []
    report = {"base_url": args.base_url, "models": {}}

    for model in [m.strip() for m in args.models.split(",") if m.strip()]:
        report["models"][model] = {}
        for suite_name in suites:
            suite = load_suite(suite_path(suite_name))
            cfg = {
                "base_url": args.base_url,
                "api_key": api_key,
                "model": model,
                "timeout": args.timeout,
                "repeat": args.repeat,
                "validate_mission": args.validate_mission,
            }
            print(f"[ai_eval] {model} :: {suite_name} ({len(suite['cases'])} cases)", flush=True)
            case_results = run_suite(cfg, suite)
            metrics = aggregate(suite_name, suite["budget_s"], case_results)
            report["models"][model][suite_name] = {"metrics": metrics, "cases": case_results}
            table.append((model, metrics))

    summary = render_markdown(table)
    print("\n" + summary)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    (out_dir / f"ai-eval-{stamp}.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (out_dir / f"ai-eval-{stamp}.md").write_text(summary + "\n", encoding="utf-8")
    print(f"\n[ai_eval] wrote {out_dir}/ai-eval-{stamp}.{{json,md}}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
