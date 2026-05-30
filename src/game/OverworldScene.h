#pragma once

#include "DxContext.h"
#include "RenderPass.h"

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

class OverworldScene {
public:
  static constexpr float kFloorSizeMeters = 60.0f;

  void Initialize(DxContext &dx);
  void BuildFrame(FrameData &frame) const;
  bool ReloadPlacements(DxContext &dx);

  bool IsReady() const { return m_ready; }
  const std::string &PlacementPath() const { return m_placementPath; }
  size_t ObjectCount() const { return m_stageObjects.size(); }

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

  bool AppendStageObject(DxContext &dx, const std::string &path,
                         const StageObject &placement,
                         std::vector<StageObject> &outObjects);
  StageModel *FindOrLoadModel(DxContext &dx, const std::string &path);

  uint32_t m_floorMeshId = UINT32_MAX;
  std::vector<StageObject> m_stageObjects;
  std::vector<StageModel> m_stageModels;
  std::string m_placementPath = "Assets/scenes/overworld_placements.json";
  bool m_ready = false;
};
