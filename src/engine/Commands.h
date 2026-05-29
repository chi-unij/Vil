// ======================================
// File: Commands.h
// Purpose: Concrete undoable command implementations for editor operations.
//          Milestone 4 Phase 1: Geometry Tools.
// ======================================

#pragma once

#include "CommandHistory.h"
#include "Entity.h"
#include "Scene.h"

#include <array>
#include <string>

class DxContext;

// ---- Transform change (gizmo drag or inspector edit) ----

class TransformCommand : public ICommand {
public:
  TransformCommand(Scene &scene, EntityId id, const Transform &before,
                   const Transform &after)
      : m_scene(scene), m_entityId(id), m_before(before), m_after(after) {}

  void Execute() override {
    if (Entity *e = m_scene.FindEntity(m_entityId))
      e->transform = m_after;
  }
  void Undo() override {
    if (Entity *e = m_scene.FindEntity(m_entityId))
      e->transform = m_before;
  }
  const char *Name() const override { return "Transform"; }

private:
  Scene &m_scene;
  EntityId m_entityId;
  Transform m_before;
  Transform m_after;
};

// ---- Create entity ----

class CreateEntityCommand : public ICommand {
public:
  CreateEntityCommand(Scene &scene, DxContext &dx, Entity entity)
      : m_scene(scene), m_dx(dx), m_entity(std::move(entity)) {}

  void Execute() override {
    m_scene.AddEntityDirect(m_entity);
    Entity *e = m_scene.FindEntity(m_entity.id);
    if (e)
      m_scene.CreateEntityMeshGpu(m_dx, *e);
  }
  void Undo() override { m_scene.RemoveEntity(m_entity.id); }
  const char *Name() const override { return "Create Entity"; }

private:
  Scene &m_scene;
  DxContext &m_dx;
  Entity m_entity;
};

// ---- Delete entity ----

class DeleteEntityCommand : public ICommand {
public:
  DeleteEntityCommand(Scene &scene, DxContext &dx, EntityId id)
      : m_scene(scene), m_dx(dx), m_entityId(id) {}

  void Execute() override {
    Entity *e = m_scene.FindEntity(m_entityId);
    if (e)
      m_storedEntity = *e;
    m_scene.RemoveEntity(m_entityId);
  }
  void Undo() override {
    m_scene.AddEntityDirect(m_storedEntity);
    Entity *e = m_scene.FindEntity(m_storedEntity.id);
    if (e)
      m_scene.CreateEntityMeshGpu(m_dx, *e);
  }
  const char *Name() const override { return "Delete Entity"; }

private:
  Scene &m_scene;
  DxContext &m_dx;
  EntityId m_entityId;
  Entity m_storedEntity;
};

// ---- Duplicate entity ----

class DuplicateEntityCommand : public ICommand {
public:
  DuplicateEntityCommand(Scene &scene, DxContext &dx, EntityId sourceId)
      : m_scene(scene), m_dx(dx), m_sourceId(sourceId) {}

  void Execute() override {
    if (!m_hasExecuted) {
      // First execution: clone from source.
      const Entity *src = m_scene.FindEntity(m_sourceId);
      if (!src)
        return;
      m_duplicate = *src;
      m_duplicate.id = m_scene.AllocateId();
      m_duplicate.name = src->name + " (Copy)";
      m_duplicate.transform.position.x += 1.0f;
      if (m_duplicate.mesh.has_value())
        m_duplicate.mesh->meshId = UINT32_MAX; // Force GPU re-creation
      m_hasExecuted = true;
    }
    m_scene.AddEntityDirect(m_duplicate);
    Entity *e = m_scene.FindEntity(m_duplicate.id);
    if (e)
      m_scene.CreateEntityMeshGpu(m_dx, *e);
  }
  void Undo() override { m_scene.RemoveEntity(m_duplicate.id); }
  const char *Name() const override { return "Duplicate"; }

  EntityId DuplicatedId() const { return m_duplicate.id; }

private:
  Scene &m_scene;
  DxContext &m_dx;
  EntityId m_sourceId;
  Entity m_duplicate;
  bool m_hasExecuted = false;
};

// ---- Generic single-property change ----

template <typename T> class PropertyCommand : public ICommand {
public:
  PropertyCommand(const char *name, Scene &scene, EntityId id,
                  T Entity::*memberPtr, const T &before, const T &after)
      : m_name(name), m_scene(scene), m_entityId(id), m_memberPtr(memberPtr),
        m_before(before), m_after(after) {}

  void Execute() override {
    if (Entity *e = m_scene.FindEntity(m_entityId))
      e->*m_memberPtr = m_after;
  }
  void Undo() override {
    if (Entity *e = m_scene.FindEntity(m_entityId))
      e->*m_memberPtr = m_before;
  }
  const char *Name() const override { return m_name; }

private:
  const char *m_name;
  Scene &m_scene;
  EntityId m_entityId;
  T Entity::*m_memberPtr;
  T m_before;
  T m_after;
};

// ---- Material change (Phase 2 — Material & Texture Editor) ----

class MaterialCommand : public ICommand {
public:
  MaterialCommand(Scene &scene, DxContext &dx, EntityId id,
                  const Material &matBefore, const Material &matAfter,
                  const std::array<std::string, 6> &pathsBefore,
                  const std::array<std::string, 6> &pathsAfter)
      : m_scene(scene), m_dx(dx), m_entityId(id), m_matBefore(matBefore),
        m_matAfter(matAfter), m_pathsBefore(pathsBefore),
        m_pathsAfter(pathsAfter) {}

  void Execute() override {
    if (Entity *e = m_scene.FindEntity(m_entityId)) {
      if (e->mesh.has_value()) {
        e->mesh->material = m_matAfter;
        bool texturesChanged = (m_pathsBefore != m_pathsAfter);
        if (texturesChanged) {
          e->mesh->texturePaths = m_pathsAfter;
          e->mesh->meshId = UINT32_MAX; // force full GPU re-creation
          m_scene.CreateEntityMeshGpu(m_dx, *e);
        } else {
          m_scene.UpdateEntityMaterial(m_dx, *e);
        }
      }
    }
  }
  void Undo() override {
    if (Entity *e = m_scene.FindEntity(m_entityId)) {
      if (e->mesh.has_value()) {
        e->mesh->material = m_matBefore;
        bool texturesChanged = (m_pathsBefore != m_pathsAfter);
        if (texturesChanged) {
          e->mesh->texturePaths = m_pathsBefore;
          e->mesh->meshId = UINT32_MAX;
          m_scene.CreateEntityMeshGpu(m_dx, *e);
        } else {
          m_scene.UpdateEntityMaterial(m_dx, *e);
        }
      }
    }
  }
  const char *Name() const override { return "Material"; }

private:
  Scene &m_scene;
  DxContext &m_dx;
  EntityId m_entityId;
  Material m_matBefore;
  Material m_matAfter;
  std::array<std::string, 6> m_pathsBefore;
  std::array<std::string, 6> m_pathsAfter;
};

// ---- Scene-global light settings change (Phase 3 — Lighting Panel) ----

class LightSettingsCommand : public ICommand {
public:
  LightSettingsCommand(Scene &scene, const SceneLightSettings &before,
                       const SceneLightSettings &after)
      : m_scene(scene), m_before(before), m_after(after) {}

  void Execute() override { m_scene.LightSettings() = m_after; }
  void Undo() override { m_scene.LightSettings() = m_before; }
  const char *Name() const override { return "Light Settings"; }

private:
  Scene &m_scene;
  SceneLightSettings m_before;
  SceneLightSettings m_after;
};

// ---- Per-entity point light component change ----

class PointLightCommand : public ICommand {
public:
  PointLightCommand(Scene &scene, EntityId id,
                    const PointLightComponent &before,
                    const PointLightComponent &after)
      : m_scene(scene), m_entityId(id), m_before(before), m_after(after) {}

  void Execute() override {
    if (Entity *e = m_scene.FindEntity(m_entityId))
      e->pointLight = m_after;
  }
  void Undo() override {
    if (Entity *e = m_scene.FindEntity(m_entityId))
      e->pointLight = m_before;
  }
  const char *Name() const override { return "Point Light"; }

private:
  Scene &m_scene;
  EntityId m_entityId;
  PointLightComponent m_before;
  PointLightComponent m_after;
};

// ---- Per-entity spot light component change ----

class SpotLightCommand : public ICommand {
public:
  SpotLightCommand(Scene &scene, EntityId id,
                   const SpotLightComponent &before,
                   const SpotLightComponent &after)
      : m_scene(scene), m_entityId(id), m_before(before), m_after(after) {}

  void Execute() override {
    if (Entity *e = m_scene.FindEntity(m_entityId))
      e->spotLight = m_after;
  }
  void Undo() override {
    if (Entity *e = m_scene.FindEntity(m_entityId))
      e->spotLight = m_before;
  }
  const char *Name() const override { return "Spot Light"; }

private:
  Scene &m_scene;
  EntityId m_entityId;
  SpotLightComponent m_before;
  SpotLightComponent m_after;
};

// ---- Scene-global shadow & SSAO settings change (Phase 4) ----

class ShadowSettingsCommand : public ICommand {
public:
  ShadowSettingsCommand(Scene &scene, const SceneShadowSettings &before,
                        const SceneShadowSettings &after)
      : m_scene(scene), m_before(before), m_after(after) {}

  void Execute() override { m_scene.ShadowSettings() = m_after; }
  void Undo() override { m_scene.ShadowSettings() = m_before; }
  const char *Name() const override { return "Shadow Settings"; }

private:
  Scene &m_scene;
  SceneShadowSettings m_before;
  SceneShadowSettings m_after;
};

// ---- Scene-global post-processing settings change (Phase 4) ----

class PostProcessSettingsCommand : public ICommand {
public:
  PostProcessSettingsCommand(Scene &scene, const ScenePostProcessSettings &before,
                             const ScenePostProcessSettings &after)
      : m_scene(scene), m_before(before), m_after(after) {}

  void Execute() override { m_scene.PostProcessSettings() = m_after; }
  void Undo() override { m_scene.PostProcessSettings() = m_before; }
  const char *Name() const override { return "Post-Process Settings"; }

private:
  Scene &m_scene;
  ScenePostProcessSettings m_before;
  ScenePostProcessSettings m_after;
};
