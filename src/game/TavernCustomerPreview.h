#pragma once

#include "AnimationPlayer.h"
#include "DxContext.h"
#include "RenderPass.h"

#include <DirectXMath.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// 酒場の customer NPC model、Idle / Walk 遷移、描画をまとめる。
// model path ごとに一つ保持し、同じ animation set を共有できる roster とする。
class TavernCustomerPreview {
public:
  enum class AnimationMode : std::uint8_t { Idle, Walk, Sitting };

  struct ClipDiagnostics {
    std::string label;
    std::string sourcePath;
    float duration = 0.0f;
    size_t trackCount = 0;
    bool loaded = false;
  };

  struct AnimationState {
    BonePalette palette{};
    bool initialized = false;
    bool paletteFinite = true;
    AnimationMode mode = AnimationMode::Idle;
    AnimationMode previousMode = AnimationMode::Idle;
    bool transitioning = false;
    float animationTime = 0.0f;
    float previousAnimationTime = 0.0f;
    float transitionElapsed = 0.0f;
  };

  void Initialize(DxContext &dx, const std::string &modelLabel,
                  const std::string &modelPath);
  void InitializeAnimationState(AnimationState &state) const;
  void Update(AnimationState &state, float deltaSeconds, AnimationMode mode);
  void BuildFrame(FrameData &frame, const DirectX::XMFLOAT3 &position,
                  float yawRadians, const AnimationState &state) const;

  bool IsReady() const { return m_ready; }
  const std::string &ModelLabel() const { return m_modelLabel; }
  const std::string &ModelPath() const { return m_modelPath; }
  bool HasSkeleton() const { return m_hasSkeleton; }
  bool BonePaletteFinite() const { return m_bonePaletteFinite; }
  size_t SkeletonBoneCount() const { return m_skeleton.bones.size(); }
  size_t MaterialPartCount() const {
    return m_opaqueMeshIds.size() + m_transparentMeshIds.size();
  }
  size_t OpaqueMaterialPartCount() const { return m_opaqueMeshIds.size(); }
  size_t TransparentMaterialPartCount() const {
    return m_transparentMeshIds.size();
  }
  ClipDiagnostics IdleDiagnostics() const;
  ClipDiagnostics WalkDiagnostics() const;
  ClipDiagnostics SittingDiagnostics() const;
  uint64_t IdlePoseUpdateCount() const { return m_idlePoseUpdateCount; }
  uint64_t WalkPoseUpdateCount() const { return m_walkPoseUpdateCount; }
  uint64_t SittingPoseUpdateCount() const { return m_sittingPoseUpdateCount; }

private:
  struct PreviewClip {
    std::string label;
    std::string sourcePath;
    int clipIndex = -1;
    bool loaded = false;
  };

  bool LoadClip(const std::string &label, const std::string &path,
                PreviewClip &outClip);
  int ActiveClipIndex(AnimationMode mode) const;
  ClipDiagnostics BuildClipDiagnostics(const PreviewClip &clip) const;
  void SelectAnimationMode(AnimationState &state, AnimationMode mode) const;
  void UploadBonePalette(const BonePalette &palette);
  static bool PaletteFinite(const BonePalette &palette);

  std::vector<uint32_t> m_opaqueMeshIds;
  std::vector<uint32_t> m_transparentMeshIds;
  std::string m_modelLabel;
  std::string m_modelPath;
  Skeleton m_skeleton;
  std::vector<AnimationClip> m_animations;
  PreviewClip m_idleClip;
  PreviewClip m_walkClip;
  PreviewClip m_sittingClip;
  DxContext *m_dx = nullptr;
  bool m_ready = false;
  bool m_hasSkeleton = false;
  bool m_bonePaletteFinite = true;
  float m_nativeModelHeight = 0.0f;
  float m_nativeModelMinY = 0.0f;
  float m_modelToWorldScale = 1.0f;
  uint64_t m_idlePoseUpdateCount = 0;
  uint64_t m_walkPoseUpdateCount = 0;
  uint64_t m_sittingPoseUpdateCount = 0;
};
