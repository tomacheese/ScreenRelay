# ビルド手順

## 前提条件

| ツール | バージョン | 備考 |
|---|---|---|
| GCC (MinGW-w64) | 15.2.0 以上 | `gcc --version` で確認 |
| CMake | 3.25 以上 | `cmake --version` で確認 |
| Ninja | 1.11 以上 | `ninja --version` で確認 |
| FFmpeg | LGPL shared ビルド | 後述の配置手順を参照 |

> **注意**: MSVC はサポートしていません。必ず MinGW-w64 ツールチェーンを使用してください。

---

## FFmpeg の配置

FFmpeg の LGPL shared ビルドを `deps/ffmpeg/` に配置します。

```text
deps/
└── ffmpeg/
    ├── include/
    │   ├── libavcodec/
    │   ├── libavformat/
    │   ├── libavutil/
    │   └── libswscale/
    └── lib/
        ├── libavcodec.dll.a
        ├── libavformat.dll.a
        ├── libavutil.dll.a
        └── libswscale.dll.a
```

FFmpeg のビルド済みバイナリは [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases) から入手できます。  
`ffmpeg-n*-win64-lgpl-shared-*.zip` をダウンロードし、`include/` と `lib/` を上記のパスに展開してください。

---

## CMake 設定

MinGW ツールチェーンを使用して CMake を設定します。

```bash
cmake -B build \
  -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake \
  -DCMAKE_BUILD_TYPE=Release
```

デバッグビルドの場合:

```bash
cmake -B build \
  -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake \
  -DCMAKE_BUILD_TYPE=Debug
```

---

## ビルド

### 全ターゲットをビルド

```bash
cmake --build build
```

### 個別ターゲットのビルド

```bash
# メインバイナリのみ
cmake --build build --target screen-relay

# テストバイナリのみ
cmake --build build --target screen_relay_tests
```

ビルド成功後、以下のファイルが生成されます。

```text
build/
├── screen-relay.exe
└── screen_relay_tests.exe
```

---

## FFmpeg DLL のコピー

実行時に FFmpeg の DLL が必要です。ビルドディレクトリにコピーしてください。

```bash
# Windows (Git Bash)
cp deps/ffmpeg/bin/*.dll build/
```

または CMake のインストールターゲットを使用:

```bash
cmake --build build --target install
```

`build/` に以下の DLL が配置されます（バージョンは FFmpeg のビルドに依存）。

```text
build/
├── screen-relay.exe
├── screen_relay_tests.exe
├── avcodec-*.dll
├── avformat-*.dll
├── avutil-*.dll
└── swscale-*.dll
```

---

## テストの実行

```bash
cd build
./screen_relay_tests.exe
```

または CTest を使用:

```bash
ctest --test-dir build --output-on-failure
```

---

## CMake ターゲット一覧

| ターゲット | 種別 | 説明 |
|---|---|---|
| `screen_relay_lib` | 静的ライブラリ | コアロジック。`screen-relay` と `screen_relay_tests` が共有する |
| `screen-relay` | 実行ファイル | メインアプリケーション (`screen-relay.exe`) |
| `screen_relay_tests` | 実行ファイル | 単体テスト (`screen_relay_tests.exe`) |

---

## トラブルシューティング

### `libavcodec.dll.a` が見つからないと言われる

FFmpeg の `lib/` ディレクトリが正しく配置されているか確認してください。  
インポートライブラリ（`.dll.a`）は shared ビルドのみに含まれます。static ビルドでは異なるファイル名になります。

### `cc1: error: unrecognized command-line option` が出る

MinGW-w64 のバージョンが古い可能性があります。GCC 15.2.0 以上を使用してください。

### Ninja が見つからないと言われる

```bash
# Scoop を使用している場合
scoop install ninja

# または CMake の -G オプションを "MinGW Makefiles" に変更
cmake -B build -G "MinGW Makefiles" -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake
```
