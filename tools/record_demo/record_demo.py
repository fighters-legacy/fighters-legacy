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
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
from fl_ports import BindFailure, looks_like_bind_failure, with_free_port  # noqa: E402

try:
    import yaml
except ImportError:  # pragma: no cover - --help and the pure unit tests must work without it
    yaml = None

DEFAULT_ICD = "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"

# wait_for_listening outcomes. A bind failure is distinguished from a timeout because only the
# former is worth retrying on a fresh port (#1056).
LISTEN_READY = "ready"
LISTEN_BIND_FAILED = "bind-failed"
LISTEN_TIMEOUT = "timeout"

# The frame-difference floor, in signalstats YDIF units (mean absolute Y difference, 0-255 scale),
# at or below which consecutive frames count as the same picture (#1378).
#
# This is measured directly rather than through ffmpeg's `freezedetect`, because freezedetect's
# response to its own noise threshold is NOT MONOTONE on real footage and so cannot be calibrated.
# Sweeping `n` over one demo-sam-strike recording:
#
#   n=0.1 -> 0.0s   n=0.01 -> 25.7s   n=0.001 -> 0.0s   n=0.0002 -> 6.6s   n=0.000005 -> 0.0s
#
# A tighter threshold reporting MORE frozen time, twice, in both directions. Whatever value you pick
# you cannot say what it means, and the shipped default (0.001) sat above even demo-dogfight's median
# frame difference -- above the busiest demo in the set -- so every quieter demo read as frozen while
# its picture was moving. The tell was that the verdict depended on `--time-rate`, a recording SPEED
# knob: the identical shot list measured 0.0s frozen at `normal` and 93.8s at `quarter`, because the
# sky animates on wall clock and advances four times further per video frame at the faster rate.
#
# A run of frames below a YDIF floor is monotone by construction and separates cleanly on measured
# medians over 6 s windows:
#
#   footage                                                  YDIF median
#   demo-atc-scramble, cameras 2 km out over empty ground       0.000094   <- genuinely dead
#   demo-night-patrol, a deliberate static hold                 0.00014    <- genuinely still
#   demo-atc-scramble, cameras 60 m off the measured track      0.0035     <- MOVING
#   demo-dogfight, the busiest demo in the set                  0.21       <- moving, loudly
#
# 0.0005 sits ~5x above the dead footage and ~7x below real motion. A recording of one repeated frame
# -- the #1347 failure this check exists for -- measures exactly 0 and is caught with the whole margin
# to spare. Re-measure with `signalstats,metadata=print:key=lavfi.signalstats.YDIF` before changing
# it: this number IS the check.
FREEZE_YDIF = 0.0005

# A run of still frames shorter than this is not reported: a slow pan over smooth sky legitimately
# repeats a few frames, and the question is whether a shot rendered NOTHING, not whether two frames
# matched.
FREEZE_MIN_S = 2.0

_PTS_TIME_RE = re.compile(r"pts_time:([0-9.]+)")
_YDIF_RE = re.compile(r"signalstats\.YDIF=([0-9.eE+-]+)")


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


def shot_list_end(mission):
    """The sim second a mission's camera shot list ends at: max(start + duration). Pure.

    A mission with no shots ends at 0 -- there is nothing to record, which manifest_drift reports
    rather than silently treating as agreement.
    """
    shots = ((mission or {}).get("cameras") or {}).get("shots") or []
    end = 0.0
    for shot in shots:
        end = max(end, float(shot.get("start", 0.0)) + float(shot.get("duration", 0.0)))
    return end


def manifest_drift(demos, demos_dir):
    """Every demo whose declared `expected_duration` disagrees with its own shot list (#1378).

    The two numbers are load-bearing in DIFFERENT places, which is why they must agree: since #1347
    the recorder derives `--record-max-sec` from the shot list, while the acceptance window comes
    from `demos.json`. demo-atc-scramble's shots ran to 128 s while the manifest still declared the
    44 s it had carried unchanged since #909 -- so a successful 128 s recording would have failed
    its own duration gate, and the recorder capped the run instead. Reviewing the pair kept them
    honest for exactly as long as somebody looked; a check does not get bored.

    Returns a list of human-readable mismatch strings, empty when the set is clean.
    """
    if yaml is None:
        raise RuntimeError("PyYAML is required to check demos.json against the shot lists "
                           "(pip install pyyaml)")
    problems = []
    for demo in demos:
        path = Path(demos_dir) / demo["mission_file"]
        if not path.is_file():
            problems.append(f"{demo['id']}: mission file not found: {path}")
            continue
        try:
            mission = yaml.safe_load(path.read_text(encoding="utf-8"))
        except yaml.YAMLError as e:
            problems.append(f"{demo['id']}: {demo['mission_file']} is not valid YAML: {e}")
            continue
        end = shot_list_end(mission)
        declared = float(demo["expected_duration"])
        if end <= 0.0:
            problems.append(f"{demo['id']}: {demo['mission_file']} declares no camera shots, but "
                            f"demos.json expects {declared:g}s")
        elif abs(end - declared) > 0.001:
            problems.append(f"{demo['id']}: shots end at {end:g}s but demos.json says {declared:g}s "
                            f"({demo['mission_file']})")
    return problems


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
    """Black seconds from ffmpeg's blackdetect, plus the frozen runs measured from YDIF. Pure.

    Returns (black_s, frozen_s, frozen_runs). blackdetect prints
    `black_start:N black_end:N black_duration:N`; the frozen half is measured here from the
    per-frame difference rather than taken from `freezedetect`, whose response to its own threshold
    is not monotone (see FREEZE_YDIF)."""
    black = 0.0
    for line in stderr_text.splitlines():
        if "black_duration:" in line:
            try:
                black += float(line.split("black_duration:")[1].split()[0])
            except (IndexError, ValueError):
                pass
    runs = frozen_runs(parse_ydif_series(stderr_text))
    return black, sum(d for _, d in runs), runs


def parse_ydif_series(stderr_text):
    """[(pts_time, YDIF), ...] from one `signalstats,metadata=print` pass. Pure.

    ffmpeg prints the frame's `pts_time:` on one line and its `lavfi.signalstats.YDIF=` on the next;
    a value without a preceding time is dropped rather than guessed at.
    """
    series = []
    time = None
    for line in stderr_text.splitlines():
        m = _PTS_TIME_RE.search(line)
        if m:
            try:
                time = float(m.group(1))
            except ValueError:
                time = None
            continue
        m = _YDIF_RE.search(line)
        if m and time is not None:
            try:
                series.append((time, float(m.group(1))))
            except ValueError:
                pass
            time = None
    return series


def frozen_runs(series, floor=FREEZE_YDIF, min_s=FREEZE_MIN_S):
    """[(start_s, duration_s), ...] for every run of frames at or below `floor` lasting >= min_s. Pure.

    The run starts at the PREVIOUS frame's timestamp -- the first still frame is still evidence about
    the interval that produced it -- and ends at the frame that finally moved.
    """
    runs = []
    start = None
    for i, (time, ydif) in enumerate(series):
        if ydif <= floor:
            if start is None:
                start = series[i - 1][0] if i else time
        elif start is not None:
            if time - start >= min_s:
                runs.append((start, time - start))
            start = None
    if start is not None and series and series[-1][0] - start >= min_s:
        runs.append((start, series[-1][0] - start))
    return runs


def shot_windows(mission):
    """[(start, end, type), ...] for a parsed mission's camera shots, in authored order. Pure."""
    shots = ((mission or {}).get("cameras") or {}).get("shots") or []
    windows = []
    for shot in shots:
        start = float(shot.get("start", 0.0))
        windows.append((start, start + float(shot.get("duration", 0.0)), str(shot.get("type", "?"))))
    return windows


def frozen_shot_problems(intervals, windows, min_s=2.0, still_types=("static",)):
    """Frozen stretches that a MOVING shot is responsible for. Pure. Returns a list of strings.

    The recorder's timeline is the shot list's, so a freeze timestamp in the video indexes straight
    into the shot that produced it. That is what turns "8.2s frozen" -- a number the coarse
    half-the-run check happily passes -- into "the orbit shot rendered a still image", which is a
    defect every time: an orbiting or moving camera changes the picture by definition, so a frozen
    one means the shot has nothing in it, its target is gone (#1381), or the scene is not lit.

    A `static` shot is exempt: holding a quiet scene still is what it is for. Runs shorter than
    `min_s` are ignored -- a slow pan over a smooth sky legitimately produces a few identical frames,
    and freezedetect is already only called with d=2.
    """
    problems = []
    for start, dur in intervals:
        if dur < min_s:
            continue
        end = start + dur
        covering = [(ws, we, wt) for ws, we, wt in windows if start < we and end > ws]
        moving = [w for w in covering if w[2] not in still_types]
        if not covering or not moving:
            continue
        names = ", ".join(f"{wt} shot at {ws:g}-{we:g}s" for ws, we, wt in moving)
        problems.append(f"{dur:.1f}s frozen at {start:.1f}s, over the {names}")
    return problems


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
    """Decode `path` once for black frames and per-frame difference. Returns (black_s, frozen_s, runs).

    One decode pass answers both questions, and it is cheap next to the recording itself. A
    missing/broken ffmpeg returns (0, 0, []) — the duration check still applies, and openFfmpeg would
    have failed the recording long before this. `runs` is where each still stretch sits, which is what
    lets one be attributed to the shot that produced it (#1378)."""
    try:
        out = subprocess.run(
            [ffmpeg, "-v", "info", "-nostats", "-i", path,
             "-vf", "blackdetect=d=1:pix_th=0.03,signalstats,"
                    "metadata=print:key=lavfi.signalstats.YDIF",
             "-an", "-f", "null", "-"],
            capture_output=True, text=True, timeout=300,
        )
        return parse_content_probe(out.stderr)
    except (subprocess.SubprocessError, FileNotFoundError):
        return 0.0, 0.0, []


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
    # The shot list, for attributing a frozen stretch to the shot that produced it (#1378). Without
    # PyYAML the recording still runs and is still checked; only that attribution is skipped, and
    # main() has already said so once.
    mission_doc = None
    if yaml is not None:
        try:
            mission_doc = yaml.safe_load(Path(mission).read_text(encoding="utf-8"))
        except (yaml.YAMLError, OSError):
            mission_doc = None
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
        black_s, frozen_s, freezes = probe_content(out_path, args.ffmpeg)
        content, creason = content_ok(dur, black_s, frozen_s)
        if not content:
            return False, f"{out_path}: {reason}; {creason}"
        # Attribute each frozen stretch to the shot that produced it (#1378). content_ok is a
        # half-the-run envelope; it passes a demo whose orbit shot rendered a still image, and that
        # is a defect every time -- a moving camera changes the picture by definition.
        moving = frozen_shot_problems(freezes, shot_windows(mission_doc)) if mission_doc else []
        if moving:
            return False, f"{out_path}: {reason}; {creason}; still picture on a moving shot: " + \
                          "; ".join(moving)
        return True, f"{out_path}: {reason}; {creason}"

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

    # Fail before recording anything, not after (#1378). A shot list and its manifest entry that
    # disagree cost a full run to discover: the recording is capped by one number and then judged
    # against the other, so it fails a gate it was never given the wall clock to pass.
    if yaml is None:
        print("[WARN] PyYAML not installed: cannot check demos.json against the shot lists",
              file=sys.stderr)
    else:
        drift = manifest_drift(selected, args.demos_dir)
        if drift:
            print("demos.json disagrees with the shot lists it describes:", file=sys.stderr)
            for problem in drift:
                print(f"  {problem}", file=sys.stderr)
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
