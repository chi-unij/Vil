// ======================================
// File: GizmoController.cpp
// Purpose: ImGuizmo integration for viewport transform gizmos.
// ======================================

#include "GizmoController.h"
#include "Commands.h"
#include "Scene.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include <cstring>

using namespace DirectX;

// Both DirectXMath and ImGuizmo use row-major layout:
// [right(0-3), up(4-7), dir(8-11), position(12-15)].
// No transpose needed — just store XMMATRIX to float[16] directly.
static void XMMatrixToFloat16(const XMMATRIX &m, float out[16]) {
  XMFLOAT4X4 tmp;
  XMStoreFloat4x4(&tmp, m);
  std::memcpy(out, &tmp, sizeof(float) * 16);
}

bool GizmoController::Update(Scene &scene, EntityId selectedEntity,
                              const XMMATRIX &view, const XMMATRIX &proj,
                              CommandHistory &history) {
  ImGuizmo::BeginFrame();

  ImGuiIO &io = ImGui::GetIO();
  ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);

  if (selectedEntity == kInvalidEntityId) {
    // If we were dragging an entity that got deselected, finalize.
    m_isDragging = false;
    return false;
  }

  Entity *entity = scene.FindEntity(selectedEntity);
  if (!entity)
    return false;

  // Capture transform BEFORE ImGuizmo modifies it (needed for undo).
  Transform preManipTransform = entity->transform;

  // Convert XMMATRIX to float[16] for ImGuizmo.
  float viewF[16], projF[16], worldF[16];
  XMMatrixToFloat16(view, viewF);
  XMMatrixToFloat16(proj, projF);
  XMMatrixToFloat16(entity->transform.WorldMatrix(), worldF);

  // Map operation enum to ImGuizmo enum.
  ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
  if (m_operation == Operation::Rotate)
    op = ImGuizmo::ROTATE;
  else if (m_operation == Operation::Scale)
    op = ImGuizmo::SCALE;

  ImGuizmo::MODE mode =
      (m_space == Space::Local) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

  // Snap values.
  float snapValues[3] = {0, 0, 0};
  if (snapEnabled) {
    if (m_operation == Operation::Translate)
      snapValues[0] = snapValues[1] = snapValues[2] = snapTranslation;
    else if (m_operation == Operation::Rotate)
      snapValues[0] = snapValues[1] = snapValues[2] = snapRotation;
    else if (m_operation == Operation::Scale)
      snapValues[0] = snapValues[1] = snapValues[2] = snapScale;
  }

  ImGuizmo::Manipulate(viewF, projF, op, mode, worldF, nullptr,
                        snapEnabled ? snapValues : nullptr);

  bool gizmoUsing = ImGuizmo::IsUsing();

  // If gizmo modified the matrix, decompose back into entity transform.
  if (gizmoUsing) {
    float translation[3], rotation[3], scale[3];
    ImGuizmo::DecomposeMatrixToComponents(worldF, translation, rotation,
                                          scale);
    entity->transform.position = {translation[0], translation[1],
                                  translation[2]};
    entity->transform.rotation = {rotation[0], rotation[1], rotation[2]};
    entity->transform.scale = {scale[0], scale[1], scale[2]};
  }

  // Drag coalescing for undo.
  if (!m_isDragging && gizmoUsing) {
    // Drag just started. preManipTransform holds the state BEFORE this frame's
    // modification.
    m_isDragging = true;
    m_dragEntityId = selectedEntity;
    m_dragStartTransform = preManipTransform;
  }
  if (m_isDragging && !gizmoUsing) {
    // Drag ended. Push undo command with before/after transforms.
    m_isDragging = false;
    if (Entity *dragEntity = scene.FindEntity(m_dragEntityId)) {
      auto cmd = std::make_unique<TransformCommand>(
          scene, m_dragEntityId, m_dragStartTransform, dragEntity->transform);
      history.PushWithoutExecute(std::move(cmd));
    }
  }

  return ImGuizmo::IsOver() || gizmoUsing;
}

bool GizmoController::IsActive() const { return ImGuizmo::IsUsing(); }

void GizmoController::ProcessHotkeys() {
  if (ImGui::IsKeyPressed(ImGuiKey_W, false))
    m_operation = Operation::Translate;
  if (ImGui::IsKeyPressed(ImGuiKey_E, false))
    m_operation = Operation::Rotate;
  if (ImGui::IsKeyPressed(ImGuiKey_R, false))
    m_operation = Operation::Scale;
}

void GizmoController::DrawToolbar() {
  ImGui::SetNextWindowPos(ImVec2(10, 440), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(220, 180), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Gizmo")) {
    ImGui::End();
    return;
  }

  // Operation radio buttons.
  int op = static_cast<int>(m_operation);
  ImGui::RadioButton("Translate (W)", &op, 0);
  ImGui::SameLine();
  ImGui::RadioButton("Rotate (E)", &op, 1);
  ImGui::SameLine();
  ImGui::RadioButton("Scale (R)", &op, 2);
  m_operation = static_cast<Operation>(op);

  // Space toggle (not for scale — always local).
  if (m_operation != Operation::Scale) {
    int sp = static_cast<int>(m_space);
    ImGui::RadioButton("World", &sp, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Local", &sp, 1);
    m_space = static_cast<Space>(sp);
  }

  // Snap settings.
  ImGui::Checkbox("Snap", &snapEnabled);
  if (snapEnabled) {
    if (m_operation == Operation::Translate)
      ImGui::DragFloat("Grid", &snapTranslation, 0.1f, 0.1f, 10.0f);
    else if (m_operation == Operation::Rotate)
      ImGui::DragFloat("Angle", &snapRotation, 1.0f, 1.0f, 90.0f);
    else if (m_operation == Operation::Scale)
      ImGui::DragFloat("Scale Step", &snapScale, 0.01f, 0.01f, 1.0f);
  }

  ImGui::End();
}
