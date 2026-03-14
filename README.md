# MMDDesktopMascot

Windows デスクトップ上に MMD モデルを表示するデスクトップマスコットアプリケーションです。

## 主な機能

- **PMX モデル表示** — PMX 形式の 3D モデルをデスクトップ上に描画
- **VMD モーション再生** — VMD モーションファイルによるアニメーション
- **物理演算** — Bullet Physics による髪・衣服等の物理シミュレーション
- **透過ウィンドウ** — DirectComposition による背景透過レンダリング
- **システムトレイ** — トレイアイコンからモデル・モーションの切替や設定変更
- **シャドウ / FXAA** — 影描画とアンチエイリアシング
- **視線追従 / 自動まばたき / 呼吸モーション** — 各種オプション表現

## 必要環境

- Windows 11 以降
- DirectX 12 対応 GPU

## CMake ビルド方法

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Visual Studio ソリューション ビルド方法

`MMDDesktopMascot.slnx` を開いて `Release|x64` でビルドできます。

初回ビルド時は、必要な Bullet ライブラリを `build/bullet-msbuild` に自動生成するため、通常より時間がかかる場合があります。

### ビルド出力

実行バイナリは `x64/<Config>/MMDDesktopMascot.exe` に生成されます。

## 使い方

1. `x64/Release/` と同階層に `models/` フォルダを作成し、PMX モデルを配置
2. VMD モーションファイルは `motions/` フォルダに配置
3. `MMDDesktopMascot.exe` を起動
4. システムトレイのアイコンを右クリックしてモデル・モーションの選択や設定変更が可能

## プロジェクト構成

```
MMDDesktopMascot/
├── Core/          # アプリケーション本体・設定管理
├── Rendering/     # DirectX 12 レンダリング・シェーダー
├── Model/         # PMX モデル読み込み・データ構造
├── Animation/     # VMD モーション・ボーンソルバー
├── Physics/       # Bullet Physics 統合
├── UI/            # Win32 UI (トレイメニュー・設定ウィンドウ)
├── Platform/      # リソース・マニフェスト
└── Common/        # 共通ユーティリティ
```

## 使用ライブラリ

[Bullet Physics](https://github.com/bulletphysics/bullet3) zlib License
[d3dx12.h](https://github.com/microsoft/DirectX-Headers) MIT License

## ライセンス

[MIT License](LICENSE) を参照してください。
