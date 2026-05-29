// ======================================
// File: GridMaterials.h
// Purpose: Shared material factory functions for the Grid Gauntlet neon aesthetic.
//          Used by both GridGame (runtime) and SceneEditor (grid editor viewport).
// ======================================

#pragma once

#include "../Lighting.h"

// ---- Tile materials ----

static inline Material MakeFloorMaterial() {
  Material m;
  m.baseColorFactor = {0.06f, 0.06f, 0.09f, 1.0f};
  m.emissiveFactor = {0.0f, 0.04f, 0.08f};
  m.metallicFactor = 0.2f;
  m.roughnessFactor = 0.7f;
  return m;
}

static inline Material MakeWallMaterial() {
  Material m;
  m.baseColorFactor = {0.10f, 0.10f, 0.14f, 1.0f};
  m.emissiveFactor = {0.02f, 0.02f, 0.06f};
  m.metallicFactor = 0.3f;
  m.roughnessFactor = 0.5f;
  return m;
}

static inline Material MakeDestructibleWallMaterial() {
  Material m;
  m.baseColorFactor = {0.12f, 0.08f, 0.06f, 1.0f};
  m.emissiveFactor = {0.08f, 0.04f, 0.01f};
  m.metallicFactor = 0.2f;
  m.roughnessFactor = 0.6f;
  return m;
}

static inline Material MakeFireMaterial() {
  Material m;
  m.baseColorFactor = {0.15f, 0.04f, 0.02f, 1.0f};
  m.emissiveFactor = {2.0f, 0.6f, 0.1f};
  m.metallicFactor = 0.0f;
  m.roughnessFactor = 0.9f;
  m.proceduralTypeId = 1.0f;
  return m;
}

static inline Material MakeLightningMaterial() {
  Material m;
  m.baseColorFactor = {0.05f, 0.06f, 0.12f, 1.0f};
  m.emissiveFactor = {0.2f, 0.4f, 1.2f};
  m.metallicFactor = 0.1f;
  m.roughnessFactor = 0.8f;
  m.proceduralTypeId = 3.0f;
  return m;
}

static inline Material MakeSpikeMaterial() {
  Material m;
  m.baseColorFactor = {0.12f, 0.12f, 0.10f, 1.0f};
  m.emissiveFactor = {0.5f, 0.2f, 0.05f};
  m.metallicFactor = 0.6f;
  m.roughnessFactor = 0.3f;
  m.proceduralTypeId = 4.0f;
  return m;
}

static inline Material MakeIceMaterial() {
  Material m;
  m.baseColorFactor = {0.08f, 0.15f, 0.20f, 1.0f};
  m.emissiveFactor = {0.1f, 0.5f, 1.5f};
  m.metallicFactor = 0.1f;
  m.roughnessFactor = 0.2f;
  m.proceduralTypeId = 2.0f;
  return m;
}

static inline Material MakeCrumbleMaterial() {
  Material m;
  m.baseColorFactor = {0.08f, 0.07f, 0.06f, 1.0f};
  m.emissiveFactor = {0.03f, 0.02f, 0.01f};
  m.metallicFactor = 0.0f;
  m.roughnessFactor = 0.9f;
  m.proceduralTypeId = 5.0f;
  return m;
}

static inline Material MakeStartMaterial() {
  Material m;
  m.baseColorFactor = {0.05f, 0.12f, 0.06f, 1.0f};
  m.emissiveFactor = {0.0f, 1.0f, 0.4f};
  m.metallicFactor = 0.2f;
  m.roughnessFactor = 0.6f;
  return m;
}

static inline Material MakeGoalMaterial() {
  Material m;
  m.baseColorFactor = {0.15f, 0.12f, 0.02f, 1.0f};
  m.emissiveFactor = {2.0f, 1.6f, 0.2f};
  m.metallicFactor = 0.3f;
  m.roughnessFactor = 0.4f;
  return m;
}

// ---- Game object materials ----

static inline Material MakePlayerMaterial() {
  Material m;
  m.baseColorFactor = {0.0f, 0.6f, 0.9f, 1.0f};
  m.emissiveFactor = {0.0f, 0.8f, 2.0f};
  m.metallicFactor = 0.4f;
  m.roughnessFactor = 0.3f;
  return m;
}

static inline Material MakeCargoMaterial() {
  Material m;
  m.baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};  // white — let texture show through
  m.emissiveFactor = {0.0f, 0.0f, 0.0f};           // no self-glow, lit by point light
  m.metallicFactor = 0.2f;
  m.roughnessFactor = 0.8f;
  return m;
}

static inline Material MakeTowerMaterial() {
  Material m;
  m.baseColorFactor = {0.5f, 0.1f, 0.1f, 1.0f};
  m.emissiveFactor = {1.5f, 0.2f, 0.1f};
  m.metallicFactor = 0.5f;
  m.roughnessFactor = 0.4f;
  return m;
}

static inline Material MakeTelegraphMaterial() {
  Material m;
  m.baseColorFactor = {0.3f, 0.05f, 0.05f, 1.0f};
  m.emissiveFactor = {1.0f, 0.15f, 0.1f};
  m.metallicFactor = 0.0f;
  m.roughnessFactor = 1.0f;
  return m;
}

// Phase 3A: 3-beat telegraph intensity levels.
// Warn1 — dim red glow, "something's coming"
static inline Material MakeTelegraphWarn1Material() {
  Material m;
  m.baseColorFactor = {0.15f, 0.03f, 0.03f, 1.0f};
  m.emissiveFactor = {0.5f, 0.08f, 0.05f};
  m.metallicFactor = 0.0f;
  m.roughnessFactor = 1.0f;
  return m;
}

// Warn2 — bright red pulse, "move NOW"
static inline Material MakeTelegraphWarn2Material() {
  Material m;
  m.baseColorFactor = {0.4f, 0.06f, 0.06f, 1.0f};
  m.emissiveFactor = {2.0f, 0.3f, 0.15f};
  m.metallicFactor = 0.0f;
  m.roughnessFactor = 1.0f;
  return m;
}

// Fire flash — white-hot burst, maximum bloom
static inline Material MakeTelegraphFireMaterial() {
  Material m;
  m.baseColorFactor = {1.0f, 0.6f, 0.5f, 1.0f};
  m.emissiveFactor = {5.0f, 2.0f, 1.5f};
  m.metallicFactor = 0.0f;
  m.roughnessFactor = 1.0f;
  return m;
}

// Phase 3B: beam laser material (very high emissive for intense bloom).
static inline Material MakeBeamMaterial() {
  Material m;
  m.baseColorFactor = {1.0f, 0.4f, 0.3f, 1.0f};
  m.emissiveFactor = {6.0f, 1.5f, 0.8f};
  m.metallicFactor = 0.0f;
  m.roughnessFactor = 1.0f;
  return m;
}

// ---- Hazard beam material (purple/magenta, distinct from tower beams) ----

static inline Material MakeHazardBeamMaterial() {
  Material m;
  m.baseColorFactor = {0.8f, 0.2f, 1.0f, 1.0f};
  m.emissiveFactor = {4.0f, 0.5f, 6.0f};
  m.metallicFactor = 0.0f;
  m.roughnessFactor = 1.0f;
  return m;
}

// ---- Grid edge line material (neon grid) ----

static inline Material MakeGridLineMaterial() {
  Material m;
  m.baseColorFactor = {0.02f, 0.04f, 0.08f, 1.0f};
  m.emissiveFactor = {0.0f, 0.15f, 0.3f};
  m.metallicFactor = 0.3f;
  m.roughnessFactor = 0.4f;
  return m;
}

// ---- Tile border glow materials (bright outlines around special tiles) ----

static inline Material MakeBorderOrangeMaterial() {
  Material m;
  m.baseColorFactor = {0.3f, 0.08f, 0.02f, 1.0f};
  m.emissiveFactor = {3.0f, 0.8f, 0.1f};
  m.metallicFactor = 0.0f;
  m.roughnessFactor = 0.5f;
  return m;
}

static inline Material MakeBorderCyanMaterial() {
  Material m;
  m.baseColorFactor = {0.02f, 0.15f, 0.3f, 1.0f};
  m.emissiveFactor = {0.1f, 0.8f, 2.5f};
  m.metallicFactor = 0.0f;
  m.roughnessFactor = 0.5f;
  return m;
}

static inline Material MakeBorderGreenMaterial() {
  Material m;
  m.baseColorFactor = {0.02f, 0.2f, 0.05f, 1.0f};
  m.emissiveFactor = {0.0f, 1.5f, 0.5f};
  m.metallicFactor = 0.0f;
  m.roughnessFactor = 0.5f;
  return m;
}

static inline Material MakeBorderGoldMaterial() {
  Material m;
  m.baseColorFactor = {0.3f, 0.25f, 0.02f, 1.0f};
  m.emissiveFactor = {3.0f, 2.0f, 0.3f};
  m.metallicFactor = 0.0f;
  m.roughnessFactor = 0.5f;
  return m;
}

static inline Material MakeBorderRedMaterial() {
  Material m;
  m.baseColorFactor = {0.3f, 0.05f, 0.05f, 1.0f};
  m.emissiveFactor = {2.5f, 0.3f, 0.2f};
  m.metallicFactor = 0.0f;
  m.roughnessFactor = 0.5f;
  return m;
}

// ---- Player trail glow material ----

static inline Material MakeTrailMaterial() {
  Material m;
  m.baseColorFactor = {0.02f, 0.08f, 0.15f, 1.0f};
  m.emissiveFactor = {0.0f, 0.3f, 0.8f};
  m.metallicFactor = 0.0f;
  m.roughnessFactor = 0.5f;
  return m;
}

// ---- Editor-only marker material ----

static inline Material MakeHighlightMaterial() {
  Material m;
  m.baseColorFactor = {1.0f, 1.0f, 1.0f, 0.3f};
  m.emissiveFactor = {0.5f, 0.5f, 1.0f};
  m.metallicFactor = 0.0f;
  m.roughnessFactor = 1.0f;
  return m;
}
