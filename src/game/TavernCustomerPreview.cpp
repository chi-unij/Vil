#include "game/TavernCustomerPreview.h"

#include "GltfLoader.h"
#include "MeshRenderer.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

using namespace DirectX;

namespace {

constexpr float kTargetCustomerHeightMeters = 1.62f;
constexpr float kTransitionDurationSeconds = 0.16f;

bool MatrixIsFinite(const XMMATRIX &matrix) {
  XMFLOAT4X4 stored{};
  XMStoreFloat4x4(&stored, matrix);
  const float *values = &stored._11;
  for (int element = 0; element < 16; ++element) {
    if (!std::isfinite(values[element]))
      return false;
  }
  return true;
}

} // namespace

void TavernCustomerPreview::Initialize(DxContext &dx,
                                       const std::string &modelLabel,
                                       const std::string &modelPath) {
  m_dx = &dx;
  m_modelLabel = modelLabel;
  m_modelPath = modelPath;
  GltfLoader loader;
  if (!loader.LoadModel(modelPath)) {
    const std::string message = "[TavernCustomer] " + modelPath +
                                " unavailable; model roster incomplete.\n";
    OutputDebugStringA(message.c_str());
    return;
  }

  const LoadedMesh &mesh = loader.GetMesh();
  const std::vector<LoadedMeshPart> &parts = loader.GetMeshParts();
  m_opaqueMeshIds.clear();
  m_transparentMeshIds.clear();
  m_opaqueMeshIds.reserve(parts.size());
  m_transparentMeshIds.reserve(parts.size());
  for (const LoadedMeshPart &part : parts) {
    Material material = part.material;
    // FBX の Phong material を PBR へ推測変換すると character skin / cloth が
    // metallic になるため、customer 用には dielectric として固定する。
    material.metallicFactor = 0.0f;
    material.roughnessFactor = part.alphaBlend ? 0.56f : 0.72f;
    // Mixamo character の hair alpha factor が 0 で出力される converter case。
    // texture alpha は保持し、factor だけを可視値へ戻す。
    if (part.alphaBlend && !part.baseColorImage.pixels.empty() &&
        material.baseColorFactor.w <= 0.001f) {
      material.baseColorFactor.w = 1.0f;
    }

    const uint32_t meshId =
        dx.CreateMeshResources(part.mesh, part.GetMaterialImages(), material);
    if (meshId == UINT32_MAX)
      continue;
    if (part.alphaBlend)
      m_transparentMeshIds.push_back(meshId);
    else
      m_opaqueMeshIds.push_back(meshId);
  }

  if (m_opaqueMeshIds.empty() && m_transparentMeshIds.empty()) {
    Material material = loader.GetMaterial();
    material.metallicFactor = 0.0f;
    const uint32_t meshId =
        dx.CreateMeshResources(mesh, loader.GetMaterialImages(), material);
    if (meshId != UINT32_MAX)
      m_opaqueMeshIds.push_back(meshId);
  }

  const size_t uploadedPartCount =
      m_opaqueMeshIds.size() + m_transparentMeshIds.size();
  m_ready = uploadedPartCount > 0 &&
            (parts.empty() || uploadedPartCount == parts.size());

  float minY = (std::numeric_limits<float>::max)();
  float maxY = (std::numeric_limits<float>::lowest)();
  for (const MeshVertex &vertex : mesh.vertices) {
    minY = std::min(minY, vertex.pos[1]);
    maxY = std::max(maxY, vertex.pos[1]);
  }
  if (minY <= maxY) {
    m_nativeModelMinY = minY;
    m_nativeModelHeight = maxY - minY;
  }
  if (m_nativeModelHeight > 0.0001f)
    m_modelToWorldScale = kTargetCustomerHeightMeters / m_nativeModelHeight;

  if (!mesh.hasSkeleton) {
    const std::string message =
        "[TavernCustomer] WARNING: " + modelPath + " has no skeleton.\n";
    OutputDebugStringA(message.c_str());
    return;
  }

  m_skeleton = mesh.skeleton;
  m_hasSkeleton = true;
  LoadClip("Idle", "Assets/models/animations/Idle_npc1.glb", m_idleClip);
  LoadClip("Walk", "Assets/models/animations/Walk_npc1.glb", m_walkClip);
  LoadClip("Sitting Idle",
           "Assets/models/animations/Sitting_Idle_npc2.glb",
           m_sittingClip);

  BonePalette bindPose;
  ComputeBindPose(m_skeleton, bindPose);
  UploadBonePalette(bindPose);
  const std::string message =
      "[TavernCustomer] " + modelLabel +
      (m_ready && m_idleClip.loaded && m_walkClip.loaded &&
               m_sittingClip.loaded
           ? " reuses shared Idle / Walk / Sitting Idle.\n"
           : " shared animation set incomplete.\n");
  OutputDebugStringA(message.c_str());
}

bool TavernCustomerPreview::LoadClip(const std::string &label,
                                     const std::string &path,
                                     PreviewClip &outClip) {
  outClip = {};
  outClip.label = label;
  outClip.sourcePath = path;
  std::vector<AnimationClip> loaded;
  if (!LoadAnimationFile(path, m_skeleton, loaded) || loaded.empty())
    return false;

  outClip.clipIndex = static_cast<int>(m_animations.size());
  outClip.loaded = true;
  for (AnimationClip &clip : loaded)
    m_animations.push_back(std::move(clip));
  return true;
}

void TavernCustomerPreview::InitializeAnimationState(
    AnimationState &state) const {
  state = {};
  if (!m_ready || !m_hasSkeleton)
    return;
  ComputeBindPose(m_skeleton, state.palette);
  state.paletteFinite = PaletteFinite(state.palette);
  state.initialized = state.palette.boneCount > 0 && state.paletteFinite;
}

int TavernCustomerPreview::ActiveClipIndex(AnimationMode mode) const {
  switch (mode) {
  case AnimationMode::Walk:
    return m_walkClip.clipIndex;
  case AnimationMode::Sitting:
    return m_sittingClip.clipIndex;
  case AnimationMode::Idle:
  default:
    return m_idleClip.clipIndex;
  }
}

void TavernCustomerPreview::SelectAnimationMode(AnimationState &state,
                                                AnimationMode mode) const {
  if (mode == state.mode)
    return;

  const int previousClipIndex = ActiveClipIndex(state.mode);
  state.previousMode = state.mode;
  state.previousAnimationTime = state.animationTime;
  state.mode = mode;
  state.animationTime = 0.0f;
  state.transitionElapsed = 0.0f;
  const int nextClipIndex = ActiveClipIndex(state.mode);
  state.transitioning = previousClipIndex >= 0 && nextClipIndex >= 0;
}

void TavernCustomerPreview::Update(AnimationState &state, float deltaSeconds,
                                   AnimationMode mode) {
  if (!m_ready || !m_hasSkeleton)
    return;
  if (!state.initialized)
    InitializeAnimationState(state);
  if (!state.initialized) {
    m_bonePaletteFinite = false;
    return;
  }

  SelectAnimationMode(state, mode);
  const float dt =
      std::isfinite(deltaSeconds) ? std::max(0.0f, deltaSeconds) : 0.0f;
  state.animationTime += dt;
  if (state.mode == AnimationMode::Walk)
    ++m_walkPoseUpdateCount;
  else if (state.mode == AnimationMode::Sitting)
    ++m_sittingPoseUpdateCount;
  else
    ++m_idlePoseUpdateCount;

  const int activeClipIndex = ActiveClipIndex(state.mode);
  const int previousClipIndex = ActiveClipIndex(state.previousMode);
  if (state.transitioning && previousClipIndex >= 0 &&
      activeClipIndex >= 0) {
    state.previousAnimationTime += dt;
    state.transitionElapsed += dt;
    const float linearBlend = std::clamp(
        state.transitionElapsed / kTransitionDurationSeconds, 0.0f, 1.0f);
    const float smoothBlend =
        linearBlend * linearBlend * (3.0f - 2.0f * linearBlend);
    EvaluateAnimationBlend(
        m_skeleton, m_animations[previousClipIndex],
        state.previousAnimationTime, m_animations[activeClipIndex],
        state.animationTime, smoothBlend, state.palette);
    if (linearBlend >= 1.0f)
      state.transitioning = false;
  } else if (activeClipIndex >= 0 &&
             activeClipIndex < static_cast<int>(m_animations.size())) {
    EvaluateAnimation(m_skeleton, m_animations[activeClipIndex],
                      state.animationTime, state.palette);
  } else {
    ComputeProceduralIdle(m_skeleton, state.animationTime, state.palette);
  }
  state.paletteFinite = PaletteFinite(state.palette);
  m_bonePaletteFinite = m_bonePaletteFinite && state.paletteFinite;
}

bool TavernCustomerPreview::PaletteFinite(const BonePalette &palette) {
  const int matrixCount = std::clamp(palette.boneCount, 0, kMaxBones);
  for (int boneIndex = 0; boneIndex < matrixCount; ++boneIndex) {
    if (!MatrixIsFinite(palette.matrices[boneIndex]))
      return false;
  }
  return true;
}

void TavernCustomerPreview::UploadBonePalette(const BonePalette &palette) {
  m_bonePaletteFinite = PaletteFinite(palette);
  if (!m_dx)
    return;
  for (const uint32_t meshId : m_opaqueMeshIds)
    m_dx->GetMeshRenderer().SetBonePalette(meshId, palette);
  for (const uint32_t meshId : m_transparentMeshIds)
    m_dx->GetMeshRenderer().SetBonePalette(meshId, palette);
}

void TavernCustomerPreview::BuildFrame(FrameData &frame,
                                       const XMFLOAT3 &position,
                                       float yawRadians,
                                       const AnimationState &state) const {
  if (!m_ready || !state.initialized || !state.paletteFinite)
    return;
  const float groundOffset = -m_nativeModelMinY * m_modelToWorldScale;
  const XMMATRIX world =
      XMMatrixScaling(m_modelToWorldScale, m_modelToWorldScale,
                      m_modelToWorldScale) *
      XMMatrixRotationY(yawRadians) *
      XMMatrixTranslation(position.x, position.y + groundOffset + 0.01f,
                          position.z);
  for (const uint32_t meshId : m_opaqueMeshIds)
    frame.opaqueItems.push_back({meshId, world, &state.palette});
  for (const uint32_t meshId : m_transparentMeshIds)
    frame.transparentItems.push_back({meshId, world, &state.palette});
}

TavernCustomerPreview::ClipDiagnostics
TavernCustomerPreview::BuildClipDiagnostics(const PreviewClip &clip) const {
  ClipDiagnostics diagnostics{};
  diagnostics.label = clip.label;
  diagnostics.sourcePath = clip.sourcePath;
  diagnostics.loaded = clip.loaded;
  if (clip.clipIndex >= 0 &&
      clip.clipIndex < static_cast<int>(m_animations.size())) {
    diagnostics.duration = m_animations[clip.clipIndex].duration;
    diagnostics.trackCount = m_animations[clip.clipIndex].tracks.size();
  }
  return diagnostics;
}

TavernCustomerPreview::ClipDiagnostics
TavernCustomerPreview::IdleDiagnostics() const {
  return BuildClipDiagnostics(m_idleClip);
}

TavernCustomerPreview::ClipDiagnostics
TavernCustomerPreview::WalkDiagnostics() const {
  return BuildClipDiagnostics(m_walkClip);
}

TavernCustomerPreview::ClipDiagnostics
TavernCustomerPreview::SittingDiagnostics() const {
  return BuildClipDiagnostics(m_sittingClip);
}
