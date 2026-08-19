#pragma once

#include "DxContext.h"
#include "RenderPass.h"
#include "game/CollisionSystem.h"

#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

class TavernScene {
public:
  enum class Action {
    None,
    ReturnToOverworld,
  };

  void Initialize(DxContext &dx);
  void Reset();
  Action Update(float deltaSeconds, const DirectX::XMFLOAT3 &playerPosition,
                bool interactPressed, bool primaryActionDown,
                bool restartPressed, bool automateGameplay = false);
  void BuildFrame(FrameData &frame) const;
  Action DrawHud(int viewportWidth, int viewportHeight);

  bool IsReady() const { return m_ready; }
  bool GameplaySmokeComplete() const {
    return m_tableCompletedCycles[0] >= 1 && m_tableCompletedCycles[1] >= 1;
  }
  bool DayCycleSmokeComplete() const {
    return m_shiftState == ShiftState::Complete && GameplaySmokeComplete();
  }
  bool PlayerMovementLocked() const { return m_workState != WorkState::None; }
  int CompletedCycles() const { return m_completedCycles; }
  int Gold() const { return m_gold; }
  int ServedCustomers() const { return m_servedCustomers; }
  int Walkouts() const { return m_walkouts; }
  float BusinessHour() const { return m_businessHour; }
  DirectX::XMFLOAT3 PlayerSpawnPosition() const { return {0.0f, 0.0f, -0.35f}; }
  DirectX::XMFLOAT3 CameraPosition() const { return {0.0f, 3.0f, -5.15f}; }
  float CameraYaw() const { return 0.0f; }
  float CameraPitch() const { return -0.48f; }
  const std::vector<CollisionSystem::Collider> &CollisionColliders() const {
    return m_collisionColliders;
  }

private:
  enum class ShiftState { Running, Closing, Complete, Failed };
  enum class TableState {
    Empty,
    Arriving,
    WaitingOrder,
    WaitingAle,
    Eating,
    Leaving,
    Dirty,
  };
  enum class HeldItem { None, EmptyMug, FilledMug, DirtyMug };
  enum class WorkState { None, PouringAle, WashingMug };
  enum class TutorialStep {
    TakeOrder,
    GetMug,
    StartPour,
    PourAle,
    ServeAle,
    CollectMug,
    WashMug,
    Complete,
  };

  struct TableSlot {
    TableState state = TableState::Empty;
    float stateTimer = 0.0f;
    float satisfaction = 100.0f;
    bool enabled = false;
    bool servedCustomer = false;
    bool complaintPlayed = false;
    std::string speech;
    float speechTimer = 0.0f;
  };

  static const char *GetTableStateName(TableState state);
  static const char *GetHeldItemName(HeldItem item);
  void SpawnCustomer(int tableIndex);
  void TakeOrder(int tableIndex);
  void PickUpEmptyMug();
  void ReturnEmptyMug();
  void BeginPouring();
  void FinishPouring();
  void ServeAle(int tableIndex);
  void CollectDirtyMug(int tableIndex);
  void BeginWashing();
  void FinishWashing();
  void TriggerWalkout(int tableIndex);
  void RunAutomation();
  void UpdateNearbyPrompt();
  int NearestEnabledTable(float maximumDistance) const;
  int FindTableInState(TableState state) const;
  int FindMostUrgentWaitingAleTable() const;
  bool HasActiveCustomers() const;
  float CustomerSpawnDelay(int tableIndex) const;
  const char *CustomerTrafficName() const;
  bool CanServeCustomers() const;

  uint32_t m_floorMeshId = UINT32_MAX;
  uint32_t m_wallMeshId = UINT32_MAX;
  uint32_t m_woodMeshId = UINT32_MAX;
  uint32_t m_darkWoodMeshId = UINT32_MAX;
  uint32_t m_glowMeshId = UINT32_MAX;
  uint32_t m_customerBodyMeshId = UINT32_MAX;
  uint32_t m_customerHeadMeshId = UINT32_MAX;
  uint32_t m_mugMeshId = UINT32_MAX;
  uint32_t m_aleMeshId = UINT32_MAX;
  uint32_t m_foamMeshId = UINT32_MAX;
  uint32_t m_metalMeshId = UINT32_MAX;

  ShiftState m_shiftState = ShiftState::Running;
  std::array<TableSlot, 3> m_tables{};
  HeldItem m_heldItem = HeldItem::None;
  WorkState m_workState = WorkState::None;
  TutorialStep m_tutorialStep = TutorialStep::TakeOrder;
  DirectX::XMFLOAT3 m_playerPosition = {0.0f, 0.0f, -0.35f};
  std::vector<CollisionSystem::Collider> m_collisionColliders;
  std::string m_nearbyPrompt;
  std::string m_feedbackText;
  float m_businessHour = 5.0f;
  std::array<float, 3> m_spawnTimers{};
  std::array<int, 3> m_tableCompletedCycles{};
  float m_aleFill = 0.0f;
  float m_aleFoam = 0.0f;
  float m_aleOverflow = 0.0f;
  float m_pourQuality = 1.0f;
  float m_washProgress = 0.0f;
  float m_interactionCooldown = 0.0f;
  float m_feedbackTimer = 0.0f;
  float m_pourVisualTime = 0.0f;
  int m_gold = 0;
  int m_servedCustomers = 0;
  int m_walkouts = 0;
  int m_completedCycles = 0;
  int m_cleanMugs = 2;
  int m_perfectPours = 0;
  int m_heldMugTableIndex = -1;
  int m_tutorialTableIndex = 0;
  bool m_workActionStarted = false;
  bool m_cycleAwaitingWash = false;
  bool m_primaryActionActive = false;
  bool m_lastPourPerfect = false;
  bool m_ready = false;
};
