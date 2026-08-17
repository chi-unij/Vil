#include "game/OverworldScene.h"

#include "GltfLoader.h"
#include "MeshRenderer.h"
#include "ProceduralMesh.h"

#include <DirectXMath.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace DirectX;
using json = nlohmann::json;

namespace {

bool LoadTexture(const char *path, LoadedImage &outImage) {
  if (LoadImageFile(path, outImage))
    return true;

  OutputDebugStringA("[OverworldScene] WARNING: texture load failed: ");
  OutputDebugStringA(path);
  OutputDebugStringA("\n");
  return false;
}

LoadedImage BuildMetalRoughFromRoughness(const LoadedImage &roughness) {
  LoadedImage metalRough = roughness;
  for (size_t p = 0; p + 3 < metalRough.pixels.size(); p += 4) {
    const unsigned char r = roughness.pixels[p];
    metalRough.pixels[p + 0] = 0;
    metalRough.pixels[p + 1] = r;
    metalRough.pixels[p + 2] = 0;
    metalRough.pixels[p + 3] = 255;
  }
  return metalRough;
}

bool ReadFloat3(const json &object, const char *key, XMFLOAT3 &outValue) {
  if (!object.contains(key) || !object[key].is_array() ||
      object[key].size() != 3)
    return false;

  outValue = {object[key][0].get<float>(), object[key][1].get<float>(),
              object[key][2].get<float>()};
  return true;
}

void PushDebugLine(FrameData &frame, const XMFLOAT3 &start, const XMFLOAT3 &end,
                   const XMFLOAT4 &color) {
  frame.debugLines.push_back({start, end, color});
}

std::string ResolveLiveAssetPath(const std::string &path) {
  namespace fs = std::filesystem;

  const fs::path direct(path);
  const fs::path sourceFromBuild = fs::path("..") / ".." / ".." / path;

  if (fs::exists(sourceFromBuild))
    return sourceFromBuild.generic_string();
  if (fs::exists(direct))
    return direct.generic_string();
  return path;
}

XMFLOAT3 OffsetAwayFromCenter(const XMFLOAT3 &position, float distanceOffset) {
  if (std::abs(distanceOffset) <= 0.0001f)
    return position;

  const float lenSq = position.x * position.x + position.z * position.z;
  if (lenSq <= 0.0001f)
    return position;

  const float invLen = 1.0f / std::sqrt(lenSq);
  return {position.x + position.x * invLen * distanceOffset, position.y,
          position.z + position.z * invLen * distanceOffset};
}

XMFLOAT3 TransformStagePoint(const XMFLOAT3 &local,
                             const XMFLOAT3 &position,
                             const XMFLOAT3 &scale, float yawRadians) {
  const float sx = local.x * scale.x;
  const float sy = local.y * scale.y;
  const float sz = local.z * scale.z;
  const float c = std::cos(yawRadians);
  const float s = std::sin(yawRadians);
  return {position.x + sx * c - sz * s, position.y + sy,
          position.z + sx * s + sz * c};
}

float TriangleArea2D(const XMFLOAT2 &a, const XMFLOAT2 &b,
                     const XMFLOAT2 &c) {
  return std::abs((b.x - a.x) * (c.y - a.y) -
                  (b.y - a.y) * (c.x - a.x)) * 0.5f;
}

} // namespace

void OverworldScene::Initialize(DxContext &dx) {
  LoadedMesh floorMesh =
      ProceduralMesh::CreatePlane(kFloorSizeMeters, kFloorSizeMeters);

  LoadedImage baseColor;
  LoadedImage normal;
  LoadedImage roughness;
  LoadedImage ao;
  LoadedImage height;
  LoadedImage metalRough;

  MaterialImages images{};
  if (LoadTexture("Assets/textures/Floor_png/Ground037_2K-PNG_Color.png",
                  baseColor))
    images.baseColor = &baseColor;
  if (LoadTexture("Assets/textures/Floor_png/Ground037_2K-PNG_NormalDX.png",
                  normal))
    images.normal = &normal;
  if (LoadTexture(
          "Assets/textures/Floor_png/Ground037_2K-PNG_AmbientOcclusion.png",
          ao))
    images.ao = &ao;
  if (LoadTexture("Assets/textures/Floor_png/Ground037_2K-PNG_Displacement.png",
                  height))
    images.height = &height;
  if (LoadTexture("Assets/textures/Floor_png/Ground037_2K-PNG_Roughness.png",
                  roughness)) {
    metalRough = BuildMetalRoughFromRoughness(roughness);
    images.metalRough = &metalRough;
  }

  Material floorMaterial{};
  floorMaterial.baseColorFactor = {0.92f, 0.96f, 0.82f, 1.0f};
  floorMaterial.metallicFactor = 0.0f;
  floorMaterial.roughnessFactor = 0.88f;
  floorMaterial.uvTiling = {12.0f, 12.0f};
  floorMaterial.pomEnabled = true;
  floorMaterial.heightScale = 0.01f;
  floorMaterial.pomMinLayers = 8.0f;
  floorMaterial.pomMaxLayers = 20.0f;
  floorMaterial.proceduralTypeId = 7.0f;
  floorMaterial.reflectionReceiver = ReflectionReceiver::Water;
  // Mirror ray には地面を見せる。Water ray は instance mask で除外する。
  floorMaterial.rayTracingVisible = true;

  m_floorMeshId = dx.CreateMeshResources(floorMesh, images, floorMaterial);

  const LoadedMesh waterMesh =
      ProceduralMesh::CreateTessellatedPlane(1.0f, 1.0f, 256, 256);
  Material waterMaterial{};
  waterMaterial.baseColorFactor = {0.34f, 0.58f, 0.66f, 1.0f};
  waterMaterial.metallicFactor = 0.0f;
  waterMaterial.roughnessFactor = 0.12f;
  waterMaterial.emissiveFactor = {0.0f, 0.04f, 0.08f};
  waterMaterial.uvTiling = {1.0f, 1.0f};
  waterMaterial.proceduralTypeId = 0.0f;
  waterMaterial.vertexDeformTypeId = 6.0f;
  m_waterMeshId = dx.CreateMeshResources(waterMesh, {}, waterMaterial);


  const LoadedMesh castleWallMesh = ProceduralMesh::CreateCube(1.0f);

  LoadedImage wallBaseColor;
  LoadedImage wallNormal;
  LoadedImage wallRoughness;
  LoadedImage wallAo;
  LoadedImage wallHeight;
  LoadedImage wallMetalRough;

  MaterialImages wallImages{};
  if (LoadTexture(
          "Assets/textures/Castle_wall/textures/seaworn_stone_tiles_diff_2k.png",
          wallBaseColor))
    wallImages.baseColor = &wallBaseColor;
  if (LoadTexture(
          "Assets/textures/Castle_wall/textures/seaworn_stone_tiles_nor_dx_2k.png",
          wallNormal))
    wallImages.normal = &wallNormal;
  if (LoadTexture(
          "Assets/textures/Castle_wall/textures/seaworn_stone_tiles_ao_2k.png",
          wallAo))
    wallImages.ao = &wallAo;
  if (LoadTexture(
          "Assets/textures/Castle_wall/textures/seaworn_stone_tiles_disp_2k.png",
          wallHeight))
    wallImages.height = &wallHeight;
  if (LoadTexture(
          "Assets/textures/Castle_wall/textures/seaworn_stone_tiles_rough_2k.png",
          wallRoughness)) {
    wallMetalRough = BuildMetalRoughFromRoughness(wallRoughness);
    wallImages.metalRough = &wallMetalRough;
  }

  Material castleWallMaterial{};
  castleWallMaterial.baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
  castleWallMaterial.metallicFactor = 0.0f;
  castleWallMaterial.roughnessFactor = 0.82f;
  castleWallMaterial.uvTiling = {18.0f, 3.0f};
  castleWallMaterial.pomEnabled = true;
  castleWallMaterial.heightScale = 0.032f;
  castleWallMaterial.pomMinLayers = 12.0f;
  castleWallMaterial.pomMaxLayers = 28.0f;

  m_castleWallMeshId =
      dx.CreateMeshResources(castleWallMesh, wallImages, castleWallMaterial);
  Material castleMerlonMaterial = castleWallMaterial;
  castleMerlonMaterial.uvTiling = {0.55f, 0.6f};
  castleMerlonMaterial.heightScale = 0.02f;

  Material castleTowerMaterial = castleWallMaterial;
  castleTowerMaterial.uvTiling = {0.65f, 3.6f};
  castleTowerMaterial.heightScale = 0.026f;

  m_castleMerlonMeshId =
      dx.CreateMeshResources(castleWallMesh, wallImages, castleMerlonMaterial);
  m_castleTowerMeshId =
      dx.CreateMeshResources(castleWallMesh, wallImages, castleTowerMaterial);

  const LoadedMesh bossWarpMarkerMesh = ProceduralMesh::CreateCone(0.55f, 1.2f, 3);
  Material bossWarpMarkerMaterial{};
  bossWarpMarkerMaterial.baseColorFactor = {0.12f, 0.85f, 1.0f, 0.42f};
  bossWarpMarkerMaterial.metallicFactor = 0.0f;
  bossWarpMarkerMaterial.roughnessFactor = 0.18f;
  bossWarpMarkerMaterial.emissiveFactor = {0.05f, 0.55f, 0.95f};
  m_bossWarpMarkerMeshId =
      dx.CreateMeshResources(bossWarpMarkerMesh, {}, bossWarpMarkerMaterial);

  const LoadedMesh pathStoneMesh = ProceduralMesh::CreateCube(1.0f);
  Material pathStoneMaterial{};
  pathStoneMaterial.baseColorFactor = {0.28f, 0.31f, 0.27f, 1.0f};
  pathStoneMaterial.metallicFactor = 0.0f;
  pathStoneMaterial.roughnessFactor = 0.72f;
  pathStoneMaterial.proceduralTypeId = 7.0f;
  pathStoneMaterial.reflectionReceiver = ReflectionReceiver::Water;
  pathStoneMaterial.rayTracingVisible = false;
  m_pathStoneMeshId =
      dx.CreateMeshResources(pathStoneMesh, {}, pathStoneMaterial);

  const LoadedMesh lanternPostMesh = ProceduralMesh::CreateCylinder(0.5f, 1.0f, 12);
  Material lanternPostMaterial{};
  lanternPostMaterial.baseColorFactor = {0.13f, 0.08f, 0.045f, 1.0f};
  lanternPostMaterial.metallicFactor = 0.0f;
  lanternPostMaterial.roughnessFactor = 0.65f;
  m_lanternPostMeshId =
      dx.CreateMeshResources(lanternPostMesh, {}, lanternPostMaterial);

  const LoadedMesh lanternCapMesh = ProceduralMesh::CreateCube(1.0f);
  Material lanternCapMaterial{};
  lanternCapMaterial.baseColorFactor = {0.42f, 0.20f, 0.08f, 1.0f};
  lanternCapMaterial.metallicFactor = 0.0f;
  lanternCapMaterial.roughnessFactor = 0.58f;
  m_lanternCapMeshId =
      dx.CreateMeshResources(lanternCapMesh, {}, lanternCapMaterial);

  const LoadedMesh lanternGlowMesh = ProceduralMesh::CreateCube(1.0f);
  Material lanternGlowMaterial{};
  lanternGlowMaterial.baseColorFactor = {1.0f, 0.40f, 0.08f, 0.90f};
  lanternGlowMaterial.metallicFactor = 0.0f;
  lanternGlowMaterial.roughnessFactor = 0.16f;
  lanternGlowMaterial.emissiveFactor = {3.2f, 0.68f, 0.10f};
  m_lanternGlowMeshId =
      dx.CreateMeshResources(lanternGlowMesh, {}, lanternGlowMaterial);

  if (StageModel *lanternModel =
          FindOrLoadModel(dx, "Assets/models/japanese_shrine_lantern.glb")) {
    m_shrineLanternMeshIds = lanternModel->meshIds;
    OutputDebugStringA("[OverworldScene] Japanese shrine lantern model loaded.\n");
  } else {
    OutputDebugStringA(
        "[OverworldScene] WARNING: shrine lantern model not loaded; using procedural fallback.\n");
  }

  const LoadedMesh waystoneMesh = ProceduralMesh::CreateCube(1.0f);
  Material waystoneMaterial{};
  waystoneMaterial.baseColorFactor = {0.18f, 0.22f, 0.24f, 1.0f};
  waystoneMaterial.metallicFactor = 0.0f;
  waystoneMaterial.roughnessFactor = 0.9f;
  waystoneMaterial.emissiveFactor = {0.0f, 0.025f, 0.04f};
  waystoneMaterial.proceduralTypeId = 7.0f;
  waystoneMaterial.reflectionReceiver = ReflectionReceiver::Water;
  waystoneMaterial.rayTracingVisible = false;
  m_waystoneMeshId =
      dx.CreateMeshResources(waystoneMesh, {}, waystoneMaterial);

  // 城下に物語・遊びへ発展できる「反写の大鏡」を置く。
  const LoadedMesh reflectionMonolithMirrorMesh =
      ProceduralMesh::CreatePlane(1.0f, 1.0f);
  Material reflectionMonolithMirrorMaterial{};
  reflectionMonolithMirrorMaterial.baseColorFactor = {0.055f, 0.075f, 0.095f,
                                                      1.0f};
  reflectionMonolithMirrorMaterial.metallicFactor = 0.96f;
  reflectionMonolithMirrorMaterial.roughnessFactor = 0.035f;
  reflectionMonolithMirrorMaterial.emissiveFactor = {0.004f, 0.012f, 0.020f};
  reflectionMonolithMirrorMaterial.reflectionReceiver =
      ReflectionReceiver::Mirror;
  reflectionMonolithMirrorMaterial.reflectionStrength = 1.0f;
  reflectionMonolithMirrorMaterial.rayTracingVisible = false;
  m_reflectionMonolithMirrorMeshId = dx.CreateMeshResources(
      reflectionMonolithMirrorMesh, {}, reflectionMonolithMirrorMaterial);

  const LoadedMesh reflectionMonolithFrameMesh =
      ProceduralMesh::CreateCube(1.0f);
  Material reflectionMonolithFrameMaterial{};
  reflectionMonolithFrameMaterial.baseColorFactor = {0.11f, 0.15f, 0.17f, 1.0f};
  reflectionMonolithFrameMaterial.metallicFactor = 0.52f;
  reflectionMonolithFrameMaterial.roughnessFactor = 0.26f;
  reflectionMonolithFrameMaterial.emissiveFactor = {0.008f, 0.045f, 0.065f};
  m_reflectionMonolithFrameMeshId = dx.CreateMeshResources(
      reflectionMonolithFrameMesh, {}, reflectionMonolithFrameMaterial);

  m_ready =
      (m_floorMeshId != UINT32_MAX && m_castleWallMeshId != UINT32_MAX &&
       m_waterMeshId != UINT32_MAX &&
       m_castleMerlonMeshId != UINT32_MAX &&
       m_castleTowerMeshId != UINT32_MAX &&
       m_bossWarpMarkerMeshId != UINT32_MAX &&
       m_pathStoneMeshId != UINT32_MAX &&
       m_lanternPostMeshId != UINT32_MAX &&
       m_lanternCapMeshId != UINT32_MAX &&
       m_lanternGlowMeshId != UINT32_MAX &&
       m_waystoneMeshId != UINT32_MAX &&
       m_reflectionMonolithMirrorMeshId != UINT32_MAX &&
       m_reflectionMonolithFrameMeshId != UINT32_MAX);

  if (m_ready) {
    OutputDebugStringA(
        "[OverworldScene] 60m x 60m floor, water moat, and castle walls initialized.\n");
  } else {
    OutputDebugStringA(
        "[OverworldScene] FAILED: floor or castle wall mesh was not created.\n");
  }

  ReloadPlacements(dx);
  BuildBackgroundForest(dx);
}

void OverworldScene::BuildFrame(FrameData &frame) const {
  if (!m_ready)
    return;

  const XMMATRIX floorWorld = XMMatrixTranslation(0.0f, 0.0f, 0.0f);
  frame.opaqueItems.push_back({m_floorMeshId, floorWorld});

  constexpr float waterY = 0.16f;
  constexpr float waterOuterHalf = kFloorSizeMeters * 0.5f - 1.0f;
  constexpr float waterInnerHalf = kCastleWallHalfExtentMeters + 0.9f;
  constexpr float waterWidth = waterOuterHalf - waterInnerHalf;
  constexpr float waterCenter = waterInnerHalf + waterWidth * 0.5f;
  constexpr float waterLongLength = waterOuterHalf * 2.0f;
  constexpr float waterSideLength = waterInnerHalf * 2.0f;

  auto pushWater = [&frame](uint32_t meshId, float sx, float sz, float x,
                            float z) {
    frame.transparentItems.push_back(
        {meshId, XMMatrixScaling(sx, 1.0f, sz) * XMMatrixTranslation(x, waterY, z)});
  };

  pushWater(m_waterMeshId, waterLongLength, waterWidth, 0.0f, waterCenter);
  pushWater(m_waterMeshId, waterLongLength, waterWidth, 0.0f, -waterCenter);
  pushWater(m_waterMeshId, waterWidth, waterSideLength, waterCenter, 0.0f);
  pushWater(m_waterMeshId, waterWidth, waterSideLength, -waterCenter, 0.0f);

  constexpr float wallThickness = 0.7f;
  constexpr float wallHeight = 4.8f;
  constexpr float wallY = wallHeight * 0.5f;
  constexpr float wallLength = kCastleWallHalfExtentMeters * 2.0f;
  constexpr float gateGapWidth = kCastleGateHalfWidthMeters * 2.0f;
  constexpr float northWallSegmentLength = (wallLength - gateGapWidth) * 0.5f;
  constexpr float northWallSegmentCenterX =
      kCastleGateHalfWidthMeters + northWallSegmentLength * 0.5f;

  const XMMATRIX southWall =
      XMMatrixScaling(wallLength, wallHeight, wallThickness) *
      XMMatrixTranslation(0.0f, wallY, -kCastleWallHalfExtentMeters);
  const XMMATRIX eastWall =
      XMMatrixScaling(wallThickness, wallHeight, wallLength) *
      XMMatrixTranslation(kCastleWallHalfExtentMeters, wallY, 0.0f);
  const XMMATRIX westWall =
      XMMatrixScaling(wallThickness, wallHeight, wallLength) *
      XMMatrixTranslation(-kCastleWallHalfExtentMeters, wallY, 0.0f);
  const XMMATRIX northWallLeft =
      XMMatrixScaling(northWallSegmentLength, wallHeight, wallThickness) *
      XMMatrixTranslation(-northWallSegmentCenterX, wallY,
                          kCastleWallHalfExtentMeters);
  const XMMATRIX northWallRight =
      XMMatrixScaling(northWallSegmentLength, wallHeight, wallThickness) *
      XMMatrixTranslation(northWallSegmentCenterX, wallY,
                          kCastleWallHalfExtentMeters);

  frame.opaqueItems.push_back({m_castleWallMeshId, southWall});
  frame.opaqueItems.push_back({m_castleWallMeshId, eastWall});
  frame.opaqueItems.push_back({m_castleWallMeshId, westWall});
  frame.opaqueItems.push_back({m_castleWallMeshId, northWallLeft});
  frame.opaqueItems.push_back({m_castleWallMeshId, northWallRight});

  constexpr float merlonWidth = 1.4f;
  constexpr float merlonHeight = 0.9f;
  constexpr float merlonDepth = 1.05f;
  constexpr float merlonSpacing = 3.2f;
  constexpr float merlonY = wallHeight + merlonHeight * 0.5f;
  constexpr float towerSize = 1.8f;
  constexpr float towerHeight = 5.8f;
  constexpr float towerY = towerHeight * 0.5f;

  auto pushCastleBlock = [&frame](uint32_t meshId, float sx, float sy, float sz,
                                  float x, float y, float z) {
    frame.opaqueItems.push_back(
        {meshId, XMMatrixScaling(sx, sy, sz) * XMMatrixTranslation(x, y, z)});
  };

  for (float x = -kCastleWallHalfExtentMeters + merlonSpacing;
       x <= kCastleWallHalfExtentMeters - merlonSpacing; x += merlonSpacing) {
    pushCastleBlock(m_castleMerlonMeshId, merlonWidth, merlonHeight,
                    merlonDepth, x, merlonY, -kCastleWallHalfExtentMeters);

    if (std::abs(x) > kCastleGateHalfWidthMeters + merlonWidth) {
      pushCastleBlock(m_castleMerlonMeshId, merlonWidth, merlonHeight,
                      merlonDepth, x, merlonY, kCastleWallHalfExtentMeters);
    }
  }

  for (float z = -kCastleWallHalfExtentMeters + merlonSpacing;
       z <= kCastleWallHalfExtentMeters - merlonSpacing; z += merlonSpacing) {
    pushCastleBlock(m_castleMerlonMeshId, merlonDepth, merlonHeight,
                    merlonWidth, -kCastleWallHalfExtentMeters, merlonY, z);
    pushCastleBlock(m_castleMerlonMeshId, merlonDepth, merlonHeight,
                    merlonWidth, kCastleWallHalfExtentMeters, merlonY, z);
  }

  pushCastleBlock(m_castleTowerMeshId, towerSize, towerHeight, towerSize,
                  -kCastleWallHalfExtentMeters, towerY,
                  -kCastleWallHalfExtentMeters);
  pushCastleBlock(m_castleTowerMeshId, towerSize, towerHeight, towerSize,
                  kCastleWallHalfExtentMeters, towerY,
                  -kCastleWallHalfExtentMeters);
  pushCastleBlock(m_castleTowerMeshId, towerSize, towerHeight, towerSize,
                  -kCastleWallHalfExtentMeters, towerY,
                  kCastleWallHalfExtentMeters);
  pushCastleBlock(m_castleTowerMeshId, towerSize, towerHeight, towerSize,
                  kCastleWallHalfExtentMeters, towerY,
                  kCastleWallHalfExtentMeters);

  for (const StageObject &object : m_stageObjects) {
    const XMMATRIX world =
        XMMatrixScaling(object.scale.x, object.scale.y, object.scale.z) *
        XMMatrixRotationY(object.yawRadians) *
        XMMatrixTranslation(object.position.x, object.position.y,
                            object.position.z);
    frame.opaqueItems.push_back({object.meshId, world});
  }

  AppendWorldPolishProps(frame);
  AppendReflectionMonolith(frame);

  const auto &forestDebug = m_backgroundForestDebug;
  if (forestDebug.enabled && !m_backgroundForestMeshIds.empty()) {
    int drawnClusters = 0;
    for (const BackgroundForestCluster &cluster : m_backgroundForestClusters) {
      if (cluster.densityTier > forestDebug.densityLevel)
        continue;
      if (forestDebug.singleClusterPreview && drawnClusters > 0)
        break;

      const XMFLOAT3 spacedPosition = {
          cluster.position.x * forestDebug.spacingMultiplier,
          cluster.position.y,
          cluster.position.z * forestDebug.spacingMultiplier};
      const XMFLOAT3 position =
          OffsetAwayFromCenter(spacedPosition, forestDebug.distanceOffset);
      const float scale = cluster.baseScale * forestDebug.scaleMultiplier;
      for (uint32_t meshId : m_backgroundForestMeshIds) {
        const XMMATRIX world =
            XMMatrixScaling(scale, scale, scale) *
            XMMatrixRotationY(cluster.yawRadians) *
            XMMatrixTranslation(position.x, position.y, position.z);
        frame.opaqueItems.push_back({meshId, world});
      }
      ++drawnClusters;
    }
  }

  // BossArena へ接続するワープ地点。半透明の三角マーカーを上下に揺らす。
  const XMFLOAT3 warpPos = BossWarpPosition();
  const float warpBob = std::sin(frame.gameTime * 2.8f) * 0.22f;
  const float warpSpin = frame.gameTime * 1.2f;
  const XMMATRIX warpMarkerWorld =
      XMMatrixScaling(1.35f, 1.35f, 1.35f) *
      XMMatrixRotationX(XM_PI) * XMMatrixRotationY(warpSpin) *
      XMMatrixTranslation(warpPos.x, 2.30f + warpBob, warpPos.z);
  frame.transparentItems.push_back({m_bossWarpMarkerMeshId, warpMarkerWorld});

  GPUPointLight warpLight{};
  warpLight.position = {warpPos.x, 1.0f + warpBob * 0.35f, warpPos.z};
  warpLight.range = 5.0f;
  warpLight.color = {0.18f, 0.85f, 1.0f};
  warpLight.intensity = 1.8f;
  frame.pointLights.push_back(warpLight);
}

void OverworldScene::AppendWorldPolishProps(FrameData &frame) const {
  if (m_pathStoneMeshId == UINT32_MAX ||
      m_lanternPostMeshId == UINT32_MAX ||
      m_lanternCapMeshId == UINT32_MAX ||
      m_lanternGlowMeshId == UINT32_MAX ||
      m_waystoneMeshId == UINT32_MAX) {
    return;
  }

  auto pushOpaque = [&frame](uint32_t meshId, float sx, float sy, float sz,
                             float x, float y, float z, float yawRadians) {
    const XMMATRIX world =
        XMMatrixScaling(sx, sy, sz) * XMMatrixRotationY(yawRadians) *
        XMMatrixTranslation(x, y, z);
    frame.opaqueItems.push_back({meshId, world});
  };

  auto pushTransparent = [&frame](uint32_t meshId, float sx, float sy, float sz,
                                  float x, float y, float z,
                                  float yawRadians) {
    const XMMATRIX world =
        XMMatrixScaling(sx, sy, sz) * XMMatrixRotationY(yawRadians) *
        XMMatrixTranslation(x, y, z);
    frame.transparentItems.push_back({meshId, world});
  };

  for (int i = 0; i < 15; ++i) {
    const float t = static_cast<float>(i) / 14.0f;
    const float z = -16.8f + t * 37.2f;
    const float x = std::sin(t * XM_2PI * 1.35f) * 0.38f;
    const float yaw = std::sin(t * XM_2PI * 2.1f) * 0.22f;
    const float width = 1.20f + 0.24f * (i % 3 == 0 ? 1.0f : 0.0f);
    const float depth = 0.58f + 0.16f * (i % 2 == 0 ? 1.0f : 0.0f);
    pushOpaque(m_pathStoneMeshId, width, 0.075f, depth, x, 0.055f, z, yaw);
  }

  struct LanternPlacement {
    float x;
    float z;
    float yaw;
    float intensity;
  };
  const LanternPlacement lanterns[] = {
      {-4.3f, -14.0f, 0.18f, 0.62f}, {4.2f, -10.0f, -0.22f, 0.58f},
      {-4.7f, -5.4f, 0.08f, 0.54f},  {4.6f, -1.0f, -0.18f, 0.54f},
      {-4.5f, 4.2f, 0.20f, 0.60f},   {4.7f, 9.0f, -0.12f, 0.62f},
      {-4.1f, 14.8f, 0.16f, 0.66f},   {4.0f, 19.2f, -0.20f, 0.74f},
  };

  for (const LanternPlacement &lantern : lanterns) {
    if (!m_shrineLanternMeshIds.empty()) {
      for (uint32_t meshId : m_shrineLanternMeshIds) {
        pushOpaque(meshId, 0.78f, 0.78f, 0.78f, lantern.x, 0.0f,
                   lantern.z, lantern.yaw);
      }
    } else {
      pushOpaque(m_lanternPostMeshId, 0.15f, 1.62f, 0.15f, lantern.x, 0.82f,
                 lantern.z, lantern.yaw);
      pushOpaque(m_lanternCapMeshId, 0.62f, 0.18f, 0.62f, lantern.x, 1.68f,
                 lantern.z, lantern.yaw + 0.35f);
    }
    pushTransparent(m_lanternGlowMeshId, 0.24f, 0.20f, 0.24f, lantern.x,
                    0.92f, lantern.z, lantern.yaw);
    GPUPointLight light{};
    light.position = {lantern.x, 0.92f, lantern.z};
    light.range = 4.6f;
    light.color = {1.0f, 0.58f, 0.22f};
    light.intensity = lantern.intensity * 2.8f;
    frame.pointLights.push_back(light);
  }

  struct WaystonePlacement {
    float x;
    float z;
    float yaw;
    float height;
  };
  const WaystonePlacement waystones[] = {
      {-7.8f, 6.2f, 0.45f, 1.35f}, {7.4f, 7.5f, -0.38f, 1.15f},
      {-2.2f, 18.2f, 0.18f, 1.50f}, {2.2f, 18.2f, -0.18f, 1.50f},
  };

  for (const WaystonePlacement &stone : waystones) {
    pushOpaque(m_waystoneMeshId, 0.48f, stone.height, 0.28f, stone.x,
               stone.height * 0.5f, stone.z, stone.yaw);
  }
}

void OverworldScene::AppendReflectionMonolith(FrameData &frame) const {
  if (m_reflectionMonolithMirrorMeshId == UINT32_MAX ||
      m_reflectionMonolithFrameMeshId == UINT32_MAX) {
    return;
  }

  // 西北側の空き地から出生地点へ向け、主経路と既存モデルを避ける。
  constexpr float centerX = -18.0f;
  constexpr float centerY = 3.25f;
  constexpr float centerZ = 16.0f;
  constexpr float yawRadians = -24.0f * XM_PI / 180.0f;
  constexpr float mirrorWidth = 8.0f;
  constexpr float mirrorHeight = 5.2f;
  constexpr float frameThickness = 0.46f;
  constexpr float frameDepth = 0.50f;
  constexpr float outerHeight = 6.1f;
  const float yawCos = std::cos(yawRadians);
  const float yawSin = std::sin(yawRadians);

  auto localPosition = [=](float x, float y, float z) {
    return XMFLOAT3{centerX + x * yawCos + z * yawSin, y,
                    centerZ - x * yawSin + z * yawCos};
  };

  const XMFLOAT3 mirrorPosition =
      localPosition(0.0f, centerY, -frameDepth * 0.5f - 0.03f);
  const XMMATRIX mirrorWorld =
      XMMatrixScaling(mirrorWidth, 1.0f, mirrorHeight) *
      XMMatrixRotationX(-XM_PIDIV2) * XMMatrixRotationY(yawRadians) *
      XMMatrixTranslation(mirrorPosition.x, mirrorPosition.y, mirrorPosition.z);
  frame.opaqueItems.push_back({m_reflectionMonolithMirrorMeshId, mirrorWorld});

  auto pushFrame = [&frame](uint32_t meshId, float sx, float sy, float sz,
                            float x, float y, float z, float yaw) {
    frame.opaqueItems.push_back(
        {meshId, XMMatrixScaling(sx, sy, sz) * XMMatrixRotationY(yaw) *
                     XMMatrixTranslation(x, y, z)});
  };

  const float sideX = mirrorWidth * 0.5f + frameThickness * 0.5f;
  const float bottomY = centerY - mirrorHeight * 0.5f - frameThickness * 0.5f;
  const float topY = centerY + mirrorHeight * 0.5f + frameThickness * 0.5f;
  const XMFLOAT3 leftPosition = localPosition(-sideX, centerY, 0.0f);
  const XMFLOAT3 rightPosition = localPosition(sideX, centerY, 0.0f);
  pushFrame(m_reflectionMonolithFrameMeshId, frameThickness, outerHeight,
            frameDepth, leftPosition.x, leftPosition.y, leftPosition.z,
            yawRadians);
  pushFrame(m_reflectionMonolithFrameMeshId, frameThickness, outerHeight,
            frameDepth, rightPosition.x, rightPosition.y, rightPosition.z,
            yawRadians);
  pushFrame(m_reflectionMonolithFrameMeshId, mirrorWidth, frameThickness,
            frameDepth, centerX, bottomY, centerZ, yawRadians);
  pushFrame(m_reflectionMonolithFrameMeshId, mirrorWidth, frameThickness,
            frameDepth, centerX, topY, centerZ, yawRadians);
  pushFrame(m_reflectionMonolithFrameMeshId, mirrorWidth + 1.4f, 0.30f, 1.25f,
            centerX, 0.15f, centerZ, yawRadians);
}
XMFLOAT3 OverworldScene::PlayerSpawnPosition() const {
  return {0.0f, 0.0f, -18.0f};
}

XMFLOAT3 OverworldScene::BossWarpPosition() const {
  return {0.0f, 0.0f, 22.0f};
}

bool OverworldScene::IsPlayerInsideBossWarp(
    const XMFLOAT3 &playerPosition) const {
  const XMFLOAT3 warpPos = BossWarpPosition();
  const float dx = playerPosition.x - warpPos.x;
  const float dz = playerPosition.z - warpPos.z;
  return dx * dx + dz * dz <=
         kBossWarpRadiusMeters * kBossWarpRadiusMeters;
}

void OverworldScene::SetWaterTransparency(DxContext &dx, float transparency) {
  if (m_waterMeshId == UINT32_MAX)
    return;

  const float t = std::clamp(transparency, 0.0f, 0.95f);
  Material &mat = dx.GetMeshRenderer().GetMeshMaterial(m_waterMeshId);
  mat.baseColorFactor.w = 1.0f - t;
  mat.baseColorFactor.x = std::lerp(0.34f, 0.18f, t);
  mat.baseColorFactor.y = std::lerp(0.58f, 0.44f, t);
  mat.baseColorFactor.z = std::lerp(0.66f, 0.55f, t);
  mat.roughnessFactor = std::lerp(0.12f, 0.055f, t);
  mat.emissiveFactor = {0.0f, std::lerp(0.04f, 0.018f, t),
                        std::lerp(0.08f, 0.035f, t)};
}

std::vector<OverworldScene::CollisionShapeConfig>
OverworldScene::BuildDefaultCollisionShapes() const {
  constexpr float wallThickness = 0.7f;
  constexpr float wallHeight = 4.8f;
  constexpr float wallLength = kCastleWallHalfExtentMeters * 2.0f;
  constexpr float gateGapWidth = kCastleGateHalfWidthMeters * 2.0f;
  constexpr float northWallSegmentLength = (wallLength - gateGapWidth) * 0.5f;
  constexpr float northWallSegmentCenterX =
      kCastleGateHalfWidthMeters + northWallSegmentLength * 0.5f;
  constexpr float towerSize = 1.8f;
  constexpr float towerHeight = 5.8f;

  auto makeBox = [](const std::string &label, float sx, float sy, float sz,
                    float x, float y, float z) {
    CollisionShapeConfig box{};
    box.label = label;
    box.shape = CollisionSystem::ShapeType::Box;
    box.center = {x, y, z};
    box.size = {sx, sy, sz};
    box.yawRadians = 0.0f;
    box.enabled = true;
    return box;
  };

  std::vector<CollisionShapeConfig> boxes;
  boxes.reserve(16);
  boxes.push_back(makeBox("South Wall", wallLength, wallHeight, wallThickness,
                          0.0f, wallHeight * 0.5f,
                          -kCastleWallHalfExtentMeters));
  boxes.push_back(makeBox("East Wall", wallThickness, wallHeight, wallLength,
                          kCastleWallHalfExtentMeters, wallHeight * 0.5f,
                          0.0f));
  boxes.push_back(makeBox("West Wall", wallThickness, wallHeight, wallLength,
                          -kCastleWallHalfExtentMeters, wallHeight * 0.5f,
                          0.0f));
  boxes.push_back(makeBox("North Wall L", northWallSegmentLength, wallHeight,
                          wallThickness, -northWallSegmentCenterX,
                          wallHeight * 0.5f, kCastleWallHalfExtentMeters));
  boxes.push_back(makeBox("North Wall R", northWallSegmentLength, wallHeight,
                          wallThickness, northWallSegmentCenterX,
                          wallHeight * 0.5f, kCastleWallHalfExtentMeters));
  boxes.push_back(makeBox("North Gate Block", gateGapWidth, wallHeight,
                          wallThickness, 0.0f, wallHeight * 0.5f,
                          kCastleWallHalfExtentMeters));
  // Castle Gate の glTF は描画モデルで、現時点では mesh collider を持たない。
  // Chihiro が実機調整した門まわりの既定値。門扉は North Gate Block で塞ぐ。
  boxes.push_back(makeBox("Gate Pillar L", 1.0f, 2.0f, 1.0f, -4.0f, 3.0f,
                          25.0f));
  boxes.back().shape = CollisionSystem::ShapeType::Circle;
  boxes.push_back(makeBox("Gate Pillar R", 1.0f, 2.0f, 1.0f, 4.0f, 3.0f,
                          25.0f));
  boxes.back().shape = CollisionSystem::ShapeType::Circle;
  boxes.push_back(makeBox("Gate Base L", 0.5f, 6.0f, 0.5f, -1.8f, 3.0f,
                          24.0f));
  boxes.push_back(makeBox("Gate Base R", 0.5f, 6.0f, 0.5f, 1.8f, 3.0f,
                          24.0f));
  boxes.push_back(makeBox("Tower SW", towerSize, towerHeight, towerSize,
                          -kCastleWallHalfExtentMeters, towerHeight * 0.5f,
                          -kCastleWallHalfExtentMeters));
  boxes.push_back(makeBox("Tower SE", towerSize, towerHeight, towerSize,
                          kCastleWallHalfExtentMeters, towerHeight * 0.5f,
                          -kCastleWallHalfExtentMeters));
  boxes.push_back(makeBox("Tower NW", towerSize, towerHeight, towerSize,
                          -kCastleWallHalfExtentMeters, towerHeight * 0.5f,
                          kCastleWallHalfExtentMeters));
  boxes.push_back(makeBox("Tower NE", towerSize, towerHeight, towerSize,
                          kCastleWallHalfExtentMeters, towerHeight * 0.5f,
                          kCastleWallHalfExtentMeters));
  boxes.push_back(
      makeBox("反写の大鏡", 9.0f, 6.3f, 0.70f, -18.0f, 3.15f, 16.0f));
  boxes.back().yawRadians = -24.0f * XM_PI / 180.0f;
  return boxes;
}

std::vector<CollisionSystem::Aabb> OverworldScene::BuildCollisionAabbs() const {
  const std::vector<CollisionShapeConfig> collisionShapes =
      BuildDefaultCollisionShapes();
  std::vector<CollisionSystem::Aabb> aabbs;
  aabbs.reserve(collisionShapes.size());
  for (const CollisionShapeConfig &box : collisionShapes) {
    if (!box.enabled)
      continue;

    const XMFLOAT3 half = {box.size.x * 0.5f, box.size.y * 0.5f,
                           box.size.z * 0.5f};
    CollisionSystem::Aabb aabb{};
    aabb.min = {box.center.x - half.x, box.center.y - half.y,
                box.center.z - half.z};
    aabb.max = {box.center.x + half.x, box.center.y + half.y,
                box.center.z + half.z};
    aabbs.push_back(aabb);
  }
  return aabbs;
}

std::vector<CollisionSystem::Collider> OverworldScene::BuildCollisionColliders(
    const std::vector<CollisionShapeConfig> &collisionShapes) const {
  std::vector<CollisionSystem::Collider> colliders;
  colliders.reserve(collisionShapes.size());
  for (const CollisionShapeConfig &shape : collisionShapes) {
    CollisionSystem::Collider collider{};
    collider.shape = shape.shape;
    collider.center = shape.center;
    collider.size = shape.size;
    collider.yawRadians = shape.yawRadians;
    collider.enabled = shape.enabled;
    colliders.push_back(collider);
  }
  return colliders;
}

void OverworldScene::AppendCollisionDebugLines(
    FrameData &frame,
    const std::vector<CollisionSystem::Collider> &collisionColliders) const {
  const XMFLOAT4 wallColor = {1.0f, 0.12f, 0.05f, 1.0f};
  constexpr float y = 0.08f;

  auto rotate2 = [](float x, float z, float yaw) {
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    return XMFLOAT2{x * c - z * s, x * s + z * c};
  };
  auto toWorld = [&](const CollisionSystem::Collider &c, float x, float z) {
    const XMFLOAT2 r = rotate2(x, z, c.yawRadians);
    return XMFLOAT3{c.center.x + r.x, y, c.center.z + r.y};
  };
  auto pushLoop = [&](const std::vector<XMFLOAT3> &points,
                      const XMFLOAT4 &color) {
    if (points.size() < 2)
      return;
    for (size_t i = 0; i < points.size(); ++i)
      PushDebugLine(frame, points[i], points[(i + 1) % points.size()], color);
  };

  for (const CollisionSystem::Collider &collider : collisionColliders) {
    if (!collider.enabled)
      continue;

    if (collider.shape == CollisionSystem::ShapeType::Circle) {
      std::vector<XMFLOAT3> points;
      constexpr int segments = 24;
      const float r = std::max(collider.size.x, collider.size.z) * 0.5f;
      points.reserve(segments);
      for (int i = 0; i < segments; ++i) {
        const float t = (static_cast<float>(i) / segments) * XM_2PI;
        points.push_back({collider.center.x + std::cos(t) * r, y,
                          collider.center.z + std::sin(t) * r});
      }
      pushLoop(points, wallColor);
    } else if (collider.shape == CollisionSystem::ShapeType::Triangle) {
      const float hx = collider.size.x * 0.5f;
      const float hz = collider.size.z * 0.5f;
      pushLoop({toWorld(collider, 0.0f, hz), toWorld(collider, -hx, -hz),
                toWorld(collider, hx, -hz)},
               wallColor);
    } else {
      const float hx = collider.size.x * 0.5f;
      const float hz = collider.size.z * 0.5f;
      pushLoop({toWorld(collider, -hx, -hz), toWorld(collider, hx, -hz),
                toWorld(collider, hx, hz), toWorld(collider, -hx, hz)},
               wallColor);
    }
  }
}

void OverworldScene::AppendStageCollisionDebugLines(FrameData &frame) const {
  const XMFLOAT4 meshColor = {0.1f, 0.75f, 1.0f, 1.0f};
  constexpr float y = 0.10f;

  for (const CollisionSystem::MeshTriangle &tri : m_stageCollisionTriangles) {
    const XMFLOAT3 a = {tri.a.x, y, tri.a.y};
    const XMFLOAT3 b = {tri.b.x, y, tri.b.y};
    const XMFLOAT3 c = {tri.c.x, y, tri.c.y};
    PushDebugLine(frame, a, b, meshColor);
    PushDebugLine(frame, b, c, meshColor);
    PushDebugLine(frame, c, a, meshColor);
  }
}

bool OverworldScene::ReloadPlacements(DxContext &dx) {
  const std::string resolvedPlacementPath =
      ResolveLiveAssetPath(m_placementPath);
  std::ifstream file(resolvedPlacementPath);
  if (!file) {
    OutputDebugStringA("[OverworldScene] WARNING: placement JSON not found: ");
    OutputDebugStringA(resolvedPlacementPath.c_str());
    OutputDebugStringA("\n");
    return false;
  }

  json root;
  try {
    file >> root;
  } catch (const std::exception &e) {
    OutputDebugStringA("[OverworldScene] WARNING: placement JSON parse failed: ");
    OutputDebugStringA(e.what());
    OutputDebugStringA("\n");
    return false;
  }

  if (!root.contains("objects") || !root["objects"].is_array()) {
    OutputDebugStringA("[OverworldScene] WARNING: placement JSON has no objects array.\n");
    return false;
  }

  std::vector<StageObject> newObjects;
  m_stageCollisionTriangles.clear();
  for (const json &entry : root["objects"]) {
    if (!entry.contains("model") || !entry["model"].is_string())
      continue;

    StageObject placement{};
    ReadFloat3(entry, "position", placement.position);

    if (entry.contains("scale") && entry["scale"].is_number()) {
      const float s = entry["scale"].get<float>();
      placement.scale = {s, s, s};
    } else {
      ReadFloat3(entry, "scale", placement.scale);
    }

    const float yawDegrees = entry.value("yawDegrees", 0.0f);
    placement.yawRadians = yawDegrees * XM_PI / 180.0f;

    AppendStageObject(dx, entry["model"].get<std::string>(), placement,
                      newObjects);
  }

  m_stageObjects = std::move(newObjects);

  OutputDebugStringA("[OverworldScene] placement JSON reloaded: ");
  OutputDebugStringA(resolvedPlacementPath.c_str());
  OutputDebugStringA("\n");
  return true;
}

OverworldScene::StageModel *
OverworldScene::FindOrLoadModel(DxContext &dx, const std::string &path) {
  const std::string resolvedPath = ResolveLiveAssetPath(path);
  for (StageModel &model : m_stageModels) {
    if (model.path == resolvedPath)
      return &model;
  }

  std::vector<LoadedMeshPart> parts;
  if (!LoadStaticModelParts(resolvedPath, parts)) {
    OutputDebugStringA("[OverworldScene] WARNING: glTF load failed: ");
    OutputDebugStringA(resolvedPath.c_str());
    OutputDebugStringA("\n");
    return nullptr;
  }

  StageModel model{};
  model.path = resolvedPath;
  for (const LoadedMeshPart &part : parts) {
    const uint32_t meshId =
        dx.CreateMeshResources(part.mesh, part.GetMaterialImages(),
                               part.material);
    if (meshId == UINT32_MAX) {
      OutputDebugStringA("[OverworldScene] WARNING: mesh part upload failed: ");
      OutputDebugStringA(resolvedPath.c_str());
      OutputDebugStringA("\n");
      continue;
    }
    model.meshIds.push_back(meshId);
    model.collisionMeshes.push_back(part.mesh);
  }

  if (!model.meshIds.empty()) {
    OutputDebugStringA("[OverworldScene] Stage object loaded with materials: ");
    OutputDebugStringA(resolvedPath.c_str());
    OutputDebugStringA("\n");
    m_stageModels.push_back(std::move(model));
    return &m_stageModels.back();
  }

  return nullptr;
}

bool OverworldScene::AppendStageObject(DxContext &dx, const std::string &path,
                                       const StageObject &placement,
                                       std::vector<StageObject> &outObjects) {
  StageModel *model = FindOrLoadModel(dx, path);
  if (!model)
    return false;

  for (uint32_t meshId : model->meshIds) {
    StageObject object = placement;
    object.meshId = meshId;
    outObjects.push_back(object);
  }
  AppendStageObjectCollision(*model, placement);
  return true;
}

void OverworldScene::AppendStageObjectCollision(const StageModel &model,
                                                const StageObject &placement) {
  constexpr float minProjectedArea = 0.0025f;

  for (const LoadedMesh &mesh : model.collisionMeshes) {
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
      const uint32_t i0 = mesh.indices[i + 0];
      const uint32_t i1 = mesh.indices[i + 1];
      const uint32_t i2 = mesh.indices[i + 2];
      if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() ||
          i2 >= mesh.vertices.size())
        continue;

      const MeshVertex &v0 = mesh.vertices[i0];
      const MeshVertex &v1 = mesh.vertices[i1];
      const MeshVertex &v2 = mesh.vertices[i2];
      const XMFLOAT3 p0 =
          TransformStagePoint({v0.pos[0], v0.pos[1], v0.pos[2]},
                              placement.position, placement.scale,
                              placement.yawRadians);
      const XMFLOAT3 p1 =
          TransformStagePoint({v1.pos[0], v1.pos[1], v1.pos[2]},
                              placement.position, placement.scale,
                              placement.yawRadians);
      const XMFLOAT3 p2 =
          TransformStagePoint({v2.pos[0], v2.pos[1], v2.pos[2]},
                              placement.position, placement.scale,
                              placement.yawRadians);

      CollisionSystem::MeshTriangle tri{};
      tri.a = {p0.x, p0.z};
      tri.b = {p1.x, p1.z};
      tri.c = {p2.x, p2.z};
      if (TriangleArea2D(tri.a, tri.b, tri.c) < minProjectedArea)
        continue;
      m_stageCollisionTriangles.push_back(tri);
    }
  }
}

void OverworldScene::BuildBackgroundForest(DxContext &dx) {
  m_backgroundForestMeshIds.clear();
  m_backgroundForestClusters.clear();

  StageModel *model =
      FindOrLoadModel(dx, "Assets/models/low_poly_forest_tree_pack.glb");
  if (!model) {
    OutputDebugStringA(
        "[OverworldScene] WARNING: background forest pack not loaded.\n");
    return;
  }

  m_backgroundForestMeshIds = model->meshIds;
  if (m_backgroundForestMeshIds.empty())
    return;

  // 城外の到達不能エリアを森林クラスターで塞ぎ、境界の空白感を減らす。
  constexpr float nearBand = kCastleWallHalfExtentMeters + 2.2f;
  constexpr float midBand = kCastleWallHalfExtentMeters + 8.0f;
  constexpr float farBand = kCastleWallHalfExtentMeters + 15.0f;

  for (int i = -5; i <= 5; ++i) {
    const float x = static_cast<float>(i) * 5.4f;
    if (std::abs(x) > kCastleGateHalfWidthMeters + 3.5f) {
      AppendBackgroundForestCluster(x, nearBand, 0.55f + 0.03f * (i & 1),
                                    17.0f * static_cast<float>(i), 0);
    }
    AppendBackgroundForestCluster(x, -nearBand, 0.58f + 0.02f * (i & 1),
                                  23.0f * static_cast<float>(i), 0);
  }

  for (int i = -4; i <= 4; ++i) {
    const float z = static_cast<float>(i) * 6.0f;
    AppendBackgroundForestCluster(-nearBand, z, 0.56f + 0.025f * (i & 1),
                                  31.0f * static_cast<float>(i), 1);
    AppendBackgroundForestCluster(nearBand, z, 0.54f + 0.025f * (i & 1),
                                  -29.0f * static_cast<float>(i), 1);
  }

  for (int i = -4; i <= 4; ++i) {
    const float x = static_cast<float>(i) * 8.0f;
    AppendBackgroundForestCluster(x, farBand, 0.72f, 41.0f * i, 2);
    AppendBackgroundForestCluster(x, -farBand, 0.72f, -37.0f * i, 2);
  }

  for (int i = -3; i <= 3; ++i) {
    const float z = static_cast<float>(i) * 9.0f;
    AppendBackgroundForestCluster(-midBand, z, 0.64f, 53.0f * i, 3);
    AppendBackgroundForestCluster(midBand, z, 0.64f, -47.0f * i, 3);
  }

  OutputDebugStringA("[OverworldScene] background forest initialized.\n");
}

void OverworldScene::AppendBackgroundForestCluster(float x, float z,
                                                   float scale,
                                                   float yawDegrees,
                                                   int densityTier) {
  BackgroundForestCluster cluster{};
  cluster.position = {x, 0.0f, z};
  cluster.baseScale = scale;
  cluster.yawRadians = yawDegrees * XM_PI / 180.0f;
  cluster.densityTier = densityTier;
  m_backgroundForestClusters.push_back(cluster);
}
