#include "game/OverworldScene.h"

#include "GltfLoader.h"
#include "ProceduralMesh.h"

#include <DirectXMath.h>
#include <Windows.h>
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

  m_floorMeshId = dx.CreateMeshResources(floorMesh, images, floorMaterial);
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
  m_ready =
      (m_floorMeshId != UINT32_MAX && m_castleWallMeshId != UINT32_MAX &&
       m_castleMerlonMeshId != UINT32_MAX &&
       m_castleTowerMeshId != UINT32_MAX);

  if (m_ready) {
    OutputDebugStringA(
        "[OverworldScene] 60m x 60m floor and castle walls initialized.\n");
  } else {
    OutputDebugStringA(
        "[OverworldScene] FAILED: floor or castle wall mesh was not created.\n");
  }

  ReloadPlacements(dx);
}

void OverworldScene::BuildFrame(FrameData &frame) const {
  if (!m_ready)
    return;

  const XMMATRIX floorWorld = XMMatrixTranslation(0.0f, 0.0f, 0.0f);
  frame.opaqueItems.push_back({m_floorMeshId, floorWorld});

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
  return true;
}
