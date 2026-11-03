# Metrics and Health Specification

## Output files

| File | Default path | Update interval |
|---|---|---|
| `health.json` | `./state/health.json` | Every `runtime.emit_metrics_interval_ms` (default 1 second) |
| `metrics.json` | `./state/metrics.json` | Every `runtime.emit_metrics_interval_ms` (default 1 second) |

Paths are set by `app.health_path` and `app.metrics_path` (see [configuration.md](configuration.md)). Both files are pretty-printed with 2-space indentation and are replaced atomically (written to `<path>.tmp`, then renamed over the old file), so readers never see a partial file. They are also written one final time at shutdown. If a write fails, `[ERROR] Failed to write ...` is printed to stderr and the previous file is kept.

---

## health.json

### Format

```json
{
  "healthy": true,
  "monitors": {
    "1": { "healthy": true, "state": "STREAMING" },
    "2": { "healthy": true, "state": "STREAMING" },
    "3": { "healthy": false, "state": "FATAL" }
  },
  "ts": "2026-04-30T19:21:03.306Z"
}
```

### Fields

| Field | Type | Description |
|---|---|---|
| `healthy` | boolean | `true` when every monitor is healthy |
| `monitors` | object | Per-monitor health, keyed by monitor number |
| `monitors[n].healthy` | boolean | `true` when that monitor is healthy |
| `monitors[n].state` | string | Current state-machine state name of that monitor (see [state-machine.md](state-machine.md)) |
| `ts` | string | Time of writing (ISO 8601, UTC) |

### How `healthy` is determined

A monitor is healthy only while its state is `STREAMING` or `CONNECTING`. Every other state (`INIT`, `CAPTURING`, `RECONNECTING`, `RECONFIGURING`, `STOPPING`, `FATAL`) counts as unhealthy immediately; there is no grace period. The top-level `healthy` is `true` only if at least one monitor is known and all known monitors are healthy, so it is `false` before the first monitor has registered.

---

## metrics.json

### Format

```json
{
  "ts": "2026-04-30T19:21:03.306Z",
  "uptime_ms": 42000,
  "monitors": {
    "1": {
      "state": "STREAMING",
      "width": 3840,
      "height": 2160,
      "fps": 60.0,
      "bitrate_kbps": 4000.0,
      "codec": "h264_nvenc",
      "frames_received": 1234,
      "frames_encoded": 1230,
      "frames_dropped": 4,
      "rtsp_errors": 0,
      "reconnect_attempts": 0
    }
  }
}
```

### Top-level fields

| Field | Type | Description |
|---|---|---|
| `ts` | string | Time of writing (ISO 8601, UTC) |
| `uptime_ms` | integer | Milliseconds since the supervisor started running |
| `monitors` | object | Per-monitor metrics, keyed by monitor number |

### Per-monitor fields

| Field | Type | Description |
|---|---|---|
| `state` | string | Current state name |
| `width` | integer | Capture resolution, width in pixels |
| `height` | integer | Capture resolution, height in pixels |
| `fps` | float | Measured encoded frame rate, recomputed about once per second while streaming. `0.0` until the first measurement |
| `bitrate_kbps` | float | Measured output bitrate (kbps), recomputed about once per second while streaming. `0.0` until the first measurement |
| `codec` | string | Codec actually in use |
| `frames_received` | integer | Total frames received from the capture backend |
| `frames_encoded` | integer | Total frames successfully encoded |
| `frames_dropped` | integer | Total frames lost to encode failures |
| `rtsp_errors` | integer | Total RTSP send errors |
| `reconnect_attempts` | integer | Total RTSP reconnect attempts |

Counters are cumulative since process start and are keyed by monitor number. Entries are never removed, so a monitor that has been unplugged keeps its last values (including its last state) in both files.

---

## Event log (JSON Lines)

Log events are written to `<log_dir>/<instance_name>.jsonl` (rotated at 10 MB, 5 files kept) and are also echoed to the console. Each line is one event. Messages at `info` or above are flushed immediately, so `tail -f` shows them in real time.

Every field value, including numbers and booleans, is written as a string. Fields other than `ts` and `event` appear in alphabetical key order. Event and error lines are filtered by `app.log_level` like any other log line; events marked `info` are therefore hidden at `warn` or above.

### state_changed

Logged when a monitor's state machine changes state.

```json
{"ts":"2026-04-30T19:21:03.000Z","event":"state_changed","from":"CONNECTING","monitor":"1","to":"STREAMING"}
```

### monitor_added

Logged when a new monitor is detected.

```json
{"ts":"2026-04-30T19:21:00.000Z","event":"monitor_added","device":"\\\\.\\DISPLAY2","height":"1024","is_primary":"false","monitor":"2","width":"1280"}
```

### monitor_removed

Logged when a monitor is removed.

```json
{"ts":"2026-04-30T19:25:00.000Z","event":"monitor_removed","monitor":"2"}
```

### pipeline_restart / pipeline_retarget

`pipeline_restart` (level `warn`) is logged when a pipeline in the `FATAL` state is stopped and recreated. `pipeline_retarget` (level `info`) is logged when a monitor's number, primary flag or handle changed and its pipeline is recreated so that `/screenN` and the `/screen0` alias follow the change. Both carry `monitor`, `stable_id` and `reason`.

```json
{"ts":"2026-04-30T19:26:00.000Z","event":"pipeline_restart","monitor":"1","reason":"FATAL pipeline restarting","stable_id":"..."}
```

### encoder_initialized

Logged when an encoder is initialized. `codec` is the codec actually selected, which differs from the configured `encoder.codec` when a fallback was used.

```json
{"ts":"2026-04-30T19:21:01.000Z","event":"encoder_initialized","bitrate_kbps":"5000","codec":"h264_mf","fps":"60","height":"1080","monitor":"1","width":"1920"}
```

### gpu_zero_copy_mode

Logged right after the encoder is initialized, with `monitor` and `active` (`"true"` when the GPU zero-copy capture-to-encoder path is in use, `"false"` for the CPU path).

```json
{"ts":"2026-04-30T19:21:01.000Z","event":"gpu_zero_copy_mode","active":"true","monitor":"1"}
```

### publish_started

Logged when RTSP publishing starts (also after a successful reconnect). One event is logged per published URL, so the primary monitor logs a second one for the `/screen0` alias.

```json
{"ts":"2026-04-30T19:21:02.000Z","event":"publish_started","fps":"60","height":"1080","monitor":"1","url":"rtsp://192.168.0.100:8554/screen1","width":"1920"}
```

### publish_stopped

Logged when publishing stops. `reason` is a free-form string, e.g. `reconnecting` when the connection is dropped for a reconnect.

```json
{"ts":"2026-04-30T19:30:01.000Z","event":"publish_stopped","monitor":"1","reason":"reconnecting"}
```

### reconnecting

Logged (level `warn`) before each RTSP reconnect attempt, with the attempt number and the delay about to be waited.

```json
{"ts":"2026-04-30T19:30:02.000Z","event":"reconnecting","attempt":"1","delay_ms":"1000","monitor":"1"}
```

### audio_pipeline_started

Logged when the shared audio pipeline starts successfully.

```json
{"ts":"2026-04-30T19:20:59.000Z","event":"audio_pipeline_started"}
```

### audio_encode_recovered

Logged when audio encoding recovers from a failing state. During consecutive failures only the first one is logged as an `AUDIO_ENCODE_FAILED` error; this event is logged once when encoding succeeds again.

```json
{"ts":"2026-04-30T19:31:00.000Z","event":"audio_encode_recovered"}
```

### supervisor_stopped

Logged once at the end of shutdown, after the grace period.

```json
{"ts":"2026-04-30T19:40:00.000Z","event":"supervisor_stopped"}
```

### error

Logged (level `err`) when an error occurs. `code` and `message` are always present; other fields (typically `monitor` and `url`) depend on the error.

```json
{"ts":"2026-04-30T19:30:00.000Z","event":"error","code":"RTSP_SEND_FAILED","message":"send_packet failed for monitor 1","monitor":"1","url":"rtsp://..."}
```

---

## Error codes

Values of the `code` field of `error` events.

| Code | Description |
|---|---|
| `DXGI_INIT_FAILED` | DXGI Desktop Duplication initialization failed (pipeline goes to `FATAL`) |
| `DXGI_HARD_ERROR` | DXGI hard error such as access lost while streaming (pipeline re-initializes capture via `RECONFIGURING`) |
| `DXGI_REINIT_FAILED` | Capture re-initialization after a resolution change or hard error failed (pipeline goes to `FATAL`) |
| `FIRST_FRAME_TIMEOUT` | No first frame arrived from the monitor (pipeline goes to `FATAL`) |
| `ENCODER_INIT_FAILED` | Encoder initialization failed after trying all codecs (pipeline goes to `FATAL`) |
| `ENCODER_ENCODE_FAILED` | Error while encoding (pipeline goes to `FATAL`; counted in `frames_dropped`) |
| `RTSP_CONNECT_FAILED` | Initial connection to the RTSP server failed (pipeline returns to `CAPTURING` and retries) |
| `RTSP_RECONNECT_FAILED` | A reconnect attempt failed (the backoff loop continues) |
| `RTSP_SEND_FAILED` | Video or audio packet send error. A video send error counts in `rtsp_errors` and moves the pipeline to `RECONNECTING` |
| `STATE_TRANSITION_FAILED` | A requested state transition was rejected by the state machine (internal consistency error) |
| `AUDIO_INIT_FAILED` | Audio pipeline initialization failed (video publishing continues) |
| `AUDIO_ENCODE_FAILED` | Audio encode failed; the buffer is discarded and processing continues |

`src/common/errors.hpp` also defines `CAPTURE_INIT_FAILED`, `MONITOR_ENUM_FAILED`, `GPU_CRASH` and `RTSP_TIMEOUT`, but the application does not currently emit them.

---

## Integration with monitoring systems

### Example: convert to the Prometheus text format

```python
import json

with open("state/metrics.json") as f:
    m = json.load(f)

for monitor_id, mon in m["monitors"].items():
    labels = f'monitor="{monitor_id}"'
    print(f'screen_relay_frames_received{{{labels}}} {mon["frames_received"]}')
    print(f'screen_relay_frames_dropped{{{labels}}} {mon["frames_dropped"]}')
    print(f'screen_relay_rtsp_errors{{{labels}}} {mon["rtsp_errors"]}')
```

### Example: health check script

```bash
#!/usr/bin/env bash
healthy=$(jq -r '.healthy' state/health.json)
if [ "$healthy" != "true" ]; then
  echo "screen-relay is unhealthy"
  jq '.monitors' state/health.json
  exit 1
fi
echo "screen-relay is healthy"
```
