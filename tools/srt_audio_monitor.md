# SRT Audio Monitor

`tools/srt_audio_monitor.py` is a local reproduction harness for SRT playback audio issues. It connects as an SRT caller, decodes video to a null sink, streams decoded audio PCM into Python, and writes a JSON report with volume/dropout metrics.

Target values come from `.env` or environment variables. Do not put private target hosts or URLs in tracked files.

Typical B-frame run:

```bash
python3 tools/srt_audio_monitor.py --duration 30
```

Closer player-output run using `ffplay` and the PulseAudio monitor source:

```bash
python3 tools/srt_audio_monitor.py --backend ffplay-pulse --duration 30
```

If auto-detection picks the wrong Pulse monitor, provide it explicitly:

```bash
python3 tools/srt_audio_monitor.py --backend ffplay-pulse --pulse-source RDPSink.monitor --duration 30
```

Useful environment variables:

```bash
SBS_TARGET_HOST=<target-host>
SBS_SRT_PORT=8888
SBS_SRT_LATENCY_US=600000
```

You can also pass a full SRT URL without saving it:

```bash
python3 tools/srt_audio_monitor.py --url 'srt://<target-host>:8888?mode=caller&latency=600000' --duration 30
```

Compare B-frame and IPP runs by changing encoder config through the WebSocket API, restarting `output-output-1`, then running the monitor with the same duration and thresholds. Reports default to `/tmp/sbs-srt-audio-monitor-*` and include:

- `report.json`: run configuration, summary, dropout intervals, starvation intervals, and artifact paths
- `windows.csv`: per-window RMS/peak/arrival metrics
- `ffmpeg.log`: decoder/player-emulation warnings
- `ffplay.log`: player warnings when using the `ffplay-pulse` backend

The `ffplay-pulse` backend reports Pulse monitor read gaps as `capture_starvation`; these are not automatically treated as audio dropouts because WSLg/Pulse can deliver monitor samples in bursts. Use `summary.audio_volume_healthy`, `dropouts`, RMS values, and `ffplay_metrics` to decide whether the player-output audio actually went silent.

Windows VLC/CoreAudio run:

```bash
host="$SBS_TARGET_HOST"
script_path=$(wslpath -w tools/vlc_audio_meter.ps1)
out_dir=$(wslpath -w /tmp/sbs-vlc-audio-meter)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$script_path" -HostName "$host" -DurationSeconds 30 -OutDir "$out_dir"
```

For VLC, use `srt://<host>:8888` without `?mode=caller&latency=...`. The Windows VLC SRT plugin did not connect with the FFmpeg-style query string in local testing.

Before each player test, restart Output 1 while the canvas is active. The SRT listener can reject or stall new callers until the output is stopped and started:

```bash
python3 - <<'PY'
import json, os, time, websocket
host = os.environ['SBS_TARGET_HOST']
ws = websocket.create_connection(f'ws://{host}:10100/api', timeout=5)
def call(i, method, params=None):
    ws.send(json.dumps({'jsonrpc': '2.0', 'id': i, 'method': method, 'params': params or {}}))
    return ws.recv()
call(1, 'output.stop', {'id': 'output-output-1'})
time.sleep(1)
call(2, 'output.start', {'id': 'output-output-1'})
ws.close()
PY
```

Windows VLC/WASAPI loopback run, which captures the actual default-render-device PCM instead of only session peak volume:

```bash
host="$SBS_TARGET_HOST"
script_path=$(wslpath -w tools/vlc_wasapi_loopback_capture.ps1)
out_dir=$(wslpath -w /tmp/sbs-vlc-wasapi-loopback)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$script_path" -HostName "$host" -DurationSeconds 30 -OutDir "$out_dir"
```

If the FFmpeg monitor does not show dropouts while VLC/OBS still breaks up, the issue is likely closer to player render scheduling or a decoder-specific path. In that case, use this report as the clean baseline and add a closer VLC/mpv/OBS backend rather than changing mux/encoder code blindly.
