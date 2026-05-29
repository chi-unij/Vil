// ======================================
// File: StageData.cpp
// Purpose: StageData implementation — resize, access, clear.
//          Milestone 4 Phase 5: Grid & Level Editor.
// ======================================

#include "StageData.h"

#include <algorithm>
#include <cassert>

void StageData::Resize(int newW, int newH) {
  if (newW <= 0 || newH <= 0)
    return;
  EnsureSize(); // Guard: tiles must match width*height before copying.
  if (newW == width && newH == height)
    return;

  std::vector<TileData> newTiles(static_cast<size_t>(newW) * newH);

  // Copy overlapping region from old grid.
  const int copyW = std::min(width, newW);
  const int copyH = std::min(height, newH);
  for (int y = 0; y < copyH; ++y) {
    for (int x = 0; x < copyW; ++x) {
      newTiles[static_cast<size_t>(y) * newW + x] =
          tiles[static_cast<size_t>(y) * width + x];
    }
  }

  tiles = std::move(newTiles);
  width = newW;
  height = newH;

  // Clamp spawn positions to new bounds.
  playerSpawnX = std::clamp(playerSpawnX, 0, width - 1);
  playerSpawnY = std::clamp(playerSpawnY, 0, height - 1);
  cargoSpawnX = std::clamp(cargoSpawnX, 0, width - 1);
  cargoSpawnY = std::clamp(cargoSpawnY, 0, height - 1);
}

TileData &StageData::At(int x, int y) {
  assert(InBounds(x, y));
  return tiles[static_cast<size_t>(y) * width + x];
}

const TileData &StageData::At(int x, int y) const {
  assert(InBounds(x, y));
  return tiles[static_cast<size_t>(y) * width + x];
}

bool StageData::InBounds(int x, int y) const {
  return x >= 0 && x < width && y >= 0 && y < height;
}

void StageData::Clear() {
  name = "Untitled";
  width = 10;
  height = 8;
  timeLimit = 0.0f;
  parMoves = 0;
  tiles.assign(static_cast<size_t>(width) * height, TileData{});
  towers.clear();
  playerSpawnX = 0;
  playerSpawnY = 0;
  cargoSpawnX = 1;
  cargoSpawnY = 0;
}

void StageData::EnsureSize() {
  const auto expected = static_cast<size_t>(width) * height;
  if (tiles.size() != expected)
    tiles.resize(expected);
}

// ---- Attack pattern computation (Phase 5C) ----

std::vector<std::pair<int, int>> ComputeAttackTiles(const TowerData &tower,
                                                     int gridW, int gridH) {
  std::vector<std::pair<int, int>> result;

  // Determine the row/column the tower fires into based on its side.
  // Left/Right sides fire along a row (tower.y), Top/Bottom fire along a column (tower.x).
  bool horizontal = (tower.side == TowerSide::Left || tower.side == TowerSide::Right);
  int row = tower.y;  // relevant for Left/Right
  int col = tower.x;  // relevant for Top/Bottom

  switch (tower.pattern) {
  case TowerPattern::Row:
    if (horizontal) {
      for (int x = 0; x < gridW; ++x)
        result.push_back({x, row});
    } else {
      // Top/Bottom tower with Row pattern: fires across the row at the tower's column position.
      // Interpret as the row at the tower's edge: for Top, row 0; for Bottom, row gridH-1.
      int r = (tower.side == TowerSide::Top) ? 0 : gridH - 1;
      for (int x = 0; x < gridW; ++x)
        result.push_back({x, r});
    }
    break;

  case TowerPattern::Column:
    if (!horizontal) {
      for (int y = 0; y < gridH; ++y)
        result.push_back({col, y});
    } else {
      // Left/Right tower with Column pattern: fires down the column at the tower's row position.
      int c = (tower.side == TowerSide::Left) ? 0 : gridW - 1;
      for (int y = 0; y < gridH; ++y)
        result.push_back({c, y});
    }
    break;

  case TowerPattern::Cross:
    if (horizontal) {
      // Full row
      for (int x = 0; x < gridW; ++x)
        result.push_back({x, row});
      // Full column from entry point
      int c = (tower.side == TowerSide::Left) ? 0 : gridW - 1;
      for (int y = 0; y < gridH; ++y) {
        if (y != row) // avoid duplicate
          result.push_back({c, y});
      }
    } else {
      // Full column
      for (int y = 0; y < gridH; ++y)
        result.push_back({col, y});
      // Full row from entry point
      int r = (tower.side == TowerSide::Top) ? 0 : gridH - 1;
      for (int x = 0; x < gridW; ++x) {
        if (x != col) // avoid duplicate
          result.push_back({x, r});
      }
    }
    break;

  case TowerPattern::Diagonal: {
    // Two diagonal lines from the tower's grid-edge entry point.
    int originX, originY;
    if (tower.side == TowerSide::Left) {
      originX = 0;
      originY = row;
    } else if (tower.side == TowerSide::Right) {
      originX = gridW - 1;
      originY = row;
    } else if (tower.side == TowerSide::Top) {
      originX = col;
      originY = 0;
    } else {
      originX = col;
      originY = gridH - 1;
    }

    // Diagonal directions: (+1,+1), (+1,-1), (-1,+1), (-1,-1)
    static const int dirs[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    for (auto &d : dirs) {
      int cx = originX + d[0];
      int cy = originY + d[1];
      while (cx >= 0 && cx < gridW && cy >= 0 && cy < gridH) {
        result.push_back({cx, cy});
        cx += d[0];
        cy += d[1];
      }
    }
    // Include origin tile itself.
    if (originX >= 0 && originX < gridW && originY >= 0 && originY < gridH)
      result.push_back({originX, originY});
    break;
  }
  }

  return result;
}

// ---- Editor viewport render items (Phase 5B) ----

void BuildStageRenderItems(const StageData &stage, const EditorMeshIds &ids,
                           std::vector<RenderItem> &outItems,
                           int highlightX, int highlightY,
                           const std::vector<std::pair<int, int>> *attackPreview) {
  using namespace DirectX;

  for (int y = 0; y < stage.height; ++y) {
    for (int x = 0; x < stage.width; ++x) {
      const TileData &t = stage.tiles[static_cast<size_t>(y) * stage.width + x];
      float wx = static_cast<float>(x);
      float wz = static_cast<float>(y);

      // Map TileType to meshId.
      uint32_t meshId = ids.floor;
      float tileY = 0.0f;
      switch (t.type) {
      case TileType::Floor:     meshId = ids.floor; break;
      case TileType::Wall:      meshId = ids.wall; tileY = 0.5f; break;
      case TileType::Fire:      meshId = ids.fire; break;
      case TileType::Lightning: meshId = ids.lightning; break;
      case TileType::Spike:     meshId = ids.spike; break;
      case TileType::Ice:       meshId = ids.ice; break;
      case TileType::Crumble:   meshId = ids.crumble; break;
      case TileType::Start:     meshId = ids.start; break;
      case TileType::Goal:      meshId = ids.goal; break;
      }
      if (meshId == UINT32_MAX)
        meshId = ids.floor;

      outItems.push_back({meshId, XMMatrixTranslation(wx, tileY, wz)});

      // Wall overlay on non-wall tiles that have a placed wall.
      if (t.hasWall && t.type != TileType::Wall && ids.wall != UINT32_MAX) {
        outItems.push_back({ids.wall, XMMatrixTranslation(wx, 0.5f, wz)});
      }
    }
  }

  // Player spawn marker (small green cube above tile).
  if (stage.InBounds(stage.playerSpawnX, stage.playerSpawnY) &&
      ids.playerSpawn != UINT32_MAX) {
    outItems.push_back(
        {ids.playerSpawn,
         XMMatrixScaling(0.3f, 0.3f, 0.3f) *
             XMMatrixTranslation(static_cast<float>(stage.playerSpawnX), 0.6f,
                                 static_cast<float>(stage.playerSpawnY))});
  }

  // Cargo spawn marker (small orange cube above tile).
  if (stage.InBounds(stage.cargoSpawnX, stage.cargoSpawnY) &&
      ids.cargoSpawn != UINT32_MAX) {
    outItems.push_back(
        {ids.cargoSpawn,
         XMMatrixScaling(0.25f, 0.25f, 0.25f) *
             XMMatrixTranslation(static_cast<float>(stage.cargoSpawnX), 0.6f,
                                 static_cast<float>(stage.cargoSpawnY))});
  }

  // Tower markers (red cones).
  if (ids.tower != UINT32_MAX) {
    for (const auto &tw : stage.towers) {
      outItems.push_back(
          {ids.tower, XMMatrixScaling(0.4f, 0.4f, 0.4f) *
                          XMMatrixTranslation(static_cast<float>(tw.x), 0.8f,
                                              static_cast<float>(tw.y))});
    }
  }

  // Selection highlight (slightly raised + scaled overlay on selected tile).
  if (highlightX >= 0 && highlightY >= 0 &&
      stage.InBounds(highlightX, highlightY) &&
      ids.highlight != UINT32_MAX) {
    outItems.push_back(
        {ids.highlight,
         XMMatrixScaling(1.02f, 0.05f, 1.02f) *
             XMMatrixTranslation(static_cast<float>(highlightX), 0.02f,
                                 static_cast<float>(highlightY))});
  }

  // Attack preview overlay (Phase 5C): telegraph planes on affected tiles.
  if (attackPreview && !attackPreview->empty() &&
      ids.telegraph != UINT32_MAX) {
    for (const auto &[ax, ay] : *attackPreview) {
      if (ax >= 0 && ax < stage.width && ay >= 0 && ay < stage.height) {
        outItems.push_back(
            {ids.telegraph,
             XMMatrixTranslation(static_cast<float>(ax), 0.03f,
                                 static_cast<float>(ay))});
      }
    }
  }
}
