#pragma once

#include "AnimationPlayer.h"
#include "DxContext.h"
#include "RenderPass.h"
#include "game/CollisionSystem.h"

#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

class Input;

// VRM プレイヤーモデル、移動、衝突、Idle / Walk / Run 遷移をまとめる。
// クラス名は旧プレイヤープレビューとの互換性のため維持している。
class PlayerAnimationPreview {
public:
  enum class ClipSlot : int {
    Idle = 0,
    Walk = 1,
    Run = 2,
  };

  struct ClipDiagnostics {
    std::string label;
    std::string sourcePath;
    float duration = 0.0f;
    size_t trackCount = 0;
    bool loaded = false;
    bool fallback = false;
  };

  void Initialize(DxContext &dx);
  void Update(float dt);
  void Update(float dt, const Input &input, float worldHalfExtentMeters,
              const std::vector<CollisionSystem::Collider> &colliders,
              const std::vector<CollisionSystem::MeshTriangle>
                  &meshTriangles,
              bool debugFly = false);
  void BuildFrame(FrameData &frame, float scaleMultiplier = 1.0f) const;
  void DrawDebugUi();
  void DrawDebugControls();

  bool IsReady() const { return m_ready; }
  bool HasSkeleton() const { return m_hasSkeleton; }
  size_t SkeletonBoneCount() const { return m_skeleton.bones.size(); }
  size_t MaterialPartCount() const {
    return m_opaqueMeshIds.size() + m_transparentMeshIds.size();
  }
  size_t OpaqueMaterialPartCount() const { return m_opaqueMeshIds.size(); }
  size_t TransparentMaterialPartCount() const {
    return m_transparentMeshIds.size();
  }
  size_t DoubleSidedMaterialPartCount() const {
    return m_doubleSidedMaterialPartCount;
  }
  ClipDiagnostics GetClipDiagnostics(ClipSlot slot) const;
  ClipSlot ActiveClip() const { return m_activeSlot; }
  bool IsTransitioning() const { return m_transitioning; }
  bool BonePaletteFinite() const { return m_bonePaletteFinite; }
  float NativeModelHeight() const { return m_nativeModelHeight; }
  float WorldModelHeight() const;
  DirectX::XMFLOAT3 Position() const { return m_previewPosition; }
  void SetPosition(const DirectX::XMFLOAT3 &position);
  void SetYaw(float yawRadians);
  void SelectLocomotionClip(ClipSlot slot);

private:
  struct PreviewClip {
    std::string label;
    std::string loadedPath;
    int clipIndex = -1;
    bool loaded = false;
    bool fallback = false;
  };

  bool LoadClip(const std::string &label, const std::string &primaryPath,
                const std::string &fallbackPath, PreviewClip &outClip);
  int ActiveClipIndex() const;
  int ClipIndex(ClipSlot slot) const;
  float ClipDuration(ClipSlot slot) const;
  float ClipPlaybackRate(ClipSlot slot) const;
  void TransitionToClip(ClipSlot slot);
  void UpdateAnimationPose(float dt);
  void UploadBonePalette(const BonePalette &palette);
  void UpdateFacing(float targetYaw, float dt);

private:
  std::vector<uint32_t> m_opaqueMeshIds;
  std::vector<uint32_t> m_transparentMeshIds;
  size_t m_doubleSidedMaterialPartCount = 0;
  Skeleton m_skeleton;
  bool m_ready = false;
  bool m_hasSkeleton = false;
  bool m_bonePaletteFinite = true;
  DxContext *m_dx = nullptr;

  std::vector<AnimationClip> m_animations;
  std::array<PreviewClip, 3> m_clips = {};
  std::array<float, 3> m_clipPlaybackRates = {1.0f, 1.0f, 1.0f};

  ClipSlot m_activeSlot = ClipSlot::Idle;
  ClipSlot m_previousSlot = ClipSlot::Idle;
  float m_animTime = 0.0f;
  float m_previousAnimTime = 0.0f;
  float m_transitionElapsed = 0.0f;
  float m_transitionDuration = 0.18f;
  bool m_transitioning = false;
  ClipSlot m_queuedSlot = ClipSlot::Idle;
  bool m_hasQueuedTransition = false;
  float m_lastMovementSpeed = 0.0f;
  float m_previewYaw = 0.0f;
  float m_previewScale = 1.0f;
  float m_nativeModelHeight = 0.0f;
  float m_nativeModelMinY = 0.0f;
  float m_modelToWorldScale = 1.0f;
  DirectX::XMFLOAT3 m_previewPosition = {0.0f, 0.0f, 0.0f};
  bool m_manualPreview = false;
  bool m_autoCycle = false;
  float m_cycleTimer = 0.0f;
};
