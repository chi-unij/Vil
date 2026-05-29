// ======================================
// File: Entity.cpp
// Purpose: Transform math + JSON serialization helpers for entity structs.
//          Uses manual JSON construction to avoid ADL issues with DirectX types.
// ======================================

#include "Entity.h"

#include <nlohmann/json.hpp>

using namespace DirectX;
using json = nlohmann::json;

// ---- Transform ----

XMMATRIX Transform::WorldMatrix() const {
  float pitchRad = XMConvertToRadians(rotation.x);
  float yawRad = XMConvertToRadians(rotation.y);
  float rollRad = XMConvertToRadians(rotation.z);

  XMMATRIX S = XMMatrixScaling(scale.x, scale.y, scale.z);
  // XYZ intrinsic order — matches ImGuizmo's DecomposeMatrixToComponents.
  // (NOT XMMatrixRotationRollPitchYaw which uses ZXY order and causes
  //  a feedback loop with ImGuizmo decomposition → spinning gizmo bug.)
  XMMATRIX R = XMMatrixRotationX(pitchRad) * XMMatrixRotationY(yawRad) *
               XMMatrixRotationZ(rollRad);
  XMMATRIX T = XMMatrixTranslation(position.x, position.y, position.z);

  return S * R * T;
}

// ---- Manual JSON helpers for DirectX types (no ADL) ----

static json Float2ToJson(const XMFLOAT2 &v) {
  return json::array({v.x, v.y});
}
static XMFLOAT2 JsonToFloat2(const json &j) {
  return {j[0].get<float>(), j[1].get<float>()};
}

static json Float3ToJson(const XMFLOAT3 &v) {
  return json::array({v.x, v.y, v.z});
}
static XMFLOAT3 JsonToFloat3(const json &j) {
  return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

static json Float4ToJson(const XMFLOAT4 &v) {
  return json::array({v.x, v.y, v.z, v.w});
}
static XMFLOAT4 JsonToFloat4(const json &j) {
  return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(),
          j[3].get<float>()};
}

// ---- MeshSourceType string conversion ----

static const char *MeshSourceTypeToString(MeshSourceType t) {
  switch (t) {
  case MeshSourceType::ProceduralCube:     return "ProceduralCube";
  case MeshSourceType::ProceduralPlane:    return "ProceduralPlane";
  case MeshSourceType::ProceduralCylinder: return "ProceduralCylinder";
  case MeshSourceType::ProceduralCone:     return "ProceduralCone";
  case MeshSourceType::ProceduralSphere:   return "ProceduralSphere";
  case MeshSourceType::GltfFile:           return "GltfFile";
  }
  return "ProceduralCube";
}

static MeshSourceType MeshSourceTypeFromString(const std::string &s) {
  if (s == "ProceduralPlane")    return MeshSourceType::ProceduralPlane;
  if (s == "ProceduralCylinder") return MeshSourceType::ProceduralCylinder;
  if (s == "ProceduralCone")     return MeshSourceType::ProceduralCone;
  if (s == "ProceduralSphere")   return MeshSourceType::ProceduralSphere;
  if (s == "GltfFile")           return MeshSourceType::GltfFile;
  return MeshSourceType::ProceduralCube;
}

// ---- Manual JSON: Transform ----

static json TransformToJson(const Transform &t) {
  json j;
  j["position"] = Float3ToJson(t.position);
  j["rotation"] = Float3ToJson(t.rotation);
  j["scale"] = Float3ToJson(t.scale);
  return j;
}
static Transform JsonToTransform(const json &j) {
  Transform t;
  t.position = JsonToFloat3(j.at("position"));
  t.rotation = JsonToFloat3(j.at("rotation"));
  t.scale = JsonToFloat3(j.at("scale"));
  return t;
}

// ---- Manual JSON: Material ----

static json MaterialToJson(const Material &m) {
  json j;
  j["baseColorFactor"] = Float4ToJson(m.baseColorFactor);
  j["metallicFactor"] = m.metallicFactor;
  j["roughnessFactor"] = m.roughnessFactor;
  j["emissiveFactor"] = Float3ToJson(m.emissiveFactor);
  j["uvTiling"] = Float2ToJson(m.uvTiling);
  j["uvOffset"] = Float2ToJson(m.uvOffset);
  j["pomEnabled"] = m.pomEnabled;
  j["heightScale"] = m.heightScale;
  j["pomMinLayers"] = m.pomMinLayers;
  j["pomMaxLayers"] = m.pomMaxLayers;
  return j;
}
static Material JsonToMaterial(const json &j) {
  Material m;
  if (j.contains("baseColorFactor"))
    m.baseColorFactor = JsonToFloat4(j["baseColorFactor"]);
  if (j.contains("metallicFactor"))
    m.metallicFactor = j["metallicFactor"].get<float>();
  if (j.contains("roughnessFactor"))
    m.roughnessFactor = j["roughnessFactor"].get<float>();
  if (j.contains("emissiveFactor"))
    m.emissiveFactor = JsonToFloat3(j["emissiveFactor"]);
  if (j.contains("uvTiling"))
    m.uvTiling = JsonToFloat2(j["uvTiling"]);
  if (j.contains("uvOffset"))
    m.uvOffset = JsonToFloat2(j["uvOffset"]);
  if (j.contains("pomEnabled"))
    m.pomEnabled = j["pomEnabled"].get<bool>();
  if (j.contains("heightScale"))
    m.heightScale = j["heightScale"].get<float>();
  if (j.contains("pomMinLayers"))
    m.pomMinLayers = j["pomMinLayers"].get<float>();
  if (j.contains("pomMaxLayers"))
    m.pomMaxLayers = j["pomMaxLayers"].get<float>();
  return m;
}

// ---- Manual JSON: MeshComponent ----

static json MeshComponentToJson(const MeshComponent &c) {
  json j;
  j["sourceType"] = MeshSourceTypeToString(c.sourceType);
  j["gltfPath"] = c.gltfPath;
  j["size"] = c.size;
  j["width"] = c.width;
  j["height"] = c.height;
  j["segments"] = c.segments;
  j["rings"] = c.rings;
  j["material"] = MaterialToJson(c.material);
  json texArr = json::array();
  for (int i = 0; i < 6; ++i)
    texArr.push_back(c.texturePaths[i]);
  j["texturePaths"] = texArr;
  return j;
}
static MeshComponent JsonToMeshComponent(const json &j) {
  MeshComponent c;
  c.sourceType = MeshSourceTypeFromString(j.at("sourceType").get<std::string>());
  c.gltfPath = j.at("gltfPath").get<std::string>();
  c.size = j.at("size").get<float>();
  c.width = j.at("width").get<float>();
  c.height = j.at("height").get<float>();
  c.segments = j.at("segments").get<uint32_t>();
  c.rings = j.at("rings").get<uint32_t>();
  c.material = JsonToMaterial(j.at("material"));
  if (j.contains("texturePaths") && j["texturePaths"].is_array()) {
    const auto &arr = j["texturePaths"];
    for (size_t i = 0; i < 6 && i < arr.size(); ++i)
      c.texturePaths[i] = arr[i].get<std::string>();
  }
  c.meshId = UINT32_MAX; // runtime only — recreated on load
  return c;
}

// ---- Manual JSON: PointLightComponent ----

static json PointLightToJson(const PointLightComponent &c) {
  json j;
  j["range"] = c.range;
  j["color"] = Float3ToJson(c.color);
  j["intensity"] = c.intensity;
  j["enabled"] = c.enabled;
  return j;
}
static PointLightComponent JsonToPointLight(const json &j) {
  PointLightComponent c;
  c.range = j.at("range").get<float>();
  c.color = JsonToFloat3(j.at("color"));
  c.intensity = j.at("intensity").get<float>();
  c.enabled = j.at("enabled").get<bool>();
  return c;
}

// ---- Manual JSON: SpotLightComponent ----

static json SpotLightToJson(const SpotLightComponent &c) {
  json j;
  j["direction"] = Float3ToJson(c.direction);
  j["range"] = c.range;
  j["color"] = Float3ToJson(c.color);
  j["intensity"] = c.intensity;
  j["innerAngleDeg"] = c.innerAngleDeg;
  j["outerAngleDeg"] = c.outerAngleDeg;
  j["enabled"] = c.enabled;
  return j;
}
static SpotLightComponent JsonToSpotLight(const json &j) {
  SpotLightComponent c;
  c.direction = JsonToFloat3(j.at("direction"));
  c.range = j.at("range").get<float>();
  c.color = JsonToFloat3(j.at("color"));
  c.intensity = j.at("intensity").get<float>();
  c.innerAngleDeg = j.at("innerAngleDeg").get<float>();
  c.outerAngleDeg = j.at("outerAngleDeg").get<float>();
  c.enabled = j.at("enabled").get<bool>();
  return c;
}

// ---- Public JSON: SceneLightSettings ----

json SceneLightSettingsToJson(const SceneLightSettings &s) {
  json j;
  j["lightDir"] = Float3ToJson(s.lightDir);
  j["lightIntensity"] = s.lightIntensity;
  j["lightColor"] = Float3ToJson(s.lightColor);
  j["iblIntensity"] = s.iblIntensity;
  return j;
}

SceneLightSettings JsonToSceneLightSettings(const json &j) {
  SceneLightSettings s;
  if (j.contains("lightDir"))
    s.lightDir = JsonToFloat3(j["lightDir"]);
  if (j.contains("lightIntensity"))
    s.lightIntensity = j["lightIntensity"].get<float>();
  if (j.contains("lightColor"))
    s.lightColor = JsonToFloat3(j["lightColor"]);
  if (j.contains("iblIntensity"))
    s.iblIntensity = j["iblIntensity"].get<float>();
  return s;
}

// ---- Public JSON: SceneShadowSettings (Phase 4) ----

json SceneShadowSettingsToJson(const SceneShadowSettings &s) {
  json j;
  j["shadowsEnabled"] = s.shadowsEnabled;
  j["shadowBias"] = s.shadowBias;
  j["shadowStrength"] = s.shadowStrength;
  j["csmLambda"] = s.csmLambda;
  j["csmMaxDistance"] = s.csmMaxDistance;
  j["csmDebugCascades"] = s.csmDebugCascades;
  j["ssaoEnabled"] = s.ssaoEnabled;
  j["ssaoRadius"] = s.ssaoRadius;
  j["ssaoBias"] = s.ssaoBias;
  j["ssaoPower"] = s.ssaoPower;
  j["ssaoKernelSize"] = s.ssaoKernelSize;
  j["ssaoStrength"] = s.ssaoStrength;
  return j;
}

SceneShadowSettings JsonToSceneShadowSettings(const json &j) {
  SceneShadowSettings s;
  if (j.contains("shadowsEnabled"))
    s.shadowsEnabled = j["shadowsEnabled"].get<bool>();
  if (j.contains("shadowBias"))
    s.shadowBias = j["shadowBias"].get<float>();
  if (j.contains("shadowStrength"))
    s.shadowStrength = j["shadowStrength"].get<float>();
  if (j.contains("csmLambda"))
    s.csmLambda = j["csmLambda"].get<float>();
  if (j.contains("csmMaxDistance"))
    s.csmMaxDistance = j["csmMaxDistance"].get<float>();
  if (j.contains("csmDebugCascades"))
    s.csmDebugCascades = j["csmDebugCascades"].get<bool>();
  if (j.contains("ssaoEnabled"))
    s.ssaoEnabled = j["ssaoEnabled"].get<bool>();
  if (j.contains("ssaoRadius"))
    s.ssaoRadius = j["ssaoRadius"].get<float>();
  if (j.contains("ssaoBias"))
    s.ssaoBias = j["ssaoBias"].get<float>();
  if (j.contains("ssaoPower"))
    s.ssaoPower = j["ssaoPower"].get<float>();
  if (j.contains("ssaoKernelSize"))
    s.ssaoKernelSize = j["ssaoKernelSize"].get<int>();
  if (j.contains("ssaoStrength"))
    s.ssaoStrength = j["ssaoStrength"].get<float>();
  return s;
}

// ---- Public JSON: ScenePostProcessSettings (Phase 4) ----

json ScenePostProcessSettingsToJson(const ScenePostProcessSettings &s) {
  json j;
  j["exposure"] = s.exposure;
  j["bloomEnabled"] = s.bloomEnabled;
  j["bloomThreshold"] = s.bloomThreshold;
  j["bloomIntensity"] = s.bloomIntensity;
  j["taaEnabled"] = s.taaEnabled;
  j["taaBlendFactor"] = s.taaBlendFactor;
  j["fxaaEnabled"] = s.fxaaEnabled;
  j["motionBlurEnabled"] = s.motionBlurEnabled;
  j["motionBlurStrength"] = s.motionBlurStrength;
  j["motionBlurSamples"] = s.motionBlurSamples;
  j["dofEnabled"] = s.dofEnabled;
  j["dofFocalDistance"] = s.dofFocalDistance;
  j["dofFocalRange"] = s.dofFocalRange;
  j["dofMaxBlur"] = s.dofMaxBlur;
  return j;
}

ScenePostProcessSettings JsonToScenePostProcessSettings(const json &j) {
  ScenePostProcessSettings s;
  if (j.contains("exposure"))
    s.exposure = j["exposure"].get<float>();
  if (j.contains("bloomEnabled"))
    s.bloomEnabled = j["bloomEnabled"].get<bool>();
  if (j.contains("bloomThreshold"))
    s.bloomThreshold = j["bloomThreshold"].get<float>();
  if (j.contains("bloomIntensity"))
    s.bloomIntensity = j["bloomIntensity"].get<float>();
  if (j.contains("taaEnabled"))
    s.taaEnabled = j["taaEnabled"].get<bool>();
  if (j.contains("taaBlendFactor"))
    s.taaBlendFactor = j["taaBlendFactor"].get<float>();
  if (j.contains("fxaaEnabled"))
    s.fxaaEnabled = j["fxaaEnabled"].get<bool>();
  if (j.contains("motionBlurEnabled"))
    s.motionBlurEnabled = j["motionBlurEnabled"].get<bool>();
  if (j.contains("motionBlurStrength"))
    s.motionBlurStrength = j["motionBlurStrength"].get<float>();
  if (j.contains("motionBlurSamples"))
    s.motionBlurSamples = j["motionBlurSamples"].get<int>();
  if (j.contains("dofEnabled"))
    s.dofEnabled = j["dofEnabled"].get<bool>();
  if (j.contains("dofFocalDistance"))
    s.dofFocalDistance = j["dofFocalDistance"].get<float>();
  if (j.contains("dofFocalRange"))
    s.dofFocalRange = j["dofFocalRange"].get<float>();
  if (j.contains("dofMaxBlur"))
    s.dofMaxBlur = j["dofMaxBlur"].get<float>();
  return s;
}

// ---- Public JSON: Entity ----

json EntityToJson(const Entity &e) {
  json j;
  j["id"] = e.id;
  j["name"] = e.name;
  j["active"] = e.active;
  j["transform"] = TransformToJson(e.transform);

  if (e.mesh.has_value())
    j["mesh"] = MeshComponentToJson(e.mesh.value());
  else
    j["mesh"] = nullptr;

  if (e.pointLight.has_value())
    j["pointLight"] = PointLightToJson(e.pointLight.value());
  else
    j["pointLight"] = nullptr;

  if (e.spotLight.has_value())
    j["spotLight"] = SpotLightToJson(e.spotLight.value());
  else
    j["spotLight"] = nullptr;

  return j;
}

Entity JsonToEntity(const json &j) {
  Entity e;
  e.id = j.at("id").get<EntityId>();
  e.name = j.at("name").get<std::string>();
  e.active = j.at("active").get<bool>();
  e.transform = JsonToTransform(j.at("transform"));

  if (j.contains("mesh") && !j["mesh"].is_null())
    e.mesh = JsonToMeshComponent(j["mesh"]);
  else
    e.mesh.reset();

  if (j.contains("pointLight") && !j["pointLight"].is_null())
    e.pointLight = JsonToPointLight(j["pointLight"]);
  else
    e.pointLight.reset();

  if (j.contains("spotLight") && !j["spotLight"].is_null())
    e.spotLight = JsonToSpotLight(j["spotLight"]);
  else
    e.spotLight.reset();

  return e;
}

// ---- Camera Preset serialization (Phase 6) ----

json CameraPresetToJson(const CameraPreset &p) {
  json j;
  j["name"] = p.name;
  j["position"] = {p.position.x, p.position.y, p.position.z};
  j["yaw"] = p.yaw;
  j["pitch"] = p.pitch;
  j["fovY"] = p.fovY;
  j["nearZ"] = p.nearZ;
  j["farZ"] = p.farZ;
  j["mode"] = static_cast<int>(p.mode);
  j["orbitTarget"] = {p.orbitTarget.x, p.orbitTarget.y, p.orbitTarget.z};
  j["orbitDistance"] = p.orbitDistance;
  j["orbitYaw"] = p.orbitYaw;
  j["orbitPitch"] = p.orbitPitch;
  return j;
}

CameraPreset JsonToCameraPreset(const json &j) {
  CameraPreset p;
  if (j.contains("name"))
    p.name = j["name"].get<std::string>();
  if (j.contains("position") && j["position"].is_array() && j["position"].size() >= 3) {
    p.position.x = j["position"][0].get<float>();
    p.position.y = j["position"][1].get<float>();
    p.position.z = j["position"][2].get<float>();
  }
  if (j.contains("yaw"))    p.yaw   = j["yaw"].get<float>();
  if (j.contains("pitch"))  p.pitch = j["pitch"].get<float>();
  if (j.contains("fovY"))   p.fovY  = j["fovY"].get<float>();
  if (j.contains("nearZ"))  p.nearZ = j["nearZ"].get<float>();
  if (j.contains("farZ"))   p.farZ  = j["farZ"].get<float>();
  if (j.contains("mode"))   p.mode  = static_cast<CameraMode>(j["mode"].get<int>());
  if (j.contains("orbitTarget") && j["orbitTarget"].is_array() && j["orbitTarget"].size() >= 3) {
    p.orbitTarget.x = j["orbitTarget"][0].get<float>();
    p.orbitTarget.y = j["orbitTarget"][1].get<float>();
    p.orbitTarget.z = j["orbitTarget"][2].get<float>();
  }
  if (j.contains("orbitDistance")) p.orbitDistance = j["orbitDistance"].get<float>();
  if (j.contains("orbitYaw"))     p.orbitYaw     = j["orbitYaw"].get<float>();
  if (j.contains("orbitPitch"))   p.orbitPitch   = j["orbitPitch"].get<float>();
  return p;
}
