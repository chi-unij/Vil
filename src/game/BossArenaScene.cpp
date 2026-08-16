#include "game/BossArenaScene.h"

#include "GltfLoader.h"
#include "MeshRenderer.h"
#include "ProceduralMesh.h"
#include "game/PlayerAnimationPreview.h"

#include <DirectXMath.h>
#include <Windows.h>
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <imgui.h>

using namespace DirectX;

namespace {

constexpr float kCurveRadius = 118.0f;
constexpr float kCurveOriginZ = -12.0f;
constexpr int kMirrorChargeMax = 3;

uint32_t MixPuzzleSeed(uint32_t value) {
  value ^= value >> 16;
  value *= 2246822519u;
  value ^= value >> 13;
  value *= 3266489917u;
  value ^= value >> 16;
  return value;
}

float CurveAngle(float z) { return (z - kCurveOriginZ) / kCurveRadius; }

XMFLOAT3 CurvePoint(float x, float y, float z) {
  const float angle = CurveAngle(z);
  const float curvedY = kCurveRadius * (std::cos(angle) - 1.0f);
  const float curvedZ = kCurveOriginZ + kCurveRadius * std::sin(angle);
  return {x, curvedY + y, curvedZ};
}

XMMATRIX CurveWorld(float sx, float sy, float sz, float x, float y, float z,
                    float yaw = 0.0f) {
  const float angle = CurveAngle(z);
  const XMFLOAT3 p = CurvePoint(x, y, z);
  return XMMatrixScaling(sx, sy, sz) * XMMatrixRotationY(yaw) *
         XMMatrixRotationX(angle) * XMMatrixTranslation(p.x, p.y, p.z);
}

LoadedMesh CreateCurvedRiverSurface(float width, float minZ, float maxZ,
                                    float surfaceY) {
  constexpr uint32_t kWidthSegments = 8;
  constexpr uint32_t kLengthSegments = 160;
  const float depth = maxZ - minZ;
  const float centerZ = (minZ + maxZ) * 0.5f;
  LoadedMesh mesh = ProceduralMesh::CreateTessellatedPlane(
      width, depth, kWidthSegments, kLengthSegments);

  // 一枚の連続面を arena の曲率へ沿わせ、頂点法線も同じ曲率で滑らかにする。
  // 分割した平面を並べる方式では SSR が各面で折れ、長い縞に見えてしまう。
  for (MeshVertex &vertex : mesh.vertices) {
    const float logicalZ = centerZ + vertex.pos[2];
    const float angle = CurveAngle(logicalZ);
    const XMFLOAT3 curved =
        CurvePoint(vertex.pos[0], surfaceY, logicalZ);
    vertex.pos[0] = curved.x;
    vertex.pos[1] = curved.y;
    vertex.pos[2] = curved.z;
    vertex.normal[0] = 0.0f;
    vertex.normal[1] = std::cos(angle);
    vertex.normal[2] = std::sin(angle);
    vertex.tangent[0] = 1.0f;
    vertex.tangent[1] = 0.0f;
    vertex.tangent[2] = 0.0f;
    vertex.tangent[3] = 1.0f;
  }
  return mesh;
}

XMMATRIX VerticalVeilWorld(float sx, float sy, float x, float y, float z,
                           float yaw = 0.0f) {
  const float angle = CurveAngle(z);
  const XMFLOAT3 p = CurvePoint(x, y, z);
  return XMMatrixScaling(sx, 1.0f, sy) * XMMatrixRotationX(-XM_PIDIV2) *
         XMMatrixRotationY(yaw) * XMMatrixRotationX(angle) *
         XMMatrixTranslation(p.x, p.y, p.z);
}

float LoopZ(float baseZ, float scroll, float minZ, float length) {
  float local = std::fmod(baseZ - minZ - scroll, length);
  if (local < 0.0f)
    local += length;
  return minZ + local;
}

float LoopEdgeFade(float z, float minZ, float maxZ, float fadeDistance) {
  const float nearEdge = std::min(z - minZ, maxZ - z);
  return std::clamp(nearEdge / fadeDistance, 0.0f, 1.0f);
}

void PushLine(FrameData &frame, const XMFLOAT3 &a, const XMFLOAT3 &b,
              const XMFLOAT4 &color) {
  frame.debugLines.push_back({a, b, color});
}

void PushCurvedLine(FrameData &frame, float x0, float y0, float z0, float x1,
                    float y1, float z1, const XMFLOAT4 &color,
                    int steps = 10) {
  XMFLOAT3 prev = CurvePoint(x0, y0, z0);
  for (int i = 1; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    const float x = std::lerp(x0, x1, t);
    const float y = std::lerp(y0, y1, t);
    const float z = std::lerp(z0, z1, t);
    const XMFLOAT3 p = CurvePoint(x, y, z);
    PushLine(frame, prev, p, color);
    prev = p;
  }
}

void PushCircle(FrameData &frame, const XMFLOAT3 &center, float radius,
                const XMFLOAT4 &color) {
  constexpr int kSegments = 48;
  XMFLOAT3 prev{center.x + radius, center.y, center.z};
  for (int i = 1; i <= kSegments; ++i) {
    const float t = (static_cast<float>(i) / kSegments) * XM_2PI;
    const XMFLOAT3 p{center.x + std::cos(t) * radius, center.y,
                     center.z + std::sin(t) * radius};
    PushLine(frame, prev, p, color);
    prev = p;
  }
}

void PushThickCircle(FrameData &frame, const XMFLOAT3 &center, float radius,
                     const XMFLOAT4 &color, int bands = 3) {
  for (int i = 0; i < bands; ++i) {
    PushCircle(frame, center, radius + static_cast<float>(i) * 0.10f, color);
  }
}

void PushRect(FrameData &frame, float minX, float minZ, float maxX, float maxZ,
              float y, const XMFLOAT4 &color) {
  const XMFLOAT3 a{minX, y, minZ};
  const XMFLOAT3 b{maxX, y, minZ};
  const XMFLOAT3 c{maxX, y, maxZ};
  const XMFLOAT3 d{minX, y, maxZ};
  PushLine(frame, a, b, color);
  PushLine(frame, b, c, color);
  PushLine(frame, c, d, color);
  PushLine(frame, d, a, color);
}

void PushCurvedRect(FrameData &frame, float minX, float minZ, float maxX,
                    float maxZ, float y, const XMFLOAT4 &color) {
  PushCurvedLine(frame, minX, y, minZ, maxX, y, minZ, color, 4);
  PushCurvedLine(frame, maxX, y, minZ, maxX, y, maxZ, color, 16);
  PushCurvedLine(frame, maxX, y, maxZ, minX, y, maxZ, color, 4);
  PushCurvedLine(frame, minX, y, maxZ, minX, y, minZ, color, 16);
}

void PushCurvedRibbon(FrameData &frame, uint32_t meshId, float x0, float y0,
                      float z0, float x1, float y1, float z1, float width,
                      int steps = 10) {
  if (meshId == UINT32_MAX)
    return;

  steps = std::max(1, steps);
  for (int i = 0; i < steps; ++i) {
    const float t0 = static_cast<float>(i) / static_cast<float>(steps);
    const float t1 = static_cast<float>(i + 1) / static_cast<float>(steps);
    const float ax = std::lerp(x0, x1, t0);
    const float ay = std::lerp(y0, y1, t0);
    const float az = std::lerp(z0, z1, t0);
    const float bx = std::lerp(x0, x1, t1);
    const float by = std::lerp(y0, y1, t1);
    const float bz = std::lerp(z0, z1, t1);
    const float dx = bx - ax;
    const float dz = bz - az;
    const float length = std::max(0.08f, std::sqrt(dx * dx + dz * dz));
    const float yaw = std::atan2(dx, dz);
    frame.transparentItems.push_back(
        {meshId, CurveWorld(width, 1.0f, length + width * 0.35f,
                            (ax + bx) * 0.5f, (ay + by) * 0.5f,
                            (az + bz) * 0.5f, yaw)});
  }
}

float DistanceSq2D(const XMFLOAT3 &a, const XMFLOAT3 &b) {
  const float dx = a.x - b.x;
  const float dz = a.z - b.z;
  return dx * dx + dz * dz;
}

float ClampArena(float v, float halfExtent) {
  return std::clamp(v, -halfExtent, halfExtent);
}

float EaseOutCubic(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  const float inv = 1.0f - t;
  return 1.0f - inv * inv * inv;
}

ImU32 Rgba(float r, float g, float b, float a) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a));
}

std::string ResolveBossArenaAssetPath(const std::string &path) {
  namespace fs = std::filesystem;

  const fs::path direct(path);
  const fs::path sourceFromBuild = fs::path("..") / ".." / ".." / path;
  if (fs::exists(sourceFromBuild))
    return sourceFromBuild.generic_string();
  if (fs::exists(direct))
    return direct.generic_string();
  return path;
}

bool LoadBossArenaModelMeshIds(DxContext &dx, const std::string &path,
                               std::vector<uint32_t> &outMeshIds) {
  const std::string resolvedPath = ResolveBossArenaAssetPath(path);
  std::vector<LoadedMeshPart> parts;
  if (!LoadStaticModelParts(resolvedPath, parts)) {
    OutputDebugStringA("[BossArenaScene] WARNING: model load failed: ");
    OutputDebugStringA(resolvedPath.c_str());
    OutputDebugStringA("\n");
    return false;
  }

  for (const LoadedMeshPart &part : parts) {
    const uint32_t meshId =
        dx.CreateMeshResources(part.mesh, part.GetMaterialImages(),
                               part.material);
    if (meshId != UINT32_MAX)
      outMeshIds.push_back(meshId);
  }

  if (outMeshIds.empty()) {
    OutputDebugStringA("[BossArenaScene] WARNING: no mesh uploaded for model: ");
    OutputDebugStringA(resolvedPath.c_str());
    OutputDebugStringA("\n");
    return false;
  }

  OutputDebugStringA("[BossArenaScene] model loaded: ");
  OutputDebugStringA(resolvedPath.c_str());
  OutputDebugStringA("\n");
  return true;
}

float SmoothPulse(float time, float speed, float floor = 0.0f) {
  const float wave = 0.5f + 0.5f * std::sin(time * speed);
  return floor + (1.0f - floor) * wave;
}

void DrawArc(ImDrawList *draw, const ImVec2 &center, float radius,
             float startRad, float endRad, ImU32 color, float thickness) {
  constexpr int kSteps = 24;
  ImVec2 prev(center.x + std::cos(startRad) * radius,
              center.y + std::sin(startRad) * radius);
  for (int i = 1; i <= kSteps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSteps);
    const float a = startRad + (endRad - startRad) * t;
    const ImVec2 p(center.x + std::cos(a) * radius,
                   center.y + std::sin(a) * radius);
    draw->AddLine(prev, p, color, thickness);
    prev = p;
  }
}

const char *MirrorSymbolLabel(int symbol) {
  switch (symbol % 3) {
  case 0:
    return "MOON";
  case 1:
    return "WATER";
  default:
    return "FIRE";
  }
}

ImU32 MirrorSymbolColor(int symbol, float alpha) {
  switch (symbol % 3) {
  case 0:
    return Rgba(0.78f, 0.92f, 1.0f, alpha);
  case 1:
    return Rgba(0.38f, 1.0f, 0.86f, alpha);
  default:
    return Rgba(1.0f, 0.55f, 0.22f, alpha);
  }
}

void PushMeshVertex(LoadedMesh &mesh, float px, float py, float pz, float u,
                    float v) {
  MeshVertex vertex{};
  vertex.pos[0] = px;
  vertex.pos[1] = py;
  vertex.pos[2] = pz;
  vertex.normal[0] = 0.0f;
  vertex.normal[1] = 1.0f;
  vertex.normal[2] = 0.0f;
  vertex.uv[0] = u;
  vertex.uv[1] = v;
  vertex.tangent[0] = 1.0f;
  vertex.tangent[1] = 0.0f;
  vertex.tangent[2] = 0.0f;
  vertex.tangent[3] = 1.0f;
  mesh.vertices.push_back(vertex);
}

LoadedMesh CreateUnitDiskMesh(uint32_t segments) {
  LoadedMesh mesh{};
  segments = std::max<uint32_t>(segments, 12);
  PushMeshVertex(mesh, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f);

  for (uint32_t i = 0; i < segments; ++i) {
    const float t = (static_cast<float>(i) / segments) * XM_2PI;
    const float x = std::cos(t);
    const float z = std::sin(t);
    PushMeshVertex(mesh, x, 0.0f, z, x * 0.5f + 0.5f, 0.5f - z * 0.5f);
  }

  for (uint32_t i = 0; i < segments; ++i) {
    const uint32_t current = i + 1;
    const uint32_t next = ((i + 1) % segments) + 1;
    mesh.indices.push_back(0);
    mesh.indices.push_back(next);
    mesh.indices.push_back(current);
  }
  return mesh;
}


LoadedMesh CreateFlameCardMesh() {
  LoadedMesh mesh{};
  PushMeshVertex(mesh, -0.45f, 0.0f, 0.0f, 0.0f, 1.0f);
  PushMeshVertex(mesh, 0.45f, 0.0f, 0.0f, 1.0f, 1.0f);
  PushMeshVertex(mesh, 0.28f, 0.0f, 0.58f, 0.78f, 0.38f);
  PushMeshVertex(mesh, 0.0f, 0.0f, 1.0f, 0.5f, 0.0f);
  PushMeshVertex(mesh, -0.28f, 0.0f, 0.58f, 0.22f, 0.38f);

  mesh.indices = {0, 1, 2, 0, 2, 4, 4, 2, 3,
                  2, 1, 0, 4, 2, 0, 3, 2, 4};
  return mesh;
}
Material MakeTelegraphMaterial(const XMFLOAT4 &baseColor,
                               const XMFLOAT3 &emissive) {
  Material material{};
  material.baseColorFactor = baseColor;
  material.emissiveFactor = emissive;
  material.metallicFactor = 0.0f;
  material.roughnessFactor = 0.35f;
  return material;
}
} // namespace

void BossArenaScene::Initialize(DxContext &dx) {
  const LoadedMesh floorMesh =
      ProceduralMesh::CreatePlane(kArenaHalfExtent * 2.0f,
                                  kArenaHalfExtent * 2.0f);
  Material floorMaterial{};
  floorMaterial.baseColorFactor = {0.18f, 0.20f, 0.18f, 1.0f};
  floorMaterial.roughnessFactor = 0.92f;
  floorMaterial.uvTiling = {8.0f, 8.0f};
  floorMaterial.proceduralTypeId = 7.0f;
  floorMaterial.reflectionReceiver = ReflectionReceiver::Water;
  floorMaterial.rayTracingVisible = false;
  m_floorMeshId = dx.CreateMeshResources(floorMesh, {}, floorMaterial);

  const LoadedMesh bossMesh = ProceduralMesh::CreateCube(1.0f);
  Material bossMaterial{};
  bossMaterial.baseColorFactor = {0.12f, 0.02f, 0.02f, 1.0f};
  bossMaterial.emissiveFactor = {0.35f, 0.02f, 0.01f};
  bossMaterial.roughnessFactor = 0.55f;
  m_bossMeshId = dx.CreateMeshResources(bossMesh, {}, bossMaterial);

  const LoadedMesh diskMesh = CreateUnitDiskMesh(96);
  const LoadedMesh telegraphPlane = ProceduralMesh::CreatePlane(1.0f, 1.0f);
  const LoadedMesh flameCardMesh = CreateFlameCardMesh();
  const LoadedMesh cubeMesh = ProceduralMesh::CreateCube(1.0f);
  constexpr float kRiverHalfWidth = 5.15f - 0.28f;
  const LoadedMesh riverWaterMesh = CreateCurvedRiverSurface(
      kRiverHalfWidth * 2.0f, -kArenaHalfExtent, kArenaHalfExtent, 0.018f);
  const LoadedMesh lanternPostMesh =
      ProceduralMesh::CreateCylinder(0.5f, 1.0f, 12);
  const LoadedMesh lanternGlowMesh = ProceduralMesh::CreateSphere(0.5f, 8, 16);
  const LoadedMesh sanctuaryDomeMesh =
      ProceduralMesh::CreateHemisphere(1.0f, 16, 32);

  m_aoeTelegraphMeshId = dx.CreateMeshResources(
      diskMesh, {},
      MakeTelegraphMaterial({1.0f, 0.04f, 0.02f, 0.42f},
                            {1.65f, 0.05f, 0.02f}));
  m_laserTelegraphMeshId = dx.CreateMeshResources(
      telegraphPlane, {},
      MakeTelegraphMaterial({1.0f, 0.02f, 0.02f, 0.36f},
                            {1.55f, 0.02f, 0.02f}));
  m_knockbackTelegraphMeshId = dx.CreateMeshResources(
      diskMesh, {},
      MakeTelegraphMaterial({1.0f, 0.58f, 0.05f, 0.38f},
                            {1.55f, 0.55f, 0.04f}));
  Material sanctuaryDomeMaterial =
      MakeTelegraphMaterial({0.08f, 0.72f, 0.86f, 0.22f},
                            {0.18f, 1.05f, 1.30f});
  sanctuaryDomeMaterial.roughnessFactor = 0.14f;
  sanctuaryDomeMaterial.proceduralTypeId = 10.0f;
  m_sanctuaryDomeMeshId = dx.CreateMeshResources(
      sanctuaryDomeMesh, {}, sanctuaryDomeMaterial);
  m_flameMeshId = dx.CreateMeshResources(
      flameCardMesh, {},
      MakeTelegraphMaterial({1.0f, 0.22f, 0.02f, 0.78f},
                            {2.2f, 0.42f, 0.06f}));
  m_pathGlowMeshId = dx.CreateMeshResources(
      telegraphPlane, {},
      MakeTelegraphMaterial({0.10f, 0.56f, 0.82f, 0.22f},
                            {0.05f, 0.28f, 0.44f}));
  m_attackSmokeMeshId = dx.CreateMeshResources(
      telegraphPlane, {},
      MakeTelegraphMaterial({0.32f, 0.46f, 0.50f, 0.26f},
                            {0.08f, 0.18f, 0.22f}));
  m_spiritVeilMeshId = dx.CreateMeshResources(
      telegraphPlane, {},
      MakeTelegraphMaterial({0.24f, 0.72f, 0.96f, 0.18f},
                            {0.12f, 0.58f, 0.82f}));
  m_laserRiftMeshId = dx.CreateMeshResources(
      telegraphPlane, {},
      MakeTelegraphMaterial({1.0f, 0.08f, 0.16f, 0.56f},
                            {2.8f, 0.10f, 0.16f}));
  m_moonDiscMeshId = dx.CreateMeshResources(
      diskMesh, {},
      MakeTelegraphMaterial({0.44f, 0.86f, 1.0f, 0.30f},
                            {0.40f, 1.10f, 1.35f}));
  Material moonRayMaterial =
      MakeTelegraphMaterial({0.46f, 0.74f, 0.82f, 0.085f},
                            {0.06f, 0.18f, 0.23f});
  moonRayMaterial.proceduralTypeId = 9.0f;
  m_moonRayMeshId =
      dx.CreateMeshResources(telegraphPlane, {}, moonRayMaterial);
  m_bossSealMeshId = dx.CreateMeshResources(
      diskMesh, {},
      MakeTelegraphMaterial({0.95f, 0.12f, 0.04f, 0.30f},
                            {1.2f, 0.18f, 0.05f}));
  m_counterRibbonMeshId = dx.CreateMeshResources(
      telegraphPlane, {},
      MakeTelegraphMaterial({0.42f, 1.0f, 0.88f, 0.58f},
                            {0.48f, 1.75f, 1.25f}));

  Material pathStoneMaterial{};
  pathStoneMaterial.baseColorFactor = {0.32f, 0.35f, 0.34f, 1.0f};
  pathStoneMaterial.emissiveFactor = {0.010f, 0.016f, 0.018f};
  pathStoneMaterial.roughnessFactor = 0.72f;
  pathStoneMaterial.proceduralTypeId = 7.0f;
  pathStoneMaterial.reflectionReceiver = ReflectionReceiver::Water;
  pathStoneMaterial.rayTracingVisible = false;
  pathStoneMaterial.ssrExcluded = true;
  m_pathStoneMeshId = dx.CreateMeshResources(cubeMesh, {}, pathStoneMaterial);

  Material pathEdgeMaterial{};
  pathEdgeMaterial.baseColorFactor = {0.10f, 0.34f, 0.31f, 1.0f};
  pathEdgeMaterial.emissiveFactor = {0.04f, 0.22f, 0.18f};
  pathEdgeMaterial.roughnessFactor = 0.58f;
  pathEdgeMaterial.ssrExcluded = true;
  m_pathEdgeMeshId = dx.CreateMeshResources(cubeMesh, {}, pathEdgeMaterial);

  Material toriiWoodMaterial{};
  toriiWoodMaterial.baseColorFactor = {0.42f, 0.035f, 0.018f, 1.0f};
  toriiWoodMaterial.emissiveFactor = {0.12f, 0.012f, 0.004f};
  toriiWoodMaterial.roughnessFactor = 0.72f;
  m_toriiWoodMeshId = dx.CreateMeshResources(cubeMesh, {}, toriiWoodMaterial);

  Material mossBankMaterial{};
  mossBankMaterial.baseColorFactor = {0.10f, 0.19f, 0.12f, 1.0f};
  mossBankMaterial.roughnessFactor = 0.98f;
  mossBankMaterial.proceduralTypeId = 7.0f;
  mossBankMaterial.reflectionReceiver = ReflectionReceiver::Water;
  mossBankMaterial.rayTracingVisible = false;
  m_mossBankMeshId = dx.CreateMeshResources(cubeMesh, {}, mossBankMaterial);

  Material lanternPostMaterial{};
  lanternPostMaterial.baseColorFactor = {0.16f, 0.08f, 0.04f, 1.0f};
  lanternPostMaterial.roughnessFactor = 0.75f;
  m_lanternPostMeshId =
      dx.CreateMeshResources(lanternPostMesh, {}, lanternPostMaterial);

  Material lanternCapMaterial{};
  lanternCapMaterial.baseColorFactor = {0.09f, 0.055f, 0.035f, 1.0f};
  lanternCapMaterial.roughnessFactor = 0.68f;
  m_lanternCapMeshId =
      dx.CreateMeshResources(cubeMesh, {}, lanternCapMaterial);

  Material lanternGlowMaterial{};
  lanternGlowMaterial.baseColorFactor = {1.0f, 0.42f, 0.10f, 0.82f};
  lanternGlowMaterial.emissiveFactor = {2.6f, 0.72f, 0.14f};
  lanternGlowMaterial.roughnessFactor = 0.18f;
  m_lanternGlowMeshId =
      dx.CreateMeshResources(lanternGlowMesh, {}, lanternGlowMaterial);

  Material lanternHaloMaterial{};
  lanternHaloMaterial.baseColorFactor = {1.0f, 0.56f, 0.22f, 0.30f};
  lanternHaloMaterial.emissiveFactor = {2.4f, 0.78f, 0.20f};
  lanternHaloMaterial.roughnessFactor = 0.18f;
  m_lanternHaloMeshId =
      dx.CreateMeshResources(lanternGlowMesh, {}, lanternHaloMaterial);

  Material mirrorChargeMaterial{};
  mirrorChargeMaterial.baseColorFactor = {0.30f, 0.95f, 0.84f, 0.72f};
  mirrorChargeMaterial.emissiveFactor = {0.36f, 1.85f, 1.25f};
  mirrorChargeMaterial.roughnessFactor = 0.18f;
  m_mirrorChargeMeshId =
      dx.CreateMeshResources(lanternGlowMesh, {}, mirrorChargeMaterial);

  Material mirrorShardMaterial{};
  mirrorShardMaterial.baseColorFactor = {0.18f, 0.82f, 0.92f, 0.62f};
  mirrorShardMaterial.emissiveFactor = {0.16f, 1.18f, 1.05f};
  mirrorShardMaterial.roughnessFactor = 0.10f;
  m_mirrorShardMeshId =
      dx.CreateMeshResources(cubeMesh, {}, mirrorShardMaterial);

  Material bossDamageShardMaterial{};
  bossDamageShardMaterial.baseColorFactor = {1.0f, 0.16f, 0.08f, 0.72f};
  bossDamageShardMaterial.emissiveFactor = {1.8f, 0.18f, 0.05f};
  bossDamageShardMaterial.roughnessFactor = 0.16f;
  m_bossDamageShardMeshId =
      dx.CreateMeshResources(cubeMesh, {}, bossDamageShardMaterial);

  Material ofudaMaterial{};
  ofudaMaterial.baseColorFactor = {0.86f, 0.78f, 0.52f, 0.86f};
  ofudaMaterial.emissiveFactor = {0.32f, 0.20f, 0.06f};
  ofudaMaterial.roughnessFactor = 0.42f;
  m_ofudaMeshId = dx.CreateMeshResources(cubeMesh, {}, ofudaMaterial);

  Material riverWaterMaterial{};
  riverWaterMaterial.baseColorFactor = {0.16f, 0.50f, 0.62f, 1.0f};
  riverWaterMaterial.metallicFactor = 0.0f;
  riverWaterMaterial.roughnessFactor = 0.028f;
  riverWaterMaterial.emissiveFactor = {0.0f, 0.006f, 0.014f};
  riverWaterMaterial.uvTiling = {1.0f, 1.0f};
  riverWaterMaterial.proceduralTypeId = 8.0f;
  riverWaterMaterial.vertexDeformTypeId = 0.0f;
  riverWaterMaterial.reflectionReceiver = ReflectionReceiver::Water;
  riverWaterMaterial.rayTracingVisible = false;
  m_riverWaterMeshId =
      dx.CreateMeshResources(riverWaterMesh, {}, riverWaterMaterial);

  LoadBossArenaModelMeshIds(dx, "Assets/models/japanese_shrine_lantern.glb",
                            m_lanternMeshIds);
  LoadBossArenaModelMeshIds(dx, "Assets/models/japanese_shrine/scene.gltf",
                            m_shrineMeshIds);
  LoadBossArenaModelMeshIds(dx, "Assets/models/shrine_gate.glb",
                            m_shrineGateMeshIds);
  m_aoeSparkBurst = std::make_unique<SparkBurstEmitter>(
      192, XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f), 72);
  m_meteorFlameEmitter = std::make_unique<MeteorFlameEmitter>(
      320, XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f));
  m_lineRiftEmitter = std::make_unique<LineRiftEmitter>(
      420, XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f));
  m_counterSparkBurst = std::make_unique<MirrorSparkBurstEmitter>(
      192, XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f), 72);
  m_mirrorPickupBurst = std::make_unique<MirrorPickupBurstEmitter>(
      256, XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f), 88);
  constexpr float petalZs[4] = {-13.0f, -4.0f, 5.0f, 13.0f};
  for (int i = 0; i < 4; ++i) {
    const XMFLOAT3 petalPos = CurvePoint(0.0f, 8.6f, petalZs[i]);
    m_counterPetalEmitters[i] = std::make_unique<SakuraPetalBurstEmitter>(
        72, XMVectorSet(petalPos.x, petalPos.y, petalPos.z, 0.0f), 42);
  }
  m_playerHitBurst = std::make_unique<DamageDropletBurstEmitter>(
      96, XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f), 36);
  const XMFLOAT3 bossSmokePos = CurvePoint(0.0f, 1.4f, kArenaHalfExtent - 3.5f);
  m_bossSmokeEmitter = std::make_unique<SmokeEmitter>(
      180, XMVectorSet(bossSmokePos.x, bossSmokePos.y, bossSmokePos.z, 0.0f),
      34.0, true);
  constexpr float mistZs[5] = {-15.6f, -13.4f, -11.2f, -9.0f, -6.8f};
  for (int i = 0; i < 5; ++i) {
    const XMFLOAT3 mistPos = CurvePoint(0.0f, 0.22f, mistZs[i]);
    m_riverMistEmitters[i] = std::make_unique<RiverMistEmitter>(
        96, XMVectorSet(mistPos.x, mistPos.y, mistPos.z, 0.0f), 15.0, false);
  }
  constexpr float sideFogZs[6] = {-15.0f, -9.0f, -3.0f,
                                  3.0f,   9.0f,  14.5f};
  for (int i = 0; i < 12; ++i) {
    const float side = (i % 2 == 0) ? -1.0f : 1.0f;
    const float z = sideFogZs[i / 2];
    const XMFLOAT3 fogPos = CurvePoint(side * 10.8f, 0.16f, z);
    m_sideFogEmitters[i] = std::make_unique<ShrineFogEmitter>(
        160, XMVectorSet(fogPos.x, fogPos.y, fogPos.z, 0.0f), side, 22.0,
        true);
    m_sideFogEmitters[i]->WarmStart(6.0);
  }
  constexpr float electricZs[7] = {-15.0f, -10.0f, -5.0f, 0.0f,
                                   5.0f,   10.0f,  15.0f};
  for (int i = 0; i < 7; ++i) {
    const XMFLOAT3 electricPos = CurvePoint(0.0f, 0.18f, electricZs[i]);
    m_readyRiverElectricEmitters[i] = std::make_unique<RiverElectricEmitter>(
        96, XMVectorSet(electricPos.x, electricPos.y, electricPos.z, 0.0f));
  }

  m_ready = (m_floorMeshId != UINT32_MAX && m_bossMeshId != UINT32_MAX &&
             m_aoeTelegraphMeshId != UINT32_MAX &&
             m_laserTelegraphMeshId != UINT32_MAX &&
             m_knockbackTelegraphMeshId != UINT32_MAX &&
             m_sanctuaryDomeMeshId != UINT32_MAX &&
             m_flameMeshId != UINT32_MAX &&
             m_pathGlowMeshId != UINT32_MAX &&
             m_attackSmokeMeshId != UINT32_MAX &&
             m_spiritVeilMeshId != UINT32_MAX &&
             m_laserRiftMeshId != UINT32_MAX &&
             m_moonDiscMeshId != UINT32_MAX &&
             m_moonRayMeshId != UINT32_MAX &&
             m_bossSealMeshId != UINT32_MAX &&
             m_counterRibbonMeshId != UINT32_MAX &&
             m_pathStoneMeshId != UINT32_MAX &&
             m_pathEdgeMeshId != UINT32_MAX &&
             m_toriiWoodMeshId != UINT32_MAX &&
             m_mossBankMeshId != UINT32_MAX &&
             m_lanternPostMeshId != UINT32_MAX &&
             m_lanternCapMeshId != UINT32_MAX &&
             m_lanternGlowMeshId != UINT32_MAX &&
             m_lanternHaloMeshId != UINT32_MAX &&
             m_riverWaterMeshId != UINT32_MAX &&
             m_mirrorChargeMeshId != UINT32_MAX &&
             m_mirrorShardMeshId != UINT32_MAX &&
             m_bossDamageShardMeshId != UINT32_MAX &&
             m_ofudaMeshId != UINT32_MAX);
}

void BossArenaScene::Reset(PlayerAnimationPreview &player) {
  m_attack = AttackType::MeteorAoE;
  m_phase = AttackPhase::Telegraph;
  m_attackIndex = 0;
  m_phaseTwoPatternIndex = 0;
  m_phaseTimer = 1.6f;
  m_resolved = false;
  m_phaseTwoIntroPending = false;
  m_sanctuaryIntroActive = false;
  m_attackCenter = {0.0f, 0.02f, 6.0f};
  m_attackRadius = 4.0f;
  m_laserVertical = true;
  m_playerHp = 3;
  m_bossHp = 5;
  m_battleTimer = 0.0f;
  m_countersUsed = 0;
  m_damageTaken = 0;
  m_hitFlashTimer = 0.0f;
  m_playerHitParticleTimer = 0.0f;
  m_failed = false;
  m_cleared = false;
  m_aoeSparkTimer = 0.0f;
  m_counterVfxTimer = 0.0f;
  m_counterPetalTimer = 0.0f;
  m_bossHitShakeTimer = 0.0f;
  m_phaseShiftVfxTimer = 0.0f;
  m_phoneOpen = false;
  m_phoneSlide = 0.0f;
  m_phoneSpaceWasDown = false;
  m_mirrorPuzzleReady = false;
  m_mirrorPuzzleSolved = false;
  m_mirrorMessageTimer = 0.0f;
  m_mirrorPickupToastTimer = 0.0f;
  m_mirrorPickupVfxTimer = 0.0f;
  m_lastMirrorPickupPosition = {0.0f, 0.0f, 0.0f};
  m_memoryTimer = 0.0f;
  m_memoryFailTimer = 0.0f;
  m_mirrorCharge = 0;
  m_memoryInputIndex = 0;
  ResetMirrorPuzzleSchedule();
  ResetReflectionTrace();
  m_readyRiverElectricWasActive = false;
  if (m_meteorFlameEmitter)
    m_meteorFlameEmitter->Emmit(false);
  if (m_lineRiftEmitter)
    m_lineRiftEmitter->Emmit(false);
  for (auto &electricEmitter : m_readyRiverElectricEmitters) {
    if (electricEmitter)
      electricEmitter->Emmit(false);
  }
  m_mirrorChargeActive = {false, false, false};
  SpawnMirrorCharges();
  player.SetPosition({0.0f, 0.0f, -12.0f});
  player.SetYaw(0.0f);
}

void BossArenaScene::Update(float dt, const Input &input,
                            PlayerAnimationPreview &player) {
#if defined(_DEBUG)
  const bool debugPanelToggleDown = input.IsKeyDown(VK_F3);
  if (debugPanelToggleDown && !m_debugPanelToggleWasDown)
    m_showDebugPanel = !m_showDebugPanel;
  m_debugPanelToggleWasDown = debugPanelToggleDown;
#else
  m_showDebugPanel = false;
  m_debugNoClip = false;
#endif

  if (!m_debugNoClip) {
    XMFLOAT3 lanePos = player.Position();
    lanePos.x = std::clamp(lanePos.x, -kLaneHalfWidth, kLaneHalfWidth);
    lanePos.z = std::clamp(lanePos.z, kRunnerMinZ, kRunnerMaxZ);
    player.SetPosition(lanePos);
  }

  UpdatePhoneOverlay(dt, input);
  m_aoeSparkTimer = std::max(0.0f, m_aoeSparkTimer - dt);
  if (m_aoeSparkBurst && m_aoeSparkTimer > 0.0f)
    m_aoeSparkBurst->Update(static_cast<double>(dt));
  if (m_counterSparkBurst && m_counterVfxTimer > 0.0f)
    m_counterSparkBurst->Update(static_cast<double>(dt));
  m_counterPetalTimer = std::max(0.0f, m_counterPetalTimer - dt);
  if (m_counterPetalTimer > 0.0f) {
    for (auto &petalEmitter : m_counterPetalEmitters) {
      if (petalEmitter && !petalEmitter->IsFinished())
        petalEmitter->Update(static_cast<double>(dt));
    }
  }
  m_playerHitParticleTimer =
      std::max(0.0f, m_playerHitParticleTimer - dt);
  if (m_playerHitParticleTimer > 0.0f && m_playerHitBurst &&
      !m_playerHitBurst->IsFinished()) {
    m_playerHitBurst->Update(static_cast<double>(dt));
  }
  if (m_mirrorPickupBurst && m_mirrorPickupVfxTimer > 0.0f &&
      !m_mirrorPickupBurst->IsFinished())
    m_mirrorPickupBurst->Update(static_cast<double>(dt));
  if (m_meteorFlameEmitter) {
    float meteorFlameIntensity = 0.0f;
    const bool meteorVisible =
        m_attack == AttackType::MeteorAoE && !m_phoneOpen && !m_failed &&
        !m_cleared;
    if (meteorVisible && m_phase == AttackPhase::Telegraph) {
      const float charge = 1.0f -
          std::clamp(m_phaseTimer / TelegraphDuration(), 0.0f, 1.0f);
      meteorFlameIntensity = 0.24f + charge * 0.66f;
    } else if (meteorVisible && m_impactTimer > 0.0f) {
      meteorFlameIntensity =
          0.52f + std::clamp(m_impactTimer / 0.58f, 0.0f, 1.0f) * 0.58f;
    }
    const XMFLOAT3 flamePosition =
        CurvePoint(m_attackCenter.x, 0.12f, m_attackCenter.z);
    m_meteorFlameEmitter->SetPosition(XMVectorSet(
        flamePosition.x, flamePosition.y, flamePosition.z, 0.0f));
    m_meteorFlameEmitter->SetRadius(m_attackRadius * 0.92f);
    m_meteorFlameEmitter->SetIntensity(meteorFlameIntensity);
    m_meteorFlameEmitter->Emmit(meteorFlameIntensity > 0.001f);
    m_meteorFlameEmitter->Update(static_cast<double>(dt));
  }
  if (m_lineRiftEmitter) {
    float lineRiftIntensity = 0.0f;
    const bool lineVisible =
        m_attack == AttackType::LaserLine && !m_phoneOpen && !m_failed &&
        !m_cleared;
    if (lineVisible && m_phase == AttackPhase::Telegraph) {
      const float charge = 1.0f -
          std::clamp(m_phaseTimer / TelegraphDuration(), 0.0f, 1.0f);
      lineRiftIntensity = 0.18f + charge * 0.76f;
    } else if (lineVisible && m_impactTimer > 0.0f) {
      lineRiftIntensity =
          0.72f + std::clamp(m_impactTimer / 0.58f, 0.0f, 1.0f) * 0.68f;
    }
    const float lineCenterZ =
        m_laserVertical ? (kRunnerMinZ + kRunnerMaxZ) * 0.5f
                        : m_attackCenter.z;
    const XMFLOAT3 linePosition = CurvePoint(0.0f, 0.08f, lineCenterZ);
    m_lineRiftEmitter->SetPosition(XMVectorSet(
        linePosition.x, linePosition.y, linePosition.z, 0.0f));
    m_lineRiftEmitter->SetLine(
        m_laserVertical,
        m_laserVertical ? (kRunnerMaxZ - kRunnerMinZ) * 0.5f + 0.6f
                        : kLaneHalfWidth + 0.8f,
        m_laserHalfWidth * 0.72f);
    m_lineRiftEmitter->SetIntensity(lineRiftIntensity);
    m_lineRiftEmitter->Emmit(lineRiftIntensity > 0.001f);
    m_lineRiftEmitter->Update(static_cast<double>(dt));
  }
  if (m_bossSmokeEmitter)
    m_bossSmokeEmitter->Update(static_cast<double>(dt));
  for (auto &mistEmitter : m_riverMistEmitters) {
    if (mistEmitter)
      mistEmitter->Update(static_cast<double>(dt));
  }
  for (auto &fogEmitter : m_sideFogEmitters) {
    if (fogEmitter)
      fogEmitter->Update(static_cast<double>(dt));
  }
  // 水面の可読性を優先し、charge 完了時も river electric は表示しない。
  const bool readyRiverElectricActive = false;
  for (auto &electricEmitter : m_readyRiverElectricEmitters) {
    if (!electricEmitter)
      continue;
    electricEmitter->SetIntensity(m_readyRiverElectricIntensity);
    electricEmitter->Emmit(readyRiverElectricActive);
    if (readyRiverElectricActive && !m_readyRiverElectricWasActive)
      electricEmitter->Prime();
    electricEmitter->Update(static_cast<double>(dt));
  }
  m_readyRiverElectricWasActive = readyRiverElectricActive;
  if (IsPhoneOverlayActive())
    return;

  if (m_failed || m_cleared)
    return;

  const XMFLOAT3 playerPos = player.Position();
  if (!m_debugNoClip && (std::abs(playerPos.x) > kArenaHalfExtent ||
                         std::abs(playerPos.z) > kArenaHalfExtent)) {
    m_playerHp = 0;
    m_failed = true;
    m_lastHitPosition = playerPos;
    m_lastHitPosition.y = 0.04f;
    m_hitFlashTimer = 0.65f;
    return;
  }

  m_hitFlashTimer = std::max(0.0f, m_hitFlashTimer - dt);
  m_impactTimer = std::max(0.0f, m_impactTimer - dt);
  m_counterVfxTimer = std::max(0.0f, m_counterVfxTimer - dt);
  m_bossHitShakeTimer = std::max(0.0f, m_bossHitShakeTimer - dt);
  m_phaseShiftVfxTimer = std::max(0.0f, m_phaseShiftVfxTimer - dt);
  m_mirrorPickupVfxTimer = std::max(0.0f, m_mirrorPickupVfxTimer - dt);
  m_battleTimer += dt;
  UpdateMirrorCharges(player);

  m_phaseTimer -= dt;
  if (m_phase == AttackPhase::Telegraph && m_phaseTimer <= 0.0f) {
    m_phase = AttackPhase::Resolve;
    m_phaseTimer = 0.24f;
    m_resolved = false;
  }

  if (m_phase == AttackPhase::Resolve && !m_resolved) {
    ResolveAttack(player);
    m_resolved = true;
  }

  if (m_phase == AttackPhase::Resolve && m_phaseTimer <= 0.0f) {
    m_phase = AttackPhase::Recovery;
    m_phaseTimer = m_bossHp <= 2 ? 0.52f : 0.76f;
  }

  if (m_phase == AttackPhase::Recovery && m_phaseTimer <= 0.0f) {
    StartNextAttack();
  }
}

void BossArenaScene::BuildFrame(FrameData &frame) const {
  if (!m_ready)
    return;

  AppendWorldPolish(frame);
  AppendMirrorCharges(frame);
  if (m_mirrorPickupVfxTimer > 0.0f) {
    const float pickupT = std::clamp(m_mirrorPickupVfxTimer / 0.72f, 0.0f,
                                     1.0f);
    const float pickupAge = 1.0f - pickupT;
    GPUPointLight pickupLight{};
    pickupLight.position = CurvePoint(m_lastMirrorPickupPosition.x, 1.05f,
                                      m_lastMirrorPickupPosition.z);
    pickupLight.range = 5.4f + pickupAge * 2.5f;
    pickupLight.color = {0.34f, 1.0f, 0.82f};
    pickupLight.intensity = 5.2f * pickupT;
    frame.pointLights.push_back(pickupLight);
  }
  if (m_memoryFailTimer > 0.0f && !m_cleared) {
    const float failT = std::clamp(m_memoryFailTimer / 0.85f, 0.0f, 1.0f);
    const float failAge = 1.0f - failT;
    const float failZ = (kRunnerMinZ + kRunnerMaxZ) * 0.5f;
    const XMFLOAT3 failCenter = CurvePoint(0.0f, 0.18f, failZ);
    PushThickCircle(frame, failCenter, 1.2f + failAge * 3.2f,
                    XMFLOAT4{1.0f, 0.10f, 0.04f, 0.62f * failT}, 3);
    PushThickCircle(frame, failCenter, 2.1f + failAge * 4.0f,
                    XMFLOAT4{1.0f, 0.32f, 0.08f, 0.28f * failT}, 2);
    frame.transparentItems.push_back(
        {m_laserRiftMeshId,
         CurveWorld(4.8f + failAge * 2.6f, 1.0f, 4.8f + failAge * 2.6f,
                    0.0f, 0.135f, failZ, frame.gameTime * 0.34f)});
    for (int i = 0; i < 8; ++i) {
      const float a = static_cast<float>(i) / 8.0f * XM_2PI +
                      std::sin(frame.gameTime * 2.0f) * 0.18f;
      PushCurvedRibbon(frame, m_laserRiftMeshId, 0.0f, 0.22f, failZ,
                       std::cos(a) * (1.8f + failAge * 2.6f), 0.28f,
                       failZ + std::sin(a) * (1.8f + failAge * 2.6f),
                       0.08f + failT * 0.08f, 4);
    }
    GPUPointLight failLight{};
    failLight.position = CurvePoint(0.0f, 1.4f, failZ);
    failLight.range = 6.0f + failAge * 4.0f;
    failLight.color = {1.0f, 0.10f, 0.035f};
    failLight.intensity = 4.8f * failT;
    frame.pointLights.push_back(failLight);
  }
  if (m_phaseShiftVfxTimer > 0.0f && !m_cleared) {
    const float phaseT = std::clamp(m_phaseShiftVfxTimer / 1.25f, 0.0f,
                                    1.0f);
    const float phaseAge = 1.0f - phaseT;
    const XMFLOAT3 center = CurvePoint(0.0f, 0.22f, kArenaHalfExtent - 3.5f);
    constexpr int kPhaseShockRings = 5;
    for (int i = 0; i < kPhaseShockRings; ++i) {
      const float fi = static_cast<float>(i);
      PushThickCircle(frame, center, 2.0f + phaseAge * 9.0f + fi * 0.72f,
                      XMFLOAT4{1.0f, 0.10f + fi * 0.05f, 0.04f,
                               (0.62f - fi * 0.08f) * phaseT},
                      3);
    }
    for (int i = 0; i < 12; ++i) {
      const float fi = static_cast<float>(i);
      const float a = fi / 12.0f * XM_2PI + frame.gameTime * 0.85f;
      const float inner = 1.4f + phaseAge * 2.0f;
      const float outer = 6.8f + phaseAge * 4.2f;
      PushCurvedRibbon(frame, m_laserRiftMeshId, std::cos(a) * inner, 0.28f,
                       kArenaHalfExtent - 3.5f + std::sin(a) * inner,
                       std::cos(a) * outer, 0.46f,
                       kArenaHalfExtent - 3.5f + std::sin(a) * outer,
                       0.14f + phaseT * 0.10f, 3);
    }
    GPUPointLight phaseBurst{};
    phaseBurst.position = CurvePoint(0.0f, 3.0f, kArenaHalfExtent - 3.5f);
    phaseBurst.range = 18.0f;
    phaseBurst.color = {1.0f, 0.12f, 0.055f};
    phaseBurst.intensity = 14.0f * phaseT;
    frame.pointLights.push_back(phaseBurst);
  }
  if (m_aoeSparkBurst && m_aoeSparkTimer > 0.0f &&
      !m_aoeSparkBurst->IsFinished())
    frame.emitters.push_back(m_aoeSparkBurst.get());
  if (m_meteorFlameEmitter)
    frame.emitters.push_back(m_meteorFlameEmitter.get());
  if (m_lineRiftEmitter)
    frame.emitters.push_back(m_lineRiftEmitter.get());
  if (m_bossSmokeEmitter && !m_cleared)
    frame.emitters.push_back(m_bossSmokeEmitter.get());
  for (const auto &mistEmitter : m_riverMistEmitters) {
    if (mistEmitter)
      frame.emitters.push_back(mistEmitter.get());
  }
  for (const auto &fogEmitter : m_sideFogEmitters) {
    if (fogEmitter)
      frame.emitters.push_back(fogEmitter.get());
  }
  for (const auto &electricEmitter : m_readyRiverElectricEmitters) {
    if (electricEmitter)
      frame.emitters.push_back(electricEmitter.get());
  }
  if (m_mirrorPickupBurst && m_mirrorPickupVfxTimer > 0.0f &&
      !m_mirrorPickupBurst->IsFinished())
    frame.emitters.push_back(m_mirrorPickupBurst.get());
  if (m_counterSparkBurst && m_counterVfxTimer > 0.0f &&
      !m_counterSparkBurst->IsFinished()) {
    frame.emitters.push_back(m_counterSparkBurst.get());
  }
  if (m_counterPetalTimer > 0.0f) {
    for (const auto &petalEmitter : m_counterPetalEmitters) {
      if (petalEmitter && !petalEmitter->IsFinished())
        frame.emitters.push_back(petalEmitter.get());
    }
  }
  if (m_playerHitParticleTimer > 0.0f && m_playerHitBurst &&
      !m_playerHitBurst->IsFinished()) {
    frame.emitters.push_back(m_playerHitBurst.get());
  }

  const float bossHitT = std::clamp(m_bossHitShakeTimer / 0.62f, 0.0f, 1.0f);
  const float bossShake =
      bossHitT * 0.72f * std::sin(frame.gameTime * 116.0f);
  const float bossSquash = 1.0f + bossHitT * 0.16f;
  const XMMATRIX bossWorld =
      CurveWorld(2.2f * bossSquash, 3.8f * (1.0f - bossHitT * 0.04f),
                 2.2f * bossSquash, bossShake, 1.9f,
                 kArenaHalfExtent - 3.5f);
  if (!m_cleared)
    frame.opaqueItems.push_back({m_bossMeshId, bossWorld});

  if (!m_cleared) {
    const int inkMoteCount = m_bossHp <= 2 ? 16 : 10;
    for (int i = 0; i < inkMoteCount; ++i) {
      const float fi = static_cast<float>(i);
      const float a = fi / static_cast<float>(inkMoteCount) * XM_2PI +
                      frame.gameTime * (0.62f + 0.03f * fi);
      const float radius = 1.15f + static_cast<float>(i % 4) * 0.26f;
      const float x = bossShake * 0.25f + std::cos(a) * radius;
      const float z = kArenaHalfExtent - 3.5f + std::sin(a) * radius;
      const float y = 1.35f + static_cast<float>((i * 5) % 7) * 0.28f +
                      std::sin(frame.gameTime * 1.9f + fi) * 0.16f;
      const float motePulse = SmoothPulse(frame.gameTime + fi * 0.21f, 4.0f,
                                          0.35f);
      frame.transparentItems.push_back(
          {m_bossDamageShardMeshId,
           CurveWorld(0.055f + motePulse * 0.055f,
                      0.18f + motePulse * 0.12f,
                      0.050f + motePulse * 0.045f, x, y, z, a)});
    }
  }

  const int bossDamageLevel = std::clamp(5 - m_bossHp, 0, 5);
  if (bossDamageLevel > 0 && !m_cleared) {
    const float damageRate = static_cast<float>(bossDamageLevel) / 5.0f;
    const float scarPulse = SmoothPulse(frame.gameTime, 5.2f, 0.26f);
    const XMFLOAT3 sealCenter = CurvePoint(0.0f, 0.135f,
                                           kArenaHalfExtent - 3.5f);
    for (int i = 0; i < bossDamageLevel; ++i) {
      const float fi = static_cast<float>(i);
      const float radius = 2.25f + fi * 0.62f + scarPulse * 0.18f;
      PushCircle(frame, sealCenter, radius,
                 XMFLOAT4{1.0f, 0.12f, 0.06f,
                          (0.22f + damageRate * 0.30f) *
                              (1.0f - fi * 0.08f)});
      const float a = fi * 1.37f + frame.gameTime * (0.35f + fi * 0.04f);
      PushCurvedRibbon(frame, m_laserRiftMeshId, std::cos(a) * radius, 0.18f,
                       kArenaHalfExtent - 3.5f + std::sin(a) * radius,
                       std::cos(a + 0.92f) * (radius + 0.9f), 0.28f,
                       kArenaHalfExtent - 3.5f +
                           std::sin(a + 0.92f) * (radius + 0.9f),
                       0.08f + damageRate * 0.09f, 3);
    }
    constexpr int kScarShardCount = 8;
    for (int i = 0; i < kScarShardCount; ++i) {
      if (i >= bossDamageLevel * 2)
        break;
      const float fi = static_cast<float>(i);
      const float a = fi / static_cast<float>(kScarShardCount) * XM_2PI +
                      frame.gameTime * 0.55f;
      const float radius = 1.05f + damageRate * 1.45f;
      const float x = std::cos(a) * radius;
      const float z = kArenaHalfExtent - 3.5f + std::sin(a) * radius;
      frame.transparentItems.push_back(
          {m_bossDamageShardMeshId,
           CurveWorld(0.10f + damageRate * 0.08f,
                      0.32f + scarPulse * 0.18f,
                      0.08f + damageRate * 0.06f, x,
                      1.25f + std::sin(a * 1.7f) * 0.45f, z, a)});
    }
    GPUPointLight scarLight{};
    scarLight.position = CurvePoint(0.0f, 2.8f, kArenaHalfExtent - 3.5f);
    scarLight.range = 7.5f + damageRate * 6.0f;
    scarLight.color = {1.0f, 0.10f, 0.045f};
    scarLight.intensity = (1.5f + scarPulse * 2.4f) * damageRate;
    frame.pointLights.push_back(scarLight);
  }

  if (m_bossHp <= 2 && !m_cleared) {
    const float phasePulse = SmoothPulse(frame.gameTime, 8.4f, 0.30f);
    const XMFLOAT3 phaseMoonPos =
        CurvePoint(0.0f, 7.2f, kArenaHalfExtent + 1.62f);
    frame.transparentItems.push_back(
        {m_bossSealMeshId,
         XMMatrixScaling(8.4f + phasePulse * 1.4f, 1.0f,
                         8.4f + phasePulse * 1.4f) *
             XMMatrixRotationX(-XM_PIDIV2) *
             XMMatrixTranslation(phaseMoonPos.x, phaseMoonPos.y,
                                 phaseMoonPos.z)});
    for (int i = 0; i < 5; ++i) {
      const float fi = static_cast<float>(i);
      const float x = (fi - 2.0f) * 2.1f;
      frame.transparentItems.push_back(
          {m_laserRiftMeshId,
           VerticalVeilWorld(0.26f + phasePulse * 0.16f,
                             6.6f + phasePulse * 2.0f, x,
                             4.5f + phasePulse * 0.7f,
                             kArenaHalfExtent - 2.9f, x * 0.055f)});
    }
    frame.transparentItems.push_back(
        {m_bossSealMeshId,
         CurveWorld(7.2f + phasePulse * 1.2f, 1.0f,
                    7.2f + phasePulse * 1.2f, 0.0f, 0.082f,
                    kArenaHalfExtent - 3.5f)});
    PushThickCircle(frame, CurvePoint(0.0f, 0.18f, kArenaHalfExtent - 3.5f),
                    4.8f + phasePulse * 1.0f,
                    XMFLOAT4{1.0f, 0.20f, 0.06f, 0.58f}, 3);

    GPUPointLight phaseLight{};
    phaseLight.position = CurvePoint(0.0f, 3.2f, kArenaHalfExtent - 3.5f);
    phaseLight.range = 13.0f;
    phaseLight.color = {1.0f, 0.18f, 0.06f};
    phaseLight.intensity = 3.2f + phasePulse * 4.0f;
    frame.pointLights.push_back(phaseLight);
    constexpr int kPhaseRifts = 8;
    for (int i = 0; i < kPhaseRifts; ++i) {
      const float fi = static_cast<float>(i);
      const float side = (i % 2 == 0) ? -1.0f : 1.0f;
      const float z = std::lerp(kRunnerMinZ, kArenaHalfExtent - 4.0f,
                                (fi + 0.5f) / kPhaseRifts);
      PushCurvedRibbon(frame, m_laserRiftMeshId, side * 4.9f, 0.20f,
                       z + std::sin(frame.gameTime * 2.1f + fi) * 0.35f,
                       side * 8.4f, 0.34f, z - 1.4f,
                       0.12f + phasePulse * 0.12f, 5);
    }
  }

  if (bossHitT > 0.0f) {
    constexpr int kDamageShardCount = 14;
    for (int i = 0; i < kDamageShardCount; ++i) {
      const float fi = static_cast<float>(i);
      const float angle = fi / static_cast<float>(kDamageShardCount) * XM_2PI +
                          frame.gameTime * 1.4f;
      const float out = (1.0f - bossHitT) * (1.3f + 0.18f * fi);
      const float x = std::cos(angle) * (0.7f + out);
      const float z = kArenaHalfExtent - 3.5f + std::sin(angle) * (0.7f + out);
      const float y = 1.4f + std::sin(fi * 1.7f) * 0.9f +
                      (1.0f - bossHitT) * 1.8f;
      frame.transparentItems.push_back(
          {m_bossDamageShardMeshId,
           CurveWorld(0.12f + 0.10f * bossHitT, 0.54f * bossHitT,
                      0.12f + 0.06f * bossHitT, x, y, z, angle)});
    }
    PushThickCircle(frame, CurvePoint(0.0f, 1.45f, kArenaHalfExtent - 3.5f),
                    2.2f + (1.0f - bossHitT) * 3.8f,
                    XMFLOAT4{1.0f, 0.18f, 0.08f, 0.72f * bossHitT}, 4);
  }

  const XMFLOAT4 borderColor{0.32f, 0.82f, 0.74f, 0.36f};
  PushCurvedLine(frame, -4.55f, 0.12f, -kArenaHalfExtent, -4.55f, 0.12f,
                 kArenaHalfExtent, borderColor, 24);
  PushCurvedLine(frame, 4.55f, 0.12f, -kArenaHalfExtent, 4.55f, 0.12f,
                 kArenaHalfExtent, borderColor, 24);
  const float lanePulse = SmoothPulse(frame.gameTime, 4.4f, 0.25f);
  PushCurvedRibbon(frame, m_counterRibbonMeshId, -kLaneHalfWidth, 0.145f,
                   kRunnerMinZ, -kLaneHalfWidth, 0.145f, kRunnerMaxZ,
                   0.12f + lanePulse * 0.08f, 12);
  PushCurvedRibbon(frame, m_counterRibbonMeshId, kLaneHalfWidth, 0.145f,
                   kRunnerMinZ, kLaneHalfWidth, 0.145f, kRunnerMaxZ,
                   0.12f + lanePulse * 0.08f, 12);

  if (IsPhoneOverlayActive() && !m_failed && !m_cleared) {
    const float phoneAura = std::clamp(m_phoneSlide, 0.0f, 1.0f);
    const float phonePulse = SmoothPulse(frame.gameTime, 4.8f, 0.25f);
    const float centerZ = (kRunnerMinZ + kRunnerMaxZ) * 0.5f;
    const XMFLOAT3 auraCenter = CurvePoint(0.0f, 0.24f, centerZ);
    PushThickCircle(frame, auraCenter, 2.4f + phonePulse * 0.35f,
                    XMFLOAT4{0.34f, 1.0f, 0.88f, 0.46f * phoneAura}, 3);
    PushThickCircle(frame, auraCenter, 4.2f + phonePulse * 0.55f,
                    XMFLOAT4{0.18f, 0.72f, 1.0f, 0.30f * phoneAura}, 3);
    frame.transparentItems.push_back(
        {m_spiritVeilMeshId,
         CurveWorld(6.2f + phonePulse * 0.8f, 1.0f,
                    6.2f + phonePulse * 0.8f, 0.0f, 1.65f, centerZ)});
    for (int i = 0; i < 6; ++i) {
      const float fi = static_cast<float>(i);
      const float a = fi / 6.0f * XM_2PI + frame.gameTime * 0.75f;
      PushCurvedRibbon(frame, m_counterRibbonMeshId, std::cos(a) * 3.8f,
                       0.38f, centerZ + std::sin(a) * 1.8f,
                       std::cos(a + 0.8f) * 1.4f, 1.35f,
                       centerZ + std::sin(a + 0.8f) * 0.8f,
                       0.16f * phoneAura, 5);
    }
  }

  const bool drawTelegraphSurface =
      m_phase != AttackPhase::Recovery || m_phaseTimer > 0.42f;
  if (drawTelegraphSurface) {
    constexpr float telegraphY = 0.055f;
    const float telegraphRemain =
        std::clamp(m_phaseTimer / TelegraphDuration(), 0.0f, 1.0f);
    const float dangerPulse = SmoothPulse(frame.gameTime, 13.5f, 0.35f);
    const float panicPulse = 1.0f + (1.0f - telegraphRemain) * 0.16f;
    const float pulse =
        (m_phase == AttackPhase::Telegraph)
            ? panicPulse + 0.055f * dangerPulse
            : 1.0f + 0.10f * std::clamp((m_phaseTimer - 0.42f) / 0.23f,
                                      0.0f, 1.0f);

    if (m_attack == AttackType::MeteorAoE) {
      const XMMATRIX world =
          CurveWorld(m_attackRadius * pulse, 1.0f, m_attackRadius * pulse,
                     m_attackCenter.x, telegraphY, m_attackCenter.z);
      frame.transparentItems.push_back({m_aoeTelegraphMeshId, world});
      const float charge = 1.0f - telegraphRemain;
      GPUPointLight meteorLight{};
      meteorLight.position =
          CurvePoint(m_attackCenter.x, 1.2f + charge * 0.8f, m_attackCenter.z);
      meteorLight.range = 5.5f + charge * 3.5f;
      meteorLight.color = {1.0f, 0.26f, 0.08f};
      meteorLight.intensity = 1.4f + charge * 3.6f;
      frame.pointLights.push_back(meteorLight);
    } else if (m_attack == AttackType::LaserLine) {
      const float width = m_laserHalfWidth * 2.0f;
      const float runnerLength = kRunnerMaxZ - kRunnerMinZ;
      const float runnerCenterZ = (kRunnerMinZ + kRunnerMaxZ) * 0.5f;
      const XMMATRIX world =
          m_laserVertical
              ? CurveWorld(width * pulse, 1.0f, runnerLength, 0.0f,
                           telegraphY, runnerCenterZ)
              : CurveWorld(kLaneHalfWidth * 2.0f + 1.4f, 1.0f,
                           width * pulse, 0.0f, telegraphY,
                           m_attackCenter.z);
      frame.transparentItems.push_back({m_laserTelegraphMeshId, world});
      const float charge = 1.0f - telegraphRemain;
      GPUPointLight riftLight{};
      riftLight.position = CurvePoint(0.0f, 1.0f, m_attackCenter.z);
      riftLight.range = 5.5f + charge * 3.5f;
      riftLight.color = {1.0f, 0.08f, 0.16f};
      riftLight.intensity = 1.2f + charge * 3.4f;
      frame.pointLights.push_back(riftLight);
    } else if (m_attack == AttackType::SanctuarySeal &&
               m_phase != AttackPhase::Recovery) {
      const float charge = 1.0f - telegraphRemain;
      float radialScale = m_attackRadius;
      float domeHeight = m_sanctuaryIntroActive ? 2.30f : 2.05f;

      if (m_phase == AttackPhase::Telegraph) {
        float rise = std::clamp(charge / 0.16f, 0.0f, 1.0f);
        rise = rise * rise * (3.0f - 2.0f * rise);
        domeHeight *= std::max(0.035f, rise);
        radialScale *=
            1.0f + 0.012f * std::sin(frame.gameTime * 4.2f);
      } else {
        const float resolveAge =
            1.0f - std::clamp(m_phaseTimer / 0.24f, 0.0f, 1.0f);
        const float resolvePulse = std::sin(resolveAge * XM_PI);
        radialScale *= 1.0f + resolvePulse * 0.04f;
        domeHeight *= 1.0f + resolvePulse * 0.06f;
      }

      frame.transparentItems.push_back(
          {m_sanctuaryDomeMeshId,
           CurveWorld(radialScale, domeHeight, radialScale,
                      m_attackCenter.x, 0.03f, m_attackCenter.z)});

      GPUPointLight sanctuaryLight{};
      sanctuaryLight.position =
          CurvePoint(m_attackCenter.x, domeHeight * 0.48f,
                     m_attackCenter.z);
      sanctuaryLight.range = m_attackRadius + 3.4f;
      sanctuaryLight.color = {0.28f, 1.0f, 0.86f};
      sanctuaryLight.intensity = 0.70f + charge * 2.10f;
      frame.pointLights.push_back(sanctuaryLight);
    }
  }

  AppendTelegraphLines(frame);

  if (m_impactTimer > 0.0f) {
    const float impactDuration = m_impactMaxRadius > 7.5f ? 0.78f : 0.58f;
    const float impactLife = std::clamp(m_impactTimer / impactDuration, 0.0f, 1.0f);
    const float impactT = 1.0f - impactLife;
    const float radius = std::lerp(0.25f, m_impactMaxRadius, impactT);
    GPUPointLight impactLight{};
    impactLight.position = CurvePoint(m_impactPosition.x, 1.0f,
                                      m_impactPosition.z);
    if (m_attack == AttackType::SanctuarySeal) {
      impactLight.range = radius + 5.0f;
      impactLight.color = {0.34f, 1.0f, 0.86f};
      impactLight.intensity = 6.0f * impactLife;
    } else {
      impactLight.range =
          radius + (m_attack == AttackType::MeteorAoE ? 4.5f : 7.0f);
      impactLight.color = {1.0f, 0.38f, 0.08f};
      impactLight.intensity =
          (m_attack == AttackType::MeteorAoE ? 5.0f : 10.0f) * impactLife;
    }
    frame.pointLights.push_back(impactLight);
  }
  if (m_attack == AttackType::LaserLine && m_impactTimer > 0.0f) {
    const float zapT = std::clamp(m_impactTimer / 0.58f, 0.0f, 1.0f);
    GPUPointLight zapLight{};
    zapLight.position = CurvePoint(0.0f, 1.35f, m_attackCenter.z);
    zapLight.range = 9.0f;
    zapLight.color = {1.0f, 0.12f, 0.22f};
    zapLight.intensity = 5.0f * zapT;
    frame.pointLights.push_back(zapLight);
  }
  if (m_cleared || m_failed) {
    const float endPulse = SmoothPulse(frame.gameTime, m_cleared ? 3.2f : 6.2f,
                                       0.25f);
    const uint32_t endMesh = m_cleared ? m_spiritVeilMeshId : m_laserRiftMeshId;
    const XMFLOAT4 endColor = m_cleared
                                  ? XMFLOAT4{0.34f, 1.0f, 0.86f, 0.74f}
                                  : XMFLOAT4{1.0f, 0.08f, 0.06f, 0.70f};
    constexpr int kEndRings = 5;
    for (int i = 0; i < kEndRings; ++i) {
      PushThickCircle(frame, CurvePoint(0.0f, 0.20f + i * 0.06f,
                                        kArenaHalfExtent - 3.5f),
                      2.0f + i * 1.15f + endPulse * 0.55f, endColor, 3);
    }
    for (int i = 0; i < 8; ++i) {
      const float z = std::lerp(kRunnerMinZ, kArenaHalfExtent - 3.8f,
                                (static_cast<float>(i) + 0.5f) / 8.0f);
      frame.transparentItems.push_back(
          {endMesh, CurveWorld(8.0f + endPulse * 2.4f, 1.0f, 1.9f, 0.0f,
                               0.20f, z)});
    }
    if (m_cleared) {
      constexpr int kClearLights = 12;
      for (int i = 0; i < kClearLights; ++i) {
        const float fi = static_cast<float>(i);
        const float a = fi / static_cast<float>(kClearLights) * XM_2PI +
                        frame.gameTime * 0.48f;
        const float radius = 1.5f + static_cast<float>(i % 4) * 0.58f;
        const float x = std::cos(a) * radius;
        const float z = kArenaHalfExtent - 3.5f + std::sin(a) * radius;
        const float y =
            1.0f + static_cast<float>((i * 3) % 7) * 0.34f + endPulse * 0.42f;
        frame.transparentItems.push_back(
            {m_mirrorChargeMeshId,
             CurveWorld(0.20f + endPulse * 0.14f,
                        0.20f + endPulse * 0.14f,
                        0.20f + endPulse * 0.14f, x, y, z)});
        frame.transparentItems.push_back(
            {m_lanternHaloMeshId,
             CurveWorld(0.88f + endPulse * 0.42f,
                        0.88f + endPulse * 0.42f,
                        0.88f + endPulse * 0.42f, x, y, z)});
      }
      for (int i = 0; i < 5; ++i) {
        const float fi = static_cast<float>(i);
        const float x = (fi - 2.0f) * 1.55f;
        frame.transparentItems.push_back(
            {m_moonRayMeshId,
             VerticalVeilWorld(0.70f + endPulse * 0.36f,
                               5.2f + endPulse * 2.4f, x,
                               3.5f + endPulse * 0.8f,
                               kArenaHalfExtent - 3.2f, x * 0.05f)});
      }
    }
  }

  if (m_counterVfxTimer > 0.0f) {
    const bool climaxVfx = !m_techShowcaseOverride || m_showcaseClimaxVfx;
    const float counterDuration = climaxVfx ? 0.92f : 0.68f;
    const float counterT =
        std::clamp(m_counterVfxTimer / counterDuration, 0.0f, 1.0f);
    const float counterAge = 1.0f - counterT;
    const XMFLOAT3 bossBase = CurvePoint(0.0f, 0.12f,
                                         kArenaHalfExtent - 3.5f);
    const float ringA = std::lerp(1.2f, 5.8f, 1.0f - counterT);
    const XMFLOAT4 mirrorColor{0.36f, 1.0f, 0.86f, counterT * 0.46f};
    PushThickCircle(frame, bossBase, ringA, mirrorColor, 3);

    // 全河面の発光は攻撃予兆を隠すため無効化し、成功演出は花びらへ移す。
    constexpr bool kEnableLegacyRiverSurge = false;
    if (climaxVfx && kEnableLegacyRiverSurge) {
      const float surgeAlpha = std::sin(std::clamp(counterAge * XM_PI, 0.0f,
                                                   XM_PI));
      const XMFLOAT3 moonPos = CurvePoint(0.0f, 7.2f, kArenaHalfExtent + 1.6f);
      frame.transparentItems.push_back(
          {m_moonDiscMeshId,
           XMMatrixScaling(9.4f + surgeAlpha * 2.6f, 1.0f,
                           9.4f + surgeAlpha * 2.6f) *
               XMMatrixRotationX(-XM_PIDIV2) *
               XMMatrixTranslation(moonPos.x, moonPos.y, moonPos.z)});
      constexpr int kCounterMoonRays = 7;
      for (int i = 0; i < kCounterMoonRays; ++i) {
        const float fi = static_cast<float>(i);
        const float u = (fi - 3.0f) / 3.0f;
        frame.transparentItems.push_back(
            {m_moonRayMeshId,
             VerticalVeilWorld(0.72f + surgeAlpha * 0.72f,
                               6.4f + surgeAlpha * 3.8f, u * 5.1f,
                               4.0f + surgeAlpha * 0.9f,
                               kArenaHalfExtent - 3.0f - std::abs(u) * 1.6f,
                               u * 0.24f)});
      }
      constexpr int kRiverBlastSegments = 10;
      const float riverBlastLength =
          (kArenaHalfExtent * 2.0f) / static_cast<float>(kRiverBlastSegments);
      for (int i = 0; i < kRiverBlastSegments; ++i) {
        const float z = -kArenaHalfExtent +
                        (static_cast<float>(i) + 0.5f) * riverBlastLength;
        const float travel = std::clamp(counterAge * 1.45f -
                                            static_cast<float>(i) * 0.075f,
                                        0.0f, 1.0f);
        const float width = 5.8f + surgeAlpha * 4.2f + travel * 2.4f;
        frame.transparentItems.push_back(
            {m_counterRibbonMeshId,
             CurveWorld(width, 1.0f, riverBlastLength + 0.36f, 0.0f,
                        0.18f + travel * 0.09f, z)});
      }

      constexpr int kSurgeLines = 9;
      for (int i = 0; i < kSurgeLines; ++i) {
        const float fi = static_cast<float>(i);
        const float laneT = (fi / static_cast<float>(kSurgeLines - 1)) * 2.0f - 1.0f;
        const float wave = std::sin(frame.gameTime * 12.0f + fi * 1.7f);
        const float startX = laneT * 4.1f + wave * 0.18f;
        const float endX = laneT * 0.72f;
        PushCurvedRibbon(frame, m_counterRibbonMeshId, startX, 0.24f,
                         kRunnerMinZ + fi * 0.18f, endX,
                         0.42f + surgeAlpha * 0.25f,
                         kArenaHalfExtent - 3.5f,
                         0.14f + std::max(0.0f, surgeAlpha) * 0.24f, 18);
      }

      const float columnHeight = 4.6f + counterAge * 3.4f;
      for (int i = 0; i < 4; ++i) {
        const float side = (i % 2 == 0) ? -1.0f : 1.0f;
        const float x = side * (0.82f + static_cast<float>(i / 2) * 1.20f);
        frame.transparentItems.push_back(
            {m_pathGlowMeshId,
             CurveWorld(0.42f + surgeAlpha * 0.36f, 1.0f, columnHeight, x,
                        1.0f + columnHeight * 0.5f,
                        kArenaHalfExtent - 3.7f,
                        side * (0.18f + counterAge * 0.25f))});
      }
      for (int i = 0; i < 6; ++i) {
        const float fi = static_cast<float>(i);
        const float angle = fi / 6.0f * XM_2PI + frame.gameTime * 1.8f;
        frame.transparentItems.push_back(
            {m_spiritVeilMeshId,
             CurveWorld(0.80f + surgeAlpha * 0.46f, 1.0f,
                        5.4f + surgeAlpha * 2.1f, std::cos(angle) * 1.15f,
                        2.6f + surgeAlpha * 1.3f,
                        kArenaHalfExtent - 3.5f + std::sin(angle) * 1.15f,
                        angle)});
      }

      for (int i = 0; i < 3; ++i) {
        const float z = std::lerp(kRunnerMinZ, kRunnerMaxZ,
                                  (static_cast<float>(i) + 0.5f) / 3.0f);
        const float waveWidth = 8.2f + surgeAlpha * 1.8f;
        frame.transparentItems.push_back(
            {m_pathGlowMeshId,
             CurveWorld(waveWidth, 1.0f, 1.25f + counterAge * 1.5f, 0.0f,
                        0.16f, z)});
      }

      constexpr int kFlameBursts = 14;
      for (int i = 0; i < kFlameBursts; ++i) {
        const float fi = static_cast<float>(i);
        const float a = (fi / static_cast<float>(kFlameBursts)) * XM_2PI +
                        frame.gameTime * 1.4f;
        const float radius = 1.55f + static_cast<float>(i % 4) * 0.72f;
        const float x = std::cos(a) * radius;
        const float z = kArenaHalfExtent - 3.5f + std::sin(a) * radius;
        const float height = (1.4f + static_cast<float>(i % 3) * 0.42f) *
                             std::max(0.0f, surgeAlpha);
        const float width = 0.36f + static_cast<float>(i % 2) * 0.12f;
        const float yaw = a + XM_PIDIV2;
        const XMFLOAT3 p = CurvePoint(x, 0.22f, z);
        frame.transparentItems.push_back(
            {m_flameMeshId,
             XMMatrixScaling(width, 1.0f, height) *
                 XMMatrixRotationX(-XM_PIDIV2) * XMMatrixRotationY(yaw) *
                 XMMatrixRotationX(CurveAngle(z)) *
                 XMMatrixTranslation(p.x, p.y, p.z)});
      }
    }

    constexpr int kCounterRays = 8;
    for (int i = 0; i < kCounterRays; ++i) {
      const float a = (static_cast<float>(i) / kCounterRays) * XM_2PI +
                      frame.gameTime * 2.8f;
      const float inner = 1.2f + (1.0f - counterT) * 1.4f;
      const float outer = 4.2f + (1.0f - counterT) * 1.6f;
      PushCurvedRibbon(frame, m_counterRibbonMeshId, std::cos(a) * inner,
                       0.18f, kArenaHalfExtent - 3.5f + std::sin(a) * inner,
                       std::cos(a) * outer, 0.22f,
                       kArenaHalfExtent - 3.5f + std::sin(a) * outer,
                       0.025f + counterT * 0.045f, 2);
    }

    GPUPointLight counterLight{};
    counterLight.position = CurvePoint(0.0f, 4.0f,
                                       kArenaHalfExtent - 3.5f);
    counterLight.range = 8.0f;
    counterLight.color = {0.34f, 1.0f, 0.82f};
    counterLight.intensity = (climaxVfx ? 2.8f : 2.0f) * counterT;
    frame.pointLights.push_back(counterLight);
  }

  if (!m_cleared) {
    GPUPointLight bossLight{};
    bossLight.position = CurvePoint(0.0f, 4.5f, kArenaHalfExtent - 3.5f);
    bossLight.range = 16.0f;
    bossLight.color = {1.0f, 0.15f, 0.08f};
    bossLight.intensity = 5.2f + bossHitT * 4.0f;
    frame.pointLights.push_back(bossLight);
  }
}

XMFLOAT3 BossArenaScene::VisualPositionForGameplayPosition(
    const XMFLOAT3 &position) const {
  constexpr float playerVisualLift = 0.18f;
  return CurvePoint(position.x, position.y + playerVisualLift, position.z);
}

float BossArenaScene::CameraImpulseAmount() const {
  const float hitShake = std::clamp(m_hitFlashTimer / 0.54f, 0.0f, 1.0f) *
                         0.030f;
  const float bossShake =
      std::clamp(m_bossHitShakeTimer / 0.62f, 0.0f, 1.0f) * 0.040f;
  const float counterShake =
      std::clamp(m_counterVfxTimer / 0.92f, 0.0f, 1.0f) * 0.035f;
  const float pickupShake =
      std::clamp(m_mirrorPickupVfxTimer / 0.72f, 0.0f, 1.0f) * 0.018f;
  return std::max(std::max(hitShake, bossShake),
                  std::max(counterShake, pickupShake));
}

void BossArenaScene::ApplyTechShowcase(FrameData &frame) const {
  if (!m_techShowcaseOverride)
    return;

  frame.shadowsEnabled = m_showcaseShadows;
  frame.ssaoEnabled = m_showcaseSSAO;
  frame.ssrEnabled = m_showcaseSSR;
  frame.bloomEnabled = m_showcaseBloom;
  frame.fxaaEnabled = m_showcaseFXAA;
  frame.taaEnabled = m_showcaseTAA;
  frame.motionBlurEnabled = m_showcaseMotionBlur;
  frame.dofEnabled = m_showcaseDOF && frame.dofEnabled;
  frame.particlesEnabled = m_showcaseRain;

  if (!m_showcaseSSR)
    frame.ssrReflectionParams.x = 0.0f;
  if (!m_showcaseBloom)
    frame.bloomIntensity = 0.0f;
  if (!m_showcaseSSAO)
    frame.ssaoStrength = 0.0f;
}

void BossArenaScene::AppendWorldPolish(FrameData &frame) const {
  constexpr float pathEdgeWidth = 5.15f;
  constexpr float pathLength = kArenaHalfExtent * 2.0f;
  constexpr float kSides[2] = {-1.0f, 1.0f};
  const float time = frame.gameTime;
  const float scroll = std::fmod(time * 6.1f, pathLength);
  constexpr float loopMinZ = -kArenaHalfExtent;
  constexpr float loopMaxZ = kArenaHalfExtent;

  auto pushOpaque = [&frame](uint32_t meshId, const XMMATRIX &world) {
    if (meshId != UINT32_MAX)
      frame.opaqueItems.push_back({meshId, world});
  };
  auto pushTransparent = [&frame](uint32_t meshId, const XMMATRIX &world) {
    if (meshId != UINT32_MAX)
      frame.transparentItems.push_back({meshId, world});
  };
  auto pushMeshGroup = [&frame](const std::vector<uint32_t> &meshIds,
                                const XMMATRIX &world) {
    for (uint32_t meshId : meshIds) {
      if (meshId != UINT32_MAX)
        frame.opaqueItems.push_back({meshId, world});
    }
  };
  auto cubeWorld = [](float sx, float sy, float sz, float x, float y, float z,
                      float yaw = 0.0f) {
    return CurveWorld(sx, sy, sz, x, y, z, yaw);
  };
  auto addTorii = [&](float x, float z, float scale, float yaw) {
    if (!m_toriiGateMeshIds.empty()) {
      const float modelScale = scale * 3.65f;
      const float angle = CurveAngle(z);
      const XMFLOAT3 p = CurvePoint(x, 0.02f, z);
      pushMeshGroup(m_toriiGateMeshIds,
                    XMMatrixScaling(modelScale, modelScale, modelScale) *
                        XMMatrixRotationX(XM_PIDIV2) *
                        XMMatrixRotationY(yaw) *
                        XMMatrixRotationX(angle) *
                        XMMatrixTranslation(p.x, p.y, p.z));
      return;
    }

    const float postX = 1.35f * scale;
    const float postHeight = 3.7f * scale;
    const float postWidth = 0.34f * scale;
    pushOpaque(m_toriiWoodMeshId,
               cubeWorld(postWidth, postHeight, postWidth, x - postX,
                         postHeight * 0.5f, z, yaw));
    pushOpaque(m_toriiWoodMeshId,
               cubeWorld(postWidth, postHeight, postWidth, x + postX,
                         postHeight * 0.5f, z, yaw));
    pushOpaque(m_toriiWoodMeshId,
               cubeWorld(4.2f * scale, 0.36f * scale, 0.42f * scale, x,
                         postHeight + 0.10f * scale, z, yaw));
    pushOpaque(m_toriiWoodMeshId,
               cubeWorld(4.9f * scale, 0.28f * scale, 0.46f * scale, x,
                         postHeight + 0.58f * scale, z, yaw));
    pushOpaque(m_toriiWoodMeshId,
               cubeWorld(2.0f * scale, 0.22f * scale, 0.36f * scale, x,
                         postHeight - 0.54f * scale, z, yaw));
  };
  auto addProceduralLantern = [&](float x, float z, float scale) {
    pushOpaque(m_lanternPostMeshId,
               cubeWorld(0.18f * scale, 1.65f * scale, 0.18f * scale, x,
                         0.82f * scale, z));
    pushOpaque(m_lanternCapMeshId,
               cubeWorld(0.82f * scale, 0.18f * scale, 0.82f * scale, x,
                         1.72f * scale, z));
    pushOpaque(m_lanternCapMeshId,
               cubeWorld(0.58f * scale, 0.44f * scale, 0.58f * scale, x,
                         1.38f * scale, z));
  };

  constexpr int kSpiritVeilCount = 7;
  for (int i = 0; i < kSpiritVeilCount; ++i) {
    const float fi = static_cast<float>(i);
    const float z = LoopZ(-20.0f + fi * 6.2f, scroll * 0.33f, loopMinZ,
                          pathLength);
    const float sideWave = std::sin(time * 0.75f + fi * 1.3f);
    const float height = 4.6f + static_cast<float>(i % 3) * 0.55f;
    pushTransparent(m_spiritVeilMeshId,
                    cubeWorld(11.5f + sideWave * 1.2f, 1.0f, 3.6f,
                              sideWave * 2.2f, height, z,
                              sideWave * 0.16f));
  }
  for (float side : kSides) {
    pushTransparent(m_spiritVeilMeshId,
                    cubeWorld(7.4f, 1.0f, 11.5f, side * 8.8f, 3.2f,
                              kArenaHalfExtent - 3.2f, side * 0.10f));
  }
  {
    const float moonZ = kArenaHalfExtent + 1.6f;
    const XMFLOAT3 moonPos = CurvePoint(0.0f, 7.2f, moonZ);
    const float moonPulse = SmoothPulse(time, 1.8f, 0.45f);
    frame.transparentItems.push_back(
        {m_moonDiscMeshId,
         XMMatrixScaling(8.8f + moonPulse * 0.55f, 1.0f,
                         8.8f + moonPulse * 0.55f) *
             XMMatrixRotationX(-XM_PIDIV2) *
             XMMatrixTranslation(moonPos.x, moonPos.y, moonPos.z)});
    frame.transparentItems.push_back(
        {m_spiritVeilMeshId,
         XMMatrixScaling(12.0f + moonPulse * 1.2f, 1.0f,
                         5.8f + moonPulse * 0.6f) *
             XMMatrixRotationX(-XM_PIDIV2) *
             XMMatrixTranslation(moonPos.x, moonPos.y - 0.25f,
                                 moonPos.z + 0.05f)});

    constexpr int kMoonRayCount = 9;
    for (int i = 0; i < kMoonRayCount; ++i) {
      const float fi = static_cast<float>(i);
      const float u = (fi - 4.0f) / 4.0f;
      const float rayPulse = SmoothPulse(time + fi * 0.13f, 1.65f, 0.35f);
      const float x = u * (4.8f + rayPulse * 1.1f);
      const float z = kArenaHalfExtent - 3.3f - std::abs(u) * 2.2f;
      const float height = 7.4f + rayPulse * 1.2f - std::abs(u) * 1.1f;
      pushTransparent(m_moonRayMeshId,
                      VerticalVeilWorld(0.48f + rayPulse * 0.16f, height, x,
                                        4.15f + rayPulse * 0.28f, z,
                                        u * 0.26f));
    }
  }

  for (int i = 0; i < 24; ++i) {
    const float fi = static_cast<float>(i);
    const float baseZ = -17.4f + fi * 1.72f;
    const float z = LoopZ(baseZ, scroll * 1.05f, loopMinZ, pathLength);
    const float edgeFade = LoopEdgeFade(z, loopMinZ, loopMaxZ, 2.8f);
    const float width = (3.05f + static_cast<float>(i % 5) * 0.22f) *
                        (0.94f + edgeFade * 0.06f);
    const float length = 1.24f + static_cast<float>((i + 1) % 3) * 0.20f;
    const float x = (static_cast<float>((i * 37) % 7) - 3.0f) * 0.10f;
    const float yaw = (static_cast<float>((i * 19) % 9) - 4.0f) * 0.015f;
    pushOpaque(m_pathStoneMeshId,
               cubeWorld(width, 0.12f, length, x, 0.105f, z, yaw));
  }

  constexpr int kCurveSegments = 12;
  const float segmentLength = pathLength / static_cast<float>(kCurveSegments);
  for (int i = 0; i < kCurveSegments; ++i) {
    const float baseZ = -kArenaHalfExtent + segmentLength * (i + 0.5f);
    const float z = LoopZ(baseZ, scroll, loopMinZ, pathLength);
    pushOpaque(m_pathEdgeMeshId,
               cubeWorld(0.22f, 0.12f, segmentLength + 0.08f,
                         -pathEdgeWidth, 0.075f, z));
    pushOpaque(m_pathEdgeMeshId,
               cubeWorld(0.22f, 0.12f, segmentLength + 0.08f,
                         pathEdgeWidth, 0.075f, z));
    for (float side : kSides) {
      pushOpaque(m_mossBankMeshId,
                 cubeWorld(4.4f, 0.075f, segmentLength + 0.12f,
                           side * 7.55f, 0.02f, z));
      pushOpaque(m_mossBankMeshId,
                 cubeWorld(3.2f, 0.06f, segmentLength + 0.12f,
                           side * 13.0f, 0.015f, z));
    }
  }

  // 河面は固定した一枚の連続曲面。SSR の反射を分割面ごとに折らない。
  pushOpaque(m_riverWaterMeshId, XMMatrixIdentity());

  const float sealPulse = 1.0f + 0.08f * SmoothPulse(time, 5.8f, 0.0f);
  frame.transparentItems.push_back(
      {m_bossSealMeshId,
       CurveWorld(5.9f * sealPulse, 1.0f, 5.9f * sealPulse, 0.0f, 0.058f,
                  kArenaHalfExtent - 3.5f)});
  const float gatePulse = SmoothPulse(time, 2.4f, 0.30f);
  frame.transparentItems.push_back(
      {m_spiritVeilMeshId,
       CurveWorld(12.6f + gatePulse * 1.8f, 1.0f, 8.4f + gatePulse * 1.2f,
                  0.0f, 3.85f, kArenaHalfExtent - 1.2f)});
  frame.transparentItems.push_back(
      {m_laserRiftMeshId,
       CurveWorld(6.8f + gatePulse * 0.9f, 1.0f, 6.8f + gatePulse * 0.9f,
                  0.0f, 0.14f, kArenaHalfExtent - 3.45f)});
  for (int i = 0; i < 8; ++i) {
    const float fi = static_cast<float>(i);
    const float angle = fi / 8.0f * XM_2PI + time * 0.32f;
    PushCurvedRibbon(frame, m_spiritVeilMeshId, std::cos(angle) * 3.2f, 1.8f,
                     kArenaHalfExtent - 3.5f + std::sin(angle) * 3.2f,
                     std::cos(angle + 0.8f) * 1.1f, 4.8f + gatePulse,
                     kArenaHalfExtent - 2.2f + std::sin(angle + 0.8f) * 1.1f,
                     0.28f + gatePulse * 0.12f, 8);
  }

  constexpr int kSpiritEmbers = 28;
  for (int i = 0; i < kSpiritEmbers; ++i) {
    const float fi = static_cast<float>(i);
    const float side = (i % 2 == 0) ? -1.0f : 1.0f;
    const float z = LoopZ(-18.0f + fi * 1.62f,
                          scroll * (0.22f + 0.025f * (i % 5)), loopMinZ,
                          pathLength);
    const float drift = std::sin(time * 0.95f + fi * 1.37f);
    const float x = side * (6.4f + static_cast<float>((i * 7) % 5) * 1.45f) +
                    drift * 0.42f;
    const float y = 1.05f + static_cast<float>((i * 3) % 7) * 0.28f +
                    std::sin(time * 1.8f + fi) * 0.16f;
    const float emberPulse = SmoothPulse(time + fi * 0.17f, 4.6f, 0.22f);
    const bool warm = (i % 3) == 0;
    pushTransparent(warm ? m_lanternHaloMeshId : m_mirrorChargeMeshId,
                    cubeWorld(warm ? 0.16f + emberPulse * 0.08f
                                   : 0.10f + emberPulse * 0.06f,
                              warm ? 0.16f + emberPulse * 0.08f
                                   : 0.10f + emberPulse * 0.06f,
                              warm ? 0.16f + emberPulse * 0.08f
                                   : 0.10f + emberPulse * 0.06f,
                              x, y, z));
  }

  for (int i = 0; i < 6; ++i) {
    const float z = LoopZ(-17.0f + static_cast<float>(i) * 7.1f,
                          scroll * 0.84f, loopMinZ, pathLength);
    const float edgeFade = LoopEdgeFade(z, loopMinZ, loopMaxZ, 3.4f);
    const float scale =
        (0.84f + static_cast<float>(i % 2) * 0.12f) *
        (0.74f + edgeFade * 0.26f);
    addTorii(-11.5f, z, scale, 0.0f);
    addTorii(11.5f, LoopZ(z + 4.2f, 0.0f, loopMinZ, pathLength),
             scale * 0.92f, 0.0f);
  }
  if (!m_shrineGateMeshIds.empty()) {
    pushMeshGroup(m_shrineGateMeshIds,
                  CurveWorld(0.24f, 0.24f, 0.24f, 0.0f, 0.76f,
                             kArenaHalfExtent + 2.6f));
  } else {
    addTorii(0.0f, kArenaHalfExtent - 1.2f, 1.92f, 0.0f);
  }

  if (m_shrineGateMeshIds.empty() && !m_shrineMeshIds.empty()) {
    const float shrineZ = kArenaHalfExtent + 5.6f;
    const float angle = CurveAngle(shrineZ);
    const XMFLOAT3 p = CurvePoint(0.0f, -0.05f, shrineZ);
    pushMeshGroup(m_shrineMeshIds,
                  XMMatrixScaling(0.034f, 0.034f, 0.034f) *
                      XMMatrixRotationY(XM_PI) *
                      XMMatrixRotationZ(XM_PI) *
                      XMMatrixRotationX(angle) *
                      XMMatrixTranslation(p.x, p.y, p.z));
  }

  constexpr int kLanternPairs = 6;
  for (int i = 0; i < kLanternPairs; ++i) {
    const float z = LoopZ(-16.0f + static_cast<float>(i) * 6.2f,
                          scroll * 1.02f, loopMinZ, pathLength);
    const float edgeFade = LoopEdgeFade(z, loopMinZ, loopMaxZ, 3.0f);
    const float lampPulse = 0.85f + 0.15f * std::sin(time * 4.2f + i * 1.7f);
    for (float side : kSides) {
      const float x = side * 5.7f;
      const float lanternScale = 0.72f * (0.70f + edgeFade * 0.30f);
      if (!m_lanternMeshIds.empty()) {
        const float angle = CurveAngle(z);
        const XMFLOAT3 p = CurvePoint(x, 0.0f, z);
        pushMeshGroup(m_lanternMeshIds,
                      XMMatrixScaling(lanternScale, lanternScale,
                                      lanternScale) *
                          XMMatrixRotationY(side > 0.0f ? -0.22f : 0.22f) *
                          XMMatrixRotationX(angle) *
                          XMMatrixTranslation(p.x, p.y, p.z));
      } else {
        addProceduralLantern(x, z, 0.70f + edgeFade * 0.30f);
      }
      GPUPointLight lamp{};
      lamp.position = CurvePoint(x, 0.92f, z);
      lamp.range = 7.8f;
      lamp.color = {1.0f, 0.48f, 0.18f};
      lamp.intensity = 3.35f * lampPulse * edgeFade;
      frame.pointLights.push_back(lamp);
    }
  }
}

BossArenaScene::RestartDestination
BossArenaScene::DrawHud(int viewportWidth, int viewportHeight) {
  RestartDestination restartDestination = RestartDestination::None;
  const float vw = static_cast<float>(viewportWidth);
  const float vh = static_cast<float>(viewportHeight);
  ImDrawList *draw = ImGui::GetForegroundDrawList();

  if (!IsPhoneOverlayActive()) {
    const bool phase2 = m_bossHp <= 2 && !m_cleared;
    const float phasePulse =
        phase2 ? SmoothPulse(static_cast<float>(ImGui::GetTime()), 5.6f, 0.25f)
               : 0.0f;
    const ImU32 topShade =
        phase2 ? Rgba(0.16f, 0.00f, 0.00f, 0.28f + 0.06f * phasePulse)
               : Rgba(0.00f, 0.045f, 0.055f, 0.26f);
    const ImU32 clearShade = Rgba(0.0f, 0.0f, 0.0f, 0.0f);
    draw->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), ImVec2(vw, vh * 0.22f),
                                  topShade, topShade, clearShade, clearShade);
    draw->AddRectFilledMultiColor(ImVec2(0.0f, vh * 0.76f), ImVec2(vw, vh),
                                  clearShade, clearShade, topShade, topShade);
    draw->AddRectFilledMultiColor(
        ImVec2(0.0f, 0.0f), ImVec2(vw * 0.16f, vh),
        phase2 ? Rgba(0.20f, 0.00f, 0.00f, 0.18f + 0.05f * phasePulse)
               : Rgba(0.00f, 0.055f, 0.060f, 0.14f),
        clearShade, clearShade,
        phase2 ? Rgba(0.20f, 0.00f, 0.00f, 0.18f + 0.05f * phasePulse)
               : Rgba(0.00f, 0.055f, 0.060f, 0.14f));
    draw->AddRectFilledMultiColor(
        ImVec2(vw * 0.84f, 0.0f), ImVec2(vw, vh), clearShade,
        phase2 ? Rgba(0.20f, 0.00f, 0.00f, 0.18f + 0.05f * phasePulse)
               : Rgba(0.00f, 0.055f, 0.060f, 0.14f),
        phase2 ? Rgba(0.20f, 0.00f, 0.00f, 0.18f + 0.05f * phasePulse)
               : Rgba(0.00f, 0.055f, 0.060f, 0.14f),
        clearShade);
  }

  const float bossBarWidth = std::clamp(vw * 0.48f, 420.0f, 720.0f);
  const float bossBarHeight = 18.0f;
  const ImVec2 bossBarMin((vw - bossBarWidth) * 0.5f, 24.0f);
  const ImVec2 bossBarMax(bossBarMin.x + bossBarWidth,
                          bossBarMin.y + bossBarHeight);
  const float bossHpRate = std::clamp(static_cast<float>(m_bossHp) / 5.0f,
                                      0.0f, 1.0f);
  draw->AddRectFilled(ImVec2(bossBarMin.x - 10.0f, bossBarMin.y - 18.0f),
                      ImVec2(bossBarMax.x + 10.0f, bossBarMax.y + 28.0f),
                      Rgba(0.02f, 0.015f, 0.014f, 0.72f), 7.0f);
  draw->AddText(ImVec2(bossBarMin.x, bossBarMin.y - 15.0f),
                Rgba(0.98f, 0.88f, 0.76f, 0.96f), "INK ONI");
  const char *phaseText = m_bossHp <= 2 ? "PHASE 2" : "PHASE 1";
  draw->AddText(ImVec2(bossBarMax.x - 74.0f, bossBarMin.y - 15.0f),
                Rgba(1.0f, 0.72f, 0.38f, 0.96f), phaseText);
  draw->AddRectFilled(bossBarMin, bossBarMax, Rgba(0.14f, 0.04f, 0.035f, 0.96f),
                      4.0f);
  draw->AddRectFilled(
      bossBarMin,
      ImVec2(bossBarMin.x + bossBarWidth * bossHpRate, bossBarMax.y),
      m_bossHp <= 2 ? Rgba(1.0f, 0.26f, 0.12f, 1.0f)
                    : Rgba(0.82f, 0.12f, 0.08f, 1.0f),
      4.0f);
  draw->AddRect(bossBarMin, bossBarMax, Rgba(1.0f, 0.52f, 0.28f, 0.84f), 4.0f,
                0, 2.0f);
  if (m_bossHp <= 2 && !m_cleared) {
    const float phasePulse =
        SmoothPulse(static_cast<float>(ImGui::GetTime()), 7.2f, 0.25f);
    draw->AddRect(ImVec2(bossBarMin.x - 5.0f - 4.0f * phasePulse,
                         bossBarMin.y - 5.0f - 2.0f * phasePulse),
                  ImVec2(bossBarMax.x + 5.0f + 4.0f * phasePulse,
                         bossBarMax.y + 5.0f + 2.0f * phasePulse),
                  Rgba(1.0f, 0.22f, 0.08f, 0.22f + 0.28f * phasePulse),
                  6.0f, 0, 1.8f);
    const char *riftText = "RIFT FIELD ACTIVE";
    const ImVec2 riftSize = ImGui::CalcTextSize(riftText);
    const ImVec2 riftPos((bossBarMin.x + bossBarMax.x - riftSize.x) * 0.5f,
                         bossBarMax.y + 10.0f);
    draw->AddRectFilled(ImVec2(riftPos.x - 14.0f, riftPos.y - 5.0f),
                        ImVec2(riftPos.x + riftSize.x + 14.0f,
                               riftPos.y + riftSize.y + 7.0f),
                        Rgba(0.10f, 0.012f, 0.010f, 0.58f), 5.0f);
    draw->AddText(riftPos, Rgba(1.0f, 0.36f, 0.18f, 0.86f), riftText);
  }
  for (int i = 1; i < 5; ++i) {
    const float x = bossBarMin.x + bossBarWidth * static_cast<float>(i) / 5.0f;
    draw->AddLine(ImVec2(x, bossBarMin.y), ImVec2(x, bossBarMax.y),
                  Rgba(0.02f, 0.015f, 0.014f, 0.72f), 1.4f);
  }
  if (!m_failed && m_counterVfxTimer > 0.0f && !IsPhoneOverlayActive()) {
    const float counterT = std::clamp(m_counterVfxTimer / 0.92f, 0.0f, 1.0f);
    const float flash = EaseOutCubic(counterT);
    const float pulse = SmoothPulse(static_cast<float>(ImGui::GetTime()), 7.0f,
                                    0.0f);
    draw->AddRect(ImVec2(bossBarMin.x - 6.0f - 8.0f * flash,
                         bossBarMin.y - 6.0f - 5.0f * flash),
                  ImVec2(bossBarMax.x + 6.0f + 8.0f * flash,
                         bossBarMax.y + 6.0f + 5.0f * flash),
                  Rgba(0.45f, 1.0f, 0.86f, 0.58f * counterT), 7.0f, 0,
                  2.0f + 2.4f * pulse);
    const char *hitText = "COUNTER HIT  -1";
    ImFont *font = ImGui::GetFont();
    const float hitSizePx = 28.0f;
    const ImVec2 hitSize =
        font->CalcTextSizeA(hitSizePx, FLT_MAX, 0.0f, hitText);
    const ImVec2 hitPos((vw - hitSize.x) * 0.5f,
                        bossBarMax.y + 20.0f - 18.0f * (1.0f - counterT));
    draw->AddText(font, hitSizePx, ImVec2(hitPos.x + 2.0f, hitPos.y + 2.0f),
                  Rgba(0.0f, 0.0f, 0.0f, 0.42f * counterT), hitText);
    draw->AddText(font, hitSizePx, hitPos,
                  Rgba(0.60f, 1.0f, 0.90f, 0.96f * counterT), hitText);
  }

  const float playerPanelX = 24.0f;
  const float playerPanelY = vh - 98.0f;
  draw->AddRectFilled(ImVec2(playerPanelX, playerPanelY),
                      ImVec2(playerPanelX + 208.0f, playerPanelY + 64.0f),
                      Rgba(0.015f, 0.020f, 0.024f, 0.72f), 6.0f);
  draw->AddText(ImVec2(playerPanelX + 14.0f, playerPanelY + 7.0f),
                Rgba(0.76f, 0.92f, 0.88f, 0.96f), "PLAYER");
  for (int i = 0; i < 3; ++i) {
    const bool alive = i < m_playerHp;
    const ImVec2 p(playerPanelX + 82.0f + i * 28.0f, playerPanelY + 27.0f);
    draw->AddCircleFilled(p, alive ? 8.0f : 6.0f,
                          alive ? Rgba(0.35f, 1.0f, 0.74f, 0.96f)
                                : Rgba(0.18f, 0.22f, 0.22f, 0.72f),
                          18);
  }
  draw->AddText(ImVec2(playerPanelX + 14.0f, playerPanelY + 38.0f),
                Rgba(0.54f, 0.95f, 0.90f, 0.92f), "MIRROR");
  for (int i = 0; i < kMirrorChargeMax; ++i) {
    const bool charged = i < m_mirrorCharge;
    const ImVec2 p(playerPanelX + 92.0f + i * 28.0f, playerPanelY + 49.0f);
    draw->AddCircleFilled(p, charged ? 7.5f : 5.5f,
                          charged ? Rgba(0.32f, 1.0f, 0.86f, 0.98f)
                                  : Rgba(0.12f, 0.24f, 0.25f, 0.70f),
                          18);
    draw->AddCircle(p, 9.5f, Rgba(0.42f, 1.0f, 0.88f, 0.46f), 18, 1.3f);
  }
  if (!m_failed && !m_cleared && !IsPhoneOverlayActive() &&
      m_mirrorPickupToastTimer > 0.0f && m_mirrorCharge > 0 &&
      m_mirrorCharge < kMirrorChargeMax && !m_mirrorPuzzleReady) {
    const float toastAlpha = std::clamp(m_mirrorPickupToastTimer / 0.72f,
                                        0.0f, 1.0f);
    char mirrorToast[32]{};
    std::snprintf(mirrorToast, sizeof(mirrorToast), "MIRROR +1   %d/%d",
                  m_mirrorCharge, kMirrorChargeMax);
    const ImVec2 toastSize = ImGui::CalcTextSize(mirrorToast);
    const ImVec2 toastMin(playerPanelX + 34.0f,
                          playerPanelY - 42.0f - 12.0f * (1.0f - toastAlpha));
    const ImVec2 toastMax(toastMin.x + toastSize.x + 34.0f,
                          toastMin.y + toastSize.y + 20.0f);
    draw->AddRectFilled(toastMin, toastMax,
                        Rgba(0.02f, 0.075f, 0.070f, 0.74f * toastAlpha),
                        6.0f);
    draw->AddRect(toastMin, toastMax,
                  Rgba(0.40f, 1.0f, 0.86f, 0.54f * toastAlpha), 6.0f, 0,
                  1.4f);
    draw->AddText(ImVec2(toastMin.x + 17.0f, toastMin.y + 10.0f),
                  Rgba(0.75f, 1.0f, 0.92f, 0.96f * toastAlpha),
                  mirrorToast);
  }

  if (!m_failed && !m_cleared && !IsPhoneOverlayActive()) {
    const float damageFlash = std::clamp(m_hitFlashTimer / 0.54f, 0.0f, 1.0f);
    if (damageFlash > 0.0f) {
      draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(vw, vh),
                          Rgba(0.70f, 0.02f, 0.01f, 0.18f * damageFlash));
      const char *hitText = "PLAYER HIT  -1";
      ImFont *font = ImGui::GetFont();
      const float hitSizePx = 26.0f;
      const ImVec2 hitSize =
          font->CalcTextSizeA(hitSizePx, FLT_MAX, 0.0f, hitText);
      const ImVec2 hitPos((vw - hitSize.x) * 0.5f,
                          vh * 0.27f - 22.0f * (1.0f - damageFlash));
      draw->AddText(font, hitSizePx, ImVec2(hitPos.x + 2.0f, hitPos.y + 2.0f),
                    Rgba(0.0f, 0.0f, 0.0f, 0.46f * damageFlash), hitText);
      draw->AddText(font, hitSizePx, hitPos,
                    Rgba(1.0f, 0.34f, 0.16f, 0.96f * damageFlash), hitText);
    }
    if (m_playerHp <= 1) {
      const float dangerPulse = 0.35f + 0.35f * SmoothPulse(
                                             static_cast<float>(ImGui::GetTime()),
                                             5.8f, 0.0f);
      draw->AddRect(ImVec2(8.0f, 8.0f), ImVec2(vw - 8.0f, vh - 8.0f),
                    Rgba(1.0f, 0.10f, 0.06f, dangerPulse), 6.0f, 0, 3.0f);
    }
    if (m_counterVfxTimer > 0.0f) {
      const float impact =
          std::clamp(m_counterVfxTimer / 0.92f, 0.0f, 1.0f);
      const float wave = 1.0f - impact;
      const ImVec2 screenCenter(vw * 0.5f, vh * 0.48f);
      draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(vw, vh),
                          Rgba(0.18f, 0.95f, 0.82f, 0.08f * impact));
      DrawArc(draw, screenCenter, 160.0f + 260.0f * wave, -0.20f, 2.80f,
              Rgba(0.45f, 1.0f, 0.88f, 0.34f * impact), 2.2f);
      DrawArc(draw, screenCenter, 210.0f + 320.0f * wave, 3.12f, 6.08f,
              Rgba(0.45f, 1.0f, 0.88f, 0.24f * impact), 1.8f);
      draw->AddLine(ImVec2(screenCenter.x - 260.0f - 180.0f * wave,
                           screenCenter.y),
                    ImVec2(screenCenter.x - 48.0f, screenCenter.y),
                    Rgba(0.48f, 1.0f, 0.90f, 0.22f * impact), 2.0f);
      draw->AddLine(ImVec2(screenCenter.x + 48.0f, screenCenter.y),
                    ImVec2(screenCenter.x + 260.0f + 180.0f * wave,
                           screenCenter.y),
                    Rgba(0.48f, 1.0f, 0.90f, 0.22f * impact), 2.0f);
    }

    const char *steps[3] = {"COLLECT MIRRORS", "OPEN PHONE", "COUNTER BOSS"};
    const int activeStep =
        m_mirrorCharge < kMirrorChargeMax ? 0 : (m_mirrorPuzzleReady ? 1 : 2);
    const float panelW = std::clamp(vw * 0.36f, 390.0f, 560.0f);
    const ImVec2 guideMin((vw - panelW) * 0.5f, vh - 126.0f);
    const ImVec2 guideMax(guideMin.x + panelW, guideMin.y + 48.0f);
    draw->AddRectFilled(guideMin, guideMax, Rgba(0.015f, 0.024f, 0.026f, 0.70f),
                        7.0f);
    draw->AddRect(guideMin, guideMax, Rgba(0.34f, 1.0f, 0.84f, 0.34f), 7.0f,
                  0, 1.2f);
    const float stepW = panelW / 3.0f;
    for (int i = 0; i < 3; ++i) {
      const bool active = i == activeStep;
      const bool done = i < activeStep;
      const ImVec2 stepMin(guideMin.x + stepW * i + 8.0f, guideMin.y + 9.0f);
      const ImVec2 stepMax(guideMin.x + stepW * (i + 1) - 8.0f,
                           guideMax.y - 9.0f);
      draw->AddRectFilled(stepMin, stepMax,
                          active ? Rgba(0.08f, 0.26f, 0.24f, 0.88f)
                                 : Rgba(0.03f, 0.06f, 0.065f, 0.74f),
                          5.0f);
      draw->AddRect(stepMin, stepMax,
                    active ? Rgba(0.38f, 1.0f, 0.84f, 0.82f)
                           : Rgba(0.34f, 0.62f, 0.60f, 0.34f),
                    5.0f, 0, active ? 1.8f : 1.0f);
      const ImU32 textColor =
          done ? Rgba(0.48f, 1.0f, 0.82f, 0.78f)
               : (active ? Rgba(0.88f, 1.0f, 0.94f, 0.98f)
                         : Rgba(0.46f, 0.62f, 0.60f, 0.78f));
      const ImVec2 labelSize = ImGui::CalcTextSize(steps[i]);
      draw->AddText(ImVec2((stepMin.x + stepMax.x - labelSize.x) * 0.5f,
                           stepMin.y + 8.0f),
                    textColor, steps[i]);
    }
  }

  if (!m_failed && !m_cleared && !IsPhoneOverlayActive()) {
    const char *attackBanner = "INK LASER";
    if (m_attack == AttackType::MeteorAoE)
      attackBanner = "METEOR FIELD";
    else if (m_attack == AttackType::SanctuarySeal)
      attackBanner = "SANCTUARY SEAL";
    const char *banner =
        m_counterVfxTimer > 0.0f
            ? "MIZUKAGAMI COUNTER"
            : (m_phase == AttackPhase::Telegraph ? attackBanner : "IMPACT");
    const ImVec2 textSize = ImGui::CalcTextSize(banner);
    const ImVec2 center(vw * 0.5f, vh * 0.18f);
    const float alpha = m_counterVfxTimer > 0.0f ? 0.94f : 0.70f;
    draw->AddRectFilled(ImVec2(center.x - textSize.x * 0.5f - 18.0f,
                               center.y - 8.0f),
                        ImVec2(center.x + textSize.x * 0.5f + 18.0f,
                               center.y + textSize.y + 10.0f),
                        Rgba(0.02f, 0.016f, 0.014f, alpha * 0.70f), 5.0f);
    draw->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y),
                  m_counterVfxTimer > 0.0f
                      ? Rgba(0.40f, 1.0f, 0.86f, alpha)
                      : Rgba(1.0f, 0.72f, 0.26f, alpha),
                  banner);
    if (m_bossHp == 2 && m_counterVfxTimer > 0.0f) {
      const char *phaseShift = "PHASE 2 AWAKENED";
      const ImVec2 phaseSize = ImGui::CalcTextSize(phaseShift);
      const ImVec2 phasePos(center.x - phaseSize.x * 0.5f,
                            center.y + textSize.y + 18.0f);
      const float phasePulse =
          SmoothPulse(static_cast<float>(ImGui::GetTime()), 6.5f, 0.35f);
      draw->AddRectFilled(ImVec2(phasePos.x - 18.0f, phasePos.y - 8.0f),
                          ImVec2(phasePos.x + phaseSize.x + 18.0f,
                                 phasePos.y + phaseSize.y + 10.0f),
                          Rgba(0.10f, 0.015f, 0.010f, 0.66f * alpha), 5.0f);
      draw->AddRect(ImVec2(phasePos.x - 18.0f, phasePos.y - 8.0f),
                    ImVec2(phasePos.x + phaseSize.x + 18.0f,
                           phasePos.y + phaseSize.y + 10.0f),
                    Rgba(1.0f, 0.24f, 0.08f, 0.46f * phasePulse), 5.0f, 0,
                    1.6f);
      draw->AddText(phasePos, Rgba(1.0f, 0.42f, 0.18f, 0.92f * alpha),
                    phaseShift);
    }
  }

  if (!IsPhoneOverlayActive() && !m_failed && !m_cleared) {
    char promptBuffer[64]{};
    const char *prompt = promptBuffer;
    const bool mirrorReady = m_mirrorCharge >= kMirrorChargeMax;
    if (m_mirrorCharge >= kMirrorChargeMax) {
      prompt = "SPACE: OPEN MIZUKAGAMI";
    } else {
      std::snprintf(promptBuffer, sizeof(promptBuffer),
                    "MIRROR CHARGE %d/%d", m_mirrorCharge,
                    kMirrorChargeMax);
    }
    const ImVec2 promptSize = ImGui::CalcTextSize(prompt);
    const ImVec2 promptPos(vw - promptSize.x - 34.0f, vh - 58.0f);
    const float readyPulse =
        mirrorReady ? SmoothPulse(static_cast<float>(ImGui::GetTime()), 4.8f,
                                  0.0f)
                    : 0.0f;
    const ImU32 promptBg =
        mirrorReady ? Rgba(0.02f, 0.08f, 0.075f, 0.78f + 0.08f * readyPulse)
                    : Rgba(0.02f, 0.018f, 0.016f, 0.74f);
    const ImU32 promptBorder =
        mirrorReady ? Rgba(0.40f, 1.0f, 0.86f, 0.36f + 0.34f * readyPulse)
                    : Rgba(0.36f, 0.80f, 0.72f, 0.24f);
    draw->AddRectFilled(ImVec2(promptPos.x - 14.0f, promptPos.y - 8.0f),
                        ImVec2(promptPos.x + promptSize.x + 14.0f,
                               promptPos.y + promptSize.y + 10.0f),
                        promptBg, 5.0f);
    draw->AddRect(ImVec2(promptPos.x - 14.0f, promptPos.y - 8.0f),
                  ImVec2(promptPos.x + promptSize.x + 14.0f,
                         promptPos.y + promptSize.y + 10.0f),
                  promptBorder, 5.0f, 0, mirrorReady ? 1.8f : 1.0f);
    if (mirrorReady) {
      draw->AddRect(ImVec2(promptPos.x - 20.0f - 4.0f * readyPulse,
                           promptPos.y - 14.0f - 3.0f * readyPulse),
                    ImVec2(promptPos.x + promptSize.x + 20.0f +
                               4.0f * readyPulse,
                           promptPos.y + promptSize.y + 16.0f +
                               3.0f * readyPulse),
                    Rgba(0.38f, 1.0f, 0.88f, 0.20f * readyPulse), 7.0f, 0,
                    1.4f);
    }
    draw->AddText(promptPos,
                  mirrorReady ? Rgba(0.78f, 1.0f, 0.92f, 0.98f)
                              : Rgba(0.44f, 1.0f, 0.86f, 0.96f),
                  prompt);
  }

  if (m_showDebugPanel) {
    ImGui::SetNextWindowPos(ImVec2(18.0f, 78.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(286.0f, 0.0f), ImGuiCond_Always);
    ImGui::Begin("##BossArenaHud", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::Text("Boss Arena Debug");
    ImGui::Separator();
    ImGui::Text("Mechanic: %s", AttackName());
    ImGui::Text(
        "Phase: %s",
        m_phase == AttackPhase::Telegraph
            ? "Telegraph"
            : (m_phase == AttackPhase::Resolve ? "Resolve" : "Recovery"));
    ImGui::Text("Timer: %.1f", std::max(0.0f, m_phaseTimer));
    bool puzzleSeedChanged =
        ImGui::Checkbox("Fixed Puzzle Seed", &m_debugFixedPuzzleSeed);
    ImGui::BeginDisabled(!m_debugFixedPuzzleSeed);
    puzzleSeedChanged |= ImGui::InputInt("Puzzle Seed", &m_debugPuzzleSeed);
    ImGui::EndDisabled();
    m_debugPuzzleSeed = std::max(1, m_debugPuzzleSeed);
    if (puzzleSeedChanged && m_countersUsed == 0 && !m_mirrorPuzzleReady &&
        !m_pendingMirrorPuzzle) {
      ResetMirrorPuzzleSchedule();
    }
    ImGui::Text("Current Puzzle Seed: %u", m_battlePuzzleSeed);
    const auto puzzleName = [](MirrorPuzzleType type) {
      switch (type) {
      case MirrorPuzzleType::SymbolMemory:
        return "Symbol";
      case MirrorPuzzleType::NumberPosition:
        return "Number";
      case MirrorPuzzleType::ReflectionTrace:
        return "Trace";
      }
      return "Unknown";
    };
    ImGui::Text("P1 Bag: %s > %s > %s", puzzleName(m_phaseOnePuzzleBag[0]),
                puzzleName(m_phaseOnePuzzleBag[1]),
                puzzleName(m_phaseOnePuzzleBag[2]));
    if (m_phaseTwoPuzzleBagReady) {
      ImGui::Text("P2 Bag: %s > %s", puzzleName(m_phaseTwoPuzzleBag[0]),
                  puzzleName(m_phaseTwoPuzzleBag[1]));
    }
    if (m_pendingMirrorPuzzle)
      ImGui::Text("Pending Retry: %s", puzzleName(m_pendingMirrorPuzzleType));
    if (ImGui::Button("Set Boss HP to 1") && !m_failed && !m_cleared) {
      // 最終カウンターの検証時間を短縮する Debug パネル専用操作。
      if (m_bossHp > 2) {
        m_phaseTwoIntroPending = true;
        m_phaseTwoPatternIndex = 0;
      }
      m_bossHp = 1;
      m_pendingMirrorPuzzle = false;
      m_phaseTwoPuzzleBagReady = false;
      PreparePhaseTwoPuzzleBag();
      m_phaseShiftVfxTimer = 1.25f;
    }
    if (ImGui::Button("Force Sanctuary Seal") && !m_failed && !m_cleared) {
      const bool useIntroTuning = m_bossHp > 2 || m_phaseTwoIntroPending;
      m_phaseTwoIntroPending = false;
      if (m_bossHp <= 2)
        m_phaseTwoPatternIndex = 0;
      ConfigureAttack(AttackType::SanctuarySeal, useIntroTuning);
    }
    const auto forceTrace = [this](ReflectionTraceVariant variant) {
      // 強制テストは shuffle bag を消費せず、現在の pending retry を解除する。
      m_pendingMirrorPuzzle = false;
      m_mirrorCharge = kMirrorChargeMax;
      m_mirrorChargeActive = {false, false, false};
      PrepareReflectionTracePuzzle(m_attack, variant);
      m_phoneOpen = true;
    };
    if (ImGui::Button("Force Attack Trace") && !m_failed && !m_cleared) {
      forceTrace(ReflectionTraceVariantForAttack(m_attack));
    }
    if (ImGui::Button("Force Branch Trace") && !m_failed && !m_cleared) {
      forceTrace(ReflectionTraceVariant::PhaseTwoBranch);
    }
    if (ImGui::Button("Force Reverse Trace") && !m_failed && !m_cleared) {
      forceTrace(ReflectionTraceVariant::PhaseTwoReversed);
    }
    ImGui::Text("Result screen: choose restart point");
    ImGui::Text("Phone: SPACE");
    ImGui::Text("Open: F3 / Camera Position panel");
    ImGui::Separator();
    ImGui::Checkbox("No Clip", &m_debugNoClip);
    if (m_debugNoClip)
      ImGui::Text("Q/E: Fly | No damage");
    if (m_mirrorPuzzleReady)
      ImGui::Text("Mizukagami: READY");
    ImGui::SliderFloat("Ready River Electric",
                       &m_readyRiverElectricIntensity,
                       0.0f, 1.5f, "%.2f");
    ImGui::Separator();
    ImGui::Text("Tech Showcase");
    ImGui::Checkbox("Override BossArena Render", &m_techShowcaseOverride);
    ImGui::BeginDisabled(!m_techShowcaseOverride);
    ImGui::Checkbox("CSM Shadows", &m_showcaseShadows);
    ImGui::Checkbox("SSAO", &m_showcaseSSAO);
    ImGui::Checkbox("SSR Water Reflection (test only)", &m_showcaseSSR);
    ImGui::Checkbox("Bloom", &m_showcaseBloom);
    ImGui::Checkbox("FXAA", &m_showcaseFXAA);
    ImGui::Checkbox("TAA", &m_showcaseTAA);
    ImGui::Checkbox("Motion Blur", &m_showcaseMotionBlur);
    ImGui::Checkbox("Phone DOF", &m_showcaseDOF);
    ImGui::Checkbox("Rain Particles", &m_showcaseRain);
    ImGui::Checkbox("Climax Counter VFX", &m_showcaseClimaxVfx);
    ImGui::Separator();
    ImGui::Text("Major VFX Stack");
    ImGui::BulletText("Night fog / spirit veil");
    ImGui::BulletText("Lantern bloom / shrine gate aura");
    ImGui::BulletText("Moon rays / side spirit embers");
    ImGui::BulletText("Meteor pillars / laser rift");
    ImGui::BulletText("River currents / counter moon flash");
    ImGui::BulletText("Phone rune trace puzzle");
    ImGui::EndDisabled();
    ImGui::End();
  }

  // 最終カウンター後は携帯の退出アニメーションを完了してから
  // リザルトを表示し、二つの UI が重ならないようにする。
  const bool resultVisible =
      (m_failed || m_cleared) && m_phoneSlide <= 0.002f;
  if (resultVisible) {
    const ImVec2 screenMin(0.0f, 0.0f);
    const ImVec2 screenMax(vw, vh);
    draw->AddRectFilled(screenMin, screenMax,
                        m_cleared ? Rgba(0.00f, 0.05f, 0.06f, 0.74f)
                                  : Rgba(0.08f, 0.02f, 0.02f, 0.78f));

    const ImVec2 center(vw * 0.5f, vh * 0.46f);
    const float panelW = std::clamp(vw * 0.46f, 520.0f, 760.0f);
    const float panelH = std::clamp(vh * 0.54f, 440.0f, 560.0f);
    const ImVec2 panelMin(center.x - panelW * 0.5f, center.y - panelH * 0.5f);
    const ImVec2 panelMax(center.x + panelW * 0.5f, center.y + panelH * 0.5f);

    const ImU32 accent =
        m_cleared ? Rgba(0.35f, 1.0f, 0.86f, 0.96f)
                  : Rgba(1.0f, 0.28f, 0.16f, 0.96f);
    const ImU32 accentSoft =
        m_cleared ? Rgba(0.22f, 0.86f, 0.78f, 0.28f)
                  : Rgba(1.0f, 0.20f, 0.12f, 0.25f);

    for (int i = 0; i < 4; ++i) {
      const float radius = 92.0f + static_cast<float>(i) * 42.0f;
      DrawArc(draw, center, radius, 0.15f + i * 0.28f,
              4.85f + i * 0.20f, accentSoft, 2.0f);
    }

    draw->AddRectFilled(panelMin, panelMax, Rgba(0.015f, 0.020f, 0.022f, 0.90f),
                        8.0f);
    draw->AddRect(panelMin, panelMax, accent, 8.0f, 0, 2.0f);
    draw->AddLine(ImVec2(panelMin.x + 34.0f, panelMin.y + 78.0f),
                  ImVec2(panelMax.x - 34.0f, panelMin.y + 78.0f), accent,
                  1.8f);

    const char *title = m_cleared ? "MISSION CLEAR" : "MISSION FAILED";
    const char *subtitle =
        m_cleared ? "Mizukagami counter sealed the Ink Oni."
                  : "The mirror trace was broken.";
    ImFont *font = ImGui::GetFont();
    const float titleSizePx = 38.0f;
    const ImVec2 titleSize = font->CalcTextSizeA(titleSizePx, FLT_MAX, 0.0f,
                                                 title);
    draw->AddText(font, titleSizePx,
                  ImVec2((panelMin.x + panelMax.x - titleSize.x) * 0.5f,
                         panelMin.y + 28.0f),
                  accent, title);
    const ImVec2 subtitleSize = ImGui::CalcTextSize(subtitle);
    draw->AddText(ImVec2((panelMin.x + panelMax.x - subtitleSize.x) * 0.5f,
                         panelMin.y + 92.0f),
                  Rgba(0.78f, 0.92f, 0.88f, 0.92f), subtitle);

    char timeText[48]{};
    char counterText[48]{};
    char damageText[48]{};
    std::snprintf(timeText, sizeof(timeText), "CLEAR TIME  %02d:%02d",
                  static_cast<int>(m_battleTimer) / 60,
                  static_cast<int>(m_battleTimer) % 60);
    std::snprintf(counterText, sizeof(counterText), "COUNTERS    %d",
                  m_countersUsed);
    std::snprintf(damageText, sizeof(damageText), "DAMAGE      %d",
                  m_damageTaken);

    const char *stats[3] = {timeText, counterText, damageText};
    const float statW = (panelW - 100.0f) / 3.0f;
    const float statY = panelMin.y + 156.0f;
    for (int i = 0; i < 3; ++i) {
      const ImVec2 statMin(panelMin.x + 34.0f + i * (statW + 16.0f), statY);
      const ImVec2 statMax(statMin.x + statW, statY + 72.0f);
      draw->AddRectFilled(statMin, statMax,
                          Rgba(0.04f, 0.08f, 0.085f, 0.82f), 6.0f);
      draw->AddRect(statMin, statMax, Rgba(0.36f, 1.0f, 0.86f, 0.42f), 6.0f,
                    0, 1.2f);
      const ImVec2 statSize = ImGui::CalcTextSize(stats[i]);
      draw->AddText(ImVec2((statMin.x + statMax.x - statSize.x) * 0.5f,
                           statMin.y + 27.0f),
                    Rgba(0.84f, 1.0f, 0.94f, 0.96f), stats[i]);
    }

    const char *rank = "--";
    const char *rankNote = "TRACE BROKEN";
    ImU32 rankColor = Rgba(0.78f, 0.88f, 0.90f, 0.96f);
    const int battleSeconds = static_cast<int>(m_battleTimer);
    if (m_cleared) {
      if (m_damageTaken == 0 && m_countersUsed <= 5 && battleSeconds <= 90) {
        rank = "S";
        rankNote = "PERFECT COUNTER";
        rankColor = Rgba(1.0f, 0.88f, 0.32f, 0.98f);
      } else if (m_damageTaken <= 1 && m_countersUsed <= 6) {
        rank = "A";
        rankNote = "STABLE CLEAR";
        rankColor = Rgba(0.42f, 1.0f, 0.82f, 0.98f);
      } else {
        rank = "B";
        rankNote = "MISSION CLEAR";
        rankColor = Rgba(0.66f, 0.84f, 1.0f, 0.98f);
      }
    }

    const ImVec2 rankCenter((panelMin.x + panelMax.x) * 0.5f,
                            panelMin.y + 266.0f);
    const ImVec2 rankMin(rankCenter.x - 112.0f, rankCenter.y - 38.0f);
    const ImVec2 rankMax(rankCenter.x + 112.0f, rankCenter.y + 38.0f);
    draw->AddRectFilled(rankMin, rankMax, Rgba(0.03f, 0.06f, 0.065f, 0.88f),
                        8.0f);
    draw->AddRect(rankMin, rankMax, rankColor, 8.0f, 0, 1.8f);
    DrawArc(draw, rankCenter, 64.0f, -0.25f, 2.98f, rankColor, 2.0f);
    DrawArc(draw, rankCenter, 72.0f, 3.35f, 6.05f, accentSoft, 1.6f);
    const char *rankLabel = "RANK";
    const ImVec2 rankLabelSize = ImGui::CalcTextSize(rankLabel);
    draw->AddText(ImVec2(rankCenter.x - rankLabelSize.x * 0.5f,
                         rankMin.y + 9.0f),
                  Rgba(0.72f, 0.88f, 0.86f, 0.88f), rankLabel);
    const float rankSizePx = 44.0f;
    const ImVec2 rankSize =
        font->CalcTextSizeA(rankSizePx, FLT_MAX, 0.0f, rank);
    draw->AddText(font, rankSizePx,
                  ImVec2(rankCenter.x - rankSize.x * 0.5f,
                         rankCenter.y - 6.0f),
                  rankColor, rank);
    const ImVec2 rankNoteSize = ImGui::CalcTextSize(rankNote);
    draw->AddText(ImVec2(rankCenter.x - rankNoteSize.x * 0.5f,
                         rankMax.y + 12.0f),
                  Rgba(0.74f, 0.92f, 0.88f, 0.90f), rankNote);

    const char *choicePrompt = "SELECT RESTART POINT";
    const ImVec2 choicePromptSize = ImGui::CalcTextSize(choicePrompt);
    draw->AddText(ImVec2((panelMin.x + panelMax.x - choicePromptSize.x) * 0.5f,
                         panelMax.y - 104.0f),
                  Rgba(0.66f, 0.82f, 0.80f, 0.92f), choicePrompt);

    // 結果画面から、作品全体の再開地点を明示的に選択できるようにする。
    const float buttonGap = 16.0f;
    const float buttonW = (panelW - 68.0f - buttonGap) * 0.5f;
    const float buttonH = 52.0f;
    const float buttonY = panelMax.y - 76.0f;
    const ImVec2 overworldMin(panelMin.x + 34.0f, buttonY);
    const ImVec2 overworldMax(overworldMin.x + buttonW, buttonY + buttonH);
    const ImVec2 bossMin(overworldMax.x + buttonGap, buttonY);
    const ImVec2 bossMax(bossMin.x + buttonW, buttonY + buttonH);

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vw, vh), ImGuiCond_Always);
    ImGui::Begin("##BossArenaRestartChoices", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNavFocus);

    const auto drawChoice = [&](const char *id, const char *label,
                                const ImVec2 &buttonMin,
                                const ImVec2 &buttonMax,
                                ImU32 buttonAccent) {
      ImGui::SetCursorScreenPos(buttonMin);
      const bool clicked = ImGui::InvisibleButton(
          id, ImVec2(buttonMax.x - buttonMin.x, buttonMax.y - buttonMin.y));
      const bool hovered = ImGui::IsItemHovered();
      draw->AddRectFilled(
          buttonMin, buttonMax,
          hovered ? Rgba(0.08f, 0.16f, 0.16f, 0.98f)
                  : Rgba(0.025f, 0.060f, 0.064f, 0.94f),
          6.0f);
      draw->AddRect(buttonMin, buttonMax, buttonAccent, 6.0f, 0,
                    hovered ? 2.4f : 1.5f);
      const ImVec2 labelSize = ImGui::CalcTextSize(label);
      draw->AddText(
          ImVec2((buttonMin.x + buttonMax.x - labelSize.x) * 0.5f,
                 (buttonMin.y + buttonMax.y - labelSize.y) * 0.5f),
          hovered ? Rgba(0.94f, 1.0f, 0.98f, 1.0f)
                  : Rgba(0.82f, 0.96f, 0.92f, 0.96f),
          label);
      return clicked;
    };

    if (drawChoice("##restart-overworld", "RESTART FROM OVERWORLD",
                   overworldMin, overworldMax,
                   Rgba(0.36f, 1.0f, 0.84f, 0.88f))) {
      restartDestination = RestartDestination::Overworld;
    }
    if (drawChoice("##restart-boss", "RETRY BOSS ARENA", bossMin, bossMax,
                   Rgba(1.0f, 0.52f, 0.28f, 0.88f))) {
      restartDestination = RestartDestination::BossArena;
    }
    ImGui::End();
  }

  DrawPhoneOverlay(viewportWidth, viewportHeight);
  return restartDestination;
}

void BossArenaScene::StartNextAttack() {
  ++m_attackIndex;
  if (m_phaseTwoIntroPending) {
    m_phaseTwoIntroPending = false;
    m_phaseTwoPatternIndex = 0;
    ConfigureAttack(AttackType::SanctuarySeal, true);
    return;
  }

  if (m_bossHp <= 2) {
    constexpr AttackType kPhaseTwoPattern[3] = {
        AttackType::MeteorAoE,
        AttackType::LaserLine,
        AttackType::SanctuarySeal,
    };
    const AttackType next =
        kPhaseTwoPattern[m_phaseTwoPatternIndex % 3];
    ++m_phaseTwoPatternIndex;
    ConfigureAttack(next);
    return;
  }

  ConfigureAttack(m_attackIndex % 2 == 0 ? AttackType::MeteorAoE
                                         : AttackType::LaserLine);
}

void BossArenaScene::ConfigureAttack(AttackType attack,
                                     bool sanctuaryIntro) {
  const bool phase2 = m_bossHp <= 2;
  m_attack = attack;
  m_phase = AttackPhase::Telegraph;
  m_resolved = false;
  m_sanctuaryIntroActive = false;

  if (attack == AttackType::MeteorAoE) {
    constexpr float kMeteorZs[3] = {-14.0f, -11.8f, -9.4f};
    const float x = (m_attackIndex % 2 == 0) ? -2.95f : 2.95f;
    const float z = kMeteorZs[m_attackIndex % 3];
    m_attackCenter = {x, 0.02f, z};
    m_attackRadius = phase2 ? 3.45f : 3.15f;
    m_phaseTimer = phase2 ? 1.32f : 1.8f;
  } else if (attack == AttackType::LaserLine) {
    constexpr float kLaserZs[3] = {-14.2f, -11.7f, -9.2f};
    const int laserCycle = m_attackIndex / 2;
    m_laserVertical = (laserCycle % 2) == 0;
    m_attackCenter = {0.0f, 0.02f, kLaserZs[laserCycle % 3]};
    m_laserHalfWidth = phase2 ? 1.70f : 1.45f;
    m_phaseTimer = phase2 ? 1.22f : 1.65f;
  } else if (attack == AttackType::SanctuarySeal) {
    m_sanctuaryIntroActive = sanctuaryIntro;
    if (sanctuaryIntro) {
      m_attackCenter = {0.0f, 0.02f, -11.7f};
      m_attackRadius = 2.25f;
      m_phaseTimer = 2.10f;
    } else {
      constexpr float kSealXs[3] = {-1.95f, 1.95f, 0.0f};
      constexpr float kSealZs[3] = {-13.2f, -10.2f, -11.7f};
      const int placement = (m_attackIndex / 3) % 3;
      m_attackCenter = {kSealXs[placement], 0.02f,
                        kSealZs[placement]};
      m_attackRadius = 1.70f;
      m_phaseTimer = 1.55f;
    }
  }
}

void BossArenaScene::ResolveAttack(PlayerAnimationPreview &player) {
  m_impactTimer = 0.58f;
  m_impactMaxRadius = 4.4f;
  m_impactPosition = m_attackCenter;
  m_impactPosition.y = 0.07f;

  if (m_attack == AttackType::LaserLine) {
    m_impactPosition = player.Position();
    m_impactPosition.y = 0.07f;
    m_impactMaxRadius = 3.8f;
  } else if (m_attack == AttackType::MeteorAoE && m_aoeSparkBurst) {
    const XMFLOAT3 burstPos =
        CurvePoint(m_attackCenter.x, 0.32f, m_attackCenter.z);
    m_aoeSparkTimer = 1.15f;
    m_aoeSparkBurst->Fire(
        XMVectorSet(burstPos.x, burstPos.y, burstPos.z, 0.0f));
  } else if (m_attack == AttackType::SanctuarySeal) {
    m_impactMaxRadius = m_attackRadius + 1.0f;
  }

  if (!m_debugNoClip && IsPlayerInCurrentAttack(player.Position())) {
    m_lastHitPosition = player.Position();
    m_lastHitPosition.y = 0.04f;
    m_hitFlashTimer = 0.54f;
    m_playerHitParticleTimer = 0.92f;
    if (m_playerHitBurst) {
      const XMFLOAT3 hitParticlePos =
          CurvePoint(m_lastHitPosition.x, 1.05f, m_lastHitPosition.z);
      m_playerHitBurst->Fire(XMVectorSet(
          hitParticlePos.x, hitParticlePos.y, hitParticlePos.z, 0.0f));
    }
    --m_playerHp;
    ++m_damageTaken;
    if (m_playerHp <= 0) {
      m_playerHp = 0;
      m_failed = true;
    }
  }
  if (!m_failed && m_mirrorCharge >= kMirrorChargeMax)
    PrepareMirrorPuzzle(m_attack);
}
bool BossArenaScene::IsPlayerInCurrentAttack(const XMFLOAT3 &playerPos) const {
  if (m_attack == AttackType::MeteorAoE) {
    const float r = m_attackRadius + kPlayerHitRadius;
    return DistanceSq2D(playerPos, m_attackCenter) <= r * r;
  }

  if (m_attack == AttackType::LaserLine) {
    if (m_laserVertical)
      return std::abs(playerPos.x) <= m_laserHalfWidth + kPlayerHitRadius;
    return std::abs(playerPos.z - m_attackCenter.z) <=
           m_laserHalfWidth + kPlayerHitRadius;
  }

  if (m_attack == AttackType::SanctuarySeal) {
    // プレイヤー全体が結界内に入った場合だけ安全と判定する。
    const float safeRadius =
        std::max(0.0f, m_attackRadius - kPlayerHitRadius);
    return DistanceSq2D(playerPos, m_attackCenter) >
           safeRadius * safeRadius;
  }

  return false;
}

const char *BossArenaScene::AttackName() const {
  switch (m_attack) {
  case AttackType::MeteorAoE:
    return "Meteor AoE";
  case AttackType::LaserLine:
    return m_laserVertical ? "Laser Column" : "Laser Row";
  case AttackType::SanctuarySeal:
    return "Sanctuary Seal";
  }
  return "Unknown";
}

void BossArenaScene::AppendTelegraphLines(FrameData &frame) const {
  if (m_phase == AttackPhase::Recovery)
    return;

  const float telegraphRemain =
      std::clamp(m_phaseTimer / TelegraphDuration(), 0.0f, 1.0f);
  const float chargeT = 1.0f - telegraphRemain;
  const XMFLOAT4 warnColor =
      m_phase == AttackPhase::Telegraph
          ? XMFLOAT4{1.0f, 0.08f + chargeT * 0.34f, 0.02f, 1.0f}
          : XMFLOAT4{1.0f, 0.18f, 0.06f, 0.88f};

  if (m_attack == AttackType::MeteorAoE) {
    const XMFLOAT3 curvedCenter =
        CurvePoint(m_attackCenter.x, m_attackCenter.y, m_attackCenter.z);
    PushCircle(frame, curvedCenter, m_attackRadius, warnColor);
  } else if (m_attack == AttackType::LaserLine) {
    if (m_laserVertical) {
      PushCurvedRect(frame, -m_laserHalfWidth, kRunnerMinZ, m_laserHalfWidth,
                     kRunnerMaxZ, 0.03f, warnColor);
    } else {
      PushCurvedRect(frame, -kLaneHalfWidth - 0.70f,
                     m_attackCenter.z - m_laserHalfWidth,
                     kLaneHalfWidth + 0.70f,
                     m_attackCenter.z + m_laserHalfWidth, 0.03f, warnColor);
    }
  } else if (m_attack == AttackType::SanctuarySeal) {
    const XMFLOAT3 curvedCenter =
        CurvePoint(m_attackCenter.x, m_attackCenter.y, m_attackCenter.z);
    const float gatherRadius = m_attackRadius * std::lerp(1.0f, 0.28f, chargeT);
    const XMFLOAT4 sanctuaryBorder = m_phase == AttackPhase::Telegraph
                                         ? XMFLOAT4{0.30f, 1.0f, 0.86f, 1.0f}
                                         : XMFLOAT4{0.82f, 1.0f, 0.96f, 1.0f};
    const XMFLOAT4 sanctuaryInner{0.48f, 0.90f, 1.0f, 0.90f};
    PushThickCircle(frame, curvedCenter, m_attackRadius, sanctuaryBorder, 4);
    PushCircle(frame, curvedCenter, gatherRadius, sanctuaryInner);
    PushCircle(frame, curvedCenter, m_attackRadius * 0.62f, sanctuaryBorder);
    PushCircle(frame, curvedCenter, m_attackRadius * 0.22f, sanctuaryInner);
  }
}

void BossArenaScene::UpdatePhoneOverlay(float dt, const Input &input) {
  m_mirrorMessageTimer = std::max(0.0f, m_mirrorMessageTimer - dt);
  m_mirrorPickupToastTimer = std::max(0.0f, m_mirrorPickupToastTimer - dt);
  m_memoryFailTimer = std::max(0.0f, m_memoryFailTimer - dt);

  if (m_phoneOpen && m_mirrorPuzzleReady && !m_mirrorPuzzleSolved) {
    if (m_mirrorPuzzleType == MirrorPuzzleType::ReflectionTrace) {
      if (m_reflectionTraceState == ReflectionTraceState::Tracing) {
        m_memoryTimer = std::max(0.0f, m_memoryTimer - dt);
        if (m_memoryTimer <= 0.0f)
          FailReflectionTrace(ReflectionTraceFailReason::Timeout);
      }
    } else {
      m_memoryTimer = std::max(0.0f, m_memoryTimer - dt);
      if (m_memoryTimer <= 0.0f)
        FailMirrorPuzzle();
    }
  }

  const bool spaceNow = input.IsKeyDown(VK_SPACE);
  if (spaceNow && !m_phoneSpaceWasDown) {
    if (m_phoneOpen) {
      if (m_mirrorPuzzleType == MirrorPuzzleType::ReflectionTrace &&
          m_reflectionTraceState == ReflectionTraceState::Tracing) {
        FailReflectionTrace(ReflectionTraceFailReason::PhoneClosed);
      } else {
        m_phoneOpen = false;
      }
    } else if (m_mirrorCharge >= kMirrorChargeMax) {
      if (!m_mirrorPuzzleReady)
        PrepareMirrorPuzzle(m_attack);
      m_phoneOpen = true;
    } else {
      m_mirrorMessageTimer = 0.85f;
    }
  }
  m_phoneSpaceWasDown = spaceNow;

  const float target = m_phoneOpen ? 1.0f : 0.0f;
  const float t = std::clamp(dt * 11.0f, 0.0f, 1.0f);
  m_phoneSlide += (target - m_phoneSlide) * t;
  if (!m_phoneOpen && m_phoneSlide < 0.002f)
    m_phoneSlide = 0.0f;
}

BossArenaScene::ReflectionTraceVariant
BossArenaScene::ReflectionTraceVariantForAttack(AttackType attack) const {
  switch (attack) {
  case AttackType::MeteorAoE:
    return ReflectionTraceVariant::MeteorCurve;
  case AttackType::LaserLine:
    return ReflectionTraceVariant::LaserZigzag;
  case AttackType::SanctuarySeal:
    return ReflectionTraceVariant::SanctuaryLoop;
  }
  return ReflectionTraceVariant::MeteorCurve;
}

void BossArenaScene::ResetMirrorPuzzleSchedule() {
  ++m_battlePuzzleResetSerial;
  if (m_debugFixedPuzzleSeed) {
    m_battlePuzzleSeed = std::max(1u, static_cast<uint32_t>(m_debugPuzzleSeed));
  } else {
    const uint64_t ticks = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const uint32_t timeBits =
        static_cast<uint32_t>(ticks) ^ static_cast<uint32_t>(ticks >> 32);
    m_battlePuzzleSeed =
        MixPuzzleSeed(timeBits ^ (m_battlePuzzleResetSerial * 0x9E3779B9u));
    if (m_battlePuzzleSeed == 0)
      m_battlePuzzleSeed = 1;
  }

  m_phaseOnePuzzleBag = {MirrorPuzzleType::SymbolMemory,
                         MirrorPuzzleType::NumberPosition,
                         MirrorPuzzleType::ReflectionTrace};
  for (int i = 0; i < static_cast<int>(m_phaseOnePuzzleBag.size()); ++i) {
    const int remaining = static_cast<int>(m_phaseOnePuzzleBag.size()) - i;
    const int offset = std::min(
        remaining - 1,
        static_cast<int>(PuzzleScheduleRandom01(100u + i * 17u) * remaining));
    std::swap(m_phaseOnePuzzleBag[i], m_phaseOnePuzzleBag[i + offset]);
  }

  m_phaseTwoPuzzleBag = {MirrorPuzzleType::ReflectionTrace,
                         MirrorPuzzleType::SymbolMemory};
  m_phaseOnePuzzleBagIndex = 0;
  m_phaseTwoPuzzleBagIndex = 0;
  m_phaseTwoPuzzleBagReady = false;
  m_hasPreviousMirrorPuzzle = false;
  m_previousMirrorPuzzleType = MirrorPuzzleType::SymbolMemory;
  m_pendingMirrorPuzzle = false;
  m_pendingMirrorPuzzlePhaseTwo = false;
  m_pendingMirrorPuzzleType = MirrorPuzzleType::SymbolMemory;
  m_pendingMirrorAttack = AttackType::MeteorAoE;
  m_pendingReflectionTraceVariant = ReflectionTraceVariant::MeteorCurve;
  m_pendingMirrorPuzzleSeed = 0;
}

void BossArenaScene::PreparePhaseTwoPuzzleBag() {
  const MirrorPuzzleType legacy = PuzzleScheduleRandom01(310u) < 0.5f
                                      ? MirrorPuzzleType::SymbolMemory
                                      : MirrorPuzzleType::NumberPosition;
  m_phaseTwoPuzzleBag = {MirrorPuzzleType::ReflectionTrace, legacy};
  if (PuzzleScheduleRandom01(340u) < 0.5f)
    std::swap(m_phaseTwoPuzzleBag[0], m_phaseTwoPuzzleBag[1]);

  // Phase 境界でも同じ family が連続しないよう、必要な場合だけ先頭を交換する。
  if (m_hasPreviousMirrorPuzzle &&
      m_phaseTwoPuzzleBag[0] == m_previousMirrorPuzzleType) {
    std::swap(m_phaseTwoPuzzleBag[0], m_phaseTwoPuzzleBag[1]);
  }
  m_phaseTwoPuzzleBagIndex = 0;
  m_phaseTwoPuzzleBagReady = true;
}

void BossArenaScene::AdvanceMirrorPuzzleSchedule() {
  if (!m_pendingMirrorPuzzle)
    return;

  m_previousMirrorPuzzleType = m_pendingMirrorPuzzleType;
  m_hasPreviousMirrorPuzzle = true;
  if (m_pendingMirrorPuzzlePhaseTwo)
    ++m_phaseTwoPuzzleBagIndex;
  else
    ++m_phaseOnePuzzleBagIndex;
  m_pendingMirrorPuzzle = false;
}

void BossArenaScene::PrepareMirrorPuzzle(AttackType attack) {
  if (m_mirrorCharge < kMirrorChargeMax)
    return;

  const bool phaseTwo = m_bossHp <= 2;
  if (!m_pendingMirrorPuzzle) {
    if (phaseTwo && !m_phaseTwoPuzzleBagReady)
      PreparePhaseTwoPuzzleBag();

    const int bagIndex = phaseTwo ? std::clamp(m_phaseTwoPuzzleBagIndex, 0, 1)
                                  : std::clamp(m_phaseOnePuzzleBagIndex, 0, 2);
    m_pendingMirrorPuzzleType = phaseTwo ? m_phaseTwoPuzzleBag[bagIndex]
                                         : m_phaseOnePuzzleBag[bagIndex];
    m_pendingMirrorPuzzle = true;
    m_pendingMirrorPuzzlePhaseTwo = phaseTwo;
    m_pendingMirrorAttack = attack;
    m_pendingMirrorPuzzleSeed = MixPuzzleSeed(
        m_battlePuzzleSeed ^ (phaseTwo ? 0xA511E9B3u : 0x63D83595u) ^
        (static_cast<uint32_t>(bagIndex + 1) * 0x9E3779B9u));

    if (m_pendingMirrorPuzzleType == MirrorPuzzleType::ReflectionTrace) {
      if (phaseTwo) {
        m_pendingReflectionTraceVariant =
            PuzzleScheduleRandom01(410u +
                                   static_cast<uint32_t>(bagIndex) * 29u) < 0.5f
                ? ReflectionTraceVariant::PhaseTwoBranch
                : ReflectionTraceVariant::PhaseTwoReversed;
      } else {
        m_pendingReflectionTraceVariant =
            ReflectionTraceVariantForAttack(m_pendingMirrorAttack);
      }
    }
  }

  m_mirrorAttack = m_pendingMirrorAttack;
  m_mirrorPuzzleType = m_pendingMirrorPuzzleType;
  if (m_mirrorPuzzleType == MirrorPuzzleType::ReflectionTrace) {
    PrepareReflectionTracePuzzle(m_pendingMirrorAttack,
                                 m_pendingReflectionTraceVariant);
    return;
  }

  ResetReflectionTrace();
  m_mirrorPuzzleReady = true;
  m_mirrorPuzzleSolved = false;
  m_mirrorMessageTimer = 0.0f;
  m_memoryTimer =
      m_mirrorPuzzleType == MirrorPuzzleType::NumberPosition ? 7.2f : 6.0f;
  m_memoryFailTimer = 0.0f;
  m_memoryInputIndex = 0;

  for (int i = 0; i < 3; ++i) {
    const int base = m_pendingMirrorAttack == AttackType::LaserLine ? 1 : 0;
    const int shift = static_cast<int>(MirrorRandom01(800 + i * 13) * 3.0f);
    m_memorySequence[i] = (base + m_countersUsed + i + shift) % 3;
  }

  std::array<int, 3> pool = {1, 2, 3};
  for (int i = 0; i < 3; ++i) {
    const int swapIndex = i + static_cast<int>(MirrorRandom01(900 + i * 23) *
                                               static_cast<float>(3 - i));
    std::swap(pool[i], pool[std::clamp(swapIndex, i, 2)]);
  }
  m_numberTargets = pool;
  m_numberButtonOrder = {1, 2, 3};
  for (int i = 0; i < 3; ++i) {
    const int swapIndex = i + static_cast<int>(MirrorRandom01(980 + i * 29) *
                                               static_cast<float>(3 - i));
    std::swap(m_numberButtonOrder[i],
              m_numberButtonOrder[std::clamp(swapIndex, i, 2)]);
  }
}

void BossArenaScene::ResetReflectionTrace() {
  m_reflectionTracePattern = {};
  m_reflectionTraceState = ReflectionTraceState::Idle;
  m_reflectionTraceFailReason = ReflectionTraceFailReason::None;
  m_reflectionTracePosition = {};
  m_reflectionTracePreviousPosition = {};
  m_reflectionTraceTrail = {};
  m_reflectionTraceTrailCount = 0;
  m_reflectionTraceNextNode = 0;
}

void BossArenaScene::PrepareReflectionTracePattern(
    ReflectionTraceVariant variant) {
  m_reflectionTracePattern = {};
  const bool phaseTwo = variant == ReflectionTraceVariant::PhaseTwoBranch ||
                        variant == ReflectionTraceVariant::PhaseTwoReversed;
  m_reflectionTracePattern.roadHalfWidth = phaseTwo ? 0.055f : 0.07f;
  m_reflectionTracePattern.dropletRadius = 0.018f;
  m_reflectionTracePattern.startEndRadius =
      m_reflectionTracePattern.roadHalfWidth + 0.012f;
  m_reflectionTracePattern.nodeRadius = phaseTwo ? 0.038f : 0.042f;
  m_reflectionTracePattern.timeLimit = phaseTwo ? 6.5f : 8.0f;
  m_reflectionTracePattern.reversed = false;

  const auto setRoad = [this](const XMFLOAT2 *points, int pointCount) {
    m_reflectionTracePattern.roadSegmentCount = std::clamp(
        pointCount - 1, 0,
        static_cast<int>(m_reflectionTracePattern.roadSegments.size()));
    for (int i = 0; i < m_reflectionTracePattern.roadSegmentCount; ++i)
      m_reflectionTracePattern.roadSegments[i] = {points[i], points[i + 1]};
    m_reflectionTracePattern.start = points[0];
    m_reflectionTracePattern.end = points[pointCount - 1];
  };

  switch (variant) {
  case ReflectionTraceVariant::MeteorCurve: {
    constexpr std::array<XMFLOAT2, 7> points = {
        XMFLOAT2{0.66f, 0.91f}, XMFLOAT2{0.43f, 0.88f}, XMFLOAT2{0.27f, 0.76f},
        XMFLOAT2{0.20f, 0.58f}, XMFLOAT2{0.24f, 0.38f}, XMFLOAT2{0.42f, 0.22f},
        XMFLOAT2{0.66f, 0.11f}};
    setRoad(points.data(), static_cast<int>(points.size()));
    m_reflectionTracePattern.nodes = {points[2], points[4], points[5], {}};
    m_reflectionTracePattern.nodeCount = 3;
    break;
  }
  case ReflectionTraceVariant::LaserZigzag: {
    constexpr std::array<XMFLOAT2, 7> points = {
        XMFLOAT2{0.25f, 0.91f}, XMFLOAT2{0.70f, 0.80f}, XMFLOAT2{0.30f, 0.65f},
        XMFLOAT2{0.72f, 0.49f}, XMFLOAT2{0.30f, 0.33f}, XMFLOAT2{0.70f, 0.18f},
        XMFLOAT2{0.48f, 0.09f}};
    setRoad(points.data(), static_cast<int>(points.size()));
    m_reflectionTracePattern.nodes = {points[1], points[3], points[5], {}};
    m_reflectionTracePattern.nodeCount = 3;
    break;
  }
  case ReflectionTraceVariant::SanctuaryLoop: {
    constexpr std::array<XMFLOAT2, 9> points = {
        XMFLOAT2{0.38f, 0.91f}, XMFLOAT2{0.22f, 0.78f}, XMFLOAT2{0.16f, 0.55f},
        XMFLOAT2{0.24f, 0.31f}, XMFLOAT2{0.44f, 0.17f}, XMFLOAT2{0.68f, 0.22f},
        XMFLOAT2{0.82f, 0.43f}, XMFLOAT2{0.78f, 0.68f}, XMFLOAT2{0.62f, 0.82f}};
    setRoad(points.data(), static_cast<int>(points.size()));
    m_reflectionTracePattern.nodes = {points[2], points[4], points[6], {}};
    m_reflectionTracePattern.nodeCount = 3;
    break;
  }
  case ReflectionTraceVariant::PhaseTwoBranch: {
    // 自己交差点では道路の形だけでなく node 順が正しい出口を示す。
    constexpr std::array<XMFLOAT2, 7> points = {
        XMFLOAT2{0.50f, 0.92f}, XMFLOAT2{0.22f, 0.72f}, XMFLOAT2{0.78f, 0.42f},
        XMFLOAT2{0.78f, 0.72f}, XMFLOAT2{0.22f, 0.42f}, XMFLOAT2{0.30f, 0.20f},
        XMFLOAT2{0.50f, 0.08f}};
    setRoad(points.data(), static_cast<int>(points.size()));
    m_reflectionTracePattern.nodes = {points[1], points[2], points[4],
                                      points[5]};
    m_reflectionTracePattern.nodeCount = 4;
    break;
  }
  case ReflectionTraceVariant::PhaseTwoReversed: {
    constexpr std::array<XMFLOAT2, 9> points = {
        XMFLOAT2{0.50f, 0.92f}, XMFLOAT2{0.25f, 0.82f}, XMFLOAT2{0.18f, 0.66f},
        XMFLOAT2{0.38f, 0.55f}, XMFLOAT2{0.72f, 0.50f}, XMFLOAT2{0.82f, 0.34f},
        XMFLOAT2{0.67f, 0.22f}, XMFLOAT2{0.42f, 0.16f}, XMFLOAT2{0.50f, 0.08f}};
    setRoad(points.data(), static_cast<int>(points.size()));
    m_reflectionTracePattern.nodes = {points[1], points[3], points[5],
                                      points[7]};
    m_reflectionTracePattern.nodeCount = 4;
    m_reflectionTracePattern.reversed = true;
    std::swap(m_reflectionTracePattern.start, m_reflectionTracePattern.end);
    std::reverse(m_reflectionTracePattern.nodes.begin(),
                 m_reflectionTracePattern.nodes.begin() +
                     m_reflectionTracePattern.nodeCount);
    break;
  }
  }

  m_reflectionTracePosition = m_reflectionTracePattern.start;
  m_reflectionTracePreviousPosition = m_reflectionTracePattern.start;
  m_reflectionTraceTrailCount = 0;
  m_reflectionTraceNextNode = 0;
  m_reflectionTraceState = ReflectionTraceState::Ready;
  m_reflectionTraceFailReason = ReflectionTraceFailReason::None;
}

void BossArenaScene::PrepareReflectionTracePuzzle(
    AttackType attack, ReflectionTraceVariant variant) {
  if (m_mirrorCharge < kMirrorChargeMax)
    return;

  ResetReflectionTrace();
  m_mirrorAttack = attack;
  m_pendingReflectionTraceVariant = variant;
  m_mirrorPuzzleType = MirrorPuzzleType::ReflectionTrace;
  m_mirrorPuzzleReady = true;
  m_mirrorPuzzleSolved = false;
  m_mirrorMessageTimer = 0.0f;
  m_memoryFailTimer = 0.0f;
  m_memoryInputIndex = 0;
  PrepareReflectionTracePattern(variant);
  m_memoryTimer = m_reflectionTracePattern.timeLimit;
}

void BossArenaScene::BeginReflectionTrace(const XMFLOAT2 &position) {
  if (m_reflectionTraceState != ReflectionTraceState::Ready)
    return;

  m_reflectionTraceState = ReflectionTraceState::Tracing;
  m_reflectionTraceFailReason = ReflectionTraceFailReason::None;
  m_reflectionTracePosition = position;
  m_reflectionTracePreviousPosition = position;
  m_reflectionTraceTrailCount = 1;
  m_reflectionTraceTrail[0] = position;
  m_reflectionTraceNextNode = 0;
  m_memoryTimer = m_reflectionTracePattern.timeLimit;
}

float BossArenaScene::DistancePointToTraceSegment(
    const XMFLOAT2 &point, const ReflectionTraceSegment &segment,
    float puzzleWidth, float puzzleHeight) const {
  const float px = point.x * puzzleWidth;
  const float py = point.y * puzzleHeight;
  const float ax = segment.a.x * puzzleWidth;
  const float ay = segment.a.y * puzzleHeight;
  const float bx = segment.b.x * puzzleWidth;
  const float by = segment.b.y * puzzleHeight;
  const float abx = bx - ax;
  const float aby = by - ay;
  const float lengthSq = abx * abx + aby * aby;
  const float projection =
      lengthSq > 0.0001f
          ? std::clamp(((px - ax) * abx + (py - ay) * aby) / lengthSq, 0.0f,
                       1.0f)
          : 0.0f;
  const float closestX = ax + abx * projection;
  const float closestY = ay + aby * projection;
  const float dx = px - closestX;
  const float dy = py - closestY;
  return std::sqrt(dx * dx + dy * dy);
}

bool BossArenaScene::IsTracePointInsideRoad(const XMFLOAT2 &position,
                                            float puzzleWidth,
                                            float puzzleHeight) const {
  if (position.x < 0.0f || position.x > 1.0f || position.y < 0.0f ||
      position.y > 1.0f) {
    return false;
  }

  const float shortSide = std::max(1.0f, std::min(puzzleWidth, puzzleHeight));
  const float safeRadius =
      std::max(0.0f, m_reflectionTracePattern.roadHalfWidth -
                         m_reflectionTracePattern.dropletRadius) *
      shortSide;
  for (int i = 0; i < m_reflectionTracePattern.roadSegmentCount; ++i) {
    if (DistancePointToTraceSegment(position,
                                    m_reflectionTracePattern.roadSegments[i],
                                    puzzleWidth, puzzleHeight) <= safeRadius) {
      return true;
    }
  }
  return false;
}

void BossArenaScene::AdvanceReflectionTrace(const XMFLOAT2 &position,
                                            float puzzleWidth,
                                            float puzzleHeight) {
  if (m_reflectionTraceState != ReflectionTraceState::Tracing)
    return;

  const XMFLOAT2 from = m_reflectionTracePosition;
  const float moveX = (position.x - from.x) * puzzleWidth;
  const float moveY = (position.y - from.y) * puzzleHeight;
  const float moveDistance = std::sqrt(moveX * moveX + moveY * moveY);
  const float shortSide = std::max(1.0f, std::min(puzzleWidth, puzzleHeight));
  const float maxStep = std::max(1.0f, m_reflectionTracePattern.dropletRadius *
                                           shortSide * 0.50f);
  const int steps =
      std::max(1, static_cast<int>(std::ceil(moveDistance / maxStep)));

  const auto distancePixels = [puzzleWidth, puzzleHeight](const XMFLOAT2 &a,
                                                          const XMFLOAT2 &b) {
    const float dx = (a.x - b.x) * puzzleWidth;
    const float dy = (a.y - b.y) * puzzleHeight;
    return std::sqrt(dx * dx + dy * dy);
  };
  const float nodeRadius = m_reflectionTracePattern.nodeRadius * shortSide;
  const float endRadius = m_reflectionTracePattern.startEndRadius * shortSide;

  for (int step = 1; step <= steps; ++step) {
    const float t = static_cast<float>(step) / static_cast<float>(steps);
    const XMFLOAT2 sample{std::lerp(from.x, position.x, t),
                          std::lerp(from.y, position.y, t)};
    if (!IsTracePointInsideRoad(sample, puzzleWidth, puzzleHeight)) {
      FailReflectionTrace(ReflectionTraceFailReason::EdgeContact);
      return;
    }

    for (int node = m_reflectionTraceNextNode;
         node < m_reflectionTracePattern.nodeCount; ++node) {
      if (distancePixels(sample, m_reflectionTracePattern.nodes[node]) >
          nodeRadius) {
        continue;
      }
      if (node != m_reflectionTraceNextNode) {
        FailReflectionTrace(ReflectionTraceFailReason::WrongNode);
        return;
      }
      ++m_reflectionTraceNextNode;
      break;
    }

    if (distancePixels(sample, m_reflectionTracePattern.end) <= endRadius) {
      m_reflectionTracePosition = sample;
      if (m_reflectionTraceNextNode < m_reflectionTracePattern.nodeCount) {
        FailReflectionTrace(ReflectionTraceFailReason::EndBeforeNodes);
        return;
      }
      m_reflectionTraceState = ReflectionTraceState::Succeeded;
      CompleteMirrorPuzzle();
      return;
    }
  }

  m_reflectionTracePreviousPosition = m_reflectionTracePosition;
  m_reflectionTracePosition = position;
  if (m_reflectionTraceTrailCount >=
      static_cast<int>(m_reflectionTraceTrail.size())) {
    int writeIndex = 0;
    for (int readIndex = 0; readIndex < m_reflectionTraceTrailCount;
         readIndex += 2) {
      m_reflectionTraceTrail[writeIndex++] = m_reflectionTraceTrail[readIndex];
    }
    m_reflectionTraceTrailCount = writeIndex;
  }
  m_reflectionTraceTrail[m_reflectionTraceTrailCount++] = position;
}

void BossArenaScene::ReleaseReflectionTrace() {
  if (m_reflectionTraceState == ReflectionTraceState::Tracing)
    FailReflectionTrace(ReflectionTraceFailReason::EarlyRelease);
}

void BossArenaScene::FailReflectionTrace(ReflectionTraceFailReason reason) {
  if (m_reflectionTraceState == ReflectionTraceState::Failed ||
      m_reflectionTraceState == ReflectionTraceState::Succeeded) {
    return;
  }
  m_reflectionTraceState = ReflectionTraceState::Failed;
  m_reflectionTraceFailReason = reason;
  FailMirrorPuzzle();
}

void BossArenaScene::CompleteMirrorPuzzle() {
  if (!m_mirrorPuzzleReady)
    return;

  AdvanceMirrorPuzzleSchedule();
  m_mirrorPuzzleReady = false;
  m_mirrorPuzzleSolved = true;
  m_mirrorMessageTimer = 1.4f;
  m_memoryTimer = 0.0f;
  m_memoryFailTimer = 0.0f;
  m_mirrorCharge = 0;
  m_memoryInputIndex = 0;
  m_phoneOpen = false;
  const int previousBossHp = m_bossHp;
  m_bossHp = std::max(0, m_bossHp - 1);
  if (previousBossHp > 2 && m_bossHp <= 2) {
    m_phaseShiftVfxTimer = 1.25f;
    // Phase 2 の開始直後に新ルールを提示し、旧攻撃だけが続く間を作らない。
    m_phaseTwoIntroPending = true;
    m_phaseTwoPatternIndex = 0;
    PreparePhaseTwoPuzzleBag();
  }
  ++m_countersUsed;
  const bool climaxVfx = !m_techShowcaseOverride || m_showcaseClimaxVfx;
  m_counterVfxTimer = climaxVfx ? 0.92f : 0.68f;
  m_counterPetalTimer = 7.2f;
  m_bossHitShakeTimer = 0.62f;

  m_impactTimer = 0.55f;
  m_impactMaxRadius = 5.4f;
  m_impactPosition = {0.0f, 0.07f, kArenaHalfExtent - 3.5f};
  if (m_counterSparkBurst) {
    const XMFLOAT3 sparkPos = CurvePoint(0.0f, 1.35f, kArenaHalfExtent - 3.5f);
    m_counterSparkBurst->Fire(
        XMVectorSet(sparkPos.x, sparkPos.y, sparkPos.z, 0.0f));
  }
  constexpr float petalZs[4] = {-13.0f, -4.0f, 5.0f, 13.0f};
  for (int i = 0; i < 4; ++i) {
    if (!m_counterPetalEmitters[i])
      continue;
    const XMFLOAT3 petalPos = CurvePoint(0.0f, 8.6f, petalZs[i]);
    m_counterPetalEmitters[i]->Fire(
        XMVectorSet(petalPos.x, petalPos.y, petalPos.z, 0.0f));
  }

  if (m_bossHp <= 0)
    m_cleared = true;
  else
    SpawnMirrorCharges();
}

void BossArenaScene::FailMirrorPuzzle() {
  if (!m_mirrorPuzzleReady && m_mirrorCharge <= 0)
    return;

  m_mirrorPuzzleReady = false;
  m_mirrorPuzzleSolved = false;
  m_mirrorMessageTimer = 1.05f;
  m_memoryTimer = 0.0f;
  m_memoryFailTimer = 0.85f;
  m_mirrorCharge = 0;
  m_memoryInputIndex = 0;
  m_phoneOpen = false;
  SpawnMirrorCharges();
}

float BossArenaScene::PuzzleScheduleRandom01(uint32_t salt) const {
  const uint32_t x = MixPuzzleSeed(m_battlePuzzleSeed ^ (salt * 1597334677u));
  return static_cast<float>(x & 0x00FFFFFFu) / 16777215.0f;
}

float BossArenaScene::MirrorRandom01(uint32_t salt) const {
  uint32_t x =
      m_pendingMirrorPuzzle
          ? m_pendingMirrorPuzzleSeed
          : MixPuzzleSeed(
                m_battlePuzzleSeed ^
                (static_cast<uint32_t>(m_attackIndex + 1) * 747796405u) ^
                (static_cast<uint32_t>(m_bossHp + 3) * 2891336453u));
  x ^= static_cast<uint32_t>(m_mirrorAttack) * 277803737u;
  x = MixPuzzleSeed(x ^ (salt * 1597334677u));
  return static_cast<float>(x & 0x00FFFFFFu) / 16777215.0f;
}

float BossArenaScene::TelegraphDuration() const {
  const bool phase2 = m_bossHp <= 2;
  if (m_attack == AttackType::MeteorAoE)
    return phase2 ? 1.32f : 1.8f;
  if (m_attack == AttackType::LaserLine)
    return phase2 ? 1.22f : 1.65f;
  if (m_attack == AttackType::SanctuarySeal)
    return m_sanctuaryIntroActive ? 2.10f : 1.55f;
  return 1.6f;
}

void BossArenaScene::SpawnMirrorCharges() {
  constexpr float kLaneXs[3] = {-2.65f, 0.0f, 2.65f};
  constexpr float kBaseZs[3] = {-14.2f, -11.8f, -9.4f};
  for (int i = 0; i < kMirrorChargeMax; ++i) {
    const int lane = (m_attackIndex + i * 2) % 3;
    const float jitterX = (MirrorRandom01(510 + i * 17) - 0.5f) * 0.58f;
    const float jitterZ = (MirrorRandom01(620 + i * 19) - 0.5f) * 1.35f;
    m_mirrorChargePositions[i] = {kLaneXs[lane] + jitterX, 0.45f,
                                  kBaseZs[i] + jitterZ};
    m_mirrorChargeActive[i] = i >= m_mirrorCharge;
  }
}

void BossArenaScene::UpdateMirrorCharges(const PlayerAnimationPreview &player) {
  if (m_mirrorCharge >= kMirrorChargeMax)
    return;

  const XMFLOAT3 playerPos = player.Position();
  constexpr float pickupRadius = 1.05f;
  bool collected = false;
  for (int i = 0; i < kMirrorChargeMax; ++i) {
    if (!m_mirrorChargeActive[i])
      continue;
    if (DistanceSq2D(playerPos, m_mirrorChargePositions[i]) >
        pickupRadius * pickupRadius)
      continue;

    m_mirrorChargeActive[i] = false;
    m_mirrorCharge = std::min(kMirrorChargeMax, m_mirrorCharge + 1);
    m_mirrorMessageTimer = 0.72f;
    m_mirrorPickupToastTimer = 0.72f;
    m_mirrorPickupVfxTimer = 0.72f;
    m_lastMirrorPickupPosition = m_mirrorChargePositions[i];
    if (m_mirrorPickupBurst) {
      const XMFLOAT3 burstPosition =
          CurvePoint(m_lastMirrorPickupPosition.x, 0.62f,
                     m_lastMirrorPickupPosition.z);
      m_mirrorPickupBurst->Fire(XMVectorSet(
          burstPosition.x, burstPosition.y, burstPosition.z, 0.0f));
    }
    m_memoryFailTimer = 0.0f;
    collected = true;
    break;
  }

  if (collected && m_mirrorCharge >= kMirrorChargeMax &&
      !m_mirrorPuzzleReady && !m_mirrorPuzzleSolved) {
    PrepareMirrorPuzzle(m_attack);
  }
}

void BossArenaScene::AppendMirrorCharges(FrameData &frame) const {
  const float time = frame.gameTime;
  for (int i = 0; i < kMirrorChargeMax; ++i) {
    if (!m_mirrorChargeActive[i])
      continue;

    const XMFLOAT3 pos = m_mirrorChargePositions[i];
    const float bob = 0.12f * std::sin(time * 4.8f + static_cast<float>(i));
    const float pulse =
        1.0f + 0.12f * SmoothPulse(time + static_cast<float>(i) * 0.3f, 5.8f);
    frame.transparentItems.push_back(
        {m_mirrorChargeMeshId,
         CurveWorld(0.72f * pulse, 0.72f * pulse, 0.72f * pulse, pos.x,
                    pos.y + bob, pos.z)});

    GPUPointLight chargeLight{};
    chargeLight.position = CurvePoint(pos.x, 1.1f + bob, pos.z);
    chargeLight.range = 4.8f;
    chargeLight.color = {0.30f, 1.0f, 0.82f};
    chargeLight.intensity = 2.9f;
    frame.pointLights.push_back(chargeLight);
  }
}

void BossArenaScene::DrawReflectionTracePuzzle(
    ImDrawList *draw, float puzzleMinX, float puzzleMinY, float puzzleMaxX,
    float puzzleMaxY, float screenAlpha) {
  if (!draw)
    return;

  const ImVec2 puzzleMin(puzzleMinX, puzzleMinY);
  const ImVec2 puzzleMax(puzzleMaxX, puzzleMaxY);
  const ImVec2 traceMin(puzzleMin.x + 22.0f, puzzleMin.y + 74.0f);
  const ImVec2 traceMax(puzzleMax.x - 22.0f, puzzleMax.y - 30.0f);
  const float traceWidth = std::max(1.0f, traceMax.x - traceMin.x);
  const float traceHeight = std::max(1.0f, traceMax.y - traceMin.y);
  const float shortSide = std::max(1.0f, std::min(traceWidth, traceHeight));

  const auto toScreen = [traceMin, traceWidth,
                         traceHeight](const XMFLOAT2 &position) {
    return ImVec2(traceMin.x + position.x * traceWidth,
                  traceMin.y + position.y * traceHeight);
  };
  const auto distancePixels = [traceWidth, traceHeight](const XMFLOAT2 &a,
                                                        const XMFLOAT2 &b) {
    const float dx = (a.x - b.x) * traceWidth;
    const float dy = (a.y - b.y) * traceHeight;
    return std::sqrt(dx * dx + dy * dy);
  };

  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const XMFLOAT2 mouseUv{(mouse.x - traceMin.x) / traceWidth,
                         (mouse.y - traceMin.y) / traceHeight};
  const bool interactive = m_phoneOpen && screenAlpha > 0.72f;
  if (interactive && m_reflectionTraceState == ReflectionTraceState::Ready &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      distancePixels(mouseUv, m_reflectionTracePattern.start) <=
          m_reflectionTracePattern.startEndRadius * shortSide) {
    BeginReflectionTrace(m_reflectionTracePattern.start);
  }
  if (interactive && m_reflectionTraceState == ReflectionTraceState::Tracing) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      AdvanceReflectionTrace(mouseUv, traceWidth, traceHeight);
    } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      ReleaseReflectionTrace();
    }
  }

  draw->AddText(ImVec2(puzzleMin.x + 20.0f, puzzleMin.y + 16.0f),
                Rgba(0.78f, 1.0f, 0.94f, screenAlpha), "REFLECTION TRACE");
  const char *traceInstruction =
      m_reflectionTracePattern.reversed
          ? "HOLD LMB: END > 4 > 3 > 2 > 1 > START"
          : (m_reflectionTracePattern.nodeCount == 4
                 ? "HOLD LMB: START > 1 > 2 > 3 > 4 > END"
                 : "HOLD LMB: START > 1 > 2 > 3 > END");
  draw->AddText(ImVec2(puzzleMin.x + 20.0f, puzzleMin.y + 39.0f),
                Rgba(0.52f, 0.76f, 0.72f, 0.92f * screenAlpha),
                traceInstruction);

  const float timerRate = std::clamp(
      m_memoryTimer / std::max(0.01f, m_reflectionTracePattern.timeLimit), 0.0f,
      1.0f);
  const bool urgent = m_reflectionTraceState == ReflectionTraceState::Tracing &&
                      timerRate < 0.34f;
  const ImVec2 timerMin(puzzleMin.x + 20.0f, puzzleMin.y + 61.0f);
  const ImVec2 timerMax(puzzleMax.x - 20.0f, puzzleMin.y + 69.0f);
  draw->AddRectFilled(timerMin, timerMax,
                      Rgba(0.03f, 0.10f, 0.11f, 0.90f * screenAlpha), 4.0f);
  draw->AddRectFilled(
      timerMin,
      ImVec2(timerMin.x + (timerMax.x - timerMin.x) * timerRate, timerMax.y),
      urgent ? Rgba(1.0f, 0.34f, 0.14f, 0.96f * screenAlpha)
             : Rgba(0.34f, 1.0f, 0.84f, 0.94f * screenAlpha),
      4.0f);

  const bool succeeded =
      m_reflectionTraceState == ReflectionTraceState::Succeeded;
  const bool failed = m_reflectionTraceState == ReflectionTraceState::Failed;
  const float roadWidth =
      m_reflectionTracePattern.roadHalfWidth * 2.0f * shortSide;
  const float dropletRadius =
      m_reflectionTracePattern.dropletRadius * shortSide;
  const ImU32 edgeColor =
      failed
          ? Rgba(1.0f, 0.22f, 0.08f, 0.90f * screenAlpha)
          : Rgba(0.24f, 0.92f, 1.0f, (succeeded ? 0.94f : 0.72f) * screenAlpha);
  const ImU32 roadColor = Rgba(0.008f, 0.045f, 0.052f, 0.99f * screenAlpha);
  const auto strokeRoad = [&](ImU32 color, float thickness) {
    if (m_reflectionTracePattern.roadSegmentCount <= 0)
      return;
    draw->PathClear();
    draw->PathLineTo(toScreen(m_reflectionTracePattern.roadSegments[0].a));
    for (int i = 0; i < m_reflectionTracePattern.roadSegmentCount; ++i)
      draw->PathLineTo(toScreen(m_reflectionTracePattern.roadSegments[i].b));
    draw->PathStroke(color, ImDrawFlags_None, thickness);
  };
  strokeRoad(edgeColor, roadWidth + 3.0f);
  strokeRoad(roadColor, roadWidth);

  const float startEndRadius =
      m_reflectionTracePattern.startEndRadius * shortSide;
  const ImVec2 startCenter = toScreen(m_reflectionTracePattern.start);
  const ImVec2 endCenter = toScreen(m_reflectionTracePattern.end);
  draw->AddCircleFilled(startCenter, startEndRadius,
                        Rgba(0.04f, 0.24f, 0.23f, 0.96f * screenAlpha), 48);
  draw->AddCircle(startCenter, startEndRadius,
                  Rgba(0.44f, 1.0f, 0.84f, screenAlpha), 48, 2.2f);
  draw->AddCircleFilled(endCenter, startEndRadius,
                        Rgba(0.10f, 0.13f, 0.17f, 0.96f * screenAlpha), 48);
  draw->AddCircle(endCenter, startEndRadius,
                  Rgba(0.46f, 0.86f, 1.0f, screenAlpha), 48, 2.2f);
  const char *startLabel = m_reflectionTracePattern.reversed ? "END" : "START";
  const char *endLabel = m_reflectionTracePattern.reversed ? "START" : "END";
  const ImVec2 startLabelSize = ImGui::CalcTextSize(startLabel);
  const ImVec2 endLabelSize = ImGui::CalcTextSize(endLabel);
  draw->AddText(ImVec2(startCenter.x - startLabelSize.x * 0.5f,
                       startCenter.y - startLabelSize.y * 0.5f),
                Rgba(0.82f, 1.0f, 0.92f, screenAlpha), startLabel);
  draw->AddText(ImVec2(endCenter.x - endLabelSize.x * 0.5f,
                       endCenter.y - endLabelSize.y * 0.5f),
                Rgba(0.80f, 0.94f, 1.0f, screenAlpha), endLabel);

  for (int i = 0; i < m_reflectionTracePattern.nodeCount; ++i) {
    const ImVec2 center = toScreen(m_reflectionTracePattern.nodes[i]);
    const bool completed = i < m_reflectionTraceNextNode;
    const bool active = i == m_reflectionTraceNextNode && !failed;
    const float pulse =
        active ? SmoothPulse(static_cast<float>(ImGui::GetTime()) + i * 0.2f,
                             5.4f, 0.0f)
               : 0.0f;
    const ImU32 nodeColor =
        completed ? Rgba(0.38f, 1.0f, 0.72f, screenAlpha)
                  : (active ? Rgba(0.70f, 1.0f, 0.94f,
                                   (0.82f + pulse * 0.18f) * screenAlpha)
                            : Rgba(0.34f, 0.64f, 0.64f, 0.74f * screenAlpha));
    char nodeLabel[4]{};
    const int visibleNode = m_reflectionTracePattern.reversed
                                ? m_reflectionTracePattern.nodeCount - i
                                : i + 1;
    std::snprintf(nodeLabel, sizeof(nodeLabel), "%d", visibleNode);
    const ImVec2 labelSize = ImGui::CalcTextSize(nodeLabel);
    const ImVec2 labelPosition(center.x - labelSize.x * 0.5f,
                               center.y - labelSize.y * 0.5f);
    draw->AddText(ImVec2(labelPosition.x + 1.0f, labelPosition.y + 1.0f),
                  Rgba(0.0f, 0.02f, 0.025f, 0.96f * screenAlpha), nodeLabel);
    draw->AddText(labelPosition, nodeColor, nodeLabel);
  }

  if (m_reflectionTraceTrailCount > 1) {
    for (int i = 1; i < m_reflectionTraceTrailCount; ++i) {
      draw->AddLine(toScreen(m_reflectionTraceTrail[i - 1]),
                    toScreen(m_reflectionTraceTrail[i]),
                    Rgba(0.66f, 1.0f, 0.92f, 0.92f * screenAlpha), 3.2f);
    }
  }

  if (m_reflectionTraceState != ReflectionTraceState::Idle) {
    const ImVec2 droplet = toScreen(m_reflectionTracePosition);
    const ImU32 dropletColor = failed ? Rgba(1.0f, 0.22f, 0.08f, screenAlpha)
                                      : Rgba(0.78f, 1.0f, 0.96f, screenAlpha);
    draw->AddCircleFilled(droplet, dropletRadius + 4.0f,
                          Rgba(0.20f, 0.86f, 1.0f, 0.24f * screenAlpha), 28);
    draw->AddCircleFilled(droplet, dropletRadius, dropletColor, 28);
    draw->AddCircle(droplet, dropletRadius + 1.5f,
                    Rgba(0.36f, 1.0f, 0.88f, screenAlpha), 28, 1.4f);
  }

  if (failed && m_memoryFailTimer > 0.0f) {
    const char *reason = "TRACE BROKEN";
    switch (m_reflectionTraceFailReason) {
    case ReflectionTraceFailReason::EdgeContact:
      reason = "EDGE CONTACT";
      break;
    case ReflectionTraceFailReason::WrongNode:
      reason = "WRONG NODE";
      break;
    case ReflectionTraceFailReason::EarlyRelease:
      reason = "RELEASED EARLY";
      break;
    case ReflectionTraceFailReason::Timeout:
      reason = "TIME OUT";
      break;
    case ReflectionTraceFailReason::EndBeforeNodes:
      reason = "NODES INCOMPLETE";
      break;
    case ReflectionTraceFailReason::PhoneClosed:
      reason = "TRACE INTERRUPTED";
      break;
    default:
      break;
    }
    const ImVec2 reasonSize = ImGui::CalcTextSize(reason);
    draw->AddRectFilled(
        ImVec2(traceMin.x + 10.0f, traceMin.y + traceHeight * 0.46f - 16.0f),
        ImVec2(traceMax.x - 10.0f, traceMin.y + traceHeight * 0.46f + 22.0f),
        Rgba(0.30f, 0.01f, 0.005f, 0.82f * screenAlpha), 7.0f);
    draw->AddText(
        ImVec2(traceMin.x + (traceWidth - reasonSize.x) * 0.5f,
               traceMin.y + traceHeight * 0.46f - reasonSize.y * 0.5f),
        Rgba(1.0f, 0.42f, 0.20f, screenAlpha), reason);
  }
}

void BossArenaScene::DrawPhoneOverlay(int viewportWidth, int viewportHeight) {
  const float vw = static_cast<float>(viewportWidth);
  const float vh = static_cast<float>(viewportHeight);
  if (vw <= 0.0f || vh <= 0.0f)
    return;

  const float eased = EaseOutCubic(m_phoneSlide);
  const float phoneWidth = std::clamp(vw * 0.22f, 300.0f, 430.0f);
  const float phoneHeight = phoneWidth * 1.92f;
  const float phoneX = (vw - phoneWidth) * 0.5f;
  const float closedY = vh - 42.0f;
  const float openY = (vh - phoneHeight) * 0.50f;
  const float phoneY = closedY + (openY - closedY) * eased;
  const float rounding = phoneWidth * 0.105f;

  const float hitHeight = m_phoneOpen ? 76.0f : 92.0f;
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool phoneHeadHovered =
      mouse.x >= phoneX && mouse.x <= phoneX + phoneWidth &&
      mouse.y >= phoneY && mouse.y <= phoneY + hitHeight;
  if (!m_failed && !m_cleared && phoneHeadHovered &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (m_phoneOpen) {
      if (m_mirrorPuzzleType == MirrorPuzzleType::ReflectionTrace &&
          m_reflectionTraceState == ReflectionTraceState::Tracing) {
        FailReflectionTrace(ReflectionTraceFailReason::PhoneClosed);
      } else {
        m_phoneOpen = false;
      }
    } else if (m_mirrorCharge >= kMirrorChargeMax) {
      if (!m_mirrorPuzzleReady)
        PrepareMirrorPuzzle(m_attack);
      m_phoneOpen = true;
    } else {
      m_mirrorMessageTimer = 0.85f;
    }
  }

  ImDrawList *draw = ImGui::GetForegroundDrawList();
  if (m_phoneSlide > 0.02f) {
    draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(vw, vh),
                        Rgba(0.00f, 0.035f, 0.050f, 0.50f * eased));
    const ImVec2 veilCenter(vw * 0.5f, vh * 0.54f);
    for (int i = 0; i < 5; ++i) {
      const float radius = 160.0f + static_cast<float>(i) * 92.0f +
                           std::sin(static_cast<float>(ImGui::GetTime()) *
                                        0.8f +
                                    static_cast<float>(i)) *
                               12.0f;
      DrawArc(draw, veilCenter, radius, 0.20f + i * 0.38f,
              2.92f + i * 0.32f, Rgba(0.18f, 0.90f, 1.0f, 0.08f * eased),
              1.4f);
      DrawArc(draw, veilCenter, radius + 24.0f, 3.22f + i * 0.26f,
              6.04f + i * 0.18f, Rgba(0.36f, 1.0f, 0.78f, 0.055f * eased),
              1.2f);
    }
  }

  const ImVec2 outerMin(phoneX, phoneY);
  const ImVec2 outerMax(phoneX + phoneWidth, phoneY + phoneHeight);
  draw->AddRectFilled(ImVec2(phoneX + 12.0f, phoneY + 18.0f),
                      ImVec2(phoneX + phoneWidth + 12.0f,
                             phoneY + phoneHeight + 18.0f),
                      Rgba(0.0f, 0.0f, 0.0f, 0.28f), rounding);
  draw->AddRectFilled(outerMin, outerMax, Rgba(0.006f, 0.030f, 0.040f, 1.0f),
                      rounding);
  draw->AddRect(outerMin, outerMax, Rgba(0.34f, 1.0f, 0.88f, 0.62f),
                rounding, 0, 2.0f);
  draw->AddRect(ImVec2(outerMin.x - 5.0f, outerMin.y - 5.0f),
                ImVec2(outerMax.x + 5.0f, outerMax.y + 5.0f),
                Rgba(0.24f, 0.88f, 1.0f, 0.18f * eased), rounding + 5.0f,
                0, 2.4f);

  const float bezel = phoneWidth * 0.055f;
  const ImVec2 screenMin(phoneX + bezel, phoneY + bezel * 1.75f);
  const ImVec2 screenMax(phoneX + phoneWidth - bezel,
                         phoneY + phoneHeight - bezel * 1.25f);
  draw->AddRectFilled(screenMin, screenMax, Rgba(0.012f, 0.064f, 0.075f, 1.0f),
                      rounding * 0.54f);

  const ImVec2 notchMin(phoneX + phoneWidth * 0.37f, phoneY + bezel * 0.72f);
  const ImVec2 notchMax(phoneX + phoneWidth * 0.63f, phoneY + bezel * 1.30f);
  draw->AddRectFilled(notchMin, notchMax, Rgba(0.0f, 0.0f, 0.0f, 1.0f),
                      phoneWidth * 0.03f);

  const float screenAlpha = std::clamp((eased - 0.18f) / 0.82f, 0.0f, 1.0f);
  if (screenAlpha > 0.0f) {
    const float gridAlpha = 0.16f * screenAlpha;
    for (int i = 1; i < 6; ++i) {
      const float x = screenMin.x + (screenMax.x - screenMin.x) * i / 6.0f;
      draw->AddLine(ImVec2(x, screenMin.y + 58.0f),
                    ImVec2(x, screenMax.y - 44.0f),
                    Rgba(0.42f, 0.78f, 0.72f, gridAlpha), 1.0f);
    }
    for (int i = 1; i < 9; ++i) {
      const float y = screenMin.y + 58.0f +
                      (screenMax.y - screenMin.y - 102.0f) * i / 9.0f;
      draw->AddLine(ImVec2(screenMin.x + 24.0f, y),
                    ImVec2(screenMax.x - 24.0f, y),
                    Rgba(0.42f, 0.78f, 0.72f, gridAlpha), 1.0f);
    }

    draw->AddText(ImVec2(screenMin.x + 24.0f, screenMin.y + 24.0f),
                  Rgba(0.76f, 0.92f, 0.88f, screenAlpha), "MIZUKAGAMI");
    draw->AddText(ImVec2(screenMin.x + 24.0f, screenMin.y + 50.0f),
                  Rgba(0.46f, 0.66f, 0.62f, screenAlpha), "REVERSE SCRIPT");

    const ImVec2 puzzleMin(screenMin.x + 30.0f, screenMin.y + 102.0f);
    const ImVec2 puzzleMax(screenMax.x - 30.0f, screenMax.y - 76.0f);
    const ImVec2 puzzleCenter((puzzleMin.x + puzzleMax.x) * 0.5f,
                              (puzzleMin.y + puzzleMax.y) * 0.5f);
    const float mirrorTime = static_cast<float>(ImGui::GetTime());
    const float mirrorPulse = SmoothPulse(mirrorTime, 3.4f, 0.32f);
    const bool showReflectionTrace =
        m_mirrorPuzzleType == MirrorPuzzleType::ReflectionTrace &&
        (m_mirrorPuzzleReady ||
         m_reflectionTraceState == ReflectionTraceState::Succeeded ||
         m_reflectionTraceState == ReflectionTraceState::Failed);
    draw->AddRectFilled(
        puzzleMin, puzzleMax,
        Rgba(0.012f, 0.082f, 0.092f,
             (showReflectionTrace ? 0.94f : 0.24f) * screenAlpha),
        10.0f);
    draw->AddRect(puzzleMin, puzzleMax,
                  Rgba(0.55f, 0.95f, 0.82f, 0.62f * screenAlpha), 10.0f, 0,
                  1.6f);
    if (!showReflectionTrace) {
      for (int i = 0; i < 5; ++i) {
        const float ringRadius =
            (puzzleMax.x - puzzleMin.x) * (0.14f + i * 0.072f) +
            std::sin(mirrorTime * 1.2f + static_cast<float>(i)) * 4.0f;
        DrawArc(draw, puzzleCenter, ringRadius,
                mirrorTime * (0.20f + i * 0.025f) + i * 0.68f,
                mirrorTime * (0.20f + i * 0.025f) + i * 0.68f + 4.35f,
                Rgba(0.32f, 1.0f, 0.86f,
                     (0.13f + 0.04f * mirrorPulse) * screenAlpha),
                1.4f);
      }
      constexpr int kMirrorStrands = 9;
      for (int i = 0; i < kMirrorStrands; ++i) {
        const float t =
            static_cast<float>(i) / static_cast<float>(kMirrorStrands - 1);
        const float y = std::lerp(puzzleMin.y + 36.0f, puzzleMax.y - 38.0f, t);
        const float wave = std::sin(mirrorTime * 2.1f + t * 8.4f);
        draw->AddLine(ImVec2(puzzleMin.x + 28.0f, y + wave * 4.0f),
                      ImVec2(puzzleMax.x - 28.0f, y - wave * 4.0f),
                      Rgba(0.18f, 0.78f, 1.0f, 0.065f * screenAlpha), 1.0f);
      }
    }
    if (m_mirrorMessageTimer > 0.0f || m_memoryFailTimer > 0.0f) {
      const bool failPulse = m_memoryFailTimer > 0.0f;
      const float fxTimer =
          failPulse ? m_memoryFailTimer : m_mirrorMessageTimer;
      const float fxAlpha =
          std::clamp(fxTimer / (failPulse ? 0.85f : 1.4f), 0.0f, 1.0f) *
          screenAlpha;
      const ImU32 fxColor = failPulse
                                ? Rgba(1.0f, 0.12f, 0.06f, 0.42f * fxAlpha)
                                : Rgba(0.34f, 1.0f, 0.86f, 0.36f * fxAlpha);
      draw->AddRectFilled(puzzleMin, puzzleMax,
                          failPulse
                              ? Rgba(0.34f, 0.015f, 0.010f, 0.20f * fxAlpha)
                              : Rgba(0.020f, 0.24f, 0.20f, 0.16f * fxAlpha),
                          10.0f);
      for (int i = 0; i < 4; ++i) {
        const float radius = (puzzleMax.x - puzzleMin.x) * (0.16f + i * 0.09f) +
                             (1.0f - fxAlpha) * 24.0f;
        DrawArc(draw, puzzleCenter, radius, i * 0.72f,
                i * 0.72f + (failPulse ? 2.1f : 5.4f), fxColor,
                failPulse ? 2.4f : 1.8f);
      }
      if (failPulse) {
        for (int i = 0; i < 7; ++i) {
          const float t = static_cast<float>(i) / 6.0f;
          const float x =
              std::lerp(puzzleMin.x + 18.0f, puzzleMax.x - 18.0f, t);
          const float y = puzzleCenter.y +
                          std::sin(t * 10.0f + static_cast<float>(i)) * 42.0f;
          draw->AddLine(ImVec2(x, y), ImVec2(puzzleCenter.x, puzzleCenter.y),
                        Rgba(1.0f, 0.18f, 0.08f, 0.34f * fxAlpha), 1.6f);
        }
      }
    }
    if (!showReflectionTrace) {
      const ImU32 glyphColor = Rgba(0.58f, 0.95f, 0.86f, 0.46f * screenAlpha);
      if (m_mirrorAttack == AttackType::MeteorAoE) {
        const float r = (puzzleMax.x - puzzleMin.x) * 0.24f;
        DrawArc(draw, puzzleCenter, r, 0.15f, 1.35f, glyphColor, 3.0f);
        DrawArc(draw, puzzleCenter, r, 1.70f, 3.05f, glyphColor, 3.0f);
        DrawArc(draw, puzzleCenter, r, 3.42f, 5.88f, glyphColor, 3.0f);
        draw->AddLine(ImVec2(puzzleCenter.x - r * 0.72f, puzzleCenter.y),
                      ImVec2(puzzleCenter.x + r * 0.72f, puzzleCenter.y),
                      glyphColor, 1.8f);
        draw->AddLine(ImVec2(puzzleCenter.x, puzzleCenter.y - r * 0.72f),
                      ImVec2(puzzleCenter.x, puzzleCenter.y + r * 0.72f),
                      glyphColor, 1.8f);
      } else if (m_mirrorAttack == AttackType::LaserLine) {
        const ImVec2 a(puzzleMin.x + 48.0f, puzzleMin.y + 58.0f);
        const ImVec2 b(puzzleMax.x - 42.0f, puzzleMax.y - 62.0f);
        draw->AddLine(a, b, glyphColor, 5.0f);
        draw->AddLine(ImVec2(a.x + 34.0f, a.y + 30.0f),
                      ImVec2(b.x - 42.0f, b.y - 28.0f), glyphColor, 1.8f);
        draw->AddLine(ImVec2(a.x + 76.0f, a.y - 12.0f),
                      ImVec2(a.x + 104.0f, a.y + 36.0f), glyphColor, 1.8f);
        draw->AddLine(ImVec2(b.x - 92.0f, b.y - 34.0f),
                      ImVec2(b.x - 54.0f, b.y + 10.0f), glyphColor, 1.8f);
      } else {
        const float maxR = (puzzleMax.x - puzzleMin.x) * 0.31f;
        DrawArc(draw, puzzleCenter, maxR * 0.36f, 0.0f, XM_2PI, glyphColor,
                2.0f);
        DrawArc(draw, puzzleCenter, maxR * 0.62f, 0.0f, XM_2PI, glyphColor,
                2.0f);
        DrawArc(draw, puzzleCenter, maxR, 0.0f, XM_2PI, glyphColor, 2.0f);
      }
    }

    if (showReflectionTrace) {
      DrawReflectionTracePuzzle(draw, puzzleMin.x, puzzleMin.y, puzzleMax.x,
                                puzzleMax.y, screenAlpha);
    } else if (m_mirrorPuzzleReady) {
      const bool numberGame =
          m_mirrorPuzzleType == MirrorPuzzleType::NumberPosition;
      const float puzzleDuration = numberGame ? 7.2f : 6.0f;
      const float timerRate =
          std::clamp(m_memoryTimer / puzzleDuration, 0.0f, 1.0f);
      const bool urgent = timerRate < 0.34f;
      const float urgentPulse =
          urgent ? SmoothPulse(static_cast<float>(ImGui::GetTime()), 8.0f, 0.0f)
                 : 0.0f;
      if (urgent) {
        draw->AddRect(ImVec2(puzzleMin.x - 5.0f - 3.0f * urgentPulse,
                             puzzleMin.y - 5.0f - 3.0f * urgentPulse),
                      ImVec2(puzzleMax.x + 5.0f + 3.0f * urgentPulse,
                             puzzleMax.y + 5.0f + 3.0f * urgentPulse),
                      Rgba(1.0f, 0.28f, 0.10f,
                           (0.28f + 0.34f * urgentPulse) * screenAlpha),
                      12.0f, 0, 2.0f);
      }
      const ImVec2 timerMin(puzzleMin.x + 26.0f, puzzleMin.y + 24.0f);
      const ImVec2 timerMax(puzzleMax.x - 26.0f, puzzleMin.y + 34.0f);
      draw->AddRectFilled(timerMin, timerMax,
                          Rgba(0.05f, 0.11f, 0.12f, 0.84f * screenAlpha), 4.0f);
      draw->AddRectFilled(
          timerMin,
          ImVec2(timerMin.x + (timerMax.x - timerMin.x) * timerRate,
                 timerMax.y),
          urgent ? Rgba(1.0f, 0.36f, 0.16f, 0.96f * screenAlpha)
                 : Rgba(0.35f, 1.0f, 0.86f, 0.92f * screenAlpha),
          4.0f);
      if (urgent) {
        char urgentText[24]{};
        std::snprintf(urgentText, sizeof(urgentText), "%.1f",
                      std::max(0.0f, m_memoryTimer));
        const ImVec2 urgentSize = ImGui::CalcTextSize(urgentText);
        draw->AddText(ImVec2(timerMax.x - urgentSize.x, timerMax.y + 8.0f),
                      Rgba(1.0f, 0.55f, 0.28f, screenAlpha), urgentText);
      }
      draw->AddText(ImVec2(puzzleMin.x + 26.0f, puzzleMin.y + 48.0f),
                    Rgba(0.74f, 0.94f, 0.90f, screenAlpha),
                    numberGame ? "POSITION TRACE" : "MEMORY TRACE");

      const float seqY = puzzleMin.y + 88.0f;
      const float stepW = (puzzleMax.x - puzzleMin.x - 72.0f) / 3.0f;
      std::array<ImVec2, 3> traceNodeCenters{};
      if (numberGame) {
        draw->AddText(ImVec2(puzzleMin.x + 26.0f, seqY - 22.0f),
                      Rgba(0.50f, 0.72f, 0.68f, screenAlpha), "POSITION MAP");
        for (int i = 0; i < 3; ++i) {
          const ImVec2 boxMin(puzzleMin.x + 26.0f + stepW * i + 10.0f, seqY);
          const ImVec2 boxMax(boxMin.x + stepW - 20.0f, seqY + 48.0f);
          draw->AddRectFilled(boxMin, boxMax,
                              Rgba(0.035f, 0.07f, 0.075f, 0.92f * screenAlpha),
                              7.0f);
          draw->AddRect(boxMin, boxMax,
                        Rgba(0.36f, 1.0f, 0.84f, 0.58f * screenAlpha), 7.0f, 0,
                        1.4f);
          char mapLabel[24]{};
          std::snprintf(mapLabel, sizeof(mapLabel), "POS %d", i + 1);
          draw->AddText(ImVec2(boxMin.x + 10.0f, boxMin.y + 7.0f),
                        Rgba(0.54f, 0.74f, 0.70f, 0.86f * screenAlpha),
                        mapLabel);
          char valueLabel[4]{};
          std::snprintf(valueLabel, sizeof(valueLabel), "%d",
                        m_numberTargets[i]);
          const ImVec2 valueSize = ImGui::CalcTextSize(valueLabel);
          draw->AddText(ImVec2((boxMin.x + boxMax.x - valueSize.x) * 0.5f,
                               boxMin.y + 23.0f),
                        Rgba(0.88f, 1.0f, 0.94f, screenAlpha), valueLabel);
        }

        const float targetY = seqY + 74.0f;
        draw->AddText(ImVec2(puzzleMin.x + 26.0f, targetY - 22.0f),
                      Rgba(0.50f, 0.72f, 0.68f, screenAlpha), "TARGET ORDER");
        for (int i = 0; i < 3; ++i) {
          const ImVec2 boxMin(puzzleMin.x + 26.0f + stepW * i + 10.0f, targetY);
          const ImVec2 boxMax(boxMin.x + stepW - 20.0f, targetY + 42.0f);
          traceNodeCenters[i] = ImVec2((boxMin.x + boxMax.x) * 0.5f,
                                       (boxMin.y + boxMax.y) * 0.5f);
          const bool solved = i < m_memoryInputIndex;
          const bool active = i == m_memoryInputIndex;
          draw->AddRectFilled(
              boxMin, boxMax,
              solved
                  ? Rgba(0.10f, 0.38f, 0.32f, 0.92f * screenAlpha)
                  : (active ? Rgba(0.09f, 0.18f, 0.18f, 0.94f * screenAlpha)
                            : Rgba(0.04f, 0.08f, 0.09f, 0.88f * screenAlpha)),
              7.0f);
          draw->AddRect(
              boxMin, boxMax,
              Rgba(0.36f, 1.0f, 0.84f, (active ? 0.92f : 0.48f) * screenAlpha),
              7.0f, 0, active ? 2.0f : 1.2f);
          char targetLabel[24]{};
          std::snprintf(targetLabel, sizeof(targetLabel), "FIND %d",
                        m_numberButtonOrder[i]);
          const ImVec2 labelSize = ImGui::CalcTextSize(targetLabel);
          draw->AddText(ImVec2((boxMin.x + boxMax.x - labelSize.x) * 0.5f,
                               boxMin.y + 13.0f),
                        active ? Rgba(0.88f, 1.0f, 0.94f, screenAlpha)
                               : Rgba(0.58f, 0.78f, 0.74f, screenAlpha),
                        targetLabel);
        }
      } else {
        for (int i = 0; i < 3; ++i) {
          const ImVec2 boxMin(puzzleMin.x + 26.0f + stepW * i + 10.0f, seqY);
          const ImVec2 boxMax(boxMin.x + stepW - 20.0f, seqY + 56.0f);
          traceNodeCenters[i] = ImVec2((boxMin.x + boxMax.x) * 0.5f,
                                       (boxMin.y + boxMax.y) * 0.5f);
          const bool solved = i < m_memoryInputIndex;
          const bool active = i == m_memoryInputIndex;
          draw->AddRectFilled(
              boxMin, boxMax,
              solved
                  ? Rgba(0.10f, 0.38f, 0.32f, 0.92f * screenAlpha)
                  : (active ? Rgba(0.08f, 0.18f, 0.19f, 0.94f * screenAlpha)
                            : Rgba(0.04f, 0.08f, 0.09f, 0.90f * screenAlpha)),
              7.0f);
          const ImU32 borderColor = MirrorSymbolColor(
              m_memorySequence[i], (active ? 0.94f : 0.76f) * screenAlpha);
          draw->AddRect(boxMin, boxMax, borderColor, 7.0f, 0,
                        active ? 2.2f : 1.6f);

          const char *label = MirrorSymbolLabel(m_memorySequence[i]);
          const ImVec2 labelSize = ImGui::CalcTextSize(label);
          draw->AddText(ImVec2((boxMin.x + boxMax.x - labelSize.x) * 0.5f,
                               boxMin.y + 18.0f),
                        MirrorSymbolColor(m_memorySequence[i], screenAlpha),
                        label);
        }
      }

      const float buttonY = puzzleMax.y - 84.0f;
      const float buttonW = (puzzleMax.x - puzzleMin.x - 70.0f) / 3.0f;
      const ImVec2 mousePos = ImGui::GetIO().MousePos;
      const bool mouseClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
      std::array<ImVec2, 3> buttonCenters{};
      for (int button = 0; button < 3; ++button) {
        const ImVec2 buttonMin(puzzleMin.x + 26.0f + button * (buttonW + 9.0f),
                               buttonY);
        const ImVec2 buttonMax(buttonMin.x + buttonW, buttonY + 52.0f);
        buttonCenters[button] = ImVec2((buttonMin.x + buttonMax.x) * 0.5f,
                                       (buttonMin.y + buttonMax.y) * 0.5f);
      }
      for (int step = 0; step < 3; ++step) {
        int expectedButton = m_memorySequence[step];
        if (numberGame) {
          const int targetNumber = m_numberButtonOrder[step];
          expectedButton = 0;
          for (int i = 0; i < 3; ++i) {
            if (m_numberTargets[i] == targetNumber) {
              expectedButton = i;
              break;
            }
          }
        }
        const bool solved = step < m_memoryInputIndex;
        const bool active = step == m_memoryInputIndex;
        if (!solved && !active)
          continue;
        const float tracePulse =
            active ? SmoothPulse(static_cast<float>(ImGui::GetTime()) +
                                     static_cast<float>(step) * 0.18f,
                                 5.6f, 0.22f)
                   : 0.55f;
        const ImU32 traceColor =
            solved ? Rgba(0.32f, 1.0f, 0.74f, 0.42f * screenAlpha)
                   : Rgba(0.68f, 1.0f, 0.94f,
                          (0.28f + tracePulse * 0.36f) * screenAlpha);
        const ImVec2 start = traceNodeCenters[step];
        const ImVec2 end = buttonCenters[std::clamp(expectedButton, 0, 2)];
        draw->AddLine(
            start, end,
            Rgba(0.03f, 0.16f, 0.14f, (active ? 0.50f : 0.34f) * screenAlpha),
            active ? 7.0f : 5.0f);
        draw->AddLine(start, end, traceColor, active ? 2.6f : 1.8f);
        draw->AddCircleFilled(start, active ? 4.8f + tracePulse * 2.0f : 3.8f,
                              traceColor, 18);
        draw->AddCircleFilled(end, active ? 5.8f + tracePulse * 2.4f : 4.2f,
                              traceColor, 20);
      }
      for (int button = 0; button < 3; ++button) {
        const bool shortcutPressed =
            ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + button),
                                false) ||
            ImGui::IsKeyPressed(
                static_cast<ImGuiKey>(ImGuiKey_Keypad1 + button), false);
        const int value = numberGame ? button + 1 : button;
        const ImVec2 buttonMin(puzzleMin.x + 26.0f + button * (buttonW + 9.0f),
                               buttonY);
        const ImVec2 buttonMax(buttonMin.x + buttonW, buttonY + 52.0f);
        const bool hovered =
            mousePos.x >= buttonMin.x && mousePos.x <= buttonMax.x &&
            mousePos.y >= buttonMin.y && mousePos.y <= buttonMax.y;
        const ImVec2 buttonCenter = buttonCenters[button];
        const float nodePulse =
            hovered ? 1.0f
                    : SmoothPulse(static_cast<float>(ImGui::GetTime()),
                                  3.8f + static_cast<float>(button), 0.50f);
        draw->AddRectFilled(
            buttonMin, buttonMax,
            hovered ? Rgba(0.08f, 0.28f, 0.28f, 0.90f * screenAlpha)
                    : Rgba(0.015f, 0.075f, 0.082f, 0.82f * screenAlpha),
            10.0f);
        const ImU32 buttonColor =
            numberGame ? Rgba(0.40f, 1.0f, 0.84f, 0.86f * screenAlpha)
                       : MirrorSymbolColor(value, 0.80f * screenAlpha);
        draw->AddRect(buttonMin, buttonMax, buttonColor, 10.0f, 0, 1.5f);
        draw->AddCircle(buttonCenter, 23.0f + nodePulse * 4.0f, buttonColor, 32,
                        1.4f);
        draw->AddCircleFilled(buttonCenter, 5.0f + nodePulse * 1.8f,
                              buttonColor, 20);

        char numberLabel[8]{};
        const char *label = nullptr;
        if (numberGame) {
          std::snprintf(numberLabel, sizeof(numberLabel), "POS %d", value);
          label = numberLabel;
        } else {
          label = MirrorSymbolLabel(value);
        }
        const ImVec2 labelSize = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2((buttonMin.x + buttonMax.x - labelSize.x) * 0.5f,
                             buttonMin.y + 17.0f),
                      numberGame ? Rgba(0.86f, 1.0f, 0.94f, screenAlpha)
                                 : MirrorSymbolColor(value, screenAlpha),
                      label);
        char shortcutLabel[4]{};
        std::snprintf(shortcutLabel, sizeof(shortcutLabel), "%d", button + 1);
        draw->AddText(ImVec2(buttonMin.x + 9.0f, buttonMin.y + 7.0f),
                      Rgba(0.58f, 0.78f, 0.74f, 0.70f * screenAlpha),
                      shortcutLabel);

        if (m_phoneOpen && screenAlpha > 0.72f &&
            ((hovered && mouseClicked) || shortcutPressed)) {
          int expected = m_memorySequence[m_memoryInputIndex];
          if (numberGame) {
            const int targetNumber = m_numberButtonOrder[m_memoryInputIndex];
            expected = 1;
            for (int i = 0; i < 3; ++i) {
              if (m_numberTargets[i] == targetNumber) {
                expected = i + 1;
                break;
              }
            }
          }
          if (value == expected) {
            ++m_memoryInputIndex;
            if (m_memoryInputIndex >= kMirrorChargeMax)
              CompleteMirrorPuzzle();
          } else {
            FailMirrorPuzzle();
          }
        }
      }
    } else {
      const char *message =
          m_memoryFailTimer > 0.0f
              ? "TRACE BROKEN"
              : (m_mirrorMessageTimer > 0.0f ? "COUNTER SENT" : "NO TRACE");
      const char *sub = m_memoryFailTimer > 0.0f
                            ? "Collect mirror charge again."
                            : (m_mirrorMessageTimer > 0.0f
                                   ? "Boss took reflected damage."
                                   : "Collect 3 mirror charges in the arena.");
      const ImU32 messageColor =
          m_memoryFailTimer > 0.0f
              ? Rgba(1.0f, 0.36f, 0.18f, screenAlpha)
              : (m_mirrorMessageTimer > 0.0f
                     ? Rgba(0.42f, 1.0f, 0.84f, screenAlpha)
                     : Rgba(0.76f, 0.92f, 0.88f, screenAlpha));
      draw->AddText(ImVec2(puzzleMin.x + 24.0f, puzzleCenter.y - 18.0f),
                    messageColor, message);
      draw->AddText(ImVec2(puzzleMin.x + 24.0f, puzzleCenter.y + 12.0f),
                    Rgba(0.46f, 0.66f, 0.62f, screenAlpha), sub);
    }

    const char *status =
        m_mirrorPuzzleReady
            ? (m_mirrorPuzzleType == MirrorPuzzleType::ReflectionTrace
                   ? "TRACE THE REFLECTED ROAD"
                   : (m_mirrorPuzzleType == MirrorPuzzleType::NumberPosition
                          ? "INPUT NUMBER POSITIONS"
                          : "INPUT SYMBOL SEQUENCE"))
            : "STANDBY";
    draw->AddText(ImVec2(screenMin.x + 24.0f, screenMax.y - 38.0f),
                  Rgba(0.76f, 0.92f, 0.88f, screenAlpha), status);
  }
}
