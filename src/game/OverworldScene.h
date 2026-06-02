#pragma once

#include "DxContext.h"
#include "RenderPass.h"

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

  struct BackgroundForestDebugSettings {
    bool enabled = true;
    bool singleClusterPreview = false;
    int densityLevel = 3;
    float scaleMultiplier = 0.40f;
    float distanceOffset = 32.0f;
    float spacingMultiplier = 0.5f;
  };

  void Initialize(DxContext &dx);
  void BuildFrame(FrameData &frame) const;
  bool ReloadPlacements(DxContext &dx);

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
  StageModel *FindOrLoadModel(DxContext &dx, const std::string &path);
  void BuildBackgroundForest(DxContext &dx);
  void AppendBackgroundForestCluster(float x, float z, float scale,
                                     float yawDegrees, int densityTier);

  uint32_t m_floorMeshId = UINT32_MAX;
  uint32_t m_castleWallMeshId = UINT32_MAX;
  uint32_t m_castleMerlonMeshId = UINT32_MAX;
  uint32_t m_castleTowerMeshId = UINT32_MAX;
  std::vector<uint32_t> m_backgroundForestMeshIds;
  std::vector<BackgroundForestCluster> m_backgroundForestClusters;
  BackgroundForestDebugSettings m_backgroundForestDebug;
  std::vector<StageObject> m_stageObjects;
  std::vector<StageModel> m_stageModels;
  std::string m_placementPath = "Assets/scenes/overworld_placements.json";
  bool m_ready = false;
};
