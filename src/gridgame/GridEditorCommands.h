// ======================================
// File: GridEditorCommands.h
// Purpose: Undo/redo commands for the Grid Editor.
//          Follows the same ICommand pattern as Commands.h.
//          Milestone 4 Phase 5: Grid & Level Editor.
// ======================================

#pragma once

#include "StageData.h"
#include "../engine/CommandHistory.h"

#include <string>
#include <utility>
#include <vector>

// ---- Paint a single tile ----

class PaintTileCommand : public ICommand {
public:
  PaintTileCommand(StageData &stage, int x, int y, const TileData &before,
                   const TileData &after)
      : m_stage(stage), m_x(x), m_y(y), m_before(before), m_after(after) {}

  void Execute() override {
    if (m_stage.InBounds(m_x, m_y))
      m_stage.At(m_x, m_y) = m_after;
  }
  void Undo() override {
    if (m_stage.InBounds(m_x, m_y))
      m_stage.At(m_x, m_y) = m_before;
  }
  const char *Name() const override { return "Paint Tile"; }

private:
  StageData &m_stage;
  int m_x, m_y;
  TileData m_before;
  TileData m_after;
};

// ---- Paint multiple tiles in a single stroke (drag-paint) ----

class PaintTilesCommand : public ICommand {
public:
  struct Entry {
    int x, y;
    TileData before;
    TileData after;
  };

  PaintTilesCommand(StageData &stage, std::vector<Entry> entries)
      : m_stage(stage), m_entries(std::move(entries)) {}

  void Execute() override {
    for (const auto &e : m_entries) {
      if (m_stage.InBounds(e.x, e.y))
        m_stage.At(e.x, e.y) = e.after;
    }
  }
  void Undo() override {
    for (const auto &e : m_entries) {
      if (m_stage.InBounds(e.x, e.y))
        m_stage.At(e.x, e.y) = e.before;
    }
  }
  const char *Name() const override { return "Paint Tiles"; }

private:
  StageData &m_stage;
  std::vector<Entry> m_entries;
};

// ---- Resize grid (stores entire before/after state) ----

class ResizeGridCommand : public ICommand {
public:
  ResizeGridCommand(StageData &stage, const StageData &before,
                    const StageData &after)
      : m_stage(stage), m_before(before), m_after(after) {}

  void Execute() override { m_stage = m_after; }
  void Undo() override { m_stage = m_before; }
  const char *Name() const override { return "Resize Grid"; }

private:
  StageData &m_stage;
  StageData m_before;
  StageData m_after;
};

// ---- Stage metadata change (name, timeLimit, parMoves, spawns) ----

struct StageMetadata {
  std::string name;
  float timeLimit = 0.0f;
  int parMoves = 0;
  int playerSpawnX = 0;
  int playerSpawnY = 0;
  int cargoSpawnX = 1;
  int cargoSpawnY = 0;
};

inline StageMetadata ExtractMetadata(const StageData &s) {
  return {s.name,         s.timeLimit,    s.parMoves,
          s.playerSpawnX, s.playerSpawnY, s.cargoSpawnX, s.cargoSpawnY};
}

inline void ApplyMetadata(StageData &s, const StageMetadata &m) {
  s.name = m.name;
  s.timeLimit = m.timeLimit;
  s.parMoves = m.parMoves;
  s.playerSpawnX = m.playerSpawnX;
  s.playerSpawnY = m.playerSpawnY;
  s.cargoSpawnX = m.cargoSpawnX;
  s.cargoSpawnY = m.cargoSpawnY;
}

class StageMetadataCommand : public ICommand {
public:
  StageMetadataCommand(StageData &stage, const StageMetadata &before,
                       const StageMetadata &after)
      : m_stage(stage), m_before(before), m_after(after) {}

  void Execute() override { ApplyMetadata(m_stage, m_after); }
  void Undo() override { ApplyMetadata(m_stage, m_before); }
  const char *Name() const override { return "Stage Metadata"; }

private:
  StageData &m_stage;
  StageMetadata m_before;
  StageMetadata m_after;
};

// ---- Tower add/remove/modify (stores entire tower list before/after) ----

class TowerCommand : public ICommand {
public:
  TowerCommand(StageData &stage, std::vector<TowerData> before,
               std::vector<TowerData> after)
      : m_stage(stage), m_before(std::move(before)),
        m_after(std::move(after)) {}

  void Execute() override { m_stage.towers = m_after; }
  void Undo() override { m_stage.towers = m_before; }
  const char *Name() const override { return "Tower Edit"; }

private:
  StageData &m_stage;
  std::vector<TowerData> m_before;
  std::vector<TowerData> m_after;
};
