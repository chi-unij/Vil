#pragma once

#include "DxContext.h"
#include "Input.h"
#include "RenderPass.h"
#include "particle_test.h"

#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class PlayerAnimationPreview;

class BossArenaScene {
public:
  enum class AttackType {
    MeteorAoE,
    LaserLine,
  };

  void Initialize(DxContext &dx);
  void Reset(PlayerAnimationPreview &player);
  void Update(float dt, const Input &input, PlayerAnimationPreview &player);
  void BuildFrame(FrameData &frame) const;
  void ApplyTechShowcase(FrameData &frame) const;
  void DrawHud(int viewportWidth, int viewportHeight);

  bool IsReady() const { return m_ready; }
  float ArenaHalfExtent() const { return kArenaHalfExtent; }
  bool IsPhoneOverlayActive() const {
    return m_phoneOpen || m_phoneSlide > 0.05f;
  }
  bool DebugNoClipEnabled() const { return m_debugNoClip; }
  bool PhaseTwoActive() const { return m_bossHp <= 2 && !m_cleared; }
  bool TechShowcaseTAAEnabled() const {
    return m_techShowcaseOverride && m_showcaseTAA;
  }
  float CameraImpulseAmount() const;
  DirectX::XMFLOAT3
  VisualPositionForGameplayPosition(const DirectX::XMFLOAT3 &position) const;

private:
  enum class AttackPhase {
    Telegraph,
    Resolve,
    Recovery,
  };

  enum class MirrorPuzzleType {
    SymbolMemory,
    NumberPosition,
  };

  static constexpr float kArenaHalfExtent = 18.0f;
  static constexpr float kLaneHalfWidth = 4.15f;
  static constexpr float kRunnerMinZ = -15.2f;
  static constexpr float kRunnerMaxZ = -8.2f;
  static constexpr float kPlayerHitRadius = 0.45f;

  void StartNextAttack();
  void ResolveAttack(PlayerAnimationPreview &player);
  bool IsPlayerInCurrentAttack(const DirectX::XMFLOAT3 &playerPos) const;
  const char *AttackName() const;
  void AppendTelegraphLines(FrameData &frame) const;
  void UpdatePhoneOverlay(float dt, const Input &input);
  void DrawPhoneOverlay(int viewportWidth, int viewportHeight);
  void PrepareMirrorPuzzle(AttackType attack);
  void CompleteMirrorPuzzle();
  void FailMirrorPuzzle();
  void SpawnMirrorCharges();
  void UpdateMirrorCharges(const PlayerAnimationPreview &player);
  void AppendMirrorCharges(FrameData &frame) const;
  float MirrorRandom01(uint32_t salt) const;
  float TelegraphDuration() const;
  void AppendWorldPolish(FrameData &frame) const;

  uint32_t m_floorMeshId = UINT32_MAX;
  uint32_t m_bossMeshId = UINT32_MAX;
  uint32_t m_aoeTelegraphMeshId = UINT32_MAX;
  uint32_t m_laserTelegraphMeshId = UINT32_MAX;
  uint32_t m_knockbackTelegraphMeshId = UINT32_MAX;
  uint32_t m_flameMeshId = UINT32_MAX;
  uint32_t m_pathGlowMeshId = UINT32_MAX;
  uint32_t m_sideMistMeshId = UINT32_MAX;
  uint32_t m_sideFogMeshId = UINT32_MAX;
  uint32_t m_spiritVeilMeshId = UINT32_MAX;
  uint32_t m_laserRiftMeshId = UINT32_MAX;
  uint32_t m_moonDiscMeshId = UINT32_MAX;
  uint32_t m_moonRayMeshId = UINT32_MAX;
  uint32_t m_bossSealMeshId = UINT32_MAX;
  uint32_t m_counterRibbonMeshId = UINT32_MAX;
  uint32_t m_pathStoneMeshId = UINT32_MAX;
  uint32_t m_pathEdgeMeshId = UINT32_MAX;
  uint32_t m_toriiWoodMeshId = UINT32_MAX;
  uint32_t m_mossBankMeshId = UINT32_MAX;
  uint32_t m_lanternPostMeshId = UINT32_MAX;
  uint32_t m_lanternCapMeshId = UINT32_MAX;
  uint32_t m_lanternGlowMeshId = UINT32_MAX;
  uint32_t m_lanternHaloMeshId = UINT32_MAX;
  uint32_t m_riverWaterMeshId = UINT32_MAX;
  uint32_t m_mirrorChargeMeshId = UINT32_MAX;
  uint32_t m_mirrorShardMeshId = UINT32_MAX;
  uint32_t m_bossDamageShardMeshId = UINT32_MAX;
  uint32_t m_ofudaMeshId = UINT32_MAX;
  std::vector<uint32_t> m_shrineMeshIds;
  std::vector<uint32_t> m_lanternMeshIds;
  std::vector<uint32_t> m_toriiGateMeshIds;
  std::vector<uint32_t> m_shrineGateMeshIds;
  std::unique_ptr<SparkBurstEmitter> m_aoeSparkBurst;
  std::unique_ptr<MirrorSparkBurstEmitter> m_counterSparkBurst;
  std::unique_ptr<SmokeEmitter> m_bossSmokeEmitter;
  std::array<std::unique_ptr<RiverMistEmitter>, 5> m_riverMistEmitters;
  std::array<std::unique_ptr<RiverMistEmitter>, 8> m_sideFogEmitters;
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
  float m_battleTimer = 0.0f;
  int m_countersUsed = 0;
  int m_damageTaken = 0;
  float m_hitFlashTimer = 0.0f;
  DirectX::XMFLOAT3 m_lastHitPosition = {0.0f, 0.04f, 0.0f};
  float m_impactTimer = 0.0f;
  float m_impactMaxRadius = 3.0f;
  DirectX::XMFLOAT3 m_impactPosition = {0.0f, 0.06f, 0.0f};
  float m_aoeSparkTimer = 0.0f;
  float m_counterVfxTimer = 0.0f;
  float m_bossHitShakeTimer = 0.0f;
  float m_phaseShiftVfxTimer = 0.0f;
  bool m_failed = false;
  bool m_cleared = false;
  bool m_debugNoClip = false;
  bool m_showDebugPanel = false;
  bool m_debugPanelToggleWasDown = false;

  bool m_techShowcaseOverride = true;
  bool m_showcaseShadows = true;
  bool m_showcaseSSAO = true;
  bool m_showcaseSSR = true;
  bool m_showcaseBloom = true;
  bool m_showcaseFXAA = true;
  bool m_showcaseTAA = false;
  bool m_showcaseMotionBlur = false;
  bool m_showcaseDOF = true;
  bool m_showcaseRain = true;
  bool m_showcaseClimaxVfx = true;

  bool m_phoneOpen = false;
  bool m_phoneSpaceWasDown = false;
  float m_phoneSlide = 0.0f;

  AttackType m_mirrorAttack = AttackType::MeteorAoE;
  MirrorPuzzleType m_mirrorPuzzleType = MirrorPuzzleType::SymbolMemory;
  std::array<DirectX::XMFLOAT3, 3> m_mirrorChargePositions = {};
  std::array<bool, 3> m_mirrorChargeActive = {};
  std::array<int, 3> m_memorySequence = {};
  std::array<int, 3> m_numberTargets = {};
  std::array<int, 3> m_numberButtonOrder = {};
  bool m_mirrorPuzzleReady = false;
  bool m_mirrorPuzzleSolved = false;
  float m_mirrorMessageTimer = 0.0f;
  float m_mirrorPickupToastTimer = 0.0f;
  float m_mirrorPickupVfxTimer = 0.0f;
  DirectX::XMFLOAT3 m_lastMirrorPickupPosition = {0.0f, 0.0f, 0.0f};
  float m_memoryTimer = 0.0f;
  float m_memoryFailTimer = 0.0f;
  int m_mirrorCharge = 0;
  int m_memoryInputIndex = 0;
};
