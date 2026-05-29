// ======================================
// File: GridGameState.h
// Purpose: Game state enum for Grid Gauntlet.
// ======================================

#pragma once

#include <cstdint>

enum class GridGameState : uint8_t {
  MainMenu      = 0,
  StageSelect   = 1,
  Intro         = 2,  // cinematic camera pan to goal before gameplay
  Playing       = 3,
  Paused        = 4,
  StageComplete = 5,
  StageFail     = 6,
};
