# DirectX 12 Portfolio Project #2 - Sumi-e Cozy Action RPG

A solo DirectX 12 game project built with C++20 and HLSL.

This project is a **cozy exploration RPG with a sumi-e boss action finale**. The player explores a compact ink-painting-inspired world, talks to villagers, then enters a boss arena for readable, telegraphed action combat.

The project reuses the DirectX 12 engine foundation developed in the previous portfolio project, but the game concept, camera, world structure, combat pacing, and art direction are being rebuilt for a different experience.

## Current Status

- **Phase**: M7 Feature Iteration / Week 1
- **Alpha deadline**: 2026-06-19
- **Current milestone goal**: complete all P0 features before alpha feature lock
- **Latest implemented task**: Overworld castle wall blockout and local-only asset policy

現在の Overworld は 60m x 60m の床を基準に、外周を procedural cube の城壁で囲んでいる。北側中央には boss entrance / castle door 用の門穴を残している。城壁本体、城垛、角塔には local `Assets/textures/Castle_wall/` の stone PBR texture を適用している。

## Game Concept

Working title: **Bokugen RPG** (temporary)

The game is designed around two connected moods:

- **Cozy exploration**: walk through a small sumi-e-style overworld, visit two village areas, and talk to NPCs.
- **Focused boss combat**: enter a boss arena and read ground telegraphs to dodge attacks.

The game structure combines a calm exploration layer with a focused boss-battle layer in a compact solo-developed scope.

## Planned Core Loop

1. Explore the overworld with a third-person camera.
2. Talk to villagers with the interaction key.
3. Learn the world context and progression hints.
4. Enter the boss arena.
5. Fight one boss with clear AoE telegraphs.
6. Reach a clear / result screen.

## P0 Feature Target

These are the alpha-critical features for the 2026-06-19 submission:

- Player character with Idle / Walk / Run animation
- Third-person follow camera
- Free-walk movement with collision
- Capsule vs AABB collision response
- One overworld scene with two village markers and a boss entrance
- Two NPCs with minimal dialogue triggers
- Boss arena transition
- One boss with at least three mechanics: Line, Circle, Fan
- Game flow: Title -> Overworld -> Boss -> Result

## Engine Features

The project currently includes or reuses the following DirectX 12 engine systems:

- Deferred rendering with G-buffer
- PBR material workflow
- Cascaded Shadow Maps
- Image-Based Lighting from HDRI skybox
- Post-processing: bloom, depth of field, motion blur, TAA, FXAA, SSAO, tonemapping
- Particle rendering
- Instanced mesh rendering
- glTF / GLB / VRM loading through tinygltf
- Skeletal animation and GPU skinning
- Dear ImGui debug UI
- Scene / entity system with editor tooling
- Material, lighting, shadow, and post-process editor controls
- Shader hot reload

## Requirements

- Windows 10/11
- Visual Studio 2022+ or Visual Studio 2026 Preview with MSVC
- Windows 10/11 SDK
- CMake 3.24+
- DirectX 12 capable GPU

## Build

From the project root:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Debug --target SoloAssignment -j
```

Run:

```powershell
.\build\bin\Debug\SoloAssignment.exe
```

Note: building the whole solution with no target may try to build external dependency sample targets. The main game target is `SoloAssignment`.

## Project Structure

```text
Solo_Assignment/
  Assets/                 Local-only runtime assets: models, animations, textures, HDRI
  shaders/                HLSL shaders loaded at runtime
  src/                    C++ source
    engine/               Scene, entity, editor, command systems
    game/                 New game-specific code
    gridgame/             Previous Grid Gauntlet-derived systems kept for reuse/reference
```

## Implemented Systems

- サードパーソン移動、カメラ、カプセル衝突判定を備えた Overworld
- Meteor、Laser、Sanctuary Seal を使用する二段階 Boss Battle
- 水鏡チャージとスマートフォン型パズルによる反撃フロー
- DirectX 12 Deferred Rendering、PBR、CSM、SSAO、SSR、Post Process
- Procedural Mesh、透明描画、Particle VFX、動的 Point Light
- Title、Overworld、BossArena、Clear、Failed、Restart を含むゲーム進行

## Asset Notes

`Assets/` はローカル専用で、Git 管理対象外。`.gitignore` で `Assets/` を除外している。

理由:

- モデル、texture、HDRI が大きくなりやすい。
- この制作は基本的に Chihiro のローカル環境で進める。
- GitHub には source、shader、build configuration、公開用 README のみを置き、runtime assets と内部開発資料はローカルで管理する。

新しく clone した環境では、`Assets/` を別途ローカルに配置する必要がある。現時点で code が参照している主な asset path:

- `Assets/scenes/overworld_placements.json`
- `Assets/textures/Floor_png/`
- `Assets/textures/Castle_wall/`
- `Assets/models/MyFirstChar.vrm`
- `Assets/models/animations/`
- `Assets/models/japanese_shrine/`
- `Assets/models/psx_japanese_house/`

## Reference Project Policy

`LearningDirectX12` is a reference project only. This repository should be modified directly, while the old project should not be changed unless explicitly requested.
