#include "game/TavernScene.h"

#include "MeshRenderer.h"
#include "ProceduralMesh.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <imgui.h>

using namespace DirectX;

namespace {

constexpr std::array<XMFLOAT3, 3> kTableInteractions = {
    XMFLOAT3{-3.8f, 0.0f, 2.72f}, XMFLOAT3{0.0f, 0.0f, 2.72f},
    XMFLOAT3{3.8f, 0.0f, 2.72f}};
constexpr XMFLOAT3 kMugRackInteraction = {-2.7f, 0.0f, 6.90f};
constexpr XMFLOAT3 kAleTapInteraction = {0.0f, 0.0f, 6.90f};
constexpr XMFLOAT3 kWashBasinInteraction = {2.7f, 0.0f, 6.90f};
constexpr std::array<XMFLOAT3, 2> kCounterMugInteractions = {
    XMFLOAT3{-1.55f, 0.0f, 6.90f}, XMFLOAT3{-0.85f, 0.0f, 6.90f}};
constexpr std::array<XMFLOAT3, 2> kCounterMugPositions = {
    XMFLOAT3{-1.55f, 1.65f, 7.65f}, XMFLOAT3{-0.85f, 1.65f, 7.65f}};
constexpr XMFLOAT3 kExitInteraction = {0.0f, 0.0f, -1.05f};
constexpr float kStationInteractionRange = 1.55f;
constexpr float kCounterMugInteractionRange = 0.58f;
constexpr float kTableInteractionRange = 2.05f;
constexpr float kPerfectPourMinimum = 0.82f;
constexpr float kPerfectPourMaximum = 0.96f;
constexpr int kActiveTableCount = 2;
constexpr int kTotalMugs = 2;
constexpr float kBusinessOpenHour = 5.0f;
constexpr float kMugScaleXZ = 0.345f;
constexpr float kMugScaleY = 0.465f;
constexpr float kAleSurfaceScaleXZ = 0.255f;
constexpr const char *kAleName = "水鏡エール";

float DistanceXZ(const XMFLOAT3 &a, const XMFLOAT3 &b) {
  const float dx = a.x - b.x;
  const float dz = a.z - b.z;
  return std::sqrt(dx * dx + dz * dz);
}

ImU32 TavernUiColor(float r, float g, float b, float a) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a));
}

void DrawPanel(ImDrawList *draw, const ImVec2 &minimum, const ImVec2 &maximum,
               ImU32 borderColor, float rounding = 10.0f) {
  draw->AddRectFilled(minimum, maximum,
                      TavernUiColor(0.035f, 0.022f, 0.015f, 0.92f), rounding);
  draw->AddRect(minimum, maximum, borderColor, rounding, 0, 1.5f);
}

} // namespace

const char *TavernScene::GetTableStateName(TableState state) {
  switch (state) {
  case TableState::Empty:
    return "空席";
  case TableState::Arriving:
    return "来店中";
  case TableState::WaitingOrder:
    return "注文待ち";
  case TableState::WaitingAle:
    return "エール待ち";
  case TableState::Eating:
    return "飲食中";
  case TableState::Leaving:
    return "退店中";
  case TableState::Dirty:
    return "汚れたジョッキ";
  }
  return "不明";
}

const char *TavernScene::GetHeldItemName(HeldItem item) {
  switch (item) {
  case HeldItem::None:
    return "手ぶら";
  case HeldItem::EmptyMug:
    return "空のジョッキ";
  case HeldItem::FilledMug:
    return "エール入りジョッキ";
  case HeldItem::DirtyMug:
    return "汚れたジョッキ";
  }
  return "手ぶら";
}

void TavernScene::Initialize(DxContext &dx) {
  const LoadedMesh planeMesh = ProceduralMesh::CreatePlane(1.0f, 1.0f);
  const LoadedMesh cubeMesh = ProceduralMesh::CreateCube(1.0f);
  const LoadedMesh cylinderMesh =
      ProceduralMesh::CreateCylinder(0.5f, 1.0f, 20);
  const LoadedMesh bodyMesh = ProceduralMesh::CreateCylinder(0.5f, 1.0f, 16);
  const LoadedMesh headMesh = ProceduralMesh::CreateSphere(0.5f, 10, 20);

  Material floorMaterial{};
  floorMaterial.baseColorFactor = {0.24f, 0.17f, 0.10f, 1.0f};
  floorMaterial.metallicFactor = 0.0f;
  floorMaterial.roughnessFactor = 0.82f;
  floorMaterial.uvTiling = {7.0f, 6.0f};
  m_floorMeshId = dx.CreateMeshResources(planeMesh, {}, floorMaterial);

  Material wallMaterial{};
  wallMaterial.baseColorFactor = {0.46f, 0.37f, 0.25f, 1.0f};
  wallMaterial.metallicFactor = 0.0f;
  wallMaterial.roughnessFactor = 0.92f;
  m_wallMeshId = dx.CreateMeshResources(cubeMesh, {}, wallMaterial);

  Material woodMaterial{};
  woodMaterial.baseColorFactor = {0.34f, 0.16f, 0.065f, 1.0f};
  woodMaterial.metallicFactor = 0.0f;
  woodMaterial.roughnessFactor = 0.62f;
  m_woodMeshId = dx.CreateMeshResources(cubeMesh, {}, woodMaterial);

  Material darkWoodMaterial{};
  darkWoodMaterial.baseColorFactor = {0.095f, 0.055f, 0.035f, 1.0f};
  darkWoodMaterial.metallicFactor = 0.0f;
  darkWoodMaterial.roughnessFactor = 0.74f;
  m_darkWoodMeshId = dx.CreateMeshResources(cubeMesh, {}, darkWoodMaterial);

  Material glowMaterial{};
  glowMaterial.baseColorFactor = {1.0f, 0.48f, 0.12f, 1.0f};
  glowMaterial.metallicFactor = 0.0f;
  glowMaterial.roughnessFactor = 0.18f;
  glowMaterial.emissiveFactor = {3.0f, 0.72f, 0.10f};
  glowMaterial.rayTracingVisible = false;
  m_glowMeshId = dx.CreateMeshResources(cubeMesh, {}, glowMaterial);

  Material customerBodyMaterial{};
  customerBodyMaterial.baseColorFactor = {0.16f, 0.42f, 0.52f, 1.0f};
  customerBodyMaterial.metallicFactor = 0.0f;
  customerBodyMaterial.roughnessFactor = 0.78f;
  m_customerBodyMeshId =
      dx.CreateMeshResources(bodyMesh, {}, customerBodyMaterial);

  Material customerHeadMaterial{};
  customerHeadMaterial.baseColorFactor = {0.92f, 0.72f, 0.52f, 1.0f};
  customerHeadMaterial.metallicFactor = 0.0f;
  customerHeadMaterial.roughnessFactor = 0.86f;
  m_customerHeadMeshId =
      dx.CreateMeshResources(headMesh, {}, customerHeadMaterial);

  Material mugMaterial{};
  mugMaterial.baseColorFactor = {0.48f, 0.50f, 0.47f, 1.0f};
  mugMaterial.metallicFactor = 0.42f;
  mugMaterial.roughnessFactor = 0.36f;
  m_mugMeshId = dx.CreateMeshResources(cylinderMesh, {}, mugMaterial);

  Material filledMugMaterial = mugMaterial;
  filledMugMaterial.baseColorFactor = {0.27f, 0.38f, 0.50f, 1.0f};
  filledMugMaterial.metallicFactor = 0.34f;
  filledMugMaterial.roughnessFactor = 0.42f;
  m_filledMugMeshId =
      dx.CreateMeshResources(cylinderMesh, {}, filledMugMaterial);

  Material dirtyMugMaterial = mugMaterial;
  dirtyMugMaterial.baseColorFactor = {0.20f, 0.46f, 0.16f, 1.0f};
  dirtyMugMaterial.metallicFactor = 0.10f;
  dirtyMugMaterial.roughnessFactor = 0.78f;
  m_dirtyMugMeshId =
      dx.CreateMeshResources(cylinderMesh, {}, dirtyMugMaterial);

  Material dirtySpotMaterial{};
  dirtySpotMaterial.baseColorFactor = {0.012f, 0.016f, 0.010f, 1.0f};
  dirtySpotMaterial.metallicFactor = 0.0f;
  dirtySpotMaterial.roughnessFactor = 0.94f;
  m_dirtySpotMeshId =
      dx.CreateMeshResources(headMesh, {}, dirtySpotMaterial);

  Material aleMaterial{};
  aleMaterial.baseColorFactor = {0.90f, 0.42f, 0.055f, 1.0f};
  aleMaterial.metallicFactor = 0.0f;
  aleMaterial.roughnessFactor = 0.20f;
  aleMaterial.emissiveFactor = {0.55f, 0.14f, 0.015f};
  aleMaterial.rayTracingVisible = false;
  m_aleMeshId = dx.CreateMeshResources(cylinderMesh, {}, aleMaterial);

  Material foamMaterial{};
  foamMaterial.baseColorFactor = {1.0f, 0.92f, 0.72f, 1.0f};
  foamMaterial.metallicFactor = 0.0f;
  foamMaterial.roughnessFactor = 0.72f;
  foamMaterial.emissiveFactor = {0.18f, 0.12f, 0.055f};
  foamMaterial.rayTracingVisible = false;
  m_foamMeshId = dx.CreateMeshResources(cylinderMesh, {}, foamMaterial);

  Material metalMaterial{};
  metalMaterial.baseColorFactor = {0.28f, 0.34f, 0.35f, 1.0f};
  metalMaterial.metallicFactor = 0.75f;
  metalMaterial.roughnessFactor = 0.30f;
  m_metalMeshId = dx.CreateMeshResources(cubeMesh, {}, metalMaterial);

  m_collisionColliders = {
      {CollisionSystem::ShapeType::Box,
       {-6.85f, 1.5f, 4.2f},
       {0.30f, 3.0f, 12.0f},
       0.0f,
       true},
      {CollisionSystem::ShapeType::Box,
       {6.85f, 1.5f, 4.2f},
       {0.30f, 3.0f, 12.0f},
       0.0f,
       true},
      {CollisionSystem::ShapeType::Box,
       {0.0f, 1.5f, 9.95f},
       {14.0f, 3.0f, 0.30f},
       0.0f,
       true},
      {CollisionSystem::ShapeType::Box,
       {0.0f, 1.5f, -1.65f},
       {14.0f, 3.0f, 0.22f},
       0.0f,
       true},
      {CollisionSystem::ShapeType::Box,
       {0.0f, 0.65f, 8.0f},
       {8.6f, 1.3f, 1.05f},
       0.0f,
       true},
      {CollisionSystem::ShapeType::Box,
       {-3.8f, 0.75f, 4.15f},
       {2.45f, 1.5f, 2.35f},
       0.0f,
       true},
      {CollisionSystem::ShapeType::Box,
       {0.0f, 0.75f, 4.15f},
       {2.45f, 1.5f, 2.35f},
       0.0f,
       true},
      {CollisionSystem::ShapeType::Box,
       {3.8f, 0.75f, 4.15f},
       {2.45f, 1.5f, 2.35f},
       0.0f,
       true},
  };

  m_ready = m_floorMeshId != UINT32_MAX && m_wallMeshId != UINT32_MAX &&
            m_woodMeshId != UINT32_MAX && m_darkWoodMeshId != UINT32_MAX &&
            m_glowMeshId != UINT32_MAX && m_customerBodyMeshId != UINT32_MAX &&
            m_customerHeadMeshId != UINT32_MAX && m_mugMeshId != UINT32_MAX &&
            m_filledMugMeshId != UINT32_MAX &&
            m_dirtyMugMeshId != UINT32_MAX &&
            m_dirtySpotMeshId != UINT32_MAX &&
            m_aleMeshId != UINT32_MAX && m_foamMeshId != UINT32_MAX &&
            m_metalMeshId != UINT32_MAX;
  Reset();
}

void TavernScene::SetBusinessHour(float hour) {
  m_businessHour = std::fmod(std::max(0.0f, hour), 24.0f);
}

void TavernScene::Reset(float startingHour) {
  m_shiftState = ShiftState::Running;
  SetBusinessHour(startingHour);
  m_spawnTimers = {2.0f, 0.0f, 0.0f};
  m_gold = 0;
  m_servedCustomers = 0;
  m_walkouts = 0;
  m_completedCycles = 0;
  m_completedDays = 0;
  m_tableCompletedCycles = {};
  m_tables = {};
  m_counterMugs = {};
  m_tables[0].enabled = true;
  m_tables[1].enabled = false;
  m_heldItem = HeldItem::None;
  m_workState = WorkState::None;
  m_tutorialStep = TutorialStep::TakeOrder;
  m_playerPosition = PlayerSpawnPosition();
  m_nearbyPrompt = "WASD：移動　E：調べる";
  m_feedbackText.clear();
  m_aleFill = 0.0f;
  m_aleFoam = 0.0f;
  m_aleOverflow = 0.0f;
  m_pourQuality = 1.0f;
  m_washProgress = 0.0f;
  m_interactionCooldown = 0.35f;
  m_feedbackTimer = 0.0f;
  m_pourVisualTime = 0.0f;
  m_cleanMugs = kTotalMugs;
  m_perfectPours = 0;
  m_heldMugTableIndex = -1;
  m_tutorialTableIndex = 0;
  m_workActionStarted = false;
  m_cycleAwaitingWash = false;
  m_primaryActionActive = false;
  m_lastPourPerfect = false;
}

void TavernScene::BeginNextDay(float startingHour) {
  const int gold = m_gold;
  const int servedCustomers = m_servedCustomers;
  const int completedCycles = m_completedCycles;
  const int perfectPours = m_perfectPours;
  const int completedDays = m_completedDays + 1;
  const std::array<int, 3> tableCompletedCycles = m_tableCompletedCycles;
  const TutorialStep tutorialStep = m_tutorialStep;

  Reset(startingHour);
  m_gold = gold;
  m_servedCustomers = servedCustomers;
  m_completedCycles = completedCycles;
  m_perfectPours = perfectPours;
  m_completedDays = completedDays;
  m_tableCompletedCycles = tableCompletedCycles;
  m_tutorialStep = tutorialStep;
  if (m_tutorialStep == TutorialStep::Complete) {
    m_tables[1].enabled = true;
    m_spawnTimers[1] = 4.0f;
  }
  m_feedbackText = "翌朝 5:00　新しい一日が始まった";
  m_feedbackTimer = 2.4f;
}

void TavernScene::SpawnCustomer(int tableIndex) {
  if (tableIndex < 0 || tableIndex >= static_cast<int>(m_tables.size()))
    return;
  TableSlot &table = m_tables[tableIndex];
  if (!table.enabled || table.state != TableState::Empty)
    return;
  table.state = TableState::Arriving;
  table.stateTimer = 0.8f;
  table.satisfaction = 100.0f;
  table.servedCustomer = false;
  table.complaintPlayed = false;
  table.speech = "すみません！";
  table.speechTimer = 2.0f;
}

void TavernScene::TakeOrder(int tableIndex) {
  TableSlot &table = m_tables[tableIndex];
  if (CanServeCustomers() && table.state == TableState::WaitingOrder) {
    table.state = TableState::WaitingAle;
    table.speech = "エールを一杯頼む！";
    table.speechTimer = 2.6f;
    m_feedbackText = "注文を受けた：水鏡エール";
    m_feedbackTimer = 1.6f;
    if (m_tutorialStep == TutorialStep::TakeOrder) {
      m_tutorialTableIndex = tableIndex;
      m_tutorialStep = TutorialStep::GetMug;
    }
  }
}

void TavernScene::PickUpEmptyMug() {
  if (m_heldItem == HeldItem::None && m_cleanMugs > 0) {
    m_heldItem = HeldItem::EmptyMug;
    --m_cleanMugs;
    if (m_tutorialStep == TutorialStep::GetMug)
      m_tutorialStep = TutorialStep::StartPour;
  }
}

void TavernScene::ReturnEmptyMug() {
  if (m_heldItem != HeldItem::EmptyMug)
    return;
  m_heldItem = HeldItem::None;
  m_cleanMugs = std::min(kTotalMugs, m_cleanMugs + 1);
}

void TavernScene::PlaceHeldMug(int slotIndex) {
  if (slotIndex < 0 ||
      slotIndex >= static_cast<int>(m_counterMugs.size()) ||
      m_heldItem == HeldItem::None ||
      m_counterMugs[slotIndex].item != HeldItem::None)
    return;

  MugState &mug = m_counterMugs[slotIndex];
  mug.item = m_heldItem;
  mug.aleFill = m_aleFill;
  mug.aleFoam = m_aleFoam;
  mug.pourQuality = m_pourQuality;
  mug.sourceTableIndex = m_heldMugTableIndex;
  mug.cycleAwaitingWash = m_cycleAwaitingWash;
  mug.lastPourPerfect = m_lastPourPerfect;

  m_heldItem = HeldItem::None;
  m_aleFill = 0.0f;
  m_aleFoam = 0.0f;
  m_pourQuality = 1.0f;
  m_heldMugTableIndex = -1;
  m_cycleAwaitingWash = false;
  m_lastPourPerfect = false;
  m_feedbackText = "ジョッキをカウンターに置いた";
  m_feedbackTimer = 1.2f;
}

void TavernScene::PickUpPlacedMug(int slotIndex) {
  if (slotIndex < 0 ||
      slotIndex >= static_cast<int>(m_counterMugs.size()) ||
      m_heldItem != HeldItem::None ||
      m_counterMugs[slotIndex].item == HeldItem::None)
    return;

  MugState &mug = m_counterMugs[slotIndex];
  m_heldItem = mug.item;
  m_aleFill = mug.aleFill;
  m_aleFoam = mug.aleFoam;
  m_pourQuality = mug.pourQuality;
  m_heldMugTableIndex = mug.sourceTableIndex;
  m_cycleAwaitingWash = mug.cycleAwaitingWash;
  m_lastPourPerfect = mug.lastPourPerfect;
  mug = {};
  m_feedbackText = "カウンターのジョッキを取った";
  m_feedbackTimer = 1.2f;
}

void TavernScene::BeginPouring() {
  if (m_heldItem != HeldItem::EmptyMug || m_workState != WorkState::None)
    return;
  m_workState = WorkState::PouringAle;
  m_aleFill = 0.0f;
  m_aleFoam = 0.0f;
  m_aleOverflow = 0.0f;
  m_pourQuality = 0.0f;
  m_workActionStarted = false;
  if (m_tutorialStep == TutorialStep::StartPour)
    m_tutorialStep = TutorialStep::PourAle;
}

void TavernScene::FinishPouring() {
  if (m_workState != WorkState::PouringAle)
    return;

  const float targetError = std::abs(m_aleFill - 0.90f);
  m_pourQuality = std::clamp(1.0f - targetError * 1.65f - m_aleFoam * 0.18f -
                                 m_aleOverflow * 3.0f,
                             0.20f, 1.0f);
  m_lastPourPerfect = m_aleFill >= kPerfectPourMinimum &&
                      m_aleFill <= kPerfectPourMaximum &&
                      m_aleOverflow <= 0.001f;
  if (m_lastPourPerfect) {
    ++m_perfectPours;
    m_feedbackText = "PERFECT POUR!";
  } else if (m_aleOverflow > 0.001f) {
    m_feedbackText = "OVERFLOW";
  } else if (m_aleFill < kPerfectPourMinimum) {
    m_feedbackText = "もう少し注げそうだ";
  } else {
    m_feedbackText = "泡が多すぎる";
  }
  m_feedbackTimer = 1.4f;
  m_heldItem = HeldItem::FilledMug;
  m_workState = WorkState::None;
  m_workActionStarted = false;
  if (m_tutorialStep == TutorialStep::PourAle)
    m_tutorialStep = TutorialStep::ServeAle;
}

void TavernScene::ServeAle(int tableIndex) {
  TableSlot &table = m_tables[tableIndex];
  if (!CanServeCustomers() || table.state != TableState::WaitingAle ||
      m_heldItem != HeldItem::FilledMug)
    return;

  constexpr int baseGold = 5;
  const int pourBonus =
      m_lastPourPerfect ? 5 : (m_pourQuality >= 0.70f ? 2 : 0);
  const int patienceBonus =
      table.satisfaction >= 80.0f ? 3 : (table.satisfaction >= 50.0f ? 1 : 0);
  const int earnedGold = baseGold + pourBonus + patienceBonus;
  m_gold += earnedGold;
  ++m_servedCustomers;
  table.state = TableState::Eating;
  table.stateTimer = 4.0f;
  table.servedCustomer = true;
  table.speech =
      m_lastPourPerfect
          ? "最高の一杯だ！"
          : (m_pourQuality >= 0.70f ? "ありがとう！" : "少し物足りないな…");
  table.speechTimer = 2.8f;
  m_heldItem = HeldItem::None;
  if (m_tutorialStep == TutorialStep::ServeAle) {
    m_tutorialTableIndex = tableIndex;
    m_tutorialStep = TutorialStep::CollectMug;
  }
  char feedback[128]{};
  std::snprintf(feedback, sizeof(feedback), "+%d G  基本%d + 注ぎ%d + 待ち%d",
                earnedGold, baseGold, pourBonus, patienceBonus);
  m_feedbackText = feedback;
  m_feedbackTimer = 1.8f;
}

void TavernScene::CollectDirtyMug(int tableIndex) {
  TableSlot &table = m_tables[tableIndex];
  if (table.state != TableState::Dirty || m_heldItem != HeldItem::None)
    return;
  m_heldItem = HeldItem::DirtyMug;
  m_heldMugTableIndex = tableIndex;
  table = {};
  table.enabled = true;
  m_spawnTimers[tableIndex] = CustomerSpawnDelay(tableIndex);
  m_cycleAwaitingWash = true;
  if (m_tutorialStep == TutorialStep::CollectMug) {
    m_tutorialTableIndex = tableIndex;
    m_tutorialStep = TutorialStep::WashMug;
  }
}

void TavernScene::BeginWashing() {
  if (m_heldItem != HeldItem::DirtyMug || m_workState != WorkState::None)
    return;
  m_workState = WorkState::WashingMug;
  m_washProgress = 0.0f;
  m_workActionStarted = false;
}

void TavernScene::FinishWashing() {
  if (m_workState != WorkState::WashingMug)
    return;
  m_workState = WorkState::None;
  m_heldItem = HeldItem::EmptyMug;
  m_washProgress = 1.0f;
  m_workActionStarted = false;
  if (m_cycleAwaitingWash) {
    ++m_completedCycles;
    if (m_heldMugTableIndex >= 0 &&
        m_heldMugTableIndex < static_cast<int>(m_tableCompletedCycles.size()))
      ++m_tableCompletedCycles[m_heldMugTableIndex];
    m_heldMugTableIndex = -1;
    m_cycleAwaitingWash = false;
  }
  if (m_tutorialStep == TutorialStep::WashMug) {
    m_tutorialStep = TutorialStep::Complete;
    m_feedbackText = "TUTORIAL COMPLETE!";
    m_feedbackTimer = 2.4f;
    if (!m_tables[1].enabled && m_shiftState == ShiftState::Running) {
      m_tables[1].enabled = true;
      m_spawnTimers[1] = 4.0f;
    }
  }
}

void TavernScene::TriggerWalkout(int tableIndex) {
  TableSlot &table = m_tables[tableIndex];
  table.state = TableState::Leaving;
  table.stateTimer = 0.8f;
  table.servedCustomer = false;
  table.speech = "もう待てない！";
  table.speechTimer = 1.4f;
  ++m_walkouts;
}

int TavernScene::NearestEnabledTable(float maximumDistance) const {
  int nearestTable = -1;
  float nearestDistance = maximumDistance;
  for (int tableIndex = 0; tableIndex < kActiveTableCount; ++tableIndex) {
    if (!m_tables[tableIndex].enabled)
      continue;
    const float distance =
        DistanceXZ(m_playerPosition, kTableInteractions[tableIndex]);
    if (distance <= nearestDistance) {
      nearestDistance = distance;
      nearestTable = tableIndex;
    }
  }
  return nearestTable;
}

int TavernScene::FindTableInState(TableState state) const {
  for (int tableIndex = 0; tableIndex < kActiveTableCount; ++tableIndex) {
    if (m_tables[tableIndex].enabled && m_tables[tableIndex].state == state)
      return tableIndex;
  }
  return -1;
}

int TavernScene::FindMostUrgentWaitingAleTable() const {
  int urgentTable = -1;
  float lowestSatisfaction = 101.0f;
  for (int tableIndex = 0; tableIndex < kActiveTableCount; ++tableIndex) {
    const TableSlot &table = m_tables[tableIndex];
    if (table.enabled && table.state == TableState::WaitingAle &&
        table.satisfaction < lowestSatisfaction) {
      lowestSatisfaction = table.satisfaction;
      urgentTable = tableIndex;
    }
  }
  return urgentTable;
}

int TavernScene::NearestCounterMugSlot(float maximumDistance) const {
  int nearestSlot = -1;
  float nearestDistance = maximumDistance;
  for (int slotIndex = 0;
       slotIndex < static_cast<int>(kCounterMugInteractions.size());
       ++slotIndex) {
    const float distance =
        DistanceXZ(m_playerPosition, kCounterMugInteractions[slotIndex]);
    if (distance <= nearestDistance) {
      nearestDistance = distance;
      nearestSlot = slotIndex;
    }
  }
  return nearestSlot;
}

bool TavernScene::HasActiveCustomers() const {
  for (int tableIndex = 0; tableIndex < kActiveTableCount; ++tableIndex) {
    const TableSlot &table = m_tables[tableIndex];
    if (!table.enabled)
      continue;
    if (table.state != TableState::Empty && table.state != TableState::Dirty)
      return true;
  }
  return false;
}

bool TavernScene::IsAfterMidnight() const {
  return m_businessHour < kBusinessOpenHour;
}

float TavernScene::CustomerSpawnDelay(int tableIndex) const {
  float delay = 7.0f;
  if (m_businessHour >= 5.0f && m_businessHour < 12.0f)
    delay = 3.5f;
  else if (m_businessHour >= 12.0f && m_businessHour < 18.0f)
    delay = 5.0f;
  else if (m_businessHour >= 18.0f)
    delay = 2.5f;
  return delay + static_cast<float>(tableIndex) * 1.15f;
}

const char *TavernScene::CustomerTrafficName() const {
  if (m_shiftState == ShiftState::Closing)
    return "就寝準備";
  if (m_businessHour < 5.0f)
    return "静かな深夜";
  if (m_businessHour < 12.0f)
    return "朝の客足";
  if (m_businessHour < 18.0f)
    return "穏やかな午後";
  return "夜のピーク";
}

bool TavernScene::CanServeCustomers() const {
  return m_shiftState == ShiftState::Running ||
         m_shiftState == ShiftState::Closing;
}

uint32_t TavernScene::MugMeshForState(HeldItem item) const {
  switch (item) {
  case HeldItem::FilledMug:
    return m_filledMugMeshId;
  case HeldItem::DirtyMug:
    return m_dirtyMugMeshId;
  case HeldItem::None:
  case HeldItem::EmptyMug:
    return m_mugMeshId;
  }
  return m_mugMeshId;
}

void TavernScene::RunAutomation() {
  if (!CanServeCustomers() || m_workState != WorkState::None)
    return;

  const int orderTable = FindTableInState(TableState::WaitingOrder);
  if (orderTable >= 0) {
    TakeOrder(orderTable);
    return;
  }

  const int aleTable = FindMostUrgentWaitingAleTable();
  if (m_heldItem == HeldItem::FilledMug) {
    if (aleTable >= 0)
      ServeAle(aleTable);
    return;
  }
  if (m_heldItem == HeldItem::DirtyMug) {
    BeginWashing();
    return;
  }
  if (m_heldItem == HeldItem::EmptyMug) {
    if (aleTable >= 0)
      BeginPouring();
    else
      ReturnEmptyMug();
    return;
  }
  if (aleTable >= 0 && m_cleanMugs > 0) {
    PickUpEmptyMug();
    return;
  }

  const int dirtyTable = FindTableInState(TableState::Dirty);
  if (dirtyTable >= 0)
    CollectDirtyMug(dirtyTable);
}

void TavernScene::UpdateNearbyPrompt() {
  if (m_workState == WorkState::PouringAle) {
    m_nearbyPrompt = "左クリック長押し：注ぐ　ちょうど良い量で離す";
    return;
  }
  if (m_workState == WorkState::WashingMug) {
    m_nearbyPrompt = "左クリック長押し：ジョッキを洗う";
    return;
  }
  if (m_shiftState == ShiftState::Complete) {
    m_nearbyPrompt = "営業終了　R：眠って翌朝 5:00 へ　入口で E：外へ戻る";
    return;
  }
  if (m_shiftState == ShiftState::Failed) {
    m_nearbyPrompt = "営業失敗　R：再挑戦　入口で E：外へ戻る";
    return;
  }

  const int nearbyTable = NearestEnabledTable(kTableInteractionRange);
  if (DistanceXZ(m_playerPosition, kExitInteraction) <=
      kStationInteractionRange) {
    m_nearbyPrompt = "E：外へ戻る";
  } else if (nearbyTable >= 0) {
    const TableSlot &table = m_tables[nearbyTable];
    const std::string tableName = "TABLE " + std::to_string(nearbyTable + 1);
    if (table.state == TableState::WaitingOrder)
      m_nearbyPrompt = "E：" + tableName + " の注文を聞く";
    else if (table.state == TableState::WaitingAle &&
             m_heldItem == HeldItem::FilledMug)
      m_nearbyPrompt = "E：" + tableName + " に水鏡エールを渡す";
    else if (table.state == TableState::Dirty && m_heldItem == HeldItem::None)
      m_nearbyPrompt = "E：" + tableName + " のジョッキを回収";
    else if (table.state == TableState::WaitingAle)
      m_nearbyPrompt = tableName + "　注文：水鏡エール";
    else
      m_nearbyPrompt =
          tableName + "：" + std::string(GetTableStateName(table.state));
  } else if (const int counterSlot =
                 NearestCounterMugSlot(kCounterMugInteractionRange);
             counterSlot >= 0) {
    const MugState &mug = m_counterMugs[counterSlot];
    if (m_heldItem == HeldItem::None && mug.item != HeldItem::None)
      m_nearbyPrompt = "E：置いたジョッキを取る";
    else if (m_heldItem != HeldItem::None && mug.item == HeldItem::None)
      m_nearbyPrompt = "E：持っているジョッキを置く";
    else if (m_heldItem == HeldItem::None)
      m_nearbyPrompt = "カウンター：ジョッキを一時置きできる";
    else
      m_nearbyPrompt = "この場所にはジョッキが置かれている";
  } else if (DistanceXZ(m_playerPosition, kMugRackInteraction) <=
             kStationInteractionRange) {
    if (m_heldItem == HeldItem::None && m_cleanMugs > 0)
      m_nearbyPrompt = "E：空のジョッキを取る";
    else if (m_heldItem == HeldItem::None)
      m_nearbyPrompt = "空のジョッキがない：使用後に洗う";
    else if (m_heldItem == HeldItem::EmptyMug)
      m_nearbyPrompt = "E：ジョッキを棚へ戻す";
    else
      m_nearbyPrompt = "ジョッキ棚";
  } else if (DistanceXZ(m_playerPosition, kAleTapInteraction) <=
             kStationInteractionRange) {
    m_nearbyPrompt = m_heldItem == HeldItem::EmptyMug
                         ? "E：ジョッキを置いてエールを注ぐ"
                         : "エール樽：空のジョッキが必要";
  } else if (DistanceXZ(m_playerPosition, kWashBasinInteraction) <=
             kStationInteractionRange) {
    if (m_heldItem == HeldItem::DirtyMug)
      m_nearbyPrompt = "E：ジョッキを洗い始める";
    else if (m_heldItem == HeldItem::FilledMug)
      m_nearbyPrompt = "E：エールを捨てる";
    else
      m_nearbyPrompt = "洗い場：汚れたジョッキを持ってくる";
  } else if (const int orderTable = FindTableInState(TableState::WaitingOrder);
             orderTable >= 0) {
    m_nearbyPrompt =
        "TABLE " + std::to_string(orderTable + 1) + " の客に注文を聞きに行く";
  } else if (FindMostUrgentWaitingAleTable() >= 0 &&
             m_heldItem == HeldItem::None && m_cleanMugs > 0) {
    m_nearbyPrompt = "カウンター左の棚からジョッキを取る";
  } else if (FindMostUrgentWaitingAleTable() >= 0 &&
             m_heldItem == HeldItem::None) {
    m_nearbyPrompt = "使用済みジョッキを回収して洗う";
  } else if (m_heldItem == HeldItem::EmptyMug &&
             FindTableInState(TableState::Dirty) >= 0 &&
             FindMostUrgentWaitingAleTable() < 0) {
    m_nearbyPrompt = "空のジョッキを棚へ戻して、使用済みを回収する";
  } else if (m_heldItem == HeldItem::EmptyMug) {
    m_nearbyPrompt = "カウンター中央のエール樽へ運ぶ";
  } else if (m_heldItem == HeldItem::FilledMug) {
    const int aleTable = FindMostUrgentWaitingAleTable();
    m_nearbyPrompt =
        aleTable >= 0
            ? "水鏡エールを TABLE " + std::to_string(aleTable + 1) + " へ運ぶ"
            : "注文待ちのテーブルへ運ぶ";
  } else if (m_heldItem == HeldItem::DirtyMug) {
    m_nearbyPrompt = "カウンター右の洗い場へ運ぶ";
  } else if (const int dirtyTable = FindTableInState(TableState::Dirty);
             dirtyTable >= 0) {
    m_nearbyPrompt = "TABLE " + std::to_string(dirtyTable + 1) +
                     " の使用済みジョッキを回収する";
  } else if (m_shiftState == ShiftState::Closing) {
    m_nearbyPrompt = "就寝準備中　店内の客を見送ろう";
  } else if (IsAfterMidnight()) {
    m_nearbyPrompt = "深夜営業中　R：受付を止めて眠る";
  } else {
    m_nearbyPrompt = "WASD：移動　E：調べる";
  }
}

TavernScene::Action
TavernScene::Update(float deltaSeconds, const XMFLOAT3 &playerPosition,
                    bool interactPressed, bool primaryActionDown,
                    bool restartPressed, bool automateGameplay) {
  m_playerPosition = playerPosition;
  const float dt = std::clamp(deltaSeconds, 0.0f, 0.25f);
  m_interactionCooldown = std::max(0.0f, m_interactionCooldown - dt);
  m_feedbackTimer = std::max(0.0f, m_feedbackTimer - dt);
  if (m_feedbackTimer <= 0.0f)
    m_feedbackText.clear();
  m_pourVisualTime += dt;
  m_primaryActionActive = false;

  if (restartPressed) {
    if (m_shiftState == ShiftState::Failed ||
        m_shiftState == ShiftState::Complete) {
      return Action::SleepUntilMorning;
    }
    if (m_shiftState == ShiftState::Running) {
      if (IsAfterMidnight()) {
        m_shiftState = ShiftState::Closing;
        m_feedbackText = "受付終了　店内の客を見送ってから眠る";
        m_feedbackTimer = 2.4f;
      } else {
        m_feedbackText = "眠れるのは 0:00 以降";
        m_feedbackTimer = 1.8f;
      }
    }
  }

  if (automateGameplay && m_shiftState == ShiftState::Running &&
      IsAfterMidnight() && m_businessHour >= 0.25f) {
    m_shiftState = ShiftState::Closing;
    m_feedbackText = "受付終了　店内の客を見送ってから眠る";
    m_feedbackTimer = 2.4f;
  }

  if (automateGameplay)
    RunAutomation();

  if (m_workState == WorkState::PouringAle) {
    const bool shouldPour =
        automateGameplay ? m_aleFill < 0.90f : primaryActionDown;
    if (shouldPour) {
      m_primaryActionActive = true;
      m_workActionStarted = true;
      const float nextFill = m_aleFill + dt * 0.34f;
      if (nextFill > 1.0f)
        m_aleOverflow += (nextFill - 1.0f) * 1.25f;
      m_aleFill = std::min(1.0f, nextFill);
      if (m_aleFill > kPerfectPourMinimum)
        m_aleFoam = std::min(0.35f, m_aleFoam + dt * 0.11f);
    } else if (m_workActionStarted) {
      FinishPouring();
    }
  } else if (m_workState == WorkState::WashingMug) {
    const bool shouldWash = automateGameplay || primaryActionDown;
    if (shouldWash) {
      m_workActionStarted = true;
      m_washProgress = std::min(1.0f, m_washProgress + dt / 1.20f);
      if (m_washProgress >= 1.0f)
        FinishWashing();
    }
  }

  for (TableSlot &table : m_tables) {
    table.speechTimer = std::max(0.0f, table.speechTimer - dt);
    if (table.speechTimer <= 0.0f)
      table.speech.clear();
  }

  if (CanServeCustomers()) {
    for (int tableIndex = 0; tableIndex < kActiveTableCount; ++tableIndex) {
      TableSlot &table = m_tables[tableIndex];
      if (!table.enabled)
        continue;
      switch (table.state) {
      case TableState::Empty:
        if (m_shiftState == ShiftState::Running) {
          m_spawnTimers[tableIndex] -= dt;
          if (m_spawnTimers[tableIndex] <= 0.0f)
            SpawnCustomer(tableIndex);
        }
        break;
      case TableState::Arriving:
        table.stateTimer -= dt;
        if (table.stateTimer <= 0.0f) {
          table.state = TableState::WaitingOrder;
          table.speech = "注文をお願い！";
          table.speechTimer = 2.2f;
        }
        break;
      case TableState::WaitingOrder:
        table.satisfaction -= 2.0f * dt;
        break;
      case TableState::WaitingAle:
        table.satisfaction -= 4.0f * dt;
        if (table.satisfaction <= 35.0f && !table.complaintPlayed) {
          table.complaintPlayed = true;
          table.speech = "まだかい？";
          table.speechTimer = 2.0f;
        }
        break;
      case TableState::Eating:
        table.stateTimer -= dt;
        if (table.stateTimer <= 0.0f) {
          table.state = TableState::Leaving;
          table.stateTimer = 0.8f;
          table.speech = "また来るよ！";
          table.speechTimer = 0.8f;
        }
        break;
      case TableState::Leaving:
        table.stateTimer -= dt;
        if (table.stateTimer <= 0.0f) {
          if (table.servedCustomer) {
            table.state = TableState::Dirty;
            table.speech.clear();
            table.speechTimer = 0.0f;
          } else {
            table = {};
            table.enabled = true;
            m_spawnTimers[tableIndex] = CustomerSpawnDelay(tableIndex);
          }
        }
        break;
      case TableState::Dirty:
        break;
      }

      if ((table.state == TableState::WaitingOrder ||
           table.state == TableState::WaitingAle) &&
          table.satisfaction <= 0.0f) {
        table.satisfaction = 0.0f;
        TriggerWalkout(tableIndex);
      }
    }
  }

  if (m_shiftState == ShiftState::Closing && !HasActiveCustomers())
    return Action::SleepUntilMorning;

  if (!automateGameplay && interactPressed && m_interactionCooldown <= 0.0f) {
    if (m_workState != WorkState::None) {
      m_workState = WorkState::None;
      m_workActionStarted = false;
      m_aleFill = 0.0f;
      m_washProgress = 0.0f;
      m_interactionCooldown = 0.20f;
    } else if (DistanceXZ(m_playerPosition, kExitInteraction) <=
               kStationInteractionRange) {
      return Action::ReturnToOverworld;
    } else if (CanServeCustomers() &&
               NearestEnabledTable(kTableInteractionRange) >= 0) {
      const int tableIndex = NearestEnabledTable(kTableInteractionRange);
      TableSlot &table = m_tables[tableIndex];
      if (table.state == TableState::WaitingOrder)
        TakeOrder(tableIndex);
      else if (table.state == TableState::WaitingAle &&
               m_heldItem == HeldItem::FilledMug)
        ServeAle(tableIndex);
      else if (table.state == TableState::Dirty && m_heldItem == HeldItem::None)
        CollectDirtyMug(tableIndex);
    } else if (const int counterSlot =
                   NearestCounterMugSlot(kCounterMugInteractionRange);
               counterSlot >= 0) {
      if (m_heldItem == HeldItem::None)
        PickUpPlacedMug(counterSlot);
      else
        PlaceHeldMug(counterSlot);
    } else if (DistanceXZ(m_playerPosition, kMugRackInteraction) <=
               kStationInteractionRange) {
      if (m_heldItem == HeldItem::None)
        PickUpEmptyMug();
      else if (m_heldItem == HeldItem::EmptyMug)
        ReturnEmptyMug();
    } else if (DistanceXZ(m_playerPosition, kAleTapInteraction) <=
                   kStationInteractionRange &&
               m_heldItem == HeldItem::EmptyMug) {
      BeginPouring();
    } else if (DistanceXZ(m_playerPosition, kWashBasinInteraction) <=
               kStationInteractionRange) {
      if (m_heldItem == HeldItem::DirtyMug)
        BeginWashing();
      else if (m_heldItem == HeldItem::FilledMug) {
        m_heldItem = HeldItem::EmptyMug;
        m_aleFill = 0.0f;
        m_aleFoam = 0.0f;
      }
    }
  }

  if (automateGameplay)
    RunAutomation();
  UpdateNearbyPrompt();
  return Action::None;
}

void TavernScene::BuildFrame(FrameData &frame) const {
  if (!m_ready)
    return;

  const auto pushMesh = [&frame](uint32_t meshId, float sx, float sy, float sz,
                                 float x, float y, float z) {
    frame.opaqueItems.push_back(
        {meshId, XMMatrixScaling(sx, sy, sz) * XMMatrixTranslation(x, y, z)});
  };
  const auto pushDirtySpots = [&](const XMFLOAT3 &position) {
    pushMesh(m_dirtySpotMeshId, 0.055f, 0.045f, 0.028f, position.x - 0.11f,
             position.y + 0.08f, position.z - 0.18f);
    pushMesh(m_dirtySpotMeshId, 0.038f, 0.032f, 0.022f, position.x + 0.12f,
             position.y - 0.04f, position.z - 0.18f);
    pushMesh(m_dirtySpotMeshId, 0.030f, 0.026f, 0.020f, position.x + 0.02f,
             position.y + 0.17f, position.z - 0.18f);
  };
  const auto pushStateMug = [&](HeldItem item, const XMFLOAT3 &position,
                                float aleFill, float aleFoam) {
    if (item == HeldItem::None)
      return;
    pushMesh(MugMeshForState(item), kMugScaleXZ, kMugScaleY, kMugScaleXZ,
             position.x, position.y, position.z);
    if (item == HeldItem::DirtyMug)
      pushDirtySpots(position);
    if (item == HeldItem::FilledMug) {
      const float visibleFill = std::max(0.12f, aleFill);
      pushMesh(m_aleMeshId, kAleSurfaceScaleXZ, 0.06f, kAleSurfaceScaleXZ,
               position.x, position.y - 0.19f + visibleFill * 0.34f,
               position.z);
      if (aleFoam > 0.002f) {
        const float foamScale = 0.045f + aleFoam * 0.16f;
        pushMesh(m_foamMeshId, 0.265f, foamScale, 0.265f, position.x,
                 position.y - 0.15f + visibleFill * 0.34f, position.z);
      }
    }
  };

  pushMesh(m_floorMeshId, 14.0f, 1.0f, 12.0f, 0.0f, 0.0f, 4.2f);
  pushMesh(m_wallMeshId, 14.0f, 4.8f, 0.34f, 0.0f, 2.4f, 10.1f);
  pushMesh(m_wallMeshId, 0.34f, 4.8f, 12.0f, -7.0f, 2.4f, 4.2f);
  pushMesh(m_wallMeshId, 0.34f, 4.8f, 12.0f, 7.0f, 2.4f, 4.2f);

  pushMesh(m_darkWoodMeshId, 8.6f, 1.25f, 1.05f, 0.0f, 0.625f, 8.0f);
  pushMesh(m_woodMeshId, 8.9f, 0.16f, 1.25f, 0.0f, 1.32f, 8.0f);
  pushMesh(m_darkWoodMeshId, 5.8f, 0.18f, 0.65f, 0.0f, 2.25f, 9.55f);
  pushMesh(m_darkWoodMeshId, 5.8f, 0.18f, 0.65f, 0.0f, 3.35f, 9.55f);

  constexpr float tableX[3] = {-3.8f, 0.0f, 3.8f};
  for (int tableIndex = 0; tableIndex < 3; ++tableIndex) {
    const float x = tableX[tableIndex];
    pushMesh(m_woodMeshId, 2.35f, 0.16f, 1.65f, x, 0.95f, 4.15f);
    pushMesh(m_darkWoodMeshId, 0.18f, 0.90f, 0.18f, x - 0.82f, 0.45f, 3.62f);
    pushMesh(m_darkWoodMeshId, 0.18f, 0.90f, 0.18f, x + 0.82f, 0.45f, 3.62f);
    pushMesh(m_darkWoodMeshId, 0.18f, 0.90f, 0.18f, x - 0.82f, 0.45f, 4.68f);
    pushMesh(m_darkWoodMeshId, 0.18f, 0.90f, 0.18f, x + 0.82f, 0.45f, 4.68f);
    pushMesh(m_darkWoodMeshId, 1.05f, 0.52f, 0.58f, x, 0.26f, 2.95f);
    pushMesh(m_darkWoodMeshId, 1.05f, 0.52f, 0.58f, x, 0.26f, 5.35f);
    if (tableIndex < kActiveTableCount)
      pushMesh(m_glowMeshId, 0.22f, 0.26f, 0.22f, x, 1.18f, 4.15f);
  }

  // 左からジョッキ棚、エール樽、洗い場。直接触る仕事場として色分けする。
  pushMesh(m_darkWoodMeshId, 1.55f, 0.16f, 0.58f, -2.7f, 1.65f, 7.72f);
  for (int mugIndex = 0; mugIndex < m_cleanMugs; ++mugIndex) {
    pushMesh(m_mugMeshId, kMugScaleXZ, kMugScaleY, kMugScaleXZ,
             -2.92f + static_cast<float>(mugIndex) * 0.42f, 1.98f, 7.72f);
  }

  pushMesh(m_darkWoodMeshId, 1.20f, 0.07f, 0.58f, -1.20f, 1.43f, 7.65f);
  for (int slotIndex = 0;
       slotIndex < static_cast<int>(m_counterMugs.size()); ++slotIndex) {
    const MugState &mug = m_counterMugs[slotIndex];
    if (mug.item == HeldItem::None) {
      const XMFLOAT3 &slotPosition = kCounterMugPositions[slotIndex];
      pushMesh(m_glowMeshId, 0.22f, 0.018f, 0.22f, slotPosition.x, 1.48f,
               slotPosition.z);
    }
    pushStateMug(mug.item, kCounterMugPositions[slotIndex], mug.aleFill,
                 mug.aleFoam);
  }

  pushMesh(m_woodMeshId, 1.15f, 1.55f, 0.95f, 0.0f, 2.12f, 9.40f);
  pushMesh(m_metalMeshId, 0.20f, 0.82f, 0.20f, 0.0f, 2.12f, 8.55f);
  pushMesh(m_metalMeshId, 0.58f, 0.16f, 0.20f, 0.0f, 2.42f, 8.25f);
  pushMesh(m_glowMeshId, 0.18f, 0.18f, 0.18f, 0.0f, 2.72f, 8.30f);

  pushMesh(m_metalMeshId, 1.55f, 0.24f, 0.88f, 2.7f, 1.50f, 7.78f);
  pushMesh(m_glowMeshId, 1.18f, 0.05f, 0.58f, 2.7f, 1.65f, 7.78f);

  for (int tableIndex = 0; tableIndex < kActiveTableCount; ++tableIndex) {
    const TableState tableState = m_tables[tableIndex].state;
    const bool customerVisible =
        tableState != TableState::Empty && tableState != TableState::Dirty;
    if (customerVisible) {
      pushMesh(m_customerBodyMeshId, 0.82f, 1.05f, 0.82f, tableX[tableIndex],
               1.18f, 5.28f);
      pushMesh(m_customerHeadMeshId, 0.92f, 0.92f, 0.92f, tableX[tableIndex],
               1.96f, 5.22f);
      if (tableState == TableState::WaitingOrder ||
          tableState == TableState::WaitingAle)
        pushMesh(m_glowMeshId, 0.16f, 0.52f, 0.16f, tableX[tableIndex], 2.72f,
                 5.18f);
    }

    if (tableState == TableState::Eating || tableState == TableState::Dirty) {
      const XMFLOAT3 tableMugPosition = {tableX[tableIndex], 1.27f, 4.32f};
      if (tableState == TableState::Eating)
        pushStateMug(HeldItem::FilledMug, tableMugPosition, 0.90f, 0.04f);
      else
        pushStateMug(HeldItem::DirtyMug, tableMugPosition, 0.0f, 0.0f);
    }
  }

  XMFLOAT3 carriedMugPosition = {m_playerPosition.x + 0.48f, 1.03f,
                                 m_playerPosition.z + 0.10f};
  if (m_workState == WorkState::PouringAle)
    carriedMugPosition = {0.0f, 1.64f, 7.38f};
  else if (m_workState == WorkState::WashingMug)
    carriedMugPosition = {2.7f, 1.86f, 7.65f};

  if (m_heldItem != HeldItem::None) {
    if (m_workState == WorkState::PouringAle) {
      pushMesh(m_mugMeshId, kMugScaleXZ, kMugScaleY, kMugScaleXZ,
               carriedMugPosition.x, carriedMugPosition.y,
               carriedMugPosition.z);
      const float visibleFill = m_heldItem == HeldItem::FilledMug
                                    ? std::max(0.12f, m_aleFill)
                                    : m_aleFill;
      if (visibleFill > 0.01f) {
        pushMesh(m_aleMeshId, kAleSurfaceScaleXZ, 0.06f, kAleSurfaceScaleXZ,
                 carriedMugPosition.x,
                 carriedMugPosition.y - 0.19f + visibleFill * 0.34f,
                 carriedMugPosition.z);
        if (m_aleFoam > 0.002f) {
          const float foamScale = 0.045f + m_aleFoam * 0.16f;
          pushMesh(m_foamMeshId, 0.265f, foamScale, 0.265f,
                   carriedMugPosition.x,
                   carriedMugPosition.y - 0.15f + visibleFill * 0.34f,
                   carriedMugPosition.z);
        }
      }
    } else {
      pushStateMug(m_heldItem, carriedMugPosition, m_aleFill, m_aleFoam);
    }
  }

  if (m_workState == WorkState::PouringAle && m_primaryActionActive) {
    const float pulse = 0.015f * std::sin(m_pourVisualTime * 24.0f);
    pushMesh(m_aleMeshId, 0.06f + pulse, 0.70f, 0.06f + pulse, 0.0f, 2.23f,
             7.38f);
    if (m_aleFill > 0.70f) {
      pushMesh(m_foamMeshId, 0.08f, 0.07f, 0.08f, -0.28f, 1.93f, 7.38f);
      pushMesh(m_foamMeshId, 0.06f, 0.05f, 0.06f, 0.30f, 1.89f, 7.35f);
    }
    if (m_aleOverflow > 0.001f) {
      const float drop = std::fmod(m_pourVisualTime * 1.8f, 0.55f);
      pushMesh(m_aleMeshId, 0.055f, 0.09f, 0.055f, 0.33f, 1.74f - drop, 7.38f);
      pushMesh(m_aleMeshId, 0.18f, 0.025f, 0.12f, 0.34f, 1.39f, 7.38f);
    }
  }

  if (m_tutorialStep != TutorialStep::Complete) {
    XMFLOAT3 tutorialMarker = kTableInteractions[m_tutorialTableIndex];
    switch (m_tutorialStep) {
    case TutorialStep::TakeOrder: {
      const int orderTable = FindTableInState(TableState::WaitingOrder);
      const int targetTable = orderTable >= 0 ? orderTable : 0;
      tutorialMarker = kTableInteractions[targetTable];
      break;
    }
    case TutorialStep::GetMug:
      tutorialMarker = kMugRackInteraction;
      break;
    case TutorialStep::StartPour:
    case TutorialStep::PourAle:
      tutorialMarker = kAleTapInteraction;
      break;
    case TutorialStep::ServeAle: {
      const int aleTable = FindMostUrgentWaitingAleTable();
      const int targetTable = aleTable >= 0 ? aleTable : m_tutorialTableIndex;
      tutorialMarker = kTableInteractions[targetTable];
      break;
    }
    case TutorialStep::CollectMug:
      tutorialMarker = kTableInteractions[m_tutorialTableIndex];
      break;
    case TutorialStep::WashMug:
      tutorialMarker = kWashBasinInteraction;
      break;
    case TutorialStep::Complete:
      break;
    }
    const float markerPulse = 1.0f + 0.14f * std::sin(m_pourVisualTime * 5.5f);
    pushMesh(m_glowMeshId, 0.16f * markerPulse, 0.58f, 0.16f * markerPulse,
             tutorialMarker.x, 2.48f, tutorialMarker.z);
    pushMesh(m_glowMeshId, 0.24f * markerPulse, 0.12f, 0.24f * markerPulse,
             tutorialMarker.x, 1.98f, tutorialMarker.z);
  }

  GPUPointLight counterLight{};
  counterLight.position = {0.0f, 3.15f, 7.2f};
  counterLight.range = 10.0f;
  counterLight.color = {1.0f, 0.48f, 0.18f};
  counterLight.intensity = 8.0f;
  frame.pointLights.push_back(counterLight);

  for (float x : tableX) {
    GPUPointLight tableLight{};
    tableLight.position = {x, 1.75f, 4.15f};
    tableLight.range = 4.2f;
    tableLight.color = {1.0f, 0.56f, 0.24f};
    tableLight.intensity = 3.2f;
    frame.pointLights.push_back(tableLight);
  }
}

TavernScene::Action TavernScene::DrawHud(int viewportWidth,
                                         int viewportHeight) {
  const ImVec2 viewportSize(static_cast<float>(viewportWidth),
                            static_cast<float>(viewportHeight));
  const ImU32 goldBorder = TavernUiColor(0.92f, 0.55f, 0.24f, 0.80f);

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(viewportSize, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ImGui::Begin("##TavernHud", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoScrollWithMouse |
                   ImGuiWindowFlags_NoInputs);

  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 statusMin(28.0f, 28.0f);
  const bool showSleepStatus = IsAfterMidnight() ||
                               m_shiftState == ShiftState::Closing;
  const ImVec2 statusMax(348.0f, showSleepStatus ? 174.0f : 142.0f);
  DrawPanel(draw, statusMin, statusMax, goldBorder);
  draw->AddText(ImVec2(50.0f, 45.0f), TavernUiColor(1.0f, 0.84f, 0.62f, 1.0f),
                "水鏡亭");
  char line[160]{};
  const int businessMinutes =
      static_cast<int>(std::lround(m_businessHour * 60.0f));
  std::snprintf(line, sizeof(line), "時刻  %02d:%02d    売上  %d G",
                businessMinutes / 60, businessMinutes % 60, m_gold);
  draw->AddText(ImVec2(50.0f, 76.0f), TavernUiColor(0.95f, 0.88f, 0.78f, 1.0f),
                line);
  std::snprintf(line, sizeof(line), "%s　提供 %d　退店 %d / 3",
                CustomerTrafficName(), m_servedCustomers, m_walkouts);
  draw->AddText(ImVec2(50.0f, 106.0f), TavernUiColor(0.82f, 0.76f, 0.68f, 1.0f),
                line);
  if (showSleepStatus) {
    draw->AddText(
        ImVec2(50.0f, 136.0f),
        TavernUiColor(0.68f, 0.82f, 1.0f, 1.0f),
        m_shiftState == ShiftState::Closing ? "店内の客を見送って就寝"
                                             : "R：受付を止めて就寝");
  }

  const float orderWidth = std::clamp(viewportSize.x - 760.0f, 520.0f, 680.0f);
  const ImVec2 orderMin((viewportSize.x - orderWidth) * 0.5f, 28.0f);
  const ImVec2 orderMax(orderMin.x + orderWidth, 174.0f);
  DrawPanel(draw, orderMin, orderMax, goldBorder);
  const float tableCardWidth = orderWidth * 0.5f;
  draw->AddLine(ImVec2(orderMin.x + tableCardWidth, orderMin.y + 12.0f),
                ImVec2(orderMin.x + tableCardWidth, orderMax.y - 12.0f),
                TavernUiColor(0.45f, 0.27f, 0.14f, 0.82f), 1.0f);
  for (int tableIndex = 0; tableIndex < kActiveTableCount; ++tableIndex) {
    const TableSlot &table = m_tables[tableIndex];
    const float cardX = orderMin.x + tableCardWidth * tableIndex;
    std::snprintf(line, sizeof(line), "TABLE %d%s", tableIndex + 1,
                  table.enabled ? "" : "  準備中");
    draw->AddText(ImVec2(cardX + 18.0f, orderMin.y + 15.0f),
                  TavernUiColor(1.0f, 0.84f, 0.62f, 1.0f), line);
    const char *orderText = !table.enabled
                                ? "初回研修後に開放"
                                : (table.state == TableState::WaitingAle
                                       ? "注文：水鏡エール"
                                       : GetTableStateName(table.state));
    draw->AddText(ImVec2(cardX + 18.0f, orderMin.y + 44.0f),
                  TavernUiColor(0.94f, 0.87f, 0.77f, 1.0f), orderText);
    std::snprintf(line, sizeof(line), "満足度 %.0f",
                  std::clamp(table.satisfaction, 0.0f, 100.0f));
    draw->AddText(ImVec2(cardX + 18.0f, orderMin.y + 73.0f),
                  TavernUiColor(0.82f, 0.76f, 0.68f, 1.0f), line);
    const ImVec2 satMin(cardX + 18.0f, orderMin.y + 94.0f);
    const ImVec2 satMax(cardX + tableCardWidth - 18.0f, orderMin.y + 104.0f);
    draw->AddRectFilled(satMin, satMax,
                        TavernUiColor(0.15f, 0.11f, 0.08f, 1.0f), 4.0f);
    const float satisfaction =
        table.enabled ? std::clamp(table.satisfaction / 100.0f, 0.0f, 1.0f)
                      : 0.0f;
    const ImU32 satisfactionColor =
        satisfaction < 0.35f ? TavernUiColor(0.92f, 0.22f, 0.14f, 1.0f)
                             : TavernUiColor(0.28f, 0.78f, 0.48f, 1.0f);
    draw->AddRectFilled(
        satMin,
        ImVec2(satMin.x + (satMax.x - satMin.x) * satisfaction, satMax.y),
        satisfactionColor, 4.0f);
    if (!table.speech.empty() && table.speechTimer > 0.0f) {
      const ImVec2 speechMin(cardX + 18.0f, orderMin.y + 116.0f);
      const ImVec2 speechMax(cardX + tableCardWidth - 18.0f,
                             orderMin.y + 140.0f);
      draw->AddRectFilled(speechMin, speechMax,
                          TavernUiColor(0.18f, 0.12f, 0.075f, 0.96f), 7.0f);
      const std::string speechText = "「" + table.speech + "」";
      draw->AddText(ImVec2(speechMin.x + 8.0f, speechMin.y + 4.0f),
                    TavernUiColor(1.0f, 0.91f, 0.72f, 1.0f),
                    speechText.c_str());
    }
  }

  const ImVec2 heldMin(viewportSize.x - 278.0f, 28.0f);
  const ImVec2 heldMax(viewportSize.x - 28.0f, 142.0f);
  DrawPanel(draw, heldMin, heldMax, goldBorder);
  draw->AddText(ImVec2(heldMin.x + 18.0f, heldMin.y + 14.0f),
                TavernUiColor(0.72f, 0.66f, 0.58f, 1.0f), "持ち物");
  draw->AddText(ImVec2(heldMin.x + 18.0f, heldMin.y + 43.0f),
                TavernUiColor(1.0f, 0.82f, 0.56f, 1.0f),
                GetHeldItemName(m_heldItem));
  std::snprintf(line, sizeof(line), "棚のジョッキ  %d / %d", m_cleanMugs,
                kTotalMugs);
  draw->AddText(ImVec2(heldMin.x + 18.0f, heldMin.y + 75.0f),
                TavernUiColor(0.82f, 0.76f, 0.68f, 1.0f), line);

  if (m_tutorialStep != TutorialStep::Complete) {
    std::string tutorialTitle;
    std::string tutorialLineOne;
    std::string tutorialLineTwo;
    switch (m_tutorialStep) {
    case TutorialStep::TakeOrder: {
      tutorialTitle = "初回チュートリアル　STEP 1 / 6";
      const int orderTable = FindTableInState(TableState::WaitingOrder);
      if (orderTable >= 0) {
        tutorialLineOne = "左側の TABLE " + std::to_string(orderTable + 1) +
                          "、光る印へ近づく";
        tutorialLineTwo = "下の表示に E が出たら、E で注文を聞く";
      } else {
        tutorialLineOne = "左側の TABLE 1 を見よう";
        tutorialLineTwo = "お客が席に着くまで少し待つ";
      }
      break;
    }
    case TutorialStep::GetMug:
      tutorialTitle = "初回チュートリアル　STEP 2 / 6";
      tutorialLineOne = "カウンター左のジョッキ棚へ行く";
      tutorialLineTwo = "光る印へ近づき、E で空のジョッキを取る";
      break;
    case TutorialStep::StartPour:
      tutorialTitle = "初回チュートリアル　STEP 3 / 6";
      tutorialLineOne = "カウンター中央のエール樽へ運ぶ";
      tutorialLineTwo = "光る印へ近づき、E で注ぎ始める";
      break;
    case TutorialStep::PourAle:
      tutorialTitle = "初回チュートリアル　STEP 3 / 6";
      tutorialLineOne = "左クリックを押し続けてエールを注ぐ";
      tutorialLineTwo = "82～96% の緑色範囲で離す";
      break;
    case TutorialStep::ServeAle:
      tutorialTitle = "初回チュートリアル　STEP 4 / 6";
      tutorialLineOne = "TABLE " + std::to_string(m_tutorialTableIndex + 1) +
                        " へエールを運ぶ";
      tutorialLineTwo = "客の近くで E：エールを渡す";
      break;
    case TutorialStep::CollectMug:
      tutorialTitle = "初回チュートリアル　STEP 5 / 6";
      if (m_tables[m_tutorialTableIndex].state == TableState::Dirty) {
        tutorialLineOne =
            "TABLE " + std::to_string(m_tutorialTableIndex + 1) + " へ戻る";
        tutorialLineTwo = "客の使用済みジョッキを E で回収する";
      } else {
        tutorialLineOne = "客が飲み終わるまで少し待つ";
        tutorialLineTwo = "テーブルに使用済みジョッキが残る";
      }
      break;
    case TutorialStep::WashMug:
      tutorialTitle = "初回チュートリアル　STEP 6 / 6";
      tutorialLineOne = "カウンター右の洗い場へ運ぶ";
      tutorialLineTwo = "E の後、左クリック長押しで洗う";
      break;
    case TutorialStep::Complete:
      break;
    }

    const ImVec2 tutorialMin(28.0f, 200.0f);
    const ImVec2 tutorialMax(448.0f, 328.0f);
    DrawPanel(draw, tutorialMin, tutorialMax,
              TavernUiColor(1.0f, 0.72f, 0.18f, 0.92f), 12.0f);
    draw->AddText(ImVec2(tutorialMin.x + 20.0f, tutorialMin.y + 16.0f),
                  TavernUiColor(1.0f, 0.84f, 0.52f, 1.0f),
                  tutorialTitle.c_str());
    draw->AddText(ImVec2(tutorialMin.x + 20.0f, tutorialMin.y + 51.0f),
                  TavernUiColor(0.96f, 0.90f, 0.82f, 1.0f),
                  tutorialLineOne.c_str());
    draw->AddText(ImVec2(tutorialMin.x + 20.0f, tutorialMin.y + 82.0f),
                  TavernUiColor(0.82f, 0.76f, 0.68f, 1.0f),
                  tutorialLineTwo.c_str());
  }

  if (m_workState == WorkState::PouringAle) {
    const ImVec2 panelSize(420.0f, 470.0f);
    const ImVec2 panelMin((viewportSize.x - panelSize.x) * 0.5f,
                          (viewportSize.y - panelSize.y) * 0.52f);
    const ImVec2 panelMax(panelMin.x + panelSize.x, panelMin.y + panelSize.y);
    DrawPanel(draw, panelMin, panelMax, goldBorder, 14.0f);
    draw->AddText(ImVec2(panelMin.x + 30.0f, panelMin.y + 25.0f),
                  TavernUiColor(1.0f, 0.82f, 0.56f, 1.0f), "水鏡エールを注ぐ");
    draw->AddText(ImVec2(panelMin.x + 30.0f, panelMin.y + 57.0f),
                  TavernUiColor(0.82f, 0.76f, 0.68f, 1.0f),
                  "左クリックを押し続け、適量で離す");

    const ImVec2 mugMin(panelMin.x + 135.0f, panelMin.y + 105.0f);
    const ImVec2 mugMax(panelMin.x + 285.0f, panelMin.y + 365.0f);
    draw->AddRectFilled(mugMin, mugMax,
                        TavernUiColor(0.11f, 0.09f, 0.07f, 0.92f), 16.0f);
    draw->AddRect(mugMin, mugMax, TavernUiColor(0.70f, 0.72f, 0.68f, 1.0f),
                  16.0f, 0, 5.0f);
    const float innerHeight = mugMax.y - mugMin.y - 16.0f;
    const float liquidTop =
        mugMax.y - 8.0f - innerHeight * std::clamp(m_aleFill, 0.0f, 1.0f);
    draw->AddRectFilled(ImVec2(mugMin.x + 8.0f, liquidTop),
                        ImVec2(mugMax.x - 8.0f, mugMax.y - 8.0f),
                        TavernUiColor(0.94f, 0.43f, 0.055f, 0.96f), 10.0f);
    if (m_primaryActionActive) {
      draw->AddRectFilled(ImVec2(panelMin.x + 207.0f, panelMin.y + 82.0f),
                          ImVec2(panelMin.x + 213.0f, panelMin.y + 112.0f),
                          TavernUiColor(0.98f, 0.52f, 0.08f, 0.95f), 3.0f);
    }
    if (m_aleFoam > 0.002f && m_aleFill > 0.01f) {
      const float foamHeight =
          std::min(innerHeight * 0.20f, innerHeight * m_aleFoam);
      draw->AddRectFilled(
          ImVec2(mugMin.x + 8.0f, liquidTop),
          ImVec2(mugMax.x - 8.0f,
                 std::min(mugMax.y - 8.0f, liquidTop + foamHeight)),
          TavernUiColor(1.0f, 0.94f, 0.76f, 0.98f), 8.0f);
    }
    const float idealLowY = mugMax.y - 8.0f - innerHeight * kPerfectPourMinimum;
    const float idealHighY =
        mugMax.y - 8.0f - innerHeight * kPerfectPourMaximum;
    draw->AddLine(ImVec2(mugMin.x - 14.0f, idealLowY),
                  ImVec2(mugMax.x + 14.0f, idealLowY),
                  TavernUiColor(0.30f, 0.92f, 0.52f, 0.95f), 2.0f);
    draw->AddLine(ImVec2(mugMin.x - 14.0f, idealHighY),
                  ImVec2(mugMax.x + 14.0f, idealHighY),
                  TavernUiColor(0.30f, 0.92f, 0.52f, 0.95f), 2.0f);
    std::snprintf(line, sizeof(line), "%d%%",
                  static_cast<int>(std::lround(m_aleFill * 100.0f)));
    draw->AddText(ImVec2(panelMin.x + 190.0f, panelMin.y + 392.0f),
                  TavernUiColor(1.0f, 0.84f, 0.62f, 1.0f), line);
    if (m_aleOverflow > 0.001f) {
      draw->AddText(ImVec2(panelMin.x + 160.0f, panelMin.y + 426.0f),
                    TavernUiColor(1.0f, 0.25f, 0.12f, 1.0f), "OVERFLOW!");
    }
  } else if (m_workState == WorkState::WashingMug) {
    const ImVec2 panelSize(470.0f, 170.0f);
    const ImVec2 panelMin((viewportSize.x - panelSize.x) * 0.5f,
                          viewportSize.y * 0.42f);
    const ImVec2 panelMax(panelMin.x + panelSize.x, panelMin.y + panelSize.y);
    DrawPanel(draw, panelMin, panelMax, goldBorder, 14.0f);
    draw->AddText(ImVec2(panelMin.x + 28.0f, panelMin.y + 24.0f),
                  TavernUiColor(1.0f, 0.84f, 0.62f, 1.0f), "ジョッキを洗う");
    draw->AddText(ImVec2(panelMin.x + 28.0f, panelMin.y + 54.0f),
                  TavernUiColor(0.82f, 0.76f, 0.68f, 1.0f),
                  "左クリックを押し続ける");
    const ImVec2 washMin(panelMin.x + 28.0f, panelMin.y + 105.0f);
    const ImVec2 washMax(panelMax.x - 28.0f, panelMin.y + 132.0f);
    draw->AddRectFilled(washMin, washMax,
                        TavernUiColor(0.15f, 0.11f, 0.08f, 1.0f), 7.0f);
    draw->AddRectFilled(
        washMin,
        ImVec2(washMin.x + (washMax.x - washMin.x) * m_washProgress, washMax.y),
        TavernUiColor(0.24f, 0.72f, 0.86f, 1.0f), 7.0f);
  }

  if (!m_feedbackText.empty() && m_feedbackTimer > 0.0f) {
    const ImVec2 feedbackSize = ImGui::CalcTextSize(m_feedbackText.c_str());
    const float feedbackAlpha = std::clamp(m_feedbackTimer, 0.0f, 1.0f);
    const ImVec2 feedbackPosition((viewportSize.x - feedbackSize.x) * 0.5f,
                                  192.0f);
    draw->AddText(ImVec2(feedbackPosition.x + 2.0f, feedbackPosition.y + 2.0f),
                  TavernUiColor(0.0f, 0.0f, 0.0f, feedbackAlpha),
                  m_feedbackText.c_str());
    draw->AddText(feedbackPosition,
                  TavernUiColor(1.0f, 0.78f, 0.24f, feedbackAlpha),
                  m_feedbackText.c_str());
  }

  const float promptWidth = std::min(760.0f, viewportSize.x - 56.0f);
  const ImVec2 promptMin((viewportSize.x - promptWidth) * 0.5f,
                         viewportSize.y - 92.0f);
  const ImVec2 promptMax(promptMin.x + promptWidth, viewportSize.y - 28.0f);
  DrawPanel(draw, promptMin, promptMax, goldBorder);
  const ImVec2 promptTextSize = ImGui::CalcTextSize(m_nearbyPrompt.c_str());
  draw->AddText(
      ImVec2((viewportSize.x - promptTextSize.x) * 0.5f, promptMin.y + 22.0f),
      TavernUiColor(1.0f, 0.86f, 0.65f, 1.0f), m_nearbyPrompt.c_str());

  if (m_shiftState == ShiftState::Complete ||
      m_shiftState == ShiftState::Failed) {
    draw->AddRectFilled(ImVec2(0.0f, 0.0f), viewportSize,
                        TavernUiColor(0.0f, 0.0f, 0.0f, 0.45f));
    const ImVec2 resultSize(560.0f, 245.0f);
    const ImVec2 resultMin((viewportSize.x - resultSize.x) * 0.5f,
                           (viewportSize.y - resultSize.y) * 0.45f);
    const ImVec2 resultMax(resultMin.x + resultSize.x,
                           resultMin.y + resultSize.y);
    DrawPanel(draw, resultMin, resultMax, goldBorder, 14.0f);
    draw->AddText(ImVec2(resultMin.x + 32.0f, resultMin.y + 28.0f),
                  TavernUiColor(1.0f, 0.82f, 0.56f, 1.0f),
                  m_shiftState == ShiftState::Complete ? "営業終了"
                                                       : "営業失敗");
    std::snprintf(line, sizeof(line), "売上 %d G　提供 %d　PERFECT %d　退店 %d",
                  m_gold, m_servedCustomers, m_perfectPours, m_walkouts);
    draw->AddText(ImVec2(resultMin.x + 32.0f, resultMin.y + 88.0f),
                  TavernUiColor(0.90f, 0.84f, 0.76f, 1.0f), line);
    draw->AddText(ImVec2(resultMin.x + 32.0f, resultMin.y + 150.0f),
                  TavernUiColor(0.82f, 0.76f, 0.68f, 1.0f),
                  m_shiftState == ShiftState::Complete
                      ? "R：眠って翌朝 5:00 へ　入口で E：外へ戻る"
                      : "R：翌朝 5:00 から再挑戦　入口で E：外へ戻る");
  }

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  return Action::None;
}

void TavernScene::DrawDebugPanel(float &timeOfDayHours, bool &automaticTime) {
  ImGui::SetNextWindowPos(ImVec2(20.0f, 190.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(330.0f, 0.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("酒場 Debug")) {
    ImGui::End();
    return;
  }

  ImGui::TextDisabled("F4：表示／非表示");
  ImGui::Text("通常速度：1 秒 = 1 分");
  ImGui::Checkbox("時間を進める", &automaticTime);

  const float normalizedHour =
      std::fmod(std::max(0.0f, timeOfDayHours), 24.0f);
  const int timeMinutes =
      static_cast<int>(std::lround(normalizedHour * 60.0f)) % (24 * 60);
  int debugHour = timeMinutes / 60;
  int debugMinute = timeMinutes % 60;
  bool timeChanged = ImGui::SliderInt("Hour（時）", &debugHour, 0, 23);
  if (ImGui::SliderInt("Minute（分）", &debugMinute, 0, 59))
    timeChanged = true;
  if (timeChanged)
    timeOfDayHours = static_cast<float>(debugHour) +
                     static_cast<float>(debugMinute) / 60.0f;
  ImGui::Text("現在時刻：%02d:%02d", debugHour, debugMinute);

  const auto setTime = [&](int hour, int minute) {
    timeOfDayHours =
        static_cast<float>(hour) + static_cast<float>(minute) / 60.0f;
  };
  if (ImGui::Button("05:00"))
    setTime(5, 0);
  ImGui::SameLine();
  if (ImGui::Button("12:00"))
    setTime(12, 0);
  ImGui::SameLine();
  if (ImGui::Button("18:00"))
    setTime(18, 0);
  ImGui::SameLine();
  if (ImGui::Button("23:50"))
    setTime(23, 50);
  ImGui::SameLine();
  if (ImGui::Button("00:30"))
    setTime(0, 30);

  ImGui::Separator();
  int debugGold = m_gold;
  if (ImGui::InputInt("Gold (G)", &debugGold, 1, 10))
    m_gold = std::clamp(debugGold, 0, 999999);
  if (ImGui::Button("Gold 0"))
    m_gold = 0;
  ImGui::SameLine();
  if (ImGui::Button("Gold +10"))
    m_gold = std::min(999999, m_gold + 10);
  ImGui::SameLine();
  if (ImGui::Button("Gold +100"))
    m_gold = std::min(999999, m_gold + 100);

  const int placedMugs = static_cast<int>(std::count_if(
      m_counterMugs.begin(), m_counterMugs.end(),
      [](const MugState &mug) { return mug.item != HeldItem::None; }));
  ImGui::Separator();
  ImGui::Text("客流：%s", CustomerTrafficName());
  ImGui::Text("提供：%d  退店：%d  PERFECT：%d", m_servedCustomers,
              m_walkouts, m_perfectPours);
  ImGui::Text("手持：%s", GetHeldItemName(m_heldItem));
  ImGui::Text("棚のジョッキ：%d / %d  一時置き：%d / %zu", m_cleanMugs,
              kTotalMugs, placedMugs, m_counterMugs.size());

  ImGui::End();
}
