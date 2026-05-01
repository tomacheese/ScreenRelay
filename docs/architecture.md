# アーキテクチャ

## 概要

screen-relay は、Windows の DXGI Desktop Duplication API を使用して複数モニターの映像をキャプチャし、H.264 エンコード後に RTSP で配信するアプリケーションです。

---

## システム構成図

```text
┌──────────────────────────────────────────────────────────────────────┐
│   MonitorSupervisor                                                  │
│   ┌──────────────────────────────────────────────────────────────┐   │
│   │  monitor_poll_loop              metrics_writer_loop          │   │
│   │  (1秒ごとにモニター変更を検出)    (1秒ごとにJSON出力)         │   │
│   └──────────────────────────────────────────────────────────────┘   │
│                                                                      │
│  ┌─────────────────────┐   ┌─────────────────────┐                  │
│  │  ScreenPipeline (1) │   │  ScreenPipeline (2) │   ...            │
│  │  DISPLAY1           │   │  DISPLAY2           │                  │
│  │  3840x2160          │   │  1280x1024          │                  │
│  └──────────┬──────────┘   └─────────────────────┘                  │
└─────────────│────────────────────────────────────────────────────────┘
              │
              ▼
   ┌──────────────────────┐
   │  DxgiCaptureBackend  │
   │  IDXGIOutputDupl     │
   └──────────┬───────────┘
              │ BGRA フレーム
              ▼
   ┌──────────────────────┐
   │   FramePump          │
   │   (有界キュー 最大4)  │
   └──────────┬───────────┘
              │
              ▼
   ┌──────────────────────┐
   │  EncoderController   │
   │  nvenc→mf→libx264    │
   │  (FFmpeg + swscale)  │
   └──────────┬───────────┘
              │ H.264 NAL パケット
              ▼
   ┌──────────────────────────────────────────────────────────────────┐
   │  RtspPublisherClient × N (通常1本、プライマリは2本)              │
   │  /screen1  (+/screen0 for primary)                               │
   └──────────┬─────────────────────────────────────────────────────┘
              │ RTP/TCP
              ▼
   ┌──────────────────────┐
   │   MediaMTX (等)      │
   │   RTSP サーバー      │
   └──────────────────────┘
```

---

## コンポーネント説明

| コンポーネント | クラス | 役割 |
|---|---|---|
| オーケストレーター | `MonitorSupervisor` | モニター変更検出、ScreenPipeline のライフサイクル管理 |
| パイプライン | `ScreenPipeline` | 1 モニター分のキャプチャ→エンコード→配信 |
| ステートマシン | `StateMachine` | 状態遷移テーブルの強制、コールバック通知 |
| キャプチャバックエンド | `DxgiCaptureBackend` | DXGI Desktop Duplication API によるスクリーン取得 |
| キャプチャキュー | `FramePump` | キャプチャスレッド実行、最大 4 フレームの有界キュー |
| エンコーダー | `EncoderController` | FFmpeg H.264 エンコード（NVENC/h264_mf/libx264）、swscale BGRA→YUV |
| RTSP クライアント | `RtspPublisherClient` | FFmpeg RTSP ANNOUNCE+SETUP+RECORD、RTP パケット送信 |
| メトリクス | `MetricsStore` | スレッドセーフなカウンター、health.json / metrics.json 出力 |
| ログ | `LogSink` | spdlog ベース JSON Lines ログ（ファイル＋コンソール） |
| 設定 | `ConfigLoader` | nlohmann/json による設定ロード・バリデーション |
| モニター検出 | `MonitorDetector` | EnumDisplayMonitors によるモニター一覧取得 |

---

## データフロー

```text
[DXGI IDXGIOutputDuplication]
       │
       │  AcquireNextFrame() → staging_texture → Map() → BGRA バイト列
       ▼
[FrameBuffer (BGRA)]
       │
       │  push (非ブロッキング、満杯なら drop)
       ▼
[有界キュー (max=4)]
       │
       │  pop (エンコードスレッド)
       ▼
[sws_scale: BGRA → YUV420P/NV12]
       │
       │  avcodec_send_frame / avcodec_receive_packet
       ▼
[AVPacket (H.264 NAL)]
       │
       │  av_interleaved_write_frame
       ▼
[RTSP RTP/TCP → MediaMTX]
```

---

## スレッドモデル

```text
メインスレッド (MonitorSupervisor::run)
│
├── monitor_poll_loop スレッド
│   └── 1秒ごとにモニター変更（追加・削除・解像度変更）を検出
│       → ScreenPipeline の追加・停止・再設定
│
├── metrics_writer_loop スレッド
│   └── 1秒ごとに health.json / metrics.json を書き出し
│
└── [モニターごとに]
    ScreenPipeline::run スレッド
    │
    ├── ステートマシンループ
    │   └── 各状態ハンドラーが同期的に処理
    │
    └── キャプチャスレッド (FramePump)
        └── DxgiCaptureBackend::acquire_frame() をポーリング
            → FrameBuffer をキューへ push
```

---

## エンコーダーフォールバック

コーデックは設定の `codec` → `fallback_codecs` の順に試行します。`avcodec_open2()` が失敗した場合は次のコーデックへフォールバックします。

```text
avcodec_find_encoder("h264_nvenc")
      │
      ├── 成功 → avcodec_open2() → 成功 → NVENC 使用
      │                          └── 失敗 → 次へ
      └── 失敗 → avcodec_find_encoder("h264_mf")
                        │
                        ├── 成功 → avcodec_open2() → 成功 → h264_mf 使用
                        │                          └── 失敗 → 次へ
                        └── 失敗 → avcodec_find_encoder("libx264")
                                          │
                                          └── avcodec_open2() → libx264 使用
```

全コーデックの試行に失敗した場合、パイプラインは `FATAL` 状態に遷移します。

---

## プライマリモニターの二重配信

プライマリモニター（デバイス名 `\\.\DISPLAY1` 等の最初のモニター）は 2 本の `RtspPublisherClient` を持ちます。

```text
ScreenPipeline (primary=true)
├── RtspPublisherClient → rtsp://host:8554/screen1
└── RtspPublisherClient → rtsp://host:8554/screen0  (エイリアス)
```

これにより `/screen0` でプライマリモニターの映像を常に受信できます。モニター番号を意識せずにプライマリ映像を参照したいクライアントはこの URL を使用してください。

---

## 依存ライブラリ

| ライブラリ | 用途 | 形式 |
|---|---|---|
| FFmpeg (avcodec/avformat/swscale) | エンコード・RTSP 配信 | 動的リンク (.dll) |
| nlohmann/json | 設定ファイルパース | ヘッダーオンリー |
| spdlog | 構造化ログ | ヘッダーオンリー |
| Direct3D 11 / DXGI | スクリーンキャプチャ | Windows システム |

---

## 既知の技術的制約

- **RTSP トランスポート**: 現状 TCP のみ（UDP 非対応）
- **ピクセルフォーマット**: DXGI は BGRA で出力する（RGBA ではない）。swscale で YUV に変換する際は `AV_PIX_FMT_BGRA` を指定すること
- **キャプチャバックエンド**: 現在 DXGI のみ実装（WGC は未実装）。`ICaptureBackend` インターフェースを実装することで追加バックエンドを組み込み可能
- **モニター番号**: デバイス名 `\\.\DISPLAYn` の `n` を使用。OS によって番号が変わる場合がある
- **DRM コンテンツ**: Protected content（DRM）が有効な映像の含まれる画面は DXGI Desktop Duplication が失敗する場合がある
