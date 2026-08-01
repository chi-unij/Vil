// ======================================
// File: main.cpp
// Purpose: Application entry point and main loop (window, input, camera,
//          rendering, ImGui). Phase 8: uses render passes + FrameData.
// ======================================

#include "Camera.h"
#include "DxContext.h"
#include "GridRenderer.h"
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
#include "game/TitleScreen.h"
#include "game/WorldRainParticles.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <dbghelp.h>
#include <fstream>
#include <sstream>
#include <vector>

#include <imgui.h>

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

static ImU32 UiColor(float r, float g, float b, float a) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a));
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

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int nCmdShow) {
  try {
    {
      std::ofstream f("app_trace_log.txt", std::ios::out | std::ios::trunc);
      if (f)
        f << "app start\n";
    }
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);

    Win32Window window;
    SetStartupStage(10);
    window.Create(L"VILLIEN", 1920, 1080);

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

    // ---- Initialize renderer modules (Phase 8) ----
    SkyRenderer skyRenderer;
    skyRenderer.Initialize(dx);

    IBLGenerator iblGenerator;
    iblGenerator.Initialize(dx, skyRenderer.HdriTexture(), skyRenderer.HdriSrvGpu());

    GridRenderer gridRenderer;
    gridRenderer.Initialize(dx);

    // Wire IBL descriptors to the mesh renderer + DxContext (for deferred lighting).
    dx.GetMeshRenderer().SetIBLDescriptors(iblGenerator.IBLTableGpuBase());
    dx.SetIblTableGpu(iblGenerator.IBLTableGpuBase());

    // CHI-35: Player / Idle / Walk / Run クリップ確認用プレビュー。
    PlayerAnimationPreview playerPreview;
    playerPreview.Initialize(dx);
    BossArenaScene bossArenaScene;
    bossArenaScene.Initialize(dx);
    bossArenaScene.Reset(playerPreview);
    OverworldScene overworldScene;
    overworldScene.Initialize(dx);
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

    // ---- Initialize SSAO (Phase 10.3) ----
    SSAORenderer ssaoRenderer;
    ssaoRenderer.Initialize(dx);

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
    float gameHoursPerSecond = 0.25f;
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
    overworldScene.SetWaterTransparency(dx, waterTransparency);

    // IBL (Phase 10.2)
    bool iblEnabled = true;

    // Settings UI (Phase 12.6)
    bool showSettings = false;
    bool prevEsc = false;
    bool gameFreeCameraEnabled = false;

    // ---- Editor/Game mode toggle (Milestone 4 Phase 0) ----
    enum class AppMode { Title, Game, BossArena, Editor };
    AppMode appMode = AppMode::Title;
    bool requestQuit = false;
    TitleScreen titleScreen;
    Scene editorScene;
    SceneEditor sceneEditor;
    StageData editStage; // Grid editor stage data (Phase 5).
    editStage.Clear();   // Initialize with default grid.
    sceneEditor.InitEditorMeshes(dx); // Phase 5B: create viewport tile meshes.
    bool prevF5 = false;
    bool prevLButton = false; // for edge-detection of left-click (mouse pick)

    // Scene play mode (Phase 8).
    bool scenePlayMode = false;
    std::string sceneSnapshot;

    // Shader hot-reload (Phase 8).
    bool prevF9 = false;
    bool prevF10 = false;
    std::string g_shaderReloadErrors;
    float g_shaderReloadTimer = 0.0f;
    static constexpr float kShaderMsgOkDuration = 3.0f;
    static constexpr float kShaderMsgErrDuration = 10.0f;
    int gameTraceFramesRemaining = 12;

    SetStartupStage(70);
    while (window.PumpMessages()) {
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
      if (appMode == AppMode::Game && gameTimeAuto) {
        gameTimeOfDayHours =
            std::fmod(gameTimeOfDayHours + dt * gameHoursPerSecond, 24.0f);
      }
      const float gameDaylightT = DaylightTFromHour(gameTimeOfDayHours);
      const float gameSkyExposure =
          LerpFloat(0.01f, 0.25f, gameDaylightT); // Sky exposure linear from 0.01 to 0.25
      const float gamePostExposure =
          LerpFloat(0.1f, 1.0f, gameDaylightT); // Post exposure linear from 0.1 to 1.0
      const GameTimeLightingProfile gameLighting =
          BuildGameTimeLighting(gameTimeOfDayHours);
      constexpr float kGameIblIntensity = 0.7f;

      auto &input = window.GetInput();
      input.PollGamepad();

      // ---- F5: シーンプレイモードのトグル（ゲームロジック未実装のためエディタ⇔シーンプレイのみ） ----
      {
        const bool f5Now = input.IsKeyDown(VK_F5);
        if (f5Now && !prevF5) {
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
      if (appMode == AppMode::Editor && !scenePlayMode &&
          sceneEditor.ConsumeScenePlayRequest()) {
        sceneSnapshot = editorScene.SerializeToString();
        scenePlayMode = true;
      }

      // ---- F6: toggle Grid Editor panel (Phase 5) ----
      {
        static bool prevF6 = false;
        const bool f6Now = input.IsKeyDown(VK_F6);
        if (f6Now && !prevF6 && appMode == AppMode::Editor)
          sceneEditor.ToggleGridEditor();
        prevF6 = f6Now;
      }

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

      // ---- 設定パネルのトグル（ImGui がキーボードを掴んでいない時のみ ESC で開閉） ----
      if (!imgui.WantCaptureKeyboard()) {
        const bool escNow = input.IsKeyDown(VK_ESCAPE);
        if (escNow && !prevEsc)
          showSettings = !showSettings;
        prevEsc = escNow;
      }

      imgui.BeginFrame(dt);

      const bool uiWantsMouse = imgui.WantCaptureMouse();
      const bool uiWantsKeyboard = imgui.WantCaptureKeyboard();

      // Camera input routing — mode-dependent (Phase 6).
      const bool isPlaying = appMode == AppMode::Game || appMode == AppMode::BossArena;

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
      } else if (!isPlaying) {
        // プレイ中以外でスクロールを消費する（プレイ中は将来のゲームロジック側で扱う想定）。
        float scroll = input.ConsumeScrollDelta();
        auto md = input.ConsumeMouseDelta();

        switch (cam.Mode()) {
        case CameraMode::FreeFly: {
          const bool wantMouseLook = !uiWantsMouse && input.IsKeyDown(VK_RBUTTON);
          if (wantMouseLook)
            cam.AddYawPitch(md.dx * cam.LookSpeed(), -md.dy * cam.LookSpeed());
          cam.Update(dt, input, wantMouseLook);
          if (!uiWantsMouse)
            cam.ApplyScrollZoom(scroll);
          break;
        }
        case CameraMode::Orbit: {
          const bool wantOrbit = !uiWantsMouse && input.IsKeyDown(VK_RBUTTON);
          if (wantOrbit) {
            cam.SetOrbitAngles(
                cam.OrbitYaw() + md.dx * cam.LookSpeed(),
                cam.OrbitPitch() - md.dy * cam.LookSpeed());
          }
          cam.UpdateOrbit(dt, input, wantOrbit);
          if (!uiWantsMouse)
            cam.ApplyOrbitScrollZoom(scroll);
          break;
        }
        case CameraMode::GameTopDown: {
          cam.UpdateGameTopDown(dt, input);
          // Scroll adjusts height.
          if (!uiWantsMouse && scroll != 0.0f) {
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
      if (appMode == AppMode::Editor && !uiWantsMouse &&
          !sceneEditor.GetGizmo().IsActive()) {
        bool lbNow = input.IsKeyDown(VK_LBUTTON);

        if (sceneEditor.IsGridEditorOpen()) {
          // Phase 5B: viewport tile picking + painting.
          // Phase 5C: also try tower picking on click.
          POINT cursorPos;
          GetCursorPos(&cursorPos);
          ScreenToClient(window.Handle(), &cursorPos);
          if (cursorPos.x >= 0 &&
              cursorPos.x < static_cast<LONG>(window.Width()) &&
              cursorPos.y >= 0 &&
              cursorPos.y < static_cast<LONG>(window.Height())) {
            if (lbNow && !prevLButton) {
              // Try tower pick first (Phase 5C).
              int towerIdx = sceneEditor.ViewportPickTower(
                  editStage, cursorPos.x, cursorPos.y,
                  static_cast<int>(window.Width()),
                  static_cast<int>(window.Height()), cam.View(), cam.Proj());
              if (towerIdx >= 0) {
                sceneEditor.SelectTower(towerIdx);
              }
            }
            if (lbNow) {
              sceneEditor.HandleViewportTilePaint(
                  editStage, cursorPos.x, cursorPos.y,
                  static_cast<int>(window.Width()),
                  static_cast<int>(window.Height()), cam.View(), cam.Proj());
            }
          }
          if (!lbNow && prevLButton) {
            sceneEditor.FinalizeViewportPaintStroke(editStage);
          }
        } else {
          // Normal entity picking (edge-detect only).
          if (lbNow && !prevLButton) {
            POINT cursorPos;
            GetCursorPos(&cursorPos);
            ScreenToClient(window.Handle(), &cursorPos);
            if (cursorPos.x >= 0 &&
                cursorPos.x < static_cast<LONG>(window.Width()) &&
                cursorPos.y >= 0 &&
                cursorPos.y < static_cast<LONG>(window.Height())) {
              sceneEditor.HandleMousePick(
                  editorScene, cursorPos.x, cursorPos.y,
                  static_cast<int>(window.Width()),
                  static_cast<int>(window.Height()), cam.View(), cam.Proj());
            }
          }
        }
        prevLButton = lbNow;
      } else {
        prevLButton = input.IsKeyDown(VK_LBUTTON);
      }

      // Update particle emitters (demo only — not during gameplay).
      if (particlesEnabled && !isPlaying) {
        // Fire emitter follows cursor in world space.
        if (fireEnabled) {
          POINT cursorPos;
          GetCursorPos(&cursorPos);
          ScreenToClient(window.Handle(), &cursorPos);

          if (cursorPos.x >= 0 &&
              cursorPos.x < static_cast<LONG>(window.Width()) &&
              cursorPos.y >= 0 &&
              cursorPos.y < static_cast<LONG>(window.Height())) {
            DirectX::XMVECTOR worldPos = ScreenToWorld(
                cursorPos.x, cursorPos.y, window.Width(), window.Height(),
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

      if ((appMode == AppMode::Game || appMode == AppMode::BossArena) &&
          !uiWantsKeyboard && !gameFreeCameraEnabled) {
        bool inBossArena = appMode == AppMode::BossArena;
        const bool bossPhoneActive =
            inBossArena && bossArenaScene.IsPhoneOverlayActive();
        if (bossPhoneActive) {
          playerPreview.Update(dt);
        } else {
          playerPreview.Update(
              dt, input,
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
          if (!inBossArena &&
              overworldScene.IsPlayerInsideBossWarp(
                  playerPreview.Position())) {
            TraceAppEvent("overworld warp: boss arena");
            appMode = AppMode::BossArena;
            inBossArena = true;
            bossArenaScene.Reset(playerPreview);
            gameCameraPosition = {0.0f, 4.0f, -20.0f};
          }
        }
        if (inBossArena)
          bossArenaScene.Update(dt, input, playerPreview);
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
      } else if (appMode == AppMode::Game || appMode == AppMode::BossArena ||
                 appMode == AppMode::Editor) {
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
        ss << L"VILLIEN [Debug] | FPS: " << fpsValue << L" | Cam: (" << p.x
           << L", " << p.y << L", " << p.z << L")";
        window.SetTitle(ss.str());
#else
        window.SetTitle(L"VILLIEN");
#endif
      }

      if (appMode == AppMode::Title) {
        switch (titleScreen.Draw(static_cast<int>(window.Width()),
                                 static_cast<int>(window.Height()))) {
        case TitleScreen::Action::Start:
          TraceAppEvent("title action: start");
          appMode = AppMode::Game;
          gameRuntimeSeconds = 0.0f;
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

      if (appMode == AppMode::Game || appMode == AppMode::BossArena) {
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
        ImGui::Text("Sun Color: %.2f, %.2f, %.2f",
                    gameLighting.sunColor.x, gameLighting.sunColor.y,
                    gameLighting.sunColor.z);
        ImGui::Text("Sun Intensity: %.2f", gameLighting.sunIntensity);
        ImGui::Text("IBL Intensity: %.2f", kGameIblIntensity);
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

      if (appMode == AppMode::Editor) {   
        imgui.DrawDebugWindow(cam, fpsValue, dt);
        ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
        ImGui::Begin("Sky");
        ImGui::SliderFloat("Exposure", &skyExposure, 0.01f, 8.0f, "%.2f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::End();

        playerPreview.DrawDebugUi();

        ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
        ImGui::Begin("Particles");
        ImGui::Checkbox("Enable All", &particlesEnabled);
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Fire (cursor)", ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::Checkbox("Fire Enable", &fireEnabled);
          ImGui::SliderFloat("Cursor depth", &particleDepth, 1.0f, 30.0f, "%.1f");
          ImGui::Text("Alive: %zu", fireEmitter.GetCount());
          if (ImGui::Button(fireEmitter.isEmmit() ? "Stop Fire" : "Start Fire")) {
            fireEmitter.Emmit(!fireEmitter.isEmmit());
          }
        }

        if (ImGui::CollapsingHeader("Smoke", ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::Checkbox("Smoke Enable", &smokeEnabled);
          ImGui::Text("Alive: %zu", smokeEmitter.GetCount());
          if (ImGui::Button(smokeEmitter.isEmmit() ? "Stop Smoke" : "Start Smoke")) {
            smokeEmitter.Emmit(!smokeEmitter.isEmmit());
          }
          static float smokeX = -3.0f, smokeY = 0.0f, smokeZ = 3.0f;
          bool smokePosDirty = false;
          smokePosDirty |= ImGui::SliderFloat("Smoke X", &smokeX, -20.0f, 20.0f, "%.1f");
          smokePosDirty |= ImGui::SliderFloat("Smoke Y", &smokeY, -5.0f, 20.0f, "%.1f");
          smokePosDirty |= ImGui::SliderFloat("Smoke Z", &smokeZ, -20.0f, 20.0f, "%.1f");
          if (smokePosDirty)
            smokeEmitter.SetPosition(DirectX::XMVectorSet(smokeX, smokeY, smokeZ, 0.0f));
        }

        if (ImGui::CollapsingHeader("Sparks", ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::Checkbox("Spark Enable", &sparkEnabled);
          ImGui::Text("Alive: %zu", sparkEmitter.GetCount());
          if (ImGui::Button(sparkEmitter.isEmmit() ? "Stop Sparks" : "Start Sparks")) {
            sparkEmitter.Emmit(!sparkEmitter.isEmmit());
          }
          static float sparkX = 3.0f, sparkY = 0.0f, sparkZ = 3.0f;
          bool sparkPosDirty = false;
          sparkPosDirty |= ImGui::SliderFloat("Spark X", &sparkX, -20.0f, 20.0f, "%.1f");
          sparkPosDirty |= ImGui::SliderFloat("Spark Y", &sparkY, -5.0f, 20.0f, "%.1f");
          sparkPosDirty |= ImGui::SliderFloat("Spark Z", &sparkZ, -20.0f, 20.0f, "%.1f");
          if (sparkPosDirty)
            sparkEmitter.SetPosition(DirectX::XMVectorSet(sparkX, sparkY, sparkZ, 0.0f));
        }

        ImGui::End();
      }

      // SSAO, Post Processing, and Cascaded Shadows panels moved to SceneEditor (Phase 4).
      if (appMode == AppMode::Game) {
        overworldCollisionColliders =
            overworldScene.BuildCollisionColliders(overworldCollisionShapes);
      }

      // ---- Compute CSM cascade splits + per-cascade light VP ----
      using namespace DirectX;
      const auto &shadowCfg = editorScene.ShadowSettings();
      const XMFLOAT3 activeSunDirection =
          (appMode == AppMode::Game || appMode == AppMode::BossArena)
              ? gameLighting.sunDirection
              : editorScene.LightSettings().lightDir;
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
      frame.gameTime = (appMode == AppMode::Game || appMode == AppMode::BossArena) ? gameRuntimeSeconds : t;
      frame.waterWaveParams = {waterWaveHeight, waterWaveSpeed,
                               waterWaveFrequency, 0.0f};
      frame.wetSurfaceParams = {wetSurfaceStrength, wetSurfaceDrySeconds,
                                wetSurfaceImpactRadius,
                                wetSurfaceCycleSeconds};
      frame.puddleParams = {puddleStrength, puddleBuildSeconds, puddleRadius,
                            puddleRippleStrength};
      frame.puddleVisualParams = {puddleClarity, puddleTint, 1.0f, 0.0f};
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
        playerPreview.BuildFrame(frame);
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
      } else if (appMode == AppMode::BossArena) {
        frame.gridEnabled = false;
        frame.waterWaveParams = {0.0f, 1.0f, 1.0f, 0.0f};
        frame.ssrReflectionParams = {1.35f, 28.0f, 0.16f, 0.20f};
        bossArenaScene.BuildFrame(frame);
        const DirectX::XMFLOAT3 gameplayPlayerPos = playerPreview.Position();
        playerPreview.SetPosition(
            bossArenaScene.VisualPositionForGameplayPosition(gameplayPlayerPos));
        playerPreview.BuildFrame(frame);
        playerPreview.SetPosition(gameplayPlayerPos);
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
        // Scene play mode (Phase 8): build frame data but skip editor UI.
        editorScene.BuildFrameData(frame);
      } else {
        sceneEditor.DrawUI(editorScene, dx, cam.View(), cam.Proj(), &iblEnabled,
                           &editStage, &cam);
        editorScene.BuildFrameData(frame);
        sceneEditor.BuildHighlightItems(editorScene, frame);

        // Phase 5B: inject 3D grid tiles into FrameData when grid editor is open.
        if (sceneEditor.IsGridEditorOpen()) {
          sceneEditor.BuildStageViewportItems(editStage, frame);
        }
      }

      // Mode indicator overlay.
      if (appMode == AppMode::Editor) {
        ImGui::SetNextWindowPos(ImVec2(10, static_cast<float>(window.Height()) - 30.0f));
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
      if (traceGameFrame)
        TraceAppEvent("pass: UI");
      uiPass.Execute(dx, frame);
      if (traceGameFrame)
        TraceAppEvent("pass: EndFrame");
      dx.EndFrame();
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

    // ---- Shutdown (reverse init order) ----
    dx.WaitForGpu(); // Flush GPU before releasing any resources
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
  return 0;
}
