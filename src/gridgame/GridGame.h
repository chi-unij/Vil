// ======================================
// File: GridGame.h
// Purpose: Top-level game manager for Grid Gauntlet. Replaces the old
//          GameManager. Follows the same interface pattern: Init/Update/
//          Shutdown/GetState/WantsQuit. Game code populates FrameData each
//          frame — never touches DxContext after init.
// ======================================

#pragma once

#include "GridGameState.h"
#include "GridMap.h"
#include "StageData.h"

#include "../AnimationPlayer.h"
#include "../Camera.h"
#include "../Input.h"
#include "../RenderPass.h"
#include "../Win32Window.h"
#include "../particle.h"

#include <cstdint>
#include <memory>
#include <vector>

class DxContext;

class GridGame {
public:
  void Init(Camera &cam, DxContext &dx);
  void Shutdown();

  void Update(float dt, Input &input, Win32Window &window, FrameData &frame);

  GridGameState GetState() const { return m_state; }
  bool WantsQuit() const { return m_wantsQuit; }

  // Load a stage from editor StageData for play-testing (Phase 5C).
  void LoadFromStageData(const StageData &stage);

  // Reload the current stage (retry/restart). Uses stored StageData if available.
  void ReloadCurrentStage();

  // Reset game state to MainMenu (for clean play-test stop).
  void ResetToMainMenu();

private:
  // State machine handlers.
  void UpdateMainMenu(FrameData &frame);
  void UpdateStageSelect(FrameData &frame);
  void UpdateIntro(float dt, Input &input, FrameData &frame);
  void UpdatePlaying(float dt, Input &input, FrameData &frame);
  void UpdatePaused(FrameData &frame);
  void UpdateStageComplete(FrameData &frame);
  void UpdateStageFail(FrameData &frame);

  // Build render items from current grid state.
  void BuildScene(FrameData &frame);

  // Load a test stage (hardcoded for Phase 1).
  void LoadTestStage();

  // Phase 2: movement helpers.
  void TryMove(int dx, int dy, bool pulling = false);
  void SetPlayerPosition(int x, int y);
  void SetCargoPosition(int x, int y);
  void SpawnPlayerAndCargo();

  // Phase 4: hazard system.
  void UpdateHazards(float dt);
  void TakeDamage(int amount);

  // Phase 3: tower system.
  void InitTowers();
  void UpdateTowers(float dt);

  // State.
  GridGameState m_state = GridGameState::MainMenu;
  bool m_wantsQuit = false;

  // Camera (non-owning).
  Camera *m_camera = nullptr;
  DxContext *m_dx = nullptr; // for SetBonePalette each frame

  // Skeletal animation (Phase 2B/2C/2D).
  Skeleton m_playerSkeleton;
  bool m_playerHasSkeleton = false;
  float m_animTime = 0.0f;
  std::vector<AnimationClip> m_playerAnimations;  // loaded from external GLB files
  int m_idleClipIndex = -1;  // index into m_playerAnimations (-1 = none)
  int m_pushClipIndex = -1;  // push/move animation clip index
  int m_runClipIndex  = -1;  // run animation clip index
  float m_pushAnimTime = 0.0f;  // separate time accumulator for push clip
  float m_runAnimTime  = 0.0f;  // separate time accumulator for run clip
  float m_pushLingerTimer = 0.0f; // seconds remaining for push anim after last cargo interaction
  float m_moveLingerTimer = 0.0f; // brief linger to smooth run→idle transition
  static constexpr float kPushLingerDuration = 1.0f; // push anim plays for 1s after interaction
  static constexpr float kMoveLingerDuration = 0.2f; // run anim lingers briefly after stop
  bool m_lastMoveWasCargoInteraction = false; // set by TryMove when push/pull actually moves cargo

  // Crossfade animation system (supports idle/run/push transitions).
  int m_animActiveClip = -1;    // clip index currently blending toward
  int m_animPrevClip   = -1;    // clip index blending from (-1 = none)
  float m_animTransition = 1.0f; // 0 = fully prev, 1 = fully active
  static constexpr float kAnimBlendSpeed = 8.0f; // crossfade speed (~0.125s)

  // Cargo drop-off (pushed off grid edge).
  bool m_cargoDropping = false;          // cargo is falling off the stage
  float m_cargoDropTimer = 0.0f;         // animation progress (0 → kCargoDropDuration)
  int m_cargoDropDx = 0, m_cargoDropDy = 0; // push direction that caused the drop
  DirectX::XMFLOAT3 m_cargoDropFrom = {};   // visual position when drop started
  static constexpr float kCargoDropDuration = 0.8f; // seconds for drop animation
  static constexpr float kCargoDropFallDepth = 5.0f; // how far cargo falls in Y
  static constexpr float kCargoDropSlideDistance = 2.0f; // how far cargo slides off edge

  // Grid.
  GridMap m_map;
  GridMeshIds m_meshIds;

  // Mesh IDs for game objects.
  uint32_t m_playerMeshId   = UINT32_MAX;
  uint32_t m_cargoMeshId    = UINT32_MAX;
  uint32_t m_towerMeshId    = UINT32_MAX;
  uint32_t m_gridLineMeshId = UINT32_MAX;

  // Phase 6: tile border glow mesh IDs (colored outlines).
  uint32_t m_borderOrangeMeshId = UINT32_MAX;  // fire, spike
  uint32_t m_borderCyanMeshId   = UINT32_MAX;  // ice, lightning
  uint32_t m_borderGreenMeshId  = UINT32_MAX;  // start
  uint32_t m_borderGoldMeshId   = UINT32_MAX;  // goal
  uint32_t m_borderRedMeshId    = UINT32_MAX;  // destructible wall
  uint32_t m_trailMeshId        = UINT32_MAX;  // player trail mark

  // Phase 2: Player & Cargo grid positions.
  int m_playerX = 0, m_playerY = 0;
  int m_cargoX = 0, m_cargoY = 0;

  // Visual interpolation (logic snaps instantly, visual lerps for polish).
  float m_playerLerpT = 1.0f;
  DirectX::XMFLOAT3 m_playerVisualPos = {};
  DirectX::XMFLOAT3 m_playerLerpFrom = {};

  float m_cargoLerpT = 1.0f;
  DirectX::XMFLOAT3 m_cargoVisualPos = {};
  DirectX::XMFLOAT3 m_cargoLerpFrom = {};

  static constexpr float kMoveSpeed = 10.0f; // 1 tile in ~0.1s

  // Input edge detection (press-to-move, not hold).
  bool m_prevUp = false, m_prevDown = false;
  bool m_prevLeft = false, m_prevRight = false;

  // Hold-to-repeat movement.
  float m_repeatTimer = 0.0f;
  bool m_repeatActive = false;
  int m_repeatDx = 0, m_repeatDy = 0;
  static constexpr float kRepeatDelay = 0.25f;
  static constexpr float kRepeatInterval = 0.12f;

  // Camera positioning.
  float m_cameraYaw       = -0.5f;  // radians
  float m_cameraPitch     = 0.7f;   // radians (elevation angle)
  float m_cameraDistance   = 20.0f;  // from grid center

  static constexpr float kCamDistMin = 0.5f;
  static constexpr float kCamDistMax = 200.0f;
  static constexpr float kCamZoomSpeed = 2.0f;

  // TPS (third-person) camera.
  float m_playerFacingYaw = 0.0f;          // target direction player faces (radians, 0 = +Z)
  float m_playerVisualYaw = 0.0f;          // smoothed visual yaw for rendering
  float m_tpsCamYaw       = 0.0f;          // smoothed camera yaw following player facing
  float m_tpsCamPitch     = 0.55f;         // elevation angle above player (radians)
  float m_tpsCamDist      = 6.0f;          // desired distance behind player
  float m_tpsCamActualDist = 6.0f;         // actual distance (may be shortened by collision)
  float m_tpsCamHeight    = 2.5f;          // extra height offset above shoulder
  static constexpr float kTpsCamSmoothSpeed = 3.5f;   // camera yaw interpolation speed
  static constexpr float kPlayerTurnSpeed   = 12.0f;  // player model rotation speed
  static constexpr float kCamCollisionSpeed = 10.0f;   // zoom in/out speed for collision
  static constexpr float kCamCollisionMinDist = 1.5f;  // minimum distance when colliding
  static constexpr float kTpsCamDistMin = 2.0f;
  static constexpr float kTpsCamDistMax = 20.0f;

  // Stage tracking.
  bool m_hasStageData = false;    // true when loaded from editor StageData
  StageData m_loadedStage;        // copy of last loaded StageData for retry
  int m_currentStage = 0;
  float m_stageTimer = 0.0f;
  int m_moveCount = 0;

  // Phase 4: HP system.
  int m_playerHP = 3;
  int m_playerMaxHP = 3;
  float m_iFrameTimer = 0.0f;
  static constexpr float kIFrameDuration = 1.0f;

  // Phase 4: stun (spike hazard).
  bool m_stunned = false;
  float m_stunTimer = 0.0f;
  static constexpr float kStunDuration = 0.5f;

  // Phase 4: ice slide.
  bool m_sliding = false;
  int m_slideDx = 0, m_slideDy = 0;

  // Phase 4: previous tile tracking (for crumble step-off).
  int m_prevTileX = 0, m_prevTileY = 0;

  // Phase 4: damage flash visual feedback.
  float m_damageFlashTimer = 0.0f;

  // Phase 4: fire DOT accumulator.
  float m_fireDotTimer = 0.0f;

  // ESC edge detection.
  bool m_prevEsc = false;

  // Phase 6: player movement trail (fading glow marks).
  static constexpr int kMaxTrailMarks = 8;
  struct TrailMark {
    int x = 0, y = 0;
    float age = 0.0f;   // seconds since placed
    bool active = false;
  };
  TrailMark m_trail[kMaxTrailMarks] = {};
  int m_trailHead = 0;  // circular buffer index

  // Phase 6: game particle emitters (fire embers, ice crystals).
  std::vector<std::unique_ptr<Emitter>> m_gameEmitters;
  void CreateGameEmitters();  // spawns emitters at hazard tile positions

  // Phase 8A: burst emitter pool (one-shot VFX).
  std::vector<std::unique_ptr<Emitter>> m_burstEmitters;
  void UpdateBurstEmitters(float dt);

  // Phase 8A: spawn a burst emitter at position.
  template <typename T, typename... Args>
  void SpawnBurst(const DirectX::XMVECTOR& position, Args&&... args) {
    auto em = std::make_unique<T>(position, std::forward<Args>(args)...);
    m_burstEmitters.push_back(std::move(em));
  }

  // Intro camera cinematic (pan to goal and back).
  float m_introTimer = 0.0f;        // total elapsed time in intro
  DirectX::XMFLOAT3 m_goalWorldPos = {};  // goal tile world position (computed on stage load)
  // Intro phases: 0→hold on player, 1→pan to goal, 2→hold on goal, 3→pan back to player
  static constexpr float kIntroHoldPlayer   = 0.5f;  // seconds: hold on player at start
  static constexpr float kIntroPanToGoal    = 1.2f;  // seconds: smooth pan to goal
  static constexpr float kIntroHoldGoal     = 0.8f;  // seconds: hold on goal
  static constexpr float kIntroPanBack      = 1.0f;  // seconds: smooth pan back to player
  // Total intro duration = sum of above

  // Camera mode: TPS vs Top-Down toggle.
  bool m_topDownMode = false;          // true = top-down, false = TPS
  float m_topDownBlend = 0.0f;         // 0 = full TPS, 1 = full top-down (smooth lerp)
  bool m_prevCamToggle = false;        // edge detection for R key / B button
  static constexpr float kCamTransitionSpeed = 3.0f;  // blend speed (seconds to full transition ~0.33s)
  static constexpr float kTopDownHeight = 18.0f;       // base height for top-down camera
  static constexpr float kTopDownPitch  = 1.48f;       // near-vertical look-down (radians, ~85 degrees)

  // Hazard beam system: periodic row/column beams.
  struct HazardBeam {
    bool isRow;         // true = row beam, false = column beam
    int index;          // row or column index
    float timer;        // time since spawn (0 → kBeamWarnTime → kBeamLifetime)
    bool damaged;       // true after damage has been applied
  };
  std::vector<HazardBeam> m_hazardBeams;
  float m_hazardBeamSpawnTimer = 0.0f;       // counts up, spawns at kBeamSpawnInterval
  static constexpr float kBeamSpawnInterval = 5.0f;   // seconds between beam spawns
  static constexpr float kBeamWarnTime      = 1.5f;   // seconds before damage
  static constexpr float kBeamLifetime      = 2.5f;   // total beam lifetime (warn + linger)
  static constexpr float kBeamMinRadius     = 0.02f;  // starting radius (thin)
  static constexpr float kBeamMaxRadius     = 0.25f;  // max radius at damage time
  static constexpr float kBeamFloatHeight   = 0.4f;   // Y position above tiles
  static constexpr int   kBeamsPerSpawn     = 2;       // how many beams per spawn event
  uint32_t m_hazardBeamMeshId = UINT32_MAX;            // separate mesh for hazard beams

  void UpdateHazardBeams(float dt);
  void SpawnHazardBeams();

  // Screen shake (triggered on damage).
  float m_shakeTimer = 0.0f;          // remaining shake time
  float m_shakeIntensity = 0.0f;      // current shake strength
  static constexpr float kShakeDuration  = 0.3f;  // seconds
  static constexpr float kShakeStrength  = 0.15f;  // max offset in world units
  static constexpr float kShakeFrequency = 35.0f;  // oscillation Hz

  // Frame delta time (cached for BuildScene camera collision smoothing).
  float m_dt = 0.0f;

  // Phase 7: UI animation timer (accumulated, drives sin/cos pulses).
  float m_uiTimer = 0.0f;

  // Phase 3: tower runtime state.
  enum class TowerPhase : uint8_t { Idle, Warn1, Warn2, Firing };

  struct RuntimeTower {
    TowerData data;
    float timer = 0.0f;
    int lastFireCycle = -1;
    float fireFlashTimer = 0.0f;
    std::vector<std::pair<int, int>> attackTiles;
  };

  std::vector<RuntimeTower> m_towers;

  // Phase 8A: helper to get tower world position.
  DirectX::XMFLOAT3 GetTowerWorldPos(const RuntimeTower& t) const;

  static constexpr float kFireFlashDuration = 0.2f;
  static constexpr float kWarn1LeadTime = 1.0f;  // telegraph starts 1s before fire
  static constexpr float kWarn2LeadTime = 0.5f;  // intense warning 0.5s before fire

  // Telegraph mesh IDs for 3-beat intensity levels.
  uint32_t m_telegraphWarn1MeshId = UINT32_MAX;
  uint32_t m_telegraphWarn2MeshId = UINT32_MAX;
  uint32_t m_telegraphFireMeshId  = UINT32_MAX;

  // Phase 3B: beam mesh ID.
  uint32_t m_beamMeshId = UINT32_MAX;

  TowerPhase GetTowerPhase(const RuntimeTower &t) const;
};
