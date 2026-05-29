// ======================================
// File: Scene.h
// Purpose: Entity container that owns all objects in the editor scene.
//          Builds FrameData for the render pipeline and handles JSON
//          serialization.
// ======================================

#pragma once

#include "Entity.h"
#include "../RenderPass.h"

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

class DxContext;

class Scene {
public:
  // Entity management.
  EntityId AddEntity(const std::string &name = "Entity");
  void AddEntityDirect(const Entity &entity);
  EntityId AllocateId();
  void RemoveEntity(EntityId id);
  Entity *FindEntity(EntityId id);
  const Entity *FindEntity(EntityId id) const;

  std::vector<Entity> &Entities() { return m_entities; }
  const std::vector<Entity> &Entities() const { return m_entities; }

  // Scene-global lighting settings.
  SceneLightSettings &LightSettings() { return m_lightSettings; }
  const SceneLightSettings &LightSettings() const { return m_lightSettings; }

  // Scene-global shadow & SSAO settings (Phase 4).
  SceneShadowSettings &ShadowSettings() { return m_shadowSettings; }
  const SceneShadowSettings &ShadowSettings() const { return m_shadowSettings; }

  // Scene-global post-processing settings (Phase 4).
  ScenePostProcessSettings &PostProcessSettings() { return m_postProcessSettings; }
  const ScenePostProcessSettings &PostProcessSettings() const { return m_postProcessSettings; }

  // Camera presets (Phase 6).
  std::vector<CameraPreset> &CameraPresets() { return m_cameraPresets; }
  const std::vector<CameraPreset> &CameraPresets() const { return m_cameraPresets; }

  // Build FrameData from all active entities.
  void BuildFrameData(FrameData &frame) const;

  // Create GPU resources for all entities with MeshComponent.
  void CreateGpuResources(DxContext &dx);

  // Create GPU resources for a single entity's mesh component.
  void CreateEntityMeshGpu(DxContext &dx, Entity &entity);

  // Update only the material factors on an existing GPU mesh (no SRV re-allocation).
  void UpdateEntityMaterial(DxContext &dx, Entity &entity);

  // JSON serialization.
  bool SaveToFile(const std::string &path) const;
  bool LoadFromFile(const std::string &path, DxContext &dx);

  // In-memory serialization for scene play mode (Phase 8).
  std::string SerializeToString() const;
  bool DeserializeFromString(const std::string &jsonStr, DxContext &dx);

  // Clear all entities.
  void Clear();

private:
  void LoadFromJson(const nlohmann::json &j);

  std::vector<Entity> m_entities;
  EntityId m_nextId = 1;
  SceneLightSettings m_lightSettings;
  SceneShadowSettings m_shadowSettings;
  ScenePostProcessSettings m_postProcessSettings;
  std::vector<CameraPreset> m_cameraPresets; // Phase 6
};
