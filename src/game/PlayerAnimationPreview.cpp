#include "game/PlayerAnimationPreview.h"

#include "GltfLoader.h"
#include "Input.h"
#include "MeshRenderer.h"

#include <DirectXMath.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <imgui.h>

using namespace DirectX;

namespace {

bool IsMovingLocomotion(PlayerAnimationPreview::ClipSlot slot) {
  return slot == PlayerAnimationPreview::ClipSlot::Walk ||
         slot == PlayerAnimationPreview::ClipSlot::Run;
}

float SmoothStep01(float value) {
  const float t = std::clamp(value, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

} // namespace

void PlayerAnimationPreview::Initialize(DxContext &dx) {
  m_dx = &dx;

  GltfLoader playerLoader;
  if (!playerLoader.LoadModel("Assets/models/MyFirstChar.vrm")) {
    OutputDebugStringA("[PlayerPreview] FAILED: Assets/models/MyFirstChar.vrm\n");
    return;
  }

  const LoadedMesh &mesh = playerLoader.GetMesh();
  const auto &meshParts = playerLoader.GetMeshParts();
  m_opaqueMeshIds.clear();
  m_transparentMeshIds.clear();
  m_opaqueMeshIds.reserve(meshParts.size());
  m_transparentMeshIds.reserve(meshParts.size());
  m_doubleSidedMaterialPartCount = 0;
  for (const LoadedMeshPart &part : meshParts) {
    const uint32_t meshId =
        dx.CreateMeshResources(part.mesh, part.GetMaterialImages(),
                               part.material);
    if (meshId == UINT32_MAX)
      continue;
    if (part.alphaBlend)
      m_transparentMeshIds.push_back(meshId);
    else
      m_opaqueMeshIds.push_back(meshId);
    if (part.doubleSided)
      ++m_doubleSidedMaterialPartCount;
  }

  // 旧形式または単一 part asset との互換 fallback。
  if (m_opaqueMeshIds.empty() && m_transparentMeshIds.empty()) {
    const uint32_t meshId =
        dx.CreateMeshResources(mesh, playerLoader.GetMaterialImages(),
                               playerLoader.GetMaterial());
    if (meshId != UINT32_MAX)
      m_opaqueMeshIds.push_back(meshId);
  }
  const size_t uploadedPartCount =
      m_opaqueMeshIds.size() + m_transparentMeshIds.size();
  m_ready = uploadedPartCount > 0 &&
            (meshParts.empty() || uploadedPartCount == meshParts.size());

  float minY = std::numeric_limits<float>::max();
  float maxY = std::numeric_limits<float>::lowest();
  for (const MeshVertex &vertex : mesh.vertices) {
    minY = std::min(minY, vertex.pos[1]);
    maxY = std::max(maxY, vertex.pos[1]);
  }
  if (minY <= maxY) {
    m_nativeModelMinY = minY;
    m_nativeModelHeight = maxY - minY;
  }
  // 全 Scene で同じ見た目の高さを使い、家具とボス演出に対する比率を安定させる。
  constexpr float kTargetPlayerHeightMeters = 1.60f;
  if (m_nativeModelHeight > 0.0001f)
    m_modelToWorldScale = kTargetPlayerHeightMeters / m_nativeModelHeight;

  if (mesh.hasSkeleton) {
    m_skeleton = mesh.skeleton;
    m_hasSkeleton = true;

    BonePalette bindPose;
    ComputeBindPose(m_skeleton, bindPose);
    UploadBonePalette(bindPose);

    LoadClip("Idle", "Assets/models/animations/Idle.glb", "", m_clips[0]);
    LoadClip("Walk", "Assets/models/animations/Walk.glb", "", m_clips[1]);
    LoadClip("Run", "Assets/models/animations/Run.glb", "", m_clips[2]);
    LoadClip("Taking Item", "Assets/models/animations/TakingItem.glb", "",
             m_takingItemClip);

    // Walk clip がない場合も Push action は流用せず、Run を低速再生して
    // locomotion の意味を維持する。Walk asset 追加後は自動的に置き換わる。
    if (!m_clips[1].loaded && m_clips[2].loaded) {
      m_clips[1].clipIndex = m_clips[2].clipIndex;
      m_clips[1].loadedPath =
          "Assets/models/animations/Run.glb (0.62x Walk fallback)";
      m_clips[1].loaded = true;
      m_clips[1].fallback = true;
      m_clipPlaybackRates[1] = 0.62f;
    }

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
  return ClipIndex(m_activeSlot);
}

int PlayerAnimationPreview::ClipIndex(ClipSlot clipSlot) const {
  const int slot = static_cast<int>(clipSlot);
  if (slot < 0 || slot >= static_cast<int>(m_clips.size()))
    return -1;
  return m_clips[slot].clipIndex;
}

float PlayerAnimationPreview::ClipDuration(ClipSlot clipSlot) const {
  const int clipIndex = ClipIndex(clipSlot);
  if (clipIndex < 0 || clipIndex >= static_cast<int>(m_animations.size()))
    return 0.0f;
  return m_animations[clipIndex].duration;
}

float PlayerAnimationPreview::ClipPlaybackRate(ClipSlot clipSlot) const {
  const int slot = static_cast<int>(clipSlot);
  if (slot < 0 || slot >= static_cast<int>(m_clipPlaybackRates.size()))
    return 1.0f;
  return m_clipPlaybackRates[slot];
}

void PlayerAnimationPreview::SelectLocomotionClip(ClipSlot slot) {
  m_manualPreview = true;
  CancelAction();
  TransitionToClip(slot);
}

bool PlayerAnimationPreview::PlayTakingItem() {
  if (!m_ready || !m_hasSkeleton || !m_takingItemClip.loaded ||
      m_takingItemClip.clipIndex < 0 ||
      m_takingItemClip.clipIndex >= static_cast<int>(m_animations.size())) {
    return false;
  }

  const AnimationClip &actionClip = m_animations[m_takingItemClip.clipIndex];
  if (!std::isfinite(actionClip.duration) || actionClip.duration <= 0.0001f ||
      !std::isfinite(m_takingItemPlaybackRate) ||
      m_takingItemPlaybackRate <= 0.0f ||
      !std::isfinite(m_takingItemStartTime)) {
    return false;
  }

  ++m_actionTriggerSerial;
  if (m_actionActive) {
    // 連続取得では現在 pose から clip 先頭へ crossfade し、各取得を再生する。
    m_actionBaseUsesAction = true;
    m_actionBaseTime = m_actionTime;
    m_actionTime = m_takingItemStartTime;
    m_actionElapsed = 0.0f;
    m_actionBlendOutStarted = false;
    SetActionReturnClip(m_actionDesiredReturnSlot);
    return true;
  }

  m_actionActive = true;
  m_actionTime = m_takingItemStartTime;
  m_actionElapsed = 0.0f;
  const bool previousPoseDominant =
      m_transitioning && m_transitionElapsed < m_transitionDuration * 0.5f &&
      ClipIndex(m_previousSlot) >= 0;
  m_actionBaseSlot = previousPoseDominant ? m_previousSlot : m_activeSlot;
  m_actionBaseTime = previousPoseDominant ? m_previousAnimTime : m_animTime;
  m_actionBaseUsesAction = false;
  m_actionReturnSlot = m_activeSlot;
  m_actionReturnTime = m_animTime;
  m_actionDesiredReturnSlot = m_activeSlot;
  m_actionBlendOutStarted = false;

  // Locomotion crossfade の行き先を action の基準 pose として確定する。
  m_transitioning = false;
  m_hasQueuedTransition = false;
  return true;
}

void PlayerAnimationPreview::CancelAction() {
  if (!m_actionActive)
    return;

  m_actionActive = false;
  m_activeSlot = m_actionReturnSlot;
  m_previousSlot = m_activeSlot;
  m_animTime = m_actionReturnTime;
  m_previousAnimTime = m_animTime;
  m_transitionElapsed = 0.0f;
  m_transitioning = false;
  m_hasQueuedTransition = false;
  m_actionBaseUsesAction = false;
  m_actionBlendOutStarted = false;

  BonePalette returnPalette;
  const int returnClipIndex = ActiveClipIndex();
  if (returnClipIndex >= 0 &&
      returnClipIndex < static_cast<int>(m_animations.size())) {
    EvaluateAnimation(m_skeleton, m_animations[returnClipIndex], m_animTime,
                      returnPalette);
  } else {
    ComputeProceduralIdle(m_skeleton, m_animTime, returnPalette);
  }
  UploadBonePalette(returnPalette);
}

void PlayerAnimationPreview::SetActionReturnClip(ClipSlot slot) {
  if (slot == m_actionReturnSlot)
    return;

  float normalizedPhase = 0.0f;
  const float previousDuration = ClipDuration(m_actionReturnSlot);
  const float nextDuration = ClipDuration(slot);
  if (IsMovingLocomotion(m_actionReturnSlot) && IsMovingLocomotion(slot) &&
      previousDuration > 0.0001f) {
    normalizedPhase =
        std::fmod(m_actionReturnTime, previousDuration) / previousDuration;
  }
  m_actionReturnSlot = slot;
  m_actionReturnTime =
      nextDuration > 0.0001f ? normalizedPhase * nextDuration : 0.0f;
}

void PlayerAnimationPreview::RequestLocomotionClip(ClipSlot slot) {
  if (!m_actionActive) {
    TransitionToClip(slot);
    return;
  }

  m_actionDesiredReturnSlot = slot;
  if (!m_actionBlendOutStarted)
    SetActionReturnClip(slot);
}

void PlayerAnimationPreview::TransitionToClip(ClipSlot slot) {
  if (slot == m_activeSlot) {
    m_hasQueuedTransition = false;
    return;
  }

  if (m_transitioning) {
    // 直前の clip へ戻る場合は crossfade を反転し、現在 pose の連続性を保つ。
    if (slot == m_previousSlot) {
      std::swap(m_activeSlot, m_previousSlot);
      std::swap(m_animTime, m_previousAnimTime);
      m_transitionElapsed = std::clamp(
          m_transitionDuration - m_transitionElapsed, 0.0f,
          m_transitionDuration);
      m_hasQueuedTransition = false;
      return;
    }

    // 第三の clip は現在の crossfade 完了後に開始し、純 clip への跳ねを防ぐ。
    m_queuedSlot = slot;
    m_hasQueuedTransition = true;
    return;
  }

  const float previousDuration = ClipDuration(m_activeSlot);
  const float nextDuration = ClipDuration(slot);
  float normalizedPhase = 0.0f;
  // Walk / Run 間だけ歩容 phase を同期する。Idle からは clip の先頭で開始する。
  const bool synchronizeLocomotionPhase =
      IsMovingLocomotion(m_activeSlot) && IsMovingLocomotion(slot);
  if (synchronizeLocomotionPhase && previousDuration > 0.0001f)
    normalizedPhase = std::fmod(m_animTime, previousDuration) / previousDuration;

  m_previousSlot = m_activeSlot;
  m_previousAnimTime = m_animTime;
  m_activeSlot = slot;
  m_animTime = nextDuration > 0.0001f ? normalizedPhase * nextDuration : 0.0f;
  m_transitionElapsed = 0.0f;
  m_transitioning = ClipIndex(m_previousSlot) >= 0 &&
                    ClipIndex(m_activeSlot) >= 0 &&
                    m_transitionDuration > 0.0001f;
  m_hasQueuedTransition = false;
}

void PlayerAnimationPreview::UploadBonePalette(const BonePalette &palette) {
  m_bonePaletteFinite = true;
  const int matrixCount = std::clamp(palette.boneCount, 0, kMaxBones);
  for (int i = 0; i < matrixCount && m_bonePaletteFinite; ++i) {
    XMFLOAT4X4 matrix{};
    XMStoreFloat4x4(&matrix, palette.matrices[i]);
    const float *values = &matrix._11;
    for (int element = 0; element < 16; ++element) {
      if (!std::isfinite(values[element])) {
        m_bonePaletteFinite = false;
        break;
      }
    }
  }

  if (!m_dx)
    return;
  for (const uint32_t meshId : m_opaqueMeshIds)
    m_dx->GetMeshRenderer().SetBonePalette(meshId, palette);
  for (const uint32_t meshId : m_transparentMeshIds)
    m_dx->GetMeshRenderer().SetBonePalette(meshId, palette);
}

void PlayerAnimationPreview::UpdateAnimationPose(float dt) {
  if (!m_ready || !m_hasSkeleton)
    return;

  const float safeDt = std::isfinite(dt) ? std::max(0.0f, dt) : 0.0f;
  if (m_actionActive) {
    const int actionClipIndex = m_takingItemClip.clipIndex;
    const bool actionClipValid =
        actionClipIndex >= 0 &&
        actionClipIndex < static_cast<int>(m_animations.size()) &&
        std::isfinite(m_animations[actionClipIndex].duration) &&
        m_animations[actionClipIndex].duration > 0.0001f;
    if (actionClipValid) {
      const AnimationClip &actionClip = m_animations[actionClipIndex];
      const float playbackRate = std::max(0.01f, m_takingItemPlaybackRate);
      const float actionEndTime = std::max(0.0f, actionClip.duration - 0.0001f);
      const float actionStartTime =
          std::clamp(m_takingItemStartTime, 0.0f, actionEndTime);
      const float wallDuration = std::max(
          0.0001f, (actionClip.duration - actionStartTime) / playbackRate);
      m_actionElapsed += safeDt;
      m_actionTime = std::min(actionStartTime + m_actionElapsed * playbackRate,
                              actionEndTime);
      if (m_actionBaseUsesAction) {
        m_actionBaseTime =
            std::min(m_actionBaseTime + safeDt * playbackRate, actionEndTime);
      } else {
        m_actionBaseTime += safeDt * ClipPlaybackRate(m_actionBaseSlot);
      }
      m_actionReturnTime += safeDt * ClipPlaybackRate(m_actionReturnSlot);

      const float blendDuration =
          std::min(m_transitionDuration, wallDuration * 0.25f);
      if (m_actionElapsed >= wallDuration - blendDuration)
        m_actionBlendOutStarted = true;

      if (m_actionElapsed >= wallDuration) {
        const ClipSlot desiredReturnSlot = m_actionDesiredReturnSlot;
        CancelAction();
        if (desiredReturnSlot != m_activeSlot)
          TransitionToClip(desiredReturnSlot);
        return;
      }

      BonePalette actionPalette;
      const int baseClipIndex = m_actionBaseUsesAction
                                    ? actionClipIndex
                                    : ClipIndex(m_actionBaseSlot);
      const int returnClipIndex = ClipIndex(m_actionReturnSlot);
      if (blendDuration > 0.0001f && m_actionElapsed < blendDuration &&
          baseClipIndex >= 0 &&
          baseClipIndex < static_cast<int>(m_animations.size())) {
        EvaluateAnimationBlend(m_skeleton, m_animations[baseClipIndex],
                               m_actionBaseTime, actionClip, m_actionTime,
                               SmoothStep01(m_actionElapsed / blendDuration),
                               actionPalette);
      } else if (blendDuration > 0.0001f &&
                 m_actionElapsed > wallDuration - blendDuration &&
                 returnClipIndex >= 0 &&
                 returnClipIndex < static_cast<int>(m_animations.size())) {
        const float blendOut =
            (m_actionElapsed - (wallDuration - blendDuration)) / blendDuration;
        EvaluateAnimationBlend(
            m_skeleton, actionClip, m_actionTime, m_animations[returnClipIndex],
            m_actionReturnTime, SmoothStep01(blendOut), actionPalette);
      } else {
        EvaluateAnimation(m_skeleton, actionClip, m_actionTime, actionPalette);
      }

      UploadBonePalette(actionPalette);
      return;
    }

    CancelAction();
  }

  m_animTime += safeDt * ClipPlaybackRate(m_activeSlot);

  BonePalette palette;
  const int activeClipIndex = ActiveClipIndex();
  const int previousClipIndex = ClipIndex(m_previousSlot);
  if (m_transitioning && previousClipIndex >= 0 && activeClipIndex >= 0) {
    m_previousAnimTime += safeDt * ClipPlaybackRate(m_previousSlot);
    m_transitionElapsed += safeDt;
    const float linearBlend =
        std::clamp(m_transitionElapsed / m_transitionDuration, 0.0f, 1.0f);
    const float smoothBlend =
        linearBlend * linearBlend * (3.0f - 2.0f * linearBlend);
    EvaluateAnimationBlend(m_skeleton, m_animations[previousClipIndex],
                           m_previousAnimTime, m_animations[activeClipIndex],
                           m_animTime, smoothBlend, palette);
    if (linearBlend >= 1.0f) {
      m_transitioning = false;
      if (m_hasQueuedTransition) {
        const ClipSlot queuedSlot = m_queuedSlot;
        m_hasQueuedTransition = false;
        TransitionToClip(queuedSlot);
      }
    }
  } else if (activeClipIndex >= 0 &&
             activeClipIndex < static_cast<int>(m_animations.size())) {
    EvaluateAnimation(m_skeleton, m_animations[activeClipIndex], m_animTime,
                      palette);
  } else {
    ComputeProceduralIdle(m_skeleton, m_animTime, palette);
  }

  UploadBonePalette(palette);
}

void PlayerAnimationPreview::Update(float dt) {
  if (!m_ready || !m_hasSkeleton)
    return;

  m_lastMovementSpeed = 0.0f;
  if (m_autoCycle) {
    m_manualPreview = true;
    m_cycleTimer += std::max(0.0f, dt);
    if (m_cycleTimer >= 3.0f) {
      m_cycleTimer = 0.0f;
      const int next = (static_cast<int>(m_activeSlot) + 1) % 3;
      RequestLocomotionClip(static_cast<ClipSlot>(next));
    }
  } else if (!m_manualPreview) {
    RequestLocomotionClip(ClipSlot::Idle);
  }

  UpdateAnimationPose(dt);
}

void PlayerAnimationPreview::UpdateFacing(float targetYaw, float dt) {
  const float delta = std::remainder(targetYaw - m_previewYaw, XM_2PI);
  const float turnBlend = 1.0f - std::exp(-14.0f * std::max(0.0f, dt));
  m_previewYaw += delta * turnBlend;
  if (m_previewYaw > XM_PI)
    m_previewYaw -= XM_2PI;
  else if (m_previewYaw < -XM_PI)
    m_previewYaw += XM_2PI;
}

PlayerAnimationPreview::ClipDiagnostics
PlayerAnimationPreview::BuildClipDiagnostics(const PreviewClip &clip) const {
  ClipDiagnostics diagnostics{};
  diagnostics.label = clip.label;
  diagnostics.sourcePath = clip.loadedPath;
  diagnostics.loaded = clip.loaded;
  diagnostics.fallback = clip.fallback;
  const int clipIndex = clip.clipIndex;
  if (clipIndex >= 0 && clipIndex < static_cast<int>(m_animations.size())) {
    diagnostics.duration = m_animations[clipIndex].duration;
    diagnostics.trackCount = m_animations[clipIndex].tracks.size();
  }
  return diagnostics;
}

PlayerAnimationPreview::ClipDiagnostics
PlayerAnimationPreview::GetClipDiagnostics(ClipSlot slot) const {
  const int slotIndex = static_cast<int>(slot);
  if (slotIndex < 0 || slotIndex >= static_cast<int>(m_clips.size()))
    return {};
  return BuildClipDiagnostics(m_clips[slotIndex]);
}

PlayerAnimationPreview::ClipDiagnostics
PlayerAnimationPreview::GetTakingItemDiagnostics() const {
  return BuildClipDiagnostics(m_takingItemClip);
}

float PlayerAnimationPreview::WorldModelHeight() const {
  return m_nativeModelHeight * m_modelToWorldScale * m_previewScale;
}

void PlayerAnimationPreview::Update(float dt, const Input &input,
                                    float worldHalfExtentMeters,
                                    const std::vector<CollisionSystem::Collider>
                                        &colliders,
                                    const std::vector<
                                        CollisionSystem::MeshTriangle>
                                        &meshTriangles,
                                    bool debugFly, float movementYawRadians) {
  if (!m_ready) {
    Update(dt);
    return;
  }

  float moveX = 0.0f;
  float moveY = 0.0f;
  float moveZ = 0.0f;
  if (input.IsKeyDown('W') || input.IsKeyDown(VK_UP))
    moveZ += 1.0f;
  if (input.IsKeyDown('S') || input.IsKeyDown(VK_DOWN))
    moveZ -= 1.0f;
  if (input.IsKeyDown('D') || input.IsKeyDown(VK_RIGHT))
    moveX += 1.0f;
  if (input.IsKeyDown('A') || input.IsKeyDown(VK_LEFT))
    moveX -= 1.0f;
  if (debugFly && input.IsKeyDown('E'))
    moveY += 1.0f;
  if (debugFly && input.IsKeyDown('Q'))
    moveY -= 1.0f;

  moveX += input.LeftStickX();
  moveZ += input.LeftStickY();

  const float moveLenSq = moveX * moveX + moveY * moveY + moveZ * moveZ;
  const bool isMoving = moveLenSq > 0.0001f;
  if (isMoving) {
    const float rawLength = std::sqrt(moveLenSq);
    const float inputStrength = std::min(1.0f, rawLength);
    const float invLen = 1.0f / rawLength;
    moveX *= invLen;
    moveY *= invLen;
    moveZ *= invLen;

    const float localMoveX = moveX;
    const float localMoveZ = moveZ;
    const float movementCos = std::cos(movementYawRadians);
    const float movementSin = std::sin(movementYawRadians);
    moveX = localMoveX * movementCos + localMoveZ * movementSin;
    moveZ = localMoveZ * movementCos - localMoveX * movementSin;

    const bool running = input.IsKeyDown(VK_SHIFT) ||
                         input.IsGamepadButtonDown(XINPUT_GAMEPAD_A);
    const float moveSpeed = running ? 6.0f : 3.2f;
    const XMFLOAT3 previousPosition = m_previewPosition;
    XMFLOAT3 desiredPosition = m_previewPosition;
    desiredPosition.x += moveX * moveSpeed * inputStrength * dt;
    desiredPosition.y += moveY * moveSpeed * inputStrength * dt;
    desiredPosition.z += moveZ * moveSpeed * inputStrength * dt;

    if (debugFly) {
      const float bounds = std::max(0.0f, worldHalfExtentMeters);
      desiredPosition.x = std::clamp(desiredPosition.x, -bounds, bounds);
      desiredPosition.z = std::clamp(desiredPosition.z, -bounds, bounds);
      m_previewPosition = desiredPosition;
    } else {
      CollisionSystem::Capsule capsule{};
      m_previewPosition =
          CollisionSystem::ResolveCapsuleAgainstCollidersAndMesh(
              desiredPosition, capsule, colliders, meshTriangles,
              worldHalfExtentMeters);
    }

    const float displacementX = m_previewPosition.x - previousPosition.x;
    const float displacementZ = m_previewPosition.z - previousPosition.z;
    m_lastMovementSpeed =
        dt > 0.00001f
            ? std::sqrt(displacementX * displacementX +
                        displacementZ * displacementZ) /
                  dt
            : 0.0f;

    if (moveX * moveX + moveZ * moveZ > 0.0001f)
      UpdateFacing(std::atan2(moveX, moveZ), dt);
    if (!m_autoCycle && !m_manualPreview) {
      RequestLocomotionClip(m_lastMovementSpeed > 0.05f
                                ? (running ? ClipSlot::Run : ClipSlot::Walk)
                                : ClipSlot::Idle);
    }
  } else {
    m_lastMovementSpeed = 0.0f;
    if (!m_autoCycle && !m_manualPreview)
      RequestLocomotionClip(ClipSlot::Idle);
  }

  if (m_autoCycle) {
    m_cycleTimer += std::max(0.0f, dt);
    if (m_cycleTimer >= 3.0f) {
      m_cycleTimer = 0.0f;
      const int next = (static_cast<int>(m_activeSlot) + 1) % 3;
      RequestLocomotionClip(static_cast<ClipSlot>(next));
    }
  }
  UpdateAnimationPose(dt);
}


void PlayerAnimationPreview::SetPosition(const XMFLOAT3 &position) {
  m_previewPosition = position;
}

void PlayerAnimationPreview::SetYaw(float yawRadians) {
  m_previewYaw = yawRadians;
}
void PlayerAnimationPreview::BuildFrame(FrameData &frame,
                                        float scaleMultiplier) const {
  if (!m_ready)
    return;

  const float renderScale = m_modelToWorldScale * m_previewScale *
                            std::max(0.01f, scaleMultiplier);
  const float groundOffset = -m_nativeModelMinY * renderScale;
  const XMMATRIX world =
      XMMatrixScaling(renderScale, renderScale, renderScale) *
      XMMatrixRotationY(m_previewYaw) *
      XMMatrixTranslation(m_previewPosition.x,
                          m_previewPosition.y + groundOffset + 0.01f,
                          m_previewPosition.z);
  for (const uint32_t meshId : m_opaqueMeshIds)
    frame.opaqueItems.push_back({meshId, world});
  for (const uint32_t meshId : m_transparentMeshIds)
    frame.transparentItems.push_back({meshId, world});

  GPUPointLight light{};
  light.position = {m_previewPosition.x, 1.1f, m_previewPosition.z - 1.5f};
  light.range = 4.0f;
  light.color = {0.9f, 0.8f, 0.65f};
  light.intensity = 1.6f;
  frame.pointLights.push_back(light);
}

void PlayerAnimationPreview::DrawDebugUi() {
  if (!ImGui::Begin("プレイヤーアニメーション")) {
    ImGui::End();
    return;
  }

  DrawDebugControls();
  ImGui::End();
}

void PlayerAnimationPreview::DrawDebugControls() {
  ImGui::Text("モデル: Assets/models/MyFirstChar.vrm");
  ImGui::Text("スケルトン: %s", m_hasSkeleton ? "OK" : "NG");
  ImGui::Text("マテリアル: %zu（不透明 %zu / 透過 %zu / 両面 %zu）",
              MaterialPartCount(), OpaqueMaterialPartCount(),
              TransparentMaterialPartCount(), DoubleSidedMaterialPartCount());
  ImGui::Text("ボーン数: %zu", m_skeleton.bones.size());
  ImGui::Text("身長: native %.3f m -> world %.3f m", m_nativeModelHeight,
              WorldModelHeight());
  ImGui::Text("移動速度: %.2f m/s", m_lastMovementSpeed);
  ImGui::Text("ボーンパレット: %s",
              m_bonePaletteFinite ? "正常" : "異常");
  ImGui::Separator();

  for (int i = 0; i < 3; ++i) {
    PreviewClip &clip = m_clips[i];
    const bool selected = static_cast<int>(m_activeSlot) == i;
    std::string buttonLabel = clip.label;
    if (clip.fallback)
      buttonLabel += "（代替）";
    if (ImGui::RadioButton(buttonLabel.c_str(), selected)) {
      SelectLocomotionClip(static_cast<ClipSlot>(i));
      m_cycleTimer = 0.0f;
    }

    ImGui::SameLine();
    if (clip.loaded) {
      ImGui::TextColored(ImVec4(0.25f, 1.0f, 0.35f, 1.0f), "OK");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", clip.loadedPath.c_str());
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "未読込");
    }
  }

  const ClipDiagnostics takingItem = GetTakingItemDiagnostics();
  ImGui::Separator();
  ImGui::Text("取得アクション: %s", takingItem.loaded ? "OK" : "未読込");
  if (takingItem.loaded) {
    ImGui::SameLine();
    const float playbackDuration =
        (takingItem.duration - m_takingItemStartTime) /
        m_takingItemPlaybackRate;
    ImGui::Text("%.2f s（開始 %.2f s）/ %.2fx = %.2f s", takingItem.duration,
                m_takingItemStartTime, m_takingItemPlaybackRate,
                playbackDuration);
    if (ImGui::Button("取得アクションを再生"))
      PlayTakingItem();
    ImGui::SameLine();
    ImGui::Text("状態: %s", m_actionActive ? "再生中" : "待機");
  }

  ImGui::Checkbox("手動プレビュー", &m_manualPreview);
  ImGui::Checkbox("自動切替", &m_autoCycle);
  if (m_autoCycle)
    m_manualPreview = true;
  ImGui::SliderFloat("遷移時間", &m_transitionDuration, 0.05f, 0.40f,
                     "%.2f s");
  ImGui::SliderFloat("向き", &m_previewYaw, -3.14159265f, 3.14159265f);
  ImGui::SliderFloat("スケール倍率", &m_previewScale, 0.75f, 1.25f);
}
