// ======================================
// File: SceneEditor.h
// Purpose: ImGui-based editor UI for the scene/entity system.
//          Entity list, property inspector, scene save/load,
//          transform gizmo, undo/redo, duplication.
//          Milestone 4 Phase 0+1.
// ======================================

#pragma once

#include "CommandHistory.h"
#include "GizmoController.h"
#include "Scene.h"
#include "../gridgame/StageData.h"
#include "../gridgame/GridEditorCommands.h"

#include <d3d12.h>
#include <DirectXMath.h>
#include <array>
#include <string>
#include <vector>

class Camera;
class DxContext;
struct FrameData;

// ---- Asset Browser types (Phase 7) ----
enum class AssetType : uint8_t { Mesh, Texture, Scene, Unknown };

struct AssetEntry {
  std::string path;        // relative to project root
  std::string displayName; // filename only
  AssetType type;
};

class SceneEditor {
public:
  // Draw all editor ImGui panels. Call once per frame during editor mode.
  // iblEnabled: pointer to IBL toggle (owned by main loop, controls descriptor binding).
  // editStage: pointer to the stage being edited (null if grid editor not used).
  // cam: pointer to the editor camera (for Phase 6 camera panel).
  void DrawUI(Scene &scene, DxContext &dx, const DirectX::XMMATRIX &view,
              const DirectX::XMMATRIX &proj, bool *iblEnabled = nullptr,
              StageData *editStage = nullptr, Camera *cam = nullptr);

  // Add wireframe highlight for the selected entity into frame.highlightItems.
  void BuildHighlightItems(const Scene &scene, FrameData &frame) const;

  // Mouse picking: cast ray through screen pixel and select nearest entity.
  void HandleMousePick(const Scene &scene, int screenX, int screenY,
                       int screenW, int screenH,
                       const DirectX::XMMATRIX &view,
                       const DirectX::XMMATRIX &proj);

  // Current selection.
  EntityId SelectedEntity() const { return m_selectedEntity; }
  void SelectEntity(EntityId id) { m_selectedEntity = id; }

  // Access gizmo (for pick guard in main.cpp).
  GizmoController &GetGizmo() { return m_gizmo; }
  const GizmoController &GetGizmo() const { return m_gizmo; }

  CommandHistory &GetHistory() { return m_history; }

  // Grid editor state.
  bool IsGridEditorOpen() const { return m_gridEditorOpen; }
  void ToggleGridEditor() {
    m_gridEditorOpen = !m_gridEditorOpen;
    if (!m_gridEditorOpen) {
      // Discard any in-progress paint stroke to avoid stale state.
      m_paintStroke.clear();
      m_miniGridPainting = false;
      m_viewportPaintStroke.clear();
      m_viewportPainting = false;
    }
  }
  CommandHistory &GetGridHistory() { return m_gridHistory; }

  // ---- Grid editor viewport (Phase 5B) ----

  // Create procedural meshes for tile visualization. Call once at init.
  void InitEditorMeshes(DxContext &dx);

  // Build 3D render items from StageData into frame.opaqueItems.
  // Includes attack preview overlay for the selected tower.
  void BuildStageViewportItems(const StageData &stage, FrameData &frame) const;

  // Pick a tower by clicking near its cone in the viewport. Returns tower index or -1.
  int ViewportPickTower(const StageData &stage, int screenX, int screenY,
                        int screenW, int screenH,
                        const DirectX::XMMATRIX &view,
                        const DirectX::XMMATRIX &proj) const;

  // Selected tower index (for external read/write).
  int SelectedTower() const { return m_selectedTower; }
  void SelectTower(int idx) { m_selectedTower = idx; }

  // Play-test request flag — set by DrawGridEditorPanel, consumed by main.cpp.
  bool ConsumePlayTestRequest() {
    bool r = m_playTestRequested;
    m_playTestRequested = false;
    return r;
  }

  // Scene play mode request flag — set by DrawMenuBar, consumed by main.cpp (Phase 8).
  bool ConsumeScenePlayRequest() {
    bool r = m_scenePlayRequested;
    m_scenePlayRequested = false;
    return r;
  }

  // Viewport tile painting: apply brush to tile under cursor during drag.
  void HandleViewportTilePaint(StageData &stage, int screenX, int screenY,
                               int screenW, int screenH,
                               const DirectX::XMMATRIX &view,
                               const DirectX::XMMATRIX &proj);

  // Finalize viewport paint stroke on mouse release (push undo command).
  void FinalizeViewportPaintStroke(StageData &stage);

private:
  void DrawMenuBar(Scene &scene, DxContext &dx);
  void DrawEntityList(Scene &scene, DxContext &dx);
  void DrawInspector(Scene &scene, DxContext &dx);
  void DrawLightingPanel(Scene &scene, bool *iblEnabled);
  void DrawShadowPanel(Scene &scene, DxContext &dx);
  void DrawPostProcessPanel(Scene &scene, DxContext &dx);
  void DrawCameraPanel(Scene &scene, Camera &cam);
  void DrawGridEditorPanel(StageData &stage);
  void DrawAssetBrowser(Scene &scene, DxContext &dx);
  void ScanAssetDirectory();
  void ProcessHotkeys(Scene &scene, DxContext &dx);

  // Ray-plane intersection: screen pixel → tile coordinate on y=0 plane.
  bool ViewportRayToTile(int screenX, int screenY, int screenW, int screenH,
                         const DirectX::XMMATRIX &view,
                         const DirectX::XMMATRIX &proj,
                         const StageData &stage, int &outX, int &outY) const;

  EntityId m_selectedEntity = kInvalidEntityId;
  char m_scenePath[256] = "scene.json";

  CommandHistory m_history;
  GizmoController m_gizmo;

  // Drag coalescing for inspector transform DragFloats.
  bool m_transformDragActive = false;
  Transform m_transformDragStart;

  // Drag coalescing for inspector material edits (Phase 2).
  bool m_materialDragActive = false;
  Material m_materialDragStart;
  std::array<std::string, 6> m_materialPathsStart = {};

  // Drag coalescing for scene-global lighting panel (Phase 3).
  bool m_lightDragActive = false;
  SceneLightSettings m_lightDragStart;

  // Drag coalescing for per-entity light component inspector (Phase 3).
  bool m_plDragActive = false;
  PointLightComponent m_plDragStart;
  bool m_slDragActive = false;
  SpotLightComponent m_slDragStart;

  // Drag coalescing for shadow & post-process panels (Phase 4).
  bool m_shadowDragActive = false;
  SceneShadowSettings m_shadowDragStart;
  bool m_ppDragActive = false;
  ScenePostProcessSettings m_ppDragStart;

  // ---- Camera panel state (Phase 6) ----
  char m_presetName[64] = "Preset";

  // ---- Grid Editor state (Phase 5) ----
  bool m_gridEditorOpen = true;
  CommandHistory m_gridHistory;
  char m_stagePath[256] = "stage.json";

  // Brush state.
  int m_brushTileType = 0; // index into TileType enum
  bool m_brushWall = false;
  bool m_brushDestructible = false;

  // Drag coalescing for grid resize.
  bool m_resizeDragActive = false;
  StageData m_resizeDragStart;

  // Mini-grid selection.
  int m_selectedTileX = -1;
  int m_selectedTileY = -1;

  // Tower selection.
  int m_selectedTower = -1;

  // Drag coalescing for stage metadata sliders.
  bool m_metaDragActive = false;
  StageMetadata m_metaDragStart;

  // Drag coalescing for tower property sliders.
  bool m_towerDragActive = false;
  std::vector<TowerData> m_towerDragStart;

  // Mini-grid paint state (click-drag painting).
  bool m_miniGridPainting = false;
  std::vector<PaintTilesCommand::Entry> m_paintStroke;

  // ---- Viewport rendering + painting state (Phase 5B) ----
  EditorMeshIds m_editorMeshIds{};
  bool m_editorMeshesInitialized = false;
  bool m_viewportPainting = false;
  std::vector<PaintTilesCommand::Entry> m_viewportPaintStroke;

  // ---- Play-test request (Phase 5C) ----
  bool m_playTestRequested = false;

  // ---- Scene play mode request (Phase 8) ----
  bool m_scenePlayRequested = false;

  // ---- Asset Browser state (Phase 7) ----
  std::vector<AssetEntry> m_assetCache;
  bool m_assetCacheValid = false;
  int m_assetFilterType = 0; // 0=All, 1=Meshes, 2=Textures, 3=Scenes
  int m_selectedAssetIndex = -1;
  std::string m_previewTexturePath; // path of currently loaded preview
};
