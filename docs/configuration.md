# 設定リファレンス

## 設定ファイルの場所

デフォルトは `config/config.json` です。コマンドライン引数で変更できます。

```bash
screen-relay.exe --config path/to/config.json
```

---

## 完全なサンプル設定

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

---

## `app` セクション

アプリケーション全体の基本設定です。

| フィールド | 型 | デフォルト | 説明 |
|---|---|---|---|
| `instance_name` | string | `"screen-relay"` | インスタンスの識別名。ログや metrics に記録される |
| `log_dir` | string | `"./logs"` | ログファイルの出力ディレクトリ |
| `log_level` | string | `"info"` | ログレベル。`trace` / `debug` / `info` / `warn` / `error` / `critical` |
| `metrics_path` | string | `"./state/metrics.json"` | metrics.json の出力パス |
| `health_path` | string | `"./state/health.json"` | health.json の出力パス |

### バリデーション

- `log_level` は `trace` / `debug` / `info` / `warn` / `error` / `critical` のいずれかでなければなりません

---

## `capture` セクション

スクリーンキャプチャバックエンドの設定です。

| フィールド | 型 | デフォルト | 説明 |
|---|---|---|---|
| `backend` | string | `"dxgi"` | キャプチャバックエンド。現在は `"dxgi"` のみ対応 |
| `frame_timeout_ms` | integer | `100` | `AcquireNextFrame()` のタイムアウト（ミリ秒）。0 は即時返却 |

### バリデーション

- `backend` は `"dxgi"` でなければなりません（将来的に `"wgc"` 等を追加予定）
- `frame_timeout_ms` は 0 以上でなければなりません

---

## `encoder` セクション

H.264 エンコーダーの設定です。

| フィールド | 型 | デフォルト | 説明 |
|---|---|---|---|
| `codec` | string | `"h264_nvenc"` | 優先コーデック名（FFmpeg のコーデック名） |
| `fallback_codecs` | string[] | `["h264_mf", "libx264"]` | フォールバックコーデックのリスト（順に試行） |
| `bitrate_kbps` | integer | `4000` | エンコードビットレート（kbps） |
| `fps` | integer | `60` | フレームレート（fps） |
| `gop_size` | integer | `60` | GOP サイズ（フレーム数）。キーフレーム間隔 |
| `max_b_frames` | integer | `0` | 最大 B フレーム数。低遅延用途では `0` を推奨 |
| `preset` | string | `"fast"` | エンコードプリセット（コーデックに依存） |
| `tune` | string | `"zerolatency"` | チューニングオプション（コーデックに依存） |
| `threads` | integer | `0` | エンコードスレッド数。`0` で自動 |

### `fallback_codecs` について

`fallback_codecs` は配列で複数のコーデックを指定できます。`codec` の初期化に失敗した場合、リスト順に試行します。

```json
"fallback_codecs": ["h264_mf", "libx264"]
```

上記の場合、試行順は `h264_nvenc` → `h264_mf` → `libx264` となります。すべて失敗した場合はパイプラインが `FATAL` 状態になります。

### バリデーション

- `bitrate_kbps` は 1 以上でなければなりません
- `fps` は 1 以上でなければなりません
- `gop_size` は 1 以上でなければなりません
- `max_b_frames` は 0 以上でなければなりません
- `threads` は 0 以上でなければなりません

---

## `rtsp` セクション

RTSP 配信の設定です。

| フィールド | 型 | デフォルト | 説明 |
|---|---|---|---|
| `base_url` | string | （必須） | RTSP サーバーのベース URL（例: `rtsp://192.168.0.100:8554`） |
| `path_pattern` | string | `"/screen{n}"` | RTSP パスのパターン。`{n}` がモニター番号に置換される |
| `connect_timeout_ms` | integer | `5000` | RTSP 接続タイムアウト（ミリ秒） |
| `send_timeout_ms` | integer | `5000` | RTP パケット送信タイムアウト（ミリ秒） |
| `reconnect_delay_ms` | integer | `1000` | 初回再接続待機時間（ミリ秒） |
| `reconnect_max_delay_ms` | integer | `30000` | 最大再接続待機時間（ミリ秒）。指数バックオフの上限 |
| `reconnect_backoff_multiplier` | float | `2.0` | 指数バックオフの乗数 |

### `path_pattern` について

`{n}` は **現在の Windows 設定ディスプレイ番号**（デバイス名 `\\.\DISPLAYn` の `n`）に置換されます。モニター構成変化（接続・切断・プライマリ切替）が発生した場合、`monitor_check_interval_ms` 以内に新しい番号へ追従します。

```
path_pattern = "/screen{n}"

DISPLAY1 → rtsp://192.168.0.100:8554/screen1
DISPLAY2 → rtsp://192.168.0.100:8554/screen2
```

**プライマリモニターのエイリアス**: プライマリモニターは `{n}` のパスに加えて `/screen0` にも自動的に配信されます。`/screen0` は**常に現在のプライマリモニター**の映像を指します。プライマリが別のモニターに移った場合、`monitor_check_interval_ms` 以内に `/screen0` が新プライマリへ追従します。

```
DISPLAY2 (primary) → /screen2 および /screen0 の両方に配信
DISPLAY1           → /screen1 のみに配信
```

> **注意**: Windows 設定での番号にギャップ（例: 1, 3 のみで 2 が欠番）が生じた場合、screenN もそのギャップをそのまま反映します（独自の番号詰め直しは行いません）。これは Windows 設定アプリの表示との一貫性を保つための仕様です。

### 再接続バックオフの計算

```
delay(n) = min(reconnect_delay_ms × reconnect_backoff_multiplier^n, reconnect_max_delay_ms)
```

デフォルト設定での待機時間の例:

| 試行回数 | 待機時間 |
|---|---|
| 1 回目 | 1000 ms |
| 2 回目 | 2000 ms |
| 3 回目 | 4000 ms |
| 4 回目 | 8000 ms |
| 5 回目以降 | 30000 ms (上限) |

### バリデーション

- `base_url` は `rtsp://` で始まらなければなりません
- `connect_timeout_ms` は 1 以上でなければなりません
- `send_timeout_ms` は 1 以上でなければなりません
- `reconnect_delay_ms` は 1 以上でなければなりません
- `reconnect_max_delay_ms` は `reconnect_delay_ms` 以上でなければなりません
- `reconnect_backoff_multiplier` は 1.0 以上でなければなりません

---

## `audio` セクション

音声配信の設定です。有効にすると、WASAPI で取得した音声を AAC でエンコードし、**全モニターの RTSP ストリーム**（`/screen1`, `/screen2`, ... `/screen0`）に同一の音声として配信します。

| フィールド | 型 | デフォルト | 説明 |
|---|---|---|---|
| `enabled` | bool | `false` | 音声配信を有効にするか |
| `source` | string | `"loopback"` | 音声取得元。`"loopback"`（システム再生音）または `"capture"`（マイク等の録音デバイス） |
| `device_id` | string | `""` | WASAPI エンドポイント ID。空の場合は既定デバイスを使用する。デバイス一覧は `--list-audio-devices` で確認できる |
| `bitrate_kbps` | integer | `128` | AAC エンコードビットレート（kbps） |
| `sample_rate` | integer | `48000` | エンコード後のサンプルレート（Hz） |
| `channels` | integer | `2` | エンコード後のチャンネル数（`1`=モノラル、`2`=ステレオ） |

### 対象デバイスの選び方

利用可能な音声デバイス（ループバック用のレンダリングデバイス、および録音デバイス）を確認するには次のコマンドを実行します。

```bash
screen-relay.exe --list-audio-devices
```

出力された `id` を `device_id` に設定してください。`device_id` を空にした場合は、`source` に応じた既定デバイス（既定の再生デバイス、または既定の録音デバイス）が使用されます。

### 音声の初期化に失敗した場合

音声パイプラインの初期化（デバイスの取得や AAC エンコーダーの初期化）に失敗しても、映像配信には影響しません。エラーはログに記録され、音声なしで起動を継続します。

### バリデーション（`enabled: true` の場合のみ）

- `source` は `"loopback"` または `"capture"` でなければなりません
- `bitrate_kbps` は 1 以上でなければなりません
- `sample_rate` は 1 以上でなければなりません
- `channels` は `1` または `2` でなければなりません

---

## `runtime` セクション

ランタイム動作の設定です。

| フィールド | 型 | デフォルト | 説明 |
|---|---|---|---|
| `monitor_check_interval_ms` | integer | `1000` | モニター変更の検出間隔（ミリ秒） |
| `shutdown_grace_ms` | integer | `3000` | シャットダウン時のグレース期間（ミリ秒） |
| `emit_metrics_interval_ms` | integer | `1000` | health.json / metrics.json の書き出し間隔（ミリ秒） |

### バリデーション

- すべての値は 1 以上でなければなりません
