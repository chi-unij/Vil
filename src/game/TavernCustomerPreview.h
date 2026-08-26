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
// 最初の一体だけを production table state に接続し、複数 NPC 導入前の
// asset / skeleton compatibility gate として使用する。
class TavernCustomerPreview {
public:
  struct ClipDiagnostics {
    std::string label;
    std::string sourcePath;
    float duration = 0.0f;
    size_t trackCount = 0;
    bool loaded = false;
  };

  void Initialize(DxContext &dx);
  void Update(float deltaSeconds, bool walking);
  void BuildFrame(FrameData &frame, const DirectX::XMFLOAT3 &position,
                  float yawRadians) const;

  bool IsReady() const { return m_ready; }
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
  uint64_t IdlePoseUpdateCount() const { return m_idlePoseUpdateCount; }
  uint64_t WalkPoseUpdateCount() const { return m_walkPoseUpdateCount; }

private:
  struct PreviewClip {
    std::string label;
    std::string sourcePath;
    int clipIndex = -1;
    bool loaded = false;
  };

  bool LoadClip(const std::string &label, const std::string &path,
                PreviewClip &outClip);
  int ActiveClipIndex() const;
  ClipDiagnostics BuildClipDiagnostics(const PreviewClip &clip) const;
  void SelectWalking(bool walking);
  void UploadBonePalette(const BonePalette &palette);

  std::vector<uint32_t> m_opaqueMeshIds;
  std::vector<uint32_t> m_transparentMeshIds;
  Skeleton m_skeleton;
  std::vector<AnimationClip> m_animations;
  PreviewClip m_idleClip;
  PreviewClip m_walkClip;
  DxContext *m_dx = nullptr;
  bool m_ready = false;
  bool m_hasSkeleton = false;
  bool m_bonePaletteFinite = true;
  bool m_walking = false;
  bool m_previousWalking = false;
  bool m_transitioning = false;
  float m_animationTime = 0.0f;
  float m_previousAnimationTime = 0.0f;
  float m_transitionElapsed = 0.0f;
  float m_nativeModelHeight = 0.0f;
  float m_nativeModelMinY = 0.0f;
  float m_modelToWorldScale = 1.0f;
  uint64_t m_idlePoseUpdateCount = 0;
  uint64_t m_walkPoseUpdateCount = 0;
};
