# VILLIEN — DirectX 12 個人制作作品

## 作品概要

『VILLIEN』は、水墨画を意識した和風世界の探索と、攻撃予兆を見極めて反撃するBoss Battleを組み合わせた3Dアクション作品です。DirectX 12によるレンダリング基盤からゲームロジック、シェーダー、UI、開発ツールまで、プログラム部分を個人で制作しました。

Boss Battleでは攻撃を回避しながら3つの「水鏡チャージ」を集め、スマートフォン型UIで記憶パズルを解くことでBossへ反撃します。単なる技術デモではなく、リアルタイムレンダリング技術をプレイヤーが理解しやすいゲーム体験へ統合することを目標にしました。

## 制作情報

| 項目 | 内容 |
|---|---|
| 制作期間 | 2026年4月～2026年7月（継続開発中） |
| 制作人数 | 1名 |
| 担当範囲 | C++／HLSL、レンダリング基盤、ゲームロジック、UI、ツール、統合・調整 |
| 開発環境 | Windows 11、C++20、DirectX 12、HLSL、CMake |
| 想定職種 | グラフィックスプログラマ／クライアントプログラマ |

3Dモデル、アニメーション、PBRテクスチャ、HDRIの一部には第三者素材を使用しています。詳細は `THIRD_PARTY_NOTICES_JP.md` に記載しています。

## 自作した主な技術

- Deferred Renderingと4 MRTのG-buffer
- PBR Lighting、Image-Based Lighting、HDRI環境光
- Cascaded Shadow MapsとPCF
- SSAOとBilateral Blur
- Screen-Space Reflectionによる水面反射
- Bloom、ACES Tonemapping、FXAA、TAA、Motion Blur、Depth of Field
- Billboard Particle Rendererと用途別Emitter
- glTF／GLB／VRM読み込み、Skeletal Animation、GPU Skinning
- Instanced Rendering、Terrain LOD、Shader Hot Reload
- Dear ImGuiを利用したScene／Material／Lighting／Post Process調整ツール

## ゲーム実装

- Title → Overworld → BossArena → Clear／Failedまでのゲームフロー
- Third-person movement、Capsule／AABB／Mesh collision
- Boss attack state machineとMeteor AoE／Line Rift／Knockback
- 攻撃予兆、Player HP、Boss HP、Phase 2、Clear rank
- 水鏡チャージ収集とスマートフォン型反撃UI
- 雨、霧、炎、電流、斬撃などのプロシージャルParticle VFX

## 最も工夫した点

当初のBoss VFXは発光meshを多用しており、Bloomが重なることで攻撃範囲と背景演出の区別がつきにくい状態でした。そこで、常設の赤い装飾や大きな発光meshを削除し、炎・電流・霧・Line Riftを用途別Particleへ置き換えました。さらに、赤色を危険予兆へ集中させ、反撃成功は青緑色で統一しました。

この改善により、描画機能を増やすだけでなく、**何が危険で、いつ反撃できたかをプレイヤーが一目で判断できる表現**へ整理しました。

## 操作方法

- `WASD`／方向キー：移動
- `Shift`：走る
- `Space`：水鏡UIを開く
- マウス左クリック：水鏡パズルを操作
- `Esc`：設定画面
- `R`：Boss Battleをリスタート

## 動作確認環境

- Windows 11 Home 64-bit
- AMD Ryzen 7 7800X3D
- NVIDIA GeForce RTX 5080
- RAM 31.1 GiB
- 1920 × 1080

平均FPS／最低FPSは、最終Release buildでBoss Battle全体を計測後に提出資料へ記載します。

## 起動方法

1. ZIPを英数字のみのパスへ展開してください。
2. `Villien.exe` を起動してください。
3. `Start` を選択するとOverworldから開始します。

Windows 10／11およびDirectX 12対応GPUが必要です。
