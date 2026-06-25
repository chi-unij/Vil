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

// CHI-35: プレイヤーモデルと Idle / Walk / Run クリップの確認用。
// 本番の PlayerController / Animation State Machine は CHI-38 で接続する。
class PlayerAnimationPreview {
public:
  enum class ClipSlot : int {
    Idle = 0,
    Walk = 1,
    Run = 2,
  };

  void Initialize(DxContext &dx);
  void Update(float dt);
  void Update(float dt, const Input &input, float worldHalfExtentMeters,
              const std::vector<CollisionSystem::Collider> &colliders,
              const std::vector<CollisionSystem::MeshTriangle>
                  &meshTriangles,
              bool debugFly = false);
  void BuildFrame(FrameData &frame) const;
  void DrawDebugUi();

  bool IsReady() const { return m_ready; }
  DirectX::XMFLOAT3 Position() const { return m_previewPosition; }
  void SetPosition(const DirectX::XMFLOAT3 &position);
  void SetYaw(float yawRadians);

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

private:
  uint32_t m_meshId = UINT32_MAX;
  Skeleton m_skeleton;
  bool m_ready = false;
  bool m_hasSkeleton = false;
  DxContext *m_dx = nullptr;

  std::vector<AnimationClip> m_animations;
  std::array<PreviewClip, 3> m_clips = {};

  ClipSlot m_activeSlot = ClipSlot::Idle;
  float m_animTime = 0.0f;
  float m_previewYaw = 3.14159265f;
  float m_previewScale = 1.0f;
  DirectX::XMFLOAT3 m_previewPosition = {0.0f, 0.0f, 0.0f};
  bool m_autoCycle = false;
  float m_cycleTimer = 0.0f;
};
