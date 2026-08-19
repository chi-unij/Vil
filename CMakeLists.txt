# ======================================
# CMakeLists.txt — Solo Assignment (DirectX 12)
# DirectX 12 portfolio game project
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

# ---- 共通ソース ----
set(VILLIEN_SOURCES
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
    src/HybridReflectionRenderer.h
    src/HybridReflectionRenderer.cpp
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
    src/game/CollisionSystem.h
    src/game/CollisionSystem.cpp
    src/game/BossArenaScene.h
    src/game/BossArenaScene.cpp
    src/game/TavernScene.h
    src/game/TavernScene.cpp
    src/game/OverworldScene.h
    src/game/OverworldScene.cpp
    src/game/WorldRainParticles.h
    src/game/WorldRainParticles.cpp
    src/game/PlayerAnimationPreview.h
    src/game/PlayerAnimationPreview.cpp
    src/game/TitleScreen.h
    src/game/TitleScreen.cpp
)

# ---- 実行ターゲット ----
add_executable(SoloAssignment WIN32 ${VILLIEN_SOURCES})
add_executable(VillienEditor WIN32 ${VILLIEN_SOURCES})

# ---- コンパイル定義 ----
foreach(VILLIEN_TARGET IN ITEMS SoloAssignment VillienEditor)
    target_compile_definitions(${VILLIEN_TARGET} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
endforeach()
target_compile_definitions(VillienEditor PRIVATE VILLIEN_EDITOR_BUILD=1)

# ---- DXR / Shader Model 6.5 compile proof ----
# Raster shaders keep the existing D3DCompile hot-reload path. Only the
# isolated Inline RayQuery shader is compiled with Windows SDK DXC.
find_program(VILLIEN_DXC_EXECUTABLE
    NAMES dxc.exe dxc
    HINTS
        "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
)

set(VILLIEN_GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
set(VILLIEN_HYBRID_REFLECTION_DXIL
    "${VILLIEN_GENERATED_SHADER_DIR}/hybrid_reflection.dxil")

if (VILLIEN_DXC_EXECUTABLE)
    add_custom_command(
        OUTPUT "${VILLIEN_HYBRID_REFLECTION_DXIL}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VILLIEN_GENERATED_SHADER_DIR}"
        COMMAND "${VILLIEN_DXC_EXECUTABLE}"
                -T cs_6_5
                -E CSMain
                -Fo "${VILLIEN_HYBRID_REFLECTION_DXIL}"
                "${CMAKE_SOURCE_DIR}/shaders/hybrid_reflection.hlsl"
        DEPENDS "${CMAKE_SOURCE_DIR}/shaders/hybrid_reflection.hlsl"
        COMMENT "Compiling Hybrid DXR reflection shader (cs_6_5)"
        VERBATIM
    )
    add_custom_target(VillienDxrShaders DEPENDS "${VILLIEN_HYBRID_REFLECTION_DXIL}")
    add_dependencies(SoloAssignment VillienDxrShaders)
    add_dependencies(VillienEditor VillienDxrShaders)
    target_compile_definitions(SoloAssignment PRIVATE VILLIEN_DXR_SHADER_BUILD=1)
    target_compile_definitions(VillienEditor PRIVATE VILLIEN_DXR_SHADER_BUILD=1)
else()
    message(WARNING "Windows SDK dxc.exe was not found; Hybrid DXR will use SSR fallback")
    target_compile_definitions(SoloAssignment PRIVATE VILLIEN_DXR_SHADER_BUILD=0)
    target_compile_definitions(VillienEditor PRIVATE VILLIEN_DXR_SHADER_BUILD=0)
endif()

# ---- MSVC 警告レベル ----
if (MSVC)
    foreach(VILLIEN_TARGET IN ITEMS SoloAssignment VillienEditor)
        target_compile_options(${VILLIEN_TARGET} PRIVATE /W4 /permissive- /utf-8)
    endforeach()
endif()

# ---- リンク ----
foreach(VILLIEN_TARGET IN ITEMS SoloAssignment VillienEditor)
    target_link_libraries(${VILLIEN_TARGET} PRIVATE
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
endforeach()

# ---- インクルードパス ----
foreach(VILLIEN_TARGET IN ITEMS SoloAssignment VillienEditor)
    target_include_directories(${VILLIEN_TARGET} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${tinyexr_SOURCE_DIR}
        ${tinyexr_SOURCE_DIR}/deps/miniz
        ${tinygltf_SOURCE_DIR}
    )
endforeach()

# ---- 出力先 ----
set_target_properties(SoloAssignment VillienEditor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---- ビルド後に shaders/ と Assets/ を出力ディレクトリへコピー ----
add_custom_command(TARGET SoloAssignment POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SoloAssignment>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:SoloAssignment>/shaders"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SoloAssignment>/Assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Assets" "$<TARGET_FILE_DIR:SoloAssignment>/Assets"
)

if (VILLIEN_DXC_EXECUTABLE)
    add_custom_command(TARGET SoloAssignment POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${VILLIEN_HYBRID_REFLECTION_DXIL}"
                "$<TARGET_FILE_DIR:SoloAssignment>/shaders/hybrid_reflection.dxil"
    )
endif()

# Editor はルートの Assets を直接使用し、巨大アセットの複製を避ける。
add_custom_command(TARGET VillienEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:VillienEditor>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:VillienEditor>/shaders"
)

if (VILLIEN_DXC_EXECUTABLE)
    add_custom_command(TARGET VillienEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${VILLIEN_HYBRID_REFLECTION_DXIL}"
                "$<TARGET_FILE_DIR:VillienEditor>/shaders/hybrid_reflection.dxil"
    )
endif()
