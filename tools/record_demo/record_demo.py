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

Only the pure helpers (manifest parse, duration bounds, the wall-clock cap, the content checks)
are unit-tested (tests/test_record_demo.py); the
subprocess orchestration is exercised manually and by the optional demo-videos CI job (#918).
"""
import argparse
import hashlib
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


# Sim seconds per wall-clock second, per `fl-server --time-rate` (engine/loop/TimeRate.h). The
# recorder's --record-max-sec is WALL CLOCK and a shot list is SIM time, so this table is the ratio
# between the cap and what the cap actually buys (#1347).
TIME_RATE_MULTIPLIER = {
    "paused": 0.0, "eighth": 0.125, "quarter": 0.25, "half": 0.5, "normal": 1.0,
    "double": 2.0, "quad": 4.0, "octa": 8.0,
}


def wall_clock_cap(expected_duration, time_rate, requested=0, slack=1.5, fixed_s=60.0):
    """Wall-clock seconds to allow a demo whose shot list is `expected_duration` SIM seconds.

    The recorder's cap is wall clock; the shot list is sim time; `--time-rate` is the ratio. A flat
    180 s cap at `--time-rate eighth` buys 22.5 s of shot list, which is why every demo recorded to
    exactly 22.6 s whatever its shots said — and the cap is a silent stop, so nothing said so.

    `requested > 0` (an explicit --max-sec) wins: the operator asked for a specific safety stop.
    Pure — unit-tested. Returns whole seconds."""
    if requested and requested > 0:
        return int(requested)
    mult = TIME_RATE_MULTIPLIER.get(str(time_rate).lower(), 1.0)
    if mult <= 0.0:  # "paused" advances no sim time at all; no cap can be enough
        raise ValueError("--time-rate paused records nothing: sim time never advances")
    return int(expected_duration / mult * slack + fixed_s)


def parse_content_probe(stderr_text):
    """Sum the black and frozen seconds ffmpeg's blackdetect/freezedetect reported. Pure — unit-tested.

    Returns (black_s, frozen_s). blackdetect prints `black_start:N black_end:N black_duration:N`;
    freezedetect prints `lavfi.freezedetect.freeze_duration: N` per frozen run."""
    black = 0.0
    frozen = 0.0
    for line in stderr_text.splitlines():
        if "black_duration:" in line:
            try:
                black += float(line.split("black_duration:")[1].split()[0])
            except (IndexError, ValueError):
                pass
        if "freeze_duration" in line:
            try:
                frozen += float(line.rsplit(":", 1)[1].strip())
            except (IndexError, ValueError):
                pass
    return black, frozen


def content_ok(duration, black_s, frozen_s, share=0.5):
    """Does the video contain a PICTURE? Pure — unit-tested. Returns (ok, reason).

    The duration check is an envelope check: it cannot fail on an empty video, and it did not — every
    black, 22.6 s recording in #1347 printed `[OK] ... within [3.0, N]`. A gate that checks the
    envelope and never the payload is not evidence. This is the payload check, and it is deliberately
    coarse: more than half the run black, or more than half of it a single frozen frame, is not a
    demo video however plausible its duration. Anything subtler is a job for a human watching it."""
    if duration <= 0.0:
        return False, "no content (zero-length)"
    if black_s >= duration * share:
        return False, f"{black_s:.1f}s of {duration:.1f}s is BLACK"
    if frozen_s >= duration * share:
        return False, f"{frozen_s:.1f}s of {duration:.1f}s is a FROZEN frame"
    return True, f"picture ok (black {black_s:.1f}s, frozen {frozen_s:.1f}s)"


def distinct_frames_ok(hashes, min_distinct=8):
    """A PNG sequence must contain more than one distinct image. Pure — unit-tested.

    The mp4 path has ffmpeg to look at the pixels; the PNG path deliberately has no image dependency,
    so it counts DISTINCT FILE CONTENTS instead. One repeated black frame — the exact #1347 failure —
    hashes to one value however many files it wrote."""
    n = len(set(hashes))
    if not hashes:
        return False, "no frames"
    want = min(min_distinct, len(hashes))
    if n < want:
        return False, f"only {n} distinct frame(s) in {len(hashes)} — the picture never changed"
    return True, f"{n} distinct of {len(hashes)} frame(s)"


def probe_content(path, ffmpeg="ffmpeg"):
    """Decode `path` once through blackdetect+freezedetect. Returns (black_s, frozen_s).

    Both filters are cheap next to the recording itself, and one decode pass answers both questions.
    A missing/broken ffmpeg returns (0, 0) — the duration check still applies, and openFfmpeg would
    have failed the recording long before this."""
    try:
        out = subprocess.run(
            [ffmpeg, "-v", "info", "-nostats", "-i", path,
             "-vf", "blackdetect=d=1:pix_th=0.03,freezedetect=n=0.001:d=2",
             "-an", "-f", "null", "-"],
            capture_output=True, text=True, timeout=300,
        )
        return parse_content_probe(out.stderr)
    except (subprocess.SubprocessError, FileNotFoundError):
        return 0.0, 0.0


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
    # The recorder's cap is WALL CLOCK and the shot list is SIM time; --time-rate is the ratio, so
    # the cap has to be derived from both or it silently truncates every demo to the same length
    # (#1347). --max-sec still overrides it, and the subprocess timeout follows the cap.
    try:
        max_sec = wall_clock_cap(float(demo["expected_duration"]), args.time_rate, args.max_sec)
    except ValueError as e:
        return False, str(e)
    timeout = args.timeout if args.timeout > 0 else max_sec + 120.0
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
                      "--exit-on-mission-end", "--record-max-sec", str(max_sec),
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

            proc = subprocess.run(client, timeout=timeout, env=env)
            if proc.returncode != 0:
                return False, f"recorder client exited {proc.returncode} (duplicated-frame cap or encoder error)"
        finally:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()

        # 3) Check the output — the envelope AND the payload (#1347). A duration bound cannot fail on
        # a video of one repeated black frame, and for a while it did not: every such run printed
        # "[OK] ... 22.6s within [3.0, N]".
        if args.png:
            pngs = sorted((out_dir / demo["out_name"]).glob("frame_*.png"))
            if not pngs:
                return False, "no PNG frames written"
            # Hash a spread of frames rather than all of them: enough to tell a moving picture from a
            # still one without reading a gigabyte of PNGs.
            step = max(1, len(pngs) // 32)
            hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in pngs[::step]]
            ok, reason = distinct_frames_ok(hashes)
            return ok, f"{len(pngs)} PNG frame(s): {reason}"
        dur = ffprobe_duration(out_path, args.ffprobe)
        ok, reason = duration_ok(dur, float(demo["expected_duration"]))
        if not ok:
            return False, f"{out_path}: {reason}"
        black_s, frozen_s = probe_content(out_path, args.ffmpeg)
        content, creason = content_ok(dur, black_s, frozen_s)
        return content, f"{out_path}: {reason}; {creason}"

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
    ap.add_argument("--max-sec", type=int, default=0,
                    help="per-demo WALL-CLOCK recording cap; 0 = derive it from the shot list and "
                         "--time-rate (a flat cap truncates every demo to the same length, #1347)")
    ap.add_argument("--max-dup", type=int, default=300, help="fail a demo if duplicated frames exceed this")
    ap.add_argument("--timeout", type=float, default=0.0,
                    help="per-demo client subprocess timeout; 0 = the derived recording cap + 120 s")
    ap.add_argument("--headless", action="store_true", help="set the lavapipe env (no display, no GPU)")
    ap.add_argument("--png", action="store_true", help="record a PNG sequence instead of mp4 (no ffmpeg)")
    ap.add_argument("--ffprobe", default="ffprobe")
    ap.add_argument("--ffmpeg", default="ffmpeg", help="used for the black/frozen content check")
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
