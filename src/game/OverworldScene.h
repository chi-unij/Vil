#pragma once

#include "DxContext.h"
#include "GltfLoader.h"
#include "RenderPass.h"
#include "game/CollisionSystem.h"

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

class OverworldScene {
public:
  static constexpr float kFloorSizeMeters = 60.0f; // Floor size
  static constexpr float kPlayableHalfExtentMeters = 24.0f; // player movement allow range
  static constexpr float kCastleWallHalfExtentMeters = 25.5f; // castle wall range
  static constexpr float kCastleGateHalfWidthMeters = 4.0f; // Castle gate range
  static constexpr float kBossWarpRadiusMeters = 1.45f;
  static constexpr float kTavernInteractionRadiusMeters = 2.35f;

  struct BackgroundForestDebugSettings {
    bool enabled = true;
    bool singleClusterPreview = false;
    int densityLevel = 3;
    float scaleMultiplier = 0.40f;
    float distanceOffset = 32.0f;
    float spacingMultiplier = 0.5f;
  };

  struct CollisionShapeConfig {
    std::string label;
    CollisionSystem::ShapeType shape = CollisionSystem::ShapeType::Box;
    DirectX::XMFLOAT3 center = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 size = {1.0f, 1.0f, 1.0f};
    float yawRadians = 0.0f;
    bool enabled = true;
  };

  void Initialize(DxContext &dx);
  void BuildFrame(FrameData &frame) const;
  DirectX::XMFLOAT3 PlayerSpawnPosition() const;
  DirectX::XMFLOAT3 BossWarpPosition() const;
  bool IsPlayerInsideBossWarp(const DirectX::XMFLOAT3 &playerPosition) const;
  DirectX::XMFLOAT3 TavernEntrancePosition() const;
  DirectX::XMFLOAT3 TavernReturnPosition() const;
  bool IsPlayerNearTavernEntrance(
      const DirectX::XMFLOAT3 &playerPosition) const;
  void SetWaterTransparency(DxContext &dx, float transparency);
  bool ReloadPlacements(DxContext &dx);
  std::vector<CollisionShapeConfig> BuildDefaultCollisionShapes() const;
  std::vector<CollisionSystem::Aabb> BuildCollisionAabbs() const;
  std::vector<CollisionSystem::Collider> BuildCollisionColliders(
      const std::vector<CollisionShapeConfig> &collisionShapes) const;
  void AppendCollisionDebugLines(
      FrameData &frame,
      const std::vector<CollisionSystem::Collider> &collisionColliders) const;
  const std::vector<CollisionSystem::MeshTriangle> &StageCollisionTriangles()
      const {
    return m_stageCollisionTriangles;
  }
  void AppendStageCollisionDebugLines(FrameData &frame) const;

  bool IsReady() const { return m_ready; }
  const std::string &PlacementPath() const { return m_placementPath; }
  size_t ObjectCount() const { return m_stageObjects.size(); }
  size_t BackgroundForestClusterCount() const {
    return m_backgroundForestClusters.size();
  }
  size_t BackgroundForestMeshPartCount() const {
    return m_backgroundForestMeshIds.size();
  }
  BackgroundForestDebugSettings &BackgroundForestDebug() {
    return m_backgroundForestDebug;
  }
  const BackgroundForestDebugSettings &BackgroundForestDebug() const {
    return m_backgroundForestDebug;
  }

private:
  struct StageObject {
    uint32_t meshId = UINT32_MAX;
    DirectX::XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 scale = {1.0f, 1.0f, 1.0f};
    float yawRadians = 0.0f;
  };

  struct StageModel {
    std::string path;
    std::vector<uint32_t> meshIds;
    std::vector<LoadedMesh> collisionMeshes;
  };

  struct BackgroundForestCluster {
    DirectX::XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
    float baseScale = 1.0f;
    float yawRadians = 0.0f;
    int densityTier = 0;
  };

  bool AppendStageObject(DxContext &dx, const std::string &path,
                         const StageObject &placement,
                         std::vector<StageObject> &outObjects);
  void AppendStageObjectCollision(const StageModel &model,
                                  const StageObject &placement);
  StageModel *FindOrLoadModel(DxContext &dx, const std::string &path);
  void BuildBackgroundForest(DxContext &dx);
  void AppendWorldPolishProps(FrameData &frame) const;
  void AppendReflectionMonolith(FrameData &frame) const;
  void AppendTavernPlaceholder(FrameData &frame) const;
  void AppendBackgroundForestCluster(float x, float z, float scale,
                                     float yawDegrees, int densityTier);

  uint32_t m_floorMeshId = UINT32_MAX;
  uint32_t m_waterMeshId = UINT32_MAX;
  uint32_t m_castleWallMeshId = UINT32_MAX;
  uint32_t m_castleMerlonMeshId = UINT32_MAX;
  uint32_t m_castleTowerMeshId = UINT32_MAX;
  uint32_t m_bossWarpMarkerMeshId = UINT32_MAX;
  uint32_t m_pathStoneMeshId = UINT32_MAX;
  uint32_t m_lanternPostMeshId = UINT32_MAX;
  uint32_t m_lanternCapMeshId = UINT32_MAX;
  uint32_t m_lanternGlowMeshId = UINT32_MAX;
  uint32_t m_waystoneMeshId = UINT32_MAX;
  uint32_t m_reflectionMonolithMirrorMeshId = UINT32_MAX;
  uint32_t m_reflectionMonolithFrameMeshId = UINT32_MAX;
  uint32_t m_tavernWallMeshId = UINT32_MAX;
  uint32_t m_tavernRoofMeshId = UINT32_MAX;
  uint32_t m_tavernDoorMeshId = UINT32_MAX;
  uint32_t m_tavernGlowMeshId = UINT32_MAX;
  std::vector<uint32_t> m_shrineLanternMeshIds;
  std::vector<uint32_t> m_backgroundForestMeshIds;
  std::vector<BackgroundForestCluster> m_backgroundForestClusters;
  BackgroundForestDebugSettings m_backgroundForestDebug;
  std::vector<StageObject> m_stageObjects;
  std::vector<StageModel> m_stageModels;
  std::vector<CollisionSystem::MeshTriangle> m_stageCollisionTriangles;
  std::string m_placementPath = "Assets/scenes/overworld_placements.json";
  bool m_ready = false;
};
