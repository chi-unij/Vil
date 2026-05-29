// ======================================
// File: Entity.h
// Purpose: Core entity data structures for the scene/object model.
//          Milestone 4 Phase 0: Simple struct components with std::optional.
// ======================================

#pragma once

#include "../Camera.h"
#include "../Lighting.h"

#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

using EntityId = uint64_t;
static constexpr EntityId kInvalidEntityId = 0;

// How a mesh was created — needed for serialization/recreation.
enum class MeshSourceType : uint8_t {
  ProceduralCube,
  ProceduralPlane,
  ProceduralCylinder,
  ProceduralCone,
  ProceduralSphere,
  GltfFile,
};

struct Transform {
  DirectX::XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
  DirectX::XMFLOAT3 rotation = {0.0f, 0.0f, 0.0f}; // Euler degrees (pitch, yaw, roll)
  DirectX::XMFLOAT3 scale = {1.0f, 1.0f, 1.0f};

  // Compute world matrix: Scale * Rotation * Translation.
  DirectX::XMMATRIX WorldMatrix() const;
};

struct MeshComponent {
  MeshSourceType sourceType = MeshSourceType::ProceduralCube;
  std::string gltfPath;

  // Procedural mesh parameters (stored for serialization).
  float size = 1.0f;       // cube size, sphere radius
  float width = 1.0f;      // plane width, cylinder/cone radius
  float height = 1.0f;     // plane depth, cylinder/cone height
  uint32_t segments = 16;  // cylinder/cone/sphere segments
  uint32_t rings = 12;     // sphere rings

  // Material overrides.
  Material material = {};

  // Per-slot texture file overrides (empty = use glTF texture or default).
  // Indices: 0=baseColor, 1=normal, 2=metalRough, 3=AO, 4=emissive, 5=height
  std::array<std::string, 6> texturePaths = {};

  // Runtime GPU mesh ID (not serialized — recreated on load).
  uint32_t meshId = UINT32_MAX;
};

struct PointLightComponent {
  float range = 10.0f;
  DirectX::XMFLOAT3 color = {1.0f, 1.0f, 1.0f};
  float intensity = 5.0f;
  bool enabled = true;
};

struct SpotLightComponent {
  DirectX::XMFLOAT3 direction = {0.0f, -1.0f, 0.0f};
  float range = 15.0f;
  DirectX::XMFLOAT3 color = {1.0f, 1.0f, 1.0f};
  float intensity = 10.0f;
  float innerAngleDeg = 15.0f;
  float outerAngleDeg = 30.0f;
  bool enabled = true;
};

// Scene-global lighting settings (not per-entity).
struct SceneLightSettings {
  // Directional (sun) light
  DirectX::XMFLOAT3 lightDir = {0.3f, -1.0f, 0.2f};
  float lightIntensity = 3.0f;
  DirectX::XMFLOAT3 lightColor = {1.0f, 0.98f, 0.92f};

  // Image-Based Lighting
  float iblIntensity = 0.4f;
};

// Scene-global shadow & SSAO settings (Phase 4).
struct SceneShadowSettings {
  // Cascaded Shadow Maps
  bool shadowsEnabled = true;
  float shadowBias = 0.001f;
  float shadowStrength = 1.0f;
  float csmLambda = 0.5f;         // split scheme blend (0=uniform, 1=logarithmic)
  float csmMaxDistance = 200.0f;   // view-space depth limit
  bool csmDebugCascades = false;

  // SSAO
  bool ssaoEnabled = true;
  float ssaoRadius = 0.4f;
  float ssaoBias = 0.025f;
  float ssaoPower = 1.8f;
  int ssaoKernelSize = 32;
  float ssaoStrength = 0.8f;
};

// Scene-global post-processing settings (Phase 4).
struct ScenePostProcessSettings {
  // Tone mapping
  float exposure = 1.2f;

  // Bloom
  bool bloomEnabled = true;
  float bloomThreshold = 0.8f;
  float bloomIntensity = 0.6f;

  // TAA
  bool taaEnabled = false;
  float taaBlendFactor = 0.05f;

  // FXAA
  bool fxaaEnabled = true;

  // Motion blur
  bool motionBlurEnabled = false;
  float motionBlurStrength = 1.0f;
  int motionBlurSamples = 8;

  // Depth of Field
  bool dofEnabled = false;
  float dofFocalDistance = 10.0f;
  float dofFocalRange = 5.0f;
  float dofMaxBlur = 8.0f;
};

struct Entity {
  EntityId id = kInvalidEntityId;
  std::string name = "Entity";
  bool active = true;

  Transform transform;

  std::optional<MeshComponent> mesh;
  std::optional<PointLightComponent> pointLight;
  std::optional<SpotLightComponent> spotLight;
};

// JSON serialization (implemented in Entity.cpp, avoids ADL issues with DirectX types).
nlohmann::json EntityToJson(const Entity &e);
Entity JsonToEntity(const nlohmann::json &j);

nlohmann::json SceneLightSettingsToJson(const SceneLightSettings &s);
SceneLightSettings JsonToSceneLightSettings(const nlohmann::json &j);

nlohmann::json SceneShadowSettingsToJson(const SceneShadowSettings &s);
SceneShadowSettings JsonToSceneShadowSettings(const nlohmann::json &j);

nlohmann::json ScenePostProcessSettingsToJson(const ScenePostProcessSettings &s);
ScenePostProcessSettings JsonToScenePostProcessSettings(const nlohmann::json &j);

// Camera preset serialization (Phase 6).
nlohmann::json CameraPresetToJson(const CameraPreset &p);
CameraPreset JsonToCameraPreset(const nlohmann::json &j);
