# トラブルシューティング

---

## 1. 起動・設定エラー

### `Failed to open config file`

設定ファイルが見つかりません。

- `--config` オプションで指定したパスを確認してください
- デフォルトは `config/config.json` です
- パスは実行ファイルからの相対パスまたは絶対パスで指定できます

```bash
screen-relay.exe --config C:\path\to\config.json
```

### `Config validation error: ...`

設定ファイルの JSON が不正または必須フィールドが欠けています。

- JSON の構文エラーがないか確認してください
- 必須フィールド（`rtsp.base_url` 等）が存在するか確認してください
- 設定リファレンス ([configuration.md](./configuration.md)) を参照してください

### `log_dir の作成に失敗`

ログディレクトリの作成権限がありません。

- `log_dir` に指定したディレクトリの親ディレクトリが存在するか確認してください
- ディレクトリへの書き込み権限があるか確認してください

---

## 2. DXGI キャプチャ関連

### `DXGI_INIT_FAILED`

DXGI Desktop Duplication の初期化に失敗しました。

**原因と対処法:**

| 原因 | 対処法 |
|---|---|
| モニターが接続されていない | 物理または仮想モニターを接続してください |
| `IDXGIOutput1` が取得できない | GPU ドライバーが DXGI 1.2 以上に対応しているか確認してください |
| 他のプロセスが Desktop Duplication を占有 | 競合するキャプチャソフトを終了してください |
| セッション 0 で実行している | インタラクティブなデスクトップセッション（通常ログオン状態）で実行してください |

### フレームが取得できない（映像が止まる）

- **DRM コンテンツが表示されている**: Protected content（Netflix 等）が画面に表示されている場合、DXGI Desktop Duplication はフレームを返さないことがあります。DRM コンテンツを非表示にするか、別のモニターに移動してください
- **`frame_timeout_ms` が短すぎる**: `capture.frame_timeout_ms` を 100ms 以上に増やしてみてください
- **ディスプレイドライバーの問題**: GPU ドライバーを最新版に更新してください

### 色がおかしい（赤と青が入れ替わっている）

DXGI Desktop Duplication は **BGRA** 形式でフレームを出力します（RGBA ではありません）。

- コードで swscale を使う際は `AV_PIX_FMT_BGRA` を入力フォーマットとして指定してください
- `AV_PIX_FMT_RGBA` と混同しないよう注意してください

### モニターが検出されない

- `\\.\DISPLAYn` 形式のデバイス名が正しく列挙されているか確認してください
- ディスプレイ設定（「ディスプレイの表示方法」）で「複製」や「拡張」が正しく設定されているか確認してください
- 仮想モニター（IDD Driver 等）はデバイス名が異なる場合があります

---

## 3. エンコーダー関連

### `ENCODER_INIT_FAILED`

全コーデックの初期化に失敗しました。

**コーデック別の確認事項:**

| コーデック | 確認事項 |
|---|---|
| `h264_nvenc` | NVIDIA GPU が搭載されているか、CUDA ドライバーが最新か |
| `h264_mf` | Windows 10 / 11 であること（Media Foundation は標準搭載） |
| `libx264` | FFmpeg ビルドに libx264 が含まれているか（LGPL ビルドには含まれない場合がある） |

FFmpeg ビルドの対応コーデック一覧を確認するには:

```bash
ffmpeg.exe -codecs | grep h264
```

### エンコード品質が悪い・遅延が大きい

- `encoder.bitrate_kbps` を増やしてください（高解像度では 8000 以上を推奨）
- `encoder.preset` を `"fast"` または `"ultrafast"` に設定してください
- `encoder.tune` を `"zerolatency"` に設定してください（低遅延配信向け）
- `encoder.max_b_frames` を `0` に設定してください（B フレームは遅延を増加させます）

---

## 4. RTSP 関連

### `RTSP_CONNECT_FAILED`

RTSP サーバーへの接続に失敗しました。

1. MediaMTX が起動しているか確認してください
2. `rtsp.base_url` のホスト・ポートが正しいか確認してください
3. ファイアウォールでポート（デフォルト 8554）が開いているか確認してください
4. `ping` や `telnet` でネットワーク疎通を確認してください

```bash
# ポートの疎通確認
telnet 192.168.0.100 8554
```

### `RTSP_SEND_FAILED` が頻発する

- **帯域不足**: `encoder.bitrate_kbps` を下げてください
- **タイムアウト**: `rtsp.send_timeout_ms` を増やしてください（デフォルト 5000ms）
- **ネットワークの不安定**: 有線接続を使用してください
- **MediaMTX の設定**: MediaMTX の `readBufferCount` / `writeQueueSize` を増やしてください

### `RTSP_TIMEOUT`

- `rtsp.send_timeout_ms` を増やしてください
- MediaMTX のログを確認し、サーバー側でエラーが出ていないか確認してください

### VLC で再生できない

1. MediaMTX が起動していること
2. `rtsp://host:8554/screen{n}` の `{n}` が正しいモニター番号であること
3. VLC の「ネットワークストリームを開く」で以下の URL を指定:

```
rtsp://192.168.0.100:8554/screen1
```

4. VLC の「ツール」→「環境設定」→「入力/コーデック」で「ネットワーク キャッシュ」を小さくすると遅延が減ります

### 再接続が多発する

- `rtsp.reconnect_delay_ms` と `rtsp.reconnect_max_delay_ms` を確認してください
- MediaMTX のセッションタイムアウト設定を確認してください
- ネットワークの品質を確認してください（パケットロス、ジッターなど）

---

## 5. ビルド関連

### `undefined reference to 'avcodec_...'`

FFmpeg のインポートライブラリがリンクできていません。

- `deps/ffmpeg/lib/` に `.dll.a` ファイルが存在するか確認してください
- LGPL shared ビルドの FFmpeg を使用しているか確認してください

```bash
ls deps/ffmpeg/lib/
# libavcodec.dll.a, libavformat.dll.a, libavutil.dll.a, libswscale.dll.a
```

### `cannot find -lm` または類似エラー

MinGW-w64 のインストールが不完全な可能性があります。

```bash
gcc --version  # 15.2.0 以上であることを確認
```

### DLL が見つからない（実行時エラー）

```bash
cp deps/ffmpeg/bin/*.dll build/
```

DLL を `build/` ディレクトリにコピーしてください。`PATH` に FFmpeg の `bin/` を追加する方法でも対応できます。

### CMake が toolchain ファイルを認識しない

```bash
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$(pwd)/toolchain-mingw.cmake \
  -DCMAKE_BUILD_TYPE=Release
```

絶対パスで `-DCMAKE_TOOLCHAIN_FILE` を指定してください。

---

## 6. E2E テスト手順

手動での E2E テストは以下の手順で実施してください。

### 1. MediaMTX の起動

```powershell
Start-Process deps\mediamtx\mediamtx.exe
```

または:

```bash
./deps/mediamtx/mediamtx.exe &
```

MediaMTX が起動すると `Listening on :8554 (RTSP)` のようなログが表示されます。

### 2. screen-relay の起動

```bash
./build/screen-relay.exe --config config/config.json
```

起動後、以下のようなログが表示されれば正常です:

```json
{"event":"state_changed","monitor":1,"from":"INIT","to":"CAPTURING"}
{"event":"encoder_initialized","monitor":1,"codec":"h264_nvenc"}
{"event":"publish_started","monitor":1,"url":"rtsp://192.168.0.100:8554/screen1"}
{"event":"state_changed","monitor":1,"from":"CONNECTING","to":"STREAMING"}
```

### 3. ffprobe でストリームを確認

```bash
ffprobe rtsp://192.168.0.100:8554/screen1
```

H.264 ビデオストリームの情報が表示されれば成功です。

### 4. ffplay でライブ再生

```bash
ffplay -fflags nobuffer -flags low_delay -rtsp_transport tcp rtsp://192.168.0.100:8554/screen1
```

### 5. VLC での確認

```powershell
vlc rtsp://192.168.0.100:8554/screen1
```

または:

```bash
"C:/Program Files/VideoLAN/VLC/vlc.exe" rtsp://192.168.0.100:8554/screen1
```

### 6. ヘルスチェックの確認

```bash
cat state/health.json
# "healthy": true であることを確認

cat state/metrics.json
# frames_received が増加し、frames_dropped が極端に多くないことを確認
```

### 7. モニター変更のテスト

1. モニターを接続・取り外して `monitor_added` / `monitor_removed` イベントがログに記録されること
2. 解像度を変更して `RECONFIGURING` → `CAPTURING` → `CONNECTING` → `STREAMING` の遷移が正常に行われること

---

## ログの読み方

ログは `log_dir` 以下に JSON Lines 形式で出力されます。以下のコマンドで確認できます。

```bash
# 最新のログをリアルタイムで表示 (<instance_name> は config.json の app.instance_name)
tail -f logs/<instance_name>.jsonl | python -m json.tool

# エラーイベントのみ抽出
grep '"event":"error"' logs/<instance_name>.jsonl

# 特定モニターのイベントを抽出
grep '"monitor":"1"' logs/<instance_name>.jsonl
```
