# State machine

## Overview

Each `ScreenPipeline` is driven by a `StateMachine`. The transition table is defined in `src/app/state_machine.cpp`; a transition that is not in the table is rejected (`transition_to()` returns `false`) and the caller logs a `STATE_TRANSITION_FAILED` error.

---

## States

| State | Description |
|---|---|
| `INIT` | Just started. The pipeline initializes DXGI capture and waits for the first frame. |
| `CAPTURING` | Capture is running; waiting to initialize the encoder and connect. |
| `CONNECTING` | Initializing the encoder and connecting to the RTSP server. |
| `STREAMING` | Streaming normally: capture → encode → RTSP send. |
| `RECONNECTING` | RTSP connection lost; reconnecting with exponential back-off. |
| `RECONFIGURING` | Resolution change detected; capture, encoder and RTSP are being re-initialized. |
| `STOPPING` | Shutting down (terminal). |
| `FATAL` | Unrecoverable error (terminal). The pipeline thread tears down and exits; `MonitorSupervisor` recreates the pipeline on its next monitor poll. |

---

## Transition diagram

```text
INIT ──▶ CAPTURING ──▶ CONNECTING ──▶ STREAMING ──▶ RECONNECTING
              ▲             │             │  ▲            │
              │             │             │  └── OK ──────┘
              └─ RTSP ──────┘             ▼
              │  connect failed      RECONFIGURING
              └────── done ──────────────┘
```

`STOPPING` is reachable from every state except `INIT` and `FATAL` (shutdown request), and `FATAL` is reachable from `INIT`, `CAPTURING`, `CONNECTING`, `STREAMING` and `RECONFIGURING` (unrecoverable error). See the table below for the exact set.

---

## Valid transitions

These are the entries of `valid_transitions_` in `state_machine.cpp`.

| From | To | Condition |
|---|---|---|
| `INIT` | `CAPTURING` | DXGI initialization succeeded |
| `INIT` | `FATAL` | DXGI initialization failed |
| `CAPTURING` | `CONNECTING` | Start of a connect attempt |
| `CAPTURING` | `STOPPING` | Shutdown request |
| `CAPTURING` | `FATAL` | Unrecoverable error |
| `CONNECTING` | `STREAMING` | Encoder initialized and RTSP connected |
| `CONNECTING` | `CAPTURING` | RTSP connection failed (retry after `reconnect_delay_ms`) |
| `CONNECTING` | `STOPPING` | Shutdown request |
| `CONNECTING` | `FATAL` | Encoder initialization failed (all fallbacks failed) |
| `STREAMING` | `RECONNECTING` | RTSP send error |
| `STREAMING` | `RECONFIGURING` | Resolution change detected, or DXGI hard error (`DXGI_HARD_ERROR`, e.g. access lost) |
| `STREAMING` | `STOPPING` | Shutdown request |
| `STREAMING` | `FATAL` | Encode failure |
| `RECONNECTING` | `STREAMING` | Reconnect succeeded |
| `RECONNECTING` | `CAPTURING` | Allowed by the table; not used by the current pipeline code |
| `RECONNECTING` | `STOPPING` | Shutdown request |
| `RECONFIGURING` | `CAPTURING` | Reconfiguration finished |
| `RECONFIGURING` | `STOPPING` | Shutdown request |
| `RECONFIGURING` | `FATAL` | DXGI re-initialization failed |

---

## Behavior per state

### INIT

1. `do_init_and_capture()` initializes `DxgiCaptureBackend` (calls `IDXGIOutput1::DuplicateOutput`), retrying every 200 ms up to 15 times because `DuplicateOutput` can return `E_ACCESSDENIED` right after a resolution change.
2. On success it starts `FramePump`, moves to `CAPTURING`, and waits for the first frame to learn the resolution.
3. If initialization fails, it logs `DXGI_INIT_FAILED` and moves to `FATAL`.
4. If no first frame arrives in time, it logs `FIRST_FRAME_TIMEOUT` and moves to `FATAL`.

### CAPTURING

1. `FramePump` runs the capture thread in the background, polling `DxgiCaptureBackend::acquire_frame()`.
2. On each pass of the pipeline loop, `do_connect()` moves to `CONNECTING`.

### CONNECTING

1. Initialize the encoder (with codec fallback). If every codec fails, log `ENCODER_INIT_FAILED` and move to `FATAL`.
2. Connect every `RtspPublisherClient` (ANNOUNCE → SETUP → RECORD).
3. On success move to `STREAMING`.
4. If an RTSP connection fails, log `RTSP_CONNECT_FAILED`, release the encoder, return to `CAPTURING`, and retry after `reconnect_delay_ms`.

### STREAMING

1. Take frames from the queue and encode them. When no new frame arrives, the last frame is re-encoded at the configured `fps` (freeze frame).
2. Send the encoded packets to every RTSP client.
3. An RTSP send error (`RTSP_SEND_FAILED`) moves to `RECONNECTING`.
4. A pending resolution change, or a DXGI hard error (`DXGI_HARD_ERROR`), moves to `RECONFIGURING`.
5. An encode failure (`ENCODER_ENCODE_FAILED`) moves to `FATAL`.

### RECONNECTING

1. Disconnect the RTSP clients.
2. Wait with exponential back-off (`reconnect_delay_ms` × `reconnect_backoff_multiplier`, capped at `reconnect_max_delay_ms`).
3. Reconnect every RTSP client. On success reset the back-off, request an IDR frame, and move to `STREAMING`.
4. On failure (`RTSP_RECONNECT_FAILED`) repeat from step 2. There is no attempt limit; the loop ends only on shutdown (`STOPPING`).

### RECONFIGURING

1. Stop `FramePump`, flush and release the encoder, and disconnect the RTSP clients.
2. Re-initialize DXGI capture at the new resolution (same retry as in `INIT`). If that fails, log `DXGI_REINIT_FAILED` and move to `FATAL`.
3. Restart `FramePump` and move to `CAPTURING`, which then re-runs encoder initialization and the RTSP connection.

### STOPPING

1. Stop the capture thread.
2. Disconnect the RTSP clients.
3. Flush and release the encoder, and release the capture backend.

### FATAL

1. Release all resources (same teardown as `STOPPING`).
2. The pipeline thread exits.
3. On its next poll, `MonitorSupervisor::apply_monitor_changes()` sees the pipeline is FATAL and recreates it (`FATAL pipeline restarting`).

---

## State change event log

Every transition is written to the event log (`state_changed`) and reflected in the `state` field of `health.json` / `metrics.json`. See the event log section of [metrics.md](./metrics.md).
