// ======================================
// File: GridMap.h
// Purpose: Grid data structure for Grid Gauntlet. Stores tile types, wall
//          state, and hazard timers. Produces RenderItems for the engine.
// ======================================

#pragma once

#include "../Lighting.h"
#include "../RenderPass.h"

#include <DirectXMath.h>
#include <cstdint>
#include <vector>

enum class TileType : uint8_t {
  Floor   = 0,
  Wall    = 1, // impassable block
  Fire    = 2, // DOT while standing
  Lightning = 3, // periodic burst
  Spike   = 4, // toggle damage + stun
  Ice     = 5, // slow movement
  Crumble = 6, // breaks after stepping off
  Start   = 7, // player/cargo spawn (walkable)
  Goal    = 8, // right-edge goal zone (walkable)
};

struct Tile {
  TileType type = TileType::Floor;

  // Wall on this tile (separate from TileType::Wall — any floor tile can also
  // have a wall placed on it that blocks movement).
  bool hasWall = false;
  bool wallDestructible = false; // can tower attacks destroy this wall?
  bool wallDestroyed = false;    // has been destroyed by tower attack?

  // Per-tile hazard state (used by HazardSystem).
  float hazardTimer = 0.0f;
  bool  hazardActive = false;   // spikes up, lightning charging, etc.
  bool  crumbleBroken = false;  // crumble tile destroyed after stepping off
};

// Mesh IDs for different tile visuals. Set by GridGame during Init.
struct GridMeshIds {
  uint32_t floor       = UINT32_MAX;
  uint32_t wall        = UINT32_MAX;
  uint32_t wallDestructible = UINT32_MAX;
  uint32_t fire        = UINT32_MAX;
  uint32_t lightning   = UINT32_MAX;
  uint32_t spike       = UINT32_MAX;
  uint32_t ice         = UINT32_MAX;
  uint32_t crumble     = UINT32_MAX;
  uint32_t start       = UINT32_MAX;
  uint32_t goal        = UINT32_MAX;
  uint32_t telegraph   = UINT32_MAX; // attack warning overlay
};

class GridMap {
public:
  void Init(int width, int height, const std::vector<Tile> &layout);
  void Reset(); // clear to empty

  // Accessors.
  int Width() const { return m_width; }
  int Height() const { return m_height; }
  bool InBounds(int x, int y) const;

  Tile &At(int x, int y);
  const Tile &At(int x, int y) const;

  // World position of a tile's center (tile size = 1 unit, grid on XZ plane).
  DirectX::XMFLOAT3 TileCenter(int x, int y) const;

  // Can an entity walk on this tile?
  bool IsWalkable(int x, int y) const;

  // Mutation.
  void DestroyWall(int x, int y);
  void DestroyCrumble(int x, int y);

  // Build render items for all visible tiles.
  void BuildRenderItems(const GridMeshIds &meshIds,
                        std::vector<RenderItem> &outItems,
                        std::vector<GPUPointLight> &outLights,
                        float gameTime = 0.0f) const;

private:
  int m_width = 0;
  int m_height = 0;
  std::vector<Tile> m_tiles; // row-major: index = y * m_width + x
};
