# メトリクス & ヘルス仕様

## 出力ファイル

| ファイル | デフォルトパス | 更新間隔 |
|---|---|---|
| `health.json` | `./state/health.json` | `emit_metrics_interval_ms` ごと（デフォルト 1 秒） |
| `metrics.json` | `./state/metrics.json` | `emit_metrics_interval_ms` ごと（デフォルト 1 秒） |

---

## health.json

### フォーマット

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

### フィールド説明

| フィールド | 型 | 説明 |
|---|---|---|
| `healthy` | boolean | すべてのモニターが正常なとき `true` |
| `monitors` | object | モニター番号をキーとした各モニターのヘルス情報 |
| `monitors[n].healthy` | boolean | そのモニターが正常なとき `true` |
| `monitors[n].state` | string | そのモニターの現在の状態（ステートマシンの状態名） |
| `ts` | string | 書き出し時刻（ISO 8601 UTC） |

### `healthy` の判定ロジック

トップレベルの `healthy` は、**すべてのモニターが `STREAMING` または `CONNECTING` 状態のとき `true`** になります。1 台でも `FATAL` や長時間 `RECONNECTING` 状態のモニターがある場合は `false` になります。

---

## metrics.json

### フォーマット

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

### トップレベルフィールド

| フィールド | 型 | 説明 |
|---|---|---|
| `ts` | string | 書き出し時刻（ISO 8601 UTC） |
| `uptime_ms` | integer | アプリケーション起動からの経過時間（ミリ秒） |
| `monitors` | object | モニター番号をキーとした各モニターのメトリクス |

### モニターごとのフィールド

| フィールド | 型 | 説明 |
|---|---|---|
| `state` | string | 現在の状態名 |
| `width` | integer | キャプチャ解像度（幅、ピクセル） |
| `height` | integer | キャプチャ解像度（高さ、ピクセル） |
| `fps` | float | 設定されたフレームレート |
| `bitrate_kbps` | float | 設定されたビットレート（kbps） |
| `codec` | string | 実際に使用中のコーデック名 |
| `frames_received` | integer | キャプチャバックエンドから受信したフレーム総数 |
| `frames_encoded` | integer | エンコードに成功したフレーム総数 |
| `frames_dropped` | integer | キューが満杯のためドロップしたフレーム総数 |
| `rtsp_errors` | integer | RTSP 送信エラーの累計回数 |
| `reconnect_attempts` | integer | RTSP 再接続を試みた累計回数 |

---

## イベントログ（JSON Lines）

ログファイルは `log_dir` に `<instance_name>.jsonl` として JSON Lines 形式で出力されます。各行が 1 件のイベントです。

フィールドはすべて文字列値として出力されます（数値・boolean を含む）。

### state_changed

ステートマシンの状態が変化したときに記録されます。

```json
{"ts":"2026-04-30T19:21:03.000Z","event":"state_changed","from":"CONNECTING","monitor":"1","to":"STREAMING"}
```

### monitor_added

新しいモニターが検出されたときに記録されます。

```json
{"ts":"2026-04-30T19:21:00.000Z","event":"monitor_added","device":"\\\\.\\DISPLAY2","height":"1024","is_primary":"false","monitor":"2","width":"1280"}
```

### monitor_removed

モニターが取り外されたときに記録されます。

```json
{"ts":"2026-04-30T19:25:00.000Z","event":"monitor_removed","monitor":"2"}
```

### encoder_initialized

エンコーダーが初期化されたときに記録されます。`codec` には実際に選択されたコーデック名が入ります（フォールバックが発生した場合は設定値と異なる場合があります）。

```json
{"ts":"2026-04-30T19:21:01.000Z","event":"encoder_initialized","bitrate_kbps":"5000","codec":"h264_mf","fps":"60","height":"1080","monitor":"1","width":"1920"}
```

### publish_started

RTSP 配信が開始されたときに記録されます。

```json
{"ts":"2026-04-30T19:21:02.000Z","event":"publish_started","fps":"60","height":"1080","monitor":"1","url":"rtsp://192.168.0.100:8554/screen1","width":"1920"}
```

### audio_pipeline_started

共有音声パイプラインの起動に成功したときに記録されます。

```json
{"ts":"2026-04-30T19:20:59.000Z","event":"audio_pipeline_started"}
```

### audio_encode_recovered

音声エンコードが失敗状態から復帰したときに記録されます（連続失敗中は最初の 1 回のみ `AUDIO_ENCODE_FAILED` エラーが記録され、復帰時にこのイベントが 1 回だけ記録されます）。

```json
{"ts":"2026-04-30T19:31:00.000Z","event":"audio_encode_recovered"}
```

### error

エラーが発生したときに記録されます。

```json
{"ts":"2026-04-30T19:30:00.000Z","event":"error","code":"RTSP_SEND_FAILED","message":"av_interleaved_write_frame failed: Broken pipe","monitor":"1","url":"rtsp://..."}
```

---

## エラーコード一覧

| コード | 説明 |
|---|---|
| `DXGI_INIT_FAILED` | DXGI Desktop Duplication 初期化失敗 |
| `ENCODER_INIT_FAILED` | エンコーダー初期化失敗（全コーデック試行後） |
| `ENCODER_ENCODE_FAILED` | エンコード中のエラー |
| `RTSP_CONNECT_FAILED` | RTSP サーバーへの接続失敗 |
| `RTSP_SEND_FAILED` | RTP パケット送信エラー |
| `RTSP_TIMEOUT` | RTSP 送信タイムアウト |
| `FATAL_ERROR` | 回復不能なエラー |
| `AUDIO_INIT_FAILED` | 音声パイプライン初期化失敗（映像配信は継続） |
| `AUDIO_ENCODE_FAILED` | 音声エンコード失敗（バッファを破棄して継続） |

---

## 監視システムとの連携例

### Prometheus テキスト形式への変換スクリプト例

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

### ヘルスチェックスクリプト例

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
