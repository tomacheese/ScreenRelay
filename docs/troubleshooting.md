# Troubleshooting

Fatal startup errors are printed to stderr as `[ERROR] <message>` and the process exits with code 1.
Runtime errors are written to the log as `"event":"error"` entries with a `code` field (see [Reading the log](#reading-the-log) and [metrics.md](./metrics.md)).

RTSP URLs in this document assume `rtsp.path_pattern` is `/screen{n}` (the value in `config/config.example.json`).
The built-in default is `/live/screen{n}`, so adjust the URLs if you did not set `path_pattern`.

---

## 1. Startup and configuration errors

### `Cannot open config file: <path>`

The config file was not found or cannot be read.

- Check the path given with `--config` (or `-c`).
- The default is `config/config.json`.
- A relative path is resolved against the current working directory, not the executable's directory.

```bash
screen-relay.exe --config C:\path\to\config.json
```

### `JSON parse error: ...` / `Config type error ...` / `Config read error: ...`

The config file is not valid JSON, or a field has the wrong type.

- Check the JSON syntax.
- Check that each field has the type shown in [configuration.md](./configuration.md).

### Validation errors (for example `rtsp.base_url must start with rtsp://`)

The file parsed, but a value was rejected.
The message names the offending key, for example:

- `rtsp.base_url must start with rtsp://` (`rtsp.base_url` is the only key without a usable default)
- `encoder.fps must be between 1 and 240`
- `encoder.bitrate_kbps must be positive`
- `capture.backend must be "dxgi" (only dxgi is currently supported)`
- `rtsp.reconnect_max_delay_ms must be >= rtsp.reconnect_delay_ms`
- `audio.source must be "loopback" or "capture"` (only checked when `audio.enabled` is `true`)

See [configuration.md](./configuration.md) for every rule.

### `Supervisor init failed: Failed to init log sink: ...`

The log directory could not be created or the log file could not be opened.

- `log_dir` (and any missing parent directories) is created automatically, so check that the process may write to that location.
- Check that the path is not occupied by a file and that the disk has free space.

---

## 2. Capture (DXGI)

### `DXGI_INIT_FAILED`

DXGI Desktop Duplication could not be initialized for a monitor.
The initialization is retried up to 15 times at 200 ms intervals before the error is logged, and the pipeline then enters `FATAL`.
The supervisor recreates a `FATAL` pipeline on its next monitor poll (`pipeline_restart` event), so the error may repeat.

The `message` field ends with the last DXGI error (for example `DuplicateOutput failed: HRESULT=...` or `No matching DXGI output found for the specified monitor`).

| Cause | Action |
|---|---|
| No monitor is connected, or the monitor is not attached to a GPU output | Connect a physical or virtual monitor |
| `DuplicateOutput` fails | Make sure the GPU driver supports DXGI 1.2 or later |
| Another process holds Desktop Duplication for the same output | Close competing capture software |
| Running in session 0 (service) or on a locked / disconnected desktop | Run in an interactive desktop session |

### `FIRST_FRAME_TIMEOUT`

Capture was initialized, but no first frame arrived within `capture.frame_timeout_ms` × 100 (10 seconds with the default 100 ms).
The pipeline enters `FATAL` and is recreated by the supervisor.
Check for DRM-protected content on that monitor, a stuck GPU driver, or another process capturing the output.

### `DXGI_HARD_ERROR` / `DXGI_REINIT_FAILED`

`DXGI_HARD_ERROR` means `AcquireNextFrame` failed (for example `ACCESS_LOST` after a mode change, UAC / secure desktop switch or driver reset).
The pipeline moves to `RECONFIGURING` automatically and re-initializes capture, retrying up to 15 times at 200 ms intervals.
If that fails, `DXGI_REINIT_FAILED` is logged and the pipeline enters `FATAL`, after which the supervisor recreates it.

### The picture is frozen or black

- A static screen is normal: Desktop Duplication only returns a frame when something changes, so the last frame is re-sent at the configured fps.
- DRM-protected content (for example Netflix) is captured as black.
  Hide the content or move it to another monitor.
- Update the GPU driver if `DXGI_HARD_ERROR` keeps recurring.

### Colors are wrong (red and blue swapped)

Desktop Duplication outputs **BGRA**, not RGBA.
When changing the conversion code, use `AV_PIX_FMT_BGRA` as the input pixel format of `swscale`.

### A monitor is not detected

- Check that the display shows up in Windows display settings; a monitor must be active (extended or the source of a duplicated display).
- Mirroring-driver devices (`DISPLAY_DEVICE_MIRRORING_DRIVER`) are intentionally ignored.
- Virtual monitors (for example IDD drivers) must still appear as `\\.\DISPLAYn` devices.
- Look for `monitor_added` / `monitor_removed` events in the log.

---

## 3. Encoder

### `ENCODER_INIT_FAILED`

Every codec in the chain (`encoder.codec` followed by `encoder.fallback_codecs`) failed.
The `message` field names the last failure, for example `libx264: avcodec_open2 failed: ...` or `No encoder found for 'h264_nvenc' or any fallback`.
The pipeline enters `FATAL` and is recreated by the supervisor on its next poll.

| Codec | Check |
|---|---|
| `h264_nvenc` | An NVIDIA GPU is present, the driver is current, and the NVENC session limit of consumer GPUs (2-3) has not been reached |
| `h264_mf` | Media Foundation is available (standard on Windows 10 / 11), and the FFmpeg build includes `h264_mf` |
| `libx264` | The FFmpeg build includes libx264 (a GPL component, normally absent from LGPL builds) |

To list the H.264 encoders of your FFmpeg build:

```bash
ffmpeg.exe -encoders | grep h264
```

The `encoder_initialized` event shows which codec was actually selected, and `gpu_zero_copy_mode` shows whether the NVENC zero-copy path is active.

### `ENCODER_ENCODE_FAILED`

`encode()` returned an error while streaming.
`frames_dropped` is incremented and the pipeline enters `FATAL`; the supervisor recreates it.

### Poor quality or high latency

- Increase `encoder.bitrate_kbps` (8000 or more is recommended for high resolutions).
- Keep `encoder.max_b_frames` at `0`; B-frames add latency.
- `encoder.preset` is passed to the selected encoder, and preset names are encoder specific (for example `fast` / `ultrafast` for `libx264`).
- `encoder.tune` (`zerolatency`) only applies to software encoders such as `libx264`; NVENC is configured with `rc=cbr` and `delay=0` instead.

---

## 4. RTSP

### `RTSP_CONNECT_FAILED`

The first connection to the RTSP server failed.
The `message` field contains the FFmpeg error (for example `RTSP connect (write_header) failed: ...`) and the log entry carries `url` and `monitor`.
The pipeline returns to `CAPTURING` and retries after `rtsp.reconnect_delay_ms`.

1. Check that MediaMTX (or your RTSP server) is running.
2. Check the host and port in `rtsp.base_url`.
3. Check that the firewall allows the port (8554 by default).
4. Check connectivity with `ping` or `telnet`.
5. A timeout is governed by `rtsp.connect_timeout_ms`.

```bash
# Port check
telnet 192.168.0.100 8554
```

### `RTSP_SEND_FAILED`

Sending a packet failed while streaming, or the audio packet could not be sent.
For video, the pipeline enters `RECONNECTING` and `rtsp_errors` is incremented.

- **Insufficient bandwidth**: lower `encoder.bitrate_kbps`.
- **Timeouts**: increase `rtsp.send_timeout_ms` (default 5000 ms).
- **Unstable network**: use a wired connection.
- **Server side**: check the MediaMTX log, and increase its write queue (`writeQueueSize`, `readBufferCount` in older versions).

### `RTSP_RECONNECT_FAILED`

A reconnect attempt failed.
The pipeline keeps retrying with exponential backoff; each attempt is logged as a `reconnecting` event with `attempt` and `delay_ms`.

- Check `rtsp.reconnect_delay_ms`, `rtsp.reconnect_max_delay_ms` and `rtsp.reconnect_backoff_multiplier`.
- Check the MediaMTX session timeout settings and the network quality (packet loss, jitter).
- The primary monitor publishes two streams (`screen{n}` and `screen0`); a failure of either one reconnects both.

### VLC cannot play the stream

1. Check that MediaMTX is running and the stream is being published (`publish_started` in the log).
2. Check that the `{n}` in the URL is the Windows Settings display number of the monitor; `screen0` is the primary monitor.
3. In VLC, choose "Open Network Stream" and enter a URL such as:

```
rtsp://192.168.0.100:8554/screen1
```

4. Lowering the network cache under "Tools" → "Preferences" → "Input / Codecs" reduces latency.

---

## 5. Audio (when `audio.enabled` is `true`)

### `AUDIO_INIT_FAILED`

The audio pipeline could not start, and screen-relay continues without audio.
The `message` field ends with the reason, for example `Audio device not found: <id>` or `Invalid audio.source: ...`.

- List the available endpoints and copy the ID of the device you want into `audio.device_id`:

```bash
screen-relay.exe --list-audio-devices
```

- Entries starting with `[loopback]` are playback devices (system sound, `audio.source: "loopback"`); entries starting with `[capture]` are recording devices (`audio.source: "capture"`).
- An empty `audio.device_id` selects the default device.
- The FFmpeg build must include the AAC encoder.

### `AUDIO_ENCODE_FAILED`

Audio encoding failed and buffers are being dropped; it is logged once when the failure starts, and an `audio_encode_recovered` event is logged when it recovers.

---

## 6. Build

### `undefined reference to 'avcodec_...'`

The FFmpeg import libraries could not be linked.

- Check that `deps/ffmpeg/lib/` contains the `.dll.a` files.
- Check that you are using an LGPL **shared** FFmpeg build.

```bash
ls deps/ffmpeg/lib/
# libavcodec.dll.a, libavformat.dll.a, libavutil.dll.a, libswscale.dll.a, libswresample.dll.a
```

### `cannot find -lm` or similar

The MinGW-w64 installation may be incomplete.

```bash
gcc --version  # GCC 13 or later is required
```

### DLLs not found at run time

```bash
cp deps/ffmpeg/bin/*.dll build/
```

Copy the DLLs next to the executable, or add FFmpeg's `bin/` directory to `PATH`.

### CMake does not pick up the toolchain file

```bash
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$(pwd)/toolchain-mingw.cmake \
  -DCMAKE_BUILD_TYPE=Release
```

Pass `-DCMAKE_TOOLCHAIN_FILE` as an absolute path.
See [build.md](./build.md) for the full procedure.

---

## 7. Manual end-to-end test

### 1. Start MediaMTX

```powershell
Start-Process deps\mediamtx\mediamtx.exe
```

or:

```bash
./deps/mediamtx/mediamtx.exe &
```

MediaMTX prints a line such as `Listening on :8554 (RTSP)` when it is ready.

### 2. Start screen-relay

```bash
./build/screen-relay.exe --config config/config.json
```

Output like the following means it is working (each line is also written to the log file; the keys are sorted alphabetically and all values are strings):

```json
{"ts":"2026-01-01T12:00:00.000Z","event":"monitor_added","device":"\\\\.\\DISPLAY1","height":"1080","is_primary":"true","monitor":"1","width":"1920"}
{"ts":"2026-01-01T12:00:00.100Z","event":"state_changed","from":"INIT","monitor":"1","to":"CAPTURING"}
{"ts":"2026-01-01T12:00:00.200Z","event":"state_changed","from":"CAPTURING","monitor":"1","to":"CONNECTING"}
{"ts":"2026-01-01T12:00:00.300Z","event":"gpu_zero_copy_mode","active":"true","monitor":"1"}
{"ts":"2026-01-01T12:00:00.300Z","event":"encoder_initialized","bitrate_kbps":"4000","codec":"h264_nvenc","fps":"60","height":"1080","monitor":"1","width":"1920"}
{"ts":"2026-01-01T12:00:00.500Z","event":"publish_started","fps":"60","height":"1080","monitor":"1","url":"rtsp://192.168.0.100:8554/screen1","width":"1920"}
{"ts":"2026-01-01T12:00:00.500Z","event":"publish_started","fps":"60","height":"1080","monitor":"1","url":"rtsp://192.168.0.100:8554/screen0","width":"1920"}
{"ts":"2026-01-01T12:00:00.500Z","event":"state_changed","from":"CONNECTING","monitor":"1","to":"STREAMING"}
```

(The example is a primary monitor, which publishes both `screen1` and `screen0`.)

### 3. Check the stream with ffprobe

```bash
ffprobe rtsp://192.168.0.100:8554/screen1
```

Information about an H.264 video stream means it works.

### 4. Play it live with ffplay

```bash
ffplay -fflags nobuffer -flags low_delay -rtsp_transport tcp rtsp://192.168.0.100:8554/screen1
```

### 5. Check with VLC

```powershell
vlc rtsp://192.168.0.100:8554/screen1
```

or:

```bash
"C:/Program Files/VideoLAN/VLC/vlc.exe" rtsp://192.168.0.100:8554/screen1
```

### 6. Check health and metrics

Paths are `app.health_path` and `app.metrics_path` (defaults `./state/health.json` and `./state/metrics.json`), written every `runtime.emit_metrics_interval_ms`.

```bash
cat state/health.json
# top-level "healthy" must be true (a monitor is healthy in STREAMING or CONNECTING)

cat state/metrics.json
# monitors.<n>.frames_received should keep increasing and monitors.<n>.frames_dropped should stay low
```

### 7. Test monitor changes

1. Connect and disconnect a monitor; `monitor_added` / `monitor_removed` events must appear in the log.
2. Change the resolution; the pipeline must go through `RECONFIGURING` → `CAPTURING` → `CONNECTING` → `STREAMING`.

---

## Reading the log

Logs are written as JSON Lines to `<log_dir>/<app.instance_name>.jsonl` (rotated at 10 MB, 5 files kept) and also to the console.
Every entry has `ts` and `event`; all other values are strings (for example `"monitor":"1"`).
Errors are entries with `"event":"error"` and the fields `code` and `message`.

```bash
# Follow the log (<instance_name> is app.instance_name in config.json)
tail -f logs/<instance_name>.jsonl | jq .

# Extract error events only
grep '"event":"error"' logs/<instance_name>.jsonl

# Extract the events of a specific monitor
grep '"monitor":"1"' logs/<instance_name>.jsonl
```
