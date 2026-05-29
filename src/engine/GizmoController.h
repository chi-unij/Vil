// ======================================
// File: GizmoController.h
// Purpose: Encapsulates ImGuizmo state for translate/rotate/scale gizmos.
//          Handles hotkeys, snap settings, and drag coalescing for undo.
//          Milestone 4 Phase 1: Geometry Tools.
// ======================================

#pragma once

#include "Entity.h"
#include "CommandHistory.h"

#include <DirectXMath.h>

class Scene;

class GizmoController {
public:
  enum class Operation { Translate, Rotate, Scale };
  enum class Space { World, Local };

  // Call once per frame after ImGui::NewFrame().
  // Returns true if the gizmo consumed the mouse (suppress mouse picking).
  bool Update(Scene &scene, EntityId selectedEntity,
              const DirectX::XMMATRIX &view, const DirectX::XMMATRIX &proj,
              CommandHistory &history);

  // Draw gizmo toolbar in an ImGui window.
  void DrawToolbar();

  // Process keyboard shortcuts (W/E/R for mode). Call when ImGui doesn't
  // want keyboard.
  void ProcessHotkeys();

  Operation GetOperation() const { return m_operation; }
  Space GetSpace() const { return m_space; }
  bool IsActive() const;

  // Snap settings (public for ImGui editing).
  bool snapEnabled = false;
  float snapTranslation = 1.0f;
  float snapRotation = 15.0f;
  float snapScale = 0.1f;

private:
  Operation m_operation = Operation::Translate;
  Space m_space = Space::World;

  // Drag coalescing for undo.
  bool m_isDragging = false;
  EntityId m_dragEntityId = kInvalidEntityId;
  Transform m_dragStartTransform;
};
