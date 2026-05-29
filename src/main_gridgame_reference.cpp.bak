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
#include "gridgame/GridGame.h"
#include "gridgame/StageData.h"
#include "engine/Scene.h"
#include "engine/SceneEditor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <dbghelp.h>
#include <fstream>
#include <sstream>

#include <imgui.h>

static volatile LONG g_startupStage = 0;

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
  case 50:
    return "50: GridGame::Init (begin)";
  case 60:
    return "60: GridGame::Init (done)";
  case 70:
    return "70: main loop";
  default:
    return "unknown";
  }
}

static void SetStartupStage(LONG s) { InterlockedExchange(&g_startupStage, s); }

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
  DxContext *dx = nullptr;
  Camera *cam = nullptr;
};

static void OnResize(uint32_t w, uint32_t h, void *userData) {
  auto *ctx = reinterpret_cast<AppResizeContext *>(userData);
  if (ctx && ctx->dx)
    ctx->dx->Resize(w, h);
  if (ctx && ctx->cam && h != 0) {
    const float aspect = static_cast<float>(w) / static_cast<float>(h);
    // Preserve current FOV/near/far when resizing (Phase 6).
    ctx->cam->SetLens(ctx->cam->FovY(), aspect, ctx->cam->NearZ(), ctx->cam->FarZ());
  }
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

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int nCmdShow) {
  try {
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);

    Win32Window window;
    SetStartupStage(10);
    window.Create(L"DX12 Tutorial 12", 1920, 1080);

    DxContext dx;
    SetStartupStage(20);
    dx.Initialize(window.Handle(), window.Width(), window.Height(), true);

    Camera cam;
    cam.SetPosition(0.0f, 1.5f, -4.0f);
    cam.SetYawPitch(0.0f, 0.0f);
    cam.SetLens(DirectX::XM_PIDIV4,
                static_cast<float>(window.Width()) /
                    static_cast<float>(window.Height()),
                0.1f, 1000.0f);

    AppResizeContext resizeCtx{&dx, &cam};
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

    // ---- Initialize Grid Gauntlet ----
    GridGame gridGame;
    SetStartupStage(50);
    gridGame.Init(cam, dx);
    SetStartupStage(60);

    // Wire IBL descriptors to the mesh renderer + DxContext (for deferred lighting).
    dx.GetMeshRenderer().SetIBLDescriptors(iblGenerator.IBLTableGpuBase());
    dx.SetIblTableGpu(iblGenerator.IBLTableGpuBase());

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

    // IBL (Phase 10.2)
    bool iblEnabled = true;

    // Settings UI (Phase 12.6)
    bool showSettings = false;
    bool prevEsc = false;

    // ---- Editor/Game mode toggle (Milestone 4 Phase 0) ----
    enum class AppMode { Editor, Game };
    AppMode appMode = AppMode::Editor;
    Scene editorScene;
    SceneEditor sceneEditor;
    StageData editStage; // Grid editor stage data (Phase 5).
    editStage.Clear();   // Initialize with default grid.
    sceneEditor.InitEditorMeshes(dx); // Phase 5B: create viewport tile meshes.
    bool prevF5 = false;
    bool prevLButton = false; // for edge-detection of left-click (mouse pick)
    bool playTesting = false; // Phase 5C: true when play-testing from grid editor

    // Scene play mode (Phase 8).
    bool scenePlayMode = false;
    std::string sceneSnapshot;

    // Shader hot-reload (Phase 8).
    bool prevF9 = false;
    std::string g_shaderReloadErrors;
    float g_shaderReloadTimer = 0.0f;
    static constexpr float kShaderMsgOkDuration = 3.0f;
    static constexpr float kShaderMsgErrDuration = 10.0f;

    SetStartupStage(70);
    while (window.PumpMessages() && !(appMode == AppMode::Game && gridGame.WantsQuit())) {
      auto now = clock::now();
      float dt = std::chrono::duration<float>(now - prev).count();
      prev = now;

      auto t = std::chrono::duration<float>(now - start).count();
      float r = 0.1f + 0.1f * (0.5f + 0.5f * sinf(t));
      float g = 0.1f + 0.1f * (0.5f + 0.5f * sinf(t * 1.7f));
      float b = 0.2f + 0.2f * (0.5f + 0.5f * sinf(t * 0.9f));

      auto &input = window.GetInput();
      input.PollGamepad();

      // ---- F5: toggle Editor / Game / Scene-Play mode (Milestone 4 + Phase 8) ----
      {
        const bool f5Now = input.IsKeyDown(VK_F5);
        if (f5Now && !prevF5) {
          if (playTesting) {
            // Branch 1: stop grid play-test.
            gridGame.ResetToMainMenu();
            appMode = AppMode::Editor;
            playTesting = false;
          } else if (scenePlayMode) {
            // Branch 2: stop scene play, restore snapshot.
            dx.WaitForGpu();
            editorScene.DeserializeFromString(sceneSnapshot, dx);
            sceneSnapshot.clear();
            scenePlayMode = false;
          } else if (appMode == AppMode::Editor && sceneEditor.IsGridEditorOpen()) {
            // Branch 3: start grid play-test.
            gridGame.LoadFromStageData(editStage);
            appMode = AppMode::Game;
            playTesting = true;
          } else if (appMode == AppMode::Editor) {
            // Branch 4: start scene play mode.
            sceneSnapshot = editorScene.SerializeToString();
            scenePlayMode = true;
          } else {
            // Game mode (non-play-test): toggle back to editor.
            appMode = AppMode::Editor;
          }
        }
        prevF5 = f5Now;
      }

      // Check play-test button request from grid editor panel (Phase 5C).
      if (appMode == AppMode::Editor && sceneEditor.ConsumePlayTestRequest()) {
        gridGame.LoadFromStageData(editStage);
        appMode = AppMode::Game;
        playTesting = true;
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
          g_shaderReloadErrors += dx.GetParticleRenderer().ReloadShaders(dx);
          g_shaderReloadTimer = g_shaderReloadErrors.empty()
                                    ? kShaderMsgOkDuration
                                    : kShaderMsgErrDuration;
        }
        prevF9 = f9Now;
      }

      // ---- Settings toggle — only when in game mode on main menu (Phase 12.6) ----
      if (appMode == AppMode::Game && gridGame.GetState() == GridGameState::MainMenu) {
        const bool escNow = input.IsKeyDown(VK_ESCAPE);
        if (escNow && !prevEsc)
          showSettings = !showSettings;
        prevEsc = escNow;
      }

      imgui.BeginFrame(dt);

      const bool uiWantsMouse = imgui.WantCaptureMouse();

      // Camera input routing — mode-dependent (Phase 6).
      const bool isPlaying = (appMode == AppMode::Game &&
          (gridGame.GetState() == GridGameState::Playing || gridGame.GetState() == GridGameState::Intro));

      // Consume scroll here only when not playing — GridGame handles its own scroll.
      if (!isPlaying) {
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
      if (particlesEnabled && !isPlaying &&
          (appMode == AppMode::Editor || (gridGame.GetState() != GridGameState::Paused))) {
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

      // FPS + debug title update.
      fpsTimer += dt;
      fpsFrames += 1;
      if (fpsTimer >= 1.0f) {
        fpsValue = fpsFrames / fpsTimer;
        fpsTimer = 0.0f;
        fpsFrames = 0;

        auto p = cam.GetPosition();
        std::wstringstream ss;
        ss.setf(std::ios::fixed);
        ss.precision(2);
        ss << L"DX12 Tutorial 12 | FPS: " << fpsValue << L" | Cam: (" << p.x
           << L", " << p.y << L", " << p.z << L")";
        window.SetTitle(ss.str());
      }

      // ---- Settings window (Phase 12.6) ----
      if (showSettings) {
        ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_Once);
        ImGui::Begin("Settings", &showSettings);

        ImGui::Text("Display Mode");
        ImGui::Separator();

        const bool isFS = window.IsFullscreen();
        const uint32_t curW = window.Width();
        const uint32_t curH = window.Height();

        if (ImGui::RadioButton("Fullscreen", isFS)) {
          if (!isFS) window.SetFullscreen(true);
        }
        if (ImGui::RadioButton("1080p (Windowed)", !isFS && curW == 1920 && curH == 1080)) {
          window.SetWindowedResolution(1920, 1080);
        }
        if (ImGui::RadioButton("720p (Windowed)", !isFS && curW == 1280 && curH == 720)) {
          window.SetWindowedResolution(1280, 720);
        }

        ImGui::End();
      }

      // ---- ImGui debug windows ----
      imgui.DrawDebugWindow(cam, fpsValue, dt);
      ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
      ImGui::Begin("Sky");
      ImGui::SliderFloat("Exposure", &skyExposure, 0.01f, 8.0f, "%.2f",
                         ImGuiSliderFlags_Logarithmic);
      ImGui::End();

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

      // SSAO, Post Processing, and Cascaded Shadows panels moved to SceneEditor (Phase 4).

      // ---- Compute CSM cascade splits + per-cascade light VP ----
      using namespace DirectX;
      const auto &shadowCfg = editorScene.ShadowSettings();
      const XMVECTOR raysDir =
          XMVector3Normalize(XMLoadFloat3(&editorScene.LightSettings().lightDir));
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
      cam.EnableJitter(editorScene.PostProcessSettings().taaEnabled);
      cam.AdvanceJitter(dx.Width(), dx.Height());

      // ---- Build FrameData (Phase 8) ----
      // Shadow, SSAO, and post-process fields are set by Scene::BuildFrameData().
      FrameData frame{};
      frame.view = cam.View();
      frame.proj = cam.Proj(); // includes jitter when TAA is enabled
      frame.cameraPos = camPosF;
      frame.cascadeCount = cascadeCount;
      frame.cascadeLightViewProj = cascadeVP;
      frame.cascadeSplitDistances = cascadeSplitDists;
      frame.skyExposure = skyExposure;
      frame.clearColor[0] = r;
      frame.clearColor[1] = g;
      frame.clearColor[2] = b;
      frame.clearColor[3] = 1.0f;
      frame.particlesEnabled = true;
      // Demo emitters only outside gameplay
      if (appMode == AppMode::Editor || (gridGame.GetState() != GridGameState::Playing &&
          gridGame.GetState() != GridGameState::Paused &&
          gridGame.GetState() != GridGameState::Intro)) {
        if (particlesEnabled && fireEnabled)
          frame.emitters.push_back(&fireEmitter);
        if (particlesEnabled && smokeEnabled)
          frame.emitters.push_back(&smokeEmitter);
        if (particlesEnabled && sparkEnabled)
          frame.emitters.push_back(&sparkEmitter);
      }

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

      // ---- Game / Editor layer update (Milestone 4) ----
      if (appMode == AppMode::Game) {
        gridGame.Update(dt, input, window, frame);

        // Phase 5C: return to editor when play-test game reaches MainMenu.
        if (playTesting && gridGame.GetState() == GridGameState::MainMenu) {
          appMode = AppMode::Editor;
          playTesting = false;
        }
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
      {
        ImGui::SetNextWindowPos(ImVec2(10, static_cast<float>(window.Height()) - 30.0f));
        ImGui::Begin("##ModeIndicator", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoBackground);
        if (playTesting) {
          ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                             "[PLAY-TEST] ESC=Pause  F5=Stop");
        } else if (scenePlayMode) {
          ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                             "[SCENE PLAY] F5=Stop");
        } else if (appMode == AppMode::Editor && sceneEditor.IsGridEditorOpen()) {
          ImGui::TextColored(ImVec4(0.2f,0.8f,1,1), "[EDITOR] F5=Play Test  F6=Grid Editor (ON)");
        } else {
          ImGui::TextColored(
              appMode == AppMode::Editor ? ImVec4(0,1,0.5f,1) : ImVec4(1,0.5f,0,1),
              appMode == AppMode::Editor ? "[EDITOR] F5=Game  F6=Grid Editor" : "[GAME] F5=Editor");
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
      dx.BeginFrame();
      shadowPass.Execute(dx, frame);
      skyPass.Execute(dx, frame);
      gbufferPass.Execute(dx, frame);
      deferredLightingPass.Execute(dx, frame);
      gridPass.Execute(dx, frame);
      transparentPass.Execute(dx, frame);
      highlightPass.Execute(dx, frame);
      ssaoPass.Execute(dx, frame);
      velocityGenPass.Execute(dx, frame);  // moved before TAA (Phase 10.4)
      taaPass.Execute(dx, frame);          // TAA resolve (Phase 10.4)
      bloomPass.Execute(dx, frame);
      tonemapPass.Execute(dx, frame);
      dofPass.Execute(dx, frame);
      motionBlurPass.Execute(dx, frame);
      fxaaPass.Execute(dx, frame);
      uiPass.Execute(dx, frame);
      dx.EndFrame();

      // Swap TAA ping-pong so next frame reads our output as history.
      if (editorScene.PostProcessSettings().taaEnabled) {
        dx.SwapTaaBuffers();
      }

      // Store current VP as "previous" for next frame's motion blur.
      cam.UpdatePrevViewProj();
    }

    // ---- Shutdown (reverse init order) ----
    dx.WaitForGpu(); // Flush GPU before releasing any resources
    gridGame.Shutdown();
    ssaoRenderer.Reset();
    postProcess.Reset();
    iblGenerator.Reset();
    skyRenderer.Reset();
    gridRenderer.Reset();
    imgui.Shutdown(window);
    dx.Shutdown();
  } catch (const std::exception &e) {
    MessageBoxA(nullptr, e.what(), "Error",
                MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SYSTEMMODAL);
    return -1;
  } catch (...) {
    MessageBoxA(nullptr, "Unknown error occurred", "Error",
                MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SYSTEMMODAL);
    return -1;
  }
  return 0;
}
