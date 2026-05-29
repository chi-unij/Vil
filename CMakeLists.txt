# ======================================
# CMakeLists.txt — Solo Assignment (DirectX 12)
# 個人制作（HAL GM23 + 2027 developmentポートフォリオ第二弾）
#
# 注意：Grid Gauntlet (LearningDirectX12) のビルド設定を参考にしつつ、
#       本プロジェクト用に新規記述。コピペ禁止（CLAUDE.md ルール）。
# ======================================

cmake_minimum_required(VERSION 3.24)
project(SoloAssignment LANGUAGES CXX)

# ---- C++ standard ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- 外部依存（FetchContent でビルド時に取得）----
include(FetchContent)

# Dear ImGui — GUI 描画
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.5
)
FetchContent_MakeAvailable(imgui)

# tinyexr — HDRI (.exr) ロード用
FetchContent_Declare(
    tinyexr
    GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
    GIT_TAG v1.0.12
)
set(TINYEXR_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(TINYEXR_USE_MINIZ ON CACHE BOOL "" FORCE)
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(tinyexr)
set(CMAKE_WARN_DEPRECATED ON CACHE BOOL "" FORCE)

# tinygltf — glTF モデルロード
FetchContent_Declare(
    tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG v2.9.3
)
FetchContent_MakeAvailable(tinygltf)

# nlohmann/json — シリアライズ
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# ImGuizmo — 3D ギズモ操作
FetchContent_Declare(
    imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG master
)
FetchContent_MakeAvailable(imguizmo)

# ---- ImGui 統合ライブラリ（バックエンド + ImGuizmo を同梱）----
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
    ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imguizmo_SOURCE_DIR}/src
)
if (MSVC)
    target_compile_options(imgui PRIVATE /W0)
endif()

# ---- 本体ターゲット ----
add_executable(SoloAssignment WIN32
    # エントリポイント
    src/main.cpp

    # プラットフォーム / 入力
    src/Win32Window.h
    src/Win32Window.cpp
    src/Input.h

    # DirectX 12 基盤
    src/DxContext.h
    src/DxContext.cpp
    src/DxUtil.h
    src/DxUtil.cpp
    src/ShaderCompiler.h

    # カメラ
    src/Camera.h
    src/Camera.cpp

    # ImGui レイヤー
    src/ImGuiLayer.h
    src/ImGuiLayer.cpp

    # レンダラーモジュール群
    src/MeshRenderer.h
    src/MeshRenderer.cpp
    src/SkyRenderer.h
    src/SkyRenderer.cpp
    src/GridRenderer.h
    src/GridRenderer.cpp
    src/ShadowMap.h
    src/ShadowMap.cpp
    src/Lighting.h
    src/Lighting.cpp
    src/IBLGenerator.h
    src/IBLGenerator.cpp
    src/HdriLoader.h
    src/HdriLoader.cpp
    src/SSAORenderer.h
    src/SSAORenderer.cpp
    src/PostProcess.h
    src/PostProcess.cpp
    src/TerrainLOD.h
    src/TerrainLOD.cpp
    src/ProceduralMesh.h
    src/ProceduralMesh.cpp

    # メッシュ / アニメーション
    src/GltfLoader.h
    src/GltfLoader.cpp
    src/AnimationPlayer.h
    src/AnimationPlayer.cpp

    # パーティクル
    src/particle.h
    src/particle.cpp
    src/particle_test.h
    src/particle_test.cpp
    src/ParticleRenderer.h
    src/ParticleRenderer.cpp

    # レンダーパス
    src/RenderPass.h
    src/RenderPasses.h
    src/RenderPasses.cpp

    # エンジン層（Scene / Entity / Editor）
    src/engine/Entity.h
    src/engine/Entity.cpp
    src/engine/Scene.h
    src/engine/Scene.cpp
    src/engine/SceneEditor.h
    src/engine/SceneEditor.cpp
    src/engine/CommandHistory.h
    src/engine/CommandHistory.cpp
    src/engine/Commands.h
    src/engine/GizmoController.h
    src/engine/GizmoController.cpp

    # Grid 系（SceneEditor 依存。新ゲーム方針確定後に整理予定）
    src/gridgame/StageData.h
    src/gridgame/StageData.cpp
    src/gridgame/StageSerializer.h
    src/gridgame/StageSerializer.cpp
    src/gridgame/GridMap.h
    src/gridgame/GridMap.cpp
    src/gridgame/GridMaterials.h
    src/gridgame/GridParticles.h
    src/gridgame/GridEditorCommands.h

    # 新ゲームコード配置先（空。実装はここから始める）
    src/game/PlayerAnimationPreview.h
    src/game/PlayerAnimationPreview.cpp
)

# ---- コンパイル定義 ----
target_compile_definitions(SoloAssignment PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

# ---- MSVC 警告レベル ----
if (MSVC)
    target_compile_options(SoloAssignment PRIVATE /W4 /permissive- /utf-8)
endif()

# ---- リンク ----
target_link_libraries(SoloAssignment PRIVATE
    d3d12
    dxgi
    d3dcompiler
    dbghelp
    xinput
    imgui
    tinyexr
    tinygltf
    nlohmann_json::nlohmann_json
)

# ---- インクルードパス ----
target_include_directories(SoloAssignment PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${tinyexr_SOURCE_DIR}
    ${tinyexr_SOURCE_DIR}/deps/miniz
    ${tinygltf_SOURCE_DIR}
)

# ---- 出力先 ----
set_target_properties(SoloAssignment PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---- ビルド後に shaders/ と Assets/ を出力ディレクトリへコピー ----
add_custom_command(TARGET SoloAssignment POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SoloAssignment>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:SoloAssignment>/shaders"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SoloAssignment>/Assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Assets" "$<TARGET_FILE_DIR:SoloAssignment>/Assets"
)
