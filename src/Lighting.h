// ======================================
// File: Lighting.h
// Purpose: Lighting + material data structures shared between CPU and shaders.
//          Phase 11.5: Material struct separated from light params so each mesh
//          can carry its own PBR surface properties.
// ======================================

#pragma once

#include <DirectXMath.h>

// Per-mesh material properties. Populated from glTF, editable via ImGui.
// Stored per-mesh inside MeshRenderer so different objects can have different
// surfaces.
struct Material {
  // PBR scalar factors (from glTF pbrMetallicRoughness)
  DirectX::XMFLOAT4 baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
  float metallicFactor  = 0.0f;
  float roughnessFactor = 0.8f;
  DirectX::XMFLOAT3 emissiveFactor = {0.0f, 0.0f, 0.0f};

  // UV transform (tiling + offset applied before texture sampling)
  DirectX::XMFLOAT2 uvTiling = {1.0f, 1.0f};
  DirectX::XMFLOAT2 uvOffset = {0.0f, 0.0f};

  // Procedural tile shader type (0 = none, 1 = fire, 2 = ice, etc.)
  float proceduralTypeId = 0.0f;

  // Parallax Occlusion Mapping (per-material, not all meshes have height maps)
  bool  pomEnabled   = false;
  float heightScale  = 0.02f;
  float pomMinLayers = 8.0f;
  float pomMaxLayers = 32.0f;

  // Texture presence flags (set during mesh resource creation)
  bool hasBaseColor  = false;
  bool hasNormal     = false;
  bool hasMetalRough = false;
  bool hasAO         = false;
  bool hasEmissive   = false;
  bool hasHeight     = false;
};

// Scene-global light parameters. Shared across all draws in a frame.
struct LightParams {
  // Direction of *sun rays* traveling through the world (not "to the light").
  // In shader we convert to L = -raysDir.
  DirectX::XMFLOAT3 lightDir = {0.3f, -1.0f, 0.2f};
  float lightIntensity = 3.0f;

  DirectX::XMFLOAT3 lightColor = {1.0f, 0.98f, 0.92f};
  float iblIntensity = 1.0f;

  float cascadeDebug = 0.0f; // >0.5 = tint fragments by cascade index
};

// ---- Multi-light structs (Phase 12.2) ----

static constexpr uint32_t kMaxPointLights = 32;
static constexpr uint32_t kMaxSpotLights  = 32;

// GPU-aligned point light (48 bytes = 3 x float4).
// Bound as StructuredBuffer<GPUPointLight> in deferred_lighting.hlsl.
struct GPUPointLight {
  DirectX::XMFLOAT3 position;
  float range;
  DirectX::XMFLOAT3 color;
  float intensity;
  DirectX::XMFLOAT3 _pad;
  float _pad2;
};
static_assert(sizeof(GPUPointLight) == 48, "GPUPointLight must be 48 bytes");

// GPU-aligned spot light (64 bytes = 4 x float4).
struct GPUSpotLight {
  DirectX::XMFLOAT3 position;
  float range;
  DirectX::XMFLOAT3 color;
  float intensity;
  DirectX::XMFLOAT3 direction;
  float innerConeAngleCos;
  float outerConeAngleCos;
  DirectX::XMFLOAT3 _pad;
};
static_assert(sizeof(GPUSpotLight) == 64, "GPUSpotLight must be 64 bytes");

// CPU-side editor structs for ImGui (user-friendly units).
struct PointLightEditor {
  DirectX::XMFLOAT3 position = {0.0f, 3.0f, 0.0f};
  float range = 10.0f;
  DirectX::XMFLOAT3 color = {1.0f, 1.0f, 1.0f};
  float intensity = 5.0f;
  bool enabled = true;
};

struct SpotLightEditor {
  DirectX::XMFLOAT3 position = {0.0f, 5.0f, 0.0f};
  DirectX::XMFLOAT3 direction = {0.0f, -1.0f, 0.0f};
  float range = 15.0f;
  DirectX::XMFLOAT3 color = {1.0f, 1.0f, 1.0f};
  float intensity = 10.0f;
  float innerAngleDeg = 15.0f;
  float outerAngleDeg = 30.0f;
  bool enabled = true;
};
