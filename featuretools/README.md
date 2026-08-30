# VILLIEN Editor ランチャー

`featuretools/` は、VILLIEN の実ランタイムを使用する統合 Editor の起動・技術確認用フォルダーです。正式な実行ファイルは root project の `VillienEditor.exe` であり、`SoloAssignment.exe` と同じ engine、game、editor source、`Assets/`、`shaders/` を使用します。

このフォルダーには、過去の比較確認に使う自己完結型 `FeatureTools.exe` も残っています。ただし、これは deprecated standalone lab です。現在の機能追加、DXR、実ゲーム Scene、Editor workflow の確認先は `VillienEditor.exe` です。

## 構成

```text
VILLIEN root
  |-- build/bin/Debug/SoloAssignment.exe
  `-- build/bin/Debug/VillienEditor.exe
        `-- real VILLIEN runtime + SceneEditor

featuretools/
  |-- RunFeatureTools.bat       Debug Editor launcher
  |-- feature_catalog.md        公開可能な技術 showcase contract
  |-- README.md                 起動方法と運用範囲
  `-- src/, shaders/, tests/    deprecated standalone lab
```

Editor は VILLIEN root を working directory として起動します。大容量 Asset を Editor output へ複製せず、root の Asset／Shader を直接読み込みます。

## 起動方法

File Explorer から次をダブルクリックします。

```text
featuretools/RunFeatureTools.bat
```

Launcher は次の処理だけを行います。

1. `featuretools/` の親を VILLIEN root として解決します。
2. Visual Studio 2026 Developer environment を初期化し、`PATH` または Visual Studio 付属の CMake を検索します。
3. root の唯一の build directory である `build/` を、必要な場合だけ configure します。
4. Debug の `VillienEditor` target のみを build します。
5. VILLIEN root を working directory として `VillienEditor.exe --editor` を起動します。

Launcher は Release build、package 作成、commit、push を行いません。

## 手動 Debug build

VILLIEN root で実行します。

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Debug --target VillienEditor -j
.\build\bin\Debug\VillienEditor.exe --editor
```

出力先：

```text
build/bin/Debug/VillienEditor.exe
```

既存の `build/` に generator／compiler 問題がある場合は、その `build/` を修復または再 configure します。別名の root build directory は使用しません。

## 技術 catalog

[`feature_catalog.md`](feature_catalog.md) は、実装と runtime route だけを記録する project-facing の技術 showcase contract です。

- `VillienEditor Technology Showcase Registry` は、実 VILLIEN runtime で確認する項目を明示します。
- 各項目は `Scene`、`Environment`、`Rendering`、`World`、`VFX`、`Camera`、`Overworld`、`Tavern`、`Boss`、`Observed` のいずれかへ route されます。
- 安全な runtime state だけを checkbox にし、renderer foundation は control panel／Game View／observed status で確認します。
- `Standalone 3D Feature Lab Registry` は deprecated lab の互換性を保つ 42 項目です。

Editor の Inventory 件数は source inventory の更新に合わせて動的に変化します。過去の固定件数を completion contract として扱いません。Showcase 側も catalog の一意な ID、既知 route、重複なしを検証対象とします。

## 実装済み Editor workflow

- 可変 workspace：`Scene Objects`、live `Scene View`、`Inspector`、`Project`、`Systems`、`Console`。
- Scene View の viewport-local picking、camera navigation、Transform gizmo。
- Entity の作成、選択、複製、削除、Transform／Material／Mesh／Light 編集。
- Undo／Redo、JSON scene save／load、Play snapshot／restore。
- Environment、Rendering、World、VFX、Camera、Features panel。
- Weather／time、water／wet surface／puddle、forest、collision、particles、animation preview の runtime binding。
- PBR material、texture slot、POM、reflection receiver、ray-tracing visibility の Inspector。
- Shadow／CSM、SSAO、post process、IBL、SSR／Hybrid DXR の実 runtime control／status。
- Title、Overworld、Tavern、Boss の実 Game View route。
- `feature_catalog.md` と source inventory を使う Feature Coverage view。

### 主な操作

| Control | Action |
|---|---|
| `F1` | Game View から Editor へ戻る |
| `F5` | Editor scene snapshot preview を Play／Stop |
| `F6` | Grid Editor panel を開閉する |
| `F8` | Debug reflection mode を切り替える |
| `F9` | Raster shader を hot reload する |
| `F10` | Overworld placement JSON を reload する |
| `W`, `E`, `R` | Translate／Rotate／Scale gizmo |
| Right mouse + movement keys | Scene View camera を操作する |

## Feature route の方針

- `Scene`：Scene Objects／Inspector で実 object、material、asset を確認します。
- `Environment`：Weather、time、sky、lighting を Environment panel で操作します。
- `Rendering`：Shadow、SSAO、post process、IBL、reflection の実 pass／setting を確認します。
- `World`：Water、wet surface、puddle、forest、collision を World panel で確認します。
- `VFX`：Particle／animation preview と runtime emitter を確認します。
- `Camera`：Editor camera、game camera、navigation lock を確認します。
- `Overworld`、`Tavern`、`Boss`：対応する実 Game View を起動します。
- `Observed`：DirectX 12 foundation、resource state、import pipeline など、安全な一括 ON／OFF に適さない core path を status／実行結果で確認します。
- `Legacy`：deprecated standalone lab だけに存在する比較項目です。

## 検証範囲

Debug verification では、少なくとも次を別々に確認します。

- `VillienEditor` の build と実 frame loop。
- Title／Overworld／Tavern／Boss Game View の起動と `F1` return。
- SSR／Hybrid DXR の requested mode、active mode、fallback reason。
- D3D12 Debug Layer の error／corruption が 0 件であること。
- Feature catalog の ID uniqueness、route validity、runtime binding。
- Runtime settings の save／load round trip。

Startup smoke は Debug 実行確認のみを対象とし、performance benchmark、Release build、capture、package は対象外です。

## Deprecated standalone lab

次は過去の procedural comparison lab であり、正式な VILLIEN Editor ではありません。

```text
featuretools/CMakeLists.txt
featuretools/CMakePresets.json
featuretools/src/
featuretools/shaders/
featuretools/tests/
```

必要な場合だけ `featuretools/` を source directory として build します。次の command は `featuretools/` を current directory として実行します。

```powershell
cmake --preset vs2026
cmake --build --preset debug
ctest --preset debug
.\build\bin\Debug\FeatureTools.exe --feature-file .\feature_catalog.md --self-test
```

Standalone lab の簡易 PBR、single shadow map、SSAO、SSR は主ゲーム renderer と同等ではありません。DXR、IBL、CSM、実 asset／animation／gameplay の証拠として使用しません。これらは `VillienEditor.exe` の実 runtime route で確認します。

## 現在の制限

- Scene Objects は flat entity list で、parent-child authoring hierarchy は未実装です。
- Project panel は asset browser であり、汎用 asset database ではありません。
- F5 は snapshot preview／restore であり、完全分離された Play Mode ではありません。
- Prefab、generic component add/remove、audio authoring、dialogue、quest、inventory、gameplay save/load は未実装です。
- DXR は static opaque geometry を中心とした hybrid reflection proof です。Dynamic／skinned／alpha-cutout geometry と full material parity は未実装です。

未実装機能を fake preview や動かない checkbox で置き換えません。
