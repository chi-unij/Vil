// ======================================
// File: main.cpp
// Purpose: Application entry point and main loop (window, input, camera,
//          rendering, ImGui). Phase 8: uses render passes + FrameData.
// ======================================

#include "Camera.h"
#include "DxContext.h"
#include "GridRenderer.h"
#include "HybridReflectionRenderer.h"
#include "ImGuiLayer.h"
#include "Input.h"
#include "PostProcess.h"
#include "RenderPass.h"
#include "RenderPasses.h"
#include "SSAORenderer.h"
#include "IBLGenerator.h"
#include "MeshRenderer.h"
#include "ParticleRenderer.h"
#include "SkyRenderer.h"
#include "Win32Window.h"
#include "particle_test.h"
// GridGame は本プロジェクトでは未使用のため除外。StageData はシーンエディタ依存のため残す。
#include "gridgame/StageData.h"
#include "engine/Scene.h"
#include "engine/SceneEditor.h"
#include "game/BossArenaScene.h"
#include "game/OverworldScene.h"
#include "game/PlayerAnimationPreview.h"
#include "game/TavernScene.h"
#include "game/TitleScreen.h"
#include "game/WorldRainParticles.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <dbghelp.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

static volatile LONG g_startupStage = 0;
static DxContext *g_crashDxContext = nullptr;

static void TraceAppEvent(const char *message) {
  std::ofstream f("app_trace_log.txt", std::ios::out | std::ios::app);
  if (f)
    f << message << "\n";
}

static const char *StartupStageName(LONG s) {
  switch (s) {
  case 0:
    return "0: start";
  case 10:
    return "10: window.Create";
  case 20:
    return "20: dx.Initialize";
  case 30:
    return "30: window.Show";
  case 40:
    return "40: imgui.Initialize";
  case 70:
    return "70: main loop";
  default:
    return "unknown";
  }
}

static void SetStartupStage(LONG s) { InterlockedExchange(&g_startupStage, s); }

static void ApplySceneGlobalsToFrame(const Scene &scene, FrameData &frame) {
  const auto &light = scene.LightSettings();
  frame.lighting.lightDir = light.lightDir;
  frame.lighting.lightIntensity = light.lightIntensity;
  frame.lighting.lightColor = light.lightColor;

  const auto &shadow = scene.ShadowSettings();
  frame.shadowsEnabled = shadow.shadowsEnabled;
  frame.shadowBias = shadow.shadowBias;
  frame.shadowStrength = shadow.shadowStrength;
  frame.ssaoEnabled = shadow.ssaoEnabled;
  frame.ssaoRadius = shadow.ssaoRadius;
  frame.ssaoBias = shadow.ssaoBias;
  frame.ssaoPower = shadow.ssaoPower;
  frame.ssaoKernelSize = shadow.ssaoKernelSize;
  frame.ssaoStrength = shadow.ssaoStrength;

  const auto &post = scene.PostProcessSettings();
  frame.exposure = post.exposure;
  frame.bloomEnabled = post.bloomEnabled;
  frame.bloomThreshold = post.bloomThreshold;
  frame.bloomIntensity = post.bloomIntensity;
  frame.taaEnabled = post.taaEnabled;
  frame.taaBlendFactor = post.taaBlendFactor;
  frame.fxaaEnabled = post.fxaaEnabled;
  frame.motionBlurEnabled = post.motionBlurEnabled;
  frame.motionBlurStrength = post.motionBlurStrength;
  frame.motionBlurSamples = post.motionBlurSamples;
  frame.dofEnabled = post.dofEnabled;
  frame.dofFocalDistance = post.dofFocalDistance;
  frame.dofFocalRange = post.dofFocalRange;
  frame.dofMaxBlur = post.dofMaxBlur;
}

static void TryWriteSymbolizedStack(std::ostream &out, void **frames,
                                    USHORT frameCount) {
  HANDLE proc = GetCurrentProcess();
  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
  if (!SymInitialize(proc, nullptr, TRUE)) {
    out << "SymInitialize failed.\n";
    return;
  }

  // Allocate a SYMBOL_INFO large enough for typical symbol names.
  alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
  auto *sym = reinterpret_cast<SYMBOL_INFO *>(symBuf);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = 255;

  IMAGEHLP_LINE64 line{};
  line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

  for (USHORT i = 0; i < frameCount; ++i) {
    DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
    out << "  [" << i << "] " << frames[i];

    DWORD64 disp = 0;
    if (SymFromAddr(proc, addr, &disp, sym)) {
      out << "  " << sym->Name << " +0x" << std::hex << disp << std::dec;

      DWORD lineDisp = 0;
      if (SymGetLineFromAddr64(proc, addr, &lineDisp, &line)) {
        out << "  (" << line.FileName << ":" << line.LineNumber << ")";
      }
    }
    out << "\n";
  }

  SymCleanup(proc);
}

struct AppResizeContext {
  Camera *cam = nullptr;
  bool pendingResize = false;
  uint32_t width = 0;
  uint32_t height = 0;
};

static void OnResize(uint32_t w, uint32_t h, void *userData) {
  auto *ctx = reinterpret_cast<AppResizeContext *>(userData);
  if (!ctx || w == 0 || h == 0)
    return;

  ctx->pendingResize = true;
  ctx->width = w;
  ctx->height = h;
}

static LONG WINAPI UnhandledExceptionHandler(_EXCEPTION_POINTERS *ep) {
  const LONG stage = g_startupStage;
  void *addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress
                                        : nullptr;

  char modPath[MAX_PATH] = {};
  HMODULE mod = nullptr;
  if (addr &&
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(addr), &mod) &&
      mod) {
    GetModuleFileNameA(mod, modPath, MAX_PATH);
  }

  void *frames[32] = {};
  USHORT frameCount = CaptureStackBackTrace(0, 32, frames, nullptr);

  {
    std::ofstream f("crash_log.txt", std::ios::out | std::ios::trunc);
    if (f) {
      f << "StartupStage: " << stage << " (" << StartupStageName(stage) << ")\n";
      if (ep && ep->ExceptionRecord) {
        f << "ExceptionCode: 0x" << std::hex << ep->ExceptionRecord->ExceptionCode
          << "\n";
        f << "ExceptionAddress: " << ep->ExceptionRecord->ExceptionAddress
          << "\n";
      }
      if (modPath[0] != '\0')
        f << "Module: " << modPath << "\n";
      if (g_crashDxContext) {
        f << "\n";
        g_crashDxContext->DumpDebugMessages(f);
        f << "\n";
      }
      f << "Stack (symbolized):\n";
      TryWriteSymbolizedStack(f, frames, frameCount);
    }
  }

  std::ostringstream ss;
  ss << "Unhandled exception (SEH).\n"
     << "Stage: " << StartupStageName(stage) << "\n"
     << "Code: 0x" << std::hex << ep->ExceptionRecord->ExceptionCode << "\n"
     << "Address: " << ep->ExceptionRecord->ExceptionAddress << "\n";
  if (modPath[0] != '\0')
    ss << "Module: " << modPath << "\n";
  ss << "\nWrote: crash_log.txt";
  MessageBoxA(nullptr, ss.str().c_str(), "Crash",
              MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SYSTEMMODAL);
  return EXCEPTION_EXECUTE_HANDLER;
}

// Convert screen-space mouse position to a world-space point at a given depth
// from the camera. Used to place the particle emitter in 3D space.
static DirectX::XMVECTOR ScreenToWorld(int screenX, int screenY,
                                       int screenW, int screenH,
                                       const DirectX::XMMATRIX &view,
                                       const DirectX::XMMATRIX &proj,
                                       float depth) {
  using namespace DirectX;
  float ndcX = (2.0f * screenX / screenW - 1.0f);
  float ndcY = -(2.0f * screenY / screenH - 1.0f); // flip Y

  XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);
  XMVECTOR nearPt = XMVector3TransformCoord(
      XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invViewProj);
  XMVECTOR farPt = XMVector3TransformCoord(
      XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invViewProj);

  XMVECTOR dir = XMVector3Normalize(XMVectorSubtract(farPt, nearPt));
  return XMVectorAdd(nearPt, XMVectorScale(dir, depth));
}

// ---- CSM helpers (Phase 10.1) ----

// Practical split scheme (GPU Gems 3, Nvidia).
// lambda=0 is uniform, lambda=1 is logarithmic. Typical: 0.5.
static void ComputeCascadeSplits(float nearZ, float farZ, uint32_t count,
                                 float lambda, float outSplits[]) {
  outSplits[0] = nearZ;
  for (uint32_t i = 1; i <= count; ++i) {
    const float p = static_cast<float>(i) / static_cast<float>(count);
    const float logSplit = nearZ * std::pow(farZ / nearZ, p);
    const float uniSplit = nearZ + (farZ - nearZ) * p;
    outSplits[i] = lambda * logSplit + (1.0f - lambda) * uniSplit;
  }
}

// Build a tight orthographic light-view-projection for a single cascade slice.
static DirectX::XMMATRIX ComputeCascadeViewProj(
    const DirectX::XMMATRIX &cameraView, float fovY, float aspect,
    float splitNear, float splitFar,
    const DirectX::XMVECTOR &lightDir) {
  using namespace DirectX;

  // 1) Build perspective projection for this split range.
  const XMMATRIX sliceProj =
      XMMatrixPerspectiveFovLH(fovY, aspect, splitNear, splitFar);
  const XMMATRIX invVP = XMMatrixInverse(nullptr, cameraView * sliceProj);

  // 2) Transform 8 NDC corners to world space.
  static const XMFLOAT3 ndcCorners[8] = {
      {-1, -1, 0}, {+1, -1, 0}, {-1, +1, 0}, {+1, +1, 0}, // near
      {-1, -1, 1}, {+1, -1, 1}, {-1, +1, 1}, {+1, +1, 1}, // far
  };
  XMVECTOR corners[8];
  for (int i = 0; i < 8; ++i)
    corners[i] = XMVector3TransformCoord(XMLoadFloat3(&ndcCorners[i]), invVP);

  // 3) Compute centroid of frustum slice.
  XMVECTOR center = XMVectorZero();
  for (int i = 0; i < 8; ++i)
    center = XMVectorAdd(center, corners[i]);
  center = XMVectorScale(center, 1.0f / 8.0f);

  // 4) Build light view matrix looking along lightDir from centroid.
  const XMVECTOR lightDirN = XMVector3Normalize(lightDir);
  XMVECTOR up = XMVectorSet(0, 1, 0, 0);
  if (std::fabs(XMVectorGetX(XMVector3Dot(up, lightDirN))) > 0.99f)
    up = XMVectorSet(0, 0, 1, 0);

  const XMVECTOR lightPos =
      XMVectorSubtract(center, XMVectorScale(lightDirN, 200.0f));
  const XMMATRIX lightView = XMMatrixLookAtLH(lightPos, center, up);

  // 5) Transform corners to light space, find AABB.
  float minX = FLT_MAX, maxX = -FLT_MAX;
  float minY = FLT_MAX, maxY = -FLT_MAX;
  float minZ = FLT_MAX, maxZ = -FLT_MAX;
  for (int i = 0; i < 8; ++i) {
    const XMVECTOR lc = XMVector3TransformCoord(corners[i], lightView);
    const float x = XMVectorGetX(lc);
    const float y = XMVectorGetY(lc);
    const float z = XMVectorGetZ(lc);
    if (x < minX) minX = x; if (x > maxX) maxX = x;
    if (y < minY) minY = y; if (y > maxY) maxY = y;
    if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
  }

  // 6) Extend minZ backward to catch shadow casters behind the frustum.
  const float zRange = maxZ - minZ;
  minZ -= zRange * 2.0f;

  // 7) Build tight orthographic projection.
  const XMMATRIX lightProj =
      XMMatrixOrthographicOffCenterLH(minX, maxX, minY, maxY, minZ, maxZ);

  return lightView * lightProj;
}

static float LerpFloat(float a, float b, float t) {
  return a + (b - a) * t;
}

static float DaylightTFromHour(float hour) {
  const float wrapped = std::fmod(std::max(hour, 0.0f), 24.0f);
  if (wrapped <= 12.0f)
    return wrapped / 12.0f;
  return (24.0f - wrapped) / 12.0f;
}

struct GameTimeLightingProfile {
  DirectX::XMFLOAT3 sunDirection = {0.3f, -1.0f, 0.2f};
  DirectX::XMFLOAT3 sunColor = {1.0f, 0.98f, 0.92f};
  float sunIntensity = 3.0f;
};

static DirectX::XMFLOAT3 LerpFloat3(const DirectX::XMFLOAT3 &a,
                                    const DirectX::XMFLOAT3 &b, float t) {
  return {LerpFloat(a.x, b.x, t), LerpFloat(a.y, b.y, t),
          LerpFloat(a.z, b.z, t)};
}

static GameTimeLightingProfile BuildGameTimeLighting(float hour) {
  using namespace DirectX;

  const float wrappedHour = std::fmod(std::max(hour, 0.0f), 24.0f);
  const float dayProgress = wrappedHour / 24.0f;
  const float sunAngle = dayProgress * XM_2PI;
  const float altitude = std::sin(sunAngle - XM_PIDIV2);
  const float daylight = std::clamp((altitude + 0.16f) / 1.16f, 0.0f, 1.0f);
  const float warmEdge =
      std::pow(1.0f - std::clamp(std::abs(altitude), 0.0f, 1.0f), 2.0f);

  const float horizontal =
      std::max(0.18f, std::sqrt(std::max(0.0f, 1.0f - altitude * altitude)));
  const float azimuth = -0.65f + dayProgress * XM_PI;
  const XMVECTOR sunDir =
      XMVector3Normalize(XMVectorSet(std::cos(azimuth) * horizontal,
                                     -std::max(0.08f, std::abs(altitude)),
                                     std::sin(azimuth) * horizontal, 0.0f));

  const XMFLOAT3 middayColor = {1.0f, 0.98f, 0.90f};
  const XMFLOAT3 goldenColor = {1.0f, 0.60f, 0.32f};
  const XMFLOAT3 moonColor = {0.34f, 0.43f, 0.72f};
  const XMFLOAT3 dayColor = LerpFloat3(middayColor, goldenColor, warmEdge);

  GameTimeLightingProfile profile{};
  XMStoreFloat3(&profile.sunDirection, sunDir);
  profile.sunColor = LerpFloat3(moonColor, dayColor, daylight);
  profile.sunIntensity = LerpFloat(0.16f, 4.2f, daylight);
  return profile;
}

static GameTimeLightingProfile BuildTavernLighting() {
  GameTimeLightingProfile profile{};
  // 室内では時刻に連動する太陽を使わず、天井からの固定暖色光として扱う。
  profile.sunDirection = {-0.24f, -0.94f, 0.24f};
  profile.sunColor = {1.0f, 0.76f, 0.52f};
  profile.sunIntensity = 0.72f;
  return profile;
}

static ImU32 UiColor(float r, float g, float b, float a) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a));
}

static void DrawTavernEntrancePrompt(int viewportWidth, int viewportHeight) {
  ImDrawList *draw = ImGui::GetForegroundDrawList();
  const char *prompt = "E / A  水鏡亭に入る";
  const ImVec2 textSize = ImGui::CalcTextSize(prompt);
  const ImVec2 panelSize(textSize.x + 54.0f, textSize.y + 30.0f);
  const ImVec2 panelMin(
      (static_cast<float>(viewportWidth) - panelSize.x) * 0.5f,
      static_cast<float>(viewportHeight) - panelSize.y - 48.0f);
  const ImVec2 panelMax(panelMin.x + panelSize.x, panelMin.y + panelSize.y);

  draw->AddRectFilled(panelMin, panelMax, UiColor(0.025f, 0.018f, 0.012f, 0.90f),
                      9.0f);
  draw->AddRect(panelMin, panelMax, UiColor(0.96f, 0.57f, 0.22f, 0.82f),
                9.0f, 0, 1.5f);
  draw->AddText(ImVec2(panelMin.x + 27.0f, panelMin.y + 15.0f),
                UiColor(1.0f, 0.88f, 0.68f, 0.98f), prompt);
}

static bool DrawSettingsChoice(ImDrawList *draw, const char *id,
                               const char *title, const char *note,
                               const ImVec2 &min, const ImVec2 &max,
                               bool selected) {
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(min);
  const bool clicked =
      ImGui::InvisibleButton("##hit", ImVec2(max.x - min.x, max.y - min.y));
  const bool hovered = ImGui::IsItemHovered();
  ImGui::PopID();

  draw->AddRectFilled(min, max,
                      selected ? UiColor(0.08f, 0.24f, 0.23f, 0.94f)
                               : (hovered ? UiColor(0.08f, 0.12f, 0.13f, 0.94f)
                                          : UiColor(0.035f, 0.055f, 0.060f, 0.90f)),
                      8.0f);
  draw->AddRect(min, max,
                selected ? UiColor(0.36f, 1.0f, 0.84f, 0.88f)
                         : UiColor(0.42f, 0.62f, 0.62f, hovered ? 0.70f : 0.36f),
                8.0f, 0, selected ? 2.0f : 1.2f);

  const ImVec2 dot(min.x + 24.0f, (min.y + max.y) * 0.5f);
  draw->AddCircle(dot, 8.5f, UiColor(0.44f, 0.92f, 0.84f, 0.74f), 20, 1.5f);
  if (selected)
    draw->AddCircleFilled(dot, 5.0f, UiColor(0.38f, 1.0f, 0.84f, 0.96f), 20);
  draw->AddText(ImVec2(min.x + 46.0f, min.y + 13.0f),
                UiColor(0.86f, 1.0f, 0.94f, 0.96f), title);
  draw->AddText(ImVec2(min.x + 46.0f, min.y + 36.0f),
                UiColor(0.52f, 0.70f, 0.68f, 0.88f), note);
  return clicked;
}

static bool HasCommandLineSwitch(const wchar_t *commandLine,
                                 std::wstring_view expectedSwitch) {
  if (!commandLine || expectedSwitch.empty())
    return false;

  const wchar_t *cursor = commandLine;
  while (*cursor != L'\0') {
    while (std::iswspace(*cursor))
      ++cursor;
    if (*cursor == L'\0')
      break;

    const bool quoted = *cursor == L'"';
    if (quoted)
      ++cursor;
    const wchar_t *argumentBegin = cursor;

    while (*cursor != L'\0' &&
           (quoted ? *cursor != L'"' : !std::iswspace(*cursor))) {
      ++cursor;
    }

    if (std::wstring_view(argumentBegin,
                          static_cast<size_t>(cursor - argumentBegin)) ==
        expectedSwitch) {
      return true;
    }

    if (quoted && *cursor == L'"')
      ++cursor;
  }

  return false;
}

static bool SetWorkingDirectoryToContentRoot() {
  std::array<wchar_t, 32768> executablePathBuffer{};
  const DWORD pathLength =
      GetModuleFileNameW(nullptr, executablePathBuffer.data(),
                         static_cast<DWORD>(executablePathBuffer.size()));
  if (pathLength == 0 ||
      pathLength >= static_cast<DWORD>(executablePathBuffer.size()))
    return false;

  std::filesystem::path candidate =
      std::filesystem::path(executablePathBuffer.data()).parent_path();
  std::filesystem::path packagedContentFallback;
  std::error_code error;
  while (!candidate.empty()) {
    error.clear();
    const bool hasAssets =
        std::filesystem::is_directory(candidate / L"Assets", error);
    error.clear();
    const bool hasShaders =
        std::filesystem::is_directory(candidate / L"shaders", error);
    if (hasAssets && hasShaders) {
      // A developer build can sit beside a stale post-build Assets copy.
      // Prefer the real repository root so the editor always reflects the
      // current source Assets/shaders instead of yesterday's copied output.
      error.clear();
      const bool hasProjectFile =
          std::filesystem::is_regular_file(candidate / L"CMakeLists.txt",
                                           error);
      error.clear();
      const bool hasSourceTree =
          std::filesystem::is_directory(candidate / L"src", error);
      if (hasProjectFile && hasSourceTree &&
          SetCurrentDirectoryW(candidate.c_str())) {
        return true;
      }
      if (packagedContentFallback.empty())
        packagedContentFallback = candidate;
    }

    const std::filesystem::path parent = candidate.parent_path();
    if (parent == candidate)
      break;
    candidate = parent;
  }

  if (!packagedContentFallback.empty())
    return SetCurrentDirectoryW(packagedContentFallback.c_str()) != FALSE;

  return false;
}

static void PopulateEditorWelcomeScene(Scene &scene, DxContext &dx) {
  if (!scene.Entities().empty())
    return;

  const auto addMesh = [&](const char *name, MeshSourceType sourceType,
                           const DirectX::XMFLOAT3 &position,
                           const DirectX::XMFLOAT4 &color, float metallic,
                           float roughness) -> Entity & {
    Entity entity;
    entity.id = scene.AllocateId();
    entity.name = name;
    entity.transform.position = position;
    entity.mesh = MeshComponent{};
    entity.mesh->sourceType = sourceType;
    entity.mesh->material.baseColorFactor = color;
    entity.mesh->material.metallicFactor = metallic;
    entity.mesh->material.roughnessFactor = roughness;
    scene.AddEntityDirect(entity);
    return scene.Entities().back();
  };

  Entity &ground = addMesh("PBR Ground", MeshSourceType::ProceduralPlane,
                           {0.0f, -1.0f, 4.0f},
                           {0.48f, 0.50f, 0.54f, 1.0f}, 0.0f, 0.82f);
  ground.mesh->width = 18.0f;
  ground.mesh->height = 18.0f;
  ground.mesh->material.uvTiling = {5.0f, 5.0f};
  ground.mesh->material.proceduralTypeId = 7.0f;
  ground.mesh->material.reflectionReceiver = ReflectionReceiver::Water;
  ground.mesh->material.reflectionStrength = 1.0f;
  // Mirror ray は textured ground を hit できる。Water ray は TLAS の
  // instance mask で receiver を除外し、self-reflection を防止する。
  ground.mesh->material.rayTracingVisible = true;
  ground.mesh->texturePaths[0] =
      "Assets/textures/Floor_png/Ground037_2K-PNG_Color.png";
  ground.mesh->texturePaths[1] =
      "Assets/textures/Floor_png/Ground037_2K-PNG_NormalDX.png";
  ground.mesh->texturePaths[2] =
      "Assets/textures/Floor_png/Ground037_2K-PNG_Roughness.png";
  ground.mesh->texturePaths[3] =
      "Assets/textures/Floor_png/Ground037_2K-PNG_AmbientOcclusion.png";
  ground.mesh->texturePaths[5] =
      "Assets/textures/Floor_png/Ground037_2K-PNG_Displacement.png";

  Entity &cube = addMesh("Metal Cube", MeshSourceType::ProceduralCube,
                         {-2.2f, 0.0f, 3.0f},
                         {0.74f, 0.16f, 0.12f, 1.0f}, 0.86f, 0.22f);
  cube.mesh->size = 1.6f;
  cube.transform.rotation = {0.0f, 24.0f, 0.0f};

  Entity &sphere = addMesh("Ceramic Sphere",
                           MeshSourceType::ProceduralSphere,
                           {0.0f, 0.0f, 4.0f},
                           {0.10f, 0.42f, 0.86f, 1.0f}, 0.05f, 0.18f);
  sphere.mesh->size = 1.05f;
  sphere.mesh->rings = 24;
  sphere.mesh->segments = 32;

  Entity &cylinder = addMesh("Rough Cylinder",
                             MeshSourceType::ProceduralCylinder,
                             {2.2f, -0.05f, 3.4f},
                             {0.14f, 0.64f, 0.38f, 1.0f}, 0.12f, 0.74f);
  cylinder.mesh->width = 0.9f;
  cylinder.mesh->height = 1.9f;
  cylinder.mesh->segments = 32;

  Entity &mirror = addMesh("DXR Mirror", MeshSourceType::ProceduralPlane,
                           {0.0f, 1.25f, 7.2f},
                           {0.08f, 0.09f, 0.11f, 1.0f}, 0.94f, 0.035f);
  mirror.mesh->width = 7.2f;
  mirror.mesh->height = 4.5f;
  mirror.transform.rotation = {-90.0f, 0.0f, 0.0f};
  mirror.mesh->material.reflectionReceiver = ReflectionReceiver::Mirror;
  mirror.mesh->material.reflectionStrength = 1.0f;
  mirror.mesh->material.rayTracingVisible = false;

  Entity pointLight;
  pointLight.id = scene.AllocateId();
  pointLight.name = "Key Point Light";
  pointLight.transform.position = {-2.0f, 3.5f, 0.5f};
  pointLight.pointLight = PointLightComponent{};
  pointLight.pointLight->color = {1.0f, 0.52f, 0.28f};
  pointLight.pointLight->intensity = 16.0f;
  pointLight.pointLight->range = 12.0f;
  scene.AddEntityDirect(pointLight);

  Entity spotLight;
  spotLight.id = scene.AllocateId();
  spotLight.name = "Fill Spot Light";
  spotLight.transform.position = {3.5f, 4.5f, -0.5f};
  spotLight.spotLight = SpotLightComponent{};
  spotLight.spotLight->direction = {-0.35f, -0.8f, 0.45f};
  spotLight.spotLight->color = {0.32f, 0.58f, 1.0f};
  spotLight.spotLight->intensity = 22.0f;
  spotLight.spotLight->range = 18.0f;
  scene.AddEntityDirect(spotLight);

  scene.CreateGpuResources(dx);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int nCmdShow) {
  int applicationExitCode = 0;
  try {
#if defined(VILLIEN_EDITOR_BUILD)
    constexpr bool kDedicatedEditorBuild = true;
#else
    constexpr bool kDedicatedEditorBuild = false;
#endif
    const bool dxrSmokeRequested =
        HasCommandLineSwitch(commandLine, L"--dxr-smoke");
    const bool playerAnimationSmokeRequested =
        HasCommandLineSwitch(commandLine, L"--player-animation-smoke");
    const bool bossMirrorPickupSmokeRequested =
        HasCommandLineSwitch(commandLine, L"--boss-mirror-pickup-smoke");
    const bool dxrOverworldSmokeRequested =
        HasCommandLineSwitch(commandLine, L"--dxr-overworld-smoke");
    const bool overworldShortcutRequested =
        HasCommandLineSwitch(commandLine, L"--overworld");
    const bool dxrOverworldRequested =
        HasCommandLineSwitch(commandLine, L"--dxr-overworld") ||
        dxrOverworldSmokeRequested;
    const bool overworldRequested =
        overworldShortcutRequested || dxrOverworldRequested;
    const bool bossShortcutRequested =
        HasCommandLineSwitch(commandLine, L"--boss");
    const bool bossRequested =
        bossShortcutRequested || bossMirrorPickupSmokeRequested;
    const bool tavernSmokeRequested =
        HasCommandLineSwitch(commandLine, L"--tavern-smoke");
    const bool tavernGameplaySmokeRequested =
        HasCommandLineSwitch(commandLine, L"--tavern-gameplay-smoke");
    const bool tavernWaitingPreviewRequested =
        HasCommandLineSwitch(commandLine, L"--tavern-waiting-preview");
    const bool tavernOrderPreviewRequested =
        HasCommandLineSwitch(commandLine, L"--tavern-order-preview");
    const bool tavernKitchenPreviewRequested =
        HasCommandLineSwitch(commandLine, L"--tavern-kitchen-preview");
    const bool tavernDaySmokeRequested =
        HasCommandLineSwitch(commandLine, L"--tavern-day-smoke");
    const bool tavernManagementSmokeRequested =
        HasCommandLineSwitch(commandLine, L"--tavern-management-smoke");
    const bool tavernSuppliesSmokeRequested =
        HasCommandLineSwitch(commandLine, L"--tavern-supplies-smoke");
    const bool tavernUpgradesSmokeRequested =
        HasCommandLineSwitch(commandLine, L"--tavern-upgrades-smoke");
    const bool tavernThirdPersonRequested =
        HasCommandLineSwitch(commandLine, L"--tavern-third-person");
    const bool tavernFirstPersonEnabled = !tavernThirdPersonRequested;
    const int tavernSmokeModeCount =
        static_cast<int>(tavernSmokeRequested) +
        static_cast<int>(tavernGameplaySmokeRequested) +
        static_cast<int>(tavernDaySmokeRequested) +
        static_cast<int>(tavernManagementSmokeRequested) +
        static_cast<int>(tavernSuppliesSmokeRequested) +
        static_cast<int>(tavernUpgradesSmokeRequested);
    const bool tavernRequested =
        HasCommandLineSwitch(commandLine, L"--tavern") ||
        tavernWaitingPreviewRequested || tavernOrderPreviewRequested ||
        tavernKitchenPreviewRequested ||
        tavernThirdPersonRequested ||
        tavernSmokeRequested || tavernGameplaySmokeRequested ||
        tavernDaySmokeRequested || tavernManagementSmokeRequested ||
        tavernSuppliesSmokeRequested || tavernUpgradesSmokeRequested;
    const bool dxrSmokeMode = !playerAnimationSmokeRequested &&
                              !bossMirrorPickupSmokeRequested &&
                              (dxrSmokeRequested || dxrOverworldSmokeRequested);
    const bool dxrProofRequested =
        HasCommandLineSwitch(commandLine, L"--dxr-proof") || dxrSmokeRequested;
    const bool launchEditor = kDedicatedEditorBuild ||
                              HasCommandLineSwitch(commandLine, L"--editor") ||
                              dxrProofRequested;
    const wchar_t *windowTitle = launchEditor ? L"VILLIEN Editor" : L"VILLIEN";
    const bool contentRootFound = SetWorkingDirectoryToContentRoot();

    {
      std::ofstream f("app_trace_log.txt", std::ios::out | std::ios::trunc);
      if (f) {
        f << "app start\n";
        if (!contentRootFound)
          f << "warning: Assets/shaders content root was not found\n";
      }
    }
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);
    if (tavernSmokeModeCount > 1) {
      TraceAppEvent("Tavern smoke: FAIL; select exactly one Tavern smoke mode");
      return 2;
    }

    Win32Window window;
    SetStartupStage(10);
    window.Create(windowTitle, 1920, 1080);

    DxContext dx;
    g_crashDxContext = &dx;
    SetStartupStage(20);
#if defined(_DEBUG)
    constexpr bool kEnableD3D12DebugLayer = true;
#else
    constexpr bool kEnableD3D12DebugLayer = false;
#endif
    dx.Initialize(window.Handle(), window.Width(), window.Height(),
                  kEnableD3D12DebugLayer);

    // DXR 1.1 / DXIL の前提だけを検証する。失敗時は既存 SSR を維持する。
    HybridReflectionRenderer hybridReflection;
    hybridReflection.Initialize(dx);
    bool dxrProofSceneLogged = false;
    bool dxrProofAuditLogged = false;
    bool dxrProofDispatchLogged = false;
    int dxrSmokeFramesRemaining = dxrSmokeMode ? 4 : -1;
    int tavernSmokeTavernFramesRemaining = tavernSmokeRequested ? 2 : -1;
    int tavernSmokeOverworldFramesRemaining = tavernSmokeRequested ? 2 : -1;
    bool tavernSmokeReturnIssued = false;
    bool tavernGameplaySmokeLogged = false;
    bool tavernDaySmokeLogged = false;
    bool tavernManagementSmokeFailed = false;
    bool tavernManagementSmokeCompleted = false;
    int tavernManagementSmokePhase = 0;
    int tavernManagementSmokeUpdateFrames = 0;
    int tavernManagementSmokeLoopFrames = 0;
    int tavernManagementBaselineGold = 0;
    int tavernManagementBaselineCompletedCycles = 0;
    enum class TavernSuppliesSmokePhase {
      OpenRoot,
      VerifyRoot,
      EnterSupplies,
      VerifyInitialFull,
      AttemptFullOrder,
      VerifyFullNoOp,
      BackToRoot,
      VerifyBackToRoot,
      CloseRoot,
      VerifyClosed,
      AutomateToCancelablePour,
      CancelPour,
      VerifyCancelledPour,
      AutomateOneService,
      ReopenRoot,
      VerifyReopenedRoot,
      ReenterSupplies,
      VerifyStockAfterService,
      SelectOneAle,
      VerifyOneAleSelected,
      PurchaseOneAle,
      VerifyPurchase,
      SelectUnaffordableAle,
      VerifyUnaffordableSelection,
      AttemptUnaffordableOrder,
      VerifyUnaffordableNoOp,
    };
    bool tavernSuppliesSmokeFailed = false;
    bool tavernSuppliesSmokeCompleted = false;
    TavernSuppliesSmokePhase tavernSuppliesSmokePhase =
        TavernSuppliesSmokePhase::OpenRoot;
    int tavernSuppliesSmokeUpdateFrames = 0;
    int tavernSuppliesSmokeLoopFrames = 0;
    int tavernSuppliesPostServeGold = 0;
    uint64_t tavernSuppliesPurchaseSerialBefore = 0;
    uint64_t tavernSuppliesPurchaseSerialAfter = 0;
    enum class TavernUpgradesSmokePhase {
      OpenRoot,
      VerifyRoot,
      SelectUpgrades,
      VerifyUpgradesSelected,
      EnterUpgrades,
      VerifyInitialPage,
      VerifyEmergencyReserve,
      AttemptEmergencyReservePurchase,
      VerifyEmergencyReserveNoOp,
      VerifyRestoredInitialPage,
      OpenExtraMugConfirmation,
      VerifyExtraMugConfirmation,
      CancelExtraMugConfirmation,
      VerifyExtraMugCancellation,
      ReopenExtraMugConfirmation,
      VerifyReopenedExtraMugConfirmation,
      PurchaseExtraMug,
      VerifyExtraMugPurchase,
      AttemptOwnedExtraMug,
      VerifyOwnedExtraMugNoOp,
      SelectAleCapacity,
      VerifyAleCapacitySelection,
      OpenAleCapacityConfirmation,
      VerifyAleCapacityConfirmation,
      PurchaseAleCapacity,
      VerifyAleCapacityPurchase,
      AttemptMaxedAleCapacity,
      VerifyMaxedAleCapacityNoOp,
      SelectTable3,
      VerifyTable3Selection,
      OpenTable3Confirmation,
      VerifyTable3Confirmation,
      PurchaseTable3,
      VerifyTable3Purchase,
      AttemptOwnedTable3,
      VerifyOwnedTable3NoOp,
      BackToRoot,
      VerifyBackToRoot,
      CloseRoot,
      VerifyClosed,
      VerifyNextDayPersistence,
      VerifyReentryPersistence,
      AutomateTable3Cycle,
    };
    bool tavernUpgradesSmokeFailed = false;
    bool tavernUpgradesSmokeCompleted = false;
    TavernUpgradesSmokePhase tavernUpgradesSmokePhase =
        TavernUpgradesSmokePhase::OpenRoot;
    int tavernUpgradesSmokeUpdateFrames = 0;
    int tavernUpgradesSmokeLoopFrames = 0;
    int tavernUpgradesExpectedGold = 200;
    int tavernUpgradesExpectedAleLevel = 0;
    int tavernUpgradesExpectedAlePrice = 20;
    uint64_t tavernUpgradesPurchaseSerialBefore = 0;
    ReflectionMode reflectionMode =
        (dxrProofRequested || dxrOverworldRequested) &&
                hybridReflection.IsSupported()
            ? ReflectionMode::HybridDXR
            : ReflectionMode::SSR;
    const std::string dxrStartupStatus =
        "DXR preflight: " + hybridReflection.Status();
    TraceAppEvent(dxrStartupStatus.c_str());

    Camera cam;
    cam.SetPosition(0.0f, 1.5f, -4.0f);
    cam.SetYawPitch(0.0f, 0.0f);
    cam.SetLens(DirectX::XM_PIDIV4,
                static_cast<float>(window.Width()) /
                    static_cast<float>(window.Height()),
                0.1f, 1000.0f);

    AppResizeContext resizeCtx{&cam};
    window.SetResizeCallback(&OnResize, &resizeCtx);
    SetStartupStage(30);
    window.Show(nCmdShow);

    ImGuiLayer imgui;
    SetStartupStage(40);
    imgui.Initialize(window, dx);
    TraceAppEvent("startup: ImGui ready");

    // ---- Initialize renderer modules (Phase 8) ----
    SkyRenderer skyRenderer;
    skyRenderer.Initialize(dx);
    TraceAppEvent("startup: sky ready");

    IBLGenerator iblGenerator;
    iblGenerator.Initialize(dx, skyRenderer.HdriTexture(), skyRenderer.HdriSrvGpu());
    TraceAppEvent("startup: IBL ready");

    GridRenderer gridRenderer;
    gridRenderer.Initialize(dx);

    // Wire IBL descriptors to the mesh renderer + DxContext (for deferred lighting).
    dx.GetMeshRenderer().SetIBLDescriptors(iblGenerator.IBLTableGpuBase());
    dx.SetIblTableGpu(iblGenerator.IBLTableGpuBase());

    // Player / Idle / Walk / Run クリップ確認用プレビュー。
    PlayerAnimationPreview playerPreview;
    playerPreview.Initialize(dx);
    TraceAppEvent("startup: player preview ready");

    constexpr size_t kPlayerAnimationSmokeExpectedParts = 13;
    constexpr size_t kPlayerAnimationSmokeExpectedOpaqueParts = 9;
    constexpr size_t kPlayerAnimationSmokeExpectedTransparentParts = 4;
    constexpr size_t kPlayerAnimationSmokeExpectedDoubleSidedParts = 8;
    // 0.18 秒の crossfade が完了した pose まで描画して検証する。
    constexpr int kPlayerAnimationSmokeFramesPerState = 16;
    constexpr std::array<PlayerAnimationPreview::ClipSlot, 4>
        kPlayerAnimationSmokeSequence = {
            PlayerAnimationPreview::ClipSlot::Idle,
            PlayerAnimationPreview::ClipSlot::Walk,
            PlayerAnimationPreview::ClipSlot::Run,
            PlayerAnimationPreview::ClipSlot::Idle,
        };
    constexpr int kPlayerAnimationSmokeTotalFrames =
        kPlayerAnimationSmokeFramesPerState *
        static_cast<int>(kPlayerAnimationSmokeSequence.size());
    constexpr int kPlayerAnimationSmokeActionMaxFrames = 180;
    int playerAnimationSmokeRenderedFrames = 0;
    int playerAnimationSmokeActionRenderedFrames = 0;
    int playerAnimationSmokeRenderedActionPoseFrames = 0;
    bool playerAnimationSmokeFailed = false;
    bool playerAnimationSmokeSequenceCompleted = false;
    bool playerAnimationSmokeActionStarted = false;
    bool playerAnimationSmokeActionCompleted = false;
    bool playerAnimationSmokeActionPoseThisFrame = false;
    uint64_t playerAnimationSmokeActionSerial = 0;

    const auto playerClipName = [](PlayerAnimationPreview::ClipSlot slot) {
      switch (slot) {
      case PlayerAnimationPreview::ClipSlot::Idle:
        return "Idle";
      case PlayerAnimationPreview::ClipSlot::Walk:
        return "Walk";
      case PlayerAnimationPreview::ClipSlot::Run:
        return "Run";
      }
      return "Unknown";
    };
    const auto failPlayerAnimationSmoke = [&](const std::string &reason) {
      if (!playerAnimationSmokeRequested || playerAnimationSmokeFailed)
        return;
      playerAnimationSmokeFailed = true;
      applicationExitCode = 2;
      const std::string message = "Player animation smoke: FAIL; " + reason;
      TraceAppEvent(message.c_str());
    };

    if (playerAnimationSmokeRequested) {
      if (!playerPreview.IsReady())
        failPlayerAnimationSmoke("player model is not ready");
      if (!playerPreview.HasSkeleton())
        failPlayerAnimationSmoke("player skeleton is unavailable");

      const size_t skeletonBoneCount = playerPreview.SkeletonBoneCount();
      if (skeletonBoneCount == 0 ||
          skeletonBoneCount > static_cast<size_t>(kMaxBones)) {
        failPlayerAnimationSmoke(
            "skeleton bone count is outside the supported range: " +
            std::to_string(skeletonBoneCount));
      }

      const size_t materialPartCount = playerPreview.MaterialPartCount();
      if (materialPartCount != kPlayerAnimationSmokeExpectedParts) {
        failPlayerAnimationSmoke(
            "expected 13 material parts, got " +
            std::to_string(materialPartCount));
      }
      if (playerPreview.OpaqueMaterialPartCount() !=
              kPlayerAnimationSmokeExpectedOpaqueParts ||
          playerPreview.TransparentMaterialPartCount() !=
              kPlayerAnimationSmokeExpectedTransparentParts) {
        failPlayerAnimationSmoke(
            "expected 9 opaque and 4 transparent material parts, got " +
            std::to_string(playerPreview.OpaqueMaterialPartCount()) +
            " opaque and " +
            std::to_string(playerPreview.TransparentMaterialPartCount()) +
            " transparent");
      }
      if (playerPreview.DoubleSidedMaterialPartCount() !=
          kPlayerAnimationSmokeExpectedDoubleSidedParts) {
        failPlayerAnimationSmoke(
            "expected 8 double-sided material parts, got " +
            std::to_string(playerPreview.DoubleSidedMaterialPartCount()));
      }

      const auto validateClip = [&](PlayerAnimationPreview::ClipSlot slot) {
        const PlayerAnimationPreview::ClipDiagnostics diagnostics =
            playerPreview.GetClipDiagnostics(slot);
        std::ostringstream status;
        status << "Player animation smoke: clip " << playerClipName(slot)
               << " loaded=" << (diagnostics.loaded ? 1 : 0)
               << " duration=" << diagnostics.duration
               << " tracks=" << diagnostics.trackCount << " source="
               << diagnostics.sourcePath;
        TraceAppEvent(status.str().c_str());

        if (!diagnostics.loaded || diagnostics.duration <= 0.0f ||
            !std::isfinite(diagnostics.duration) ||
            diagnostics.trackCount == 0) {
          failPlayerAnimationSmoke(std::string(playerClipName(slot)) +
                                   " clip diagnostics are invalid");
        }
        if (slot == PlayerAnimationPreview::ClipSlot::Walk &&
            diagnostics.sourcePath.find("Push.glb") != std::string::npos) {
          failPlayerAnimationSmoke(
              "Walk clip must not use Push.glb: " + diagnostics.sourcePath);
        }
        if (slot == PlayerAnimationPreview::ClipSlot::Walk &&
            diagnostics.fallback) {
          failPlayerAnimationSmoke(
              "Walk clip must load the dedicated Walk.glb asset: " +
              diagnostics.sourcePath);
        }
      };
      validateClip(PlayerAnimationPreview::ClipSlot::Idle);
      validateClip(PlayerAnimationPreview::ClipSlot::Walk);
      validateClip(PlayerAnimationPreview::ClipSlot::Run);

      const PlayerAnimationPreview::ClipDiagnostics takingItemDiagnostics =
          playerPreview.GetTakingItemDiagnostics();
      {
        std::ostringstream status;
        status << "Player animation smoke: clip TakingItem loaded="
               << (takingItemDiagnostics.loaded ? 1 : 0)
               << " duration=" << takingItemDiagnostics.duration
               << " tracks=" << takingItemDiagnostics.trackCount
               << " source=" << takingItemDiagnostics.sourcePath;
        TraceAppEvent(status.str().c_str());
      }
      if (!takingItemDiagnostics.loaded ||
          takingItemDiagnostics.duration <= 0.0f ||
          !std::isfinite(takingItemDiagnostics.duration) ||
          takingItemDiagnostics.trackCount == 0 ||
          takingItemDiagnostics.sourcePath.find("TakingItem.glb") ==
              std::string::npos) {
        failPlayerAnimationSmoke(
            "TakingItem clip must load from the dedicated GLB asset");
      }

      if (!playerPreview.BonePaletteFinite())
        failPlayerAnimationSmoke("initial bone palette contains non-finite data");
      if (!playerAnimationSmokeFailed) {
        std::ostringstream readyMessage;
        readyMessage << "Player animation smoke: diagnostics ready; parts="
                     << materialPartCount
                     << " opaque=" << playerPreview.OpaqueMaterialPartCount()
                     << " transparent="
                     << playerPreview.TransparentMaterialPartCount()
                     << " doubleSided="
                     << playerPreview.DoubleSidedMaterialPartCount()
                     << " bones=" << skeletonBoneCount;
        TraceAppEvent(readyMessage.str().c_str());
      }
    }
    BossArenaScene bossArenaScene;
    bossArenaScene.Initialize(dx);
    bossArenaScene.Reset(playerPreview);
    TraceAppEvent("startup: boss arena ready");
    OverworldScene overworldScene;
    overworldScene.Initialize(dx);
    TraceAppEvent("startup: overworld ready");
    TavernScene tavernScene;
    tavernScene.Initialize(dx);
    TraceAppEvent(tavernScene.ImportedArtReady()
                      ? "startup: reconstructed tavern art ready"
                      : "startup: tavern procedural fallback ready");
    std::vector<OverworldScene::CollisionShapeConfig> overworldCollisionShapes =
        overworldScene.BuildDefaultCollisionShapes();
    std::vector<CollisionSystem::Collider> overworldCollisionColliders =
        overworldScene.BuildCollisionColliders(overworldCollisionShapes);
    const std::vector<CollisionSystem::MeshTriangle> emptyMeshTriangles;
    const std::vector<CollisionSystem::Collider> emptyCollisionColliders;
    bool showCollisionDebug = false;
    bool useModelMeshCollision = true;
    bool showModelCollisionDebug = false;

    // Initialize particle system.
    dx.InitParticleRenderer();
    DirectX::XMVECTOR firePos = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    NormalEmitter fireEmitter(512, firePos, 120.0, true);
    float particleDepth = 8.0f;
    bool particlesEnabled = false;
    bool fireEnabled = true;

    DirectX::XMVECTOR smokePos = DirectX::XMVectorSet(-3.0f, 0.0f, 3.0f, 0.0f);
    SmokeEmitter smokeEmitter(256, smokePos, 30.0, true);
    bool smokeEnabled = true;

    DirectX::XMVECTOR sparkPos = DirectX::XMVectorSet(3.0f, 0.0f, 3.0f, 0.0f);
    SparkEmitter sparkEmitter(256, sparkPos, 80.0, true);
    bool sparkEnabled = true;

    RainEmitter rainEmitter(1024, OverworldScene::kFloorSizeMeters * 0.54f,
                            12.0f, 0.18f, 760.0, true);
    rainEmitter.WarmStart();
    bool rainEnabled = true;

    // ---- Initialize post-processing (Phase 9) ----
    PostProcessRenderer postProcess;
    postProcess.Initialize(dx);
    TraceAppEvent("startup: post process ready");

    // ---- Initialize SSAO (Phase 10.3) ----
    SSAORenderer ssaoRenderer;
    ssaoRenderer.Initialize(dx);
    TraceAppEvent("startup: SSAO ready");

    // ---- Create render passes (Phase 8 + Phase 9 + Phase 12.1) ----
    ShadowPass shadowPass(dx.GetShadowMap(), dx.GetMeshRenderer());
    SkyPass skyPass(skyRenderer);
    GBufferPass gbufferPass(dx.GetMeshRenderer());
    DeferredLightingPass deferredLightingPass(dx.GetShadowMap(), dx.GetMeshRenderer());
    GridPass gridPass(gridRenderer);
    SSRPass ssrPass;
    TransparentMeshPass transparentMeshPass(dx.GetMeshRenderer(), dx.GetShadowMap());
    TransparentPass transparentPass(dx.GetParticleRenderer());
    SSAOPass ssaoPass(ssaoRenderer);
    BloomPass bloomPass(postProcess);
    TonemapPass tonemapPass(postProcess);
    DOFPass dofPass(postProcess);
    VelocityGenPass velocityGenPass(postProcess);
    TAAPass taaPass(postProcess);
    MotionBlurPass motionBlurPass(postProcess);
    FXAAPass fxaaPass(postProcess);
    HighlightPass highlightPass(dx.GetMeshRenderer());
    UIPass uiPass(imgui);

    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    auto prev = start;

    float fpsTimer = 0.0f;
    uint32_t fpsFrames = 0;
    float fpsValue = 0.0f;
    float skyExposure = 0.3f;
    float gameTimeOfDayHours = 12.0f;
    bool gameTimeAuto = true;
    // 通常プレイは 1 real second = 1 game minute（24 分で一日）。
    float gameHoursPerSecond = 1.0f / 60.0f;
    float gameRuntimeSeconds = 0.0f;
    DirectX::XMFLOAT3 gameCameraPosition = {0.0f, 4.0f, -20.0f};
    float waterWaveHeight = 1.0f;
    float waterWaveSpeed = 1.0f;
    float waterWaveFrequency = 1.0f;
    float waterTransparency = 0.45f;
    float wetSurfaceStrength = 0.9f;
    float wetSurfaceDrySeconds = 4.0f;
    float wetSurfaceImpactRadius = 4.5f;
    float wetSurfaceCycleSeconds = 5.5f;
    float puddleStrength = 1.0f;
    float puddleBuildSeconds = 0.0f;
    float puddleRadius = 3.2f;
    float puddleClarity = 0.90f;
    float puddleTint = 0.28f;
    float puddleRippleStrength = 0.35f;
    constexpr float kInkEdgeReviewStrength = 0.65f;
    constexpr float kInkFlowReviewStrength = 0.90f;
    constexpr float kInkFlowReviewSpeed = 1.50f;
    float inkWashStrength = kInkEdgeReviewStrength;
    float inkFlowStrength = kInkFlowReviewStrength;
    float inkFlowSpeed = kInkFlowReviewSpeed;
    overworldScene.SetWaterTransparency(dx, waterTransparency);

    // IBL (Phase 10.2)
    bool iblEnabled = true;

    // Settings UI (Phase 12.6)
    bool showSettings = false;
    bool prevEsc = false;
    bool gameFreeCameraEnabled = false;

    // ---- Editor/Game mode toggle (Milestone 4 Phase 0) ----
    enum class AppMode { Title, Game, Tavern, BossArena, Editor };
    AppMode appMode =
        bossRequested                   ? AppMode::BossArena
        : playerAnimationSmokeRequested ? AppMode::Game
        : tavernRequested
            ? AppMode::Tavern
            : (overworldRequested
                   ? AppMode::Game
                   : (launchEditor ? AppMode::Editor : AppMode::Title));
    bool requestQuit = false;
    const auto failTavernManagementSmoke = [&](const std::string &reason) {
      if (!tavernManagementSmokeRequested || tavernManagementSmokeFailed)
        return;
      tavernManagementSmokeFailed = true;
      applicationExitCode = 2;
      requestQuit = true;
      const std::string message = "Tavern management smoke: FAIL; " + reason;
      TraceAppEvent(message.c_str());
    };
    const auto failTavernSuppliesSmoke = [&](const std::string &reason) {
      if (!tavernSuppliesSmokeRequested || tavernSuppliesSmokeFailed)
        return;
      tavernSuppliesSmokeFailed = true;
      applicationExitCode = 2;
      requestQuit = true;
      const std::string message = "Tavern supplies smoke: FAIL; " + reason;
      TraceAppEvent(message.c_str());
    };
    const auto failTavernUpgradesSmoke = [&](const std::string &reason) {
      if (!tavernUpgradesSmokeRequested || tavernUpgradesSmokeFailed)
        return;
      tavernUpgradesSmokeFailed = true;
      applicationExitCode = 2;
      requestQuit = true;
      const std::string message = "Tavern upgrades smoke: FAIL; " + reason;
      TraceAppEvent(message.c_str());
    };
    bool bossMirrorPickupSmokeFailed = false;
    bool bossMirrorPickupObserved = false;
    bool bossMirrorPickupSmokeCompleted = false;
    int bossMirrorPickupSmokeFrames = 0;
    int bossMirrorPickupCollectedCount = 0;
    int bossMirrorPickupRenderedActionFrames = 0;
    int bossMirrorPickupRenderedResponses = 0;
    bool bossMirrorPickupActionPoseThisFrame = false;
    bool bossMirrorPickupTriggeredThisFrame = false;
    bool bossMirrorPickupAwaitingResponse = false;
    bool bossMirrorPickupTeleportPending = false;
    DirectX::XMFLOAT3 bossMirrorPickupPendingPosition{};
    int bossMirrorPickupInitialCharge = 0;
    uint64_t bossMirrorPickupInitialActionSerial = 0;
    uint64_t bossMirrorPickupExpectedActionSerial = 0;
    const auto failBossMirrorPickupSmoke = [&](const std::string &reason) {
      if (!bossMirrorPickupSmokeRequested || bossMirrorPickupSmokeFailed)
        return;
      bossMirrorPickupSmokeFailed = true;
      applicationExitCode = 2;
      requestQuit = true;
      const std::string message = "Boss mirror pickup smoke: FAIL; " + reason;
      TraceAppEvent(message.c_str());
    };
    if (bossMirrorPickupSmokeRequested) {
      const PlayerAnimationPreview::ClipDiagnostics diagnostics =
          playerPreview.GetTakingItemDiagnostics();
      if (!bossArenaScene.IsReady())
        failBossMirrorPickupSmoke("BossArena is not ready");
      if (!diagnostics.loaded || diagnostics.duration <= 0.0f ||
          !std::isfinite(diagnostics.duration) || diagnostics.trackCount == 0)
        failBossMirrorPickupSmoke("TakingItem animation is not ready");

      DirectX::XMFLOAT3 chargePosition{};
      if (!bossArenaScene.TryGetActiveMirrorChargePosition(chargePosition)) {
        failBossMirrorPickupSmoke("no active mirror charge is available");
      } else {
        chargePosition.y = 0.0f;
        playerPreview.SetPosition(chargePosition);
        playerPreview.SetYaw(0.0f);
        gameCameraPosition = {chargePosition.x, 3.2f, chargePosition.z - 5.8f};
        cam.SetPosition(gameCameraPosition.x, gameCameraPosition.y,
                        gameCameraPosition.z);
        cam.SetYawPitch(0.0f, -0.28f);
      }
      bossMirrorPickupInitialCharge = bossArenaScene.MirrorChargeCount();
      bossMirrorPickupInitialActionSerial = playerPreview.ActionTriggerSerial();
      TraceAppEvent("Boss mirror pickup smoke: route ready");
    }
    if (bossShortcutRequested && !bossMirrorPickupSmokeRequested) {
      const DirectX::XMFLOAT3 playerPosition = playerPreview.Position();
      gameCameraPosition = {playerPosition.x, playerPosition.y + 3.2f,
                            playerPosition.z - 5.8f};
      cam.SetPosition(gameCameraPosition.x, gameCameraPosition.y,
                      gameCameraPosition.z);
      cam.SetYawPitch(0.0f, -0.28f);
      cam.SetLens(DirectX::XM_PIDIV4,
                  static_cast<float>(window.Width()) /
                      static_cast<float>(window.Height()),
                  0.1f, 1000.0f);
      TraceAppEvent("debug shortcut: BossArena ready");
    }
    const auto enterTavernMode = [&]() {
      appMode = AppMode::Tavern;
      showSettings = false;
      gameFreeCameraEnabled = false;
      gameRuntimeSeconds = 0.0f;
      tavernScene.SetBusinessHour(gameTimeOfDayHours);
      playerPreview.SetPosition(tavernScene.PlayerSpawnPosition());
      playerPreview.SetYaw(0.0f);
      const DirectX::XMFLOAT3 cameraPosition =
          tavernFirstPersonEnabled
              ? tavernScene.FirstPersonCameraPosition(playerPreview.Position())
              : tavernScene.CameraPosition();
      gameCameraPosition = cameraPosition;
      cam.SetPosition(cameraPosition.x, cameraPosition.y, cameraPosition.z);
      cam.SetYawPitch(tavernScene.CameraYaw(),
                      tavernFirstPersonEnabled ? -0.06f
                                               : tavernScene.CameraPitch());
      cam.SetLens(tavernFirstPersonEnabled ? DirectX::XM_PI / 3.0f
                                           : DirectX::XM_PIDIV4,
                  static_cast<float>(window.Width()) /
                      static_cast<float>(window.Height()),
                  0.1f, 1000.0f);
      TraceAppEvent(tavernFirstPersonEnabled
                        ? "tavern transition: enter first-person"
                        : "tavern transition: enter third-person fallback");
    };
    const auto returnFromTavern = [&]() {
      appMode = AppMode::Game;
      showSettings = false;
      playerPreview.SetPosition(overworldScene.TavernReturnPosition());
      playerPreview.SetYaw(0.0f);
      const DirectX::XMFLOAT3 playerPosition = playerPreview.Position();
      gameCameraPosition = {playerPosition.x, 3.2f, playerPosition.z - 5.8f};
      cam.SetPosition(gameCameraPosition.x, gameCameraPosition.y,
                      gameCameraPosition.z);
      cam.SetYawPitch(0.0f, -0.28f);
      cam.SetLens(DirectX::XM_PIDIV4,
                  static_cast<float>(window.Width()) /
                      static_cast<float>(window.Height()),
                  0.1f, 1000.0f);
      TraceAppEvent("tavern transition: return to overworld");
    };
    if (tavernRequested && !playerAnimationSmokeRequested &&
        !bossMirrorPickupSmokeRequested)
      enterTavernMode();
    if (tavernGameplaySmokeRequested) {
      std::string waitingFailure;
      if (!TavernScene::RunEntranceWaitingRegression(waitingFailure)) {
        TraceAppEvent(("Tavern entrance regression: FAIL; " + waitingFailure).c_str());
        applicationExitCode = 2;
        requestQuit = true;
      } else {
        TraceAppEvent("Tavern entrance regression: PASS; dirty arrival, collection, "
                      "timeout, gold floor, pause, closing, reset, tutorial, Table 3");
      }
      std::string bubbleFailure;
      if (!tavernScene.RunOrderBubbleRegression(bubbleFailure)) {
        TraceAppEvent(("Tavern order bubble regression: FAIL; " + bubbleFailure).c_str());
        applicationExitCode = 2;
        requestQuit = true;
      } else {
        TraceAppEvent("Tavern order bubble regression: PASS; acceptance, persistence, "
                      "Table 3, billboard, Ale/Food models, fallback, serve, "
                      "walkout, reset");
      }
      std::string kitchenFailure;
      if (!TavernScene::RunKitchenRegression(kitchenFailure)) {
        TraceAppEvent(("Tavern Kitchen regression: FAIL; " + kitchenFailure)
                          .c_str());
        applicationExitCode = 2;
        requestQuit = true;
      } else {
        TraceAppEvent("Tavern Kitchen regression: PASS; mixed order, asynchronous "
                      "cooking, pause, serve, bowl wash, walkout, reset");
      }
      std::string expansionFailure;
      if (!TavernScene::RunExpansionRegression(expansionFailure)) {
        TraceAppEvent(("Tavern expansion regression: FAIL; " + expansionFailure)
                          .c_str());
        applicationExitCode = 2;
        requestQuit = true;
      } else {
        TraceAppEvent("Tavern expansion regression: PASS; 2-seat and 4-seat "
                      "purchases, party capacity, rewards, movable layout, "
                      "day persistence, new-game reset");
      }
    }
    if (tavernWaitingPreviewRequested && tavernSmokeModeCount == 0) {
      tavernScene.ConfigureEntranceWaitingPreview();
      playerPreview.SetPosition({0.0f, 0.0f, 1.6f});
      playerPreview.SetYaw(DirectX::XM_PI);
      cam.SetYawPitch(DirectX::XM_PI, -0.06f);
    }
    if (tavernOrderPreviewRequested && tavernSmokeModeCount == 0) {
      tavernScene.ConfigureOrderBubblePreview();
      playerPreview.SetPosition({0.0f, 0.0f, -0.35f});
      cam.SetYawPitch(0.0f, tavernFirstPersonEnabled ? -0.06f : -0.28f);
    }
    if (tavernKitchenPreviewRequested && tavernSmokeModeCount == 0) {
      tavernScene.ConfigureKitchenPreview();
      playerPreview.SetPosition({4.75f, 0.0f, 5.75f});
      playerPreview.SetYaw(0.0f);
      cam.SetYawPitch(0.0f, tavernFirstPersonEnabled ? -0.06f : -0.28f);
    }
    if (tavernManagementSmokeRequested) {
      const DirectX::XMFLOAT3 managementPosition =
          tavernScene.ManagementInteractionPosition();
      playerPreview.SetPosition(managementPosition);
      playerPreview.SetYaw(DirectX::XM_PIDIV2);
      tavernManagementBaselineGold = tavernScene.Gold();
      tavernManagementBaselineCompletedCycles = tavernScene.CompletedCycles();
      TraceAppEvent(
          "Tavern management smoke: production interaction route ready");
    }
    if (tavernSuppliesSmokeRequested) {
      const DirectX::XMFLOAT3 managementPosition =
          tavernScene.ManagementInteractionPosition();
      playerPreview.SetPosition(managementPosition);
      playerPreview.SetYaw(DirectX::XM_PIDIV2);
      if (tavernScene.AleStock() != 6 || tavernScene.AleCapacity() != 6 ||
          tavernScene.AleUnitPrice() != 2 || tavernScene.Gold() != 0) {
        failTavernSuppliesSmoke(
            "initial economy was not ALE 6/6 at 2 G each with 0 G");
      } else {
        tavernSuppliesPurchaseSerialBefore = tavernScene.SupplyPurchaseSerial();
        TraceAppEvent("Tavern supplies smoke: initial ALE 6/6 route ready");
      }
    }
    if (tavernUpgradesSmokeRequested) {
      const DirectX::XMFLOAT3 managementPosition =
          tavernScene.ManagementInteractionPosition();
      playerPreview.SetPosition(managementPosition);
      playerPreview.SetYaw(DirectX::XM_PIDIV2);
      tavernScene.ConfigureUpgradesSmokeState(200, 6, true);
      if (tavernScene.Gold() != 200 || tavernScene.AleStock() != 6 ||
          tavernScene.AleCapacity() != 6 ||
          tavernScene.AleCapacityLevel() != 0 || tavernScene.TotalMugs() != 2 ||
          tavernScene.ExtraMugOwned() || tavernScene.Table3Unlocked() ||
          tavernScene.Table3Enabled()) {
        failTavernUpgradesSmoke("initial progression was not 200 G, 2 mugs, "
                                "ALE 6/6, and Table 3 closed");
      } else {
        tavernUpgradesPurchaseSerialBefore =
            tavernScene.UpgradePurchaseSerial();
        TraceAppEvent("Tavern upgrades smoke: 200 G progression route ready");
      }
    }
    TitleScreen titleScreen;
    Scene editorScene;
    SceneEditor sceneEditor;
    PopulateEditorWelcomeScene(editorScene, dx);
    TraceAppEvent("startup: editor welcome scene ready");
    CameraPreset editorReturnCamera;
    bool editorReturnCameraValid = false;
    if (overworldRequested || playerAnimationSmokeRequested) {
      playerPreview.SetPosition(overworldScene.PlayerSpawnPosition());
      playerPreview.SetYaw(0.0f);
      const DirectX::XMFLOAT3 spawn = playerPreview.Position();
      gameCameraPosition = {spawn.x, 3.2f, spawn.z - 5.8f};
      cam.SetPosition(gameCameraPosition.x, gameCameraPosition.y,
                      gameCameraPosition.z);
      cam.SetYawPitch(0.0f, -0.28f);
      if (playerAnimationSmokeRequested) {
        TraceAppEvent("Player animation smoke: Overworld route ready");
      } else if (overworldShortcutRequested) {
        TraceAppEvent("debug shortcut: Overworld ready");
      } else {
        TraceAppEvent(
            dxrOverworldSmokeRequested
                ? "DXR overworld smoke: reflection monolith route ready"
                : "DXR overworld review: reflection monolith route ready");
      }
    }
    bool scenePlayMode = false;
    std::string sceneSnapshot;
    const auto stopEditorScenePreview = [&]() {
      if (!scenePlayMode)
        return;
      dx.WaitForGpu();
      editorScene.DeserializeFromString(sceneSnapshot, dx);
      sceneSnapshot.clear();
      scenePlayMode = false;
    };
    auto &forestDebugSettings = overworldScene.BackgroundForestDebug();
    EditorRuntimeBindings editorRuntimeBindings;
    editorRuntimeBindings.skyExposure = &skyExposure;
    editorRuntimeBindings.timeOfDayHours = &gameTimeOfDayHours;
    editorRuntimeBindings.automaticTime = &gameTimeAuto;
    editorRuntimeBindings.hoursPerSecond = &gameHoursPerSecond;
    editorRuntimeBindings.rainEnabled = &rainEnabled;
    editorRuntimeBindings.waterWaveHeight = &waterWaveHeight;
    editorRuntimeBindings.waterWaveSpeed = &waterWaveSpeed;
    editorRuntimeBindings.waterWaveFrequency = &waterWaveFrequency;
    editorRuntimeBindings.waterTransparency = &waterTransparency;
    editorRuntimeBindings.wetSurfaceStrength = &wetSurfaceStrength;
    editorRuntimeBindings.wetSurfaceDrySeconds = &wetSurfaceDrySeconds;
    editorRuntimeBindings.wetSurfaceImpactRadius = &wetSurfaceImpactRadius;
    editorRuntimeBindings.wetSurfaceCycleSeconds = &wetSurfaceCycleSeconds;
    editorRuntimeBindings.puddleStrength = &puddleStrength;
    editorRuntimeBindings.puddleBuildSeconds = &puddleBuildSeconds;
    editorRuntimeBindings.puddleRadius = &puddleRadius;
    editorRuntimeBindings.puddleClarity = &puddleClarity;
    editorRuntimeBindings.puddleTint = &puddleTint;
    editorRuntimeBindings.puddleRippleStrength = &puddleRippleStrength;
    editorRuntimeBindings.forestEnabled = &forestDebugSettings.enabled;
    editorRuntimeBindings.forestSingleCluster =
        &forestDebugSettings.singleClusterPreview;
    editorRuntimeBindings.forestDensity = &forestDebugSettings.densityLevel;
    editorRuntimeBindings.forestScale = &forestDebugSettings.scaleMultiplier;
    editorRuntimeBindings.forestDistance = &forestDebugSettings.distanceOffset;
    editorRuntimeBindings.forestSpacing = &forestDebugSettings.spacingMultiplier;
    editorRuntimeBindings.particlesEnabled = &particlesEnabled;
    editorRuntimeBindings.fireEnabled = &fireEnabled;
    editorRuntimeBindings.smokeEnabled = &smokeEnabled;
    editorRuntimeBindings.sparkEnabled = &sparkEnabled;
    editorRuntimeBindings.particleDepth = &particleDepth;
    editorRuntimeBindings.collisionDebug = &showCollisionDebug;
    editorRuntimeBindings.modelMeshCollision = &useModelMeshCollision;
    editorRuntimeBindings.modelCollisionDebug = &showModelCollisionDebug;
    editorRuntimeBindings.gameFreeCamera = &gameFreeCameraEnabled;
    editorRuntimeBindings.setWaterTransparency = [&](float transparency) {
      overworldScene.SetWaterTransparency(dx, transparency);
    };
    editorRuntimeBindings.resetWater = [&]() {
      waterWaveHeight = 1.0f;
      waterWaveSpeed = 1.0f;
      waterWaveFrequency = 1.0f;
      waterTransparency = 0.45f;
      overworldScene.SetWaterTransparency(dx, waterTransparency);
      wetSurfaceStrength = 0.9f;
      wetSurfaceDrySeconds = 4.0f;
      wetSurfaceImpactRadius = 4.5f;
      wetSurfaceCycleSeconds = 5.5f;
      puddleStrength = 1.0f;
      puddleBuildSeconds = 0.0f;
      puddleRadius = 3.2f;
      puddleClarity = 0.90f;
      puddleTint = 0.28f;
      puddleRippleStrength = 0.35f;
    };
    editorRuntimeBindings.resetForest = [&]() {
      forestDebugSettings.enabled = true;
      forestDebugSettings.singleClusterPreview = false;
      forestDebugSettings.densityLevel = 3;
      forestDebugSettings.scaleMultiplier = 0.40f;
      forestDebugSettings.distanceOffset = 32.0f;
      forestDebugSettings.spacingMultiplier = 0.5f;
    };
    editorRuntimeBindings.reloadPlacements = [&]() {
      if (overworldScene.ReloadPlacements(dx)) {
        overworldCollisionShapes = overworldScene.BuildDefaultCollisionShapes();
        overworldCollisionColliders =
            overworldScene.BuildCollisionColliders(overworldCollisionShapes);
      }
    };
    editorRuntimeBindings.drawAnimationControls = [&]() {
      playerPreview.DrawDebugControls();
    };
    editorRuntimeBindings.playTitle = [&]() {
      stopEditorScenePreview();
      editorReturnCamera = cam.MakePreset("Editor Return");
      editorReturnCameraValid = true;
      appMode = AppMode::Title;
      showSettings = false;
    };
    editorRuntimeBindings.playOverworld = [&]() {
      stopEditorScenePreview();
      editorReturnCamera = cam.MakePreset("Editor Return");
      editorReturnCameraValid = true;
      appMode = AppMode::Game;
      showSettings = false;
      gameRuntimeSeconds = 0.0f;
      playerPreview.SetPosition(overworldScene.PlayerSpawnPosition());
      playerPreview.SetYaw(0.0f);
      const DirectX::XMFLOAT3 spawn = playerPreview.Position();
      gameCameraPosition = {spawn.x, 3.2f, spawn.z - 5.8f};
      cam.SetPosition(gameCameraPosition.x, gameCameraPosition.y,
                      gameCameraPosition.z);
      cam.SetYawPitch(0.0f, -0.28f);
      cam.SetLens(DirectX::XM_PIDIV4,
                  static_cast<float>(window.Width()) /
                      static_cast<float>(window.Height()),
                  0.1f, 1000.0f);
    };
    editorRuntimeBindings.playBossArena = [&]() {
      stopEditorScenePreview();
      editorReturnCamera = cam.MakePreset("Editor Return");
      editorReturnCameraValid = true;
      appMode = AppMode::BossArena;
      showSettings = false;
      gameRuntimeSeconds = 0.0f;
      bossArenaScene.Reset(playerPreview);
      const DirectX::XMFLOAT3 playerPosition = playerPreview.Position();
      gameCameraPosition = {playerPosition.x, 3.2f,
                            playerPosition.z - 5.8f};
      cam.SetPosition(gameCameraPosition.x, gameCameraPosition.y,
                      gameCameraPosition.z);
      cam.SetYawPitch(0.0f, -0.28f);
      cam.SetLens(DirectX::XM_PIDIV4,
                  static_cast<float>(window.Width()) /
                      static_cast<float>(window.Height()),
                  0.1f, 1000.0f);
    };
    editorRuntimeBindings.saveRuntimeSettings = [&]() {
      try {
        nlohmann::json settings = {
            {"schemaVersion", 1},
            {"skyExposure", skyExposure},
            {"timeOfDayHours", gameTimeOfDayHours},
            {"automaticTime", gameTimeAuto},
            {"hoursPerSecond", gameHoursPerSecond},
            {"rainEnabled", rainEnabled},
            {"waterWaveHeight", waterWaveHeight},
            {"waterWaveSpeed", waterWaveSpeed},
            {"waterWaveFrequency", waterWaveFrequency},
            {"waterTransparency", waterTransparency},
            {"wetSurfaceStrength", wetSurfaceStrength},
            {"wetSurfaceDrySeconds", wetSurfaceDrySeconds},
            {"wetSurfaceImpactRadius", wetSurfaceImpactRadius},
            {"wetSurfaceCycleSeconds", wetSurfaceCycleSeconds},
            {"puddleStrength", puddleStrength},
            {"puddleBuildSeconds", puddleBuildSeconds},
            {"puddleRadius", puddleRadius},
            {"puddleClarity", puddleClarity},
            {"puddleTint", puddleTint},
            {"puddleRippleStrength", puddleRippleStrength},
            {"forestEnabled", forestDebugSettings.enabled},
            {"forestSingleCluster",
             forestDebugSettings.singleClusterPreview},
            {"forestDensity", forestDebugSettings.densityLevel},
            {"forestScale", forestDebugSettings.scaleMultiplier},
            {"forestDistance", forestDebugSettings.distanceOffset},
            {"forestSpacing", forestDebugSettings.spacingMultiplier},
            {"particlesEnabled", particlesEnabled},
            {"fireEnabled", fireEnabled},
            {"smokeEnabled", smokeEnabled},
            {"sparkEnabled", sparkEnabled},
            {"particleDepth", particleDepth},
            {"collisionDebug", showCollisionDebug},
            {"modelMeshCollision", useModelMeshCollision},
            {"modelCollisionDebug", showModelCollisionDebug},
            {"gameFreeCamera", gameFreeCameraEnabled},
        };
        std::ofstream output("featuretools/editor_runtime_settings.json",
                             std::ios::out | std::ios::trunc);
        if (!output)
          return false;
        output << settings.dump(2) << '\n';
        return output.good();
      } catch (...) {
        return false;
      }
    };
    editorRuntimeBindings.loadRuntimeSettings = [&]() {
      try {
        std::ifstream input("featuretools/editor_runtime_settings.json");
        if (!input)
          return false;
        nlohmann::json settings;
        input >> settings;
        const auto loadValue = [&](const char *key, auto &value) {
          if (settings.contains(key))
            settings.at(key).get_to(value);
        };
        loadValue("skyExposure", skyExposure);
        loadValue("timeOfDayHours", gameTimeOfDayHours);
        loadValue("automaticTime", gameTimeAuto);
        loadValue("hoursPerSecond", gameHoursPerSecond);
        loadValue("rainEnabled", rainEnabled);
        loadValue("waterWaveHeight", waterWaveHeight);
        loadValue("waterWaveSpeed", waterWaveSpeed);
        loadValue("waterWaveFrequency", waterWaveFrequency);
        loadValue("waterTransparency", waterTransparency);
        loadValue("wetSurfaceStrength", wetSurfaceStrength);
        loadValue("wetSurfaceDrySeconds", wetSurfaceDrySeconds);
        loadValue("wetSurfaceImpactRadius", wetSurfaceImpactRadius);
        loadValue("wetSurfaceCycleSeconds", wetSurfaceCycleSeconds);
        loadValue("puddleStrength", puddleStrength);
        loadValue("puddleBuildSeconds", puddleBuildSeconds);
        loadValue("puddleRadius", puddleRadius);
        loadValue("puddleClarity", puddleClarity);
        loadValue("puddleTint", puddleTint);
        loadValue("puddleRippleStrength", puddleRippleStrength);
        loadValue("forestEnabled", forestDebugSettings.enabled);
        loadValue("forestSingleCluster",
                  forestDebugSettings.singleClusterPreview);
        loadValue("forestDensity", forestDebugSettings.densityLevel);
        loadValue("forestScale", forestDebugSettings.scaleMultiplier);
        loadValue("forestDistance", forestDebugSettings.distanceOffset);
        loadValue("forestSpacing", forestDebugSettings.spacingMultiplier);
        loadValue("particlesEnabled", particlesEnabled);
        loadValue("fireEnabled", fireEnabled);
        loadValue("smokeEnabled", smokeEnabled);
        loadValue("sparkEnabled", sparkEnabled);
        loadValue("particleDepth", particleDepth);
        loadValue("collisionDebug", showCollisionDebug);
        loadValue("modelMeshCollision", useModelMeshCollision);
        loadValue("modelCollisionDebug", showModelCollisionDebug);
        loadValue("gameFreeCamera", gameFreeCameraEnabled);
        overworldScene.SetWaterTransparency(dx, waterTransparency);
        return true;
      } catch (...) {
        return false;
      }
    };
    StageData editStage; // Grid editor stage data (Phase 5).
    editStage.Clear();   // Initialize with default grid.
    sceneEditor.InitEditorMeshes(dx); // Phase 5B: create viewport tile meshes.
    bool prevF1 = false;
    bool prevF5 = false;
    bool prevTavernInteractKey = false;
    bool prevTavernRestartKey = false;
#if defined(_DEBUG)
    bool prevF4 = false;
    bool prevF7 = false;
    bool prevF8 = false;
    bool showTavernDebug = false;
#endif
    bool prevLButton = false; // for edge-detection of left-click (mouse pick)

    // Shader hot-reload (Phase 8).
    bool prevF9 = false;
    bool prevF10 = false;
    std::string g_shaderReloadErrors;
    float g_shaderReloadTimer = 0.0f;
    static constexpr float kShaderMsgOkDuration = 3.0f;
    static constexpr float kShaderMsgErrDuration = 10.0f;
    int gameTraceFramesRemaining = 12;

    SetStartupStage(70);
    TraceAppEvent("startup: entering main loop");
    while (window.PumpMessages()) {
      bossMirrorPickupActionPoseThisFrame = false;
      bossMirrorPickupTriggeredThisFrame = false;
      if (resizeCtx.pendingResize) {
        resizeCtx.pendingResize = false;
        const uint32_t resizeW = resizeCtx.width;
        const uint32_t resizeH = resizeCtx.height;
        if (resizeW != dx.Width() || resizeH != dx.Height()) {
          TraceAppEvent("resize: dx.Resize begin");
          dx.Resize(resizeW, resizeH);
          TraceAppEvent("resize: dx.Resize end");
        }
        if (resizeH != 0) {
          const float aspect =
              static_cast<float>(resizeW) / static_cast<float>(resizeH);
          // Preserve current FOV/near/far when resizing.
          cam.SetLens(cam.FovY(), aspect, cam.NearZ(), cam.FarZ());
        }
      }

      auto now = clock::now();
      float dt = std::chrono::duration<float>(now - prev).count();
      prev = now;

      auto t = std::chrono::duration<float>(now - start).count();
      float r = 0.1f + 0.1f * (0.5f + 0.5f * sinf(t));
      float g = 0.1f + 0.1f * (0.5f + 0.5f * sinf(t * 1.7f));
      float b = 0.2f + 0.2f * (0.5f + 0.5f * sinf(t * 0.9f));
      const float tavernClockScale =
          tavernDaySmokeRequested
              ? 60.0f
              : (tavernGameplaySmokeRequested ? 12.0f : 1.0f);
      const float worldClockDelta =
          dt * (appMode == AppMode::Tavern ? tavernClockScale : 1.0f);
      const bool tavernManagementPaused =
          appMode == AppMode::Tavern && tavernScene.ManagementMenuOpen();
      if ((appMode == AppMode::Game || appMode == AppMode::Tavern) &&
          gameTimeAuto && !tavernManagementPaused) {
        gameTimeOfDayHours =
            std::fmod(gameTimeOfDayHours +
                          worldClockDelta * gameHoursPerSecond,
                      24.0f);
      }
      const float gameDaylightT = DaylightTFromHour(gameTimeOfDayHours);
      const float gameSkyExposure = LerpFloat(
          0.01f, 0.25f, gameDaylightT); // Sky exposure linear from 0.01 to 0.25
      const float gamePostExposure = LerpFloat(
          0.1f, 1.0f, gameDaylightT); // Post exposure linear from 0.1 to 1.0
      const GameTimeLightingProfile gameLighting =
          BuildGameTimeLighting(gameTimeOfDayHours);
      const GameTimeLightingProfile tavernLighting = BuildTavernLighting();
      constexpr float kGameIblIntensity = 0.7f;

      auto &input = window.GetInput();
      input.PollGamepad();
      if (tavernManagementSmokeRequested && !tavernManagementSmokeCompleted &&
          !tavernManagementSmokeFailed) {
        ++tavernManagementSmokeLoopFrames;
        if (tavernManagementSmokeLoopFrames > 240) {
          failTavernManagementSmoke(
              "open/select/close route exceeded the 240-frame watchdog");
        }
      }
      if (tavernSuppliesSmokeRequested && !tavernSuppliesSmokeCompleted &&
          !tavernSuppliesSmokeFailed) {
        ++tavernSuppliesSmokeLoopFrames;
        if (tavernSuppliesSmokeLoopFrames > 600) {
          failTavernSuppliesSmoke(
              "full/order/recovery route exceeded the 600-frame watchdog");
        }
      }
      if (tavernUpgradesSmokeRequested && !tavernUpgradesSmokeCompleted &&
          !tavernUpgradesSmokeFailed) {
        ++tavernUpgradesSmokeLoopFrames;
        if (tavernUpgradesSmokeLoopFrames > 900) {
          failTavernUpgradesSmoke("purchase/persistence/Table 3 route exceeded "
                                  "the 900-frame watchdog");
        }
      }

      // The dedicated editor can run the real game full-screen. F1 restores
      // the authoring camera and workspace without restarting the process.
      const bool f1Now = input.IsKeyDown(VK_F1);
      if (launchEditor && f1Now && !prevF1 &&
          (appMode == AppMode::Title || appMode == AppMode::Game ||
           appMode == AppMode::Tavern || appMode == AppMode::BossArena)) {
        appMode = AppMode::Editor;
        showSettings = false;
        if (editorReturnCameraValid)
          cam.ApplyPreset(editorReturnCamera);
      }
      prevF1 = f1Now;

      // ---- F4: 酒場 gameplay debug panel（Debug のみ） ----
#if defined(_DEBUG)
      {
        const bool f4Now = input.IsKeyDown(VK_F4);
        if (f4Now && !prevF4 && appMode == AppMode::Tavern)
          showTavernDebug = !showTavernDebug;
        prevF4 = f4Now;
      }
#endif

      // ---- F5: シーンプレイモードのトグル（ゲームロジック未実装のためエディタ⇔シーンプレイのみ） ----
      {
        const bool f5Now = input.IsKeyDown(VK_F5);
        if (f5Now && !prevF5 && appMode == AppMode::Editor) {
          if (scenePlayMode) {
            // シーンプレイを停止し、スナップショットから復元する。
            dx.WaitForGpu();
            editorScene.DeserializeFromString(sceneSnapshot, dx);
            sceneSnapshot.clear();
            scenePlayMode = false;
          } else {
            // シーンプレイを開始する。
            sceneSnapshot = editorScene.SerializeToString();
            scenePlayMode = true;
          }
        }
        prevF5 = f5Now;
      }

      // Check scene play request from menu bar (Phase 8).
      if (appMode == AppMode::Editor &&
          sceneEditor.ConsumeScenePlayRequest()) {
        if (scenePlayMode) {
          dx.WaitForGpu();
          editorScene.DeserializeFromString(sceneSnapshot, dx);
          sceneSnapshot.clear();
          scenePlayMode = false;
        } else {
          sceneSnapshot = editorScene.SerializeToString();
          scenePlayMode = true;
        }
      }

      // ---- F6: toggle Grid Editor panel (Phase 5) ----
      {
        static bool prevF6 = false;
        const bool f6Now = input.IsKeyDown(VK_F6);
        if (f6Now && !prevF6 && appMode == AppMode::Editor)
          sceneEditor.ToggleGridEditor();
        prevF6 = f6Now;
      }

      // ---- F7: Phase 2 水墨輪郭と墨流れの比較切り替え（Debug のみ） ----
#if defined(_DEBUG)
      {
        const bool f7Now = input.IsKeyDown(VK_F7);
        if (f7Now && !prevF7 &&
            appMode == AppMode::BossArena && bossArenaScene.PhaseTwoActive()) {
          const bool inkEnabled =
              inkWashStrength > 0.001f || inkFlowStrength > 0.001f;
          inkWashStrength = inkEnabled ? 0.0f : kInkEdgeReviewStrength;
          inkFlowStrength = inkEnabled ? 0.0f : kInkFlowReviewStrength;
          if (!inkEnabled)
            inkFlowSpeed = kInkFlowReviewSpeed;
        }
        prevF7 = f7Now;
      }
#endif

      // ---- F8: 反射経路の比較（Debug のみ） ----
#if defined(_DEBUG)
      {
        const bool f8Now = input.IsKeyDown(VK_F8);
        if (f8Now && !prevF8) {
          switch (reflectionMode) {
          case ReflectionMode::Off:
            reflectionMode = ReflectionMode::SSR;
            TraceAppEvent("reflection mode: SSR");
            break;
          case ReflectionMode::SSR:
            if (hybridReflection.IsSupported()) {
              reflectionMode = ReflectionMode::HybridDXR;
              TraceAppEvent("reflection mode: HybridDXR");
            } else {
              reflectionMode = ReflectionMode::Off;
              TraceAppEvent("reflection mode: Off (DXR unavailable)");
            }
            break;
          case ReflectionMode::HybridDXR:
            reflectionMode = ReflectionMode::Off;
            TraceAppEvent("reflection mode: Off");
            break;
          }
        }
        prevF8 = f8Now;
      }
#endif

      // ---- F9: shader hot-reload (Phase 8) ----
      {
        const bool f9Now = input.IsKeyDown(VK_F9);
        if (f9Now && !prevF9) {
          dx.WaitForGpu();
          g_shaderReloadErrors.clear();
          g_shaderReloadErrors += dx.GetMeshRenderer().ReloadShaders(dx);
          g_shaderReloadErrors += postProcess.ReloadShaders(dx);
          g_shaderReloadErrors += ssaoRenderer.ReloadShaders(dx);
          g_shaderReloadErrors += skyRenderer.ReloadShaders(dx);
          g_shaderReloadErrors += gridRenderer.ReloadShaders(dx);
          g_shaderReloadErrors += deferredLightingPass.ReloadShaders(dx);
          g_shaderReloadErrors += ssrPass.ReloadShaders(dx);
          g_shaderReloadErrors += dx.GetParticleRenderer().ReloadShaders(dx);
          g_shaderReloadTimer = g_shaderReloadErrors.empty()
                                    ? kShaderMsgOkDuration
                                    : kShaderMsgErrDuration;
        }
        prevF9 = f9Now;
      }

      // ---- F10: Overworld 配置 JSON の再読み込み ----
      {
        const bool f10Now = input.IsKeyDown(VK_F10);
        if (f10Now && !prevF10 && appMode == AppMode::Game)
          overworldScene.ReloadPlacements(dx);
        prevF10 = f10Now;
      }

      // ---- 設定パネルのトグル（管理 UI 中の Esc は閉じる操作へ渡す） ----
      const bool escNow = input.IsKeyDown(VK_ESCAPE);
      const bool escPressed = escNow && !prevEsc;
      const bool tavernMenuOpenBeforeUi =
          appMode == AppMode::Tavern && tavernScene.ManagementMenuOpen();
      const bool tavernLayoutPlacementBeforeUi =
          appMode == AppMode::Tavern && tavernScene.LayoutPlacementActive();
      if (!imgui.WantCaptureKeyboard() && !tavernMenuOpenBeforeUi &&
          !tavernLayoutPlacementBeforeUi && escPressed) {
        showSettings = !showSettings;
      }
      prevEsc = escNow;

      imgui.BeginFrame(dt);

      const bool uiWantsMouse = imgui.WantCaptureMouse();
      const bool uiWantsKeyboard = imgui.WantCaptureKeyboard();
      const bool tavernFirstPersonLookActive =
          appMode == AppMode::Tavern && tavernFirstPersonEnabled &&
          !gameFreeCameraEnabled && !showSettings &&
          !tavernMenuOpenBeforeUi && !uiWantsMouse && !uiWantsKeyboard;
      window.SetMouseCaptured(tavernFirstPersonLookActive &&
                              GetForegroundWindow() == window.Handle());
      const bool tavernInteractKeyNow = input.IsKeyDown('E');
      const bool tavernGamepadConfirmPressed =
          input.GamepadButtonPressed(XINPUT_GAMEPAD_A);
      const bool tavernInteractPressed =
          (tavernInteractKeyNow && !prevTavernInteractKey) ||
          (!tavernMenuOpenBeforeUi && tavernGamepadConfirmPressed);
      prevTavernInteractKey = tavernInteractKeyNow;
      const bool tavernRestartKeyNow = input.IsKeyDown('R');
      const bool tavernRestartPressed =
          tavernRestartKeyNow && !prevTavernRestartKey;
      prevTavernRestartKey = tavernRestartKeyNow;
      TavernScene::ManagementInput tavernManagementInput;
      if (tavernMenuOpenBeforeUi || tavernLayoutPlacementBeforeUi) {
        tavernManagementInput.previousPressed =
            ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ||
            input.GamepadButtonPressed(XINPUT_GAMEPAD_DPAD_LEFT);
        tavernManagementInput.nextPressed =
            ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ||
            input.GamepadButtonPressed(XINPUT_GAMEPAD_DPAD_RIGHT);
        tavernManagementInput.upPressed =
            ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
            input.GamepadButtonPressed(XINPUT_GAMEPAD_DPAD_UP);
        tavernManagementInput.downPressed =
            ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
            input.GamepadButtonPressed(XINPUT_GAMEPAD_DPAD_DOWN);
        tavernManagementInput.rotateLeftPressed =
            ImGui::IsKeyPressed(ImGuiKey_Q, false) ||
            input.GamepadButtonPressed(XINPUT_GAMEPAD_LEFT_SHOULDER);
        tavernManagementInput.rotateRightPressed =
            ImGui::IsKeyPressed(ImGuiKey_R, false) ||
            input.GamepadButtonPressed(XINPUT_GAMEPAD_RIGHT_SHOULDER);
        tavernManagementInput.cyclePressed =
            ImGui::IsKeyPressed(ImGuiKey_Tab, false) ||
            input.GamepadButtonPressed(XINPUT_GAMEPAD_Y);
        tavernManagementInput.confirmPressed =
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
            tavernGamepadConfirmPressed;
        tavernManagementInput.cancelPressed =
            escPressed || input.GamepadButtonPressed(XINPUT_GAMEPAD_B);
      }

      bool cursorInEditorViewport = false;
      if (appMode == AppMode::Editor) {
        const EditorViewportRect &viewport = sceneEditor.ViewportRect();
        if (viewport.width >= 64.0f && viewport.height >= 64.0f) {
          const float viewportAspect = viewport.width / viewport.height;
          if (std::isfinite(viewportAspect) && viewportAspect > 0.0f &&
              std::abs(viewportAspect - cam.Aspect()) > 0.0001f) {
            cam.SetLens(cam.FovY(), viewportAspect, cam.NearZ(), cam.FarZ());
          }

          POINT cursorPos;
          if (GetCursorPos(&cursorPos) &&
              ScreenToClient(window.Handle(), &cursorPos)) {
            cursorInEditorViewport =
                viewport.Contains(cursorPos.x, cursorPos.y);
          }
        }
      }
      const bool cameraPointerInputAllowed =
          (appMode != AppMode::Editor || cursorInEditorViewport) &&
          (appMode != AppMode::Editor ||
           sceneEditor.CameraNavigationEnabled());

      // Camera input routing — mode-dependent (Phase 6).
      const bool isPlaying = appMode == AppMode::Game ||
                             appMode == AppMode::Tavern ||
                             appMode == AppMode::BossArena;

      // ゲーム中は通常カメラを固定し、設定で有効化した時だけデバッグ用フリーカメラを動かす。
      if (isPlaying && gameFreeCameraEnabled) {
        float scroll = input.ConsumeScrollDelta();
        auto md = input.ConsumeMouseDelta();
        const bool wantMouseLook = !uiWantsMouse && input.IsKeyDown(VK_RBUTTON);
        if (wantMouseLook)
          cam.AddYawPitch(md.dx * cam.LookSpeed(), -md.dy * cam.LookSpeed());
        cam.Update(dt, input, wantMouseLook);
        if (!uiWantsMouse)
          cam.ApplyScrollZoom(scroll);
      } else if (appMode == AppMode::Tavern && tavernFirstPersonEnabled) {
        input.ConsumeScrollDelta();
        const auto md = input.ConsumeMouseDelta();
        if (tavernFirstPersonLookActive) {
          cam.AddYawPitch(md.dx * cam.LookSpeed(),
                          -md.dy * cam.LookSpeed());
          constexpr float kGamepadLookSpeed = 2.25f;
          cam.AddYawPitch(input.RightStickX() * kGamepadLookSpeed * dt,
                          input.RightStickY() * kGamepadLookSpeed * dt);
          cam.SetYawPitch(cam.Yaw(),
                          std::clamp(cam.Pitch(), -1.05f, 1.05f));
        }
      } else if (!isPlaying) {
        // プレイ中以外でスクロールを消費する（プレイ中は将来のゲームロジック側で扱う想定）。
        float scroll = input.ConsumeScrollDelta();
        auto md = input.ConsumeMouseDelta();

        switch (cam.Mode()) {
        case CameraMode::FreeFly: {
          const bool wantMouseLook = cameraPointerInputAllowed &&
                                     !uiWantsMouse && !uiWantsKeyboard &&
                                     input.IsKeyDown(VK_RBUTTON);
          if (wantMouseLook)
            cam.AddYawPitch(md.dx * cam.LookSpeed(), -md.dy * cam.LookSpeed());
          cam.Update(dt, input, wantMouseLook);
          if (cameraPointerInputAllowed && !uiWantsMouse)
            cam.ApplyScrollZoom(scroll);
          break;
        }
        case CameraMode::Orbit: {
          const bool wantOrbit = cameraPointerInputAllowed && !uiWantsMouse &&
                                 !uiWantsKeyboard &&
                                 input.IsKeyDown(VK_RBUTTON);
          if (wantOrbit) {
            cam.SetOrbitAngles(
                cam.OrbitYaw() + md.dx * cam.LookSpeed(),
                cam.OrbitPitch() - md.dy * cam.LookSpeed());
          }
          if (cameraPointerInputAllowed && !uiWantsKeyboard)
            cam.UpdateOrbit(dt, input, wantOrbit);
          if (cameraPointerInputAllowed && !uiWantsMouse)
            cam.ApplyOrbitScrollZoom(scroll);
          break;
        }
        case CameraMode::GameTopDown: {
          if (cameraPointerInputAllowed && !uiWantsKeyboard)
            cam.UpdateGameTopDown(dt, input);
          // Scroll adjusts height.
          if (cameraPointerInputAllowed && !uiWantsMouse && scroll != 0.0f) {
            DirectX::XMFLOAT3 pos = cam.GetPosition();
            pos.y -= scroll * cam.MoveSpeed() * 0.5f;
            pos.y = std::max(1.0f, pos.y);
            cam.SetPosition(pos.x, pos.y, pos.z);
          }
          break;
        }
        default: break;
        }
      }

      // ---- Mouse picking (editor, left-click) ----
      const bool lbNow = input.IsKeyDown(VK_LBUTTON);
      if (appMode == AppMode::Editor) {
        const bool viewportInteractionAllowed =
            !uiWantsMouse && !sceneEditor.GetGizmo().IsActive() &&
            !sceneEditor.GetGizmo().IsHovered();
        const EditorViewportRect &viewport = sceneEditor.ViewportRect();
        POINT cursorPos{};
        const bool cursorAvailable =
            GetCursorPos(&cursorPos) &&
            ScreenToClient(window.Handle(), &cursorPos);
        const bool cursorInViewport =
            cursorAvailable && viewport.Contains(cursorPos.x, cursorPos.y);

        if (viewportInteractionAllowed && cursorInViewport) {
          if (sceneEditor.IsGridEditorOpen()) {
            const int localX = static_cast<int>(cursorPos.x - viewport.x);
            const int localY = static_cast<int>(cursorPos.y - viewport.y);
            const int viewportWidth =
                std::max(1, static_cast<int>(viewport.width));
            const int viewportHeight =
                std::max(1, static_cast<int>(viewport.height));

            // Phase 5B/5C: tower picking and tile painting use coordinates
            // local to the Scene View, not the complete editor window.
            if (lbNow && !prevLButton) {
              const int towerIdx = sceneEditor.ViewportPickTower(
                  editStage, localX, localY, viewportWidth, viewportHeight,
                  cam.View(), cam.Proj());
              if (towerIdx >= 0)
                sceneEditor.SelectTower(towerIdx);
            }
            if (lbNow) {
              sceneEditor.HandleViewportTilePaint(
                  editStage, localX, localY, viewportWidth, viewportHeight,
                  cam.View(), cam.Proj());
            }
          } else if (lbNow && !prevLButton) {
            // HandleMousePick owns the viewport-to-local conversion so its
            // entity ray stays in sync with SceneEditor::ViewportRect().
            sceneEditor.HandleMousePick(
                editorScene, cursorPos.x, cursorPos.y,
                static_cast<int>(window.Width()),
                static_cast<int>(window.Height()), cam.View(), cam.Proj());
          }
        }

        // Always close a grid paint stroke on release, even when the pointer
        // was released over another editor panel.
        if (sceneEditor.IsGridEditorOpen() && !lbNow && prevLButton)
          sceneEditor.FinalizeViewportPaintStroke(editStage);
      }
      prevLButton = lbNow;

      // Update particle emitters (demo only — not during gameplay).
      if (particlesEnabled && !isPlaying) {
        // Fire emitter follows cursor in world space.
        if (fireEnabled) {
          POINT cursorPos;
          GetCursorPos(&cursorPos);
          ScreenToClient(window.Handle(), &cursorPos);

          int projectionX = cursorPos.x;
          int projectionY = cursorPos.y;
          int projectionWidth = static_cast<int>(window.Width());
          int projectionHeight = static_cast<int>(window.Height());
          bool cursorInProjection =
              cursorPos.x >= 0 &&
              cursorPos.x < static_cast<LONG>(window.Width()) &&
              cursorPos.y >= 0 &&
              cursorPos.y < static_cast<LONG>(window.Height());
          if (appMode == AppMode::Editor) {
            const EditorViewportRect &viewport = sceneEditor.ViewportRect();
            cursorInProjection = viewport.Contains(cursorPos.x, cursorPos.y);
            projectionX = static_cast<int>(cursorPos.x - viewport.x);
            projectionY = static_cast<int>(cursorPos.y - viewport.y);
            projectionWidth = std::max(1, static_cast<int>(viewport.width));
            projectionHeight = std::max(1, static_cast<int>(viewport.height));
          }

          if (cursorInProjection) {
            DirectX::XMVECTOR worldPos = ScreenToWorld(
                projectionX, projectionY, projectionWidth, projectionHeight,
                cam.View(), cam.Proj(), particleDepth);
            fireEmitter.SetPosition(worldPos);
          }
          fireEmitter.Update(static_cast<double>(dt));
        }

        if (smokeEnabled)
          smokeEmitter.Update(static_cast<double>(dt));
        if (sparkEnabled)
          sparkEmitter.Update(static_cast<double>(dt));
      }

      if ((appMode == AppMode::Game || appMode == AppMode::BossArena) &&
          rainEnabled) {
        rainEmitter.Update(static_cast<double>(dt));
      }

      if (playerAnimationSmokeRequested) {
        if (!playerAnimationSmokeFailed &&
            !playerAnimationSmokeSequenceCompleted) {
          const int sequenceIndex =
              playerAnimationSmokeRenderedFrames /
              kPlayerAnimationSmokeFramesPerState;
          if (sequenceIndex < 0 ||
              sequenceIndex >=
                  static_cast<int>(kPlayerAnimationSmokeSequence.size())) {
            failPlayerAnimationSmoke("animation sequence index is invalid");
          } else {
            const PlayerAnimationPreview::ClipSlot expectedClip =
                kPlayerAnimationSmokeSequence[sequenceIndex];
            playerPreview.SelectLocomotionClip(expectedClip);
            playerPreview.Update(1.0f / 60.0f);
            if (playerPreview.ActiveClip() != expectedClip) {
              failPlayerAnimationSmoke(
                  std::string("expected active clip ") +
                  playerClipName(expectedClip) + ", got " +
                  playerClipName(playerPreview.ActiveClip()));
            }
            if (!playerPreview.BonePaletteFinite()) {
              failPlayerAnimationSmoke(
                  std::string(playerClipName(expectedClip)) +
                  " produced a non-finite bone palette");
            }
          }
        } else if (!playerAnimationSmokeFailed &&
                   playerAnimationSmokeActionStarted &&
                   !playerAnimationSmokeActionCompleted) {
          const bool actionWasActive = playerPreview.IsActionPlaying();
          playerPreview.Update(1.0f / 60.0f);
          playerAnimationSmokeActionPoseThisFrame =
              actionWasActive && playerPreview.IsActionPlaying();
          if (!playerPreview.BonePaletteFinite()) {
            failPlayerAnimationSmoke(
                "TakingItem one-shot produced a non-finite bone palette");
          }
          if (playerPreview.ActionTriggerSerial() !=
              playerAnimationSmokeActionSerial) {
            failPlayerAnimationSmoke(
                "TakingItem one-shot retriggered without a pickup event");
          }
        }
      } else if (appMode == AppMode::Tavern && !uiWantsKeyboard &&
                 !gameFreeCameraEnabled) {
        if (tavernManagementSmokeRequested || tavernSuppliesSmokeRequested ||
            tavernUpgradesSmokeRequested ||
            tavernScene.PlayerMovementLocked()) {
          playerPreview.Update(dt);
        } else {
          playerPreview.Update(dt, input, 12.0f,
                               tavernScene.CollisionColliders(),
                               emptyMeshTriangles, false,
                               tavernFirstPersonEnabled ? cam.Yaw() : 0.0f);
        }
        const DirectX::XMFLOAT3 playerPos = playerPreview.Position();
        if (tavernFirstPersonEnabled) {
          gameCameraPosition =
              tavernScene.FirstPersonCameraPosition(playerPos);
        } else {
          const DirectX::XMFLOAT3 cameraFollowOffset =
              tavernScene.CameraFollowOffset();
          const DirectX::XMFLOAT3 targetCameraPos = {
              playerPos.x + cameraFollowOffset.x,
              playerPos.y + cameraFollowOffset.y,
              playerPos.z + cameraFollowOffset.z};
          const float cameraFollowT = std::clamp(dt * 8.5f, 0.0f, 1.0f);
          gameCameraPosition =
              LerpFloat3(gameCameraPosition, targetCameraPos, cameraFollowT);
        }
        cam.SetPosition(gameCameraPosition.x, gameCameraPosition.y,
                        gameCameraPosition.z);
        if (!tavernFirstPersonEnabled)
          cam.SetYawPitch(tavernScene.CameraYaw(), tavernScene.CameraPitch());
      } else if ((appMode == AppMode::Game || appMode == AppMode::BossArena) &&
                 ((!uiWantsKeyboard && !gameFreeCameraEnabled) ||
                  bossMirrorPickupSmokeRequested)) {
        bool inBossArena = appMode == AppMode::BossArena;
        bool enteredTavern = false;
        const float activeGameplayDt =
            bossMirrorPickupSmokeRequested ? 0.75f : dt;
        const bool actionWasActiveBeforeUpdate =
            bossMirrorPickupSmokeRequested && playerPreview.IsActionPlaying();
        const bool bossPhoneActive =
            inBossArena && bossArenaScene.IsPhoneOverlayActive();
        if (bossMirrorPickupSmokeRequested) {
          playerPreview.Update(activeGameplayDt);
        } else if (bossPhoneActive) {
          playerPreview.Update(activeGameplayDt);
        } else {
          playerPreview.Update(
              activeGameplayDt, input,
              inBossArena
                  ? (bossArenaScene.DebugNoClipEnabled()
                         ? 1000.0f
                         : bossArenaScene.ArenaHalfExtent() + 6.0f)
                          : OverworldScene::kFloorSizeMeters * 0.5f,
              inBossArena ? emptyCollisionColliders
                          : overworldCollisionColliders,
              inBossArena ? emptyMeshTriangles
                          : (useModelMeshCollision
                                 ? overworldScene.StageCollisionTriangles()
                                 : emptyMeshTriangles),
              inBossArena && bossArenaScene.DebugNoClipEnabled());
          if (!inBossArena && tavernInteractPressed &&
              overworldScene.IsPlayerNearTavernEntrance(
                  playerPreview.Position())) {
            enterTavernMode();
            enteredTavern = true;
          } else if (!inBossArena &&
                     overworldScene.IsPlayerInsideBossWarp(
                         playerPreview.Position())) {
            TraceAppEvent("overworld warp: boss arena");
            appMode = AppMode::BossArena;
            inBossArena = true;
            bossArenaScene.Reset(playerPreview);
            gameCameraPosition = {0.0f, 4.0f, -20.0f};
          }
        }
        bossMirrorPickupActionPoseThisFrame =
            actionWasActiveBeforeUpdate && playerPreview.IsActionPlaying();
        if (!enteredTavern) {
          if (inBossArena)
            bossArenaScene.Update(activeGameplayDt, input, playerPreview);
          if (inBossArena && bossMirrorPickupSmokeRequested &&
              !bossMirrorPickupSmokeFailed && !bossMirrorPickupSmokeCompleted) {
            ++bossMirrorPickupSmokeFrames;
            const int chargeCount = bossArenaScene.MirrorChargeCount();
            const int collectedCount =
                chargeCount - bossMirrorPickupInitialCharge;
            const uint64_t actionSerial = playerPreview.ActionTriggerSerial();

            if (collectedCount == bossMirrorPickupCollectedCount + 1) {
              bossMirrorPickupCollectedCount = collectedCount;
              bossMirrorPickupObserved = true;
              bossMirrorPickupTriggeredThisFrame = true;
              bossMirrorPickupAwaitingResponse = true;
              bossMirrorPickupExpectedActionSerial =
                  bossMirrorPickupInitialActionSerial +
                  static_cast<uint64_t>(collectedCount);
              if (actionSerial != bossMirrorPickupExpectedActionSerial) {
                failBossMirrorPickupSmoke(
                    "pickup did not trigger exactly one action");
              } else if (!playerPreview.IsActionPlaying()) {
                failBossMirrorPickupSmoke(
                    "TakingItem action was not active after pickup");
              } else {
                std::ostringstream pickupMessage;
                pickupMessage << "Boss mirror pickup smoke: pickup "
                              << collectedCount << " triggered one-shot";
                TraceAppEvent(pickupMessage.str().c_str());
              }

              if (collectedCount < 3 && !bossMirrorPickupSmokeFailed) {
                DirectX::XMFLOAT3 nextChargePosition{};
                if (!bossArenaScene.TryGetActiveMirrorChargePosition(
                        nextChargePosition)) {
                  failBossMirrorPickupSmoke(
                      "next active mirror charge is unavailable");
                } else {
                  nextChargePosition.y = 0.0f;
                  bossMirrorPickupPendingPosition = nextChargePosition;
                  bossMirrorPickupTeleportPending = true;
                }
              }
            } else if (collectedCount != bossMirrorPickupCollectedCount) {
              failBossMirrorPickupSmoke(
                  "mirror charge count changed by more than one");
            } else if (actionSerial != bossMirrorPickupExpectedActionSerial) {
              failBossMirrorPickupSmoke(
                  "TakingItem action retriggered without another pickup");
            }

            if (!playerPreview.BonePaletteFinite()) {
              failBossMirrorPickupSmoke(
                  "TakingItem action produced a non-finite bone palette");
            } else if (bossMirrorPickupCollectedCount == 3 &&
                       !playerPreview.IsActionPlaying()) {
              if (playerPreview.ActiveClip() !=
                  PlayerAnimationPreview::ClipSlot::Idle) {
                failBossMirrorPickupSmoke(
                    "TakingItem action did not return to Idle");
              } else if (bossMirrorPickupRenderedActionFrames <= 0) {
                failBossMirrorPickupSmoke(
                    "TakingItem action completed without a rendered sample");
              } else if (bossMirrorPickupRenderedResponses != 3) {
                failBossMirrorPickupSmoke(
                    "not every pickup produced a rendered action response");
              } else {
                bossMirrorPickupSmokeCompleted = true;
                requestQuit = true;
                TraceAppEvent(
                    "Boss mirror pickup smoke: all pickups returned to Idle");
              }
            } else if (bossMirrorPickupSmokeFrames >= 10) {
              failBossMirrorPickupSmoke(
                  "TakingItem pickup sequence did not finish within 10 "
                  "rendered frames");
            }
          }
          const DirectX::XMFLOAT3 playerPos = playerPreview.Position();
          const DirectX::XMFLOAT3 targetCameraPos = {
              playerPos.x,
              (inBossArena && bossArenaScene.DebugNoClipEnabled())
                  ? playerPos.y + 3.2f
                  : 3.2f,
              playerPos.z - 5.8f};
          const float cameraFollowT = std::clamp(dt * 7.5f, 0.0f, 1.0f);
          gameCameraPosition = LerpFloat3(gameCameraPosition, targetCameraPos,
                                          cameraFollowT);
          DirectX::XMFLOAT3 finalCameraPosition = gameCameraPosition;
          if (inBossArena && !bossPhoneActive) {
            const float shake = bossArenaScene.CameraImpulseAmount();
            if (shake > 0.0001f) {
              finalCameraPosition.x +=
                  std::sin(gameRuntimeSeconds * 78.0f) * shake;
              finalCameraPosition.y +=
                  std::cos(gameRuntimeSeconds * 91.0f) * shake * 0.55f;
            }
          }
          cam.SetPosition(finalCameraPosition.x, finalCameraPosition.y,
                          finalCameraPosition.z);
          cam.SetYawPitch(0.0f, -0.28f);
        }
      } else if (appMode == AppMode::Game || appMode == AppMode::Tavern ||
                 appMode == AppMode::BossArena || appMode == AppMode::Editor) {
        playerPreview.Update(dt);
      }

      // FPS + debug title update.
      fpsTimer += dt;
      fpsFrames += 1;
      if (fpsTimer >= 1.0f) {
        fpsValue = fpsFrames / fpsTimer;
        fpsTimer = 0.0f;
        fpsFrames = 0;

#if defined(_DEBUG)
        auto p = cam.GetPosition();
        std::wstringstream ss;
        ss.setf(std::ios::fixed);
        ss.precision(2);
        ss << windowTitle << L" [Debug] | FPS: " << fpsValue << L" | Cam: ("
           << p.x << L", " << p.y << L", " << p.z << L")";
        window.SetTitle(ss.str());
#else
        window.SetTitle(windowTitle);
#endif
      }

      if (appMode == AppMode::Title) {
        switch (titleScreen.Draw(static_cast<int>(window.Width()),
                                 static_cast<int>(window.Height()))) {
        case TitleScreen::Action::Start:
          TraceAppEvent("title action: start");
          appMode = AppMode::Game;
          gameRuntimeSeconds = 0.0f;
          tavernScene.Reset(gameTimeOfDayHours);
          playerPreview.SetPosition(overworldScene.PlayerSpawnPosition());
          playerPreview.SetYaw(0.0f);
          {
            const DirectX::XMFLOAT3 spawn = playerPreview.Position();
            gameCameraPosition = {spawn.x, 3.2f, spawn.z - 5.8f};
          }
          cam.SetPosition(gameCameraPosition.x, gameCameraPosition.y,
                          gameCameraPosition.z);
          cam.SetYawPitch(0.0f, -0.28f);
          cam.SetLens(DirectX::XM_PIDIV4,
                      static_cast<float>(window.Width()) /
                          static_cast<float>(window.Height()),
                      0.1f, 1000.0f);
          break;
        case TitleScreen::Action::Settings:
          TraceAppEvent("title action: settings");
          showSettings = true;
          break;
        case TitleScreen::Action::Quit:
          TraceAppEvent("title action: quit");
          requestQuit = true;
          break;
        case TitleScreen::Action::Editor:
          TraceAppEvent("title action: editor");
          appMode = AppMode::Editor;
          break;
        case TitleScreen::Action::None:
          break;
        }
      }

      if (appMode == AppMode::Game || appMode == AppMode::Tavern ||
          appMode == AppMode::BossArena) {
        gameRuntimeSeconds += dt;
      }

      // ---- Settings window (Phase 12.6) ----
      if (showSettings) {
        const float vw = static_cast<float>(window.Width());
        const float vh = static_cast<float>(window.Height());
        ImDrawList *draw = ImGui::GetForegroundDrawList();
        draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(vw, vh),
                            UiColor(0.0f, 0.025f, 0.030f, 0.66f));

        const float panelW = std::clamp(vw * 0.34f, 460.0f, 620.0f);
        const float panelH = std::clamp(vh * 0.58f, 470.0f, 620.0f);
        const ImVec2 panelMin(vw - panelW - 54.0f, (vh - panelH) * 0.5f);
        const ImVec2 panelMax(panelMin.x + panelW, panelMin.y + panelH);

        ImGui::SetNextWindowPos(panelMin, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("##GameSettingsOverlay", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoScrollWithMouse |
                         ImGuiWindowFlags_NoScrollbar);

        draw->AddRectFilled(ImVec2(panelMin.x + 12.0f, panelMin.y + 18.0f),
                            ImVec2(panelMax.x + 12.0f, panelMax.y + 18.0f),
                            UiColor(0.0f, 0.0f, 0.0f, 0.28f), 12.0f);
        draw->AddRectFilled(panelMin, panelMax,
                            UiColor(0.012f, 0.020f, 0.024f, 0.96f), 12.0f);
        draw->AddRect(panelMin, panelMax, UiColor(0.38f, 0.92f, 0.82f, 0.70f),
                      12.0f, 0, 2.0f);
        draw->AddText(ImVec2(panelMin.x + 34.0f, panelMin.y + 28.0f),
                      UiColor(0.86f, 1.0f, 0.94f, 0.98f), "SETTINGS");
        draw->AddText(ImVec2(panelMin.x + 34.0f, panelMin.y + 54.0f),
                      UiColor(0.52f, 0.70f, 0.68f, 0.88f),
                      "Display and gameplay options");
        draw->AddLine(ImVec2(panelMin.x + 34.0f, panelMin.y + 86.0f),
                      ImVec2(panelMax.x - 34.0f, panelMin.y + 86.0f),
                      UiColor(0.36f, 1.0f, 0.84f, 0.46f), 1.5f);

        const bool isFS = window.IsFullscreen();
        const uint32_t curW = window.Width();
        const uint32_t curH = window.Height();

        const float optionX = panelMin.x + 34.0f;
        const float optionW = panelW - 68.0f;
        float y = panelMin.y + 112.0f;
        draw->AddText(ImVec2(optionX, y), UiColor(0.72f, 0.95f, 0.88f, 0.96f),
                      "DISPLAY MODE");
        y += 28.0f;
        if (DrawSettingsChoice(draw, "fullscreen", "Fullscreen",
                               "Use the whole display for presentation.",
                               ImVec2(optionX, y),
                               ImVec2(optionX + optionW, y + 64.0f), isFS) &&
            !isFS) {
          window.SetFullscreen(true);
        }
        y += 76.0f;
        if (DrawSettingsChoice(draw, "window1080", "1080p Windowed",
                               "1920 x 1080 capture-friendly window.",
                               ImVec2(optionX, y),
                               ImVec2(optionX + optionW, y + 64.0f),
                               !isFS && curW == 1920 && curH == 1080)) {
          window.SetWindowedResolution(1920, 1080);
        }
        y += 76.0f;
        if (DrawSettingsChoice(draw, "window720", "720p Windowed",
                               "1280 x 720 lightweight test window.",
                               ImVec2(optionX, y),
                               ImVec2(optionX + optionW, y + 64.0f),
                               !isFS && curW == 1280 && curH == 720)) {
          window.SetWindowedResolution(1280, 720);
        }

#if defined(_DEBUG)
        y += 88.0f;
        if (appMode == AppMode::Game || appMode == AppMode::BossArena) {
          draw->AddText(ImVec2(optionX, y), UiColor(0.72f, 0.95f, 0.88f, 0.96f),
                        "CAMERA");
          y += 28.0f;
          if (DrawSettingsChoice(draw, "freecamera", "Free Camera",
                                 "Right mouse + WASD debug camera.",
                                 ImVec2(optionX, y),
                                 ImVec2(optionX + optionW, y + 64.0f),
                                 gameFreeCameraEnabled)) {
            gameFreeCameraEnabled = !gameFreeCameraEnabled;
            if (gameFreeCameraEnabled)
              cam.SetMode(CameraMode::FreeFly);
          }
        }
#endif

        const char *closeText = "ESC / CLICK HERE TO CLOSE";
        const ImVec2 closeSize = ImGui::CalcTextSize(closeText);
        const ImVec2 closeMin(panelMax.x - closeSize.x - 72.0f,
                              panelMax.y - 58.0f);
        const ImVec2 closeMax(panelMax.x - 34.0f, panelMax.y - 24.0f);
        ImGui::SetCursorScreenPos(closeMin);
        if (ImGui::InvisibleButton("##settings-close",
                                   ImVec2(closeMax.x - closeMin.x,
                                          closeMax.y - closeMin.y))) {
          showSettings = false;
        }
        const bool closeHovered = ImGui::IsItemHovered();
        draw->AddRectFilled(closeMin, closeMax,
                            closeHovered
                                ? UiColor(0.12f, 0.25f, 0.24f, 0.92f)
                                : UiColor(0.04f, 0.08f, 0.085f, 0.86f),
                            6.0f);
        draw->AddRect(closeMin, closeMax, UiColor(0.36f, 1.0f, 0.84f, 0.58f),
                      6.0f, 0, 1.2f);
        draw->AddText(ImVec2(closeMin.x + 20.0f, closeMin.y + 9.0f),
                      UiColor(0.80f, 1.0f, 0.94f, 0.96f), closeText);
        ImGui::End();
        ImGui::PopStyleVar();
      }

#if defined(_DEBUG)
      if (appMode == AppMode::Game || appMode == AppMode::BossArena) {
        const DirectX::XMFLOAT3 cameraPos = cam.GetPosition();
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(260.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Camera Position");
        ImGui::Text("X: %.2f", cameraPos.x);
        ImGui::Text("Y: %.2f", cameraPos.y);
        ImGui::Text("Z: %.2f", cameraPos.z);
        ImGui::Separator();
        ImGui::Text("Placement: {%.2ff, %.2ff, %.2ff}",
                    cameraPos.x, cameraPos.y, cameraPos.z);
        ImGui::Text("Objects: %zu", overworldScene.ObjectCount());
        if (appMode == AppMode::BossArena) {
          ImGui::Separator();
          ImGui::Text("Boss Arena");
          bool bossDebugVisible = bossArenaScene.DebugPanelVisible();
          if (ImGui::Checkbox("Boss Debug Panel", &bossDebugVisible))
            bossArenaScene.SetDebugPanelVisible(bossDebugVisible);
          ImGui::TextDisabled("Open or close without F3");
        }
        ImGui::Separator();
        ImGui::Text("Time");
        ImGui::SliderFloat("Hour", &gameTimeOfDayHours, 0.0f, 24.0f, "%.2f");
        ImGui::Checkbox("Auto Time", &gameTimeAuto);
        ImGui::SliderFloat("Hours / sec", &gameHoursPerSecond, 0.01f, 4.0f,
                           "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::Text("Sky Exposure: %.3f", gameSkyExposure);
        ImGui::Text("Post Exposure: %.3f", gamePostExposure);
        ImGui::Text("Sun Dir: %.2f, %.2f, %.2f",
                    gameLighting.sunDirection.x, gameLighting.sunDirection.y,
                    gameLighting.sunDirection.z);
        ImGui::Text("Sun Color: %.2f, %.2f, %.2f", gameLighting.sunColor.x,
                    gameLighting.sunColor.y, gameLighting.sunColor.z);
        ImGui::Text("Sun Intensity: %.2f", gameLighting.sunIntensity);
        ImGui::Text("IBL Intensity: %.2f", kGameIblIntensity);
        ImGui::Separator();
        ImGui::Text("Sumi-e Tonemap Prototype");
        ImGui::SliderFloat("Ink Edge Strength", &inkWashStrength, 0.0f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Ink Flow Strength", &inkFlowStrength, 0.0f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Ink Flow Speed", &inkFlowSpeed, 0.0f, 2.0f,
                           "%.2f");
        if (ImGui::Button("Ink Effects Off")) {
          inkWashStrength = 0.0f;
          inkFlowStrength = 0.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Review Preset")) {
          inkWashStrength = kInkEdgeReviewStrength;
          inkFlowStrength = kInkFlowReviewStrength;
          inkFlowSpeed = kInkFlowReviewSpeed;
        }
        ImGui::TextDisabled("Boss Phase 2 only");
        ImGui::TextDisabled("F7 toggles Off / Review Preset in Phase 2");
        ImGui::TextDisabled("0.00 = 06df2b9 visual baseline");
        if (ImGui::Button("Midnight")) {
          gameTimeOfDayHours = 0.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Afternoon")) {
          gameTimeOfDayHours = 12.0f;
        }
        ImGui::Separator();
        ImGui::Text("Weather");
        ImGui::Checkbox("Rain Enabled", &rainEnabled);
        ImGui::Text("Rain Particles: %zu", rainEmitter.GetCount());
        ImGui::Separator();
        ImGui::Text("Water");
        ImGui::SliderFloat("Wave Height", &waterWaveHeight, 0.0f, 3.0f,
                           "%.2f");
        ImGui::SliderFloat("Wave Speed", &waterWaveSpeed, 0.0f, 3.0f,
                           "%.2f");
        ImGui::SliderFloat("Wave Frequency", &waterWaveFrequency, 0.2f, 3.0f,
                           "%.2f");
        if (ImGui::SliderFloat("Water Transparency", &waterTransparency, 0.0f,
                               0.95f, "%.2f")) {
          overworldScene.SetWaterTransparency(dx, waterTransparency);
        }
        ImGui::SliderFloat("Wet Strength", &wetSurfaceStrength, 0.0f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Wet Dry Seconds", &wetSurfaceDrySeconds, 0.2f,
                           12.0f, "%.2f");
        ImGui::SliderFloat("Wet Impact Radius", &wetSurfaceImpactRadius, 0.5f,
                           8.0f, "%.2f");
        ImGui::SliderFloat("Wet Cycle Seconds", &wetSurfaceCycleSeconds, 0.5f,
                           16.0f, "%.2f");
        ImGui::SliderFloat("Puddle Strength", &puddleStrength, 0.0f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Puddle Build Seconds", &puddleBuildSeconds, 0.0f,
                           20.0f, "%.2f");
        ImGui::SliderFloat("Puddle Radius", &puddleRadius, 0.5f, 8.0f,
                           "%.2f");
        ImGui::SliderFloat("Puddle Clarity", &puddleClarity, 0.0f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Puddle Tint", &puddleTint, 0.0f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Puddle Ripple", &puddleRippleStrength, 0.0f, 2.0f,
                           "%.2f");
        if (ImGui::Button("Reset Water")) {
          waterWaveHeight = 1.0f;
          waterWaveSpeed = 1.0f;
          waterWaveFrequency = 1.0f;
          waterTransparency = 0.45f;
          overworldScene.SetWaterTransparency(dx, waterTransparency);
          wetSurfaceStrength = 0.9f;
          wetSurfaceDrySeconds = 4.0f;
          wetSurfaceImpactRadius = 4.5f;
          wetSurfaceCycleSeconds = 5.5f;
          puddleStrength = 1.0f;
          puddleBuildSeconds = 0.0f;
          puddleRadius = 3.2f;
          puddleClarity = 0.90f;
          puddleTint = 0.28f;
          puddleRippleStrength = 0.35f;
        }
        ImGui::Separator();
        ImGui::Text("Forest Debug");
        auto &forestDebug = overworldScene.BackgroundForestDebug();
        ImGui::Checkbox("Forest Enabled", &forestDebug.enabled);
        ImGui::Checkbox("Single Cluster Preview",
                        &forestDebug.singleClusterPreview);
        ImGui::SliderInt("Forest Density", &forestDebug.densityLevel, 0, 3);
        ImGui::SliderFloat("Forest Scale", &forestDebug.scaleMultiplier, 0.02f,
                           0.60f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Forest Distance", &forestDebug.distanceOffset,
                           -10.0f, 60.0f, "%.1f");
        ImGui::SliderFloat("Forest Spacing", &forestDebug.spacingMultiplier,
                           0.5f, 1.8f, "%.2f");
        if (ImGui::Button("Reset Forest")) {
          forestDebug.enabled = true;
          forestDebug.singleClusterPreview = false;
          forestDebug.densityLevel = 3;
          forestDebug.scaleMultiplier = 0.40f;
          forestDebug.distanceOffset = 32.0f;
          forestDebug.spacingMultiplier = 0.5f;
        }
        ImGui::Text("Clusters: %zu  Mesh parts: %zu",
                    overworldScene.BackgroundForestClusterCount(),
                    overworldScene.BackgroundForestMeshPartCount());
        ImGui::End();
      }
      if (appMode == AppMode::Tavern && showTavernDebug)
        tavernScene.DrawDebugPanel(gameTimeOfDayHours, gameTimeAuto);
#endif

      // ---- ImGui debug windows ----
      if (appMode == AppMode::BossArena) {
        const BossArenaScene::RestartDestination restartDestination =
            bossArenaScene.DrawHud(static_cast<int>(window.Width()),
                                   static_cast<int>(window.Height()));
        if (restartDestination != BossArenaScene::RestartDestination::None) {
          bossArenaScene.Reset(playerPreview);
          gameRuntimeSeconds = 0.0f;

          if (restartDestination ==
              BossArenaScene::RestartDestination::Overworld) {
            TraceAppEvent("restart choice: overworld");
            appMode = AppMode::Game;
            playerPreview.SetPosition(overworldScene.PlayerSpawnPosition());
          } else {
            TraceAppEvent("restart choice: boss arena");
            appMode = AppMode::BossArena;
          }

          playerPreview.SetYaw(0.0f);
          const DirectX::XMFLOAT3 restartPosition = playerPreview.Position();
          gameCameraPosition = {restartPosition.x, 3.2f,
                                restartPosition.z - 5.8f};
          cam.SetPosition(gameCameraPosition.x, gameCameraPosition.y,
                          gameCameraPosition.z);
          cam.SetYawPitch(0.0f, -0.28f);
        }
      }

      if (appMode == AppMode::Game && !showSettings &&
          overworldScene.IsPlayerNearTavernEntrance(playerPreview.Position())) {
        DrawTavernEntrancePrompt(static_cast<int>(window.Width()),
                                 static_cast<int>(window.Height()));
      }

      if (appMode == AppMode::Tavern) {
        const bool automateTavern =
            tavernGameplaySmokeRequested || tavernDaySmokeRequested ||
            (tavernSuppliesSmokeRequested &&
             (tavernSuppliesSmokePhase ==
                  TavernSuppliesSmokePhase::AutomateToCancelablePour ||
              tavernSuppliesSmokePhase ==
                  TavernSuppliesSmokePhase::AutomateOneService)) ||
            (tavernUpgradesSmokeRequested &&
             tavernUpgradesSmokePhase ==
                 TavernUpgradesSmokePhase::AutomateTable3Cycle);
        const bool tavernInputSmokeRequested = tavernManagementSmokeRequested ||
                                               tavernSuppliesSmokeRequested ||
                                               tavernUpgradesSmokeRequested;
        bool effectiveTavernInteractPressed =
            tavernInputSmokeRequested ? false : tavernInteractPressed;
        bool effectiveTavernPrimaryActionDown =
            tavernInputSmokeRequested ? false : lbNow;
        bool effectiveTavernRestartPressed =
            tavernInputSmokeRequested ? false : tavernRestartPressed;
        TavernScene::ManagementInput effectiveManagementInput =
            tavernInputSmokeRequested ? TavernScene::ManagementInput{}
                                      : tavernManagementInput;
        float tavernUpdateDelta = dt * (automateTavern ? 12.0f : 1.0f);
        if (tavernManagementSmokeRequested && !tavernManagementSmokeFailed &&
            !tavernManagementSmokeCompleted) {
          tavernUpdateDelta = 0.25f;
          ++tavernManagementSmokeUpdateFrames;
          if (tavernManagementSmokePhase == 0 &&
              tavernManagementSmokeUpdateFrames >= 2) {
            effectiveTavernInteractPressed = true;
            tavernManagementSmokePhase = 1;
          } else if (tavernManagementSmokePhase == 2) {
            effectiveManagementInput.nextPressed = true;
            tavernManagementSmokePhase = 3;
          } else if (tavernManagementSmokePhase == 4) {
            effectiveManagementInput.confirmPressed = true;
            tavernManagementSmokePhase = 5;
          } else if (tavernManagementSmokePhase == 6) {
            effectiveManagementInput.cancelPressed = true;
            tavernManagementSmokePhase = 7;
          } else if (tavernManagementSmokePhase == 8) {
            effectiveManagementInput.cancelPressed = true;
            tavernManagementSmokePhase = 9;
          } else if (tavernManagementSmokePhase == 10) {
            effectiveTavernInteractPressed = true;
            tavernManagementSmokePhase = 11;
          }
        }
        if (tavernSuppliesSmokeRequested && !tavernSuppliesSmokeFailed &&
            !tavernSuppliesSmokeCompleted) {
          tavernUpdateDelta = 0.25f;
          ++tavernSuppliesSmokeUpdateFrames;
          switch (tavernSuppliesSmokePhase) {
          case TavernSuppliesSmokePhase::OpenRoot:
            if (tavernSuppliesSmokeUpdateFrames >= 2) {
              effectiveTavernInteractPressed = true;
              tavernSuppliesSmokePhase = TavernSuppliesSmokePhase::VerifyRoot;
            }
            break;
          case TavernSuppliesSmokePhase::EnterSupplies:
          case TavernSuppliesSmokePhase::ReenterSupplies:
            effectiveManagementInput.confirmPressed = true;
            tavernSuppliesSmokePhase =
                tavernSuppliesSmokePhase ==
                        TavernSuppliesSmokePhase::EnterSupplies
                    ? TavernSuppliesSmokePhase::VerifyInitialFull
                    : TavernSuppliesSmokePhase::VerifyStockAfterService;
            break;
          case TavernSuppliesSmokePhase::AttemptFullOrder:
            effectiveManagementInput.nextPressed = true;
            effectiveManagementInput.confirmPressed = true;
            tavernSuppliesSmokePhase = TavernSuppliesSmokePhase::VerifyFullNoOp;
            break;
          case TavernSuppliesSmokePhase::BackToRoot:
            effectiveManagementInput.cancelPressed = true;
            tavernSuppliesSmokePhase =
                TavernSuppliesSmokePhase::VerifyBackToRoot;
            break;
          case TavernSuppliesSmokePhase::CloseRoot:
            effectiveManagementInput.cancelPressed = true;
            tavernSuppliesSmokePhase = TavernSuppliesSmokePhase::VerifyClosed;
            break;
          case TavernSuppliesSmokePhase::ReopenRoot:
            effectiveTavernInteractPressed = true;
            tavernSuppliesSmokePhase =
                TavernSuppliesSmokePhase::VerifyReopenedRoot;
            break;
          case TavernSuppliesSmokePhase::CancelPour:
            effectiveTavernInteractPressed = true;
            tavernSuppliesSmokePhase =
                TavernSuppliesSmokePhase::VerifyCancelledPour;
            break;
          case TavernSuppliesSmokePhase::SelectOneAle:
            effectiveManagementInput.nextPressed = true;
            tavernSuppliesSmokePhase =
                TavernSuppliesSmokePhase::VerifyOneAleSelected;
            break;
          case TavernSuppliesSmokePhase::PurchaseOneAle:
            effectiveManagementInput.confirmPressed = true;
            tavernSuppliesSmokePhase = TavernSuppliesSmokePhase::VerifyPurchase;
            break;
          case TavernSuppliesSmokePhase::SelectUnaffordableAle:
            effectiveManagementInput.nextPressed = true;
            tavernSuppliesSmokePhase =
                TavernSuppliesSmokePhase::VerifyUnaffordableSelection;
            break;
          case TavernSuppliesSmokePhase::AttemptUnaffordableOrder:
            effectiveManagementInput.confirmPressed = true;
            tavernSuppliesSmokePhase =
                TavernSuppliesSmokePhase::VerifyUnaffordableNoOp;
            break;
          default:
            break;
          }
        }
        if (tavernUpgradesSmokeRequested && !tavernUpgradesSmokeFailed &&
            !tavernUpgradesSmokeCompleted) {
          tavernUpdateDelta = 0.25f;
          ++tavernUpgradesSmokeUpdateFrames;
          switch (tavernUpgradesSmokePhase) {
          case TavernUpgradesSmokePhase::OpenRoot:
            if (tavernUpgradesSmokeUpdateFrames >= 2) {
              effectiveTavernInteractPressed = true;
              tavernUpgradesSmokePhase = TavernUpgradesSmokePhase::VerifyRoot;
            }
            break;
          case TavernUpgradesSmokePhase::SelectUpgrades:
            effectiveManagementInput.nextPressed = true;
            tavernUpgradesSmokePhase =
                TavernUpgradesSmokePhase::VerifyUpgradesSelected;
            break;
          case TavernUpgradesSmokePhase::EnterUpgrades:
            effectiveManagementInput.confirmPressed = true;
            tavernUpgradesSmokePhase =
                TavernUpgradesSmokePhase::VerifyInitialPage;
            break;
          case TavernUpgradesSmokePhase::AttemptEmergencyReservePurchase:
            effectiveManagementInput.confirmPressed = true;
            tavernUpgradesSmokePhase =
                TavernUpgradesSmokePhase::VerifyEmergencyReserveNoOp;
            break;
          case TavernUpgradesSmokePhase::OpenExtraMugConfirmation:
            effectiveManagementInput.confirmPressed = true;
            tavernUpgradesSmokePhase =
                TavernUpgradesSmokePhase::VerifyExtraMugConfirmation;
            break;
          case TavernUpgradesSmokePhase::CancelExtraMugConfirmation:
            effectiveManagementInput.cancelPressed = true;
            tavernUpgradesSmokePhase =
                TavernUpgradesSmokePhase::VerifyExtraMugCancellation;
            break;
          case TavernUpgradesSmokePhase::ReopenExtraMugConfirmation:
            effectiveManagementInput.confirmPressed = true;
            tavernUpgradesSmokePhase =
                TavernUpgradesSmokePhase::VerifyReopenedExtraMugConfirmation;
            break;
          case TavernUpgradesSmokePhase::PurchaseExtraMug:
            effectiveManagementInput.confirmPressed = true;
            tavernUpgradesSmokePhase =
                TavernUpgradesSmokePhase::VerifyExtraMugPurchase;
            break;
          case TavernUpgradesSmokePhase::AttemptOwnedExtraMug:
            effectiveManagementInput.confirmPressed = true;
            tavernUpgradesSmokePhase =
                TavernUpgradesSmokePhase::VerifyOwnedExtraMugNoOp;
            break;
          case TavernUpgradesSmokePhase::SelectAleCapacity:
            effectiveManagementInput.nextPressed = true;
            tavernUpgradesSmokePhase =
                TavernUpgradesSmokePhase::VerifyAleCapacitySelection;
            break;
          case TavernUpgradesSmokePhase::OpenAleCapacityConfirmation:
            effectiveManagementInput.confirmPressed = true;
            tavernUpgradesSmokePhase =
                TavernUpgradesSmokePhase::VerifyAleCapacityConfirmation;
            break;
          case TavernUpgradesSmokePhase::PurchaseAleCapacity:
            effectiveManagementInput.confirmPressed = true;
            tavernUpgradesSmokePhase =
                TavernUpgradesSmokePhase::VerifyAleCapacityPurchase;
            break;
          case TavernUpgradesSmokePhase::AttemptMaxedAleCapacity:
            effectiveManagementInput.confirmPressed = true;
            tavernUpgradesSmokePhase =
                TavernUpgradesSmokePhase::VerifyMaxedAleCapacityNoOp;
            break;
          case TavernUpgradesSmokePhase::SelectTable3:
            effectiveManagementInput.nextPressed = true;
            tavernUpgradesSmokePhase =
                TavernUpgradesSmokePhase::VerifyTable3Selection;
            break;
          case TavernUpgradesSmokePhase::OpenTable3Confirmation:
            effectiveManagementInput.confirmPressed = true;
            tavernUpgradesSmokePhase =
                TavernUpgradesSmokePhase::VerifyTable3Confirmation;
            break;
          case TavernUpgradesSmokePhase::PurchaseTable3:
            effectiveManagementInput.confirmPressed = true;
            tavernUpgradesSmokePhase =
                TavernUpgradesSmokePhase::VerifyTable3Purchase;
            break;
          case TavernUpgradesSmokePhase::AttemptOwnedTable3:
            effectiveManagementInput.confirmPressed = true;
            tavernUpgradesSmokePhase =
                TavernUpgradesSmokePhase::VerifyOwnedTable3NoOp;
            break;
          case TavernUpgradesSmokePhase::BackToRoot:
            effectiveManagementInput.cancelPressed = true;
            tavernUpgradesSmokePhase =
                TavernUpgradesSmokePhase::VerifyBackToRoot;
            break;
          case TavernUpgradesSmokePhase::CloseRoot:
            effectiveManagementInput.cancelPressed = true;
            tavernUpgradesSmokePhase = TavernUpgradesSmokePhase::VerifyClosed;
            break;
          default:
            break;
          }
        }
        tavernScene.SetBusinessHour(gameTimeOfDayHours);
        const TavernScene::Action updateAction = tavernScene.Update(
            tavernUpdateDelta, playerPreview.Position(),
            effectiveTavernInteractPressed, effectiveTavernPrimaryActionDown,
            effectiveTavernRestartPressed, effectiveManagementInput,
            automateTavern);
        const TavernScene::Action hudAction =
            tavernScene.DrawHud(static_cast<int>(window.Width()),
                                static_cast<int>(window.Height()),
                                tavernFirstPersonEnabled);
        if (tavernManagementSmokeRequested && !tavernManagementSmokeFailed &&
            !tavernManagementSmokeCompleted) {
          const TavernScene::ManagementUiDiagnostics &diagnostics =
              tavernScene.GetManagementUiDiagnostics();
          if (tavernManagementSmokePhase == 1) {
            if (!tavernScene.ManagementMenuOpen() || !diagnostics.menuOpen ||
                !diagnostics.rootRendered) {
              failTavernManagementSmoke(
                  "production E interaction did not open the root menu");
            } else if (!tavernScene.PlayerMovementLocked()) {
              failTavernManagementSmoke(
                  "player movement remained unlocked while the menu was open");
            } else if (!diagnostics.suppliesCardRendered ||
                       !diagnostics.upgradesCardRendered) {
              failTavernManagementSmoke(
                  "the root menu did not submit both cards");
            } else if (!diagnostics.labelsInRequestedOrder) {
              failTavernManagementSmoke(
                  "Supplies and Upgrades were not submitted in that order");
            } else if (!diagnostics.visualRegionsValid ||
                       !diagnostics.cardsDoNotOverlap) {
              failTavernManagementSmoke(
                  "card visual regions were invalid or overlapping");
            } else if (diagnostics.subpageOpen) {
              failTavernManagementSmoke(
                  "a deferred management subpage opened unexpectedly");
            } else if (diagnostics.selectedCardIndex != 0) {
              failTavernManagementSmoke(
                  "Supplies was not the default keyboard/gamepad selection");
            } else if (tavernScene.Gold() != tavernManagementBaselineGold ||
                       tavernScene.CompletedCycles() !=
                           tavernManagementBaselineCompletedCycles) {
              failTavernManagementSmoke(
                  "opening the root menu changed the Tavern economy");
            } else {
              TraceAppEvent("Tavern management smoke: root cards rendered");
              tavernManagementSmokePhase = 2;
            }
          } else if (tavernManagementSmokePhase == 3) {
            if (!tavernScene.ManagementMenuOpen() || !diagnostics.menuOpen) {
              failTavernManagementSmoke(
                  "card navigation unexpectedly closed the root menu");
            } else if (diagnostics.selectedCardIndex != 1) {
              failTavernManagementSmoke(
                  "right navigation did not select the Upgrades card");
            } else if (diagnostics.subpageOpen ||
                       diagnostics.activationSerial != 0) {
              failTavernManagementSmoke(
                  "navigation opened Upgrades without confirmation");
            } else if (tavernScene.Gold() != tavernManagementBaselineGold ||
                       tavernScene.CompletedCycles() !=
                           tavernManagementBaselineCompletedCycles) {
              failTavernManagementSmoke(
                  "card navigation changed the Tavern economy");
            } else {
              TraceAppEvent("Tavern management smoke: Upgrades card selected");
              tavernManagementSmokePhase = 4;
            }
          } else if (tavernManagementSmokePhase == 5) {
            if (!tavernScene.ManagementMenuOpen() || !diagnostics.menuOpen ||
                !diagnostics.subpageOpen || !diagnostics.upgradesPageRendered ||
                diagnostics.selectedCardIndex != 1) {
              failTavernManagementSmoke(
                  "confirm did not open the real Upgrades page");
            } else if (diagnostics.activationSerial == 0) {
              failTavernManagementSmoke(
                  "confirm input did not activate the selected card");
            } else if (!diagnostics.upgradeCardsRendered ||
                       !diagnostics.backControlRendered ||
                       !diagnostics.visualRegionsValid ||
                       !diagnostics.cardsDoNotOverlap) {
              failTavernManagementSmoke(
                  "the Upgrades page did not submit three valid cards");
            } else if (diagnostics.upgradeConfirmationRendered) {
              failTavernManagementSmoke(
                  "opening Upgrades unexpectedly opened a confirmation");
            } else if (tavernScene.Gold() != tavernManagementBaselineGold ||
                       tavernScene.CompletedCycles() !=
                           tavernManagementBaselineCompletedCycles) {
              failTavernManagementSmoke(
                  "card confirmation changed the Tavern economy");
            } else {
              TraceAppEvent("Tavern management smoke: Upgrades page rendered");
              tavernManagementSmokePhase = 6;
            }
          } else if (tavernManagementSmokePhase == 7) {
            if (!tavernScene.ManagementMenuOpen() || !diagnostics.menuOpen ||
                !diagnostics.rootRendered || diagnostics.subpageOpen) {
              failTavernManagementSmoke(
                  "the first B did not return Upgrades to the root menu");
            } else if (!tavernScene.PlayerMovementLocked()) {
              failTavernManagementSmoke(
                  "player movement unlocked before the root menu closed");
            } else if (diagnostics.selectedCardIndex != 1) {
              failTavernManagementSmoke(
                  "returning to the root did not preserve Upgrades selection");
            } else if (tavernScene.Gold() != tavernManagementBaselineGold ||
                       tavernScene.CompletedCycles() !=
                           tavernManagementBaselineCompletedCycles) {
              failTavernManagementSmoke(
                  "returning from Upgrades changed the Tavern economy");
            } else {
              TraceAppEvent(
                  "Tavern management smoke: B returned Upgrades to root");
              tavernManagementSmokePhase = 8;
            }
          } else if (tavernManagementSmokePhase == 9) {
            if (tavernScene.ManagementMenuOpen() || diagnostics.menuOpen) {
              failTavernManagementSmoke("the second B left the root menu open");
            } else if (tavernScene.PlayerMovementLocked()) {
              failTavernManagementSmoke(
                  "player movement remained locked after closing the root");
            } else if (tavernScene.Gold() != tavernManagementBaselineGold ||
                       tavernScene.CompletedCycles() !=
                           tavernManagementBaselineCompletedCycles) {
              failTavernManagementSmoke(
                  "closing the root menu changed the Tavern economy");
            } else {
              const DirectX::XMFLOAT3 managementPosition =
                  tavernScene.ManagementInteractionPosition();
              playerPreview.SetPosition({managementPosition.x + 0.89f,
                                         managementPosition.y,
                                         managementPosition.z + 1.10f});
              tavernManagementSmokePhase = 10;
            }
          } else if (tavernManagementSmokePhase == 11) {
            if (tavernScene.ManagementMenuOpen() || diagnostics.menuOpen) {
              failTavernManagementSmoke(
                  "management stole interaction from the nearer TABLE 1");
            } else if (tavernScene.PlayerMovementLocked()) {
              failTavernManagementSmoke(
                  "TABLE 1 priority probe unexpectedly locked movement");
            } else if (tavernScene.Gold() != tavernManagementBaselineGold ||
                       tavernScene.CompletedCycles() !=
                           tavernManagementBaselineCompletedCycles) {
              failTavernManagementSmoke(
                  "TABLE 1 priority probe changed the Tavern economy");
            } else {
              TraceAppEvent(
                  "Tavern management smoke: nearer TABLE 1 kept priority");
              tavernManagementSmokeCompleted = true;
              requestQuit = true;
            }
          }
        }
        if (tavernSuppliesSmokeRequested && !tavernSuppliesSmokeFailed &&
            !tavernSuppliesSmokeCompleted) {
          const TavernScene::ManagementUiDiagnostics &diagnostics =
              tavernScene.GetManagementUiDiagnostics();
          switch (tavernSuppliesSmokePhase) {
          case TavernSuppliesSmokePhase::VerifyRoot:
            if (!tavernScene.ManagementMenuOpen() || !diagnostics.menuOpen ||
                !diagnostics.rootRendered ||
                diagnostics.selectedCardIndex != 0) {
              failTavernSuppliesSmoke(
                  "production interaction did not open Supplies by default");
            } else {
              tavernSuppliesSmokePhase =
                  TavernSuppliesSmokePhase::EnterSupplies;
            }
            break;
          case TavernSuppliesSmokePhase::VerifyInitialFull:
            if (!diagnostics.suppliesPageRendered ||
                !diagnostics.aleCardRendered ||
                !diagnostics.backControlRendered || diagnostics.aleStock != 6 ||
                diagnostics.aleCapacity != 6 ||
                diagnostics.orderQuantity != 0 ||
                diagnostics.orderUnitPrice != 2 ||
                diagnostics.orderTotal != 0 || diagnostics.orderCanPurchase ||
                diagnostics.orderBlockReason !=
                    TavernScene::SupplyOrderBlockReason::Full) {
              failTavernSuppliesSmoke(
                  "initial 6/6 Ale card did not expose the full guard");
            } else {
              TraceAppEvent(
                  "Tavern supplies smoke: initial full guard rendered");
              tavernSuppliesSmokePhase =
                  TavernSuppliesSmokePhase::AttemptFullOrder;
            }
            break;
          case TavernSuppliesSmokePhase::VerifyFullNoOp:
            if (tavernScene.AleStock() != 6 || tavernScene.Gold() != 0 ||
                tavernScene.PendingAleOrderQuantity() != 0 ||
                tavernScene.SupplyPurchaseSerial() !=
                    tavernSuppliesPurchaseSerialBefore ||
                diagnostics.orderCanPurchase ||
                diagnostics.orderBlockReason !=
                    TavernScene::SupplyOrderBlockReason::Full) {
              failTavernSuppliesSmoke(
                  "ordering while full changed stock, Gold, or purchase state");
            } else {
              tavernSuppliesSmokePhase = TavernSuppliesSmokePhase::BackToRoot;
            }
            break;
          case TavernSuppliesSmokePhase::VerifyBackToRoot:
            if (!tavernScene.ManagementMenuOpen() || !diagnostics.menuOpen ||
                !diagnostics.rootRendered || diagnostics.subpageOpen) {
              failTavernSuppliesSmoke(
                  "B did not return from Order Supplies to the root menu");
            } else {
              tavernSuppliesSmokePhase = TavernSuppliesSmokePhase::CloseRoot;
            }
            break;
          case TavernSuppliesSmokePhase::VerifyClosed:
            if (tavernScene.ManagementMenuOpen() || diagnostics.menuOpen ||
                tavernScene.PlayerMovementLocked()) {
              failTavernSuppliesSmoke(
                  "B did not close the root menu and unlock movement");
            } else if (tavernScene.AleStock() != 6 || tavernScene.Gold() != 0) {
              failTavernSuppliesSmoke(
                  "back/close navigation changed the Tavern economy");
            } else {
              tavernSuppliesSmokePhase =
                  TavernSuppliesSmokePhase::AutomateToCancelablePour;
            }
            break;
          case TavernSuppliesSmokePhase::AutomateToCancelablePour:
            if (tavernScene.AleStock() != 6 ||
                tavernScene.ServedCustomers() != 0) {
              failTavernSuppliesSmoke(
                  "pre-cancel automation changed Ale or served a customer");
            } else if (tavernScene.AlePourInProgress()) {
              tavernSuppliesSmokePhase = TavernSuppliesSmokePhase::CancelPour;
            }
            break;
          case TavernSuppliesSmokePhase::VerifyCancelledPour:
            if (tavernScene.AlePourInProgress() ||
                tavernScene.AleStock() != 6 ||
                tavernScene.ServedCustomers() != 0) {
              failTavernSuppliesSmoke(
                  "E cancellation consumed Ale or left pouring active");
            } else {
              TraceAppEvent(
                  "Tavern supplies smoke: cancelled pour consumed no Ale");
              tavernSuppliesSmokePhase =
                  TavernSuppliesSmokePhase::AutomateOneService;
            }
            break;
          case TavernSuppliesSmokePhase::AutomateOneService:
            if (tavernScene.ServedCustomers() > 1 ||
                tavernScene.AleStock() < 5) {
              failTavernSuppliesSmoke(
                  "automation consumed more than one Ale serving");
            } else if (tavernScene.ServedCustomers() == 1) {
              if (tavernScene.AleStock() != 5 || tavernScene.Gold() <= 0 ||
                  tavernScene.SupplyPurchaseSerial() !=
                      tavernSuppliesPurchaseSerialBefore) {
                failTavernSuppliesSmoke(
                    "one completed pour/serve did not consume exactly one Ale");
              } else {
                tavernSuppliesPostServeGold = tavernScene.Gold();
                const DirectX::XMFLOAT3 managementPosition =
                    tavernScene.ManagementInteractionPosition();
                playerPreview.SetPosition(managementPosition);
                TraceAppEvent(
                    "Tavern supplies smoke: one service consumed ALE 6>5");
                tavernSuppliesSmokePhase = TavernSuppliesSmokePhase::ReopenRoot;
              }
            }
            break;
          case TavernSuppliesSmokePhase::VerifyReopenedRoot:
            if (!tavernScene.ManagementMenuOpen() ||
                !diagnostics.rootRendered ||
                diagnostics.selectedCardIndex != 0) {
              failTavernSuppliesSmoke(
                  "management root did not reopen after real Ale service");
            } else {
              tavernSuppliesSmokePhase =
                  TavernSuppliesSmokePhase::ReenterSupplies;
            }
            break;
          case TavernSuppliesSmokePhase::VerifyStockAfterService:
            if (!diagnostics.suppliesPageRendered ||
                diagnostics.aleStock != 5 || diagnostics.aleCapacity != 6 ||
                diagnostics.orderQuantity != 0 ||
                diagnostics.orderBlockReason !=
                    TavernScene::SupplyOrderBlockReason::ZeroQuantity) {
              failTavernSuppliesSmoke(
                  "Order Supplies did not show ALE 5/6 after service");
            } else {
              tavernSuppliesSmokePhase = TavernSuppliesSmokePhase::SelectOneAle;
            }
            break;
          case TavernSuppliesSmokePhase::VerifyOneAleSelected:
            if (diagnostics.orderQuantity != 1 ||
                diagnostics.orderUnitPrice != 2 ||
                diagnostics.orderTotal != 2 ||
                diagnostics.aleStockAfterOrder != 6 ||
                !diagnostics.orderCanPurchase ||
                diagnostics.orderBlockReason !=
                    TavernScene::SupplyOrderBlockReason::None) {
              failTavernSuppliesSmoke(
                  "one-Ale selection did not preview an atomic 2 G order");
            } else {
              tavernSuppliesSmokePhase =
                  TavernSuppliesSmokePhase::PurchaseOneAle;
            }
            break;
          case TavernSuppliesSmokePhase::VerifyPurchase:
            if (tavernScene.AleStock() != 6 ||
                tavernScene.Gold() != tavernSuppliesPostServeGold - 2 ||
                tavernScene.PendingAleOrderQuantity() != 0 ||
                tavernScene.SupplyPurchaseSerial() !=
                    tavernSuppliesPurchaseSerialBefore + 1) {
              failTavernSuppliesSmoke(
                  "one-Ale order was not committed atomically for 2 G");
            } else {
              tavernSuppliesPurchaseSerialAfter =
                  tavernScene.SupplyPurchaseSerial();
              tavernScene.ConfigureSuppliesSmokeState(5, 1);
              TraceAppEvent("Tavern supplies smoke: ALE 5>6 purchased for 2 G");
              tavernSuppliesSmokePhase =
                  TavernSuppliesSmokePhase::SelectUnaffordableAle;
            }
            break;
          case TavernSuppliesSmokePhase::VerifyUnaffordableSelection:
            if (diagnostics.aleStock != 5 || diagnostics.orderQuantity != 1 ||
                diagnostics.orderTotal != 2 || diagnostics.orderCanPurchase ||
                diagnostics.orderBlockReason !=
                    TavernScene::SupplyOrderBlockReason::InsufficientGold) {
              failTavernSuppliesSmoke(
                  "1 G did not disable the selected 2 G Ale order");
            } else {
              tavernSuppliesSmokePhase =
                  TavernSuppliesSmokePhase::AttemptUnaffordableOrder;
            }
            break;
          case TavernSuppliesSmokePhase::VerifyUnaffordableNoOp:
            if (tavernScene.AleStock() != 5 || tavernScene.Gold() != 1 ||
                tavernScene.PendingAleOrderQuantity() != 1 ||
                tavernScene.SupplyPurchaseSerial() !=
                    tavernSuppliesPurchaseSerialAfter) {
              failTavernSuppliesSmoke(
                  "insufficient-Gold confirmation changed economy state");
            } else {
              tavernScene.ConfigureSuppliesSmokeState(0, 1);
              tavernScene.BeginNextDay(5.0f);
              if (tavernScene.AleStock() != 2 || tavernScene.Gold() != 1) {
                failTavernSuppliesSmoke(
                    "next day did not restore the 2-Ale recovery floor");
                break;
              }
              tavernScene.ConfigureSuppliesSmokeState(0, 2);
              tavernScene.BeginNextDay(5.0f);
              if (tavernScene.AleStock() != 0 || tavernScene.Gold() != 2) {
                failTavernSuppliesSmoke(
                    "recovery floor incorrectly triggered with affordable Ale");
                break;
              }
              TraceAppEvent("Tavern supplies smoke: insufficient and recovery "
                            "guards passed");
              tavernSuppliesSmokeCompleted = true;
              requestQuit = true;
            }
            break;
          default:
            break;
          }
        }
        if (tavernUpgradesSmokeRequested && !tavernUpgradesSmokeFailed &&
            !tavernUpgradesSmokeCompleted) {
          const TavernScene::ManagementUiDiagnostics &diagnostics =
              tavernScene.GetManagementUiDiagnostics();
          switch (tavernUpgradesSmokePhase) {
          case TavernUpgradesSmokePhase::VerifyRoot:
            if (!tavernScene.ManagementMenuOpen() || !diagnostics.menuOpen ||
                !diagnostics.rootRendered || diagnostics.subpageOpen ||
                diagnostics.selectedCardIndex != 0) {
              failTavernUpgradesSmoke("production interaction did not open the "
                                      "management root on Supplies");
            } else if (tavernScene.Gold() != 200 ||
                       tavernScene.UpgradePurchaseSerial() !=
                           tavernUpgradesPurchaseSerialBefore) {
              failTavernUpgradesSmoke(
                  "opening the management root changed progression state");
            } else {
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::SelectUpgrades;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyUpgradesSelected:
            if (!diagnostics.rootRendered || diagnostics.subpageOpen ||
                diagnostics.selectedCardIndex != 1 ||
                diagnostics.activationSerial != 0) {
              failTavernUpgradesSmoke("right navigation did not select "
                                      "Upgrades without activating it");
            } else if (tavernScene.Gold() != 200 ||
                       tavernScene.UpgradePurchaseSerial() !=
                           tavernUpgradesPurchaseSerialBefore) {
              failTavernUpgradesSmoke(
                  "selecting Upgrades changed progression state");
            } else {
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::EnterUpgrades;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyInitialPage:
            if (!diagnostics.menuOpen || !diagnostics.subpageOpen ||
                !diagnostics.upgradesPageRendered ||
                !diagnostics.upgradeCardsRendered ||
                !diagnostics.backControlRendered ||
                !diagnostics.visualRegionsValid ||
                !diagnostics.cardsDoNotOverlap ||
                diagnostics.upgradeConfirmationRendered) {
              failTavernUpgradesSmoke(
                  "the Upgrades page did not render three valid cards");
            } else if (diagnostics.selectedUpgradeIndex != 0 ||
                       diagnostics.selectedUpgradePrice != 25 ||
                       diagnostics.selectedUpgradeLevel != 0 ||
                       diagnostics.extraMugCapacity != 2 ||
                       diagnostics.aleCapacityLevel != 0 ||
                       diagnostics.table3Unlocked ||
                       !diagnostics.upgradeCanPurchase ||
                       diagnostics.upgradeBlockReason !=
                           TavernScene::UpgradePurchaseBlockReason::None) {
              failTavernUpgradesSmoke("initial Extra Mug card was not a "
                                      "purchasable 25 G 2-to-3 upgrade");
            } else if (diagnostics.upgradePurchaseSerial !=
                           tavernUpgradesPurchaseSerialBefore ||
                       tavernScene.Gold() != 200) {
              failTavernUpgradesSmoke(
                  "opening Upgrades changed Gold or purchase serial");
            } else {
              TraceAppEvent("Tavern upgrades smoke: six-card page rendered");
              tavernScene.ConfigureUpgradesSmokeState(26, 0, true);
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::VerifyEmergencyReserve;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyEmergencyReserve:
            if (!diagnostics.upgradesPageRendered ||
                diagnostics.upgradeConfirmationRendered ||
                diagnostics.selectedUpgradeIndex != 0 ||
                diagnostics.selectedUpgradePrice != 25 ||
                diagnostics.upgradeCanPurchase ||
                diagnostics.upgradeBlockReason !=
                    TavernScene::UpgradePurchaseBlockReason::
                        EmergencyAleReserve) {
              failTavernUpgradesSmoke(
                  "zero Ale did not preserve the 2 G emergency reserve");
            } else if (tavernScene.Gold() != 26 ||
                       tavernScene.AleStock() != 0 ||
                       tavernScene.TotalMugs() != 2 ||
                       tavernScene.ExtraMugOwned() ||
                       tavernScene.UpgradePurchaseSerial() !=
                           tavernUpgradesPurchaseSerialBefore) {
              failTavernUpgradesSmoke(
                  "emergency reserve setup changed upgrade progression");
            } else {
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::AttemptEmergencyReservePurchase;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyEmergencyReserveNoOp:
            if (diagnostics.upgradeConfirmationRendered ||
                diagnostics.upgradeBlockReason !=
                    TavernScene::UpgradePurchaseBlockReason::
                        EmergencyAleReserve ||
                tavernScene.Gold() != 26 || tavernScene.AleStock() != 0 ||
                tavernScene.TotalMugs() != 2 || tavernScene.ExtraMugOwned() ||
                tavernScene.UpgradePurchaseSerial() !=
                    tavernUpgradesPurchaseSerialBefore) {
              failTavernUpgradesSmoke(
                  "blocked emergency-reserve purchase changed progression");
            } else {
              TraceAppEvent("Tavern upgrades smoke: emergency Ale reserve "
                            "blocked unsafe spending");
              tavernScene.ConfigureUpgradesSmokeState(200, 6, true);
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::VerifyRestoredInitialPage;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyRestoredInitialPage:
            if (!diagnostics.upgradesPageRendered ||
                diagnostics.upgradeConfirmationRendered ||
                diagnostics.selectedUpgradeIndex != 0 ||
                diagnostics.selectedUpgradePrice != 25 ||
                !diagnostics.upgradeCanPurchase ||
                diagnostics.upgradeBlockReason !=
                    TavernScene::UpgradePurchaseBlockReason::None ||
                tavernScene.Gold() != 200 || tavernScene.AleStock() != 6 ||
                tavernScene.TotalMugs() != 2 ||
                tavernScene.UpgradePurchaseSerial() !=
                    tavernUpgradesPurchaseSerialBefore) {
              failTavernUpgradesSmoke(
                  "upgrade fixture did not restore after reserve test");
            } else {
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::OpenExtraMugConfirmation;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyExtraMugConfirmation:
          case TavernUpgradesSmokePhase::VerifyReopenedExtraMugConfirmation:
            if (!diagnostics.upgradesPageRendered ||
                !diagnostics.upgradeConfirmationRendered ||
                diagnostics.selectedUpgradeIndex != 0 ||
                diagnostics.selectedUpgradePrice != 25 ||
                diagnostics.selectedUpgradeLevel != 0 ||
                !diagnostics.upgradeCanPurchase) {
              failTavernUpgradesSmoke("Extra Mug confirmation did not preserve "
                                      "the 25 G purchase preview");
            } else if (tavernScene.Gold() != 200 ||
                       tavernScene.TotalMugs() != 2 ||
                       tavernScene.ExtraMugOwned() ||
                       tavernScene.UpgradePurchaseSerial() !=
                           tavernUpgradesPurchaseSerialBefore) {
              failTavernUpgradesSmoke(
                  "opening Extra Mug confirmation mutated progression");
            } else {
              tavernUpgradesSmokePhase =
                  tavernUpgradesSmokePhase ==
                          TavernUpgradesSmokePhase::VerifyExtraMugConfirmation
                      ? TavernUpgradesSmokePhase::CancelExtraMugConfirmation
                      : TavernUpgradesSmokePhase::PurchaseExtraMug;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyExtraMugCancellation:
            if (!diagnostics.upgradesPageRendered ||
                diagnostics.upgradeConfirmationRendered ||
                diagnostics.selectedUpgradeIndex != 0 ||
                tavernScene.Gold() != 200 || tavernScene.TotalMugs() != 2 ||
                tavernScene.ExtraMugOwned() ||
                tavernScene.UpgradePurchaseSerial() !=
                    tavernUpgradesPurchaseSerialBefore) {
              failTavernUpgradesSmoke(
                  "B did not cancel Extra Mug confirmation without cost");
            } else {
              TraceAppEvent("Tavern upgrades smoke: confirmation cancellation "
                            "preserved state");
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::ReopenExtraMugConfirmation;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyExtraMugPurchase:
            tavernUpgradesExpectedGold = 175;
            if (diagnostics.upgradeConfirmationRendered ||
                diagnostics.selectedUpgradeIndex != 0 ||
                diagnostics.selectedUpgradeLevel != 1 ||
                diagnostics.extraMugCapacity != 3 ||
                diagnostics.upgradeCanPurchase ||
                diagnostics.upgradeBlockReason !=
                    TavernScene::UpgradePurchaseBlockReason::Owned ||
                tavernScene.Gold() != tavernUpgradesExpectedGold ||
                tavernScene.TotalMugs() != 3 || !tavernScene.ExtraMugOwned() ||
                tavernScene.UpgradePurchaseSerial() !=
                    tavernUpgradesPurchaseSerialBefore + 1) {
              failTavernUpgradesSmoke(
                  "Extra Mug was not purchased atomically for 25 G");
            } else {
              TraceAppEvent("Tavern upgrades smoke: mugs 2>3 purchased");
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::AttemptOwnedExtraMug;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyOwnedExtraMugNoOp:
            if (diagnostics.upgradeConfirmationRendered ||
                diagnostics.upgradeBlockReason !=
                    TavernScene::UpgradePurchaseBlockReason::Owned ||
                tavernScene.Gold() != tavernUpgradesExpectedGold ||
                tavernScene.TotalMugs() != 3 ||
                tavernScene.UpgradePurchaseSerial() !=
                    tavernUpgradesPurchaseSerialBefore + 1) {
              failTavernUpgradesSmoke(
                  "confirming an owned Extra Mug changed progression");
            } else {
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::SelectAleCapacity;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyAleCapacitySelection:
            if (diagnostics.selectedUpgradeIndex != 1 ||
                diagnostics.selectedUpgradePrice != 20 ||
                diagnostics.selectedUpgradeLevel != 0 ||
                diagnostics.aleCapacityLevel != 0 ||
                !diagnostics.upgradeCanPurchase ||
                diagnostics.upgradeBlockReason !=
                    TavernScene::UpgradePurchaseBlockReason::None) {
              failTavernUpgradesSmoke(
                  "Ale Capacity did not begin at level 0 for 20 G");
            } else {
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::OpenAleCapacityConfirmation;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyAleCapacityConfirmation:
            if (!diagnostics.upgradeConfirmationRendered ||
                diagnostics.selectedUpgradeIndex != 1 ||
                diagnostics.selectedUpgradePrice !=
                    tavernUpgradesExpectedAlePrice ||
                diagnostics.selectedUpgradeLevel !=
                    tavernUpgradesExpectedAleLevel ||
                !diagnostics.upgradeCanPurchase ||
                tavernScene.Gold() != tavernUpgradesExpectedGold ||
                tavernScene.AleStock() != 6 ||
                tavernScene.AleCapacity() !=
                    6 + tavernUpgradesExpectedAleLevel) {
              failTavernUpgradesSmoke(
                  "Ale Capacity confirmation showed the wrong level or price");
            } else {
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::PurchaseAleCapacity;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyAleCapacityPurchase: {
            const int purchasedPrice = tavernUpgradesExpectedAlePrice;
            const int purchasedLevel = tavernUpgradesExpectedAleLevel + 1;
            const int expectedGold =
                tavernUpgradesExpectedGold - purchasedPrice;
            const int expectedNextPrice =
                purchasedLevel < 3 ? 20 + purchasedLevel * 10 : 0;
            const uint64_t expectedSerial =
                tavernUpgradesPurchaseSerialBefore + 1 + purchasedLevel;
            if (diagnostics.upgradeConfirmationRendered ||
                diagnostics.selectedUpgradeIndex != 1 ||
                diagnostics.selectedUpgradeLevel != purchasedLevel ||
                diagnostics.selectedUpgradePrice != expectedNextPrice ||
                diagnostics.aleCapacityLevel != purchasedLevel ||
                tavernScene.AleCapacityLevel() != purchasedLevel ||
                tavernScene.AleCapacity() != 6 + purchasedLevel ||
                tavernScene.AleStock() != 6 ||
                tavernScene.Gold() != expectedGold ||
                tavernScene.UpgradePurchaseSerial() != expectedSerial) {
              failTavernUpgradesSmoke("Ale Capacity purchase changed the wrong "
                                      "level, Gold, stock, or serial");
              break;
            }
            tavernUpgradesExpectedAleLevel = purchasedLevel;
            tavernUpgradesExpectedAlePrice = expectedNextPrice;
            tavernUpgradesExpectedGold = expectedGold;
            if (purchasedLevel < 3) {
              if (!diagnostics.upgradeCanPurchase ||
                  diagnostics.upgradeBlockReason !=
                      TavernScene::UpgradePurchaseBlockReason::None) {
                failTavernUpgradesSmoke(
                    "the next Ale Capacity level was not purchasable");
              } else {
                tavernUpgradesSmokePhase =
                    TavernUpgradesSmokePhase::OpenAleCapacityConfirmation;
              }
            } else if (diagnostics.upgradeCanPurchase ||
                       diagnostics.upgradeBlockReason !=
                           TavernScene::UpgradePurchaseBlockReason::Maxed) {
              failTavernUpgradesSmoke(
                  "Ale Capacity level 3 was not marked MAXED");
            } else {
              TraceAppEvent(
                  "Tavern upgrades smoke: ALE capacity 6>7>8>9 without refill");
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::AttemptMaxedAleCapacity;
            }
            break;
          }
          case TavernUpgradesSmokePhase::VerifyMaxedAleCapacityNoOp:
            if (diagnostics.upgradeConfirmationRendered ||
                diagnostics.upgradeBlockReason !=
                    TavernScene::UpgradePurchaseBlockReason::Maxed ||
                tavernScene.AleCapacity() != 9 || tavernScene.AleStock() != 6 ||
                tavernScene.Gold() != tavernUpgradesExpectedGold ||
                tavernScene.UpgradePurchaseSerial() !=
                    tavernUpgradesPurchaseSerialBefore + 4) {
              failTavernUpgradesSmoke(
                  "confirming maxed Ale Capacity changed progression");
            } else {
              tavernUpgradesSmokePhase = TavernUpgradesSmokePhase::SelectTable3;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyTable3Selection:
            if (diagnostics.selectedUpgradeIndex != 2 ||
                diagnostics.selectedUpgradePrice != 60 ||
                diagnostics.selectedUpgradeLevel != 0 ||
                diagnostics.table3Unlocked || !diagnostics.upgradeCanPurchase ||
                diagnostics.upgradeBlockReason !=
                    TavernScene::UpgradePurchaseBlockReason::None ||
                tavernScene.Table3Unlocked() || tavernScene.Table3Enabled()) {
              failTavernUpgradesSmoke(
                  "Open Table 3 was not a purchasable 60 G upgrade");
            } else {
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::OpenTable3Confirmation;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyTable3Confirmation:
            if (!diagnostics.upgradeConfirmationRendered ||
                diagnostics.selectedUpgradeIndex != 2 ||
                diagnostics.selectedUpgradePrice != 60 ||
                diagnostics.selectedUpgradeLevel != 0 ||
                tavernScene.Gold() != tavernUpgradesExpectedGold ||
                tavernScene.Table3Unlocked() || tavernScene.Table3Enabled()) {
              failTavernUpgradesSmoke(
                  "Table 3 confirmation did not preserve the 60 G preview");
            } else {
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::PurchaseTable3;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyTable3Purchase:
            tavernUpgradesExpectedGold -= 60;
            if (diagnostics.upgradeConfirmationRendered ||
                diagnostics.selectedUpgradeIndex != 2 ||
                diagnostics.selectedUpgradeLevel != 1 ||
                !diagnostics.table3Unlocked || diagnostics.upgradeCanPurchase ||
                diagnostics.upgradeBlockReason !=
                    TavernScene::UpgradePurchaseBlockReason::Owned ||
                tavernScene.Gold() != tavernUpgradesExpectedGold ||
                tavernUpgradesExpectedGold != 25 ||
                !tavernScene.Table3Unlocked() || !tavernScene.Table3Enabled() ||
                tavernScene.UpgradePurchaseSerial() !=
                    tavernUpgradesPurchaseSerialBefore + 5) {
              failTavernUpgradesSmoke(
                  "Table 3 was not opened atomically for 60 G");
            } else {
              TraceAppEvent("Tavern upgrades smoke: Table 3 opened");
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::AttemptOwnedTable3;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyOwnedTable3NoOp:
            if (diagnostics.upgradeConfirmationRendered ||
                diagnostics.upgradeBlockReason !=
                    TavernScene::UpgradePurchaseBlockReason::Owned ||
                tavernScene.Gold() != 25 || !tavernScene.Table3Unlocked() ||
                !tavernScene.Table3Enabled() ||
                tavernScene.UpgradePurchaseSerial() !=
                    tavernUpgradesPurchaseSerialBefore + 5) {
              failTavernUpgradesSmoke(
                  "confirming owned Table 3 changed progression");
            } else {
              tavernUpgradesSmokePhase = TavernUpgradesSmokePhase::BackToRoot;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyBackToRoot:
            if (!tavernScene.ManagementMenuOpen() || !diagnostics.menuOpen ||
                !diagnostics.rootRendered || diagnostics.subpageOpen ||
                diagnostics.selectedCardIndex != 1 ||
                !tavernScene.PlayerMovementLocked()) {
              failTavernUpgradesSmoke(
                  "B did not return Upgrades to the management root");
            } else if (tavernScene.Gold() != 25 ||
                       tavernScene.UpgradePurchaseSerial() !=
                           tavernUpgradesPurchaseSerialBefore + 5) {
              failTavernUpgradesSmoke(
                  "returning to the root changed purchased upgrades");
            } else {
              tavernUpgradesSmokePhase = TavernUpgradesSmokePhase::CloseRoot;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyClosed:
            if (tavernScene.ManagementMenuOpen() || diagnostics.menuOpen ||
                tavernScene.PlayerMovementLocked()) {
              failTavernUpgradesSmoke(
                  "the second B did not close management and unlock movement");
            } else if (tavernScene.Gold() != 25 ||
                       tavernScene.TotalMugs() != 3 ||
                       tavernScene.AleStock() != 6 ||
                       tavernScene.AleCapacity() != 9 ||
                       !tavernScene.Table3Unlocked() ||
                       !tavernScene.Table3Enabled()) {
              failTavernUpgradesSmoke(
                  "closing management changed purchased progression");
            } else {
              gameTimeOfDayHours = 5.0f;
              tavernScene.BeginNextDay(gameTimeOfDayHours);
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::VerifyNextDayPersistence;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyNextDayPersistence:
            if (tavernScene.Gold() != 25 || tavernScene.TotalMugs() != 3 ||
                !tavernScene.ExtraMugOwned() || tavernScene.AleStock() != 6 ||
                tavernScene.AleCapacity() != 9 ||
                tavernScene.AleCapacityLevel() != 3 ||
                !tavernScene.Table3Unlocked() || !tavernScene.Table3Enabled() ||
                tavernScene.UpgradePurchaseSerial() !=
                    tavernUpgradesPurchaseSerialBefore + 5) {
              failTavernUpgradesSmoke(
                  "upgrades did not persist across BeginNextDay");
            } else {
              TraceAppEvent(
                  "Tavern upgrades smoke: next-day persistence passed");
              returnFromTavern();
              enterTavernMode();
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::VerifyReentryPersistence;
            }
            break;
          case TavernUpgradesSmokePhase::VerifyReentryPersistence:
            if (appMode != AppMode::Tavern || tavernScene.Gold() != 25 ||
                tavernScene.TotalMugs() != 3 || !tavernScene.ExtraMugOwned() ||
                tavernScene.AleStock() != 6 || tavernScene.AleCapacity() != 9 ||
                tavernScene.AleCapacityLevel() != 3 ||
                !tavernScene.Table3Unlocked() || !tavernScene.Table3Enabled() ||
                tavernScene.UpgradePurchaseSerial() !=
                    tavernUpgradesPurchaseSerialBefore + 5) {
              failTavernUpgradesSmoke("upgrades did not persist after leaving "
                                      "and re-entering Tavern");
            } else {
              TraceAppEvent(
                  "Tavern upgrades smoke: Tavern re-entry persistence passed");
              tavernUpgradesSmokePhase =
                  TavernUpgradesSmokePhase::AutomateTable3Cycle;
            }
            break;
          case TavernUpgradesSmokePhase::AutomateTable3Cycle:
            if (!tavernScene.Table3Unlocked() || !tavernScene.Table3Enabled()) {
              failTavernUpgradesSmoke(
                  "Table 3 became disabled during the production probe");
            } else if (tavernScene.TableCompletedCycles(2) >= 1) {
              TraceAppEvent("Tavern upgrades smoke: Table 3 completed a "
                            "service and wash cycle");
              tavernUpgradesSmokeCompleted = true;
              requestQuit = true;
            }
            break;
          default:
            break;
          }
        }
        if (updateAction == TavernScene::Action::SleepUntilMorning ||
            hudAction == TavernScene::Action::SleepUntilMorning) {
          gameTimeOfDayHours = 5.0f;
          tavernScene.BeginNextDay(gameTimeOfDayHours);
          playerPreview.SetPosition(tavernScene.PlayerSpawnPosition());
          playerPreview.SetYaw(0.0f);
        } else if (updateAction == TavernScene::Action::ReturnToOverworld ||
                   hudAction == TavernScene::Action::ReturnToOverworld) {
          returnFromTavern();
        }
      }

      // SSAO, Post Processing, and Cascaded Shadows panels moved to SceneEditor
      // (Phase 4).
      if (appMode == AppMode::Game) {
        overworldCollisionColliders =
            overworldScene.BuildCollisionColliders(overworldCollisionShapes);
      }

      // ---- Compute CSM cascade splits + per-cascade light VP ----
      using namespace DirectX;
      const auto &shadowCfg = editorScene.ShadowSettings();
      const XMFLOAT3 activeSunDirection =
          appMode == AppMode::Tavern
              ? tavernLighting.sunDirection
              : ((appMode == AppMode::Game || appMode == AppMode::BossArena)
                     ? gameLighting.sunDirection
                     : editorScene.LightSettings().lightDir);
      const XMVECTOR raysDir =
          XMVector3Normalize(XMLoadFloat3(&activeSunDirection));
      const XMFLOAT3 camPosF = cam.GetPosition();

      const uint32_t cascadeCount = dx.GetShadowMap().CascadeCount();
      float splits[kMaxCascades + 1];
      ComputeCascadeSplits(cam.NearZ(), shadowCfg.csmMaxDistance, cascadeCount,
                           shadowCfg.csmLambda, splits);

      std::array<XMMATRIX, kMaxCascades> cascadeVP = {};
      std::array<float, kMaxCascades> cascadeSplitDists = {};
      for (uint32_t c = 0; c < cascadeCount; ++c) {
        cascadeSplitDists[c] = splits[c + 1];
        cascadeVP[c] = ComputeCascadeViewProj(cam.View(), cam.FovY(),
                                              cam.Aspect(), splits[c],
                                              splits[c + 1], raysDir);
      }

      // ---- TAA jitter (Phase 10.4) ----
      cam.EnableJitter(editorScene.PostProcessSettings().taaEnabled ||
                       (appMode == AppMode::BossArena &&
                        bossArenaScene.TechShowcaseTAAEnabled()));
      cam.AdvanceJitter(dx.Width(), dx.Height());

      // ---- Build FrameData (Phase 8) ----
      // Shadow, SSAO, and post-process fields are set by Scene::BuildFrameData().
      FrameData frame{};
      frame.view = cam.View();
      frame.proj = cam.Proj(); // includes jitter when TAA is enabled
      frame.cameraPos = camPosF;
      frame.gameTime =
          (appMode == AppMode::Game || appMode == AppMode::Tavern ||
           appMode == AppMode::BossArena)
              ? gameRuntimeSeconds
              : t;
      frame.waterWaveParams = {waterWaveHeight, waterWaveSpeed,
                               waterWaveFrequency, 0.0f};
      frame.wetSurfaceParams = {wetSurfaceStrength, wetSurfaceDrySeconds,
                                wetSurfaceImpactRadius,
                                wetSurfaceCycleSeconds};
      frame.puddleParams = {puddleStrength, puddleBuildSeconds, puddleRadius,
                            puddleRippleStrength};
      frame.puddleVisualParams = {puddleClarity, puddleTint, 1.0f, 0.0f};
      frame.reflectionMode = reflectionMode;
      frame.cascadeCount = cascadeCount;
      frame.cascadeLightViewProj = cascadeVP;
      frame.cascadeSplitDistances = cascadeSplitDists;
      frame.skyExposure = skyExposure;
      frame.clearColor[0] = r;
      frame.clearColor[1] = g;
      frame.clearColor[2] = b;
      frame.clearColor[3] = 1.0f;
      frame.particlesEnabled = true;
      ApplySceneGlobalsToFrame(editorScene, frame);
      if (appMode == AppMode::BossArena &&
          bossArenaScene.IsPhoneOverlayActive()) {
        frame.dofEnabled = true;
        frame.dofFocalDistance = 0.8f;
        frame.dofFocalRange = 0.35f;
        frame.dofMaxBlur = 14.0f;
      }
      // ゲームロジック未実装のため、エミッタは常に有効化条件のみで追加する。
      if (particlesEnabled && fireEnabled)
        frame.emitters.push_back(&fireEmitter);
      if (particlesEnabled && smokeEnabled)
        frame.emitters.push_back(&smokeEmitter);
      if (particlesEnabled && sparkEnabled)
        frame.emitters.push_back(&sparkEmitter);
      if ((appMode == AppMode::Game || appMode == AppMode::BossArena) &&
          rainEnabled)
        frame.emitters.push_back(&rainEmitter);

      // Motion blur view-projection matrices (camera-dependent).
      {
        XMMATRIX vp = frame.view * frame.proj;
        frame.invViewProj = XMMatrixInverse(nullptr, vp);
        frame.prevViewProj = cam.PrevViewProj();
        frame.hasPrevViewProj = cam.HasPrevViewProj();
      }

      // TAA view-projection matrices (camera-dependent).
      {
        XMMATRIX vpUnjittered = frame.view * cam.ProjUnjittered();
        frame.invViewProjUnjittered = XMMatrixInverse(nullptr, vpUnjittered);
        frame.prevViewProjUnjittered = cam.PrevViewProjUnjittered();
      }

      // ---- Editor / Scene-Play レイヤー更新（ゲームモードは未実装のため除外） ----
      if (appMode == AppMode::Title) {
        frame.exposure = 0.95f;
        frame.bloomIntensity = 0.35f;
      } else if (appMode == AppMode::Game) {
        overworldScene.BuildFrame(frame);
        const size_t playerOpaqueItemBegin = frame.opaqueItems.size();
        const size_t playerTransparentItemBegin =
            frame.transparentItems.size();
        playerPreview.BuildFrame(frame);
        if (playerAnimationSmokeRequested &&
            !playerAnimationSmokeFailed &&
            !playerAnimationSmokeActionCompleted) {
          const size_t appendedOpaquePlayerItems =
              frame.opaqueItems.size() - playerOpaqueItemBegin;
          const size_t appendedTransparentPlayerItems =
              frame.transparentItems.size() - playerTransparentItemBegin;
          if (appendedOpaquePlayerItems !=
                  kPlayerAnimationSmokeExpectedOpaqueParts ||
              appendedTransparentPlayerItems !=
                  kPlayerAnimationSmokeExpectedTransparentParts) {
            failPlayerAnimationSmoke(
                "Player BuildFrame expected 9 opaque and 4 transparent items, "
                "got " +
                std::to_string(appendedOpaquePlayerItems) + " opaque and " +
                std::to_string(appendedTransparentPlayerItems) +
                " transparent");
          }
        }
        if (showCollisionDebug) {
          overworldScene.AppendCollisionDebugLines(frame,
                                                   overworldCollisionColliders);
        }
        if (showModelCollisionDebug) {
          overworldScene.AppendStageCollisionDebugLines(frame);
        }
        frame.skyExposure = gameSkyExposure;
        frame.lighting.lightDir = gameLighting.sunDirection;
        frame.lighting.lightColor = gameLighting.sunColor;
        frame.lighting.lightIntensity = gameLighting.sunIntensity;
        frame.lighting.iblIntensity = kGameIblIntensity;
        frame.exposure = gamePostExposure;
        frame.ssrEnabled = true;
        frame.ssrReflectionParams = {1.20f, 24.0f, 0.18f, 0.20f};
      } else if (appMode == AppMode::Tavern) {
        TavernScene::ViewContext tavernView;
        tavernView.firstPerson = tavernFirstPersonEnabled;
        tavernView.cameraPosition = cam.GetPosition();
        tavernView.cameraYaw = cam.Yaw();
        tavernView.cameraPitch = cam.Pitch();
        tavernScene.BuildFrame(frame, tavernView);
        if (!tavernFirstPersonEnabled)
          playerPreview.BuildFrame(frame);
        frame.gridEnabled = false;
        frame.clearColor[0] = 0.035f;
        frame.clearColor[1] = 0.020f;
        frame.clearColor[2] = 0.012f;
        frame.clearColor[3] = 1.0f;
        frame.skyExposure = 0.025f;
        frame.lighting.lightDir = tavernLighting.sunDirection;
        frame.lighting.lightColor = tavernLighting.sunColor;
        frame.lighting.lightIntensity = tavernLighting.sunIntensity;
        frame.lighting.iblIntensity = 0.28f;
        frame.exposure = 1.05f;
        frame.bloomEnabled = true;
        frame.bloomThreshold = 0.72f;
        frame.bloomIntensity = 0.58f;
        frame.ssrEnabled = false;
        frame.reflectionMode = ReflectionMode::Off;
      } else if (appMode == AppMode::BossArena) {
        frame.gridEnabled = false;
        frame.waterWaveParams = {0.0f, 1.0f, 1.0f, 0.0f};
        frame.ssrReflectionParams = {1.35f, 28.0f, 0.16f, 0.20f};
        bossArenaScene.BuildFrame(frame);
        const DirectX::XMFLOAT3 gameplayPlayerPos = playerPreview.Position();
        playerPreview.SetPosition(
            bossArenaScene.VisualPositionForGameplayPosition(gameplayPlayerPos));
        const size_t playerOpaqueItemBegin = frame.opaqueItems.size();
        const size_t playerTransparentItemBegin = frame.transparentItems.size();
        playerPreview.BuildFrame(frame);
        playerPreview.SetPosition(gameplayPlayerPos);
        if (bossMirrorPickupSmokeRequested && !bossMirrorPickupSmokeFailed) {
          const size_t appendedOpaquePlayerItems =
              frame.opaqueItems.size() - playerOpaqueItemBegin;
          const size_t appendedTransparentPlayerItems =
              frame.transparentItems.size() - playerTransparentItemBegin;
          if (appendedOpaquePlayerItems !=
                  kPlayerAnimationSmokeExpectedOpaqueParts ||
              appendedTransparentPlayerItems !=
                  kPlayerAnimationSmokeExpectedTransparentParts) {
            failBossMirrorPickupSmoke(
                "Player BuildFrame did not submit the expected model parts");
          }
        }
        const bool bossPhaseTwo = bossArenaScene.PhaseTwoActive();
        frame.clearColor[0] = bossPhaseTwo ? 0.070f : 0.028f;
        frame.clearColor[1] = bossPhaseTwo ? 0.030f : 0.045f;
        frame.clearColor[2] = bossPhaseTwo ? 0.040f : 0.058f;
        frame.clearColor[3] = 1.0f;
        frame.skyExposure = 0.055f;
        frame.lighting.lightDir = {-0.34f, -0.72f, 0.44f};
        frame.lighting.lightColor =
            bossPhaseTwo ? DirectX::XMFLOAT3{0.90f, 0.42f, 0.38f}
                         : DirectX::XMFLOAT3{0.42f, 0.70f, 0.92f};
        frame.lighting.lightIntensity = bossPhaseTwo ? 1.46f : 1.28f;
        frame.lighting.iblIntensity = 0.36f;
        frame.exposure = bossPhaseTwo ? 1.12f : 1.05f;
        frame.bloomThreshold = bossPhaseTwo ? 0.56f : 0.62f;
        frame.bloomIntensity =
            std::max(frame.bloomIntensity, bossPhaseTwo ? 1.02f : 0.88f);
        bossArenaScene.ApplyTechShowcase(frame);
      } else if (scenePlayMode) {
        // Keep the complete shell visible during play, including Stop.
        editorScene.BuildFrameData(frame);
        sceneEditor.DrawUI(editorScene, dx, cam.View(), cam.Proj(), &iblEnabled,
                           &editStage, &cam, &editorRuntimeBindings, true);
      } else {
        sceneEditor.DrawUI(editorScene, dx, cam.View(), cam.Proj(), &iblEnabled,
                           &editStage, &cam, &editorRuntimeBindings, false);
        editorScene.BuildFrameData(frame);
        sceneEditor.BuildHighlightItems(editorScene, frame);

        // Phase 5B: inject 3D grid tiles into FrameData when grid editor is open.
        if (sceneEditor.IsGridEditorOpen()) {
          sceneEditor.BuildStageViewportItems(editStage, frame);
        }
      }

      // 水墨輪郭と墨流れは Boss Phase 2 の世界変化としてのみ適用する。
      if (appMode == AppMode::BossArena && bossArenaScene.PhaseTwoActive()) {
        frame.inkWashStrength = inkWashStrength;
        frame.inkFlowStrength = inkFlowStrength;
        frame.inkFlowSpeed = inkFlowSpeed;
      }

      // Mode indicator overlay.
      if (appMode == AppMode::Editor) {
        const EditorViewportRect &modeViewport = sceneEditor.ViewportRect();
        ImGui::SetNextWindowPos(
            ImVec2(modeViewport.x + 10.0f,
                   modeViewport.y + modeViewport.height - 8.0f),
            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::Begin("##ModeIndicator", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoBackground);
        if (scenePlayMode) {
          ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                             "[SCENE PLAY] F5=Stop");
        } else if (sceneEditor.IsGridEditorOpen()) {
          // グリッドエディタ表示中の状態表示。
          ImGui::TextColored(ImVec4(0.2f,0.8f,1,1), "[EDITOR] F5=Scene Play  F6=Grid Editor (ON)");
        } else {
          // 通常エディタ状態。
          ImGui::TextColored(ImVec4(0,1,0.5f,1), "[EDITOR] F5=Scene Play  F6=Grid Editor");
        }
        ImGui::TextUnformatted("Reflection [F8]");
        ImGui::SameLine();
        if (ImGui::RadioButton("Off##ReflectionMode",
                               reflectionMode == ReflectionMode::Off)) {
          reflectionMode = ReflectionMode::Off;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("SSR##ReflectionMode",
                               reflectionMode == ReflectionMode::SSR)) {
          reflectionMode = ReflectionMode::SSR;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!hybridReflection.IsSupported());
        if (ImGui::RadioButton(
                "Hybrid DXR##ReflectionMode",
                reflectionMode == ReflectionMode::HybridDXR)) {
          reflectionMode = ReflectionMode::HybridDXR;
        }
        ImGui::EndDisabled();
        ImGui::End();
      }

      if (launchEditor &&
          (appMode == AppMode::Game || appMode == AppMode::Tavern ||
           appMode == AppMode::BossArena)) {
        ImGui::SetNextWindowPos(
            ImVec2(static_cast<float>(window.Width()) - 18.0f, 18.0f),
            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.72f);
        ImGui::Begin("##ReturnToEditor", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoNav);
        ImGui::Text("F1  Return to VILLIEN Editor");
        ImGui::End();
      }

      // ---- Shader reload overlay (Phase 8) ----
      if (g_shaderReloadTimer > 0.0f) {
        g_shaderReloadTimer -= dt;
        float alpha = std::min(1.0f, g_shaderReloadTimer);
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(window.Width()) / 2.0f, 40.0f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::Begin("##ShaderReload", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        if (g_shaderReloadErrors.empty()) {
          ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "Shaders reloaded OK");
        } else {
          ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Shader reload errors:");
          ImGui::TextUnformatted(g_shaderReloadErrors.c_str());
        }
        ImGui::End();
        ImGui::PopStyleVar();
      }

      // Toggle IBL descriptors on/off (mesh renderer + deferred lighting).
      if (iblEnabled) {
        dx.GetMeshRenderer().SetIBLDescriptors(iblGenerator.IBLTableGpuBase());
        dx.SetIblTableGpu(iblGenerator.IBLTableGpuBase());
      } else {
        dx.GetMeshRenderer().SetIBLDescriptors({});
        dx.SetIblTableGpu({});
      }
      // IBL intensity: gate by iblEnabled toggle, otherwise use scene value.
      if (!iblEnabled)
        frame.lighting.iblIntensity = 0.0f;
      frame.lighting.cascadeDebug = shadowCfg.csmDebugCascades ? 1.0f : 0.0f;

      // ---- Execute render passes (Phase 8 + Phase 9 + Phase 12.1) ----
      const bool traceGameFrame =
          appMode == AppMode::Game && gameTraceFramesRemaining > 0;
      if (traceGameFrame) {
        TraceAppEvent("game frame: begin");
        --gameTraceFramesRemaining;
      }
      if (traceGameFrame)
        TraceAppEvent("pass: BeginFrame");
      dx.BeginFrame();
      if (traceGameFrame)
        TraceAppEvent("pass: Shadow");
      shadowPass.Execute(dx, frame);
      if (traceGameFrame)
        TraceAppEvent("pass: Sky");
      skyPass.Execute(dx, frame);
      if (traceGameFrame)
        TraceAppEvent("pass: GBuffer");
      gbufferPass.Execute(dx, frame);
      if (traceGameFrame)
        TraceAppEvent("pass: DeferredLighting");
      deferredLightingPass.Execute(dx, frame);
      bool dxrProofSceneReady = hybridReflection.SceneReady();
      if (frame.reflectionMode == ReflectionMode::HybridDXR) {
        dxrProofSceneReady = hybridReflection.PrepareScene(
            dx, frame, dx.GetMeshRenderer());
        if (!dxrProofAuditLogged) {
          const std::string dxrAudit =
              "DXR scene audit: opaqueItems=" +
              std::to_string(frame.opaqueItems.size()) + " status=" +
              hybridReflection.Status();
          TraceAppEvent(dxrAudit.c_str());
          dxrProofAuditLogged = true;
        }
        if (dxrProofSceneReady && !dxrProofSceneLogged) {
          const std::string dxrSceneStatus =
              "DXR scene ready: " + hybridReflection.Status() +
              " instances=" +
              std::to_string(hybridReflection.SceneInstanceCount()) +
              " cachedBlas=" +
              std::to_string(hybridReflection.BlasCount());
          TraceAppEvent(dxrSceneStatus.c_str());
          dxrProofSceneLogged = true;
        }
        if (dxrProofSceneReady &&
            hybridReflection.Execute(dx, frame, skyRenderer.HdriTexture(),
                                     skyRenderer.HdriSrvGpu())) {
          if (!dxrProofDispatchLogged) {
            const std::string dxrDispatchStatus =
                "DXR dispatch proof: " + hybridReflection.Status();
            TraceAppEvent(dxrDispatchStatus.c_str());
            dxrProofDispatchLogged = true;
          }
        } else {
          // Scene data がまだ揃わない frame では既存 SSR へ安全に戻す。
          frame.reflectionMode = ReflectionMode::SSR;
        }
      }
      // SSR は opaque scene と対応する depth だけを参照する。
      // Particle／debug overlay を先に描くと、深度を持たない色が水面へ伸びる。
      if (traceGameFrame)
        TraceAppEvent("pass: SSR");
      ssrPass.Execute(dx, frame);
      if (traceGameFrame)
        TraceAppEvent("pass: Grid");
      gridPass.Execute(dx, frame);
      if (traceGameFrame)
        TraceAppEvent("pass: Transparent");
      transparentPass.Execute(dx, frame);
      if (traceGameFrame)
        TraceAppEvent("pass: Highlight");
      highlightPass.Execute(dx, frame);
      if (traceGameFrame)
        TraceAppEvent("pass: TransparentMesh");
      transparentMeshPass.Execute(dx, frame);
      if (traceGameFrame)
        TraceAppEvent("pass: SSAO");
      ssaoPass.Execute(dx, frame);
      if (traceGameFrame)
        TraceAppEvent("pass: VelocityGen");
      velocityGenPass.Execute(dx, frame);  // moved before TAA (Phase 10.4)
      if (traceGameFrame)
        TraceAppEvent("pass: TAA");
      taaPass.Execute(dx, frame);          // TAA resolve (Phase 10.4)
      if (traceGameFrame)
        TraceAppEvent("pass: Bloom");
      bloomPass.Execute(dx, frame);
      if (traceGameFrame)
        TraceAppEvent("pass: Tonemap");
      tonemapPass.Execute(dx, frame);
      if (traceGameFrame)
        TraceAppEvent("pass: DOF");
      dofPass.Execute(dx, frame);
      if (traceGameFrame)
        TraceAppEvent("pass: MotionBlur");
      motionBlurPass.Execute(dx, frame);
      if (traceGameFrame)
        TraceAppEvent("pass: FXAA");
      fxaaPass.Execute(dx, frame);
      if (appMode == AppMode::Editor)
        dx.CaptureBackBufferForEditor();
      if (traceGameFrame)
        TraceAppEvent("pass: UI");
      uiPass.Execute(dx, frame);
      if (traceGameFrame)
        TraceAppEvent("pass: EndFrame");
      dx.EndFrame();
      if (bossMirrorPickupSmokeRequested &&
          bossMirrorPickupActionPoseThisFrame && !bossMirrorPickupSmokeFailed) {
        ++bossMirrorPickupRenderedActionFrames;
        if (bossMirrorPickupAwaitingResponse &&
            !bossMirrorPickupTriggeredThisFrame) {
          ++bossMirrorPickupRenderedResponses;
          bossMirrorPickupAwaitingResponse = false;
          if (bossMirrorPickupTeleportPending) {
            playerPreview.SetPosition(bossMirrorPickupPendingPosition);
            bossMirrorPickupTeleportPending = false;
          }
        }
      }
      if (playerAnimationSmokeRequested && playerAnimationSmokeFailed) {
        requestQuit = true;
      } else if (playerAnimationSmokeRequested &&
                 !playerAnimationSmokeSequenceCompleted) {
        ++playerAnimationSmokeRenderedFrames;
        if ((playerAnimationSmokeRenderedFrames %
             kPlayerAnimationSmokeFramesPerState) == 0) {
          const int completedSequenceIndex =
              playerAnimationSmokeRenderedFrames /
                  kPlayerAnimationSmokeFramesPerState -
              1;
          if (playerPreview.IsTransitioning()) {
            failPlayerAnimationSmoke(
                std::string("crossfade did not settle for state ") +
                playerClipName(
                    kPlayerAnimationSmokeSequence[completedSequenceIndex]));
          } else {
            const std::string stateMessage =
                std::string("Player animation smoke: settled state ") +
                playerClipName(
                    kPlayerAnimationSmokeSequence[completedSequenceIndex]) +
                " after " +
                std::to_string(kPlayerAnimationSmokeFramesPerState) +
                " rendered frames";
            TraceAppEvent(stateMessage.c_str());
          }
        }
        if (!playerAnimationSmokeFailed &&
            playerAnimationSmokeRenderedFrames ==
                kPlayerAnimationSmokeTotalFrames) {
          TraceAppEvent("Player animation smoke: render sequence completed");
          playerAnimationSmokeSequenceCompleted = true;
          const uint64_t actionSerialBefore =
              playerPreview.ActionTriggerSerial();
          if (!playerPreview.PlayTakingItem()) {
            failPlayerAnimationSmoke(
                "TakingItem one-shot could not be started");
          } else if (playerPreview.ActionTriggerSerial() !=
                         actionSerialBefore + 1 ||
                     !playerPreview.IsActionPlaying()) {
            failPlayerAnimationSmoke(
                "TakingItem one-shot did not activate exactly once");
          } else {
            playerAnimationSmokeActionStarted = true;
            playerAnimationSmokeActionSerial = actionSerialBefore + 1;
            TraceAppEvent(
                "Player animation smoke: TakingItem one-shot started");
          }
        }
      } else if (playerAnimationSmokeRequested &&
                 playerAnimationSmokeActionStarted &&
                 !playerAnimationSmokeActionCompleted) {
        ++playerAnimationSmokeActionRenderedFrames;
        if (playerAnimationSmokeActionPoseThisFrame)
          ++playerAnimationSmokeRenderedActionPoseFrames;
        if (!playerPreview.IsActionPlaying()) {
          if (playerPreview.ActiveClip() !=
              PlayerAnimationPreview::ClipSlot::Idle) {
            failPlayerAnimationSmoke(
                "TakingItem one-shot did not return to Idle");
          } else if (playerPreview.IsTransitioning()) {
            failPlayerAnimationSmoke(
                "TakingItem one-shot left a locomotion crossfade active");
          } else if (playerAnimationSmokeRenderedActionPoseFrames <= 0) {
            failPlayerAnimationSmoke(
                "TakingItem one-shot completed without a rendered pose");
          } else {
            playerAnimationSmokeActionCompleted = true;
            TraceAppEvent(
                "Player animation smoke: TakingItem one-shot completed "
                "without looping");
            requestQuit = true;
          }
        } else if (playerAnimationSmokeActionRenderedFrames >=
                   kPlayerAnimationSmokeActionMaxFrames) {
          failPlayerAnimationSmoke(
              "TakingItem one-shot did not finish within 180 frames");
        }
      }
      if (dxrSmokeMode && dxrProofDispatchLogged &&
          dxrSmokeFramesRemaining > 0) {
        --dxrSmokeFramesRemaining;
        if (dxrSmokeFramesRemaining == 0) {
          TraceAppEvent(
              dxrOverworldSmokeRequested
                  ? "DXR overworld smoke: completed four rendered frames"
                  : "DXR smoke: completed four rendered frames");
          requestQuit = true;
        }
      }
      if (tavernSmokeRequested) {
        if (!tavernSmokeReturnIssued && appMode == AppMode::Tavern &&
            tavernSmokeTavernFramesRemaining > 0) {
          --tavernSmokeTavernFramesRemaining;
          if (tavernSmokeTavernFramesRemaining == 0) {
            TraceAppEvent("Tavern smoke: tavern frames complete");
            returnFromTavern();
            tavernSmokeReturnIssued = true;
          }
        } else if (tavernSmokeReturnIssued && appMode == AppMode::Game &&
                   tavernSmokeOverworldFramesRemaining > 0) {
          --tavernSmokeOverworldFramesRemaining;
          if (tavernSmokeOverworldFramesRemaining == 0) {
            TraceAppEvent(
                "Tavern smoke: enter and return route completed four frames");
            requestQuit = true;
          }
        }
      }
      if (tavernGameplaySmokeRequested && !tavernGameplaySmokeLogged &&
          tavernScene.GameplaySmokeComplete()) {
        std::array<TavernCustomerPreview::ClipDiagnostics,
                   TavernScene::CustomerModelCount>
            customerIdle{};
        std::array<TavernCustomerPreview::ClipDiagnostics,
                   TavernScene::CustomerModelCount>
            customerWalk{};
        std::array<TavernCustomerPreview::ClipDiagnostics,
                   TavernScene::CustomerModelCount>
            customerSitting{};
        constexpr std::array<size_t, TavernScene::CustomerModelCount>
            expectedBoneCounts = {65, 67};
        bool customerAnimationReady = tavernScene.ImportedCustomerReady() &&
                                      tavernScene.CustomerBonePaletteFinite() &&
                                      tavernScene
                                          .CustomerAnimationInstancesIndependent();
        for (size_t modelIndex = 0;
             modelIndex < TavernScene::CustomerModelCount; ++modelIndex) {
          customerIdle[modelIndex] =
              tavernScene.CustomerIdleDiagnostics(modelIndex);
          customerWalk[modelIndex] =
              tavernScene.CustomerWalkDiagnostics(modelIndex);
          customerSitting[modelIndex] =
              tavernScene.CustomerSittingDiagnostics(modelIndex);
          customerAnimationReady =
              customerAnimationReady &&
              tavernScene.CustomerSkeletonBoneCount(modelIndex) ==
                  expectedBoneCounts[modelIndex] &&
              tavernScene.CustomerMaterialPartCount(modelIndex) == 7 &&
              customerIdle[modelIndex].loaded &&
              customerWalk[modelIndex].loaded &&
              customerSitting[modelIndex].loaded &&
              std::abs(customerIdle[modelIndex].duration - 9.916667f) <=
                  0.01f &&
              std::abs(customerWalk[modelIndex].duration - 0.95f) <= 0.01f &&
              customerSitting[modelIndex].duration > 0.1f &&
              customerSitting[modelIndex].trackCount > 0 &&
              tavernScene.CustomerIdlePoseUpdateCount(modelIndex) > 0 &&
              tavernScene.CustomerWalkPoseUpdateCount(modelIndex) > 0 &&
              tavernScene.CustomerSittingPoseUpdateCount(modelIndex) > 0;
        }
        if (!customerAnimationReady) {
          TraceAppEvent(
              "Tavern gameplay smoke: FAIL; model-only NPC roster, shared "
              "Idle/Walk/Sitting animation, or per-instance pose isolation "
              "gate incomplete");
          applicationExitCode = 2;
          tavernGameplaySmokeLogged = true;
          requestQuit = true;
          continue;
        }
        std::ostringstream message;
        message << "Tavern gameplay smoke: completed mixed Ale/Food service "
                   "and dish-wash cycles; cycles="
                << tavernScene.CompletedCycles()
                << " gold=" << tavernScene.Gold()
                << " served=" << tavernScene.ServedCustomers()
                << " food=" << tavernScene.FoodServed()
                << " walkouts=" << tavernScene.Walkouts()
                << " npcRoster=" << TavernScene::CustomerModelCount
                << " poseIsolation=PASS";
        for (size_t modelIndex = 0;
             modelIndex < TavernScene::CustomerModelCount; ++modelIndex) {
          message << " [" << tavernScene.CustomerModelLabel(modelIndex)
                  << ":parts="
                  << tavernScene.CustomerMaterialPartCount(modelIndex)
                  << ",bones="
                  << tavernScene.CustomerSkeletonBoneCount(modelIndex)
                  << ",idle=" << customerIdle[modelIndex].duration << "s/"
                  << customerIdle[modelIndex].trackCount
                  << ",walk=" << customerWalk[modelIndex].duration << "s/"
                  << customerWalk[modelIndex].trackCount << ",updates="
                  << tavernScene.CustomerIdlePoseUpdateCount(modelIndex) << "/"
                  << tavernScene.CustomerWalkPoseUpdateCount(modelIndex)
                  << ",sitting=" << customerSitting[modelIndex].duration
                  << "s/" << customerSitting[modelIndex].trackCount << "/"
                  << tavernScene.CustomerSittingPoseUpdateCount(modelIndex)
                  << "]";
        }
        TraceAppEvent(message.str().c_str());
        tavernGameplaySmokeLogged = true;
        requestQuit = true;
      }
      if (tavernDaySmokeRequested && !tavernDaySmokeLogged &&
          tavernScene.DayCycleSmokeComplete()) {
        std::ostringstream message;
        message << "Tavern day smoke: crossed midnight and slept until 05:00; "
                   "hour="
                << tavernScene.BusinessHour()
                << " cycles=" << tavernScene.CompletedCycles()
                << " gold=" << tavernScene.Gold()
                << " served=" << tavernScene.ServedCustomers()
                << " walkouts=" << tavernScene.Walkouts();
        TraceAppEvent(message.str().c_str());
        tavernDaySmokeLogged = true;
        requestQuit = true;
      }
      if (traceGameFrame)
        TraceAppEvent("game frame: end");

      // Swap TAA ping-pong so next frame reads our output as history.
      if (frame.taaEnabled) {
        dx.SwapTaaBuffers();
      }

      // Store current VP as "previous" for next frame's motion blur.
      cam.UpdatePrevViewProj();

      if (requestQuit)
        TraceAppEvent("requestQuit: PostQuitMessage");
      if (requestQuit)
        PostQuitMessage(0);
    }

    if (playerAnimationSmokeRequested &&
        (!playerAnimationSmokeSequenceCompleted ||
         !playerAnimationSmokeActionCompleted) &&
        !playerAnimationSmokeFailed) {
      failPlayerAnimationSmoke(
          "application ended before locomotion and TakingItem verification "
          "completed");
    }
    if (bossMirrorPickupSmokeRequested && !bossMirrorPickupSmokeCompleted &&
        !bossMirrorPickupSmokeFailed) {
      failBossMirrorPickupSmoke(
          "application ended before pickup verification completed");
    }
    if (tavernManagementSmokeRequested && !tavernManagementSmokeCompleted &&
        !tavernManagementSmokeFailed) {
      failTavernManagementSmoke("application ended before the "
                                "root/Upgrades/back/close route completed");
    }
    if (tavernSuppliesSmokeRequested && !tavernSuppliesSmokeCompleted &&
        !tavernSuppliesSmokeFailed) {
      failTavernSuppliesSmoke(
          "application ended before the supplies economy route completed");
    }
    if (tavernUpgradesSmokeRequested && !tavernUpgradesSmokeCompleted &&
        !tavernUpgradesSmokeFailed) {
      failTavernUpgradesSmoke(
          "application ended before the upgrade progression route completed");
    }

    // ---- Shutdown (reverse init order) ----
    dx.WaitForGpu(); // Flush GPU before releasing any resources
    if (dxrSmokeMode) {
      std::ofstream debugLog("dxr_smoke_debug_log.txt",
                             std::ios::out | std::ios::trunc);
      if (debugLog)
        dx.DumpDebugMessages(debugLog);
    }
    if (tavernSmokeRequested || tavernGameplaySmokeRequested ||
        tavernDaySmokeRequested || tavernManagementSmokeRequested ||
        tavernSuppliesSmokeRequested || tavernUpgradesSmokeRequested) {
      std::ofstream debugLog("tavern_smoke_debug_log.txt",
                             std::ios::out | std::ios::trunc);
      if (debugLog)
        dx.DumpDebugMessages(debugLog);
    }
    if (tavernManagementSmokeRequested) {
      std::ostringstream debugReport;
      dx.DumpDebugMessages(debugReport);
      const std::string debugReportText = debugReport.str();
      if (debugReportText.find(
              "D3D12 Error/Corruption messages:\n  (none)\n") ==
          std::string::npos) {
        failTavernManagementSmoke(
            "D3D12 debug layer reported an error/corruption message");
      } else if (tavernManagementSmokeCompleted &&
                 !tavernManagementSmokeFailed) {
        TraceAppEvent("Tavern management smoke: PASS; menu=open cards=2 "
                      "order=Supplies,Upgrades selection=Upgrades activated=1 "
                      "upgradesPage=1 navigation=B>B "
                      "tablePriority=nearer economyDelta=0 close=unlocked "
                      "d3dErrors=0");
      }
    }
    if (tavernSuppliesSmokeRequested) {
      std::ostringstream debugReport;
      dx.DumpDebugMessages(debugReport);
      const std::string debugReportText = debugReport.str();
      if (debugReportText.find(
              "D3D12 Error/Corruption messages:\n  (none)\n") ==
          std::string::npos) {
        failTavernSuppliesSmoke(
            "D3D12 debug layer reported an error/corruption message");
      } else if (tavernSuppliesSmokeCompleted && !tavernSuppliesSmokeFailed) {
        TraceAppEvent("Tavern supplies smoke: PASS; initial=6/6 fullGuard=1 "
                      "navigation=B>B cancelNoCost=1 serviceConsumption=1 "
                      "purchase=1@2G "
                      "insufficientNoOp=1 recoveryFloor=2 affordableFloor=0 "
                      "d3dErrors=0");
      }
    }
    if (tavernUpgradesSmokeRequested) {
      std::ostringstream debugReport;
      dx.DumpDebugMessages(debugReport);
      const std::string debugReportText = debugReport.str();
      if (debugReportText.find(
              "D3D12 Error/Corruption messages:\n  (none)\n") ==
          std::string::npos) {
        failTavernUpgradesSmoke(
            "D3D12 debug layer reported an error/corruption message");
      } else if (tavernUpgradesSmokeCompleted && !tavernUpgradesSmokeFailed) {
        std::ostringstream passMessage;
        passMessage << "Tavern upgrades smoke: PASS; cards=6 confirmCancel=1 "
                       "emergencyReserve=1 "
                       "mugs=2>3 aleCapacity=6>7>8>9 noRefill=1 table3=open "
                       "ownedMaxedNoOp=1 nextDay=preserved reentry=preserved "
                       "table3Cycles="
                    << tavernScene.TableCompletedCycles(2)
                    << " purchases=5 goldBeforeService="
                    << tavernUpgradesExpectedGold << " d3dErrors=0";
        TraceAppEvent(passMessage.str().c_str());
      }
    }
    if (playerAnimationSmokeRequested) {
      std::ostringstream debugReport;
      dx.DumpDebugMessages(debugReport);
      const std::string debugReportText = debugReport.str();
      std::ofstream debugLog("player_animation_smoke_debug_log.txt",
                             std::ios::out | std::ios::trunc);
      if (debugLog)
        debugLog << debugReportText;
      if (debugReportText.find(
              "D3D12 Error/Corruption messages:\n  (none)\n") ==
          std::string::npos) {
        failPlayerAnimationSmoke(
            "D3D12 debug layer reported an error/corruption message");
      } else if (playerAnimationSmokeSequenceCompleted &&
                 playerAnimationSmokeActionCompleted &&
                 !playerAnimationSmokeFailed) {
        std::ostringstream passMessage;
        passMessage << "Player animation smoke: PASS; parts="
                    << playerPreview.MaterialPartCount()
                    << " opaque=" << playerPreview.OpaqueMaterialPartCount()
                    << " transparent="
                    << playerPreview.TransparentMaterialPartCount()
                    << " doubleSided="
                    << playerPreview.DoubleSidedMaterialPartCount()
                    << " bones=" << playerPreview.SkeletonBoneCount()
                    << " transitions=Idle>Walk>Run>Idle renderedFrames="
                    << playerAnimationSmokeRenderedFrames
                    << " takingItemOneShotFrames="
                    << playerAnimationSmokeActionRenderedFrames
                    << " takingItemPoseFrames="
                    << playerAnimationSmokeRenderedActionPoseFrames
                    << " d3dErrors=0";
        TraceAppEvent(passMessage.str().c_str());
      }
    }
    if (bossMirrorPickupSmokeRequested) {
      std::ostringstream debugReport;
      dx.DumpDebugMessages(debugReport);
      const std::string debugReportText = debugReport.str();
      std::ofstream debugLog("boss_mirror_pickup_smoke_debug_log.txt",
                             std::ios::out | std::ios::trunc);
      if (debugLog)
        debugLog << debugReportText;
      if (debugReportText.find(
              "D3D12 Error/Corruption messages:\n  (none)\n") ==
          std::string::npos) {
        failBossMirrorPickupSmoke(
            "D3D12 debug layer reported an error/corruption message");
      } else if (bossMirrorPickupSmokeCompleted &&
                 !bossMirrorPickupSmokeFailed) {
        std::ostringstream passMessage;
        passMessage << "Boss mirror pickup smoke: PASS; chargeDelta=3 "
                       "actionTriggers=3 renderedFrames="
                    << bossMirrorPickupSmokeFrames << " renderedActionFrames="
                    << bossMirrorPickupRenderedActionFrames
                    << " renderedResponses="
                    << bossMirrorPickupRenderedResponses
                    << " return=Idle d3dErrors=0";
        TraceAppEvent(passMessage.str().c_str());
      }
    }
    hybridReflection.Reset();
    ssaoRenderer.Reset();
    postProcess.Reset();
    iblGenerator.Reset();
    skyRenderer.Reset();
    gridRenderer.Reset();
    imgui.Shutdown(window);
    dx.Shutdown();
    g_crashDxContext = nullptr;
  } catch (const std::exception &e) {
    {
      std::ofstream f("fatal_log.txt", std::ios::out | std::ios::trunc);
      if (f)
        f << "std::exception: " << e.what() << "\n";
      if (g_crashDxContext)
        g_crashDxContext->DumpDebugMessages(f);
    }
    MessageBoxA(nullptr, e.what(), "Error",
                MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SYSTEMMODAL);
    return -1;
  } catch (...) {
    {
      std::ofstream f("fatal_log.txt", std::ios::out | std::ios::trunc);
      if (f)
        f << "unknown exception\n";
      if (g_crashDxContext)
        g_crashDxContext->DumpDebugMessages(f);
    }
    MessageBoxA(nullptr, "Unknown error occurred", "Error",
                MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SYSTEMMODAL);
    return -1;
  }
  return applicationExitCode;
}
