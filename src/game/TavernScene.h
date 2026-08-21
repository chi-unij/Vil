#pragma once

#include "DxContext.h"
#include "RenderPass.h"
#include "game/CollisionSystem.h"

#include <DirectXMath.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class TavernScene {
public:
  enum class Action {
    None,
    ReturnToOverworld,
    SleepUntilMorning,
  };

  enum class SupplyOrderBlockReason {
    None,
    Full,
    ZeroQuantity,
    InsufficientGold,
  };

  enum class UpgradeId : int {
    ExtraMug = 0,
    AleCapacity = 1,
    OpenTable3 = 2,
    Count = 3,
  };

  enum class UpgradePurchaseBlockReason {
    None,
    InsufficientGold,
    Owned,
    Maxed,
    TutorialRequired,
    EmergencyAleReserve,
  };

  struct ManagementUiDiagnostics {
    bool menuOpen = false;
    bool rootRendered = false;
    bool suppliesCardRendered = false;
    bool upgradesCardRendered = false;
    bool suppliesPageRendered = false;
    bool aleCardRendered = false;
    bool upgradesPageRendered = false;
    bool upgradeCardsRendered = false;
    bool upgradeConfirmationRendered = false;
    bool backControlRendered = false;
    bool labelsInRequestedOrder = false;
    bool visualRegionsValid = false;
    bool cardsDoNotOverlap = false;
    bool subpageOpen = false;
    bool orderCanPurchase = false;
    int selectedCardIndex = -1;
    int aleStock = 0;
    int aleCapacity = 0;
    int orderQuantity = 0;
    int orderUnitPrice = 0;
    int orderTotal = 0;
    int aleStockAfterOrder = 0;
    int selectedUpgradeIndex = -1;
    int selectedUpgradePrice = 0;
    int selectedUpgradeLevel = 0;
    int extraMugCapacity = 2;
    int aleCapacityLevel = 0;
    bool table3Unlocked = false;
    bool upgradeCanPurchase = false;
    SupplyOrderBlockReason orderBlockReason =
        SupplyOrderBlockReason::ZeroQuantity;
    UpgradePurchaseBlockReason upgradeBlockReason =
        UpgradePurchaseBlockReason::None;
    uint64_t activationSerial = 0;
    uint64_t purchaseSerial = 0;
    uint64_t upgradePurchaseSerial = 0;
    uint64_t renderedFrameSerial = 0;
  };

  struct ManagementInput {
    bool previousPressed = false;
    bool nextPressed = false;
    bool confirmPressed = false;
    bool cancelPressed = false;
  };

  void Initialize(DxContext &dx);
  void Reset(float startingHour = 5.0f);
  void BeginNextDay(float startingHour = 5.0f);
  void SetBusinessHour(float hour);
  Action Update(float deltaSeconds, const DirectX::XMFLOAT3 &playerPosition,
                bool interactPressed, bool primaryActionDown,
                bool restartPressed,
                const ManagementInput &managementInput = {},
                bool automateGameplay = false);
  void BuildFrame(FrameData &frame) const;
  Action DrawHud(int viewportWidth, int viewportHeight);
  void DrawDebugPanel(float &timeOfDayHours, bool &automaticTime);

  bool IsReady() const { return m_ready; }
  bool ImportedArtReady() const { return m_importedArtReady; }
  bool GameplaySmokeComplete() const {
    return m_tableCompletedCycles[0] >= 1 && m_tableCompletedCycles[1] >= 1;
  }
  bool DayCycleSmokeComplete() const {
    return m_completedDays >= 1 && GameplaySmokeComplete();
  }
  bool PlayerMovementLocked() const;
  bool ManagementMenuOpen() const;
  DirectX::XMFLOAT3 ManagementInteractionPosition() const;
  const ManagementUiDiagnostics &GetManagementUiDiagnostics() const {
    return m_managementUiDiagnostics;
  }
  int CompletedCycles() const { return m_completedCycles; }
  int Gold() const { return m_gold; }
  int AleStock() const;
  int AleCapacity() const;
  int AleCapacityLevel() const;
  int TotalMugs() const;
  bool ExtraMugOwned() const { return m_extraMugLevel > 0; }
  bool Table3Unlocked() const { return m_table3Unlocked; }
  bool Table3Enabled() const { return m_tables[2].enabled; }
  bool AlePourInProgress() const;
  int PendingAleOrderQuantity() const { return m_aleOrderQuantity; }
  int AleUnitPrice() const;
  uint64_t SupplyPurchaseSerial() const { return m_supplyPurchaseSerial; }
  uint64_t UpgradePurchaseSerial() const { return m_upgradePurchaseSerial; }
  void ConfigureSuppliesSmokeState(int aleStock, int gold);
  void ConfigureUpgradesSmokeState(int gold, int aleStock,
                                   bool tutorialComplete);
  int ServedCustomers() const { return m_servedCustomers; }
  int Walkouts() const { return m_walkouts; }
  int TableCompletedCycles(int tableIndex) const {
    return tableIndex >= 0 && tableIndex < static_cast<int>(m_tables.size())
               ? m_tableCompletedCycles[static_cast<std::size_t>(tableIndex)]
               : 0;
  }
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
  enum class ManagementPage { Closed, Root, Supplies, Upgrades };
  enum class ManagementSelection { None, Supplies, Upgrades };
  enum class SupplyType : std::size_t { Ale, Count };
  enum class TavernAsset : std::size_t {
    Barrel,
    Crate,
    Stool,
    Chest,
    RoundTable,
    LongTable,
    Bench,
    Mug,
    Plate,
    Sword,
    Halberd,
    Mace,
    Candle,
    Count,
  };
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

  struct MugState {
    HeldItem item = HeldItem::None;
    float aleFill = 0.0f;
    float aleFoam = 0.0f;
    float pourQuality = 1.0f;
    int sourceTableIndex = -1;
    bool cycleAwaitingWash = false;
    bool lastPourPerfect = false;
  };

  struct SupplyState {
    int current = 0;
    int capacity = 0;
  };

  static const char *GetTableStateName(TableState state);
  static const char *GetHeldItemName(HeldItem item);
  void SpawnCustomer(int tableIndex);
  void TakeOrder(int tableIndex);
  void PickUpEmptyMug();
  void ReturnEmptyMug();
  void PlaceHeldMug(int slotIndex);
  void PickUpPlacedMug(int slotIndex);
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
  bool ManagementInteractionHasPriority() const;
  int FindTableInState(TableState state) const;
  int FindMostUrgentWaitingAleTable() const;
  int NearestCounterMugSlot(float maximumDistance) const;
  bool HasActiveCustomers() const;
  bool IsAfterMidnight() const;
  float CustomerSpawnDelay(int tableIndex) const;
  const char *CustomerTrafficName() const;
  bool CanServeCustomers() const;
  uint32_t MugMeshForState(HeldItem item) const;
  SupplyState &AleSupply();
  const SupplyState &AleSupply() const;
  int MaximumAleOrderQuantity() const;
  SupplyOrderBlockReason CurrentAleOrderBlockReason() const;
  void AdjustAleOrderQuantity(int delta);
  bool TryOrderAle();
  int CustomerTableCount() const;
  int UpgradeLevel(UpgradeId upgrade) const;
  int UpgradeMaximumLevel(UpgradeId upgrade) const;
  int UpgradePrice(UpgradeId upgrade) const;
  UpgradePurchaseBlockReason CurrentUpgradeBlockReason(UpgradeId upgrade) const;
  void RequestUpgradePurchase(UpgradeId upgrade);
  bool TryPurchaseUpgrade(UpgradeId upgrade);
  void ResetShiftRuntime(float startingHour);
  void OpenManagementMenu();
  void OpenSuppliesPage();
  void OpenUpgradesPage();
  void BackToManagementRoot();
  void CloseManagementMenu();
  void DrawManagementUi(int viewportWidth, int viewportHeight);
  void DrawManagementRoot(int viewportWidth, int viewportHeight);
  void DrawSuppliesPage(int viewportWidth, int viewportHeight);
  void DrawUpgradesPage(int viewportWidth, int viewportHeight);

  uint32_t m_floorMeshId = UINT32_MAX;
  uint32_t m_wallMeshId = UINT32_MAX;
  uint32_t m_woodMeshId = UINT32_MAX;
  uint32_t m_darkWoodMeshId = UINT32_MAX;
  uint32_t m_glowMeshId = UINT32_MAX;
  uint32_t m_customerBodyMeshId = UINT32_MAX;
  uint32_t m_customerHeadMeshId = UINT32_MAX;
  uint32_t m_customerHairMeshId = UINT32_MAX;
  uint32_t m_mugMeshId = UINT32_MAX;
  uint32_t m_filledMugMeshId = UINT32_MAX;
  uint32_t m_dirtyMugMeshId = UINT32_MAX;
  uint32_t m_dirtySpotMeshId = UINT32_MAX;
  uint32_t m_aleMeshId = UINT32_MAX;
  uint32_t m_foamMeshId = UINT32_MAX;
  uint32_t m_metalMeshId = UINT32_MAX;
  uint32_t m_waterMeshId = UINT32_MAX;
  std::array<std::vector<uint32_t>,
             static_cast<std::size_t>(TavernAsset::Count)>
      m_tavernAssetMeshIds{};

  ShiftState m_shiftState = ShiftState::Running;
  std::array<TableSlot, 3> m_tables{};
  std::array<MugState, 2> m_counterMugs{};
  HeldItem m_heldItem = HeldItem::None;
  WorkState m_workState = WorkState::None;
  ManagementPage m_managementPage = ManagementPage::Closed;
  ManagementSelection m_managementSelection = ManagementSelection::None;
  ManagementUiDiagnostics m_managementUiDiagnostics{};
  uint64_t m_managementActivationSerial = 0;
  uint64_t m_supplyPurchaseSerial = 0;
  uint64_t m_upgradePurchaseSerial = 0;
  TutorialStep m_tutorialStep = TutorialStep::TakeOrder;
  DirectX::XMFLOAT3 m_playerPosition = {0.0f, 0.0f, -0.35f};
  std::vector<CollisionSystem::Collider> m_collisionColliders;
  std::string m_nearbyPrompt;
  std::string m_feedbackText;
  float m_businessHour = 5.0f;
  std::array<float, 3> m_spawnTimers{};
  std::array<int, 3> m_tableCompletedCycles{};
  std::array<SupplyState, static_cast<std::size_t>(SupplyType::Count)>
      m_supplies{};
  int m_aleOrderQuantity = 0;
  int m_selectedUpgradeIndex = 0;
  int m_extraMugLevel = 0;
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
  int m_completedDays = 0;
  int m_cleanMugs = 2;
  int m_perfectPours = 0;
  int m_heldMugTableIndex = -1;
  int m_tutorialTableIndex = 0;
  bool m_workActionStarted = false;
  bool m_cycleAwaitingWash = false;
  bool m_primaryActionActive = false;
  bool m_lastPourPerfect = false;
  bool m_table3Unlocked = false;
  bool m_upgradeConfirmationOpen = false;
  bool m_upgradeConfirmBuySelected = false;
  bool m_importedArtReady = false;
  bool m_ready = false;
};
