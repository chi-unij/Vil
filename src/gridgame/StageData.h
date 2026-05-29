// ======================================
// File: StageData.h
// Purpose: Pure data structs for stage editing in the Grid Editor.
//          No runtime state (hazardTimer, etc.) — just the level layout
//          that serializes cleanly to JSON.
//          Milestone 4 Phase 5: Grid & Level Editor.
// ======================================

#pragma once

#include "GridMap.h" // TileType enum
#include "../RenderPass.h" // RenderItem

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// ---- Tile editing data (no runtime state) ----

struct TileData {
  TileType type = TileType::Floor;
  bool hasWall = false;
  bool wallDestructible = false;
};

// ---- Tower definitions ----

enum class TowerSide : uint8_t { Left, Right, Top, Bottom };
enum class TowerPattern : uint8_t { Row, Column, Cross, Diagonal };

struct TowerData {
  int x = 0;
  int y = 0;
  TowerSide side = TowerSide::Left;
  TowerPattern pattern = TowerPattern::Row;
  float delay = 1.0f;
  float interval = 3.0f;
};

// ---- Stage definition ----

struct StageData {
  std::string name = "Untitled";
  int width = 10;
  int height = 8;
  float timeLimit = 0.0f;  // 0 = no limit
  int parMoves = 0;         // 0 = no par
  std::vector<TileData> tiles; // row-major [y * width + x]
  std::vector<TowerData> towers;
  int playerSpawnX = 0;
  int playerSpawnY = 0;
  int cargoSpawnX = 1;
  int cargoSpawnY = 0;

  // Resize grid, preserving existing tiles in the overlap region.
  void Resize(int newW, int newH);

  // Access tile at (x, y). Asserts InBounds.
  TileData &At(int x, int y);
  const TileData &At(int x, int y) const;

  bool InBounds(int x, int y) const;

  // Reset to default empty grid.
  void Clear();

  // Ensure tiles vector matches width * height. Called after deserialization.
  void EnsureSize();
};

// ---- Editor viewport mesh IDs (Phase 5B) ----

struct EditorMeshIds {
  uint32_t floor = UINT32_MAX;
  uint32_t wall = UINT32_MAX;
  uint32_t fire = UINT32_MAX;
  uint32_t lightning = UINT32_MAX;
  uint32_t spike = UINT32_MAX;
  uint32_t ice = UINT32_MAX;
  uint32_t crumble = UINT32_MAX;
  uint32_t start = UINT32_MAX;
  uint32_t goal = UINT32_MAX;
  uint32_t playerSpawn = UINT32_MAX;
  uint32_t cargoSpawn = UINT32_MAX;
  uint32_t tower = UINT32_MAX;
  uint32_t highlight = UINT32_MAX;
  uint32_t telegraph = UINT32_MAX; // attack preview overlay plane
};

// Compute which grid tiles a tower's attack pattern covers.
std::vector<std::pair<int, int>> ComputeAttackTiles(const TowerData &tower,
                                                     int gridW, int gridH);

// Build RenderItems from StageData for editor viewport visualization.
void BuildStageRenderItems(const StageData &stage, const EditorMeshIds &ids,
                           std::vector<RenderItem> &outItems,
                           int highlightX = -1, int highlightY = -1,
                           const std::vector<std::pair<int, int>> *attackPreview = nullptr);
