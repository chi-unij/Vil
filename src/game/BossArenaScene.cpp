#include "game/BossArenaScene.h"

#include "GltfLoader.h"
#include "MeshRenderer.h"
#include "ProceduralMesh.h"
#include "game/PlayerAnimationPreview.h"

#include <DirectXMath.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <imgui.h>

using namespace DirectX;

namespace {

void PushLine(FrameData &frame, const XMFLOAT3 &a, const XMFLOAT3 &b,
              const XMFLOAT4 &color) {
  frame.debugLines.push_back({a, b, color});
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

float DistanceSq(const ImVec2 &a, const ImVec2 &b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return dx * dx + dy * dy;
}

ImVec2 RectPoint(const ImVec2 &min, const ImVec2 &max,
                 const XMFLOAT2 &uv) {
  return ImVec2(min.x + (max.x - min.x) * uv.x,
                min.y + (max.y - min.y) * uv.y);
}

XMFLOAT2 RectUv(const ImVec2 &min, const ImVec2 &max, const ImVec2 &p) {
  const float w = std::max(1.0f, max.x - min.x);
  const float h = std::max(1.0f, max.y - min.y);
  return {std::clamp((p.x - min.x) / w, 0.0f, 1.0f),
          std::clamp((p.y - min.y) / h, 0.0f, 1.0f)};
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
  const LoadedMesh lanternPostMesh =
      ProceduralMesh::CreateCylinder(0.5f, 1.0f, 12);
  const LoadedMesh lanternGlowMesh = ProceduralMesh::CreateSphere(0.5f, 8, 16);

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
  m_flameMeshId = dx.CreateMeshResources(
      flameCardMesh, {},
      MakeTelegraphMaterial({1.0f, 0.22f, 0.02f, 0.78f},
                            {2.2f, 0.42f, 0.06f}));
  m_pathGlowMeshId = dx.CreateMeshResources(
      telegraphPlane, {},
      MakeTelegraphMaterial({0.18f, 0.78f, 0.72f, 0.20f},
                            {0.08f, 0.42f, 0.38f}));
  m_sideMistMeshId = dx.CreateMeshResources(
      telegraphPlane, {},
      MakeTelegraphMaterial({0.18f, 0.26f, 0.28f, 0.17f},
                            {0.05f, 0.10f, 0.12f}));
  m_bossSealMeshId = dx.CreateMeshResources(
      diskMesh, {},
      MakeTelegraphMaterial({0.95f, 0.12f, 0.04f, 0.30f},
                            {1.2f, 0.18f, 0.05f}));

  Material pathStoneMaterial{};
  pathStoneMaterial.baseColorFactor = {0.28f, 0.30f, 0.28f, 1.0f};
  pathStoneMaterial.roughnessFactor = 0.96f;
  pathStoneMaterial.proceduralTypeId = 7.0f;
  m_pathStoneMeshId = dx.CreateMeshResources(cubeMesh, {}, pathStoneMaterial);

  Material pathEdgeMaterial{};
  pathEdgeMaterial.baseColorFactor = {0.10f, 0.34f, 0.31f, 1.0f};
  pathEdgeMaterial.emissiveFactor = {0.04f, 0.22f, 0.18f};
  pathEdgeMaterial.roughnessFactor = 0.58f;
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
  lanternGlowMaterial.baseColorFactor = {1.0f, 0.48f, 0.18f, 0.62f};
  lanternGlowMaterial.emissiveFactor = {1.4f, 0.42f, 0.08f};
  lanternGlowMaterial.roughnessFactor = 0.24f;
  m_lanternGlowMeshId =
      dx.CreateMeshResources(lanternGlowMesh, {}, lanternGlowMaterial);

  LoadBossArenaModelMeshIds(dx, "Assets/models/japanese_shrine_lantern.glb",
                            m_lanternMeshIds);
  LoadBossArenaModelMeshIds(dx, "Assets/models/japanese_shrine/scene.gltf",
                            m_shrineMeshIds);
  LoadBossArenaModelMeshIds(dx, "Assets/models/torii-gate/source/Test1.glb",
                            m_toriiGateMeshIds);
  LoadBossArenaModelMeshIds(dx, "Assets/models/shrine_gate.glb",
                            m_shrineGateMeshIds);

  m_ready = (m_floorMeshId != UINT32_MAX && m_bossMeshId != UINT32_MAX &&
             m_aoeTelegraphMeshId != UINT32_MAX &&
             m_laserTelegraphMeshId != UINT32_MAX &&
             m_knockbackTelegraphMeshId != UINT32_MAX &&
             m_flameMeshId != UINT32_MAX &&
             m_pathGlowMeshId != UINT32_MAX &&
             m_sideMistMeshId != UINT32_MAX &&
             m_bossSealMeshId != UINT32_MAX &&
             m_pathStoneMeshId != UINT32_MAX &&
             m_pathEdgeMeshId != UINT32_MAX &&
             m_toriiWoodMeshId != UINT32_MAX &&
             m_mossBankMeshId != UINT32_MAX &&
             m_lanternPostMeshId != UINT32_MAX &&
             m_lanternCapMeshId != UINT32_MAX &&
             m_lanternGlowMeshId != UINT32_MAX);
}

void BossArenaScene::Reset(PlayerAnimationPreview &player) {
  m_attack = AttackType::MeteorAoE;
  m_phase = AttackPhase::Telegraph;
  m_attackIndex = 0;
  m_phaseTimer = 1.6f;
  m_resolved = false;
  m_attackCenter = {0.0f, 0.02f, 6.0f};
  m_attackRadius = 4.0f;
  m_laserVertical = true;
  m_playerHp = 3;
  m_bossHp = 5;
  m_surviveTimer = 0.0f;
  m_failed = false;
  m_cleared = false;
  m_counterVfxTimer = 0.0f;
  m_bossHitShakeTimer = 0.0f;
  m_phoneOpen = false;
  m_phoneSlide = 0.0f;
  m_phoneSpaceWasDown = false;
  m_mirrorPuzzleReady = false;
  m_mirrorPuzzleSolved = false;
  m_mirrorMessageTimer = 0.0f;
  m_dragMirrorDot = -1;
  m_mirrorPlaced = {false, false, false};
  player.SetPosition({0.0f, 0.0f, -12.0f});
  player.SetYaw(0.0f);
}

void BossArenaScene::Update(float dt, const Input &input,
                            PlayerAnimationPreview &player) {
  if (input.IsKeyDown('R')) {
    Reset(player);
    return;
  }

  UpdatePhoneOverlay(dt, input);
  if (IsPhoneOverlayActive())
    return;

  if (m_failed || m_cleared)
    return;

  if (!m_debugNoClip && m_knockbackTimer > 0.0f) {
    XMFLOAT3 knockbackPos = player.Position();
    knockbackPos.x += m_knockbackVelocity.x * dt;
    knockbackPos.z += m_knockbackVelocity.z * dt;
    player.SetPosition(knockbackPos);
    m_knockbackTimer = std::max(0.0f, m_knockbackTimer - dt);
  }

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
  m_surviveTimer += dt;
  if (m_surviveTimer >= 45.0f) {
    m_cleared = true;
    return;
  }

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
    m_phaseTimer = 0.76f;
  }

  if (m_phase == AttackPhase::Recovery && m_phaseTimer <= 0.0f) {
    StartNextAttack();
  }
}

void BossArenaScene::BuildFrame(FrameData &frame) const {
  if (!m_ready)
    return;

  frame.opaqueItems.push_back({m_floorMeshId, XMMatrixIdentity()});
  AppendWorldPolish(frame);

  const float bossHitT = std::clamp(m_bossHitShakeTimer / 0.62f, 0.0f, 1.0f);
  const float bossShake =
      bossHitT * 0.72f * std::sin(frame.gameTime * 116.0f);
  const float bossSquash = 1.0f + bossHitT * 0.16f;
  const XMMATRIX bossWorld =
      XMMatrixScaling(2.2f * bossSquash, 3.8f * (1.0f - bossHitT * 0.04f),
                      2.2f * bossSquash) *
      XMMatrixTranslation(bossShake, 1.9f, kArenaHalfExtent - 3.5f);
  frame.opaqueItems.push_back({m_bossMeshId, bossWorld});

  const XMFLOAT4 borderColor{0.55f, 0.75f, 0.72f, 1.0f};
  PushRect(frame, -kArenaHalfExtent, -kArenaHalfExtent, kArenaHalfExtent,
           kArenaHalfExtent, 0.04f, borderColor);

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
          XMMatrixScaling(m_attackRadius * pulse, 1.0f,
                          m_attackRadius * pulse) *
          XMMatrixTranslation(m_attackCenter.x, telegraphY, m_attackCenter.z);
      frame.transparentItems.push_back({m_aoeTelegraphMeshId, world});
    } else if (m_attack == AttackType::LaserLine) {
      const float width = m_laserHalfWidth * 2.0f;
      const float length = kArenaHalfExtent * 2.0f;
      const XMMATRIX world =
          (m_laserVertical ? XMMatrixScaling(width * pulse, 1.0f, length)
                           : XMMatrixScaling(length, 1.0f, width * pulse)) *
          XMMatrixTranslation(0.0f, telegraphY, 0.0f);
      frame.transparentItems.push_back({m_laserTelegraphMeshId, world});
    } else if (m_attack == AttackType::Knockback) {
      const XMMATRIX outerWorld =
          XMMatrixScaling((m_attackRadius + 2.0f) * pulse, 1.0f,
                          (m_attackRadius + 2.0f) * pulse) *
          XMMatrixTranslation(m_attackCenter.x, telegraphY, m_attackCenter.z);
      const XMMATRIX coreWorld =
          XMMatrixScaling(m_attackRadius * pulse, 1.0f,
                          m_attackRadius * pulse) *
          XMMatrixTranslation(m_attackCenter.x, telegraphY + 0.006f,
                              m_attackCenter.z);
      frame.transparentItems.push_back({m_knockbackTelegraphMeshId,
                                        outerWorld});
      frame.transparentItems.push_back({m_knockbackTelegraphMeshId,
                                        coreWorld});
    }
  }

  AppendTelegraphLines(frame);

  if (m_impactTimer > 0.0f) {
    const float impactDuration = m_impactMaxRadius > 7.5f ? 0.78f : 0.58f;
    const float impactLife = std::clamp(m_impactTimer / impactDuration, 0.0f, 1.0f);
    const float impactT = 1.0f - impactLife;
    const float radius = std::lerp(0.25f, m_impactMaxRadius, impactT);
    const XMMATRIX impactWorld = XMMatrixScaling(radius, 1.0f, radius) *
                                  XMMatrixTranslation(m_impactPosition.x,
                                                      m_impactPosition.y,
                                                      m_impactPosition.z);
    frame.transparentItems.push_back({m_knockbackTelegraphMeshId, impactWorld});

    const XMFLOAT4 impactColor{1.0f, 0.68f, 0.10f, impactLife};
    constexpr int kImpactRays = 18;
    for (int i = 0; i < kImpactRays; ++i) {
      const float a = (static_cast<float>(i) / kImpactRays) * XM_2PI;
      const XMFLOAT3 end{m_impactPosition.x + std::cos(a) * radius,
                         0.10f,
                         m_impactPosition.z + std::sin(a) * radius};
      PushLine(frame, m_impactPosition, end, impactColor);
    }

    GPUPointLight impactLight{};
    impactLight.position = {m_impactPosition.x, 1.0f, m_impactPosition.z};
    impactLight.range = radius + 7.0f;
    impactLight.color = {1.0f, 0.38f, 0.08f};
    impactLight.intensity = 10.0f * impactLife;
    frame.pointLights.push_back(impactLight);
  }


  if (m_attack == AttackType::MeteorAoE && m_impactTimer > 0.0f) {
    constexpr int kFlameCount = 28;
    const float burnT = std::clamp(m_impactTimer / 0.58f, 0.0f, 1.0f);
    for (int i = 0; i < kFlameCount; ++i) {
      const float seed = static_cast<float>(i);
      const float angle = seed * 2.39996323f;
      const float lane = static_cast<float>((i % 5) + 1) / 5.5f;
      const float radius = m_attackRadius * lane * (0.55f + 0.08f * (i % 3));
      const float x = m_attackCenter.x + std::cos(angle) * radius;
      const float z = m_attackCenter.z + std::sin(angle) * radius;
      const float flicker = 0.78f + 0.22f * std::sin(frame.gameTime * 18.0f + seed);
      const float width = 0.55f * flicker;
      const float height = (1.35f + 0.55f * static_cast<float>(i % 4)) * burnT;
      const float yaw = angle + frame.gameTime * 2.5f;
      const XMMATRIX base = XMMatrixScaling(width, 1.0f, height) *
                            XMMatrixRotationX(-XM_PIDIV2) *
                            XMMatrixRotationY(yaw) *
                            XMMatrixTranslation(x, 0.10f, z);
      const XMMATRIX cross = XMMatrixScaling(width * 0.82f, 1.0f, height * 0.9f) *
                             XMMatrixRotationX(-XM_PIDIV2) *
                             XMMatrixRotationY(yaw + XM_PIDIV2) *
                             XMMatrixTranslation(x, 0.11f, z);
      frame.transparentItems.push_back({m_flameMeshId, base});
      frame.transparentItems.push_back({m_flameMeshId, cross});
    }
  }
  if (m_hitFlashTimer > 0.0f) {
    const float hitT = std::clamp(m_hitFlashTimer / 0.54f, 0.0f, 1.0f);
    const float ringRadius = std::lerp(1.8f, 0.55f, hitT);
    const XMFLOAT4 hitColor{1.0f, 0.16f, 0.05f, 1.0f};
    PushCircle(frame, m_lastHitPosition, ringRadius, hitColor);
    PushCircle(frame, m_lastHitPosition, ringRadius + 0.35f, hitColor);
  }

  if (m_counterVfxTimer > 0.0f) {
    const float counterT = std::clamp(m_counterVfxTimer / 0.78f, 0.0f, 1.0f);
    const XMFLOAT3 bossBase{0.0f, 0.12f, kArenaHalfExtent - 3.5f};
    const float ringA = std::lerp(1.2f, 10.5f, 1.0f - counterT);
    const float ringB = ringA + 1.15f;
    const XMFLOAT4 mirrorColor{0.36f, 1.0f, 0.86f, counterT};
    PushThickCircle(frame, bossBase, ringA, mirrorColor, 3);
    PushThickCircle(frame, bossBase, ringB, mirrorColor, 2);

    const XMFLOAT4 pathRayColor{0.58f, 1.0f, 0.92f, counterT};
    PushLine(frame, {0.0f, 0.24f, -kArenaHalfExtent + 2.4f},
             {bossBase.x, 0.30f, bossBase.z}, pathRayColor);
    PushLine(frame, {-3.75f, 0.19f, -kArenaHalfExtent + 4.2f},
             {bossBase.x - 0.9f, 0.26f, bossBase.z - 0.55f}, pathRayColor);
    PushLine(frame, {3.75f, 0.19f, -kArenaHalfExtent + 4.2f},
             {bossBase.x + 0.9f, 0.26f, bossBase.z - 0.55f}, pathRayColor);

    constexpr int kCounterRays = 24;
    for (int i = 0; i < kCounterRays; ++i) {
      const float a = (static_cast<float>(i) / kCounterRays) * XM_2PI +
                      frame.gameTime * 2.8f;
      const float inner = 1.2f + (1.0f - counterT) * 1.4f;
      const float outer = 9.4f + (1.0f - counterT) * 3.2f;
      const XMFLOAT3 start{bossBase.x + std::cos(a) * inner, 0.18f,
                           bossBase.z + std::sin(a) * inner};
      const XMFLOAT3 end{bossBase.x + std::cos(a) * outer, 0.20f,
                         bossBase.z + std::sin(a) * outer};
      PushLine(frame, start, end, mirrorColor);
    }

    GPUPointLight counterLight{};
    counterLight.position = {0.0f, 4.0f, kArenaHalfExtent - 3.5f};
    counterLight.range = 20.0f;
    counterLight.color = {0.34f, 1.0f, 0.82f};
    counterLight.intensity = 16.0f * counterT;
    frame.pointLights.push_back(counterLight);
  }

  GPUPointLight bossLight{};
  bossLight.position = {0.0f, 4.5f, kArenaHalfExtent - 3.5f};
  bossLight.range = 16.0f;
  bossLight.color = {1.0f, 0.15f, 0.08f};
  bossLight.intensity = 5.2f + bossHitT * 4.0f;
  frame.pointLights.push_back(bossLight);
}

void BossArenaScene::AppendWorldPolish(FrameData &frame) const {
  constexpr float pathHalfWidth = 4.35f;
  constexpr float pathEdgeWidth = 5.15f;
  constexpr float pathLength = kArenaHalfExtent * 2.0f;
  constexpr float kSides[2] = {-1.0f, 1.0f};
  const float time = frame.gameTime;
  const float scroll = std::fmod(time * 5.4f, pathLength);
  const float pulse = SmoothPulse(time, 3.1f, 0.45f);

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
    return XMMatrixScaling(sx, sy, sz) * XMMatrixRotationY(yaw) *
           XMMatrixTranslation(x, y, z);
  };
  auto addTorii = [&](float x, float z, float scale, float yaw) {
    if (!m_toriiGateMeshIds.empty()) {
      const float modelScale = scale * 3.65f;
      pushMeshGroup(m_toriiGateMeshIds,
                    XMMatrixScaling(modelScale, modelScale, modelScale) *
                        XMMatrixRotationX(XM_PIDIV2) *
                        XMMatrixRotationY(yaw) *
                        XMMatrixTranslation(x, 0.02f, z));
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
    pushTransparent(m_lanternGlowMeshId,
                    XMMatrixScaling(0.36f * scale, 0.36f * scale,
                                    0.36f * scale) *
                        XMMatrixTranslation(x, 1.38f * scale, z));
  };

  for (int i = 0; i < 17; ++i) {
    const float fi = static_cast<float>(i);
    const float z = -16.6f + fi * 2.05f;
    const float width = 3.35f + static_cast<float>(i % 4) * 0.26f;
    const float length = 1.48f + static_cast<float>((i + 1) % 3) * 0.22f;
    const float x = (static_cast<float>((i * 37) % 5) - 2.0f) * 0.13f;
    const float yaw = (static_cast<float>((i * 19) % 7) - 3.0f) * 0.018f;
    pushOpaque(m_pathStoneMeshId,
               cubeWorld(width, 0.12f, length, x, 0.035f, z, yaw));
  }

  pushOpaque(m_pathEdgeMeshId,
             cubeWorld(0.22f, 0.12f, pathLength, -pathEdgeWidth, 0.075f, 0.0f));
  pushOpaque(m_pathEdgeMeshId,
             cubeWorld(0.22f, 0.12f, pathLength, pathEdgeWidth, 0.075f, 0.0f));
  for (float side : kSides) {
    pushOpaque(m_mossBankMeshId,
               cubeWorld(4.4f, 0.075f, pathLength, side * 7.55f, 0.02f,
                         0.0f));
    pushOpaque(m_mossBankMeshId,
               cubeWorld(3.2f, 0.06f, pathLength, side * 13.0f, 0.015f,
                         0.0f));
  }

  frame.transparentItems.push_back(
      {m_pathGlowMeshId,
       XMMatrixScaling(pathHalfWidth * 2.0f, 1.0f, pathLength) *
           XMMatrixTranslation(0.0f, 0.046f, 0.0f)});
  frame.transparentItems.push_back(
      {m_pathGlowMeshId,
       XMMatrixScaling(1.15f + pulse * 0.35f, 1.0f, pathLength) *
           XMMatrixTranslation(0.0f, 0.052f, 0.0f)});

  for (float side : kSides) {
    frame.transparentItems.push_back(
        {m_sideMistMeshId,
         XMMatrixScaling(5.6f, 1.0f, pathLength) *
             XMMatrixTranslation(side * 10.7f, 0.043f, 0.0f)});
    frame.transparentItems.push_back(
        {m_sideMistMeshId,
         XMMatrixScaling(3.2f, 1.0f, pathLength) *
             XMMatrixTranslation(side * 14.4f, 0.049f, 0.0f)});
  }

  const float sealPulse = 1.0f + 0.08f * SmoothPulse(time, 5.8f, 0.0f);
  frame.transparentItems.push_back(
      {m_bossSealMeshId,
       XMMatrixScaling(5.9f * sealPulse, 1.0f, 5.9f * sealPulse) *
           XMMatrixTranslation(0.0f, 0.058f, kArenaHalfExtent - 3.5f)});

  constexpr int kFlowLineCount = 12;
  for (int i = 0; i < kFlowLineCount; ++i) {
    float z = -kArenaHalfExtent +
              std::fmod(scroll + static_cast<float>(i) * 3.0f, pathLength);
    const float centerFade =
        1.0f - std::min(1.0f, std::abs(z) / (kArenaHalfExtent + 0.01f));
    const XMFLOAT4 flowColor{0.36f, 1.0f, 0.88f, 0.18f + centerFade * 0.24f};
    PushLine(frame, {-0.65f, 0.092f, z}, {0.65f, 0.092f, z + 0.55f},
             flowColor);
  }

  for (int i = 0; i < 4; ++i) {
    const float z = -14.0f + static_cast<float>(i) * 8.3f;
    const float scale = 0.84f + static_cast<float>(i % 2) * 0.12f;
    addTorii(-11.5f, z, scale, 0.0f);
    addTorii(11.5f, z + 4.2f, scale * 0.92f, 0.0f);
  }
  if (!m_shrineGateMeshIds.empty()) {
    pushMeshGroup(m_shrineGateMeshIds,
                  XMMatrixScaling(0.24f, 0.24f, 0.24f) *
                      XMMatrixTranslation(0.0f, 0.76f,
                                          kArenaHalfExtent + 2.6f));
  } else {
    addTorii(0.0f, kArenaHalfExtent - 1.2f, 1.92f, 0.0f);
  }

  if (m_shrineGateMeshIds.empty() && !m_shrineMeshIds.empty()) {
    pushMeshGroup(m_shrineMeshIds,
                  XMMatrixScaling(0.034f, 0.034f, 0.034f) *
                      XMMatrixRotationY(XM_PI) *
                      XMMatrixTranslation(0.0f, -0.05f,
                                          kArenaHalfExtent + 5.6f));
  }

  constexpr int kLanternPairs = 3;
  for (int i = 0; i < kLanternPairs; ++i) {
    const float z = -11.0f + static_cast<float>(i) * 10.2f;
    const float lampPulse = 0.85f + 0.15f * std::sin(time * 4.2f + i * 1.7f);
    for (float side : kSides) {
      const float x = side * 5.7f;
      if (!m_lanternMeshIds.empty()) {
        pushMeshGroup(m_lanternMeshIds,
                      XMMatrixScaling(0.72f, 0.72f, 0.72f) *
                          XMMatrixRotationY(side > 0.0f ? -0.22f : 0.22f) *
                          XMMatrixTranslation(x, 0.0f, z));
      } else {
        addProceduralLantern(x, z, 1.0f);
      }
      GPUPointLight lamp{};
      lamp.position = {x, 1.35f, z};
      lamp.range = 5.2f;
      lamp.color = {1.0f, 0.48f, 0.18f};
      lamp.intensity = 1.55f * lampPulse;
      frame.pointLights.push_back(lamp);
    }
  }
}

void BossArenaScene::DrawHud(int viewportWidth, int viewportHeight) {
  const float vw = static_cast<float>(viewportWidth);
  const float vh = static_cast<float>(viewportHeight);
  ImDrawList *draw = ImGui::GetForegroundDrawList();

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
  for (int i = 1; i < 5; ++i) {
    const float x = bossBarMin.x + bossBarWidth * static_cast<float>(i) / 5.0f;
    draw->AddLine(ImVec2(x, bossBarMin.y), ImVec2(x, bossBarMax.y),
                  Rgba(0.02f, 0.015f, 0.014f, 0.72f), 1.4f);
  }

  const float playerPanelX = 24.0f;
  const float playerPanelY = vh - 82.0f;
  draw->AddRectFilled(ImVec2(playerPanelX, playerPanelY),
                      ImVec2(playerPanelX + 188.0f, playerPanelY + 46.0f),
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

  if (!m_failed && !m_cleared && !IsPhoneOverlayActive()) {
    const char *banner =
        m_counterVfxTimer > 0.0f
            ? "MIZUKAGAMI COUNTER"
            : (m_phase == AttackPhase::Telegraph ? "DANGER - READ THE FIELD"
                                                  : "IMPACT");
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
  }

  ImGui::SetNextWindowPos(ImVec2(18.0f, 78.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(250.0f, 0.0f), ImGuiCond_Always);
  ImGui::Begin("##BossArenaHud", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::Text("Boss Arena Debug");
  ImGui::Separator();
  ImGui::Text("Mechanic: %s", AttackName());
  ImGui::Text("Phase: %s",
              m_phase == AttackPhase::Telegraph
                  ? "Telegraph"
                  : (m_phase == AttackPhase::Resolve ? "Resolve" : "Recovery"));
  ImGui::Text("Timer: %.1f", std::max(0.0f, m_phaseTimer));
  ImGui::Text("R: Restart");
  ImGui::Text("Phone: SPACE");
  ImGui::Separator();
  ImGui::Checkbox("No Clip", &m_debugNoClip);
  ImGui::Checkbox("Invisible", &m_debugInvisible);
  if (m_mirrorPuzzleReady)
    ImGui::Text("Mizukagami: READY");
  ImGui::End();

  if (m_failed || m_cleared) {
    const char *text = m_cleared ? "CLEAR" : "FAILED";
    ImGui::SetNextWindowPos(
        ImVec2(static_cast<float>(viewportWidth) * 0.5f,
               static_cast<float>(viewportHeight) * 0.34f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("##BossArenaResult", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(2.2f);
    ImGui::Text("%s", text);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Text("Press R to restart");
    ImGui::End();
  }

  DrawPhoneOverlay(viewportWidth, viewportHeight);
}

void BossArenaScene::StartNextAttack() {
  ++m_attackIndex;
  const int pattern = m_attackIndex % 3;
  m_phase = AttackPhase::Telegraph;
  m_resolved = false;

  if (pattern == 0) {
    m_attack = AttackType::MeteorAoE;
    const float x = (m_attackIndex % 2 == 0) ? -3.75f : 3.75f;
    const float z = -7.0f + static_cast<float>((m_attackIndex * 5) % 16);
    m_attackCenter = {x, 0.02f, z};
    m_attackRadius = 3.85f;
    m_phaseTimer = 1.8f;
  } else if (pattern == 1) {
    m_attack = AttackType::LaserLine;
    m_laserVertical = (m_attackIndex % 2) == 0;
    m_laserHalfWidth = 1.45f;
    m_phaseTimer = 1.65f;
  } else {
    m_attack = AttackType::Knockback;
    m_attackCenter = {0.0f, 0.02f, 0.0f};
    m_attackRadius = 5.0f;
    m_phaseTimer = 2.0f;
  }
}

void BossArenaScene::ResolveAttack(PlayerAnimationPreview &player) {
  m_impactTimer = 0.58f;
  m_impactMaxRadius = (m_attack == AttackType::Knockback) ? 8.2f : 4.4f;
  m_impactPosition = m_attackCenter;
  m_impactPosition.y = 0.07f;

  if (m_attack == AttackType::LaserLine) {
    m_impactPosition = player.Position();
    m_impactPosition.y = 0.07f;
    m_impactMaxRadius = 3.8f;
  }

  if (m_attack == AttackType::Knockback) {
    const XMFLOAT3 playerPos = player.Position();
    const float dangerRadius = m_attackRadius + kPlayerHitRadius;
    if (!m_debugNoClip &&
        DistanceSq2D(playerPos, m_attackCenter) <= dangerRadius * dangerRadius) {
      m_lastHitPosition = playerPos;
      m_lastHitPosition.y = 0.04f;
      m_hitFlashTimer = 0.54f;
      --m_playerHp;
      if (m_playerHp <= 0) {
        m_playerHp = 0;
        m_failed = true;
      }
    }
    if (!m_debugNoClip)
      ApplyKnockback(player);
    if (!m_failed)
      PrepareMirrorPuzzle(m_attack);
    return;
  }

  if (!m_debugNoClip && IsPlayerInCurrentAttack(player.Position())) {
    m_lastHitPosition = player.Position();
    m_lastHitPosition.y = 0.04f;
    m_hitFlashTimer = 0.54f;
    --m_playerHp;
    if (m_playerHp <= 0) {
      m_playerHp = 0;
      m_failed = true;
    }
  }
  if (!m_failed)
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
    return std::abs(playerPos.z) <= m_laserHalfWidth + kPlayerHitRadius;
  }

  return false;
}

void BossArenaScene::ApplyKnockback(PlayerAnimationPreview &player) {
  const XMFLOAT3 pos = player.Position();
  float dx = pos.x - m_attackCenter.x;
  float dz = pos.z - m_attackCenter.z;
  float lenSq = dx * dx + dz * dz;
  if (lenSq <= 0.0001f) {
    dx = 0.0f;
    dz = -1.0f;
    lenSq = 1.0f;
  }

  const float invLen = 1.0f / std::sqrt(lenSq);
  dx *= invLen;
  dz *= invLen;
  constexpr float knockbackSeconds = 0.38f;
  constexpr float knockbackDistance = 9.5f;
  m_knockbackVelocity = {dx * (knockbackDistance / knockbackSeconds), 0.0f,
                         dz * (knockbackDistance / knockbackSeconds)};
  m_knockbackTimer = knockbackSeconds;
}
const char *BossArenaScene::AttackName() const {
  switch (m_attack) {
  case AttackType::MeteorAoE:
    return "Meteor AoE";
  case AttackType::LaserLine:
    return m_laserVertical ? "Laser Column" : "Laser Row";
  case AttackType::Knockback:
    return "Knockback";
  }
  return "Unknown";
}

void BossArenaScene::AppendTelegraphLines(FrameData &frame) const {
  if (m_phase == AttackPhase::Recovery)
    return;

  const float telegraphRemain =
      std::clamp(m_phaseTimer / TelegraphDuration(), 0.0f, 1.0f);
  const float chargeT = 1.0f - telegraphRemain;
  const float scanRadius =
      m_attackRadius * std::lerp(0.35f, 1.0f, chargeT);
  const XMFLOAT4 warnColor =
      m_phase == AttackPhase::Telegraph
          ? XMFLOAT4{1.0f, 0.08f + chargeT * 0.34f, 0.02f, 1.0f}
          : XMFLOAT4{1.0f, 0.92f, 0.18f, 1.0f};
  const XMFLOAT4 innerColor{1.0f, 0.92f, 0.22f, 0.92f};

  if (m_attack == AttackType::MeteorAoE) {
    PushThickCircle(frame, m_attackCenter, m_attackRadius, warnColor, 3);
    PushCircle(frame, m_attackCenter, scanRadius, innerColor);
    PushCircle(frame, m_attackCenter, m_attackRadius * 0.62f, warnColor);
    PushLine(frame, {m_attackCenter.x - m_attackRadius, 0.03f, m_attackCenter.z},
             {m_attackCenter.x + m_attackRadius, 0.03f, m_attackCenter.z},
             warnColor);
    PushLine(frame, {m_attackCenter.x, 0.03f, m_attackCenter.z - m_attackRadius},
             {m_attackCenter.x, 0.03f, m_attackCenter.z + m_attackRadius},
             warnColor);
  } else if (m_attack == AttackType::LaserLine) {
    const float focusWidth = std::max(0.18f, m_laserHalfWidth * (1.0f - chargeT * 0.78f));
    if (m_laserVertical) {
      PushRect(frame, -m_laserHalfWidth, -kArenaHalfExtent, m_laserHalfWidth,
               kArenaHalfExtent, 0.03f, warnColor);
      PushRect(frame, -focusWidth, -kArenaHalfExtent, focusWidth,
               kArenaHalfExtent, 0.045f, innerColor);
      PushLine(frame, {0.0f, 0.055f, -kArenaHalfExtent},
               {0.0f, 0.055f, kArenaHalfExtent}, innerColor);
    } else {
      PushRect(frame, -kArenaHalfExtent, -m_laserHalfWidth, kArenaHalfExtent,
               m_laserHalfWidth, 0.03f, warnColor);
      PushRect(frame, -kArenaHalfExtent, -focusWidth, kArenaHalfExtent,
               focusWidth, 0.045f, innerColor);
      PushLine(frame, {-kArenaHalfExtent, 0.055f, 0.0f},
               {kArenaHalfExtent, 0.055f, 0.0f}, innerColor);
    }
  } else if (m_attack == AttackType::Knockback) {
    PushThickCircle(frame, m_attackCenter, m_attackRadius, warnColor, 3);
    PushThickCircle(frame, m_attackCenter, m_attackRadius + 2.0f, warnColor, 2);
    PushCircle(frame, m_attackCenter, scanRadius + 1.0f, innerColor);
    PushLine(frame, {-m_attackRadius, 0.03f, 0.0f},
             {m_attackRadius, 0.03f, 0.0f}, warnColor);
    PushLine(frame, {0.0f, 0.03f, -m_attackRadius},
             {0.0f, 0.03f, m_attackRadius}, warnColor);
    constexpr int kSpokes = 12;
    for (int i = 0; i < kSpokes; ++i) {
      const float a = (static_cast<float>(i) / kSpokes) * XM_2PI;
      const XMFLOAT3 start{std::cos(a) * (m_attackRadius * 0.35f), 0.05f,
                           std::sin(a) * (m_attackRadius * 0.35f)};
      const XMFLOAT3 end{std::cos(a) * (m_attackRadius + 2.0f), 0.05f,
                         std::sin(a) * (m_attackRadius + 2.0f)};
      PushLine(frame, start, end, innerColor);
    }
  }
}

void BossArenaScene::UpdatePhoneOverlay(float dt, const Input &input) {
  m_mirrorMessageTimer = std::max(0.0f, m_mirrorMessageTimer - dt);

  const bool spaceNow = input.IsKeyDown(VK_SPACE);
  if (spaceNow && !m_phoneSpaceWasDown)
    m_phoneOpen = !m_phoneOpen;
  m_phoneSpaceWasDown = spaceNow;

  const float target = m_phoneOpen ? 1.0f : 0.0f;
  const float t = std::clamp(dt * 11.0f, 0.0f, 1.0f);
  m_phoneSlide += (target - m_phoneSlide) * t;
  if (!m_phoneOpen && m_phoneSlide < 0.002f)
    m_phoneSlide = 0.0f;
}

void BossArenaScene::PrepareMirrorPuzzle(AttackType attack) {
  m_mirrorAttack = attack;
  m_mirrorPuzzleReady = true;
  m_mirrorPuzzleSolved = false;
  m_mirrorMessageTimer = 0.0f;
  m_dragMirrorDot = -1;
  m_mirrorPlaced = {false, false, false};

  std::array<XMFLOAT2, 3> baseTargets = {};
  if (attack == AttackType::MeteorAoE) {
    baseTargets = {XMFLOAT2{0.50f, 0.25f}, XMFLOAT2{0.25f, 0.66f},
                   XMFLOAT2{0.75f, 0.66f}};
  } else if (attack == AttackType::LaserLine) {
    baseTargets = {XMFLOAT2{0.30f, 0.30f}, XMFLOAT2{0.50f, 0.50f},
                   XMFLOAT2{0.70f, 0.70f}};
  } else {
    baseTargets = {XMFLOAT2{0.50f, 0.24f}, XMFLOAT2{0.23f, 0.72f},
                   XMFLOAT2{0.77f, 0.72f}};
  }

  const float angle =
      (MirrorRandom01(11) - 0.5f) *
      (attack == AttackType::LaserLine ? 1.10f : 0.72f);
  const float scale = 0.82f + MirrorRandom01(23) * 0.24f;
  const XMFLOAT2 center{0.50f + (MirrorRandom01(37) - 0.5f) * 0.18f,
                        0.50f + (MirrorRandom01(41) - 0.5f) * 0.16f};
  const float cs = std::cos(angle);
  const float sn = std::sin(angle);

  for (int i = 0; i < 3; ++i) {
    const float x = (baseTargets[i].x - 0.5f) * scale;
    const float y = (baseTargets[i].y - 0.5f) * scale;
    const float jitterX = (MirrorRandom01(100 + i * 7) - 0.5f) * 0.07f;
    const float jitterY = (MirrorRandom01(140 + i * 11) - 0.5f) * 0.07f;
    m_mirrorTargets[i] = {
        std::clamp(center.x + x * cs - y * sn + jitterX, 0.18f, 0.82f),
        std::clamp(center.y + x * sn + y * cs + jitterY, 0.20f, 0.78f)};
  }

  for (int i = 0; i < 3; ++i) {
    const float lane = (static_cast<float>(i) + 0.5f) / 3.0f;
    const float x = std::clamp(lane + (MirrorRandom01(220 + i) - 0.5f) * 0.16f,
                               0.16f, 0.84f);
    const float y = 0.86f - MirrorRandom01(260 + i * 3) * 0.10f;
    m_mirrorDots[i] = {x, y};
  }
}

void BossArenaScene::CompleteMirrorPuzzle() {
  if (!m_mirrorPuzzleReady)
    return;

  m_mirrorPuzzleReady = false;
  m_mirrorPuzzleSolved = true;
  m_mirrorMessageTimer = 1.4f;
  m_dragMirrorDot = -1;
  m_bossHp = std::max(0, m_bossHp - 1);
  m_counterVfxTimer = 0.78f;
  m_bossHitShakeTimer = 0.62f;

  m_impactTimer = 0.78f;
  m_impactMaxRadius = 10.2f;
  m_impactPosition = {0.0f, 0.07f, kArenaHalfExtent - 3.5f};

  if (m_bossHp <= 0)
    m_cleared = true;
}

bool BossArenaScene::AreMirrorDotsPlaced() const {
  return m_mirrorPlaced[0] && m_mirrorPlaced[1] && m_mirrorPlaced[2];
}

float BossArenaScene::MirrorRandom01(uint32_t salt) const {
  uint32_t x = static_cast<uint32_t>(m_attackIndex + 1) * 747796405u;
  x ^= static_cast<uint32_t>(m_bossHp + 3) * 2891336453u;
  x ^= static_cast<uint32_t>(m_mirrorAttack) * 277803737u;
  x ^= salt * 1597334677u;
  x ^= x >> 16;
  x *= 2246822519u;
  x ^= x >> 13;
  x *= 3266489917u;
  x ^= x >> 16;
  return static_cast<float>(x & 0x00FFFFFFu) / 16777215.0f;
}

float BossArenaScene::TelegraphDuration() const {
  if (m_attack == AttackType::MeteorAoE)
    return 1.8f;
  if (m_attack == AttackType::LaserLine)
    return 1.65f;
  if (m_attack == AttackType::Knockback)
    return 2.0f;
  return 1.6f;
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
  if (phoneHeadHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    m_phoneOpen = !m_phoneOpen;
  }

  ImDrawList *draw = ImGui::GetForegroundDrawList();
  if (m_phoneSlide > 0.02f) {
    draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(vw, vh),
                        Rgba(0.02f, 0.025f, 0.03f, 0.30f * eased));
  }

  const ImVec2 outerMin(phoneX, phoneY);
  const ImVec2 outerMax(phoneX + phoneWidth, phoneY + phoneHeight);
  draw->AddRectFilled(ImVec2(phoneX + 12.0f, phoneY + 18.0f),
                      ImVec2(phoneX + phoneWidth + 12.0f,
                             phoneY + phoneHeight + 18.0f),
                      Rgba(0.0f, 0.0f, 0.0f, 0.28f), rounding);
  draw->AddRectFilled(outerMin, outerMax, Rgba(0.015f, 0.017f, 0.02f, 1.0f),
                      rounding);
  draw->AddRect(outerMin, outerMax, Rgba(0.42f, 0.52f, 0.58f, 0.72f),
                rounding, 0, 2.0f);

  const float bezel = phoneWidth * 0.055f;
  const ImVec2 screenMin(phoneX + bezel, phoneY + bezel * 1.75f);
  const ImVec2 screenMax(phoneX + phoneWidth - bezel,
                         phoneY + phoneHeight - bezel * 1.25f);
  draw->AddRectFilled(screenMin, screenMax, Rgba(0.045f, 0.060f, 0.066f, 1.0f),
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
                  Rgba(0.76f, 0.92f, 0.88f, screenAlpha),
                  "MIZUKAGAMI");
    draw->AddText(ImVec2(screenMin.x + 24.0f, screenMin.y + 50.0f),
                  Rgba(0.46f, 0.66f, 0.62f, screenAlpha),
                  "REVERSE SCRIPT");

    const ImVec2 puzzleMin(screenMin.x + 30.0f, screenMin.y + 102.0f);
    const ImVec2 puzzleMax(screenMax.x - 30.0f, screenMax.y - 76.0f);
    draw->AddRect(puzzleMin, puzzleMax,
                  Rgba(0.55f, 0.95f, 0.82f, 0.62f * screenAlpha), 10.0f, 0,
                  1.6f);

    const ImVec2 puzzleCenter((puzzleMin.x + puzzleMax.x) * 0.5f,
                              (puzzleMin.y + puzzleMax.y) * 0.5f);
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

    if (m_phoneOpen && m_mirrorPuzzleReady && screenAlpha > 0.72f) {
      const ImVec2 mousePos = ImGui::GetIO().MousePos;
      const bool mouseClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
      const bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
      const bool mouseReleased =
          ImGui::IsMouseReleased(ImGuiMouseButton_Left);

      if (mouseClicked && m_dragMirrorDot < 0) {
        float bestDistSq = 24.0f * 24.0f;
        int bestIndex = -1;
        for (int i = 0; i < 3; ++i) {
          if (m_mirrorPlaced[i])
            continue;
          const ImVec2 p = RectPoint(puzzleMin, puzzleMax, m_mirrorDots[i]);
          const float d = DistanceSq(mousePos, p);
          if (d < bestDistSq) {
            bestDistSq = d;
            bestIndex = i;
          }
        }
        m_dragMirrorDot = bestIndex;
      }

      if (m_dragMirrorDot >= 0 && mouseDown) {
        m_mirrorDots[m_dragMirrorDot] =
            RectUv(puzzleMin, puzzleMax, mousePos);
      }

      if (m_dragMirrorDot >= 0 && mouseReleased) {
        const int dot = m_dragMirrorDot;
        const ImVec2 dotPos = RectPoint(puzzleMin, puzzleMax, m_mirrorDots[dot]);
        const ImVec2 targetPos =
            RectPoint(puzzleMin, puzzleMax, m_mirrorTargets[dot]);
        if (DistanceSq(dotPos, targetPos) <= 34.0f * 34.0f) {
          m_mirrorDots[dot] = m_mirrorTargets[dot];
          m_mirrorPlaced[dot] = true;
        }
        m_dragMirrorDot = -1;
        if (AreMirrorDotsPlaced())
          CompleteMirrorPuzzle();
      }
    } else if (!m_phoneOpen) {
      m_dragMirrorDot = -1;
    }

    if (m_mirrorPuzzleReady) {
      for (int i = 0; i < 3; ++i) {
        const ImVec2 target = RectPoint(puzzleMin, puzzleMax,
                                        m_mirrorTargets[i]);
        const float pulse =
            1.0f + 0.12f *
                       std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f +
                                static_cast<float>(i));
        draw->AddCircle(target, 18.0f * pulse,
                        Rgba(0.72f, 1.0f, 0.88f, 0.60f * screenAlpha),
                        24, 2.2f);
        char label[2] = {static_cast<char>('1' + i), '\0'};
        draw->AddText(ImVec2(target.x - 4.0f, target.y - 8.0f),
                      Rgba(0.84f, 1.0f, 0.92f, screenAlpha), label);
      }

      for (int i = 0; i < 3; ++i) {
        const ImVec2 dot = RectPoint(puzzleMin, puzzleMax, m_mirrorDots[i]);
        const bool dragging = m_dragMirrorDot == i;
        const ImU32 dotColor =
            m_mirrorPlaced[i]
                ? Rgba(0.52f, 1.0f, 0.76f, screenAlpha)
                : Rgba(0.95f, 0.96f, 0.84f, screenAlpha);
        draw->AddCircleFilled(dot, dragging ? 15.0f : 12.0f, dotColor, 24);
        draw->AddCircle(dot, dragging ? 19.0f : 16.0f,
                        Rgba(0.05f, 0.09f, 0.08f, 0.92f * screenAlpha), 24,
                        2.0f);
        char label[2] = {static_cast<char>('1' + i), '\0'};
        draw->AddText(ImVec2(dot.x - 4.0f, dot.y - 8.0f),
                      Rgba(0.05f, 0.08f, 0.07f, screenAlpha), label);
      }
    } else {
      const char *message =
          m_mirrorMessageTimer > 0.0f ? "COUNTER SENT" : "NO TRACE";
      const char *sub =
          m_mirrorMessageTimer > 0.0f
              ? "Boss took reflected damage."
              : "Dodge a boss attack to capture a mirror trace.";
      draw->AddText(ImVec2(puzzleMin.x + 24.0f, puzzleCenter.y - 18.0f),
                    Rgba(0.76f, 0.92f, 0.88f, screenAlpha), message);
      draw->AddText(ImVec2(puzzleMin.x + 24.0f, puzzleCenter.y + 12.0f),
                    Rgba(0.46f, 0.66f, 0.62f, screenAlpha), sub);
    }

    const char *status =
        m_mirrorPuzzleReady ? "MATCH NUMBERED INK SEALS" : "STANDBY";
    draw->AddText(ImVec2(screenMin.x + 24.0f, screenMax.y - 38.0f),
                  Rgba(0.76f, 0.92f, 0.88f, screenAlpha), status);
  }
}
