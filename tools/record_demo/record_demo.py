# SPDX-FileCopyrightText: Contributors to Fighters Legacy
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cinematic demo-recording driver (#917, epic #909).

Records the scripted demo missions (missions/demos/*.yaml) to video, headless, with the built-in AI
bots and no content pack. Per demo it launches a standalone fl-server at a reduced wall-clock rate so a
slow software-rendered (lavapipe) client never misses a capture boundary, connects a headless observer
recorder client, waits for it to finish, and ffprobes the output as an end-to-end smoke check.

    tools/record_demo/record_demo.py --game <fighters-legacy> --server <fl-server> --assets <root> \\
        --mission demo-dogfight --out-dir /tmp/demos --headless

`--all` records every demo in missions/demos/demos.json. `--headless` sets the lavapipe environment
(VK_ICD_FILENAMES + unset DISPLAY) so it renders with no display and no GPU. `--png` writes a PNG
sequence instead of an mp4 (no ffmpeg/libx264 needed). See docs/developer/demo-recording.md.

Only the pure helpers (manifest parse, duration bounds) are unit-tested (tests/test_record_demo.py); the
subprocess orchestration is exercised manually and by the optional demo-videos CI job (#918).
"""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
from fl_ports import BindFailure, looks_like_bind_failure, with_free_port  # noqa: E402

DEFAULT_ICD = "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"

# wait_for_listening outcomes. A bind failure is distinguished from a timeout because only the
# former is worth retrying on a fresh port (#1056).
LISTEN_READY = "ready"
LISTEN_BIND_FAILED = "bind-failed"
LISTEN_TIMEOUT = "timeout"


def load_manifest(path):
    """Parse demos.json into a list of demo dicts. Pure — unit-tested."""
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    demos = data.get("demos", [])
    for d in demos:
        for key in ("id", "mission_file", "out_name", "expected_duration"):
            if key not in d:
                raise ValueError(f"demo entry missing '{key}': {d!r}")
    return demos


def duration_ok(actual, expected, floor_frac=0.3, ceil_slack=6.0):
    """A loose smoke bound on the recorded video duration (seconds). The recorder stops at the shot-list
    end OR an earlier mission end, so the video may be shorter than the shot total but never much longer.
    Pure — unit-tested. Returns (ok, reason)."""
    if actual <= 0.0:
        return False, "empty/zero-length video"
    floor = min(3.0, expected * floor_frac)
    ceil = expected + ceil_slack
    if actual < floor:
        return False, f"too short: {actual:.1f}s < floor {floor:.1f}s"
    if actual > ceil:
        return False, f"too long: {actual:.1f}s > ceil {ceil:.1f}s"
    return True, f"{actual:.1f}s within [{floor:.1f}, {ceil:.1f}]"


def ffprobe_duration(path, ffprobe="ffprobe"):
    """Return the container duration of a media file in seconds (0.0 if unavailable)."""
    try:
        out = subprocess.run(
            [ffprobe, "-v", "error", "-show_entries", "format=duration",
             "-of", "default=noprint_wrappers=1:nokey=1", path],
            capture_output=True, text=True, timeout=30,
        )
        return float(out.stdout.strip() or "0")
    except (subprocess.SubprocessError, ValueError, FileNotFoundError):
        return 0.0


def wait_for_listening(log_path, deadline_s=15.0):
    """Poll an fl-server log until it is listening, fails to bind, or the deadline passes.

    Returns one of `LISTEN_READY` / `LISTEN_BIND_FAILED` / `LISTEN_TIMEOUT`. Reporting the bind
    failure distinctly is what lets `record_one` retry on a different port instead of spending the
    full deadline and then blaming the demo (#1056).
    """
    end = time.monotonic() + deadline_s
    while time.monotonic() < end:
        try:
            text = Path(log_path).read_text(encoding="utf-8", errors="ignore")
        except FileNotFoundError:
            text = ""
        if "listening on" in text:
            return LISTEN_READY
        if looks_like_bind_failure(text):
            return LISTEN_BIND_FAILED
        time.sleep(0.15)
    return LISTEN_TIMEOUT


def record_one(args, demo, env):
    """Record a single demo. Returns (ok, message)."""
    mission = str(Path(args.demos_dir) / demo["mission_file"])
    if not Path(mission).is_file():
        return False, f"mission file not found: {mission}"
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    srv_log = str(out_dir / f"{demo['out_name']}-server.log")

    def attempt(port):
        port = str(port)
        # 1) Launch a standalone fl-server at a reduced wall-rate so the recorder keeps pace. NOTE: do NOT
        # force --transport enet here — the game client with --connect uses the server default (GNS when
        # built with FL_ENABLE_GNS, else the enet fallback), and both ends must agree or the client never
        # connects. Leaving transport unset keeps the server on the same default the client picks.
        with open(srv_log, "w", encoding="utf-8") as slog:
            server = subprocess.Popen(
                [args.server, port, "8", "--bind", "127.0.0.1", "--assets", args.assets,
                 "--mission", mission, "--time-rate", args.time_rate],
                stdout=slog, stderr=subprocess.STDOUT, env=env,
            )
        try:
            listening = wait_for_listening(srv_log)
            if listening == LISTEN_BIND_FAILED:
                # Retried on a fresh port by with_free_port; the transport binds UDP and the probe
                # socket is closed before the server starts, so the port can be taken in the gap.
                raise BindFailure(f"fl-server could not bind port {port} (see {srv_log})")
            if listening != LISTEN_READY:
                return False, f"server never reported 'listening on' (see {srv_log})"

            # 2) Launch the headless observer recorder client.
            client = [args.game, "--connect", f"127.0.0.1:{port}", "--observer", "--auto",
                      "--assets", args.assets, "--shot-track", mission,
                      "--record-fps", str(args.fps), "--record-res", args.res,
                      "--exit-on-mission-end", "--record-max-sec", str(args.max_sec),
                      "--record-max-dup", str(args.max_dup)]
            if args.headless:
                client.append("--headless")
            if args.png:
                png_dir = out_dir / demo["out_name"]
                png_dir.mkdir(parents=True, exist_ok=True)
                client += ["--record-png-dir", str(png_dir)]
                out_path = str(png_dir)
            else:
                out_path = str(out_dir / f"{demo['out_name']}.mp4")
                client += ["--record", out_path]

            proc = subprocess.run(client, timeout=args.timeout, env=env)
            if proc.returncode != 0:
                return False, f"recorder client exited {proc.returncode} (duplicated-frame cap or encoder error)"
        finally:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()

        # 3) Smoke-check the output (mp4 only; PNG sequences are checked by frame count).
        if args.png:
            frames = len(list((out_dir / demo["out_name"]).glob("frame_*.png")))
            if frames <= 0:
                return False, "no PNG frames written"
            return True, f"{frames} PNG frame(s)"
        dur = ffprobe_duration(out_path, args.ffprobe)
        ok, reason = duration_ok(dur, float(demo["expected_duration"]))
        return ok, f"{out_path}: {reason}"

    try:
        return with_free_port(attempt)
    except BindFailure as e:
        return False, str(e)


def build_env(headless):
    env = dict(os.environ)
    if headless:
        env.setdefault("VK_ICD_FILENAMES", DEFAULT_ICD)
        env["DISPLAY"] = ""  # force off-screen even if a display is present
    return env


def main(argv=None):
    ap = argparse.ArgumentParser(description="Record the scripted demo missions to video, headless (#909).")
    ap.add_argument("--game", required=True, help="path to the fighters-legacy client binary")
    ap.add_argument("--server", required=True, help="path to the fl-server binary")
    ap.add_argument("--assets", required=True, help="content root (a bare checkout works — zero pack)")
    ap.add_argument("--demos-dir", default=str(Path(__file__).resolve().parents[2] / "missions" / "demos"))
    ap.add_argument("--out-dir", default="demo-videos")
    ap.add_argument("--mission", help="record a single demo by id (see demos.json)")
    ap.add_argument("--all", action="store_true", help="record every demo in demos.json")
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--res", default="1280x720")
    ap.add_argument("--time-rate", default="quarter", help="fl-server wall-rate: quarter|eighth|half|normal")
    ap.add_argument("--max-sec", type=int, default=180, help="per-demo wall-clock recording cap")
    ap.add_argument("--max-dup", type=int, default=300, help="fail a demo if duplicated frames exceed this")
    ap.add_argument("--timeout", type=float, default=900.0, help="per-demo client subprocess timeout")
    ap.add_argument("--headless", action="store_true", help="set the lavapipe env (no display, no GPU)")
    ap.add_argument("--png", action="store_true", help="record a PNG sequence instead of mp4 (no ffmpeg)")
    ap.add_argument("--ffprobe", default="ffprobe")
    args = ap.parse_args(argv)

    manifest = str(Path(args.demos_dir) / "demos.json")
    demos = load_manifest(manifest)
    if args.all:
        selected = demos
    elif args.mission:
        selected = [d for d in demos if d["id"] == args.mission]
        if not selected:
            print(f"unknown demo id: {args.mission} (choices: {[d['id'] for d in demos]})", file=sys.stderr)
            return 2
    else:
        print("specify --mission <id> or --all", file=sys.stderr)
        return 2

    env = build_env(args.headless)
    failures = 0
    for demo in selected:
        print(f"== recording {demo['id']} ==", flush=True)
        ok, msg = record_one(args, demo, env)
        status = "OK" if ok else "FAIL"
        print(f"[{status}] {demo['id']}: {msg}", flush=True)
        if not ok:
            failures += 1
    print(f"\n{len(selected) - failures}/{len(selected)} demo(s) recorded successfully.")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
