# VILLIEN Editor ランチャー

`featuretools/` は、VILLIEN の実ランタイムを使用する Editor の起動・運用フォルダーです。現在の Editor 実行ファイルは `VillienEditor.exe` であり、デモ用 Renderer を複製したものではありません。

`VillienEditor` は `SoloAssignment` と同じ engine、game、editor source から build され、実行時に project root の `Assets/` と `shaders/` を読み込みます。大容量 Asset は Editor output へ複製しません。

## 構成

```text
VILLIEN root source and assets
  |-- SoloAssignment.exe   Game target
  `-- VillienEditor.exe    Editor target (VILLIEN_EDITOR_BUILD)
        `-- SceneEditor + real DirectX 12 runtime

featuretools/
  |-- RunFeatureTools.bat  Configure, build, and launch VillienEditor
  |-- README.md            Operation and verification notes
  `-- legacy lab sources   Deprecated reference implementation
```

Editor は project root を working directory として起動するため、本物の `Assets/` と `shaders/` を使用します。専用 target は compile-time に Editor mode を有効化し、launcher も `--editor` を渡します。

## 起動方法

File Explorer から次のファイルをダブルクリックします。

```text
featuretools/RunFeatureTools.bat
```

Launcher は以下を自動実行します。

1. Launcher の位置から VILLIEN project root を解決します。
2. `PATH` 上の CMake、または Visual Studio 2026 付属 CMake を検索します。
3. 必要な場合だけ `build-codex/` を configure します。
4. Debug の `VillienEditor` target のみを build します。
5. VILLIEN root を working directory として Editor を起動します。

失敗した場合は console を閉じず、必要な tool、target、または expected path を表示します。
Debug 初回起動では root Asset／Shader の初期化に約 20～25 秒かかる場合があります。Window title に FPS が表示され、Scene View が描画されるまで待ってください。

## 手動 Build

VILLIEN project root で実行します。

```powershell
cmake -S . -B build-codex -G "Visual Studio 18 2026" -A x64
cmake --build build-codex --config Debug --target VillienEditor -j
.\build-codex\bin\Debug\VillienEditor.exe --editor
```

出力先：

```text
build-codex/bin/Debug/VillienEditor.exe
```

## 実装済み Editor Workflow

- `Scene Objects`、live `Scene View`、`Inspector`、`Project`、`Systems`、`Console` を可変 splitter で配置した workspace。
- DirectX 12 の最終 backbuffer を表示する Scene View、viewport-local picking、`W`／`E`／`R` ImGuizmo、Local／World 操作。
- Entity の作成、選択、複製、削除、Transform／Material／Mesh／Light 編集、Undo／Redo、JSON scene load／save。
- `Environment`、`Rendering`、`World`、`VFX`、`Camera`、`Features` tab から root runtime の設定を直接調整。
- Weather／time、post-process、water、wet surface／puddle、forest、collision、particles、animation preview、camera の実 runtime binding。
- `Camera Navigation` checkbox。OFF の間は Scene View の mouse、keyboard、wheel 移動を停止し、数値入力と Camera Preset は継続利用できます。
- `feature.md` から **219 個の main inventory rows** と **42 個の Showcase IDs** を読み込む Feature Coverage matrix。42 ID はすべて real checkbox、control panel、Game View、observed core、または Legacy-only に明示的に route されます。
- `Window` menu から各 Systems tab と Feature Coverage を直接選択し、Camera Navigation も即時 lock／unlock。
- F5 による editor scene snapshot preview／restore。
- 本物の Title、Overworld、BossArena を Game View として起動し、`F1` で authoring scene へ復帰。
- Local runtime settings を `featuretools/editor_runtime_settings.json` へ save／load。

### 主な操作

| Control | Action |
|---|---|
| `F1` | Overworld／BossArena Game View から Editor へ戻る |
| `F5` | Editor scene snapshot preview を Play／Stop |
| `F6` | Grid Editor panel を開閉する |
| `F9` | Shader を reload する |
| `W`, `E`, `R` | Translate／Rotate／Scale gizmo |
| Right mouse + movement keys | Scene View camera を操作する |

`feature.md` は Feature inventory／coverage contract です。`Inventory` tab では全 219 rows を検索・status filter でき、`Showcase` tab では 42 IDs の route を検証できます。Markdown の各行から unsafe な runtime switch を自動生成するものではありません。Root runtime に接続した安全な control のみ checkbox とし、core renderer と gameplay state machine は本物の panel／Game View で確認します。

## Debug 検証結果

2026-08-08、Windows／Visual Studio 2026 Debug toolchain で以下を確認しました。

- `VillienEditor` と既存 `SoloAssignment` の両 target が build 成功。
- Editor が root Asset／Shader を使用して 15 秒間継続動作。
- Scene View rendering、object selection、Inspector population、gizmo 表示、Play／Stop snapshot restore が正常動作。
- 本物の Overworld と BossArena が Editor から起動し、`F1` で正常復帰。
- Water と Forest value を使用した runtime settings の save／load round trip が成功。
- 既存 game target が別の 12 秒 startup smoke test を通過。
- 最終の Camera Navigation lock 追加後、両 target を再 build し、Editor 10 秒／Game 8 秒の regression smoke test を通過。
- Feature Coverage landing view が **219 + 42** rows を読み込み、**42／42 mapped** と表示することを direct window capture で確認。
- 最終 build 後、Editor は約 20 秒、既存 Game は 25 秒以内に実 FPS frame loop へ到達しました。これは startup readiness の確認であり、performance benchmark ではありません。
- F5 を Editor mode のみに限定し、小 window layout clamp、Forest Density `0～3`、Editor viewport texture の lazy allocation を修正。

Release build、最終 performance benchmark、Gold Master 判定は実施していません。

## Scope と制限

VILLIEN Editor は、既存 VILLIEN runtime 専用の Unity-style authoring front end です。Unity／Unreal Engine と同等の汎用 Engine を完成済みと主張するものではありません。

- `Scene Objects` は現時点では flat entity list で、parent-child hierarchy は未実装です。
- `Project` は asset browser であり、完全な importer／asset database ではありません。
- F5 は scene snapshot preview／restore で、完全に分離された Unity Play Mode ではありません。
- Prefab、generic component add/remove、audio authoring、NPC dialogue、quest、inventory、game save/load は未実装です。
- Terrain LOD は experimental で、現在の default editor scene には instance 化していません。

これらは `Features` の Inventory／Limits view で unavailable／experimental として表示し、未実装機能を simulation で置き換えません。

## Deprecated standalone lab

旧 `FeatureTools.exe` project は、deprecated reference implementation として一時的に残しています。

```text
featuretools/CMakeLists.txt
featuretools/src/
featuretools/shaders/
featuretools/tests/
```

これは自己完結型 procedural DirectX 12 lab であり、正式な VILLIEN Editor ではありません。`RunFeatureTools.bat` はこの target を起動しません。Regression 比較が必要な場合のみ、`featuretools/CMakePresets.json` から build し、`featuretools/build/bin/Debug/FeatureTools.exe --self-test` を直接実行します。
