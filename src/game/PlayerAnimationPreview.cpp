#include "game/PlayerAnimationPreview.h"

#include "GltfLoader.h"
#include "MeshRenderer.h"

#include <DirectXMath.h>
#include <Windows.h>
#include <algorithm>
#include <imgui.h>

using namespace DirectX;

void PlayerAnimationPreview::Initialize(DxContext &dx) {
  m_dx = &dx;

  GltfLoader playerLoader;
  if (!playerLoader.LoadModel("Assets/models/MyFirstChar.vrm")) {
    OutputDebugStringA("[PlayerPreview] FAILED: Assets/models/MyFirstChar.vrm\n");
    return;
  }

  const LoadedMesh &mesh = playerLoader.GetMesh();
  m_meshId = dx.CreateMeshResources(mesh, playerLoader.GetMaterialImages(),
                                    playerLoader.GetMaterial());
  m_ready = true;

  if (mesh.hasSkeleton) {
    m_skeleton = mesh.skeleton;
    m_hasSkeleton = true;

    BonePalette bindPose;
    ComputeBindPose(m_skeleton, bindPose);
    dx.GetMeshRenderer().SetBonePalette(m_meshId, bindPose);

    LoadClip("Idle", "Assets/models/animations/Idle.glb", "", m_clips[0]);
    LoadClip("Walk", "Assets/models/animations/Walk.glb",
             "Assets/models/animations/Push.glb", m_clips[1]);
    LoadClip("Run", "Assets/models/animations/Run.glb", "", m_clips[2]);

    OutputDebugStringA("[PlayerPreview] Player VRM and animation clips loaded.\n");
  } else {
    OutputDebugStringA("[PlayerPreview] WARNING: player model has no skeleton.\n");
  }
}

bool PlayerAnimationPreview::LoadClip(const std::string &label,
                                      const std::string &primaryPath,
                                      const std::string &fallbackPath,
                                      PreviewClip &outClip) {
  outClip = {};
  outClip.label = label;

  auto tryLoad = [&](const std::string &path, bool fallback) -> bool {
    if (path.empty())
      return false;

    std::vector<AnimationClip> loaded;
    if (!LoadAnimationFile(path, m_skeleton, loaded) || loaded.empty())
      return false;

    outClip.clipIndex = static_cast<int>(m_animations.size());
    outClip.loadedPath = path;
    outClip.loaded = true;
    outClip.fallback = fallback;
    for (auto &clip : loaded)
      m_animations.push_back(std::move(clip));
    return true;
  };

  if (tryLoad(primaryPath, false))
    return true;
  return tryLoad(fallbackPath, true);
}

int PlayerAnimationPreview::ActiveClipIndex() const {
  const int slot = static_cast<int>(m_activeSlot);
  if (slot < 0 || slot >= static_cast<int>(m_clips.size()))
    return -1;
  return m_clips[slot].clipIndex;
}

void PlayerAnimationPreview::Update(float dt) {
  if (!m_ready || !m_hasSkeleton)
    return;

  if (m_autoCycle) {
    m_cycleTimer += dt;
    if (m_cycleTimer >= 3.0f) {
      m_cycleTimer = 0.0f;
      int next = (static_cast<int>(m_activeSlot) + 1) % 3;
      m_activeSlot = static_cast<ClipSlot>(next);
      m_animTime = 0.0f;
    }
  }

  m_animTime += dt;

  BonePalette palette;
  const int clipIndex = ActiveClipIndex();
  if (clipIndex >= 0 && clipIndex < static_cast<int>(m_animations.size())) {
    EvaluateAnimation(m_skeleton, m_animations[clipIndex], m_animTime, palette);
  } else {
    ComputeProceduralIdle(m_skeleton, m_animTime, palette);
  }

  if (m_dx)
    m_dx->GetMeshRenderer().SetBonePalette(m_meshId, palette);
}

void PlayerAnimationPreview::BuildFrame(FrameData &frame) const {
  if (!m_ready)
    return;

  const XMMATRIX world =
      XMMatrixScaling(m_previewScale, m_previewScale, m_previewScale) *
      XMMatrixRotationY(m_previewYaw) *
      XMMatrixTranslation(0.0f, 0.01f, 0.0f);
  frame.opaqueItems.push_back({m_meshId, world});

  GPUPointLight light{};
  light.position = {0.0f, 1.1f, -1.5f};
  light.range = 4.0f;
  light.color = {0.9f, 0.8f, 0.65f};
  light.intensity = 1.6f;
  frame.pointLights.push_back(light);
}

void PlayerAnimationPreview::DrawDebugUi() {
  if (!ImGui::Begin("CHI-35 Player Animation Preview")) {
    ImGui::End();
    return;
  }

  ImGui::Text("Player: Assets/models/MyFirstChar.vrm");
  ImGui::Text("Skeleton: %s", m_hasSkeleton ? "OK" : "NG");
  ImGui::Separator();

  for (int i = 0; i < 3; ++i) {
    PreviewClip &clip = m_clips[i];
    const bool selected = static_cast<int>(m_activeSlot) == i;
    std::string buttonLabel = clip.label;
    if (clip.fallback)
      buttonLabel += " (fallback)";
    if (ImGui::RadioButton(buttonLabel.c_str(), selected)) {
      m_activeSlot = static_cast<ClipSlot>(i);
      m_animTime = 0.0f;
      m_cycleTimer = 0.0f;
    }

    ImGui::SameLine();
    if (clip.loaded) {
      ImGui::TextColored(ImVec4(0.25f, 1.0f, 0.35f, 1.0f), "OK");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", clip.loadedPath.c_str());
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "Missing");
    }
  }

  ImGui::Checkbox("Auto cycle", &m_autoCycle);
  ImGui::SliderFloat("Yaw", &m_previewYaw, -3.14159265f, 3.14159265f);
  ImGui::SliderFloat("Scale", &m_previewScale, 0.2f, 2.0f);

  ImGui::End();
}
