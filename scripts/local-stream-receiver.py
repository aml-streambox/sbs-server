#!/usr/bin/env python3
"""FFmpeg-backed local ingest receiver for SBS output validation.

The receiver intentionally keeps all runtime state under artifacts/ so local
endpoints, logs, and PIDs are not tracked. It does not store or require real
stream keys; use disposable local keys when testing stream-key plumbing.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_STATE_DIR = ROOT_DIR / "artifacts" / "local-stream-receiver"
STATE_FILENAME = "receiver-state.json"
CONFIG_FILENAME = "receiver-config.json"
SUPERVISOR_LOG = "supervisor.log"
DEFAULT_RTMP_APP = "live"
DEFAULT_RTMP_STREAM = "test"


def utc_now() -> str:
    return _dt.datetime.now(tz=_dt.timezone.utc).isoformat(timespec="seconds")


def die(message: str, code: int = 1) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(code)


def state_file(state_dir: Path) -> Path:
    return state_dir / STATE_FILENAME


def config_file(state_dir: Path) -> Path:
    return state_dir / CONFIG_FILENAME


def load_json(path: Path) -> dict[str, Any] | None:
    try:
        with path.open("r", encoding="utf-8") as fh:
            data = json.load(fh)
    except FileNotFoundError:
        return None
    except json.JSONDecodeError as exc:
        die(f"Failed to parse {path}: {exc}")
    if not isinstance(data, dict):
        die(f"Invalid JSON object in {path}")
    return data


def write_json_atomic(path: Path, data: dict[str, Any]) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2, sort_keys=True)
        fh.write("\n")
    os.replace(tmp, path)


def pid_alive(pid: int | None) -> bool:
    if not pid or pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def terminate_process_group(pid: int, timeout: float = 5.0) -> None:
    if not pid_alive(pid):
        return
    try:
        os.killpg(pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    except PermissionError:
        os.kill(pid, signal.SIGTERM)

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not pid_alive(pid):
            return
        time.sleep(0.1)

    try:
        os.killpg(pid, signal.SIGKILL)
    except ProcessLookupError:
        return
    except PermissionError:
        os.kill(pid, signal.SIGKILL)


def receiver_set(selection: str) -> list[str]:
    if selection == "all":
        return ["srt", "rtmp"]
    return [selection]


def display_host(bind: str) -> str:
    if bind in ("", "0.0.0.0", "::", "[::]"):
        return "<receiver-host>"
    return bind


def srt_input_url(config: dict[str, Any]) -> str:
    bind = config["bind"]
    port = int(config["srt_port"])
    latency_us = int(config["srt_latency_ms"]) * 1000
    return f"srt://{bind}:{port}?mode=listener&transtype=live&latency={latency_us}"


def rtmp_input_url(config: dict[str, Any]) -> str:
    bind = config["bind"]
    port = int(config["rtmp_port"])
    app = config["rtmp_app"].strip("/") or DEFAULT_RTMP_APP
    stream = config["rtmp_stream"].strip("/") or DEFAULT_RTMP_STREAM
    return f"rtmp://{bind}:{port}/{app}/{stream}"


def snapshot_path(kind: str, config: dict[str, Any]) -> Path:
    return Path(config["state_dir"]) / "snapshots" / f"{kind}-latest.jpg"


def srt_client_hint(config: dict[str, Any]) -> str:
    host = display_host(config["bind"])
    port = int(config["srt_port"])
    return f"srt://{host}:{port}/live/test"


def rtmp_client_hint(config: dict[str, Any]) -> tuple[str, str]:
    host = display_host(config["bind"])
    port = int(config["rtmp_port"])
    app = config["rtmp_app"].strip("/") or DEFAULT_RTMP_APP
    stream = config["rtmp_stream"].strip("/") or DEFAULT_RTMP_STREAM
    return f"rtmp://{host}:{port}/{app}", stream


def ffmpeg_command(kind: str, config: dict[str, Any]) -> list[str]:
    ffmpeg = config["ffmpeg"]
    loglevel = config["loglevel"]
    common = [
        ffmpeg,
        "-hide_banner",
        "-nostdin",
        "-y",
        "-loglevel",
        loglevel,
    ]
    demux = [
        "-fflags",
        "+genpts",
        "-analyzeduration",
        "10M",
        "-probesize",
        "10M",
    ]
    output = ["-map", "0", "-c", "copy", "-f", "null", "-"]
    if config.get("snapshots", True):
        interval = max(float(config["snapshot_interval_sec"]), 0.1)
        output += [
            "-map",
            "0:v:0",
            "-an",
            "-vf",
            f"fps=1/{interval:g}",
            "-q:v",
            str(int(config["snapshot_quality"])),
            "-update",
            "1",
            str(snapshot_path(kind, config)),
        ]
    if kind == "srt":
        return common + demux + ["-i", srt_input_url(config)] + output
    if kind == "rtmp":
        return common + ["-listen", "1"] + demux + ["-i", rtmp_input_url(config)] + output
    raise ValueError(kind)


def command_for_log(argv: list[str]) -> str:
    return " ".join(shlex_quote(part) for part in argv)


def shlex_quote(value: str) -> str:
    if value and all(ch.isalnum() or ch in "@%_+=:,./-" for ch in value):
        return value
    return "'" + value.replace("'", "'\"'\"'") + "'"


def check_ffmpeg(config: dict[str, Any]) -> None:
    ffmpeg = config["ffmpeg"]
    if shutil.which(ffmpeg) is None:
        die(f"FFmpeg executable not found: {ffmpeg}")
    try:
        proc = subprocess.run(
            [ffmpeg, "-hide_banner", "-protocols"],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=10,
        )
    except subprocess.SubprocessError as exc:
        die(f"Failed to inspect FFmpeg protocols: {exc}")
    protocols = {line.strip() for line in proc.stdout.splitlines()}
    missing = [kind for kind in config["receivers"] if kind not in protocols]
    if missing:
        die(f"FFmpeg does not list required input protocol(s): {', '.join(missing)}")


class ReceiverProcess:
    def __init__(self, kind: str, config: dict[str, Any], state_dir: Path) -> None:
        self.kind = kind
        self.config = config
        self.state_dir = state_dir
        self.process: subprocess.Popen[bytes] | None = None
        self.log_fh: Any | None = None
        self.restart_count = 0
        self.last_exit: int | None = None
        self.last_start: str | None = None
        self.log_path = state_dir / f"{kind}.ffmpeg.log"

    def start(self) -> None:
        self.close_log()
        self.log_fh = self.log_path.open("ab", buffering=0)
        cmd = ffmpeg_command(self.kind, self.config)
        header = (
            f"\n[{utc_now()}] starting {self.kind} receiver\n"
            f"command: {command_for_log(cmd)}\n"
        )
        self.log_fh.write(header.encode("utf-8"))
        self.process = subprocess.Popen(
            cmd,
            stdin=subprocess.DEVNULL,
            stdout=self.log_fh,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        self.restart_count += 1
        self.last_exit = None
        self.last_start = utc_now()

    def poll(self) -> int | None:
        if not self.process:
            return None
        return self.process.poll()

    def ensure_running(self) -> None:
        if not self.process:
            self.start()
            return
        rc = self.process.poll()
        if rc is not None:
            self.last_exit = rc
            self.close_log()
            self.process = None
            time.sleep(float(self.config["restart_delay_sec"]))
            self.start()

    def stop(self) -> None:
        if self.process and self.process.poll() is None:
            terminate_process_group(self.process.pid, timeout=3.0)
            self.process.poll()
        self.close_log()

    def close_log(self) -> None:
        if self.log_fh:
            self.log_fh.close()
            self.log_fh = None

    def state(self) -> dict[str, Any]:
        pid = self.process.pid if self.process else None
        alive = self.process is not None and self.process.poll() is None
        snap_path = snapshot_path(self.kind, self.config)
        snapshot = {
            "enabled": bool(self.config.get("snapshots", True)),
            "path": str(snap_path),
            "exists": snap_path.exists(),
            "size": snap_path.stat().st_size if snap_path.exists() else 0,
            "mtime": _dt.datetime.fromtimestamp(
                snap_path.stat().st_mtime,
                tz=_dt.timezone.utc,
            ).isoformat(timespec="seconds") if snap_path.exists() else None,
        }
        if self.kind == "srt":
            listen_url = srt_input_url(self.config)
            hint = {
                "sbs_srt_uri": srt_client_hint(self.config),
                "sbs_srt_mode": "caller",
                "dummy_srt_stream_key": "?streamname=local-test&key=local-test&schedule=srt",
            }
        else:
            rtmp_uri, passcode = rtmp_client_hint(self.config)
            listen_url = rtmp_input_url(self.config)
            hint = {
                "sbs_rtmp_uri": rtmp_uri,
                "sbs_rtmp_passcode": passcode,
                "sbs_rtmp_plugin": "streambox",
                "sbs_codec": "h265",
            }
        return {
            "enabled": True,
            "alive": alive,
            "pid": pid,
            "last_exit": self.last_exit,
            "last_start": self.last_start,
            "restart_count": self.restart_count,
            "ffmpeg_input": listen_url,
            "log": str(self.log_path),
            "snapshot": snapshot,
            "hint": hint,
        }


def build_state(config: dict[str, Any], state_dir: Path,
                receivers: dict[str, ReceiverProcess], status: str) -> dict[str, Any]:
    return {
        "status": status,
        "supervisor_pid": os.getpid(),
        "updated_at": utc_now(),
        "state_dir": str(state_dir),
        "profile": config["profile"],
        "receivers": {kind: proc.state() for kind, proc in receivers.items()},
    }


def supervise(args: argparse.Namespace) -> int:
    cfg = load_json(Path(args.config_file))
    if not cfg:
        die(f"Missing config file: {args.config_file}")
    state_dir = Path(cfg["state_dir"])
    receivers = {kind: ReceiverProcess(kind, cfg, state_dir) for kind in cfg["receivers"]}
    running = True

    def handle_stop(signum: int, _frame: Any) -> None:
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, handle_stop)
    signal.signal(signal.SIGINT, handle_stop)

    try:
        while running:
            for proc in receivers.values():
                proc.ensure_running()
            write_json_atomic(state_file(state_dir), build_state(cfg, state_dir, receivers, "running"))
            time.sleep(1.0)
    finally:
        for proc in receivers.values():
            proc.stop()
        write_json_atomic(state_file(state_dir), build_state(cfg, state_dir, receivers, "stopped"))
    return 0


def stop_existing(state_dir: Path, quiet: bool = False) -> None:
    state = load_json(state_file(state_dir))
    if not state:
        if not quiet:
            print("No receiver state found.")
        return
    pid = int(state.get("supervisor_pid") or 0)
    if pid_alive(pid):
        terminate_process_group(pid)
        if not quiet:
            print(f"Stopped receiver supervisor pid {pid}.")
    else:
        if not quiet:
            print(f"Receiver supervisor pid {pid or '<unknown>'} is not running.")
    receivers = state.get("receivers") if isinstance(state.get("receivers"), dict) else {}
    for info in receivers.values():
        if isinstance(info, dict):
            child_pid = int(info.get("pid") or 0)
            if pid_alive(child_pid):
                terminate_process_group(child_pid, timeout=2.0)
            info["alive"] = False
    state["status"] = "stopped"
    state["updated_at"] = utc_now()
    write_json_atomic(state_file(state_dir), state)


def validate_config(config: dict[str, Any]) -> None:
    if "srt" in config["receivers"] and "rtmp" in config["receivers"]:
        if int(config["srt_port"]) == int(config["rtmp_port"]):
            die("SRT and RTMP cannot listen on the same port in one receiver instance.")
    if int(config["srt_port"]) <= 0 or int(config["srt_port"]) > 65535:
        die("Invalid SRT port.")
    if int(config["rtmp_port"]) <= 0 or int(config["rtmp_port"]) > 65535:
        die("Invalid RTMP port.")
    if int(config["srt_latency_ms"]) <= 0:
        die("SRT latency must be positive.")
    if int(config["snapshot_quality"]) < 2 or int(config["snapshot_quality"]) > 31:
        die("Snapshot quality must be between 2 and 31.")


def start(args: argparse.Namespace) -> int:
    state_dir = Path(args.state_dir).resolve()
    state_dir.mkdir(parents=True, exist_ok=True)
    (state_dir / "snapshots").mkdir(parents=True, exist_ok=True)

    if args.replace:
        stop_existing(state_dir, quiet=True)
    else:
        current = load_json(state_file(state_dir))
        pid = int(current.get("supervisor_pid") or 0) if current else 0
        if pid_alive(pid):
            die(f"Receiver already running with supervisor pid {pid}. Use --replace or stop first.")

    config = {
        "state_dir": str(state_dir),
        "profile": args.profile,
        "receivers": receiver_set(args.receiver),
        "bind": args.bind,
        "srt_port": args.srt_port,
        "rtmp_port": args.rtmp_port,
        "rtmp_app": args.rtmp_app,
        "rtmp_stream": args.rtmp_stream,
        "srt_latency_ms": args.srt_latency_ms,
        "ffmpeg": args.ffmpeg,
        "loglevel": args.loglevel,
        "restart_delay_sec": args.restart_delay_sec,
        "snapshots": not args.no_snapshots,
        "snapshot_interval_sec": args.snapshot_interval_sec,
        "snapshot_quality": args.snapshot_quality,
    }
    validate_config(config)
    check_ffmpeg(config)
    write_json_atomic(config_file(state_dir), config)

    supervisor_log = state_dir / SUPERVISOR_LOG
    with supervisor_log.open("ab", buffering=0) as log_fh:
        cmd = [sys.executable, str(Path(__file__).resolve()), "supervise", "--config-file", str(config_file(state_dir))]
        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.DEVNULL,
            stdout=log_fh,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )

    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        state = load_json(state_file(state_dir))
        if state and int(state.get("supervisor_pid") or 0) == proc.pid:
            break
        if proc.poll() is not None:
            die(f"Receiver supervisor exited early with code {proc.returncode}; see {supervisor_log}")
        time.sleep(0.2)

    print(f"Local stream receiver started: supervisor pid {proc.pid}")
    print(f"State: {state_file(state_dir)}")
    print(f"Logs:  {state_dir}")
    print_hints(config)
    return 0


def stop(args: argparse.Namespace) -> int:
    stop_existing(Path(args.state_dir).resolve())
    return 0


def status(args: argparse.Namespace) -> int:
    state_dir = Path(args.state_dir).resolve()
    state = load_json(state_file(state_dir))
    if not state:
        if args.json:
            print(json.dumps({"status": "not-running"}, indent=2, sort_keys=True))
        else:
            print("Local stream receiver is not running.")
        return 1

    pid = int(state.get("supervisor_pid") or 0)
    state["supervisor_alive"] = pid_alive(pid)
    receivers = state.get("receivers") if isinstance(state.get("receivers"), dict) else {}
    for info in receivers.values():
        if isinstance(info, dict):
            info["alive"] = pid_alive(int(info.get("pid") or 0))

    if args.json:
        print(json.dumps(state, indent=2, sort_keys=True))
        return 0 if state["supervisor_alive"] else 1

    print(f"Status: {state.get('status', 'unknown')} supervisor_pid={pid} alive={state['supervisor_alive']}")
    print(f"State: {state_file(state_dir)}")
    for kind, info in receivers.items():
        if not isinstance(info, dict):
            continue
        print(f"{kind}: pid={info.get('pid')} alive={info.get('alive')} restarts={info.get('restart_count')} last_exit={info.get('last_exit')}")
        print(f"  input: {info.get('ffmpeg_input')}")
        print(f"  log:   {info.get('log')}")
        snapshot = info.get("snapshot") if isinstance(info.get("snapshot"), dict) else {}
        print(f"  snapshot: {snapshot.get('path')} exists={snapshot.get('exists')} size={snapshot.get('size')}")
        hint = info.get("hint") if isinstance(info.get("hint"), dict) else {}
        for key, value in hint.items():
            print(f"  {key}: {value}")
    return 0 if state["supervisor_alive"] else 1


def logs(args: argparse.Namespace) -> int:
    state_dir = Path(args.state_dir).resolve()
    state = load_json(state_file(state_dir)) or {}
    receivers = receiver_set(args.receiver)
    paths: list[Path] = []
    if args.receiver in ("all", "supervisor"):
        paths.append(state_dir / SUPERVISOR_LOG)
    if args.receiver != "supervisor":
        for kind in receivers:
            paths.append(state_dir / f"{kind}.ffmpeg.log")

    found = False
    for path in paths:
        if not path.exists():
            continue
        found = True
        print(f"==> {path} <==")
        for line in tail_lines(path, args.lines):
            print(line, end="" if line.endswith("\n") else "\n")
    if not found:
        print("No receiver logs found.")
        return 1
    return 0


def tail_lines(path: Path, limit: int) -> list[str]:
    if limit <= 0:
        return []
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        lines = fh.readlines()
    return lines[-limit:]


def print_hints(config: dict[str, Any]) -> None:
    if "srt" in config["receivers"]:
        print("SRT caller test:")
        print(f"  srt_uri={srt_client_hint(config)}")
        print("  srt_mode=caller")
        print("  optional dummy srt_stream_key=?streamname=local-test&key=local-test&schedule=srt")
    if "rtmp" in config["receivers"]:
        rtmp_uri, passcode = rtmp_client_hint(config)
        print("Enhanced-FLV RTMP test:")
        print(f"  rtmp_uri={rtmp_uri}")
        print(f"  rtmp_passcode={passcode}")
        print("  rtmp_plugin=streambox")
        print("  codec=h265")


def add_common_state_arg(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--state-dir",
        default=str(DEFAULT_STATE_DIR),
        help="runtime state/log directory (default: artifacts/local-stream-receiver)",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run local FFmpeg-backed SRT/RTMP ingest receivers for SBS validation.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("start", help="start receiver supervisor in the background")
    add_common_state_arg(p)
    p.add_argument("--replace", action="store_true", help="stop an existing receiver first")
    p.add_argument("--receiver", choices=["all", "srt", "rtmp"], default="all")
    p.add_argument("--profile", choices=["generic", "bilibili", "twitch", "youtube"], default="generic")
    p.add_argument("--bind", default="0.0.0.0", help="listen address for FFmpeg receivers")
    p.add_argument("--srt-port", type=int, default=8888)
    p.add_argument("--rtmp-port", type=int, default=1935)
    p.add_argument("--rtmp-app", default=DEFAULT_RTMP_APP)
    p.add_argument("--rtmp-stream", default=DEFAULT_RTMP_STREAM)
    p.add_argument("--srt-latency-ms", type=int, default=600)
    p.add_argument("--ffmpeg", default=os.environ.get("FFMPEG", "ffmpeg"))
    p.add_argument("--loglevel", default="info", choices=["quiet", "error", "warning", "info", "verbose", "debug"])
    p.add_argument("--restart-delay-sec", type=float, default=1.0)
    p.add_argument("--no-snapshots", action="store_true", help="disable latest-frame JPEG extraction")
    p.add_argument("--snapshot-interval-sec", type=float, default=5.0, help="seconds between latest-frame JPEG updates")
    p.add_argument("--snapshot-quality", type=int, default=4, help="JPEG qscale for snapshots, 2 is high quality")
    p.set_defaults(func=start)

    p = sub.add_parser("stop", help="stop receiver supervisor")
    add_common_state_arg(p)
    p.set_defaults(func=stop)

    p = sub.add_parser("status", help="show receiver state and SBS config hints")
    add_common_state_arg(p)
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=status)

    p = sub.add_parser("logs", help="show recent receiver logs")
    add_common_state_arg(p)
    p.add_argument("--receiver", choices=["all", "srt", "rtmp", "supervisor"], default="all")
    p.add_argument("--lines", type=int, default=80)
    p.set_defaults(func=logs)

    p = sub.add_parser("supervise", help="internal supervisor loop")
    p.add_argument("--config-file", required=True)
    p.set_defaults(func=supervise)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return int(args.func(args) or 0)


if __name__ == "__main__":
    raise SystemExit(main())
