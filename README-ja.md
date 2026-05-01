# screen-relay

DXGI Desktop Duplication API で接続中の全モニターをキャプチャし、FFmpeg（NVENC → h264_mf → libx264 フォールバック）で H.264 エンコードして、[MediaMTX](https://github.com/bluenviron/mediamtx) などの RTSP サーバーへ個別のストリームとして配信する Windows アプリケーションです。

## 特徴

- **マルチモニターキャプチャ** — 接続されている各ディスプレイを個別の RTSP ストリームとして配信（`/screen1`、`/screen2`、…）
- **プライマリモニターエイリアス** — `/screen0` は常にプライマリモニターを指す
- **ハードウェア H.264 エンコード** — NVIDIA NVENC 優先、`h264_mf`（Windows Media Foundation）、`libx264` の順で自動フォールバック
- **動的モニター検出** — 再起動なしでモニターの接続・切断・解像度変更をランタイムで処理
- **自動再接続** — DXGI・エンコーダー・RTSP 障害時のエクスポネンシャルバックオフ。モニターの電源サイクル後も自己回復
- **構造化 JSON ログ** — タイムスタンプ付き JSON Lines イベントログ
- **ランタイムメトリクス** — `health.json` / `metrics.json` を毎秒ディスクへ書き出し
- **ステートマシン駆動** — モニターごとの明示的なパイプライン状態遷移。障害時の未定義動作なし

## 動作要件

### 実行時

| コンポーネント | 備考 |
|--------------|------|
| Windows 10 / 11 (x64) | 必須 |
| NVIDIA GPU | 任意 — NVENC ハードウェア H.264 が使用可能。なければ `h264_mf`（Windows Media Foundation）、次いで `libx264` にフォールバック |
| FFmpeg 共有ライブラリ | リリース ZIP に同梱済み（`avcodec`、`avformat`、`avutil`、`swscale`、`swresample`） |
| RTSP サーバー | [MediaMTX](https://github.com/bluenviron/mediamtx) v1.x 等 — ストリームの受信側として別途起動が必要 |

### ビルド時

| コンポーネント | バージョン |
|--------------|----------|
| GCC (MinGW-w64) | ≥ 13 推奨 |
| CMake | ≥ 3.20 |
| Ninja | 最新版 |
| FFmpeg 開発ファイル | [win64-lgpl-shared](https://github.com/BtbN/FFmpeg-Builds/releases)、avcodec ≥ 62 |

## クイックスタート

### 1. Releases からダウンロード

1. [最新リリース](https://github.com/tomacheese/screen-relay/releases/latest) を開く
2. `screen-relay-vX.Y.Z-win64.zip` をダウンロード
3. 任意のフォルダ（例: `C:\Tools\screen-relay\`）に展開

展開フォルダには以下が含まれます。

- `screen-relay.exe` — 本体実行ファイル
- `*.dll` — FFmpeg LGPL 共有ライブラリ（同じフォルダに置く必要があります）
- `config.example.json` — 設定テンプレート（コメント付き）
- `README.md`

### 2. 設定ファイルを作成

`config.example.json` を同じフォルダに `config.json` としてコピーし、最低限以下を設定します。

```json
{
  "rtsp": {
    "base_url": "rtsp://<mediamtx-host>:8554",
    "path_pattern": "/screen{n}"
  }
}
```

`<mediamtx-host>` は [MediaMTX](https://github.com/bluenviron/mediamtx) サーバーの IP アドレスまたはホスト名に置き換えてください。

### 3. 起動

```bat
screen-relay.exe --config config.json
```

各モニターが個別の RTSP ストリームとして利用できるようになります。

| モニター | ストリーム URL |
|---------|--------------|
| プライマリ（例: DISPLAY1） | `rtsp://<host>:8554/screen1` および `rtsp://<host>:8554/screen0` |
| DISPLAY2 | `rtsp://<host>:8554/screen2` |
| DISPLAY3 | `rtsp://<host>:8554/screen3` |

VLC、FFplay などの RTSP 対応プレイヤーで確認できます。

```powershell
ffplay rtsp://<mediamtx-host>:8554/screen1
```

`Ctrl+C` でグレースフルシャットダウンします。

---

## ソースからビルド

```powershell
# 1. FFmpeg LGPL 共有ビルドを deps/ffmpeg/ に配置
#    https://github.com/BtbN/FFmpeg-Builds/releases からダウンロード
#    (ffmpeg-master-latest-win64-lgpl-shared.zip → deps/ffmpeg/ に展開)

# 2. MinGW ツールチェーンで CMake 設定
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake -DBUILD_TESTS=ON

# 3. ビルド
cmake --build build

# 4. FFmpeg DLL を実行ファイルと同じ場所へコピー
Copy-Item deps\ffmpeg\bin\*.dll build\

# 5. 設定ファイルを編集
cp config/config.example.json config/config.json
# rtsp.base_url を設定

# 6. MediaMTX（または任意の RTSP サーバー）を起動
mediamtx.exe

# 7. 起動
build/screen-relay.exe --config config/config.json
```

詳細な手順は [`docs/build.md`](docs/build.md) を参照してください。

## 使い方

```powershell
screen-relay.exe --config <config.json へのパス>
screen-relay.exe --help
```

`Ctrl+C` または `CTRL_CLOSE_EVENT` でグレースフルシャットダウンします。

## 設定

完全なサンプル: [`config/config.example.json`](config/config.example.json)  
設定リファレンス: [`docs/configuration.md`](docs/configuration.md)

### 最小構成

```json
{
  "rtsp": {
    "base_url": "rtsp://192.168.0.100:8554",
    "path_pattern": "/screen{n}"
  }
}
```

`{n}` はモニター番号（1、2、3、…）に置き換えられます。プライマリモニターには `/screen0` のエイリアスも付与されます。

## ディレクトリ構成

```text
screen-relay/
├── src/
│   ├── app/          # MonitorSupervisor + ScreenPipeline + StateMachine
│   ├── capture/      # ICaptureBackend、DxgiCaptureBackend、FramePump
│   ├── common/       # 型定義、エラーコード、時刻ユーティリティ
│   ├── config/       # JSON 設定ローダー
│   ├── encoder/      # FFmpeg H.264 エンコーダー（NVENC / h264_mf / libx264）
│   ├── logging/      # spdlog JSON Lines シンク
│   ├── metrics/      # MetricsStore → health.json / metrics.json
│   ├── monitor/      # MonitorDetector（EnumDisplayMonitors）
│   └── rtsp/         # FFmpeg RTSP ANNOUNCE/RECORD クライアント
├── tests/
│   └── unit/         # カスタムテストランナーによるユニットテスト
├── config/
│   ├── config.example.json
│   └── config.json   # （.gitignore 対象、ローカル設定）
└── docs/             # カテゴリ別ドキュメント（日本語）
```

## ドキュメント一覧

| ドキュメント | 内容 |
|-------------|------|
| [docs/architecture.md](docs/architecture.md) | システムアーキテクチャ、データフロー、スレッドモデル |
| [docs/build.md](docs/build.md) | 詳細ビルド手順・依存関係セットアップ |
| [docs/configuration.md](docs/configuration.md) | 設定項目の完全リファレンス |
| [docs/state-machine.md](docs/state-machine.md) | ステートマシン：状態・遷移・エラー処理 |
| [docs/metrics.md](docs/metrics.md) | メトリクス/ヘルス JSON フォーマット、イベントログ |
| [docs/troubleshooting.md](docs/troubleshooting.md) | よくあるエラーと解決策 |

## テストの実行

```powershell
build/tests/screen_relay_tests.exe
# 期待結果の末尾: All tests passed.
```

## ライセンス

このプロジェクトのライセンスは [MIT License](LICENSE) です。

本プロジェクトは以下を使用しています。

- [FFmpeg](https://ffmpeg.org/) — LGPL-2.1+（win64-lgpl-shared ビルド）
- [nlohmann/json](https://github.com/nlohmann/json) — MIT
- [spdlog](https://github.com/gabime/spdlog) — MIT
- [winpthread (mingw-w64)](https://sourceforge.net/projects/mingw-w64/) — BSD 2-Clause

ライセンス全文は [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) を参照してください。
