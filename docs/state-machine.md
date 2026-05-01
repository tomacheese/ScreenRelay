# ステートマシン

## 概要

`ScreenPipeline` は `StateMachine` クラスによって状態管理されます。状態遷移テーブルは `state_machine.cpp` に定義されており、許可されていない遷移は実行時に検出・ログ記録されます。

---

## 状態一覧

| 状態 | 説明 |
|---|---|
| `INIT` | 起動直後。DXGI 初期化・エンコーダー確認を行う |
| `CAPTURING` | フレームキャプチャ中。エンコーダー初期化待ち |
| `CONNECTING` | エンコーダー初期化完了 → RTSP 接続を試みる |
| `STREAMING` | 正常配信中。フレームキャプチャ → エンコード → RTSP 送信 |
| `RECONNECTING` | RTSP 切断後の再接続待機（指数バックオフ） |
| `RECONFIGURING` | 解像度変更検出。エンコーダー・RTSP を再初期化 |
| `STOPPING` | シャットダウン処理中 |
| `FATAL` | 回復不能エラー。このモニターのパイプライン終了 |

---

## 状態遷移図

```text
        ┌─────────┐
        │  INIT   │
        └────┬────┘
             │ DXGI 初期化成功
             ▼
        ┌────────────┐
   ┌───▶│ CAPTURING  │◀───────────────┐
   │    └─────┬──────┘                │
   │          │ エンコーダー初期化成功 │
   │          ▼                       │
   │    ┌────────────┐                │
   │    │ CONNECTING │                │ 最大回数超過後
   │    └─────┬──────┘                │
   │          │ RTSP 接続成功         │
   │          ▼                       │
   │    ┌────────────┐                │
   │    │ STREAMING  │────────────────┤
   │    └──────┬─────┘ RECONFIGURING  │
   │           │ RTSP エラー          │
   │           ▼                      │
   │    ┌──────────────┐              │
   └────│ RECONNECTING │──────────────┘
        └──────────────┘
```

`INIT` / `CAPTURING` / `CONNECTING` / `STREAMING` / `RECONNECTING` / `RECONFIGURING` のいずれの状態からも `STOPPING` および `FATAL` へ遷移できます（シャットダウン要求または回復不能エラーの発生時）。

---

## 有効な遷移一覧

| From | To | 条件 |
|---|---|---|
| `INIT` | `CAPTURING` | DXGI 初期化成功 |
| `INIT` | `FATAL` | DXGI 初期化失敗 |
| `CAPTURING` | `CONNECTING` | エンコーダー初期化成功 |
| `CAPTURING` | `STOPPING` | シャットダウン要求 |
| `CAPTURING` | `FATAL` | エンコーダー初期化失敗（全フォールバック失敗） |
| `CONNECTING` | `STREAMING` | RTSP 接続成功 |
| `CONNECTING` | `CAPTURING` | RTSP 接続失敗（再試行） |
| `CONNECTING` | `STOPPING` | シャットダウン要求 |
| `STREAMING` | `RECONNECTING` | RTSP 送信エラー |
| `STREAMING` | `RECONFIGURING` | 解像度変更検出 |
| `STREAMING` | `STOPPING` | シャットダウン要求 |
| `STREAMING` | `FATAL` | 回復不能エラー |
| `RECONNECTING` | `STREAMING` | 再接続成功 |
| `RECONNECTING` | `CAPTURING` | 最大再接続回数超過後のリセット |
| `RECONNECTING` | `STOPPING` | シャットダウン要求 |
| `RECONFIGURING` | `CAPTURING` | 再設定完了 |
| `RECONFIGURING` | `STOPPING` | シャットダウン要求 |

---

## 各状態の詳細動作

### INIT

1. `DxgiCaptureBackend` を初期化（`IDXGIOutput1::DuplicateOutput` を呼び出す）
2. 成功すれば `CAPTURING` へ遷移
3. 失敗すれば `FATAL` へ遷移（エラーコード: `DXGI_INIT_FAILED`）

### CAPTURING

1. `FramePump` を起動してキャプチャスレッドを開始
2. バックグラウンドでキャプチャスレッドが `DxgiCaptureBackend::acquire_frame()` をポーリング
3. エンコーダー初期化を試みる（コーデックフォールバックあり）
4. 成功すれば `CONNECTING` へ遷移
5. 全フォールバック失敗で `FATAL` へ遷移（エラーコード: `ENCODER_INIT_FAILED`）

### CONNECTING

1. `RtspPublisherClient::connect()` を呼び出す（ANNOUNCE → SETUP → RECORD）
2. 成功すれば `STREAMING` へ遷移
3. 失敗すれば `CAPTURING` へ戻り再試行

### STREAMING

1. キューからフレームを取り出してエンコード
2. エンコード済みパケットを RTSP で送信
3. RTSP 送信エラーが発生すれば `RECONNECTING` へ遷移
4. 解像度変更を検出すれば `RECONFIGURING` へ遷移

### RECONNECTING

1. 指数バックオフで待機
2. 待機後に `RtspPublisherClient::connect()` を再試行
3. 成功すれば `STREAMING` へ遷移
4. 最大再接続回数を超えると `CAPTURING` へ戻ってエンコーダーから再初期化

### RECONFIGURING

1. エンコーダーを解放して再初期化
2. RTSP 接続を解放
3. 完了後 `CAPTURING` へ遷移（エンコーダー初期化・RTSP 接続を再実行）

### STOPPING

1. キャプチャスレッドに停止シグナルを送信
2. RTSP 接続を正常終了（TEARDOWN）
3. エンコーダーを解放
4. スレッド終了を待機（`shutdown_grace_ms` でタイムアウト）

### FATAL

1. すべてのリソースを解放
2. このモニターの `ScreenPipeline` スレッドが終了
3. `MonitorSupervisor` に FATAL 通知が送られ、`health.json` に反映される

---

## 状態変化のイベントログ

状態が変化するたびにイベントログに記録されます。詳細は [metrics.md](./metrics.md) の「イベントログ」セクションを参照してください。
