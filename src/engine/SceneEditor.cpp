// ======================================
// File: SceneEditor.cpp
// Purpose: ImGui editor panels for entity management, property inspection,
//          scene save/load, transform gizmo, undo/redo, duplication.
//          Milestone 4 Phase 0+1.
// ======================================

#include "SceneEditor.h"
#include "../Camera.h"
#include "Commands.h"
#include "../DxContext.h"
#include "../ProceduralMesh.h"
#include "../RenderPass.h"
#include "../gridgame/GridEditorCommands.h"
#include "../gridgame/GridMaterials.h"
#include "../gridgame/StageSerializer.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <windows.h>
#include <commdlg.h>

// Names matching MeshSourceType enum order.
static const char *kMeshSourceTypeNames[] = {
    "Cube", "Plane", "Cylinder", "Cone", "Sphere", "glTF File"};

// ---- Material presets (Phase 2) ----

struct MaterialPreset {
  const char *name;
  DirectX::XMFLOAT4 baseColorFactor;
  float metallicFactor;
  float roughnessFactor;
  DirectX::XMFLOAT3 emissiveFactor;
};

static const MaterialPreset kPresets[] = {
    {"Default",       {1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.8f, {0.0f, 0.0f, 0.0f}},
    {"Metal",         {0.8f, 0.8f, 0.85f, 1.0f}, 1.0f, 0.3f, {0.0f, 0.0f, 0.0f}},
    {"Plastic",       {1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.5f, {0.0f, 0.0f, 0.0f}},
    {"Wood",          {0.55f, 0.35f, 0.15f, 1.0f}, 0.0f, 0.7f, {0.0f, 0.0f, 0.0f}},
    {"Emissive Glow", {0.0f, 0.0f, 0.0f, 1.0f}, 0.0f, 0.5f, {1.0f, 0.5f, 0.1f}},
    {"Mirror",        {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f, 0.05f, {0.0f, 0.0f, 0.0f}},
    {"Rough Stone",   {0.5f, 0.5f, 0.45f, 1.0f}, 0.0f, 0.95f, {0.0f, 0.0f, 0.0f}},
};
static constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

// Texture slot names for the editor UI.
static const char *kTexSlotNames[] = {
    "BaseColor", "Normal", "MetalRough", "AO", "Emissive", "Height"};

static bool OpenTextureFileDialog(char *outPath, size_t maxLen) {
  OPENFILENAMEA ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFilter =
      "Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files\0*.*\0";
  ofn.lpstrFile = outPath;
  ofn.nMaxFile = static_cast<DWORD>(maxLen);
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
  outPath[0] = '\0';
  return GetOpenFileNameA(&ofn) != 0;
}

void SceneEditor::DrawUI(Scene &scene, DxContext &dx,
                          const DirectX::XMMATRIX &view,
                          const DirectX::XMMATRIX &proj,
                          bool *iblEnabled,
                          StageData *editStage,
                          Camera *cam) {
  // Process hotkeys first (undo/redo/duplicate/gizmo mode).
  ProcessHotkeys(scene, dx);

  // Gizmo update (before panels so gizmo can consume mouse).
  m_gizmo.Update(scene, m_selectedEntity, view, proj, m_history);

  // Draw panels.
  DrawMenuBar(scene, dx);
  DrawEntityList(scene, dx);
  DrawInspector(scene, dx);
  DrawLightingPanel(scene, iblEnabled);
  DrawShadowPanel(scene, dx);
  DrawPostProcessPanel(scene, dx);
  if (cam)
    DrawCameraPanel(scene, *cam);
  DrawAssetBrowser(scene, dx);
  if (m_gridEditorOpen && editStage)
    DrawGridEditorPanel(*editStage);
  m_gizmo.DrawToolbar();
}

// ---- Hotkeys ----

void SceneEditor::ProcessHotkeys(Scene &scene, DxContext &dx) {
  // Gizmo mode hotkeys (W/E/R) — only when ImGui doesn't want keyboard
  // and right mouse button is NOT held (RMB+WASD is camera movement).
  if (!ImGui::GetIO().WantCaptureKeyboard && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
    m_gizmo.ProcessHotkeys();
  }

  // Ctrl shortcuts always active in editor.
  bool ctrl = ImGui::GetIO().KeyCtrl;

  if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
    if (m_gridEditorOpen && m_gridHistory.CanUndo())
      m_gridHistory.Undo();
    else
      m_history.Undo();
  }
  if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
    if (m_gridEditorOpen && m_gridHistory.CanRedo())
      m_gridHistory.Redo();
    else
      m_history.Redo();
  }

  // Ctrl+D — duplicate selected entity.
  if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) {
    if (m_selectedEntity != kInvalidEntityId) {
      auto cmd =
          std::make_unique<DuplicateEntityCommand>(scene, dx, m_selectedEntity);
      m_history.Execute(std::move(cmd));
      // Select the duplicate: it's the last entity in the scene.
      auto &entities = scene.Entities();
      if (!entities.empty())
        m_selectedEntity = entities.back().id;
    }
  }

  // Delete key — delete selected entity.
  if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) &&
      !ImGui::GetIO().WantCaptureKeyboard) {
    if (m_selectedEntity != kInvalidEntityId) {
      auto cmd = std::make_unique<DeleteEntityCommand>(scene, dx,
                                                        m_selectedEntity);
      m_history.Execute(std::move(cmd));
      m_selectedEntity = kInvalidEntityId;
    }
  }
}

// ---- Menu Bar ----

void SceneEditor::DrawMenuBar(Scene &scene, DxContext &dx) {
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("Scene")) {
      ImGui::InputText("Path", m_scenePath, sizeof(m_scenePath));
      if (ImGui::MenuItem("Save"))
        scene.SaveToFile(m_scenePath);
      if (ImGui::MenuItem("Load")) {
        scene.LoadFromFile(m_scenePath, dx);
        m_history.Clear();
        m_selectedEntity = kInvalidEntityId;
      }
      if (ImGui::MenuItem("Clear")) {
        scene.Clear();
        m_history.Clear();
        m_selectedEntity = kInvalidEntityId;
      }
      ImGui::EndMenu();
    }

    // Undo/redo status in menu bar.
    if (ImGui::BeginMenu("Edit")) {
      const char *undoName = m_history.UndoName();
      const char *redoName = m_history.RedoName();

      char undoLabel[128] = "Undo";
      char redoLabel[128] = "Redo";
      if (undoName)
        snprintf(undoLabel, sizeof(undoLabel), "Undo %s", undoName);
      if (redoName)
        snprintf(redoLabel, sizeof(redoLabel), "Redo %s", redoName);

      if (ImGui::MenuItem(undoLabel, "Ctrl+Z", false, m_history.CanUndo()))
        m_history.Undo();
      if (ImGui::MenuItem(redoLabel, "Ctrl+Y", false, m_history.CanRedo()))
        m_history.Redo();

      ImGui::Separator();
      if (ImGui::MenuItem("Duplicate", "Ctrl+D", false,
                           m_selectedEntity != kInvalidEntityId)) {
        auto cmd = std::make_unique<DuplicateEntityCommand>(scene, dx,
                                                             m_selectedEntity);
        m_history.Execute(std::move(cmd));
        auto &entities = scene.Entities();
        if (!entities.empty())
          m_selectedEntity = entities.back().id;
      }
      if (ImGui::MenuItem("Delete", "Del", false,
                           m_selectedEntity != kInvalidEntityId)) {
        auto cmd = std::make_unique<DeleteEntityCommand>(scene, dx,
                                                          m_selectedEntity);
        m_history.Execute(std::move(cmd));
        m_selectedEntity = kInvalidEntityId;
      }
      ImGui::EndMenu();
    }

    // Play Scene button (Phase 8).
    ImGui::Separator();
    if (ImGui::MenuItem("Play Scene", "F5"))
      m_scenePlayRequested = true;

    ImGui::EndMainMenuBar();
  }
}

// ---- Entity List Panel ----

void SceneEditor::DrawEntityList(Scene &scene, DxContext &dx) {
  ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(220, 400), ImGuiCond_FirstUseEver);
  ImGui::Begin("Entities");

  // Helper lambda to create entity via command.
  auto createEntity = [&](const char *name, MeshSourceType type) {
    Entity e;
    e.id = scene.AllocateId();
    e.name = name;
    MeshComponent mc;
    mc.sourceType = type;
    e.mesh = mc;
    auto cmd = std::make_unique<CreateEntityCommand>(scene, dx, std::move(e));
    m_history.Execute(std::move(cmd));
    m_selectedEntity = scene.Entities().back().id;
  };

  auto createLightEntity = [&](const char *name, bool isSpot) {
    Entity e;
    e.id = scene.AllocateId();
    e.name = name;
    if (isSpot) {
      e.spotLight = SpotLightComponent{};
      e.transform.position = {0.0f, 5.0f, 0.0f};
    } else {
      e.pointLight = PointLightComponent{};
      e.transform.position = {0.0f, 3.0f, 0.0f};
    }
    auto cmd = std::make_unique<CreateEntityCommand>(scene, dx, std::move(e));
    m_history.Execute(std::move(cmd));
    m_selectedEntity = scene.Entities().back().id;
  };

  // Create entity buttons.
  if (ImGui::Button("+ Empty")) {
    Entity e;
    e.id = scene.AllocateId();
    e.name = "Empty";
    auto cmd = std::make_unique<CreateEntityCommand>(scene, dx, std::move(e));
    m_history.Execute(std::move(cmd));
    m_selectedEntity = scene.Entities().back().id;
  }
  ImGui::SameLine();
  if (ImGui::Button("+ Cube"))
    createEntity("Cube", MeshSourceType::ProceduralCube);
  ImGui::SameLine();
  if (ImGui::Button("+ Plane"))
    createEntity("Plane", MeshSourceType::ProceduralPlane);

  if (ImGui::Button("+ Sphere"))
    createEntity("Sphere", MeshSourceType::ProceduralSphere);
  ImGui::SameLine();
  if (ImGui::Button("+ Cylinder"))
    createEntity("Cylinder", MeshSourceType::ProceduralCylinder);
  ImGui::SameLine();
  if (ImGui::Button("+ Cone"))
    createEntity("Cone", MeshSourceType::ProceduralCone);

  if (ImGui::Button("+ Point Light"))
    createLightEntity("Point Light", false);
  ImGui::SameLine();
  if (ImGui::Button("+ Spot Light"))
    createLightEntity("Spot Light", true);

  ImGui::Separator();

  // Entity list.
  auto &entities = scene.Entities();
  for (auto &e : entities) {
    bool selected = (e.id == m_selectedEntity);
    char label[128];
    snprintf(label, sizeof(label), "%s [%llu]", e.name.c_str(),
             static_cast<unsigned long long>(e.id));
    if (ImGui::Selectable(label, selected))
      m_selectedEntity = e.id;
  }

  ImGui::Separator();

  // Delete selected.
  if (m_selectedEntity != kInvalidEntityId) {
    if (ImGui::Button("Delete Selected")) {
      auto cmd = std::make_unique<DeleteEntityCommand>(scene, dx,
                                                        m_selectedEntity);
      m_history.Execute(std::move(cmd));
      m_selectedEntity = kInvalidEntityId;
    }
  }

  ImGui::End();
}

// ---- Inspector Panel ----

void SceneEditor::DrawInspector(Scene &scene, DxContext &dx) {
  ImGui::SetNextWindowPos(ImVec2(240, 30), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(320, 500), ImGuiCond_FirstUseEver);
  ImGui::Begin("Inspector");

  Entity *e = scene.FindEntity(m_selectedEntity);
  if (!e) {
    ImGui::TextDisabled("No entity selected.");
    ImGui::End();
    return;
  }

  // Name.
  char nameBuf[128];
  snprintf(nameBuf, sizeof(nameBuf), "%s", e->name.c_str());
  if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
    e->name = nameBuf;

  ImGui::Checkbox("Active", &e->active);

  // Transform (with undo coalescing for DragFloat3).
  if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::DragFloat3("Position", &e->transform.position.x, 0.1f);
    if (ImGui::IsItemActivated() && !m_transformDragActive) {
      m_transformDragActive = true;
      m_transformDragStart = e->transform;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_transformDragActive) {
      auto cmd = std::make_unique<TransformCommand>(
          scene, m_selectedEntity, m_transformDragStart, e->transform);
      m_history.PushWithoutExecute(std::move(cmd));
      m_transformDragActive = false;
    }

    ImGui::DragFloat3("Rotation", &e->transform.rotation.x, 1.0f);
    if (ImGui::IsItemActivated() && !m_transformDragActive) {
      m_transformDragActive = true;
      m_transformDragStart = e->transform;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_transformDragActive) {
      auto cmd = std::make_unique<TransformCommand>(
          scene, m_selectedEntity, m_transformDragStart, e->transform);
      m_history.PushWithoutExecute(std::move(cmd));
      m_transformDragActive = false;
    }

    ImGui::DragFloat3("Scale", &e->transform.scale.x, 0.05f, 0.01f, 100.0f);
    if (ImGui::IsItemActivated() && !m_transformDragActive) {
      m_transformDragActive = true;
      m_transformDragStart = e->transform;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_transformDragActive) {
      auto cmd = std::make_unique<TransformCommand>(
          scene, m_selectedEntity, m_transformDragStart, e->transform);
      m_history.PushWithoutExecute(std::move(cmd));
      m_transformDragActive = false;
    }
  }

  // Mesh component.
  if (e->mesh.has_value()) {
    if (ImGui::CollapsingHeader("Mesh Component",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      auto &mc = e->mesh.value();

      // Drop target: accept mesh files dragged from Asset Browser.
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload =
                ImGui::AcceptDragDropPayload("ASSET_PATH")) {
          std::string droppedPath(static_cast<const char *>(payload->Data));
          std::string ext;
          auto dotPos = droppedPath.rfind('.');
          if (dotPos != std::string::npos)
            ext = droppedPath.substr(dotPos);
          for (auto &c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          if (ext == ".gltf" || ext == ".glb") {
            Material oldMat = mc.material;
            auto oldPaths = mc.texturePaths;
            mc.sourceType = MeshSourceType::GltfFile;
            mc.gltfPath = droppedPath;
            mc.meshId = UINT32_MAX;
            m_history.Execute(std::make_unique<MaterialCommand>(
                scene, dx, e->id, oldMat, mc.material, oldPaths,
                mc.texturePaths));
          }
        }
        ImGui::EndDragDropTarget();
      }

      int srcType = static_cast<int>(mc.sourceType);
      bool meshChanged = false;

      if (ImGui::Combo("Mesh Type", &srcType, kMeshSourceTypeNames,
                        IM_ARRAYSIZE(kMeshSourceTypeNames))) {
        mc.sourceType = static_cast<MeshSourceType>(srcType);
        meshChanged = true;
      }

      // Show relevant parameters based on source type.
      switch (mc.sourceType) {
      case MeshSourceType::ProceduralCube: {
        float sz = e->transform.scale.x;
        if (ImGui::DragFloat("Size", &sz, 0.05f, 0.1f, 50.0f)) {
          e->transform.scale = {sz, sz, sz};
        }
      } break;
      case MeshSourceType::ProceduralPlane:
        meshChanged |=
            ImGui::DragFloat("Width", &mc.width, 0.05f, 0.1f, 100.0f);
        meshChanged |=
            ImGui::DragFloat("Depth", &mc.height, 0.05f, 0.1f, 100.0f);
        break;
      case MeshSourceType::ProceduralCylinder:
      case MeshSourceType::ProceduralCone:
        meshChanged |=
            ImGui::DragFloat("Radius", &mc.width, 0.05f, 0.05f, 20.0f);
        meshChanged |=
            ImGui::DragFloat("Height", &mc.height, 0.05f, 0.1f, 50.0f);
        {
          int seg = static_cast<int>(mc.segments);
          if (ImGui::DragInt("Segments", &seg, 1, 3, 64)) {
            mc.segments = static_cast<uint32_t>(seg);
            meshChanged = true;
          }
        }
        break;
      case MeshSourceType::ProceduralSphere:
        meshChanged |=
            ImGui::DragFloat("Radius", &mc.size, 0.05f, 0.05f, 20.0f);
        {
          int r = static_cast<int>(mc.rings);
          int s = static_cast<int>(mc.segments);
          meshChanged |= ImGui::DragInt("Rings", &r, 1, 3, 64);
          meshChanged |= ImGui::DragInt("Segments", &s, 1, 3, 64);
          mc.rings = static_cast<uint32_t>(r);
          mc.segments = static_cast<uint32_t>(s);
        }
        break;
      case MeshSourceType::GltfFile: {
        char pathBuf[256];
        snprintf(pathBuf, sizeof(pathBuf), "%s", mc.gltfPath.c_str());
        if (ImGui::InputText("glTF Path", pathBuf, sizeof(pathBuf)))
          mc.gltfPath = pathBuf;
        if (ImGui::Button("Load glTF"))
          meshChanged = true;
      } break;
      }

      // ---- Material properties (Phase 2 — Material & Texture Editor) ----
      ImGui::Separator();
      if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {

        auto &mat = mc.material;
        bool matChanged = false;

        // Track drag-start for undo coalescing.
        auto CheckDragStart = [&]() {
          if (ImGui::IsItemActivated() && !m_materialDragActive) {
            m_materialDragActive = true;
            m_materialDragStart = mat;
            m_materialPathsStart = mc.texturePaths;
          }
        };
        auto CheckDragEnd = [&]() {
          if (ImGui::IsItemDeactivatedAfterEdit() && m_materialDragActive) {
            m_materialDragActive = false;
            m_history.Execute(std::make_unique<MaterialCommand>(
                scene, dx, e->id, m_materialDragStart, mat,
                m_materialPathsStart, mc.texturePaths));
          }
        };

        // Material preset combo.
        if (ImGui::BeginCombo("Preset", "Select...")) {
          for (int p = 0; p < kPresetCount; ++p) {
            if (ImGui::Selectable(kPresets[p].name)) {
              Material oldMat = mat;
              auto oldPaths = mc.texturePaths;
              mat.baseColorFactor = kPresets[p].baseColorFactor;
              mat.metallicFactor = kPresets[p].metallicFactor;
              mat.roughnessFactor = kPresets[p].roughnessFactor;
              mat.emissiveFactor = kPresets[p].emissiveFactor;
              m_history.Execute(std::make_unique<MaterialCommand>(
                  scene, dx, e->id, oldMat, mat, oldPaths, mc.texturePaths));
              matChanged = true;
            }
          }
          ImGui::EndCombo();
        }

        // PBR scalars.
        matChanged |= ImGui::ColorEdit4("Base Color", &mat.baseColorFactor.x);
        CheckDragStart(); CheckDragEnd();
        matChanged |= ImGui::SliderFloat("Metallic", &mat.metallicFactor, 0, 1);
        CheckDragStart(); CheckDragEnd();
        matChanged |= ImGui::SliderFloat("Roughness", &mat.roughnessFactor, 0, 1);
        CheckDragStart(); CheckDragEnd();
        matChanged |= ImGui::ColorEdit3("Emissive", &mat.emissiveFactor.x);
        CheckDragStart(); CheckDragEnd();

        // UV tiling/offset.
        ImGui::Separator();
        ImGui::Text("UV Transform");
        matChanged |= ImGui::DragFloat2("UV Tiling", &mat.uvTiling.x, 0.05f, 0.01f, 100.0f);
        CheckDragStart(); CheckDragEnd();
        matChanged |= ImGui::DragFloat2("UV Offset", &mat.uvOffset.x, 0.01f, -10.0f, 10.0f);
        CheckDragStart(); CheckDragEnd();

        // POM controls.
        ImGui::Separator();
        ImGui::Text("Parallax Occlusion Mapping");
        if (ImGui::Checkbox("Enable POM", &mat.pomEnabled))
          matChanged = true;
        if (mat.pomEnabled) {
          matChanged |= ImGui::DragFloat("Height Scale", &mat.heightScale, 0.001f, 0.001f, 0.1f);
          CheckDragStart(); CheckDragEnd();
          matChanged |= ImGui::DragFloat("Min Layers", &mat.pomMinLayers, 1.0f, 1.0f, 64.0f);
          CheckDragStart(); CheckDragEnd();
          matChanged |= ImGui::DragFloat("Max Layers", &mat.pomMaxLayers, 1.0f, 1.0f, 128.0f);
          CheckDragStart(); CheckDragEnd();
        }

        // Texture slots.
        ImGui::Separator();
        ImGui::Text("Textures");
        const bool *hasFlags[] = {&mat.hasBaseColor, &mat.hasNormal,
                                  &mat.hasMetalRough, &mat.hasAO,
                                  &mat.hasEmissive, &mat.hasHeight};
        for (int i = 0; i < 6; ++i) {
          ImGui::PushID(i);

          // Thumbnail (only if mesh has valid GPU resources).
          if (mc.meshId != UINT32_MAX) {
            D3D12_GPU_DESCRIPTOR_HANDLE thumbGpu =
                dx.GetTextureImGuiSrv(mc.meshId, i);
            if (thumbGpu.ptr != 0) {
              ImGui::Image(static_cast<ImTextureID>(thumbGpu.ptr),
                           ImVec2(32, 32));
              ImGui::SameLine();
            }
          }

          // Status + slot name.
          const char *status = *hasFlags[i] ? "[Loaded]" : "[Default]";
          ImGui::Text("%s %s", kTexSlotNames[i], status);
          ImGui::SameLine();

          // Load button.
          char loadLabel[32];
          snprintf(loadLabel, sizeof(loadLabel), "Load##%d", i);
          if (ImGui::SmallButton(loadLabel)) {
            char filePath[512];
            if (OpenTextureFileDialog(filePath, sizeof(filePath))) {
              Material oldMat = mat;
              auto oldPaths = mc.texturePaths;
              mc.texturePaths[i] = filePath;
              m_history.Execute(std::make_unique<MaterialCommand>(
                  scene, dx, e->id, oldMat, mat, oldPaths, mc.texturePaths));
              matChanged = true;
            }
          }
          ImGui::SameLine();

          // Clear button (only if a texture is loaded for this slot).
          if (*hasFlags[i] || !mc.texturePaths[i].empty()) {
            char clearLabel[32];
            snprintf(clearLabel, sizeof(clearLabel), "Clear##%d", i);
            if (ImGui::SmallButton(clearLabel)) {
              Material oldMat = mat;
              auto oldPaths = mc.texturePaths;
              mc.texturePaths[i].clear();
              m_history.Execute(std::make_unique<MaterialCommand>(
                  scene, dx, e->id, oldMat, mat, oldPaths, mc.texturePaths));
              matChanged = true;
            }
          }

          // Drop target: accept texture files from Asset Browser.
          // Use an invisible button as drop area for this slot row.
          {
            char dropLabel[64];
            snprintf(dropLabel, sizeof(dropLabel), "##TexDrop%d", i);
            ImGui::SameLine();
            ImGui::SmallButton(dropLabel);
            if (ImGui::BeginDragDropTarget()) {
              if (const ImGuiPayload *payload =
                      ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                std::string droppedPath(
                    static_cast<const char *>(payload->Data));
                std::string ext2;
                auto dotPos2 = droppedPath.rfind('.');
                if (dotPos2 != std::string::npos)
                  ext2 = droppedPath.substr(dotPos2);
                for (auto &c : ext2)
                  c = static_cast<char>(
                      std::tolower(static_cast<unsigned char>(c)));
                if (ext2 == ".png" || ext2 == ".jpg" || ext2 == ".jpeg" ||
                    ext2 == ".bmp" || ext2 == ".tga") {
                  Material oldMat = mat;
                  auto oldPaths = mc.texturePaths;
                  mc.texturePaths[i] = droppedPath;
                  m_history.Execute(std::make_unique<MaterialCommand>(
                      scene, dx, e->id, oldMat, mat, oldPaths,
                      mc.texturePaths));
                  matChanged = true;
                }
              }
              ImGui::EndDragDropTarget();
            }
          }

          ImGui::PopID();
        }

        if (matChanged) {
          scene.UpdateEntityMaterial(dx, *e);
        }
      } // end Material collapsing header

      // Mesh param changes (size, segments, glTF load, etc.) also need GPU update.
      if (meshChanged) {
        scene.CreateEntityMeshGpu(dx, *e);
      }

      if (ImGui::Button("Remove Mesh Component"))
        e->mesh.reset();
    }
  } else {
    if (ImGui::Button("Add Mesh Component")) {
      e->mesh = MeshComponent{};
      scene.CreateEntityMeshGpu(dx, *e);
    }
  }

  // Point light component.
  if (e->pointLight.has_value()) {
    if (ImGui::CollapsingHeader("Point Light",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      auto &pl = e->pointLight.value();

      auto CheckPLDragStart = [&]() {
        if (ImGui::IsItemActivated() && !m_plDragActive) {
          m_plDragActive = true;
          m_plDragStart = pl;
        }
      };
      auto CheckPLDragEnd = [&]() {
        if (ImGui::IsItemDeactivatedAfterEdit() && m_plDragActive) {
          auto cmd = std::make_unique<PointLightCommand>(
              scene, e->id, m_plDragStart, pl);
          m_history.PushWithoutExecute(std::move(cmd));
          m_plDragActive = false;
        }
      };

      ImGui::Checkbox("Enabled##PL", &pl.enabled);
      ImGui::ColorEdit3("Color##PL", &pl.color.x);
      CheckPLDragStart();
      CheckPLDragEnd();
      ImGui::DragFloat("Intensity##PL", &pl.intensity, 0.1f, 0.0f, 100.0f);
      CheckPLDragStart();
      CheckPLDragEnd();
      ImGui::DragFloat("Range##PL", &pl.range, 0.1f, 0.1f, 200.0f);
      CheckPLDragStart();
      CheckPLDragEnd();

      if (ImGui::Button("Remove Point Light"))
        e->pointLight.reset();
    }
  } else {
    if (ImGui::Button("Add Point Light")) {
      e->pointLight = PointLightComponent{};
    }
  }

  // Spot light component.
  if (e->spotLight.has_value()) {
    if (ImGui::CollapsingHeader("Spot Light",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      auto &sl = e->spotLight.value();

      auto CheckSLDragStart = [&]() {
        if (ImGui::IsItemActivated() && !m_slDragActive) {
          m_slDragActive = true;
          m_slDragStart = sl;
        }
      };
      auto CheckSLDragEnd = [&]() {
        if (ImGui::IsItemDeactivatedAfterEdit() && m_slDragActive) {
          auto cmd = std::make_unique<SpotLightCommand>(
              scene, e->id, m_slDragStart, sl);
          m_history.PushWithoutExecute(std::move(cmd));
          m_slDragActive = false;
        }
      };

      ImGui::Checkbox("Enabled##SL", &sl.enabled);
      ImGui::DragFloat3("Direction##SL", &sl.direction.x, 0.01f, -1.0f, 1.0f);
      CheckSLDragStart();
      CheckSLDragEnd();
      ImGui::ColorEdit3("Color##SL", &sl.color.x);
      CheckSLDragStart();
      CheckSLDragEnd();
      ImGui::DragFloat("Intensity##SL", &sl.intensity, 0.1f, 0.0f, 100.0f);
      CheckSLDragStart();
      CheckSLDragEnd();
      ImGui::DragFloat("Range##SL", &sl.range, 0.1f, 0.1f, 200.0f);
      CheckSLDragStart();
      CheckSLDragEnd();
      ImGui::DragFloat("Inner Angle##SL", &sl.innerAngleDeg, 0.5f, 0.0f,
                        89.0f);
      CheckSLDragStart();
      CheckSLDragEnd();
      ImGui::DragFloat("Outer Angle##SL", &sl.outerAngleDeg, 0.5f, 0.0f,
                        89.0f);
      if (sl.outerAngleDeg < sl.innerAngleDeg + 0.5f)
        sl.outerAngleDeg = sl.innerAngleDeg + 0.5f;
      CheckSLDragStart();
      CheckSLDragEnd();

      if (ImGui::Button("Remove Spot Light"))
        e->spotLight.reset();
    }
  } else {
    if (ImGui::Button("Add Spot Light")) {
      e->spotLight = SpotLightComponent{};
    }
  }

  ImGui::End();
}

// ---- Lighting Panel (Phase 3) ----

void SceneEditor::DrawLightingPanel(Scene &scene, bool *iblEnabled) {
  ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
  ImGui::Begin("Lighting");

  auto &ls = scene.LightSettings();

  // Helper lambdas for drag coalescing (same pattern as material editor).
  auto CheckLightDragStart = [&]() {
    if (ImGui::IsItemActivated() && !m_lightDragActive) {
      m_lightDragActive = true;
      m_lightDragStart = ls;
    }
  };
  auto CheckLightDragEnd = [&]() {
    if (ImGui::IsItemDeactivatedAfterEdit() && m_lightDragActive) {
      auto cmd = std::make_unique<LightSettingsCommand>(scene, m_lightDragStart,
                                                        ls);
      m_history.PushWithoutExecute(std::move(cmd));
      m_lightDragActive = false;
    }
  };

  // ---- Directional (sun) light ----
  if (ImGui::CollapsingHeader("Directional Light",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::SliderFloat3("Sun Direction", &ls.lightDir.x, -1.0f, 1.0f);
    CheckLightDragStart();
    CheckLightDragEnd();

    ImGui::DragFloat("Intensity##Sun", &ls.lightIntensity, 0.1f, 0.0f, 50.0f,
                     "%.2f");
    CheckLightDragStart();
    CheckLightDragEnd();

    ImGui::ColorEdit3("Color##Sun", &ls.lightColor.x);
    CheckLightDragStart();
    CheckLightDragEnd();
  }

  // ---- Environment ----
  if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (iblEnabled)
      ImGui::Checkbox("Enable IBL", iblEnabled);
    ImGui::DragFloat("IBL Intensity", &ls.iblIntensity, 0.01f, 0.0f, 5.0f,
                     "%.2f");
    CheckLightDragStart();
    CheckLightDragEnd();
  }

  ImGui::End();
}

// ---- Shadow & SSAO Panel (Phase 4) ----

void SceneEditor::DrawShadowPanel(Scene &scene, DxContext &dx) {
  ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
  ImGui::Begin("Shadows & SSAO");

  auto &ss = scene.ShadowSettings();

  auto CheckShadowDragStart = [&]() {
    if (ImGui::IsItemActivated() && !m_shadowDragActive) {
      m_shadowDragActive = true;
      m_shadowDragStart = ss;
    }
  };
  auto CheckShadowDragEnd = [&]() {
    if (ImGui::IsItemDeactivatedAfterEdit() && m_shadowDragActive) {
      auto cmd =
          std::make_unique<ShadowSettingsCommand>(scene, m_shadowDragStart, ss);
      m_history.PushWithoutExecute(std::move(cmd));
      m_shadowDragActive = false;
    }
  };
  // Checkbox helper: immediate undo command (no drag coalescing needed).
  auto ShadowCheckbox = [&](const char *label, bool *val) {
    SceneShadowSettings before = ss;
    if (ImGui::Checkbox(label, val)) {
      auto cmd = std::make_unique<ShadowSettingsCommand>(scene, before, ss);
      m_history.PushWithoutExecute(std::move(cmd));
    }
  };

  // ---- Cascaded Shadows ----
  if (ImGui::CollapsingHeader("Cascaded Shadows",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    ShadowCheckbox("Enable Shadows", &ss.shadowsEnabled);

    ImGui::SliderFloat("Strength##CSM", &ss.shadowStrength, 0.0f, 1.0f,
                       "%.2f");
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ImGui::SliderFloat("Bias##CSM", &ss.shadowBias, 0.0000f, 0.01f, "%.5f",
                       ImGuiSliderFlags_Logarithmic);
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ImGui::SliderFloat("Lambda (split)", &ss.csmLambda, 0.0f, 1.0f, "%.2f");
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ImGui::SliderFloat("Max Distance", &ss.csmMaxDistance, 20.0f, 500.0f,
                       "%.0f");
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ShadowCheckbox("Debug Cascades", &ss.csmDebugCascades);

    ImGui::Separator();
    ImGui::Text("Shadow map: %ux%u x%u cascades", dx.GetShadowMap().Size(),
                dx.GetShadowMap().Size(), dx.GetShadowMap().CascadeCount());
  }

  // ---- SSAO ----
  if (ImGui::CollapsingHeader("SSAO", ImGuiTreeNodeFlags_DefaultOpen)) {
    ShadowCheckbox("Enable SSAO", &ss.ssaoEnabled);

    ImGui::SliderFloat("Radius##SSAO", &ss.ssaoRadius, 0.05f, 2.0f, "%.2f");
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ImGui::SliderFloat("Bias##SSAO", &ss.ssaoBias, 0.001f, 0.1f, "%.4f",
                       ImGuiSliderFlags_Logarithmic);
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ImGui::SliderFloat("Power##SSAO", &ss.ssaoPower, 0.5f, 8.0f, "%.1f");
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ImGui::SliderInt("Kernel Size##SSAO", &ss.ssaoKernelSize, 8, 64);
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ImGui::SliderFloat("Strength##SSAO", &ss.ssaoStrength, 0.0f, 2.0f,
                       "%.2f");
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ImGui::Separator();
    ImGui::Text("AO resolution: %ux%u", dx.SsaoWidth(), dx.SsaoHeight());
  }

  ImGui::End();
}

// ---- Post-Processing Panel (Phase 4) ----

void SceneEditor::DrawPostProcessPanel(Scene &scene, DxContext &dx) {
  ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
  ImGui::Begin("Post Processing");

  auto &pp = scene.PostProcessSettings();

  auto CheckPPDragStart = [&]() {
    if (ImGui::IsItemActivated() && !m_ppDragActive) {
      m_ppDragActive = true;
      m_ppDragStart = pp;
    }
  };
  auto CheckPPDragEnd = [&]() {
    if (ImGui::IsItemDeactivatedAfterEdit() && m_ppDragActive) {
      auto cmd = std::make_unique<PostProcessSettingsCommand>(
          scene, m_ppDragStart, pp);
      m_history.PushWithoutExecute(std::move(cmd));
      m_ppDragActive = false;
    }
  };
  auto PPCheckbox = [&](const char *label, bool *val) {
    ScenePostProcessSettings before = pp;
    if (ImGui::Checkbox(label, val)) {
      auto cmd =
          std::make_unique<PostProcessSettingsCommand>(scene, before, pp);
      m_history.PushWithoutExecute(std::move(cmd));
    }
  };

  // ---- Tone Mapping ----
  if (ImGui::CollapsingHeader("Tone Mapping",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::SliderFloat("Exposure", &pp.exposure, 0.01f, 10.0f, "%.2f",
                       ImGuiSliderFlags_Logarithmic);
    CheckPPDragStart();
    CheckPPDragEnd();
  }

  // ---- Bloom ----
  if (ImGui::CollapsingHeader("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
    PPCheckbox("Enable Bloom", &pp.bloomEnabled);

    ImGui::SliderFloat("Threshold##Bloom", &pp.bloomThreshold, 0.0f, 5.0f,
                       "%.2f");
    CheckPPDragStart();
    CheckPPDragEnd();

    ImGui::SliderFloat("Intensity##Bloom", &pp.bloomIntensity, 0.0f, 2.0f,
                       "%.2f");
    CheckPPDragStart();
    CheckPPDragEnd();
  }

  // ---- TAA ----
  if (ImGui::CollapsingHeader("TAA")) {
    bool prevTaa = pp.taaEnabled;
    PPCheckbox("Enable TAA", &pp.taaEnabled);
    if (pp.taaEnabled && !prevTaa) {
      dx.ResetTaaFirstFrame();
    }

    if (pp.taaEnabled) {
      ImGui::SliderFloat("Blend Factor##TAA", &pp.taaBlendFactor, 0.01f, 0.5f,
                         "%.3f");
      CheckPPDragStart();
      CheckPPDragEnd();
    }
  }

  // ---- FXAA ----
  if (ImGui::CollapsingHeader("FXAA")) {
    PPCheckbox("Enable FXAA", &pp.fxaaEnabled);
  }

  // ---- Motion Blur ----
  if (ImGui::CollapsingHeader("Motion Blur")) {
    PPCheckbox("Enable Motion Blur", &pp.motionBlurEnabled);

    if (pp.motionBlurEnabled) {
      ImGui::SliderFloat("Strength##MB", &pp.motionBlurStrength, 0.0f, 3.0f,
                         "%.2f");
      CheckPPDragStart();
      CheckPPDragEnd();

      ImGui::SliderInt("Samples##MB", &pp.motionBlurSamples, 4, 32);
      CheckPPDragStart();
      CheckPPDragEnd();
    }
  }

  // ---- Depth of Field ----
  if (ImGui::CollapsingHeader("Depth of Field")) {
    PPCheckbox("Enable DOF", &pp.dofEnabled);

    if (pp.dofEnabled) {
      ImGui::SliderFloat("Focal Distance##DOF", &pp.dofFocalDistance, 0.5f,
                         100.0f, "%.1f");
      CheckPPDragStart();
      CheckPPDragEnd();

      ImGui::SliderFloat("Focal Range##DOF", &pp.dofFocalRange, 0.5f, 50.0f,
                         "%.1f");
      CheckPPDragStart();
      CheckPPDragEnd();

      ImGui::SliderFloat("Max Blur##DOF", &pp.dofMaxBlur, 1.0f, 20.0f,
                         "%.1f");
      CheckPPDragStart();
      CheckPPDragEnd();
    }
  }

  ImGui::End();
}

void SceneEditor::BuildHighlightItems(const Scene &scene,
                                      FrameData &frame) const {
  if (m_selectedEntity == kInvalidEntityId)
    return;

  const Entity *e = scene.FindEntity(m_selectedEntity);
  if (!e || !e->active || !e->mesh.has_value())
    return;
  if (e->mesh->meshId == UINT32_MAX)
    return;

  frame.highlightItems.push_back({e->mesh->meshId, e->transform.WorldMatrix()});
}

// ---- Mouse picking via ray-AABB intersection ----

static DirectX::XMFLOAT3 MeshLocalHalfExtents(const MeshComponent &mc) {
  using namespace DirectX;
  switch (mc.sourceType) {
  case MeshSourceType::ProceduralCube: {
    float h = mc.size * 0.5f;
    return {h, h, h};
  }
  case MeshSourceType::ProceduralPlane:
    return {mc.width * 0.5f, 0.05f, mc.height * 0.5f};
  case MeshSourceType::ProceduralCylinder:
    return {mc.width, mc.height * 0.5f, mc.width};
  case MeshSourceType::ProceduralCone:
    return {mc.width, mc.height * 0.5f, mc.width};
  case MeshSourceType::ProceduralSphere:
    return {mc.size, mc.size, mc.size};
  default:
    return {0.5f, 0.5f, 0.5f};
  }
}

static DirectX::XMFLOAT3 MeshLocalCenter(const MeshComponent &mc) {
  using namespace DirectX;
  switch (mc.sourceType) {
  case MeshSourceType::ProceduralCone:
    return {0.0f, mc.height * 0.5f, 0.0f};
  default:
    return {0.0f, 0.0f, 0.0f};
  }
}

static bool RayVsAABB(DirectX::XMVECTOR rayOrigin, DirectX::XMVECTOR rayDir,
                      DirectX::XMFLOAT3 aabbMin, DirectX::XMFLOAT3 aabbMax,
                      float &tOut) {
  using namespace DirectX;
  float tMin = 0.0f;
  float tMax = 1e30f;

  float orig[3], dir[3], bmin[3], bmax[3];
  XMStoreFloat3(reinterpret_cast<XMFLOAT3 *>(orig), rayOrigin);
  XMStoreFloat3(reinterpret_cast<XMFLOAT3 *>(dir), rayDir);
  bmin[0] = aabbMin.x; bmin[1] = aabbMin.y; bmin[2] = aabbMin.z;
  bmax[0] = aabbMax.x; bmax[1] = aabbMax.y; bmax[2] = aabbMax.z;

  for (int i = 0; i < 3; ++i) {
    if (fabsf(dir[i]) < 1e-8f) {
      if (orig[i] < bmin[i] || orig[i] > bmax[i])
        return false;
    } else {
      float invD = 1.0f / dir[i];
      float t1 = (bmin[i] - orig[i]) * invD;
      float t2 = (bmax[i] - orig[i]) * invD;
      if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
      tMin = (t1 > tMin) ? t1 : tMin;
      tMax = (t2 < tMax) ? t2 : tMax;
      if (tMin > tMax)
        return false;
    }
  }
  tOut = tMin;
  return tMin >= 0.0f;
}

void SceneEditor::HandleMousePick(const Scene &scene, int screenX, int screenY,
                                  int screenW, int screenH,
                                  const DirectX::XMMATRIX &view,
                                  const DirectX::XMMATRIX &proj) {
  using namespace DirectX;

  float ndcX = 2.0f * screenX / screenW - 1.0f;
  float ndcY = -(2.0f * screenY / screenH - 1.0f);

  XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);
  XMVECTOR nearPt =
      XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invViewProj);
  XMVECTOR farPt =
      XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invViewProj);
  XMVECTOR rayDir = XMVector3Normalize(XMVectorSubtract(farPt, nearPt));
  XMVECTOR rayOrigin = nearPt;

  float bestT = 1e30f;
  EntityId bestId = kInvalidEntityId;

  for (const auto &e : scene.Entities()) {
    if (!e.active || !e.mesh.has_value())
      continue;
    if (e.mesh->meshId == UINT32_MAX)
      continue;

    XMMATRIX world = e.transform.WorldMatrix();
    XMMATRIX invWorld = XMMatrixInverse(nullptr, world);

    XMVECTOR localOrigin = XMVector3TransformCoord(rayOrigin, invWorld);
    XMVECTOR localDir = XMVector3Normalize(
        XMVector3TransformNormal(rayDir, invWorld));

    XMFLOAT3 halfExt = MeshLocalHalfExtents(e.mesh.value());
    XMFLOAT3 center = MeshLocalCenter(e.mesh.value());
    XMFLOAT3 aabbMin = {center.x - halfExt.x, center.y - halfExt.y,
                         center.z - halfExt.z};
    XMFLOAT3 aabbMax = {center.x + halfExt.x, center.y + halfExt.y,
                         center.z + halfExt.z};

    float t = 0.0f;
    if (RayVsAABB(localOrigin, localDir, aabbMin, aabbMax, t)) {
      if (t < bestT) {
        bestT = t;
        bestId = e.id;
      }
    }
  }

  m_selectedEntity = bestId;
}

// ======================================================================
// Grid Editor Panel (Milestone 4 Phase 5)
// ======================================================================

// Tile type names for brush palette.
static const char *kTileTypeNames[] = {"Floor",     "Wall",  "Fire",
                                       "Lightning", "Spike", "Ice",
                                       "Crumble",   "Start", "Goal"};
static constexpr int kTileTypeCount = 9;

// Colors for mini-grid rendering (matches neon aesthetic).
static ImU32 TileColor(TileType type, bool hasWall) {
  if (hasWall)
    return IM_COL32(160, 120, 80, 255); // brown for placed wall
  switch (type) {
  case TileType::Floor:     return IM_COL32(30, 30, 45, 255);
  case TileType::Wall:      return IM_COL32(100, 100, 120, 255);
  case TileType::Fire:      return IM_COL32(220, 60, 20, 255);
  case TileType::Lightning: return IM_COL32(40, 80, 200, 255);
  case TileType::Spike:     return IM_COL32(200, 180, 40, 255);
  case TileType::Ice:       return IM_COL32(40, 180, 220, 255);
  case TileType::Crumble:   return IM_COL32(100, 80, 60, 255);
  case TileType::Start:     return IM_COL32(30, 180, 60, 255);
  case TileType::Goal:      return IM_COL32(220, 200, 40, 255);
  }
  return IM_COL32(30, 30, 45, 255);
}

static const char *kTowerSideNames[] = {"Left", "Right", "Top", "Bottom"};
static const char *kTowerPatternNames[] = {"Row", "Column", "Cross",
                                           "Diagonal"};

static bool OpenStageFileDialog(char *outPath, size_t maxLen, bool save) {
  OPENFILENAMEA ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFilter = "Stage Files\0*.stage.json\0JSON Files\0*.json\0All "
                    "Files\0*.*\0";
  ofn.lpstrFile = outPath;
  ofn.nMaxFile = static_cast<DWORD>(maxLen);
  ofn.lpstrDefExt = "json";
  ofn.Flags = OFN_NOCHANGEDIR;
  if (!save)
    ofn.Flags |= OFN_FILEMUSTEXIST;
  outPath[0] = '\0';
  return save ? GetSaveFileNameA(&ofn) != 0 : GetOpenFileNameA(&ofn) != 0;
}

// ---- Camera Panel (Phase 6) ----

void SceneEditor::DrawCameraPanel(Scene &scene, Camera &cam) {
  ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
  ImGui::Begin("Camera");

  // Mode selector.
  int modeInt = static_cast<int>(cam.Mode());
  const char *modeNames[] = {"Free Fly", "Orbit", "Game (Top-Down)"};
  if (ImGui::Combo("Mode", &modeInt, modeNames, 3)) {
    cam.SetMode(static_cast<CameraMode>(modeInt));
  }

  ImGui::Separator();

  // Position display (read-only in orbit/game, editable in free-fly).
  DirectX::XMFLOAT3 pos = cam.GetPosition();
  if (cam.Mode() == CameraMode::FreeFly) {
    if (ImGui::DragFloat3("Position", &pos.x, 0.1f)) {
      cam.SetPosition(pos.x, pos.y, pos.z);
    }
    float yaw = DirectX::XMConvertToDegrees(cam.Yaw());
    float pitch = DirectX::XMConvertToDegrees(cam.Pitch());
    if (ImGui::DragFloat("Yaw", &yaw, 0.5f, -180.0f, 180.0f)) {
      cam.SetYawPitch(DirectX::XMConvertToRadians(yaw), cam.Pitch());
    }
    if (ImGui::DragFloat("Pitch", &pitch, 0.5f, -89.0f, 89.0f)) {
      cam.SetYawPitch(cam.Yaw(), DirectX::XMConvertToRadians(pitch));
    }
  } else {
    ImGui::Text("Position: %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);
  }

  // Orbit-specific controls.
  if (cam.Mode() == CameraMode::Orbit) {
    DirectX::XMFLOAT3 target = cam.OrbitTarget();
    if (ImGui::DragFloat3("Orbit Target", &target.x, 0.1f)) {
      cam.SetOrbitTarget(target.x, target.y, target.z);
    }
    float dist = cam.OrbitDistance();
    if (ImGui::DragFloat("Distance", &dist, 0.1f, 0.5f, 500.0f)) {
      cam.SetOrbitDistance(dist);
    }
  }

  ImGui::Separator();

  // Lens settings.
  float fovDeg = DirectX::XMConvertToDegrees(cam.FovY());
  float nearZ = cam.NearZ();
  float farZ = cam.FarZ();
  bool lensChanged = false;
  lensChanged |= ImGui::SliderFloat("FOV (deg)", &fovDeg, 10.0f, 120.0f);
  lensChanged |= ImGui::DragFloat("Near Plane", &nearZ, 0.01f, 0.001f, 10.0f, "%.3f");
  lensChanged |= ImGui::DragFloat("Far Plane", &farZ, 1.0f, 10.0f, 10000.0f, "%.0f");
  if (lensChanged) {
    cam.SetLens(DirectX::XMConvertToRadians(fovDeg), cam.Aspect(), nearZ, farZ);
  }

  ImGui::Separator();

  // Speed controls.
  float moveSpeed = cam.MoveSpeed();
  float lookSpeed = cam.LookSpeed() * 1000.0f; // display in mrad for readability
  if (ImGui::DragFloat("Move Speed", &moveSpeed, 0.1f, 0.1f, 100.0f))
    cam.SetMoveSpeed(moveSpeed);
  if (ImGui::DragFloat("Look Speed", &lookSpeed, 0.1f, 0.1f, 20.0f))
    cam.SetLookSpeed(lookSpeed * 0.001f);

  ImGui::Separator();

  // ---- Camera presets ----
  ImGui::Text("Presets");
  auto &presets = scene.CameraPresets();

  ImGui::InputText("Name", m_presetName, sizeof(m_presetName));
  ImGui::SameLine();
  if (ImGui::Button("Save")) {
    presets.push_back(cam.MakePreset(m_presetName));
  }

  // List saved presets.
  int deleteIdx = -1;
  for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
    ImGui::PushID(i);
    if (ImGui::Button("Load")) {
      cam.ApplyPreset(presets[i]);
    }
    ImGui::SameLine();
    if (ImGui::Button("X")) {
      deleteIdx = i;
    }
    ImGui::SameLine();
    ImGui::Text("%s", presets[i].name.c_str());
    ImGui::PopID();
  }
  if (deleteIdx >= 0) {
    presets.erase(presets.begin() + deleteIdx);
  }

  ImGui::End();
}

// ---- Asset Browser (Phase 7) ----

static AssetType ClassifyAsset(const std::string &ext) {
  if (ext == ".gltf" || ext == ".glb")
    return AssetType::Mesh;
  if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
      ext == ".tga")
    return AssetType::Texture;
  if (ext == ".json")
    return AssetType::Scene;
  return AssetType::Unknown;
}

void SceneEditor::ScanAssetDirectory() {
  m_assetCache.clear();
  namespace fs = std::filesystem;
  const fs::path root("Assets");
  if (!fs::exists(root) || !fs::is_directory(root))
    return;

  for (auto &entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file())
      continue;
    std::string ext = entry.path().extension().string();
    // lowercase extension
    for (auto &c : ext)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    AssetType type = ClassifyAsset(ext);
    if (type == AssetType::Unknown)
      continue;

    AssetEntry ae;
    ae.path = entry.path().generic_string(); // forward slashes
    ae.displayName = entry.path().filename().string();
    ae.type = type;
    m_assetCache.push_back(std::move(ae));
  }

  // Sort by path for consistent display.
  std::sort(m_assetCache.begin(), m_assetCache.end(),
            [](const AssetEntry &a, const AssetEntry &b) {
              return a.path < b.path;
            });

  m_assetCacheValid = true;
  m_selectedAssetIndex = -1;
}

void SceneEditor::DrawAssetBrowser(Scene &scene, DxContext &dx) {
  ImGui::SetNextWindowPos(ImVec2(0, 400), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(300, 350), ImGuiCond_FirstUseEver);
  ImGui::Begin("Asset Browser");

  // Scan / Refresh.
  if (!m_assetCacheValid || ImGui::Button("Refresh")) {
    ScanAssetDirectory();
  }

  if (m_assetCache.empty()) {
    ImGui::TextDisabled("No assets found in Assets/");
    ImGui::End();
    return;
  }

  // Filter combo.
  const char *filterLabels[] = {"All", "Meshes", "Textures", "Scenes"};
  ImGui::Combo("Filter", &m_assetFilterType, filterLabels,
               IM_ARRAYSIZE(filterLabels));

  ImGui::Separator();

  // Build directory tree structure. We use a flat list grouped by parent dir.
  // Track which directories are open via TreeNode.
  ImGui::BeginChild("AssetTree", ImVec2(0, -140), true);

  std::string lastDir;
  bool dirOpen = false;

  for (int i = 0; i < static_cast<int>(m_assetCache.size()); ++i) {
    const auto &asset = m_assetCache[i];

    // Apply filter.
    if (m_assetFilterType == 1 && asset.type != AssetType::Mesh)
      continue;
    if (m_assetFilterType == 2 && asset.type != AssetType::Texture)
      continue;
    if (m_assetFilterType == 3 && asset.type != AssetType::Scene)
      continue;

    // Extract parent directory.
    std::string dir;
    auto slashPos = asset.path.rfind('/');
    if (slashPos != std::string::npos)
      dir = asset.path.substr(0, slashPos);

    // If directory changed, close old tree node and open new one.
    if (dir != lastDir) {
      if (!lastDir.empty() && dirOpen) {
        ImGui::TreePop();
        dirOpen = false;
      }
      if (!dir.empty()) {
        dirOpen = ImGui::TreeNode(dir.c_str());
      } else {
        dirOpen = true; // root-level files always shown
      }
      lastDir = dir;
    }

    if (!dirOpen)
      continue;

    // Type prefix.
    const char *prefix = "[?]";
    switch (asset.type) {
    case AssetType::Mesh:    prefix = "[M]"; break;
    case AssetType::Texture: prefix = "[T]"; break;
    case AssetType::Scene:   prefix = "[S]"; break;
    default: break;
    }

    char label[512];
    snprintf(label, sizeof(label), "%s %s", prefix, asset.displayName.c_str());

    bool selected = (m_selectedAssetIndex == i);
    if (ImGui::Selectable(label, selected)) {
      m_selectedAssetIndex = i;
    }

    // Double-click scene files to load.
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) &&
        asset.type == AssetType::Scene) {
      scene.LoadFromFile(asset.path, dx);
    }

    // Drag-drop source.
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
      const char *payloadType = "ASSET_PATH";
      ImGui::SetDragDropPayload(payloadType, asset.path.c_str(),
                                asset.path.size() + 1);
      ImGui::Text("%s %s", prefix, asset.displayName.c_str());
      ImGui::EndDragDropSource();
    }
  }

  // Close last dir tree node if open.
  if (!lastDir.empty() && dirOpen)
    ImGui::TreePop();

  ImGui::EndChild();

  // ---- Detail / Preview area ----
  ImGui::Separator();

  if (m_selectedAssetIndex >= 0 &&
      m_selectedAssetIndex < static_cast<int>(m_assetCache.size())) {
    const auto &sel = m_assetCache[m_selectedAssetIndex];

    ImGui::Text("File: %s", sel.displayName.c_str());
    ImGui::TextWrapped("Path: %s", sel.path.c_str());

    // File size.
    namespace fs = std::filesystem;
    if (fs::exists(sel.path)) {
      auto sz = fs::file_size(sel.path);
      if (sz < 1024)
        ImGui::Text("Size: %llu B", static_cast<unsigned long long>(sz));
      else if (sz < 1024 * 1024)
        ImGui::Text("Size: %.1f KB", sz / 1024.0);
      else
        ImGui::Text("Size: %.1f MB", sz / (1024.0 * 1024.0));
    }

    // Texture preview.
    if (sel.type == AssetType::Texture) {
      // Only reload when selection changes.
      // Request upload (deferred to next BeginFrame).
      if (m_previewTexturePath != sel.path) {
        LoadedImage img;
        if (LoadImageFile(sel.path, img)) {
          dx.RequestPreviewTexture(img);
          m_previewTexturePath = sel.path;
        }
      }
      // Show preview if available (uploaded in previous frame's BeginFrame).
      if (m_previewTexturePath == sel.path && dx.HasPreviewTexture()) {
        ImGui::Image(static_cast<ImTextureID>(dx.PreviewTextureGpu().ptr),
                     ImVec2(64, 64));
      }
    }

    // Assign buttons.
    Entity *selEnt = scene.FindEntity(m_selectedEntity);
    if (selEnt && selEnt->mesh.has_value()) {
      if (sel.type == AssetType::Mesh && ImGui::Button("Load as Mesh")) {
        auto &mc = selEnt->mesh.value();
        Material oldMat = mc.material;
        auto oldPaths = mc.texturePaths;
        mc.sourceType = MeshSourceType::GltfFile;
        mc.gltfPath = sel.path;
        mc.meshId = UINT32_MAX;
        m_history.Execute(std::make_unique<MaterialCommand>(
            scene, dx, selEnt->id, oldMat, mc.material, oldPaths,
            mc.texturePaths));
      }

      if (sel.type == AssetType::Texture) {
        if (ImGui::BeginCombo("Assign to Slot", "Select...")) {
          for (int s = 0; s < 6; ++s) {
            if (ImGui::Selectable(kTexSlotNames[s])) {
              auto &mc = selEnt->mesh.value();
              Material oldMat = mc.material;
              auto oldPaths = mc.texturePaths;
              mc.texturePaths[s] = sel.path;
              m_history.Execute(std::make_unique<MaterialCommand>(
                  scene, dx, selEnt->id, oldMat, mc.material, oldPaths,
                  mc.texturePaths));
            }
          }
          ImGui::EndCombo();
        }
      }
    }
  } else {
    ImGui::TextDisabled("Select an asset above.");
  }

  ImGui::End();
}

void SceneEditor::DrawGridEditorPanel(StageData &stage) {
  ImGui::SetNextWindowSize(ImVec2(340, 700), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowCollapsed(false, ImGuiCond_FirstUseEver);
  ImGui::Begin("Grid Editor");

  // ---- Play Test button (Phase 5C) ----
  if (ImGui::Button("Play Test (F5)")) {
    m_playTestRequested = true;
  }
  ImGui::Separator();

  // ---- A. File I/O toolbar ----
  if (ImGui::CollapsingHeader("File", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::InputText("Path", m_stagePath, sizeof(m_stagePath));

    if (ImGui::Button("New Stage")) {
      stage.Clear();
      m_gridHistory.Clear();
      m_selectedTileX = m_selectedTileY = -1;
      m_selectedTower = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
      if (m_stagePath[0] != '\0')
        StageSerializer::SaveToFile(stage, m_stagePath);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save As...")) {
      char path[256] = {};
      if (OpenStageFileDialog(path, sizeof(path), true)) {
        if (StageSerializer::SaveToFile(stage, path))
          std::snprintf(m_stagePath, sizeof(m_stagePath), "%s", path);
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load...")) {
      char path[256] = {};
      if (OpenStageFileDialog(path, sizeof(path), false)) {
        StageData loaded;
        if (StageSerializer::LoadFromFile(path, loaded)) {
          stage = loaded;
          std::snprintf(m_stagePath, sizeof(m_stagePath), "%s", path);
          m_gridHistory.Clear();
          m_selectedTileX = m_selectedTileY = -1;
          m_selectedTower = -1;
        }
      }
    }
  }

  // ---- B. Stage metadata ----
  if (ImGui::CollapsingHeader("Stage Settings",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    // Metadata drag coalescing helpers.
    auto MetaDragStart = [&]() {
      if (ImGui::IsItemActivated() && !m_metaDragActive) {
        m_metaDragActive = true;
        m_metaDragStart = ExtractMetadata(stage);
      }
    };
    auto MetaDragEnd = [&]() {
      if (ImGui::IsItemDeactivatedAfterEdit() && m_metaDragActive) {
        auto cmd = std::make_unique<StageMetadataCommand>(
            stage, m_metaDragStart, ExtractMetadata(stage));
        m_gridHistory.PushWithoutExecute(std::move(cmd));
        m_metaDragActive = false;
      }
    };

    // Name.
    char nameBuf[128] = {};
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", stage.name.c_str());
    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
      auto before = ExtractMetadata(stage);
      stage.name = nameBuf;
      auto cmd = std::make_unique<StageMetadataCommand>(
          stage, before, ExtractMetadata(stage));
      m_gridHistory.PushWithoutExecute(std::move(cmd));
    }

    // Grid dimensions (resize) — drag coalesced.
    int dims[2] = {stage.width, stage.height};
    if (ImGui::DragInt2("Grid Size", dims, 1.0f, 2, 100)) {
      dims[0] = std::clamp(dims[0], 2, 100);
      dims[1] = std::clamp(dims[1], 2, 100);
      if (dims[0] != stage.width || dims[1] != stage.height) {
        stage.Resize(dims[0], dims[1]);
        // Clamp selection.
        if (m_selectedTileX >= stage.width)
          m_selectedTileX = -1;
        if (m_selectedTileY >= stage.height)
          m_selectedTileY = -1;
      }
    }
    if (ImGui::IsItemActivated()) {
      m_resizeDragActive = true;
      m_resizeDragStart = stage;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_resizeDragActive) {
      auto cmd =
          std::make_unique<ResizeGridCommand>(stage, m_resizeDragStart, stage);
      m_gridHistory.PushWithoutExecute(std::move(cmd));
      m_resizeDragActive = false;
    }

    ImGui::DragFloat("Time Limit", &stage.timeLimit, 1.0f, 0.0f, 600.0f,
                     "%.0f s");
    MetaDragStart();
    MetaDragEnd();

    ImGui::DragInt("Par Moves", &stage.parMoves, 1.0f, 0, 999);
    MetaDragStart();
    MetaDragEnd();

    ImGui::Separator();
    ImGui::Text("Spawn Positions");

    ImGui::DragInt("Player X", &stage.playerSpawnX, 1.0f, 0,
                   stage.width - 1);
    MetaDragStart();
    MetaDragEnd();
    ImGui::DragInt("Player Y", &stage.playerSpawnY, 1.0f, 0,
                   stage.height - 1);
    MetaDragStart();
    MetaDragEnd();
    ImGui::DragInt("Cargo X", &stage.cargoSpawnX, 1.0f, 0,
                   stage.width - 1);
    MetaDragStart();
    MetaDragEnd();
    ImGui::DragInt("Cargo Y", &stage.cargoSpawnY, 1.0f, 0,
                   stage.height - 1);
    MetaDragStart();
    MetaDragEnd();
  }

  // ---- C. Brush palette ----
  if (ImGui::CollapsingHeader("Brush", ImGuiTreeNodeFlags_DefaultOpen)) {
    for (int i = 0; i < kTileTypeCount; ++i) {
      // Color indicator.
      ImVec4 c;
      ImU32 col = TileColor(static_cast<TileType>(i), false);
      c.x = ((col >> 0) & 0xFF) / 255.0f;
      c.y = ((col >> 8) & 0xFF) / 255.0f;
      c.z = ((col >> 16) & 0xFF) / 255.0f;
      c.w = 1.0f;
      ImGui::ColorButton(("##bc" + std::to_string(i)).c_str(), c,
                          ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
      ImGui::SameLine();
      ImGui::RadioButton(kTileTypeNames[i], &m_brushTileType, i);
    }

    ImGui::Separator();
    ImGui::Checkbox("Place Wall On Tile", &m_brushWall);
    if (m_brushWall)
      ImGui::Checkbox("  Destructible", &m_brushDestructible);
  }

  // ---- D. Mini-grid view ----
  if (ImGui::CollapsingHeader("Grid View", ImGuiTreeNodeFlags_DefaultOpen)) {
    stage.EnsureSize();

    const float avail = ImGui::GetContentRegionAvail().x;
    const int maxDim = std::max(stage.width, stage.height);
    const float cellSize =
        std::max(4.0f, std::min(avail / maxDim, 24.0f));
    const float gridW = cellSize * stage.width;
    const float gridH = cellSize * stage.height;

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##grid", ImVec2(gridW, gridH));

    ImDrawList *dl = ImGui::GetWindowDrawList();

    // Draw tiles.
    for (int y = 0; y < stage.height; ++y) {
      for (int x = 0; x < stage.width; ++x) {
        const auto &tile = stage.At(x, y);
        ImVec2 pMin = {canvasPos.x + x * cellSize,
                       canvasPos.y + y * cellSize};
        ImVec2 pMax = {pMin.x + cellSize - 1.0f, pMin.y + cellSize - 1.0f};
        dl->AddRectFilled(pMin, pMax, TileColor(tile.type, tile.hasWall));
      }
    }

    // Player spawn marker.
    if (stage.InBounds(stage.playerSpawnX, stage.playerSpawnY)) {
      ImVec2 p = {canvasPos.x + stage.playerSpawnX * cellSize + cellSize * 0.5f,
                  canvasPos.y + stage.playerSpawnY * cellSize + cellSize * 0.5f};
      dl->AddText(ImVec2(p.x - 3.0f, p.y - 5.0f), IM_COL32(0, 255, 255, 255),
                  "P");
    }

    // Cargo spawn marker.
    if (stage.InBounds(stage.cargoSpawnX, stage.cargoSpawnY)) {
      ImVec2 p = {canvasPos.x + stage.cargoSpawnX * cellSize + cellSize * 0.5f,
                  canvasPos.y + stage.cargoSpawnY * cellSize + cellSize * 0.5f};
      dl->AddText(ImVec2(p.x - 3.0f, p.y - 5.0f),
                  IM_COL32(255, 200, 0, 255), "C");
    }

    // Tower position markers (red triangles on perimeter).
    for (const auto &tower : stage.towers) {
      float tx = 0.0f, ty = 0.0f;
      switch (tower.side) {
      case TowerSide::Left:
        tx = canvasPos.x - 6.0f;
        ty = canvasPos.y + tower.y * cellSize + cellSize * 0.5f;
        break;
      case TowerSide::Right:
        tx = canvasPos.x + gridW + 2.0f;
        ty = canvasPos.y + tower.y * cellSize + cellSize * 0.5f;
        break;
      case TowerSide::Top:
        tx = canvasPos.x + tower.x * cellSize + cellSize * 0.5f;
        ty = canvasPos.y - 6.0f;
        break;
      case TowerSide::Bottom:
        tx = canvasPos.x + tower.x * cellSize + cellSize * 0.5f;
        ty = canvasPos.y + gridH + 2.0f;
        break;
      }
      dl->AddTriangleFilled(ImVec2(tx, ty - 4), ImVec2(tx - 4, ty + 4),
                            ImVec2(tx + 4, ty + 4),
                            IM_COL32(255, 50, 50, 255));
    }

    // Selected tile highlight.
    if (m_selectedTileX >= 0 && m_selectedTileX < stage.width &&
        m_selectedTileY >= 0 && m_selectedTileY < stage.height) {
      ImVec2 pMin = {canvasPos.x + m_selectedTileX * cellSize,
                     canvasPos.y + m_selectedTileY * cellSize};
      ImVec2 pMax = {pMin.x + cellSize - 1.0f, pMin.y + cellSize - 1.0f};
      dl->AddRect(pMin, pMax, IM_COL32(255, 255, 255, 255), 0.0f, 0,
                  2.0f);
    }

    // ---- Mini-grid mouse interaction (click to paint) ----
    if (ImGui::IsItemActive()) {
      ImVec2 mouse = ImGui::GetMousePos();
      int mx = static_cast<int>((mouse.x - canvasPos.x) / cellSize);
      int my = static_cast<int>((mouse.y - canvasPos.y) / cellSize);
      if (stage.InBounds(mx, my)) {
        m_selectedTileX = mx;
        m_selectedTileY = my;

        // Build brush tile.
        TileData brushTile;
        brushTile.type = static_cast<TileType>(m_brushTileType);
        brushTile.hasWall = m_brushWall;
        brushTile.wallDestructible = m_brushDestructible;

        const TileData &current = stage.At(mx, my);

        // Check if we need to paint (tile differs from brush).
        bool differs = current.type != brushTile.type ||
                       current.hasWall != brushTile.hasWall ||
                       current.wallDestructible != brushTile.wallDestructible;

        if (differs) {
          // Check if this tile is already in the current stroke.
          bool alreadyInStroke = false;
          for (const auto &e : m_paintStroke) {
            if (e.x == mx && e.y == my) {
              alreadyInStroke = true;
              break;
            }
          }
          if (!alreadyInStroke) {
            PaintTilesCommand::Entry entry;
            entry.x = mx;
            entry.y = my;
            entry.before = current;
            entry.after = brushTile;
            m_paintStroke.push_back(entry);

            // Apply immediately for visual feedback.
            stage.At(mx, my) = brushTile;
          }
        }
        m_miniGridPainting = true;
      }
    }

    // Finalize stroke on mouse release.
    if (m_miniGridPainting && !ImGui::IsItemActive()) {
      if (!m_paintStroke.empty()) {
        if (m_paintStroke.size() == 1) {
          auto &e = m_paintStroke[0];
          auto cmd = std::make_unique<PaintTileCommand>(stage, e.x, e.y,
                                                        e.before, e.after);
          m_gridHistory.PushWithoutExecute(std::move(cmd));
        } else {
          auto cmd = std::make_unique<PaintTilesCommand>(
              stage, std::move(m_paintStroke));
          m_gridHistory.PushWithoutExecute(std::move(cmd));
        }
        m_paintStroke.clear();
      }
      m_miniGridPainting = false;
    }

    // Info text.
    if (m_selectedTileX >= 0 && m_selectedTileY >= 0 &&
        stage.InBounds(m_selectedTileX, m_selectedTileY)) {
      const auto &sel = stage.At(m_selectedTileX, m_selectedTileY);
      ImGui::Text("Selected: (%d, %d) %s%s", m_selectedTileX,
                  m_selectedTileY,
                  kTileTypeNames[static_cast<int>(sel.type)],
                  sel.hasWall ? " [Wall]" : "");
    }

    ImGui::Text("Grid: %d x %d (%d tiles)", stage.width, stage.height,
                stage.width * stage.height);
  }

  // ---- E. Tower list ----
  if (ImGui::CollapsingHeader("Towers")) {
    // Tower drag coalescing helpers.
    auto TowerDragStart = [&]() {
      if (ImGui::IsItemActivated() && !m_towerDragActive) {
        m_towerDragActive = true;
        m_towerDragStart = stage.towers;
      }
    };
    auto TowerDragEnd = [&]() {
      if (ImGui::IsItemDeactivatedAfterEdit() && m_towerDragActive) {
        auto cmd = std::make_unique<TowerCommand>(stage, m_towerDragStart,
                                                  stage.towers);
        m_gridHistory.PushWithoutExecute(std::move(cmd));
        m_towerDragActive = false;
      }
    };

    if (ImGui::Button("Add Tower")) {
      auto before = stage.towers;
      TowerData newTower;
      newTower.side = TowerSide::Left;
      newTower.y = 0;
      stage.towers.push_back(newTower);
      auto cmd =
          std::make_unique<TowerCommand>(stage, before, stage.towers);
      m_gridHistory.PushWithoutExecute(std::move(cmd));
      m_selectedTower = static_cast<int>(stage.towers.size()) - 1;
    }

    // Tower list.
    for (int i = 0; i < static_cast<int>(stage.towers.size()); ++i) {
      ImGui::PushID(i);
      bool selected = (m_selectedTower == i);
      char label[64];
      std::snprintf(label, sizeof(label), "Tower %d (%s)", i,
                    kTowerSideNames[static_cast<int>(stage.towers[i].side)]);
      if (ImGui::Selectable(label, selected))
        m_selectedTower = i;
      ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
      if (ImGui::SmallButton("X")) {
        auto before = stage.towers;
        stage.towers.erase(stage.towers.begin() + i);
        auto cmd =
            std::make_unique<TowerCommand>(stage, before, stage.towers);
        m_gridHistory.PushWithoutExecute(std::move(cmd));
        if (m_selectedTower >= static_cast<int>(stage.towers.size()))
          m_selectedTower = static_cast<int>(stage.towers.size()) - 1;
        ImGui::PopID();
        break; // list changed, bail
      }
      ImGui::PopID();
    }

    // Tower inspector.
    if (m_selectedTower >= 0 &&
        m_selectedTower < static_cast<int>(stage.towers.size())) {
      ImGui::Separator();
      auto &t = stage.towers[m_selectedTower];

      int sideIdx = static_cast<int>(t.side);
      if (ImGui::Combo("Side", &sideIdx, kTowerSideNames, 4)) {
        auto before = stage.towers;
        t.side = static_cast<TowerSide>(sideIdx);
        auto cmd =
            std::make_unique<TowerCommand>(stage, before, stage.towers);
        m_gridHistory.PushWithoutExecute(std::move(cmd));
      }

      // Position along the side.
      int maxPos = (t.side == TowerSide::Left || t.side == TowerSide::Right)
                       ? stage.height - 1
                       : stage.width - 1;
      int &posRef = (t.side == TowerSide::Left || t.side == TowerSide::Right)
                        ? t.y
                        : t.x;
      ImGui::DragInt("Position", &posRef, 1.0f, 0, maxPos);
      TowerDragStart();
      TowerDragEnd();

      int patIdx = static_cast<int>(t.pattern);
      if (ImGui::Combo("Pattern", &patIdx, kTowerPatternNames, 4)) {
        auto before = stage.towers;
        t.pattern = static_cast<TowerPattern>(patIdx);
        auto cmd =
            std::make_unique<TowerCommand>(stage, before, stage.towers);
        m_gridHistory.PushWithoutExecute(std::move(cmd));
      }

      ImGui::DragFloat("Delay", &t.delay, 0.1f, 0.0f, 10.0f, "%.1f s");
      TowerDragStart();
      TowerDragEnd();

      ImGui::DragFloat("Interval", &t.interval, 0.1f, 0.1f, 10.0f,
                       "%.1f s");
      TowerDragStart();
      TowerDragEnd();
    }
  }

  // ---- F. Undo/Redo controls ----
  ImGui::Separator();
  {
    bool canUndo = m_gridHistory.CanUndo();
    bool canRedo = m_gridHistory.CanRedo();

    if (!canUndo)
      ImGui::BeginDisabled();
    if (ImGui::Button("Undo"))
      m_gridHistory.Undo();
    if (!canUndo)
      ImGui::EndDisabled();

    ImGui::SameLine();

    if (!canRedo)
      ImGui::BeginDisabled();
    if (ImGui::Button("Redo"))
      m_gridHistory.Redo();
    if (!canRedo)
      ImGui::EndDisabled();

    if (canUndo) {
      ImGui::SameLine();
      ImGui::TextDisabled("(%s)", m_gridHistory.UndoName());
    }
  }

  ImGui::End();
}

// ========================================================================
// Phase 5B — Editor viewport rendering, tile picking, viewport painting
// ========================================================================

void SceneEditor::InitEditorMeshes(DxContext &dx) {
  if (m_editorMeshesInitialized)
    return;

  auto plane = ProceduralMesh::CreatePlane(1.0f, 1.0f);
  auto cube = ProceduralMesh::CreateCube(1.0f);
  auto cone = ProceduralMesh::CreateCone(0.4f, 1.2f, 12);

  m_editorMeshIds.floor = dx.CreateMeshResources(plane, {}, MakeFloorMaterial());
  m_editorMeshIds.wall = dx.CreateMeshResources(cube, {}, MakeWallMaterial());
  m_editorMeshIds.fire = dx.CreateMeshResources(plane, {}, MakeFireMaterial());
  m_editorMeshIds.lightning =
      dx.CreateMeshResources(plane, {}, MakeLightningMaterial());
  m_editorMeshIds.spike = dx.CreateMeshResources(plane, {}, MakeSpikeMaterial());
  m_editorMeshIds.ice = dx.CreateMeshResources(plane, {}, MakeIceMaterial());
  m_editorMeshIds.crumble =
      dx.CreateMeshResources(plane, {}, MakeCrumbleMaterial());
  m_editorMeshIds.start = dx.CreateMeshResources(plane, {}, MakeStartMaterial());
  m_editorMeshIds.goal = dx.CreateMeshResources(plane, {}, MakeGoalMaterial());
  m_editorMeshIds.playerSpawn =
      dx.CreateMeshResources(cube, {}, MakePlayerMaterial());
  m_editorMeshIds.cargoSpawn =
      dx.CreateMeshResources(cube, {}, MakeCargoMaterial());
  m_editorMeshIds.tower = dx.CreateMeshResources(cone, {}, MakeTowerMaterial());
  m_editorMeshIds.highlight =
      dx.CreateMeshResources(cube, {}, MakeHighlightMaterial());
  m_editorMeshIds.telegraph =
      dx.CreateMeshResources(plane, {}, MakeTelegraphMaterial());

  m_editorMeshesInitialized = true;
}

void SceneEditor::BuildStageViewportItems(const StageData &stage,
                                          FrameData &frame) const {
  if (!m_gridEditorOpen || !m_editorMeshesInitialized)
    return;

  // Compute attack preview for selected tower (Phase 5C).
  std::vector<std::pair<int, int>> attackTiles;
  const std::vector<std::pair<int, int>> *attackPtr = nullptr;
  if (m_selectedTower >= 0 &&
      m_selectedTower < static_cast<int>(stage.towers.size())) {
    attackTiles = ComputeAttackTiles(stage.towers[m_selectedTower],
                                     stage.width, stage.height);
    attackPtr = &attackTiles;
  }

  BuildStageRenderItems(stage, m_editorMeshIds, frame.opaqueItems,
                        m_selectedTileX, m_selectedTileY, attackPtr);
}

// ---- Ray-plane intersection for tile picking ----

bool SceneEditor::ViewportRayToTile(int screenX, int screenY, int screenW,
                                    int screenH,
                                    const DirectX::XMMATRIX &view,
                                    const DirectX::XMMATRIX &proj,
                                    const StageData &stage, int &outX,
                                    int &outY) const {
  using namespace DirectX;

  // Screen -> NDC.
  float ndcX = 2.0f * screenX / screenW - 1.0f;
  float ndcY = -(2.0f * screenY / screenH - 1.0f);

  // NDC -> World ray via inverse ViewProj.
  XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);
  XMVECTOR nearPt =
      XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invViewProj);
  XMVECTOR farPt =
      XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invViewProj);
  XMVECTOR rayDir = XMVector3Normalize(XMVectorSubtract(farPt, nearPt));
  XMVECTOR rayOrigin = nearPt;

  // Ray-plane intersection at y=0.
  float originY = XMVectorGetY(rayOrigin);
  float dirY = XMVectorGetY(rayDir);

  if (std::fabsf(dirY) < 1e-6f)
    return false; // Ray parallel to plane.

  float t = -originY / dirY;
  if (t < 0.0f)
    return false; // Plane behind camera.

  XMVECTOR hitPt = XMVectorAdd(rayOrigin, XMVectorScale(rayDir, t));
  float hitX = XMVectorGetX(hitPt);
  float hitZ = XMVectorGetZ(hitPt);

  // Tile at grid coord (gx, gy) is centered at world (gx, 0, gy).
  int gx = static_cast<int>(std::floorf(hitX + 0.5f));
  int gy = static_cast<int>(std::floorf(hitZ + 0.5f));

  if (!stage.InBounds(gx, gy))
    return false;

  outX = gx;
  outY = gy;
  return true;
}

void SceneEditor::HandleViewportTilePaint(StageData &stage, int screenX,
                                          int screenY, int screenW,
                                          int screenH,
                                          const DirectX::XMMATRIX &view,
                                          const DirectX::XMMATRIX &proj) {
  int gx, gy;
  if (!ViewportRayToTile(screenX, screenY, screenW, screenH, view, proj, stage,
                         gx, gy))
    return;

  // Update selection to the tile under cursor.
  m_selectedTileX = gx;
  m_selectedTileY = gy;

  // Build brush tile from current brush state.
  TileData brushTile;
  brushTile.type = static_cast<TileType>(m_brushTileType);
  brushTile.hasWall = m_brushWall;
  brushTile.wallDestructible = m_brushDestructible;

  const TileData &current = stage.At(gx, gy);
  bool differs = (current.type != brushTile.type ||
                  current.hasWall != brushTile.hasWall ||
                  current.wallDestructible != brushTile.wallDestructible);

  if (!differs)
    return;

  // Check if already painted this tile in the current stroke.
  for (const auto &e : m_viewportPaintStroke) {
    if (e.x == gx && e.y == gy)
      return;
  }

  PaintTilesCommand::Entry entry;
  entry.x = gx;
  entry.y = gy;
  entry.before = current;
  entry.after = brushTile;
  m_viewportPaintStroke.push_back(entry);

  // Apply immediately for visual feedback.
  stage.At(gx, gy) = brushTile;
  m_viewportPainting = true;
}

void SceneEditor::FinalizeViewportPaintStroke(StageData &stage) {
  if (!m_viewportPainting || m_viewportPaintStroke.empty()) {
    m_viewportPainting = false;
    m_viewportPaintStroke.clear();
    return;
  }

  if (m_viewportPaintStroke.size() == 1) {
    auto &e = m_viewportPaintStroke[0];
    auto cmd =
        std::make_unique<PaintTileCommand>(stage, e.x, e.y, e.before, e.after);
    m_gridHistory.PushWithoutExecute(std::move(cmd));
  } else {
    auto cmd = std::make_unique<PaintTilesCommand>(
        stage, std::move(m_viewportPaintStroke));
    m_gridHistory.PushWithoutExecute(std::move(cmd));
  }

  m_viewportPaintStroke.clear();
  m_viewportPainting = false;
}

// ---- Tower viewport picking (Phase 5C) ----

int SceneEditor::ViewportPickTower(const StageData &stage, int screenX,
                                   int screenY, int screenW, int screenH,
                                   const DirectX::XMMATRIX &view,
                                   const DirectX::XMMATRIX &proj) const {
  using namespace DirectX;

  // Screen -> NDC -> world ray.
  float ndcX = 2.0f * screenX / screenW - 1.0f;
  float ndcY = -(2.0f * screenY / screenH - 1.0f);

  XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);
  XMVECTOR nearPt =
      XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invViewProj);
  XMVECTOR farPt =
      XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invViewProj);
  XMVECTOR rayDir = XMVector3Normalize(XMVectorSubtract(farPt, nearPt));
  XMVECTOR rayOrigin = nearPt;

  // Intersect with y=0.8 plane (tower cone center height).
  float originY = XMVectorGetY(rayOrigin);
  float dirY = XMVectorGetY(rayDir);
  if (std::fabsf(dirY) < 1e-6f)
    return -1;
  float t = (0.8f - originY) / dirY;
  if (t < 0.0f)
    return -1;

  XMVECTOR hitPt = XMVectorAdd(rayOrigin, XMVectorScale(rayDir, t));
  float hitX = XMVectorGetX(hitPt);
  float hitZ = XMVectorGetZ(hitPt);

  // Find closest tower within 0.5 units.
  int bestIdx = -1;
  float bestDist = 0.5f * 0.5f; // squared threshold

  for (int i = 0; i < static_cast<int>(stage.towers.size()); ++i) {
    const auto &tw = stage.towers[i];
    // Tower world position matches BuildStageRenderItems: (tw.x, 0.8, tw.y).
    float twX = static_cast<float>(tw.x);
    float twZ = static_cast<float>(tw.y);

    float dx = hitX - twX;
    float dz = hitZ - twZ;
    float dist2 = dx * dx + dz * dz;
    if (dist2 < bestDist) {
      bestDist = dist2;
      bestIdx = i;
    }
  }

  return bestIdx;
}
