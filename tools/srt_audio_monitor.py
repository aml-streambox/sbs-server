#!/usr/bin/env python3
"""Measure decoded audio health from an SRT stream.

The tool connects as an SRT caller through FFmpeg, keeps video decoding active
by sending video to a null sink, and reads decoded PCM audio from stdout. It is
intended for reproducing player-side audio breakup before changing the encoder
or muxer again.
"""

from __future__ import annotations

import argparse
import array
import csv
import json
import math
import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any
from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit


DEFAULT_SAMPLE_RATE = 48000
DEFAULT_CHANNELS = 2
DEFAULT_LATENCY_US = 600000


def load_env_file(path: Path) -> None:
    if not path.exists():
        return

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        if key and key not in os.environ:
            os.environ[key] = value


def redact_srt_url(url: str) -> str:
    parts = urlsplit(url)
    host = "<host>"
    if parts.port:
        host = f"{host}:{parts.port}"
    return urlunsplit((parts.scheme, host, parts.path, parts.query, parts.fragment))


def redact_command(cmd: list[str], url: str) -> list[str]:
    return [redact_srt_url(item) if item == url else item for item in cmd]


def redact_urls_in_text(text: str) -> str:
    def replace(match: re.Match[str]) -> str:
        return redact_srt_url(match.group(0))

    return re.sub(r"srt://\S+", replace, text)


def detect_pulse_monitor(ffmpeg: str) -> str | None:
    try:
        proc = subprocess.run(
            [ffmpeg, "-hide_banner", "-sources", "pulse"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None

    for line in proc.stdout.splitlines():
        match = re.search(r"\b(\S+\.monitor)\b", line)
        if match:
            return match.group(1)
    return None


def parse_ffplay_log(path: Path) -> dict[str, Any] | None:
    if not path.exists():
        return None

    text = path.read_text(encoding="utf-8", errors="replace")
    status_re = re.compile(
        r"(?P<pos>\d+(?:\.\d+)?)\s+A-V:\s*(?P<av>[+-]?(?:\d+(?:\.\d+)?|nan))\s+"
        r"fd=\s*(?P<fd>\d+)\s+aq=\s*(?P<aq>\d+)KB\s+vq=\s*(?P<vq>\d+)KB"
    )
    stats: list[dict[str, Any]] = []
    for match in status_re.finditer(text):
        av_raw = match.group("av")
        stats.append({
            "position_s": float(match.group("pos")),
            "av_sync_s": None if av_raw == "nan" else float(av_raw),
            "frame_drops": int(match.group("fd")),
            "audio_queue_kb": int(match.group("aq")),
            "video_queue_kb": int(match.group("vq")),
        })

    av_values = [s["av_sync_s"] for s in stats if s["av_sync_s"] is not None]
    frame_drops = [s["frame_drops"] for s in stats]
    audio_queues = [s["audio_queue_kb"] for s in stats]
    video_queues = [s["video_queue_kb"] for s in stats]
    error_lines = [
        redact_urls_in_text(line)
        for line in text.splitlines()
        if re.search(r"\b(error|failed|invalid|deadlock)\b", line, re.I)
    ]
    return {
        "status_samples": len(stats),
        "av_sync_s_min": None if not av_values else round(min(av_values), 6),
        "av_sync_s_max": None if not av_values else round(max(av_values), 6),
        "av_sync_s_avg": None if not av_values else round(sum(av_values) / len(av_values), 6),
        "frame_drops_final": None if not frame_drops else frame_drops[-1],
        "frame_drops_max": None if not frame_drops else max(frame_drops),
        "audio_queue_kb_max": None if not audio_queues else max(audio_queues),
        "video_queue_kb_max": None if not video_queues else max(video_queues),
        "error_line_count": len(error_lines),
        "error_lines_sample": error_lines[:20],
    }


def append_query(url: str, extra: dict[str, str]) -> str:
    parts = urlsplit(url)
    query = dict(parse_qsl(parts.query, keep_blank_values=True))
    for key, value in extra.items():
        query.setdefault(key, value)
    return urlunsplit((parts.scheme, parts.netloc, parts.path, urlencode(query), parts.fragment))


def build_srt_url(args: argparse.Namespace) -> str:
    url = args.url or os.environ.get("SBS_SRT_URL")
    latency_us = args.latency_us or int(os.environ.get("SBS_SRT_LATENCY_US", DEFAULT_LATENCY_US))

    if url:
        return append_query(url, {"mode": "caller", "latency": str(latency_us)})

    host = args.host or os.environ.get("SBS_TARGET_HOST") or os.environ.get("TARGET")
    if not host:
        raise SystemExit("Provide --url, --host, SBS_SRT_URL, SBS_TARGET_HOST, or TARGET")

    port = args.port or int(os.environ.get("SBS_SRT_PORT", "8888"))
    return f"srt://{host}:{port}?mode=caller&latency={latency_us}"


def dbfs(value: float) -> float | None:
    if value <= 0:
        return None
    return 20.0 * math.log10(value / 32768.0)


def analyze_window(pcm: bytes, index: int, window_s: float, channels: int, near_zero: int) -> dict[str, Any]:
    samples = array.array("h")
    samples.frombytes(pcm)
    if sys.byteorder != "little":
        samples.byteswap()

    count = len(samples)
    if count == 0:
        rms = 0.0
        peak = 0
        near_zero_count = 0
    else:
        total_sq = 0
        peak = 0
        near_zero_count = 0
        for sample in samples:
            value = int(sample)
            av = abs(value)
            if av > peak:
                peak = av
            if av <= near_zero:
                near_zero_count += 1
            total_sq += value * value
        rms = math.sqrt(total_sq / count)

    rms_dbfs = dbfs(rms)
    peak_dbfs = dbfs(float(peak))
    return {
        "index": index,
        "start_s": round(index * window_s, 6),
        "end_s": round((index + 1) * window_s, 6),
        "frames": count // channels if channels > 0 else count,
        "samples": count,
        "rms": round(rms, 3),
        "rms_dbfs": None if rms_dbfs is None else round(rms_dbfs, 3),
        "peak": peak,
        "peak_dbfs": None if peak_dbfs is None else round(peak_dbfs, 3),
        "near_zero_ratio": round(near_zero_count / count, 6) if count else 1.0,
    }


def detect_dropouts(
    windows: list[dict[str, Any]],
    silence_dbfs: float,
    min_dropout_s: float,
    ignore_leading_silence: bool,
) -> tuple[list[dict[str, Any]], float]:
    dropouts: list[dict[str, Any]] = []
    current: dict[str, Any] | None = None
    analysis_started = not ignore_leading_silence
    leading_silence_s = 0.0

    for window in windows:
        rms_dbfs = window.get("rms_dbfs")
        is_silent = rms_dbfs is None or rms_dbfs <= silence_dbfs
        window["silent"] = is_silent

        if not analysis_started:
            if is_silent:
                window["ignored_leading_silence"] = True
                leading_silence_s = window["end_s"]
                continue
            analysis_started = True
        window["ignored_leading_silence"] = False

        if is_silent:
            if current is None:
                current = {
                    "start_s": window["start_s"],
                    "end_s": window["end_s"],
                    "window_count": 1,
                    "min_rms_dbfs": rms_dbfs,
                }
            else:
                current["end_s"] = window["end_s"]
                current["window_count"] += 1
                if rms_dbfs is None or current["min_rms_dbfs"] is None:
                    current["min_rms_dbfs"] = None
                else:
                    current["min_rms_dbfs"] = min(current["min_rms_dbfs"], rms_dbfs)
        elif current is not None:
            duration = current["end_s"] - current["start_s"]
            if duration >= min_dropout_s:
                current["duration_s"] = round(duration, 6)
                dropouts.append(current)
            current = None

    if current is not None:
        duration = current["end_s"] - current["start_s"]
        if duration >= min_dropout_s:
            current["duration_s"] = round(duration, 6)
            dropouts.append(current)

    return dropouts, round(leading_silence_s, 6)


def write_windows_csv(path: Path, windows: list[dict[str, Any]]) -> None:
    if not windows:
        return
    fields = [
        "index",
        "start_s",
        "end_s",
        "wall_time_s",
        "arrival_gap_s",
        "frames",
        "samples",
        "rms",
        "rms_dbfs",
        "peak",
        "peak_dbfs",
        "near_zero_ratio",
        "silent",
        "ignored_leading_silence",
    ]
    with path.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(windows)


def terminate_process(proc: subprocess.Popen[bytes], grace_s: float = 3.0) -> int | None:
    if proc.poll() is not None:
        return proc.returncode
    proc.terminate()
    try:
        return proc.wait(timeout=grace_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        return proc.wait(timeout=grace_s)


def start_optional_ts_capture(args: argparse.Namespace, url: str, out_dir: Path) -> tuple[subprocess.Popen[bytes] | None, Path | None]:
    if not args.keep_ts:
        return None, None

    ts_path = out_dir / "capture.ts"
    stderr_path = out_dir / "capture.ffmpeg.log"
    cmd = [
        args.ffmpeg,
        "-y",
        "-nostdin",
        "-hide_banner",
        "-loglevel",
        args.ffmpeg_loglevel,
        "-t",
        str(args.duration),
        "-i",
        url,
        "-map",
        "0",
        "-c",
        "copy",
        "-f",
        "mpegts",
        str(ts_path),
    ]
    stderr = stderr_path.open("wb")
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=stderr)
    proc._sbs_stderr_file = stderr  # type: ignore[attr-defined]
    return proc, ts_path


def close_optional_ts_capture(proc: subprocess.Popen[bytes] | None) -> int | None:
    if proc is None:
        return None
    code = terminate_process(proc)
    stderr = getattr(proc, "_sbs_stderr_file", None)
    if stderr:
        stderr.close()
    return code


def run_monitor(args: argparse.Namespace, url: str, out_dir: Path) -> dict[str, Any]:
    sample_rate = args.sample_rate
    channels = args.channels
    sample_bytes = 2
    window_s = args.window_ms / 1000.0
    window_bytes = int(sample_rate * channels * sample_bytes * window_s)
    if window_bytes <= 0:
        raise SystemExit("window size is too small")

    stderr_path = out_dir / "ffmpeg.log"
    player_stderr_path = out_dir / "ffplay.log"
    csv_path = out_dir / "windows.csv"
    report_path = out_dir / "report.json"

    player_cmd: list[str] | None = None
    pulse_source: str | None = None
    if args.backend == "ffplay-pulse":
        pulse_source = args.pulse_source or os.environ.get("SBS_PULSE_MONITOR") or detect_pulse_monitor(args.ffmpeg)
        if not pulse_source:
            raise SystemExit("No PulseAudio monitor source found; pass --pulse-source")
        cmd = [
            args.ffmpeg,
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            args.ffmpeg_loglevel,
            "-f",
            "pulse",
            "-i",
            pulse_source,
            "-ac",
            str(channels),
            "-ar",
            str(sample_rate),
            "-c:a",
            "pcm_s16le",
            "-f",
            "s16le",
            "pipe:1",
        ]
        player_cmd = [
            args.ffplay,
            "-hide_banner",
            "-loglevel",
            args.ffmpeg_loglevel,
        ]
        if args.ffplay_nodisp:
            player_cmd.append("-nodisp")
        player_cmd.append(url)
    else:
        cmd = [
            args.ffmpeg,
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            args.ffmpeg_loglevel,
            "-i",
            url,
            "-map",
            "0:a:0",
            "-ac",
            str(channels),
            "-ar",
            str(sample_rate),
            "-c:a",
            "pcm_s16le",
            "-f",
            "s16le",
            "pipe:1",
        ]
        if args.audio_only:
            cmd.extend(["-map", "0:v:0", "-c:v", "copy", "-f", "null", "/dev/null"])
        else:
            cmd.extend(["-map", "0:v:0", "-f", "null", "/dev/null"])

    ts_proc, ts_path = start_optional_ts_capture(args, url, out_dir)
    stderr_file = stderr_path.open("wb")
    player_stderr_file = None
    player_proc = None
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=stderr_file, bufsize=0)
    if player_cmd:
        time.sleep(args.player_start_delay_ms / 1000.0)
        player_stderr_file = player_stderr_path.open("wb")
        player_proc = subprocess.Popen(
            player_cmd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=player_stderr_file,
        )
    assert proc.stdout is not None
    os.set_blocking(proc.stdout.fileno(), False)

    start = time.monotonic()
    deadline = start + args.duration
    buffer = bytearray()
    windows: list[dict[str, Any]] = []
    starvation_events: list[dict[str, Any]] = []
    total_bytes = 0
    first_audio_wall_s: float | None = None
    last_audio_time: float | None = None
    starve_start: float | None = None
    previous_window_wall_s: float | None = None
    early_exit = False

    try:
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                early_exit = True
                break
            if player_proc is not None and player_proc.poll() is not None:
                early_exit = True
                break

            readable, _, _ = select.select([proc.stdout.fileno()], [], [], 0.05)
            now = time.monotonic()
            if readable:
                try:
                    chunk = os.read(proc.stdout.fileno(), 65536)
                except BlockingIOError:
                    chunk = b""
                if not chunk:
                    early_exit = True
                    break

                if first_audio_wall_s is None:
                    first_audio_wall_s = now - start
                if starve_start is not None:
                    starvation_events.append({
                        "start_wall_s": round(starve_start - start, 6),
                        "end_wall_s": round(now - start, 6),
                        "duration_s": round(now - starve_start, 6),
                    })
                    starve_start = None
                last_audio_time = now
                total_bytes += len(chunk)
                buffer.extend(chunk)

                while len(buffer) >= window_bytes:
                    pcm = bytes(buffer[:window_bytes])
                    del buffer[:window_bytes]
                    window = analyze_window(pcm, len(windows), window_s, channels, args.near_zero)
                    wall_s = time.monotonic() - start
                    window["wall_time_s"] = round(wall_s, 6)
                    if previous_window_wall_s is None:
                        window["arrival_gap_s"] = None
                    else:
                        window["arrival_gap_s"] = round(wall_s - previous_window_wall_s, 6)
                    previous_window_wall_s = wall_s
                    windows.append(window)
            elif last_audio_time is not None and now - last_audio_time >= args.starve_ms / 1000.0:
                if starve_start is None:
                    starve_start = last_audio_time

        now = time.monotonic()
        if starve_start is not None:
            starvation_events.append({
                "start_wall_s": round(starve_start - start, 6),
                "end_wall_s": round(now - start, 6),
                "duration_s": round(now - starve_start, 6),
            })
    finally:
        ffmpeg_exit = terminate_process(proc)
        player_exit = terminate_process(player_proc) if player_proc is not None else None
        stderr_file.close()
        if player_stderr_file is not None:
            player_stderr_file.close()
        ts_exit = close_optional_ts_capture(ts_proc)

    ffplay_metrics = parse_ffplay_log(player_stderr_path) if player_cmd else None
    dropouts, leading_silence_s = detect_dropouts(
        windows,
        args.silence_dbfs,
        args.min_dropout_ms / 1000.0,
        not args.no_ignore_leading_silence,
    )
    rms_values = [w["rms_dbfs"] for w in windows if w.get("rms_dbfs") is not None]
    arrival_gaps = [w["arrival_gap_s"] for w in windows if w.get("arrival_gap_s") is not None]
    ffplay_failed = bool(
        ffplay_metrics
        and ffplay_metrics["error_line_count"] > 0
        and ffplay_metrics["status_samples"] == 0
    )
    capture_timing_unhealthy = bool(starvation_events)
    audio_volume_unhealthy = bool(dropouts or not windows)
    player_unhealthy = bool(early_exit or ffplay_failed)
    unhealthy = bool(audio_volume_unhealthy or player_unhealthy)

    report: dict[str, Any] = {
        "schema_version": 1,
        "tool": "srt_audio_monitor.py",
        "input": {
            "url_redacted": redact_srt_url(url),
            "backend": args.backend,
            "duration_s": args.duration,
            "sample_rate": sample_rate,
            "channels": channels,
            "window_ms": args.window_ms,
            "video_decode_active": not args.audio_only,
            "ffmpeg": shutil.which(args.ffmpeg) or args.ffmpeg,
            "ffplay": shutil.which(args.ffplay) or args.ffplay if args.backend == "ffplay-pulse" else None,
            "pulse_source": pulse_source,
        },
        "thresholds": {
            "silence_dbfs": args.silence_dbfs,
            "min_dropout_ms": args.min_dropout_ms,
            "starve_ms": args.starve_ms,
            "near_zero_sample": args.near_zero,
        },
        "summary": {
            "healthy": not unhealthy,
            "audio_volume_healthy": not audio_volume_unhealthy,
            "player_healthy": not player_unhealthy,
            "capture_timing_healthy": not capture_timing_unhealthy,
            "windows": len(windows),
            "pcm_bytes": total_bytes,
            "pcm_duration_s": round((total_bytes / (sample_rate * channels * sample_bytes)), 6),
            "first_audio_wall_s": None if first_audio_wall_s is None else round(first_audio_wall_s, 6),
            "leading_silence_s": leading_silence_s,
            "dropout_count": len(dropouts),
            "starvation_count": len(starvation_events),
            "rms_dbfs_min": None if not rms_values else round(min(rms_values), 3),
            "rms_dbfs_max": None if not rms_values else round(max(rms_values), 3),
            "rms_dbfs_avg": None if not rms_values else round(sum(rms_values) / len(rms_values), 3),
            "arrival_gap_s_max": None if not arrival_gaps else round(max(arrival_gaps), 6),
            "arrival_gap_s_avg": None if not arrival_gaps else round(sum(arrival_gaps) / len(arrival_gaps), 6),
            "early_exit": early_exit,
            "ffmpeg_exit_code": ffmpeg_exit,
            "player_exit_code": player_exit,
            "ts_capture_exit_code": ts_exit,
        },
        "ffplay_metrics": ffplay_metrics,
        "dropouts": dropouts,
        "starvation_events": starvation_events,
        "artifacts": {
            "report_json": str(report_path),
            "windows_csv": str(csv_path) if not args.no_csv else None,
            "ffmpeg_log": str(stderr_path),
            "ffplay_log": str(player_stderr_path) if player_cmd else None,
            "capture_ts": str(ts_path) if ts_path else None,
        },
        "commands": {
            "monitor_ffmpeg_redacted": redact_command(cmd, url),
            "player_ffplay_redacted": redact_command(player_cmd, url) if player_cmd else None,
        },
    }

    if not args.no_csv:
        write_windows_csv(csv_path, windows)
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Monitor decoded audio health from an SRT stream")
    parser.add_argument("--env-file", type=Path, default=Path(".env"), help="optional env file to load before resolving target values")
    parser.add_argument("--url", help="full SRT URL; otherwise SBS_SRT_URL or host/port env is used")
    parser.add_argument("--host", help="target host; otherwise SBS_TARGET_HOST or TARGET is used")
    parser.add_argument("--port", type=int, help="SRT port when constructing URL, default 8888")
    parser.add_argument("--latency-us", type=int, help="SRT latency query value in microseconds")
    parser.add_argument("--duration", type=float, default=20.0, help="wall-clock monitor duration in seconds")
    parser.add_argument("--out-dir", type=Path, help="output directory; default is a temporary directory")
    parser.add_argument("--backend", choices=("ffmpeg-pipe", "ffplay-pulse"), default="ffmpeg-pipe", help="monitor backend")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="ffmpeg executable")
    parser.add_argument("--ffplay", default="ffplay", help="ffplay executable for ffplay-pulse backend")
    parser.add_argument("--ffmpeg-loglevel", default="warning", help="ffmpeg loglevel")
    parser.add_argument("--pulse-source", help="PulseAudio monitor source for ffplay-pulse backend")
    parser.add_argument("--ffplay-nodisp", action="store_true", help="run ffplay without video display; use only as a control")
    parser.add_argument("--player-start-delay-ms", type=float, default=250.0, help="delay after starting Pulse capture before starting ffplay")
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--channels", type=int, default=DEFAULT_CHANNELS)
    parser.add_argument("--window-ms", type=float, default=50.0, help="PCM analysis window size")
    parser.add_argument("--silence-dbfs", type=float, default=-55.0, help="RMS threshold for silent windows")
    parser.add_argument("--min-dropout-ms", type=float, default=120.0, help="minimum consecutive silence duration to report dropout")
    parser.add_argument("--starve-ms", type=float, default=200.0, help="wall-clock no-audio timeout to report starvation")
    parser.add_argument("--near-zero", type=int, default=2, help="absolute sample value considered near zero")
    parser.add_argument("--keep-ts", action="store_true", help="also run a concurrent ffmpeg MPEG-TS copy capture")
    parser.add_argument("--no-csv", action="store_true", help="skip writing per-window CSV")
    parser.add_argument("--no-ignore-leading-silence", action="store_true", help="count startup silence as a dropout")
    parser.add_argument("--audio-only", action="store_true", help="do not actively decode video; use only as a control")
    parser.add_argument("--fail-on-dropout", action="store_true", help="exit non-zero when report is unhealthy")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    load_env_file(args.env_file)

    if shutil.which(args.ffmpeg) is None:
        raise SystemExit(f"ffmpeg not found: {args.ffmpeg}")
    if args.backend == "ffplay-pulse" and shutil.which(args.ffplay) is None:
        raise SystemExit(f"ffplay not found: {args.ffplay}")

    url = build_srt_url(args)
    out_dir = args.out_dir
    if out_dir is None:
        out_dir = Path(tempfile.mkdtemp(prefix="sbs-srt-audio-monitor-"))
    else:
        out_dir.mkdir(parents=True, exist_ok=True)

    report = run_monitor(args, url, out_dir)
    summary = report["summary"]
    print(f"report: {report['artifacts']['report_json']}")
    print(
        "healthy={healthy} audio_volume_healthy={audio_volume_healthy} windows={windows} "
        "pcm_duration={pcm_duration_s:.3f}s dropouts={dropout_count} "
        "capture_starvation={starvation_count} rms_avg={rms_dbfs_avg}".format(**summary)
    )
    return 2 if args.fail_on_dropout and not summary["healthy"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
