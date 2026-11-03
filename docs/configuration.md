# Configuration Reference

## Config file location

The default is `config/config.json`. Override it on the command line with `--config <path>` (short form `-c`).

```bash
screen-relay.exe --config path/to/config.json
```

`config/config.example.json` is a ready-to-copy template. Any key you omit keeps the default listed below, except `rtsp.base_url`, which is required. A malformed file or a validation failure prints `[ERROR] <reason>` and exits with code 1.

Other command-line options:

| Option | Description |
|---|---|
| `--list-audio-devices` | Print the available audio endpoints (`[loopback]` render devices and `[capture]` recording devices) with their IDs, then exit |
| `--help`, `-h` | Print usage and exit |

---

## Full sample configuration

```json
{
  "app": {
    "instance_name": "screen-relay-01",
    "log_dir": "./logs",
    "log_level": "info",
    "metrics_path": "./state/metrics.json",
    "health_path": "./state/health.json"
  },
  "capture": {
    "backend": "dxgi",
    "frame_timeout_ms": 100
  },
  "encoder": {
    "codec": "h264_nvenc",
    "fallback_codecs": ["h264_mf", "libx264"],
    "bitrate_kbps": 4000,
    "fps": 60,
    "gop_size": 60,
    "max_b_frames": 0,
    "preset": "fast",
    "tune": "zerolatency",
    "threads": 0
  },
  "rtsp": {
    "base_url": "rtsp://192.168.0.100:8554",
    "path_pattern": "/screen{n}",
    "connect_timeout_ms": 5000,
    "send_timeout_ms": 5000,
    "reconnect_delay_ms": 1000,
    "reconnect_max_delay_ms": 30000,
    "reconnect_backoff_multiplier": 2.0
  },
  "audio": {
    "enabled": false,
    "source": "loopback",
    "device_id": "",
    "bitrate_kbps": 128,
    "sample_rate": 48000,
    "channels": 2
  },
  "runtime": {
    "monitor_check_interval_ms": 1000,
    "shutdown_grace_ms": 3000,
    "emit_metrics_interval_ms": 1000
  }
}
```

`encoder.threads` is accepted but is not included in `config/config.example.json`.

---

## `app` section

General application settings.

| Field | Type | Default | Description |
|---|---|---|---|
| `instance_name` | string | `"screen-relay"` | Instance name. Used as the log file name (`<instance_name>.jsonl`) and as the logger name |
| `log_dir` | string | `"./logs"` | Directory for log files (created if missing) |
| `log_level` | string | `"info"` | Log level: `trace` / `debug` / `info` / `warn` / `error` / `critical` |
| `metrics_path` | string | `"./state/metrics.json"` | Output path of `metrics.json` (parent directory is created if missing) |
| `health_path` | string | `"./state/health.json"` | Output path of `health.json` (parent directory is created if missing) |

### Validation

- None. The loader does not validate this section. An unrecognized `log_level` silently falls back to `info`.

---

## `capture` section

Screen capture backend settings.

| Field | Type | Default | Description |
|---|---|---|---|
| `backend` | string | `"dxgi"` | Capture backend. Only `"dxgi"` is currently supported |
| `frame_timeout_ms` | integer | `100` | Timeout in milliseconds for `AcquireNextFrame()`. `0` returns immediately |

### Validation

- `backend` must be `"dxgi"`
- `frame_timeout_ms` must be 0 or greater

---

## `encoder` section

H.264 encoder settings.

| Field | Type | Default | Description |
|---|---|---|---|
| `codec` | string | `"h264_nvenc"` | Preferred codec (FFmpeg encoder name) |
| `fallback_codecs` | string[] | `["h264_mf", "libx264"]` | Fallback codecs, tried in order |
| `bitrate_kbps` | integer | `4000` | Target bitrate (kbps) |
| `fps` | integer | `60` | Frame rate (fps) |
| `gop_size` | integer | `60` | GOP size in frames (keyframe interval) |
| `max_b_frames` | integer | `0` | Maximum number of B-frames. `0` is recommended for low latency |
| `preset` | string | `"fast"` | Encoder preset (codec dependent) |
| `tune` | string | `"zerolatency"` | Tuning option (codec dependent) |
| `threads` | integer | `0` | Number of encoder threads. `0` means automatic |

### `fallback_codecs`

If initializing `codec` fails, the codecs in `fallback_codecs` are tried in list order. For the default configuration the order is `h264_nvenc` → `h264_mf` → `libx264`. If every codec fails, the monitor's pipeline enters the `FATAL` state (see [state-machine.md](state-machine.md)).

```json
"fallback_codecs": ["h264_mf", "libx264"]
```

The codec actually selected is reported in the `encoder_initialized` log event and in the `codec` field of `metrics.json`.

### Validation

- `bitrate_kbps` must be greater than 0
- `fps` must be between 1 and 240
- `gop_size`, `max_b_frames` and `threads` are not range-checked by the loader

---

## `rtsp` section

RTSP publishing settings.

| Field | Type | Default | Description |
|---|---|---|---|
| `base_url` | string | (required) | Base URL of the RTSP server (e.g. `rtsp://192.168.0.100:8554`) |
| `path_pattern` | string | `"/live/screen{n}"` | Stream path pattern. `{n}` is replaced with the monitor number. See the note below about the default |
| `connect_timeout_ms` | integer | `5000` | RTSP connect timeout (ms) |
| `send_timeout_ms` | integer | `5000` | RTP packet send timeout (ms) |
| `reconnect_delay_ms` | integer | `1000` | Initial reconnect delay (ms) |
| `reconnect_max_delay_ms` | integer | `30000` | Maximum reconnect delay (ms). Upper bound of the exponential backoff |
| `reconnect_backoff_multiplier` | float | `2.0` | Exponential backoff multiplier |

### `path_pattern`

The built-in default (`types.hpp`) is `/live/screen{n}`, but `config/config.example.json` and all examples in this document use `/screen{n}`. If you want `/screen{n}`, set it explicitly; a config that omits `path_pattern` publishes to `/live/screen{n}`.

`{n}` is replaced with the **display number in the current Windows settings** (the `n` of the device name `\\.\DISPLAYn`). When the monitor configuration changes (connect, disconnect, primary switch), the stream paths follow the new numbers within `monitor_check_interval_ms`.

```
path_pattern = "/screen{n}"

DISPLAY1 → rtsp://192.168.0.100:8554/screen1
DISPLAY2 → rtsp://192.168.0.100:8554/screen2
```

**Primary monitor alias**: the primary monitor is also published automatically on `0` in place of `{n}` (e.g. `/screen0` for `/screen{n}`). That path always carries the current primary monitor. When the primary moves to another monitor, the alias follows within `monitor_check_interval_ms`.

```
DISPLAY2 (primary) → published to both /screen2 and /screen0
DISPLAY1           → published to /screen1 only
```

> **Note**: if the Windows settings numbers have gaps (e.g. only 1 and 3 exist), the stream paths reflect the gaps as is; numbers are not repacked. This keeps the paths consistent with what the Windows Settings app shows.

### Reconnect backoff

With `n` counting from 0 for the first retry:

```
delay(n) = min(reconnect_delay_ms × reconnect_backoff_multiplier^n, reconnect_max_delay_ms)
```

Example delays with the default settings:

| Attempt | Delay |
|---|---|
| 1st | 1000 ms |
| 2nd | 2000 ms |
| 3rd | 4000 ms |
| 4th | 8000 ms |
| 5th and later | 30000 ms (cap) |

The `reconnecting` log event records each attempt number and delay, and `reconnect_attempts` in `metrics.json` counts them.

### Validation

- `base_url` must start with `rtsp://`
- `connect_timeout_ms` must be greater than 0
- `send_timeout_ms` must be greater than 0
- `reconnect_delay_ms` must be greater than 0
- `reconnect_max_delay_ms` must be greater than 0 and at least `reconnect_delay_ms`
- `reconnect_backoff_multiplier` must be 1.0 or greater

---

## `audio` section

Audio publishing settings. When enabled, audio captured through WASAPI is encoded as AAC and published as the same audio track on **every monitor's RTSP stream** (including the `/screen0` alias).

| Field | Type | Default | Description |
|---|---|---|---|
| `enabled` | boolean | `false` | Enable audio publishing |
| `source` | string | `"loopback"` | Audio source: `"loopback"` (system playback sound) or `"capture"` (recording device such as a microphone) |
| `device_id` | string | `""` | WASAPI endpoint ID. Empty selects the default device for `source` |
| `bitrate_kbps` | integer | `128` | AAC encode bitrate (kbps) |
| `sample_rate` | integer | `48000` | Encoded sample rate (Hz) |
| `channels` | integer | `2` | Encoded channel count (`1` = mono, `2` = stereo) |

### Choosing a device

To list the available audio devices (render devices for loopback, and recording devices), run:

```bash
screen-relay.exe --list-audio-devices
```

Each line is prefixed with `[loopback]` (render device, usable with `source: "loopback"`) or `[capture]` (recording device, usable with `source: "capture"`), followed by the endpoint ID and the device name. Set the ID in `device_id`. If `device_id` is empty, the default playback device (loopback) or default recording device (capture) is used according to `source`.

### Audio initialization failure

If the audio pipeline fails to initialize (device acquisition or AAC encoder setup), video publishing is not affected. An `AUDIO_INIT_FAILED` error is logged and the application continues without audio.

### Validation (only when `enabled` is `true`)

- `source` must be `"loopback"` or `"capture"`
- `bitrate_kbps` must be greater than 0
- `sample_rate` must be greater than 0
- `channels` must be `1` or `2`

---

## `runtime` section

Runtime behavior settings.

| Field | Type | Default | Description |
|---|---|---|---|
| `monitor_check_interval_ms` | integer | `1000` | Interval (ms) for polling monitor configuration changes |
| `shutdown_grace_ms` | integer | `3000` | Grace period (ms) at shutdown |
| `emit_metrics_interval_ms` | integer | `1000` | Write interval (ms) of `health.json` and `metrics.json` |

### Validation

- `monitor_check_interval_ms` and `emit_metrics_interval_ms` must be greater than 0
- `shutdown_grace_ms` must be 0 or greater (the loader's error message says "positive", but `0` is accepted)
