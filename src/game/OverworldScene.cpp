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
  m_ready = (m_floorMeshId != UINT32_MAX);

  if (m_ready) {
    OutputDebugStringA("[OverworldScene] 60m x 60m floor initialized.\n");
  } else {
    OutputDebugStringA("[OverworldScene] FAILED: floor mesh was not created.\n");
  }

  ReloadPlacements(dx);
}

void OverworldScene::BuildFrame(FrameData &frame) const {
  if (!m_ready)
    return;

  const XMMATRIX floorWorld = XMMatrixTranslation(0.0f, 0.0f, 0.0f);
  frame.opaqueItems.push_back({m_floorMeshId, floorWorld});

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
