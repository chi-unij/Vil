#pragma once

#include "DxContext.h"
#include "Input.h"
#include "RenderPass.h"

#include <DirectXMath.h>
#include <array>
#include <cstdint>

class PlayerAnimationPreview;

class BossArenaScene {
public:
  enum class AttackType {
    MeteorAoE,
    LaserLine,
    Knockback,
  };

  void Initialize(DxContext &dx);
  void Reset(PlayerAnimationPreview &player);
  void Update(float dt, const Input &input, PlayerAnimationPreview &player);
  void BuildFrame(FrameData &frame) const;
  void DrawHud(int viewportWidth, int viewportHeight);

  bool IsReady() const { return m_ready; }
  float ArenaHalfExtent() const { return kArenaHalfExtent; }
  bool IsPhoneOverlayActive() const {
    return m_phoneOpen || m_phoneSlide > 0.05f;
  }

private:
  enum class AttackPhase {
    Telegraph,
    Resolve,
    Recovery,
  };

  static constexpr float kArenaHalfExtent = 18.0f;
  static constexpr float kPlayerHitRadius = 0.45f;

  void StartNextAttack();
  void ResolveAttack(PlayerAnimationPreview &player);
  bool IsPlayerInCurrentAttack(const DirectX::XMFLOAT3 &playerPos) const;
  void ApplyKnockback(PlayerAnimationPreview &player);
  const char *AttackName() const;
  void AppendTelegraphLines(FrameData &frame) const;
  void UpdatePhoneOverlay(float dt, const Input &input);
  void DrawPhoneOverlay(int viewportWidth, int viewportHeight);
  void PrepareMirrorPuzzle(AttackType attack);
  void CompleteMirrorPuzzle();
  bool AreMirrorDotsPlaced() const;
  float MirrorRandom01(uint32_t salt) const;
  float TelegraphDuration() const;

  uint32_t m_floorMeshId = UINT32_MAX;
  uint32_t m_bossMeshId = UINT32_MAX;
  uint32_t m_aoeTelegraphMeshId = UINT32_MAX;
  uint32_t m_laserTelegraphMeshId = UINT32_MAX;
  uint32_t m_knockbackTelegraphMeshId = UINT32_MAX;
  uint32_t m_flameMeshId = UINT32_MAX;
  bool m_ready = false;

  AttackType m_attack = AttackType::MeteorAoE;
  AttackPhase m_phase = AttackPhase::Telegraph;
  int m_attackIndex = 0;
  float m_phaseTimer = 0.0f;
  bool m_resolved = false;

  DirectX::XMFLOAT3 m_attackCenter = {0.0f, 0.02f, 0.0f};
  float m_attackRadius = 4.0f;
  float m_laserHalfWidth = 1.4f;
  bool m_laserVertical = true;

  int m_playerHp = 3;
  int m_bossHp = 5;
  float m_surviveTimer = 0.0f;
  float m_hitFlashTimer = 0.0f;
  DirectX::XMFLOAT3 m_lastHitPosition = {0.0f, 0.04f, 0.0f};
  float m_impactTimer = 0.0f;
  float m_impactMaxRadius = 3.0f;
  DirectX::XMFLOAT3 m_impactPosition = {0.0f, 0.06f, 0.0f};
  float m_counterVfxTimer = 0.0f;
  float m_bossHitShakeTimer = 0.0f;
  DirectX::XMFLOAT3 m_knockbackVelocity = {0.0f, 0.0f, 0.0f};
  float m_knockbackTimer = 0.0f;
  bool m_failed = false;
  bool m_cleared = false;

  bool m_phoneOpen = false;
  bool m_phoneSpaceWasDown = false;
  float m_phoneSlide = 0.0f;

  AttackType m_mirrorAttack = AttackType::MeteorAoE;
  std::array<DirectX::XMFLOAT2, 3> m_mirrorTargets = {};
  std::array<DirectX::XMFLOAT2, 3> m_mirrorDots = {};
  std::array<bool, 3> m_mirrorPlaced = {};
  bool m_mirrorPuzzleReady = false;
  bool m_mirrorPuzzleSolved = false;
  float m_mirrorMessageTimer = 0.0f;
  int m_dragMirrorDot = -1;
};
