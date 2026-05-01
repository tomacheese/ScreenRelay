# screen-relay 設計書

## 1. 確定要件一覧

| カテゴリ | 項目 | 仕様 |
|---|---|---|
| 実行環境 | OS | Windows 11 (最新 LTS) |
| 実行環境 | GPU | NVIDIA GPU オプション (存在すれば NVENC 使用) |
| 実行環境 | ビルド | MSYS2 / MinGW-w64 (GCC 13+) |
| キャプチャ | API | DXGI / WGC — 設定ファイルで切替 |
| キャプチャ | カーソル | 非表示 (固定) |
| キャプチャ | 解像度 | 論理解像度 (DPI スケーリング後) |
| キャプチャ | フレームレート | 60 fps (設定可変) |
| キャプチャ | 音声 | スコープ外 |
| エンコード | コーデック | h264_nvenc → h264_mf → libx264 (モニター単位でフォールバック) |
| エンコード | ビットレート | デフォルト 4000 kbps (設定可変) |
| エンコード | GOP / B フレーム | GOP = FPS 値、B フレーム = 0 (低遅延固定) |
| モニター | 最大台数 | 4 台 |
| モニター | 番号体系 | Windows 設定アプリのディスプレイ番号 (\\.\DISPLAYn の n) |
| モニター | プライマリ特例 | screen0 を追加エイリアスとして発行 → プライマリは 2 ストリーム配信 |
| モニター | 変更検知 | ポーリング (デフォルト 1 秒)、接続時に再開・切断時に停止 |
| モニター | ミラー表示 | ソース画面のみ配信 |
| 配信 | プロトコル | RTSP (ANNOUNCE/RECORD) |
| 配信 | パスパターン | `/live/screen{n}` (設定可変) |
| 配信 | 許容遅延 | ≤ 3 秒 |
| 配信 | 再接続 | 指数バックオフ付き自動再接続 |
| 障害対応 | GPU クラッシュ | キャプチャ・エンコード再初期化 |
| ライブラリ | FFmpeg | 共有 LGPL ビルド (動的リンク) |
| ログ | 形式 | JSON Lines (spdlog)、レベル設定可変 |
| メトリクス | 出力 | FPS・遅延・ドロップ率 (JSON ファイル) |
| UI | 形式 | CLI のみ |
| 配布 | 形式 | exe + FFmpeg DLL 同梱 ZIP |

---

## 2. アーキテクチャ概要

```
main()
├── ConfigLoader          JSON 設定読込・バリデーション
├── Logger                spdlog JSON Lines
├── SignalHandler         Ctrl+C → グレースフルシャットダウン
├── MetricsStore          スレッドセーフなカウンタ群
└── MonitorSupervisor     全体オーケストレーター
    ├── MonitorDetector   モニター変化検知スレッド
    └── ScreenPipeline[]  モニターごとのパイプライン
        ├── CaptureBackend    (DXGI or WGC)
        ├── FramePump         キャプチャスレッド + 境界キュー
        ├── EncoderController FFmpeg H.264 エンコーダー
        ├── RtspPublisher[0]  /live/screen{n}
        └── RtspPublisher[1]  /live/screen0 (プライマリのみ)
```

### データフロー

```
[モニター (GPU テクスチャ)]
  ↓ CaptureBackend::acquire_frame()
[FrameBuffer (BGRA, 論理解像度)]
  ↓ FramePump → bounded queue (max 4)
[EncoderController::encode()]
  sws_scale: BGRA → NV12/YUV420P
  avcodec: → H.264 NAL packets
[EncodedPacket]
  ↓ fan-out
[RtspPublisher::send()] × 1 or 2
  RTSP ANNOUNCE/RECORD → MediaMTX
```

---

## 3. コンポーネント詳細

### 3.1 MonitorDetector

- `EnumDisplayMonitors()` を `monitor_check_interval_ms` ごとにポーリング
- 各 HMONITOR に対して `GetMonitorInfo()` → `MONITORINFOEX.szDevice` から番号抽出
  - `\\.\DISPLAY2` → monitor_number = 2
- `MONITORINFOF_PRIMARY` フラグでプライマリ判定
- ミラー表示の除外: `EnumDisplayDevices()` で `DISPLAY_DEVICE_MIRRORING_DRIVER` フラグをチェック
- 変化検知 (追加 / 削除 / 解像度変更) を `MonitorSupervisor` にコールバック通知

```
モニター状態変化の種別:
  ADDED     → ScreenPipeline 生成・起動
  REMOVED   → ScreenPipeline 停止・破棄
  RESIZED   → ScreenPipeline へ RECONFIGURING イベント送信
```

#### プライマリモニターの RTSP パス生成

```
primary monitor (\\.\DISPLAY2, Windows 番号 = 2)
  → paths: ["/live/screen2", "/live/screen0"]

non-primary monitor (\\.\DISPLAY1, Windows 番号 = 1)
  → paths: ["/live/screen1"]
```

### 3.2 CaptureBackend インターフェース

```cpp
class ICaptureBackend {
public:
    /** モニターに対してバックエンドを初期化する */
    virtual bool init(HMONITOR monitor, int logical_width, int logical_height) = 0;
    /** 最新フレームを取得する。フレームがなければ nullopt */
    virtual std::optional<FrameBuffer> acquire_frame(int timeout_ms) = 0;
    /** 解像度変更を通知する */
    virtual bool reconfigure(int new_width, int new_height) = 0;
    virtual void release() = 0;
};
```

#### DXGI バックエンド (DxgiCaptureBackend)

- `IDXGIFactory1` → アダプター列挙 → HMONITOR と `IDXGIOutput` を照合
- `IDXGIOutput1::DuplicateOutput` → `IDXGIOutputDuplication`
- `AcquireNextFrame` → デスクトップテクスチャ → ステージングテクスチャにコピー → Map → CPU BGRA
- 物理解像度で取得後、`sws_scale` で論理解像度にダウンスケール
- マルチ GPU 環境対応: `EnumDisplayDevices` でモニターのアダプター特定

#### WGC バックエンド (WgcCaptureBackend)

- `GraphicsCaptureItem::CreateForMonitor(HMONITOR)` でモニターキャプチャアイテム生成
- `Direct3D11CaptureFramePool` に論理解像度を直接指定 → スケーリング不要
- フレームポーリングまたは `FrameArrived` コールバック
- **リスク**: MinGW での C++/WinRT ヘッダー互換性 (→ 後述)

### 3.3 FramePump

- キャプチャスレッドで `CaptureBackend::acquire_frame()` をループ呼出
- `std::queue<FrameBuffer>` + `std::mutex` + `std::condition_variable`
- キューが満杯 (max 4) の場合は最古フレームを破棄してドロップカウント増加
- FPS 計測をここで行う

### 3.4 EncoderController

SpoutRelay から移植・拡張:

- フォールバックチェーン: `h264_nvenc` → `h264_mf` → `libx264`
- 入力: BGRA (FrameBuffer)
- `sws_scale`: BGRA → NV12 (NVENC) / YUV420P (libx264)
- 低遅延設定: `max_b_frames=0`, `gop_size=fps`, `tune=zerolatency`
- 解像度変更時: `avcodec_close` → 再初期化

### 3.5 RtspPublisher

SpoutRelay から移植・拡張:

- RTSP ANNOUNCE/RECORD プロトコル (FFmpeg `avformat`)
- `send(EncodedPacket)` を複数の Publisher が受け取るファンアウト構造
- 独立した接続状態と指数バックオフ再接続
- タイムスタンプ: `AV_TIME_BASE` 基準の単調増加

### 3.6 ScreenPipeline ステートマシン

```
          +-------+
     +--->| FATAL |
     |    +-------+
     |
+--------+    +------------+    +-----------+    +-----------+
|  INIT  |--->| CAPTURING  |--->| CONNECTING|--->| STREAMING |
+--------+    +------------+    +-----------+    +-----------+
                   ^                                  |   |
                   |           +-----------+          |   |
                   |           | RECONNECT |<---------+   |
                   |           +-----------+              |
                   |           +-------------+            |
                   +-----------|RECONFIGURING|<-----------+
                               +-------------+
```

| 状態 | 説明 |
|---|---|
| INIT | CaptureBackend・Encoder 初期化 |
| CAPTURING | フレーム取得中、RTSP 接続前 |
| CONNECTING | RTSP ANNOUNCE/RECORD 送信中 |
| STREAMING | 正常配信中 |
| RECONNECTING | RTSP 再接続中 (指数バックオフ) |
| RECONFIGURING | 解像度変更対応 → キャプチャ再初期化 |
| STOPPING | グレースフルシャットダウン |
| FATAL | 回復不能エラー (GPU クラッシュ後の再試行上限超え等) |

---

## 4. スレッドモデル

| スレッド | 数 | 役割 |
|---|---|---|
| メインスレッド | 1 | シグナル処理、MonitorSupervisor ループ |
| MonitorDetector | 1 | EnumDisplayMonitors ポーリング |
| キャプチャスレッド | モニター数 | CaptureBackend → FramePump |
| エンコード+送信スレッド | モニター数 | FramePump → Encoder → RtspPublisher |
| メトリクス書込スレッド | 1 | health.json / metrics.json を定期書込 |

プライマリモニターのパイプラインは、エンコード+送信スレッドが 2 つの `RtspPublisher` に同じ `EncodedPacket` を送出する。2 本の RTSP 接続の状態は独立して管理する。

---

## 5. 設定ファイル構造

```json
{
  "app": {
    "instance_name": "screen-relay",
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
    "tune": "zerolatency"
  },
  "rtsp": {
    "base_url": "rtsp://192.168.0.100:8554",
    "path_pattern": "/live/screen{n}",
    "connect_timeout_ms": 5000,
    "send_timeout_ms": 5000,
    "reconnect_delay_ms": 1000,
    "reconnect_max_delay_ms": 30000,
    "reconnect_backoff_multiplier": 2.0
  },
  "runtime": {
    "monitor_check_interval_ms": 1000,
    "shutdown_grace_ms": 3000,
    "emit_metrics_interval_ms": 1000
  }
}
```

---

## 6. ディレクトリ構成

```
screen-relay/
├── src/
│   ├── main.cpp
│   ├── app/
│   │   ├── supervisor.hpp/cpp          # MonitorSupervisor
│   │   └── state_machine.hpp/cpp
│   ├── capture/
│   │   ├── capture_backend.hpp         # ICaptureBackend インターフェース
│   │   ├── dxgi_capture.hpp/cpp        # DXGI 実装
│   │   ├── wgc_capture.hpp/cpp         # WGC 実装
│   │   └── frame_pump.hpp/cpp          # キャプチャスレッド + キュー
│   ├── common/
│   │   ├── types.hpp
│   │   ├── errors.hpp
│   │   └── time_utils.hpp
│   ├── config/
│   │   └── config_loader.hpp/cpp
│   ├── encoder/
│   │   └── encoder_controller.hpp/cpp
│   ├── logging/
│   │   └── log_sink.hpp/cpp
│   ├── metrics/
│   │   └── metrics_store.hpp/cpp
│   ├── monitor/
│   │   └── monitor_detector.hpp/cpp    # EnumDisplayMonitors + 番号マッピング
│   └── rtsp/
│       └── rtsp_publisher_client.hpp/cpp
├── tests/
│   └── unit/
├── config/
│   └── config.example.json
├── docs/
│   └── design.md
├── CMakeLists.txt
└── toolchain-mingw.cmake
```

---

## 7. 依存ライブラリ

| ライブラリ | バージョン | ライセンス | 形態 |
|---|---|---|---|
| FFmpeg | avcodec ≥ 62 | LGPL-2.1+ | 動的 DLL |
| spdlog | v1.15.1 | MIT | FetchContent (ヘッダー) |
| nlohmann/json | v3.12.0 | MIT | FetchContent (ヘッダー) |
| Windows SDK | (MSYS2 同梱) | - | システム |

FFmpeg は LGPL 動的リンクにより、アプリ本体を MIT ライセンスで配布可能。

---

## 8. 技術的リスクと対策

### R1: MinGW + C++/WinRT 互換性 (中リスク)

- **内容**: WGC バックエンドは C++/WinRT ヘッダーを使用する。MinGW での動作は未確認
- **対策**: ビルド初期段階で WGC の簡易プロトタイプを構築して検証。失敗した場合は WRL (Windows Runtime C++ Template Library) の COM ベース API に切替えるか、WGC バックエンドを MSVC 専用オプションとして分岐
- **影響範囲**: wgc_capture.hpp/cpp のみ

### R2: DXGI マルチ GPU 環境 (低リスク)

- **内容**: ノート PC 等の iGPU + dGPU 環境で、各モニターが異なるアダプターに接続される場合がある
- **対策**: `EnumDisplayDevices` でモニターのアダプター名を特定し、対応する `IDXGIAdapter` を使って `DuplicateOutput` を呼出す

### R3: Windows ディスプレイ番号の信頼性 (低リスク)

- **内容**: `\\.\DISPLAYn` の n が常に Windows 設定と一致するかは構成依存
- **対策**: 起動時に検出したマッピングをログに出力し、ユーザーが確認できるようにする。将来的に設定ファイルで手動マッピングを上書きできる拡張ポイントを設ける

### R4: NVENC セッション上限 (既知・設計済み)

- **内容**: コンシューマー GPU は同時 NVENC セッション数 2〜3 が上限
- **対策**: モニター単位で `h264_nvenc` 初期化を試みて失敗したら次のコーデックにフォールバック。混在運用を許容する設計

### R5: DRM 保護コンテンツ (受容済み)

- **内容**: DXGI / WGC ともに DRM 保護コンテンツは黒画面になる (Windows のセキュリティ仕様)
- **対策**: 仕様として受け入れる。ドキュメントに明記する

---

## 9. 実装フェーズ案

| フェーズ | 内容 |
|---|---|
| 1 | ビルド環境構築、CMakeLists.txt、FFmpeg リンク確認 |
| 2 | MonitorDetector + 番号マッピング |
| 3 | DXGI キャプチャバックエンド + FramePump |
| 4 | EncoderController (SpoutRelay から移植・調整) |
| 5 | RtspPublisher (SpoutRelay から移植・調整) |
| 6 | ScreenPipeline + ステートマシン統合 |
| 7 | MonitorSupervisor (追加・削除・再構成の動的管理) |
| 8 | ConfigLoader + Logger + MetricsStore |
| 9 | WGC バックエンド (MinGW 互換性確認後) |
| 10 | 結合テスト・ユニットテスト |
