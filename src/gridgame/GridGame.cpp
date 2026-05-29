// ======================================
// File: GridGame.cpp
// Purpose: Grid Gauntlet game manager implementation (Phase 1: infrastructure).
//          Creates procedural meshes, manages game state, builds FrameData.
// ======================================

#include "GridGame.h"
#include "GridMaterials.h"
#include "GridParticles.h"
#include "StageData.h"

#include "../DxContext.h"
#include "../GltfLoader.h"
#include "../MeshRenderer.h"
#include "../ProceduralMesh.h"

#include <algorithm>
#include <cstdlib>
#include <imgui.h>

using namespace DirectX;

// ---- Phase 7: Neon UI helpers ----

static void PushNeonTheme() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 8));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 8));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);

  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.03f, 0.08f, 0.88f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.6f, 0.8f, 0.5f));
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.25f, 0.35f, 0.9f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.55f, 0.75f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.8f, 1.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.9f, 0.95f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.05f, 0.08f, 0.15f, 0.8f));
}

static void PopNeonTheme() {
  ImGui::PopStyleColor(7);
  ImGui::PopStyleVar(5);
}

static void DrawDimOverlay(float alpha = 0.55f) {
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
  ImGui::Begin("##DimOverlay", nullptr,
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
  ImGui::GetWindowDrawList()->AddRectFilled(
      ImVec2(0, 0), ImGui::GetIO().DisplaySize,
      ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.0f, 0.05f, alpha)));
  ImGui::End();
}

static bool CenteredButton(const char *label, float btnWidth, float winWidth) {
  ImGui::SetCursorPosX((winWidth - btnWidth) * 0.5f);
  return ImGui::Button(label, ImVec2(btnWidth, 40));
}

static char CalcRating(float time, int moves, float timeLimit, int parMoves) {
  if (timeLimit <= 0.0f && parMoves <= 0) return 'B';

  int score = 0;
  if (timeLimit > 0.0f) {
    float ratio = time / timeLimit;
    if (ratio <= 0.5f) score += 2;
    else if (ratio <= 0.75f) score += 1;
  } else {
    score += 1;
  }

  if (parMoves > 0) {
    float ratio = static_cast<float>(moves) / static_cast<float>(parMoves);
    if (ratio <= 1.0f) score += 2;
    else if (ratio <= 1.5f) score += 1;
  } else {
    score += 1;
  }

  if (score >= 4) return 'S';
  if (score >= 3) return 'A';
  if (score >= 2) return 'B';
  return 'C';
}

static ImVec4 RatingColor(char grade) {
  switch (grade) {
  case 'S': return ImVec4(1.0f, 0.85f, 0.0f, 1.0f);
  case 'A': return ImVec4(0.0f, 1.0f, 0.4f, 1.0f);
  case 'B': return ImVec4(0.0f, 0.8f, 1.0f, 1.0f);
  default:  return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
  }
}

// ---- Init / Shutdown ----

void GridGame::Init(Camera &cam, DxContext &dx) {
  m_camera = &cam;
  m_dx = &dx;

  // Create procedural meshes and upload to GPU.
  auto plane = ProceduralMesh::CreatePlane(1.0f, 1.0f);
  auto cube = ProceduralMesh::CreateCube(1.0f);
  auto cone = ProceduralMesh::CreateCone(0.4f, 1.2f, 12);

  // Tile meshes (planes with different materials).

  // Floor: load PBR textures (Ground037 set from ambientCG).
  {
    LoadedImage floorColor, floorNorm, floorRough, floorAO, floorDisp;
    MaterialImages floorImages;
    if (LoadImageFile("Assets/textures/Floor_png/Ground037_2K-PNG_Color.png", floorColor))
      floorImages.baseColor = &floorColor;
    if (LoadImageFile("Assets/textures/Floor_png/Ground037_2K-PNG_NormalDX.png", floorNorm))
      floorImages.normal = &floorNorm;
    if (LoadImageFile("Assets/textures/Floor_png/Ground037_2K-PNG_AmbientOcclusion.png", floorAO))
      floorImages.ao = &floorAO;
    if (LoadImageFile("Assets/textures/Floor_png/Ground037_2K-PNG_Displacement.png", floorDisp))
      floorImages.height = &floorDisp;

    // Combine roughness (no metalness for ground) into glTF-style metalRough:
    // G=roughness, B=metallic(0).
    LoadedImage floorMetalRough;
    if (LoadImageFile("Assets/textures/Floor_png/Ground037_2K-PNG_Roughness.png", floorRough)) {
      floorMetalRough = floorRough;
      for (size_t p = 0; p < floorMetalRough.pixels.size(); p += 4) {
        uint8_t r = floorRough.pixels[p];
        floorMetalRough.pixels[p + 0] = 0;
        floorMetalRough.pixels[p + 1] = r;   // G = roughness
        floorMetalRough.pixels[p + 2] = 0;   // B = metallic (none)
        floorMetalRough.pixels[p + 3] = 255;
      }
      floorImages.metalRough = &floorMetalRough;
    }

    Material floorMat = MakeFloorMaterial();
    floorMat.baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
    floorMat.uvTiling = {2.0f, 2.0f};
    m_meshIds.floor = dx.CreateMeshResources(plane, floorImages, floorMat);
  }

  // Wall: load PBR textures (Metal046B set from ambientCG).
  {
    LoadedImage wallColor, wallNorm, wallRough, wallMetal, wallDisp;
    MaterialImages wallImages;
    if (LoadImageFile("Assets/textures/Wall_png/Metal046B_2K-PNG_Color.png", wallColor))
      wallImages.baseColor = &wallColor;
    if (LoadImageFile("Assets/textures/Wall_png/Metal046B_2K-PNG_NormalDX.png", wallNorm))
      wallImages.normal = &wallNorm;
    if (LoadImageFile("Assets/textures/Wall_png/Metal046B_2K-PNG_Displacement.png", wallDisp))
      wallImages.height = &wallDisp;

    // Combine separate roughness + metalness into glTF-style metalRough.
    LoadedImage wallMetalRough;
    bool hasRough = LoadImageFile("Assets/textures/Wall_png/Metal046B_2K-PNG_Roughness.png", wallRough);
    bool hasMetal = LoadImageFile("Assets/textures/Wall_png/Metal046B_2K-PNG_Metalness.png", wallMetal);
    if (hasRough || hasMetal) {
      wallMetalRough = hasRough ? wallRough : wallMetal;
      for (size_t p = 0; p < wallMetalRough.pixels.size(); p += 4) {
        uint8_t r = hasRough ? wallRough.pixels[p] : 128;
        uint8_t m = hasMetal ? wallMetal.pixels[p] : 0;
        wallMetalRough.pixels[p + 0] = 0;
        wallMetalRough.pixels[p + 1] = r;   // G = roughness
        wallMetalRough.pixels[p + 2] = m;   // B = metallic
        wallMetalRough.pixels[p + 3] = 255;
      }
      wallImages.metalRough = &wallMetalRough;
    }

    Material wallMat = MakeWallMaterial();
    wallMat.baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
    m_meshIds.wall = dx.CreateMeshResources(cube, wallImages, wallMat);
  }
  m_meshIds.wallDestructible =
      dx.CreateMeshResources(cube, {}, MakeDestructibleWallMaterial());
  m_meshIds.fire = dx.CreateMeshResources(plane, {}, MakeFireMaterial());
  m_meshIds.lightning =
      dx.CreateMeshResources(plane, {}, MakeLightningMaterial());
  m_meshIds.spike = dx.CreateMeshResources(plane, {}, MakeSpikeMaterial());
  m_meshIds.ice = dx.CreateMeshResources(plane, {}, MakeIceMaterial());
  m_meshIds.crumble = dx.CreateMeshResources(plane, {}, MakeCrumbleMaterial());
  m_meshIds.start = dx.CreateMeshResources(plane, {}, MakeStartMaterial());
  m_meshIds.goal = dx.CreateMeshResources(plane, {}, MakeGoalMaterial());
  m_meshIds.telegraph =
      dx.CreateMeshResources(plane, {}, MakeTelegraphMaterial());

  // Phase 3A: 3-beat telegraph intensity meshes.
  m_telegraphWarn1MeshId = dx.CreateMeshResources(plane, {}, MakeTelegraphWarn1Material());
  m_telegraphWarn2MeshId = dx.CreateMeshResources(plane, {}, MakeTelegraphWarn2Material());
  m_telegraphFireMeshId  = dx.CreateMeshResources(plane, {}, MakeTelegraphFireMaterial());

  // Phase 3B: beam laser mesh (stretched cube).
  m_beamMeshId = dx.CreateMeshResources(cube, {}, MakeBeamMaterial());

  // Hazard beam mesh (purple/magenta, distinct from tower beams).
  m_hazardBeamMeshId = dx.CreateMeshResources(cube, {}, MakeHazardBeamMaterial());

  // Game object meshes.
  // Load VRM character model for player (VRoid Studio export).
  {
    GltfLoader playerLoader;
    if (playerLoader.LoadModel("Assets/models/MyFirstChar.vrm")) {
      MaterialImages playerImages = playerLoader.GetMaterialImages();
      Material playerMat = playerLoader.GetMaterial();
      const auto &loadedMesh = playerLoader.GetMesh();
      m_playerMeshId = dx.CreateMeshResources(loadedMesh, playerImages, playerMat);

      // Store skeleton for animation.
      if (loadedMesh.hasSkeleton) {
        m_playerSkeleton = loadedMesh.skeleton;
        m_playerHasSkeleton = true;
        // Set initial bind pose.
        BonePalette bindPose;
        ComputeBindPose(m_playerSkeleton, bindPose);
        dx.GetMeshRenderer().SetBonePalette(m_playerMeshId, bindPose);
        OutputDebugStringA("[GridGame] Player skeleton extracted, bone count: ");
        char buf[32];
        sprintf_s(buf, "%d\n", bindPose.boneCount);
        OutputDebugStringA(buf);

        // Dump skeleton debug info to file.
        {
          FILE *logFile = nullptr;
          fopen_s(&logFile, "anim_debug.txt", "w");
          if (logFile) {
            fprintf(logFile, "=== VRoid Skeleton ===\n");
            fprintf(logFile, "Bone count: %zu\n\n", m_playerSkeleton.bones.size());
            for (size_t bi = 0; bi < m_playerSkeleton.bones.size(); ++bi) {
              const auto &b = m_playerSkeleton.bones[bi];
              fprintf(logFile, "[%3zu] %-40s parent=%d\n",
                      bi, b.name.c_str(), b.parentIndex);
            }
            fprintf(logFile, "\n");
            fclose(logFile);
          }
        }

        // Load external animation clips (Mixamo exports).
        std::vector<AnimationClip> idleClips;
        if (LoadAnimationFile("Assets/models/animations/Idle.glb",
                              m_playerSkeleton, idleClips)) {
          for (auto &c : idleClips) {
            m_playerAnimations.push_back(std::move(c));
          }
          m_idleClipIndex = 0;

          // Append animation info to debug log.
          FILE *logFile = nullptr;
          fopen_s(&logFile, "anim_debug.txt", "a");
          if (logFile) {
            fprintf(logFile, "=== Animation Clips ===\n");
            for (size_t ci = 0; ci < m_playerAnimations.size(); ++ci) {
              const auto &clip = m_playerAnimations[ci];
              fprintf(logFile, "Clip[%zu] \"%s\": duration=%.3fs, tracks=%zu\n",
                      ci, clip.name.c_str(), clip.duration, clip.tracks.size());
              for (size_t ti = 0; ti < clip.tracks.size(); ++ti) {
                const auto &tr = clip.tracks[ti];
                const char *pathStr = (tr.path == AnimTargetPath::Rotation) ? "ROT"
                    : (tr.path == AnimTargetPath::Translation) ? "POS" : "SCL";
                const char *boneName = (tr.boneIndex >= 0 &&
                    tr.boneIndex < static_cast<int>(m_playerSkeleton.bones.size()))
                    ? m_playerSkeleton.bones[tr.boneIndex].name.c_str() : "???";
                fprintf(logFile, "  Track[%2zu] bone=%3d %-35s %s keys=%zu\n",
                        ti, tr.boneIndex, boneName, pathStr, tr.keyframes.size());
              }
            }
            fclose(logFile);
          }

          // Enhanced debug: compare bind pose vs animation frame 0 for arm bones.
          {
            FILE *dbg = nullptr;
            fopen_s(&dbg, "anim_debug.txt", "a");
            if (dbg) {
              fprintf(dbg, "\n=== Bind vs Anim Frame0 (arm chain) ===\n");

              // Compute VRoid local bind T/R/S from IBMs.
              int bc = static_cast<int>(m_playerSkeleton.bones.size());
              std::vector<DirectX::XMMATRIX> gBind(bc);
              for (int i = 0; i < bc; ++i) {
                DirectX::XMMATRIX ibm = DirectX::XMLoadFloat4x4(
                    &m_playerSkeleton.bones[i].inverseBindMatrix);
                DirectX::XMVECTOR det;
                gBind[i] = DirectX::XMMatrixInverse(&det, ibm);
              }

              // Bones to inspect: root, hips->head, arms, legs.
              int inspectBones[] = {0, 1, 10, 11, 12, 17, 18,
                                    72, 73, 74, 75, 91, 92, 93, 94,
                                    110, 119, 120, 121, 122, 131, 132, 133};
              for (int bi : inspectBones) {
                if (bi >= bc) continue;
                const auto &bone = m_playerSkeleton.bones[bi];

                // Compute local bind.
                DirectX::XMMATRIX localB;
                if (bone.parentIndex < 0 || bone.parentIndex >= bc) {
                  localB = gBind[bi];
                } else {
                  DirectX::XMVECTOR det;
                  localB = gBind[bi] *
                      DirectX::XMMatrixInverse(&det, gBind[bone.parentIndex]);
                }
                DirectX::XMVECTOR bS, bR, bT;
                DirectX::XMMatrixDecompose(&bS, &bR, &bT, localB);

                DirectX::XMFLOAT4 bindR;
                DirectX::XMStoreFloat4(&bindR, bR);
                DirectX::XMFLOAT3 bindTr, bindSc;
                DirectX::XMStoreFloat3(&bindTr, bT);
                DirectX::XMStoreFloat3(&bindSc, bS);

                // Also get global bind position (world-space).
                DirectX::XMFLOAT4 gPos;
                DirectX::XMStoreFloat4(&gPos, gBind[bi].r[3]);

                fprintf(dbg, "[%3d] %-30s parent=%d\n", bi, bone.name.c_str(),
                        bone.parentIndex);
                fprintf(dbg, "  bindT=(%.4f, %.4f, %.4f)  bindS=(%.4f, %.4f, %.4f)\n",
                        bindTr.x, bindTr.y, bindTr.z,
                        bindSc.x, bindSc.y, bindSc.z);
                fprintf(dbg, "  bindR=(%.4f, %.4f, %.4f, %.4f)\n",
                        bindR.x, bindR.y, bindR.z, bindR.w);
                fprintf(dbg, "  globalPos=(%.4f, %.4f, %.4f)\n",
                        gPos.x, gPos.y, gPos.z);

                // Find animation frame 0 rotation for this bone.
                if (m_idleClipIndex >= 0) {
                  const auto &clip = m_playerAnimations[m_idleClipIndex];
                  for (const auto &tr : clip.tracks) {
                    if (tr.boneIndex == bi &&
                        tr.path == AnimTargetPath::Rotation &&
                        !tr.keyframes.empty()) {
                      const auto &kf = tr.keyframes[0];
                      fprintf(dbg, "  animR=(%.4f, %.4f, %.4f, %.4f) t=%.3f\n",
                              kf.value.x, kf.value.y, kf.value.z, kf.value.w,
                              kf.time);
                      break;
                    }
                  }
                }
              }

              // Also dump frame-0 skin matrices for key bones.
              fprintf(dbg, "\n=== Skin Matrices at t=0.033 ===\n");
              {
                BonePalette testPal;
                EvaluateAnimation(m_playerSkeleton,
                    m_playerAnimations[m_idleClipIndex], 0.033f, testPal);
                int skinBones[] = {0, 1, 72, 73, 110, 119};
                for (int si : skinBones) {
                  if (si >= bc) continue;
                  DirectX::XMFLOAT4X4 sm;
                  DirectX::XMStoreFloat4x4(&sm, testPal.matrices[si]);
                  fprintf(dbg, "[%3d] %-30s\n", si,
                      m_playerSkeleton.bones[si].name.c_str());
                  fprintf(dbg, "  |%8.4f %8.4f %8.4f %8.4f|\n",
                      sm._11, sm._12, sm._13, sm._14);
                  fprintf(dbg, "  |%8.4f %8.4f %8.4f %8.4f|\n",
                      sm._21, sm._22, sm._23, sm._24);
                  fprintf(dbg, "  |%8.4f %8.4f %8.4f %8.4f|\n",
                      sm._31, sm._32, sm._33, sm._34);
                  fprintf(dbg, "  |%8.4f %8.4f %8.4f %8.4f|\n",
                      sm._41, sm._42, sm._43, sm._44);
                }
              }
              fclose(dbg);
            }
          }

          OutputDebugStringA("[GridGame] Idle animation loaded OK\n");
        } else {
          OutputDebugStringA("[GridGame] WARNING: Failed to load Idle.glb\n");
        }

        // Load push/move animation (Mixamo export).
        std::vector<AnimationClip> pushClips;
        if (LoadAnimationFile("Assets/models/animations/Push.glb",
                              m_playerSkeleton, pushClips)) {
          m_pushClipIndex = static_cast<int>(m_playerAnimations.size());
          for (auto &c : pushClips) {
            m_playerAnimations.push_back(std::move(c));
          }
          OutputDebugStringA("[GridGame] Push animation loaded OK\n");
        } else {
          OutputDebugStringA("[GridGame] WARNING: Failed to load Push.glb\n");
        }

        // Load run animation (Mixamo export).
        std::vector<AnimationClip> runClips;
        if (LoadAnimationFile("Assets/models/animations/Run.glb",
                              m_playerSkeleton, runClips)) {
          m_runClipIndex = static_cast<int>(m_playerAnimations.size());
          for (auto &c : runClips) {
            m_playerAnimations.push_back(std::move(c));
          }
          OutputDebugStringA("[GridGame] Run animation loaded OK\n");
        } else {
          OutputDebugStringA("[GridGame] WARNING: Failed to load Run.glb\n");
        }
      }
      OutputDebugStringA("[GridGame] Player VRM model loaded OK\n");
    } else {
      OutputDebugStringA("[GridGame] FAILED to load player VRM, falling back to cube\n");
      m_playerMeshId = dx.CreateMeshResources(cube, {}, MakePlayerMaterial());
    }
  }

  // Cargo cube: load rock textures for base color, normal, and roughness.
  {
    LoadedImage cargoDiff, cargoNorm, cargoRough;
    MaterialImages cargoImages;
    if (LoadImageFile("Assets/GlTF/rock_terrain_2k.gltf/textures/rock_boulder_cracked_diff_2k.jpg", cargoDiff)) {
      cargoImages.baseColor = &cargoDiff;
      OutputDebugStringA("[GridGame] Cargo baseColor texture loaded OK\n");
    } else {
      OutputDebugStringA("[GridGame] FAILED to load cargo baseColor texture\n");
    }
    if (LoadImageFile("Assets/GlTF/rock_terrain_2k.gltf/textures/rock_boulder_cracked_nor_gl_2k.jpg", cargoNorm)) {
      cargoImages.normal = &cargoNorm;
      OutputDebugStringA("[GridGame] Cargo normal texture loaded OK\n");
    } else {
      OutputDebugStringA("[GridGame] FAILED to load cargo normal texture\n");
    }
    if (LoadImageFile("Assets/GlTF/rock_terrain_2k.gltf/textures/rock_boulder_cracked_rough_2k.jpg", cargoRough)) {
      cargoImages.metalRough = &cargoRough;
      OutputDebugStringA("[GridGame] Cargo roughness texture loaded OK\n");
    } else {
      OutputDebugStringA("[GridGame] FAILED to load cargo roughness texture\n");
    }
    m_cargoMeshId = dx.CreateMeshResources(cube, cargoImages, MakeCargoMaterial());
  }

  m_towerMeshId = dx.CreateMeshResources(cone, {}, MakeTowerMaterial());
  m_gridLineMeshId = dx.CreateMeshResources(cube, {}, MakeGridLineMaterial());

  // Phase 6: tile border glow meshes.
  m_borderOrangeMeshId = dx.CreateMeshResources(cube, {}, MakeBorderOrangeMaterial());
  m_borderCyanMeshId   = dx.CreateMeshResources(cube, {}, MakeBorderCyanMaterial());
  m_borderGreenMeshId  = dx.CreateMeshResources(cube, {}, MakeBorderGreenMaterial());
  m_borderGoldMeshId   = dx.CreateMeshResources(cube, {}, MakeBorderGoldMaterial());
  m_borderRedMeshId    = dx.CreateMeshResources(cube, {}, MakeBorderRedMaterial());
  m_trailMeshId        = dx.CreateMeshResources(plane, {}, MakeTrailMaterial());

  // Load a test stage for Phase 1.
  LoadTestStage();

  m_state = GridGameState::MainMenu;
}

void GridGame::Shutdown() {
  m_map.Reset();
  m_gameEmitters.clear();
  m_burstEmitters.clear();
  m_towers.clear();
}

// ---- Phase 8A: burst emitter pool management ----

void GridGame::UpdateBurstEmitters(float dt) {
  for (auto &em : m_burstEmitters)
    em->Update(static_cast<double>(dt));

  // Erase finished burst emitters.
  m_burstEmitters.erase(
      std::remove_if(m_burstEmitters.begin(), m_burstEmitters.end(),
                     [](const std::unique_ptr<Emitter> &em) {
                       auto *burst = dynamic_cast<BurstEmitter *>(em.get());
                       return burst && burst->IsFinished();
                     }),
      m_burstEmitters.end());
}

// ---- Hazard beam system ----

void GridGame::SpawnHazardBeams() {
  int w = m_map.Width();
  int h = m_map.Height();
  if (w <= 0 || h <= 0) return;

  for (int i = 0; i < kBeamsPerSpawn; ++i) {
    HazardBeam beam;
    // Random: 50% row, 50% column.
    beam.isRow = (rand() % 2) == 0;
    beam.index = beam.isRow ? (rand() % h) : (rand() % w);
    beam.timer = 0.0f;
    beam.damaged = false;
    m_hazardBeams.push_back(beam);
  }
}

void GridGame::UpdateHazardBeams(float dt) {
  // Spawn timer.
  m_hazardBeamSpawnTimer += dt;
  if (m_hazardBeamSpawnTimer >= kBeamSpawnInterval) {
    m_hazardBeamSpawnTimer -= kBeamSpawnInterval;
    SpawnHazardBeams();
  }

  // Update existing beams.
  for (auto &beam : m_hazardBeams) {
    beam.timer += dt;

    // Apply damage once at warning time.
    if (!beam.damaged && beam.timer >= kBeamWarnTime) {
      beam.damaged = true;

      // Check if player is on affected row/column.
      bool playerHit = false;
      if (beam.isRow && m_playerY == beam.index)
        playerHit = true;
      else if (!beam.isRow && m_playerX == beam.index)
        playerHit = true;

      if (playerHit) {
        TakeDamage(1);
      }
    }
  }

  // Remove expired beams.
  m_hazardBeams.erase(
      std::remove_if(m_hazardBeams.begin(), m_hazardBeams.end(),
                     [](const HazardBeam &b) { return b.timer >= kBeamLifetime; }),
      m_hazardBeams.end());
}

XMFLOAT3 GridGame::GetTowerWorldPos(const RuntimeTower &t) const {
  float towerX = static_cast<float>(t.data.x);
  float towerZ = static_cast<float>(t.data.y);
  switch (t.data.side) {
  case TowerSide::Left:   towerX = -1.0f; break;
  case TowerSide::Right:  towerX = static_cast<float>(m_map.Width()); break;
  case TowerSide::Top:    towerZ = -1.0f; break;
  case TowerSide::Bottom: towerZ = static_cast<float>(m_map.Height()); break;
  }
  return {towerX, 0.8f, towerZ};
}

// ---- Phase 6: create particle emitters at hazard tile positions ----

void GridGame::CreateGameEmitters() {
  m_gameEmitters.clear();
  m_burstEmitters.clear();

  for (int y = 0; y < m_map.Height(); ++y) {
    for (int x = 0; x < m_map.Width(); ++x) {
      const Tile &t = m_map.At(x, y);
      XMFLOAT3 c = m_map.TileCenter(x, y);

      if (t.type == TileType::Fire) {
        auto pos = XMVectorSet(c.x, 0.1f, c.z, 0.0f);
        auto em = std::make_unique<FireEmberEmitter>(64, pos, 15.0, true);
        m_gameEmitters.push_back(std::move(em));
      } else if (t.type == TileType::Ice) {
        auto pos = XMVectorSet(c.x, 0.05f, c.z, 0.0f);
        auto em = std::make_unique<IceCrystalEmitter>(32, pos, 8.0, true);
        m_gameEmitters.push_back(std::move(em));
      } else if (t.type == TileType::Goal) {
        // Phase 8C: goal beacon particles.
        auto pos = XMVectorSet(c.x, 0.1f, c.z, 0.0f);
        auto em = std::make_unique<GoalBeaconEmitter>(24, pos, 7.0, true);
        m_gameEmitters.push_back(std::move(em));
      }
    }
  }
}

// ---- Test stage (Phase 1 hardcoded) ----

void GridGame::LoadTestStage() {
  const int W = 12;
  const int H = 10;

  std::vector<Tile> layout(W * H);

  // Fill with floor.
  for (auto &t : layout)
    t.type = TileType::Floor;

  // Mark start tiles: player at (0,0), cargo at (1,0).
  layout[0 * W + 0].type = TileType::Start; // player spawn
  layout[0 * W + 1].type = TileType::Start; // cargo spawn

  // Mark goal column (x=W-1).
  for (int y = 0; y < H; ++y)
    layout[y * W + (W - 1)].type = TileType::Goal;

  // Add some walls.
  layout[3 * W + 4].type = TileType::Wall;
  layout[4 * W + 4].type = TileType::Wall;
  layout[5 * W + 4].type = TileType::Wall;
  layout[6 * W + 4].type = TileType::Wall;

  // Add a destructible wall (requires bait to open path).
  layout[2 * W + 4].hasWall = true;
  layout[2 * W + 4].wallDestructible = true;

  // A few hazard tiles for visual testing.
  layout[2 * W + 7].type = TileType::Fire;
  layout[5 * W + 8].type = TileType::Ice;
  layout[7 * W + 6].type = TileType::Lightning;

  m_map.Init(W, H, layout);

  // Phase 3: add test towers for telegraph testing.
  // Populate m_towers directly — do NOT set m_hasStageData here,
  // otherwise ReloadCurrentStage takes the wrong path.
  {
    m_towers.clear();

    TowerData t1;
    t1.x = 0;
    t1.y = 4;  // fires across row 4
    t1.side = TowerSide::Left;
    t1.pattern = TowerPattern::Row;
    t1.delay = 2.0f;
    t1.interval = 4.0f;

    RuntimeTower rt1;
    rt1.data = t1;
    rt1.attackTiles = ComputeAttackTiles(t1, W, H);
    m_towers.push_back(std::move(rt1));

    TowerData t2;
    t2.x = 6;
    t2.y = 0;  // fires down column 6
    t2.side = TowerSide::Top;
    t2.pattern = TowerPattern::Column;
    t2.delay = 3.0f;
    t2.interval = 5.0f;

    RuntimeTower rt2;
    rt2.data = t2;
    rt2.attackTiles = ComputeAttackTiles(t2, W, H);
    m_towers.push_back(std::move(rt2));
  }

  // Camera distance based on grid size.
  float gridMax = static_cast<float>(W > H ? W : H);
  m_cameraDistance = gridMax * 1.2f;

  SpawnPlayerAndCargo();

  // Reset HP and hazard state.
  m_playerHP = m_playerMaxHP;
  m_iFrameTimer = 0.0f;
  m_damageFlashTimer = 0.0f;
  m_fireDotTimer = 0.0f;
  m_stunned = false;
  m_stunTimer = 0.0f;
  m_sliding = false;
  m_slideDx = m_slideDy = 0;
  m_cargoDropping = false;
  m_cargoDropTimer = 0.0f;
  m_prevTileX = m_playerX;
  m_prevTileY = m_playerY;
  for (auto &mark : m_trail) mark.active = false;
  m_trailHead = 0;

  // Reset camera mode and hazard beams.
  m_topDownMode = false;
  m_topDownBlend = 0.0f;
  m_prevCamToggle = false;
  m_hazardBeams.clear();
  m_hazardBeamSpawnTimer = 0.0f;

  // Stagger hazard timers.
  int hazardIdx = 0;
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      Tile &t = m_map.At(x, y);
      if (t.type == TileType::Lightning || t.type == TileType::Spike) {
        t.hazardTimer = static_cast<float>(hazardIdx) * 0.4f;
        t.hazardActive = false;
        ++hazardIdx;
      }
    }
  }

  CreateGameEmitters();
}

// ---- Phase 5C: Load from editor StageData ----

void GridGame::ReloadCurrentStage() {
  if (m_hasStageData) {
    LoadFromStageData(m_loadedStage);
  } else {
    LoadTestStage();
    m_stageTimer = 0.0f;
    m_moveCount = 0;
    m_prevUp = m_prevDown = m_prevLeft = m_prevRight = false;
    m_repeatActive = false;
    m_repeatTimer = 0.0f;
    // Find goal for intro camera.
    m_goalWorldPos = m_playerVisualPos;
    for (int y = 0; y < m_map.Height(); ++y)
      for (int x = 0; x < m_map.Width(); ++x)
        if (m_map.At(x, y).type == TileType::Goal) {
          m_goalWorldPos = m_map.TileCenter(x, y);
          goto foundGoal1;
        }
    foundGoal1:
    m_introTimer = 0.0f;
    m_state = GridGameState::Intro;
  }
}

void GridGame::ResetToMainMenu() {
  m_state = GridGameState::MainMenu;
  m_hasStageData = false;
}

void GridGame::LoadFromStageData(const StageData &stage) {
  m_loadedStage = stage;
  m_hasStageData = true;

  const int W = stage.width;
  const int H = stage.height;

  std::vector<Tile> layout(static_cast<size_t>(W) * H);
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      const TileData &td = stage.tiles[static_cast<size_t>(y) * W + x];
      Tile &t = layout[static_cast<size_t>(y) * W + x];
      t.type = td.type;
      t.hasWall = td.hasWall;
      t.wallDestructible = td.wallDestructible;
    }
  }

  m_map.Init(W, H, layout);

  // Set player and cargo positions from stage spawn points.
  m_playerX = stage.playerSpawnX;
  m_playerY = stage.playerSpawnY;
  m_cargoX = stage.cargoSpawnX;
  m_cargoY = stage.cargoSpawnY;

  m_playerVisualPos = m_map.TileCenter(m_playerX, m_playerY);
  m_playerLerpFrom = m_playerVisualPos;
  m_playerLerpT = 1.0f;

  m_cargoVisualPos = m_map.TileCenter(m_cargoX, m_cargoY);
  m_cargoLerpFrom = m_cargoVisualPos;
  m_cargoLerpT = 1.0f;

  // Camera distance based on grid size.
  float gridMax = static_cast<float>(W > H ? W : H);
  m_cameraDistance = gridMax * 1.2f;

  // Reset game state.
  m_stageTimer = 0.0f;
  m_moveCount = 0;
  m_prevUp = m_prevDown = m_prevLeft = m_prevRight = false;
  m_repeatActive = false;
  m_repeatTimer = 0.0f;
  m_prevEsc = false;

  // Reset HP and hazard state.
  m_playerHP = m_playerMaxHP;
  m_iFrameTimer = 0.0f;
  m_damageFlashTimer = 0.0f;
  m_fireDotTimer = 0.0f;
  m_stunned = false;
  m_stunTimer = 0.0f;
  m_sliding = false;
  m_slideDx = m_slideDy = 0;
  m_cargoDropping = false;
  m_cargoDropTimer = 0.0f;
  m_shakeTimer = 0.0f;
  m_prevTileX = m_playerX;
  m_prevTileY = m_playerY;
  for (auto &mark : m_trail) mark.active = false;
  m_trailHead = 0;

  // Stagger hazard timers so same-type tiles don't sync.
  int hazardIdx = 0;
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      Tile &t = m_map.At(x, y);
      if (t.type == TileType::Lightning || t.type == TileType::Spike) {
        t.hazardTimer = static_cast<float>(hazardIdx) * 0.4f;
        t.hazardActive = false;
        ++hazardIdx;
      }
      if (t.type == TileType::Crumble) {
        t.crumbleBroken = false;
      }
    }
  }

  CreateGameEmitters();
  InitTowers();

  // Initialize TPS camera behind player.
  m_playerFacingYaw = 0.0f;
  m_playerVisualYaw = 0.0f;
  m_tpsCamYaw = 0.0f;
  m_tpsCamActualDist = m_tpsCamDist;

  // Reset camera mode and hazard beams.
  m_topDownMode = false;
  m_topDownBlend = 0.0f;
  m_prevCamToggle = false;
  m_hazardBeams.clear();
  m_hazardBeamSpawnTimer = 0.0f;

  // Find goal tile position for intro camera pan.
  m_goalWorldPos = m_playerVisualPos; // fallback if no goal found
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      if (m_map.At(x, y).type == TileType::Goal) {
        m_goalWorldPos = m_map.TileCenter(x, y);
        break;
      }
    }
  }

  // Start with intro camera cinematic.
  m_introTimer = 0.0f;
  m_state = GridGameState::Intro;
}

// ---- Phase 2: Movement helpers ----

void GridGame::SpawnPlayerAndCargo() {
  bool foundPlayer = false;
  for (int y = 0; y < m_map.Height(); ++y) {
    for (int x = 0; x < m_map.Width(); ++x) {
      if (m_map.At(x, y).type == TileType::Start) {
        if (!foundPlayer) {
          m_playerX = x;
          m_playerY = y;
          foundPlayer = true;
        } else {
          m_cargoX = x;
          m_cargoY = y;
          m_playerVisualPos = m_map.TileCenter(m_playerX, m_playerY);
          m_playerLerpFrom = m_playerVisualPos;
          m_playerLerpT = 1.0f;
          m_cargoVisualPos = m_map.TileCenter(m_cargoX, m_cargoY);
          m_cargoLerpFrom = m_cargoVisualPos;
          m_cargoLerpT = 1.0f;
          return;
        }
      }
    }
  }
}

void GridGame::SetPlayerPosition(int x, int y) {
  m_playerLerpFrom = m_playerVisualPos;
  m_playerLerpT = 0.0f;
  m_playerX = x;
  m_playerY = y;
}

void GridGame::SetCargoPosition(int x, int y) {
  m_cargoLerpFrom = m_cargoVisualPos;
  m_cargoLerpT = 0.0f;
  m_cargoX = x;
  m_cargoY = y;
}

void GridGame::TryMove(int dx, int dy, bool pulling) {
  if (m_playerLerpT < 1.0f)
    return;
  if (m_stunned)
    return;
  if (m_cargoDropping)
    return; // all input locked during cargo drop-off

  int nx = m_playerX + dx;
  int ny = m_playerY + dy;

  if (!m_map.IsWalkable(nx, ny))
    return;

  // Update player facing direction based on move (dx maps to X, dy maps to Z).
  // Skip when pulling — player walks backward, keeps facing the cargo.
  if (!pulling)
    m_playerFacingYaw = atan2f(static_cast<float>(dx), static_cast<float>(dy));

  bool destHasCargo = (nx == m_cargoX && ny == m_cargoY);

  // Track previous tile for crumble step-off.
  int oldX = m_playerX, oldY = m_playerY;

  m_lastMoveWasCargoInteraction = false; // reset; set below if cargo moves

  if (pulling && !destHasCargo) {
    int behindX = m_playerX - dx;
    int behindY = m_playerY - dy;
    bool cargoBehind = (behindX == m_cargoX && behindY == m_cargoY);

    if (cargoBehind) {
      SetCargoPosition(m_playerX, m_playerY);
      m_lastMoveWasCargoInteraction = true;
    }
    SetPlayerPosition(nx, ny);
    if (!m_sliding)
      ++m_moveCount;
  } else {
    if (destHasCargo) {
      int cx = m_cargoX + dx;
      int cy = m_cargoY + dy;
      if (!m_map.IsWalkable(cx, cy)) {
        // Check if cargo is being pushed off the grid edge (out of bounds).
        if (!m_map.InBounds(cx, cy)) {
          // Start cargo drop-off animation.
          m_cargoDropping = true;
          m_cargoDropTimer = 0.0f;
          m_cargoDropDx = dx;
          m_cargoDropDy = dy;
          m_cargoDropFrom = m_cargoVisualPos;
          m_lastMoveWasCargoInteraction = true;

          // Player moves into cargo's old position.
          SetPlayerPosition(nx, ny);
          ++m_moveCount;
        }
        return;
      }

      m_lastMoveWasCargoInteraction = true;

      // Cargo ice slide: if cargo destination is ice, keep sliding.
      SetCargoPosition(cx, cy);
      while (m_map.InBounds(cx, cy) &&
             m_map.At(cx, cy).type == TileType::Ice) {
        int ncx = cx + dx, ncy = cy + dy;
        if (!m_map.IsWalkable(ncx, ncy))
          break;
        if (ncx == nx && ncy == ny)
          break; // don't slide through player
        cx = ncx;
        cy = ncy;
        SetCargoPosition(cx, cy);
      }
    }

    SetPlayerPosition(nx, ny);
    if (!m_sliding)
      ++m_moveCount;
  }

  // Crumble step-off: break the tile we just left.
  if (m_map.InBounds(oldX, oldY) &&
      m_map.At(oldX, oldY).type == TileType::Crumble) {
    m_map.DestroyCrumble(oldX, oldY);

    // Phase 8B: crumble debris VFX.
    XMFLOAT3 cc = m_map.TileCenter(oldX, oldY);
    auto cpos = XMVectorSet(cc.x, 0.1f, cc.z, 0.0f);
    SpawnBurst<CrumbleDebrisEmitter>(cpos);
  }

  // Phase 6: record trail mark at the tile we left.
  m_trail[m_trailHead] = {oldX, oldY, 0.0f, true};
  m_trailHead = (m_trailHead + 1) % kMaxTrailMarks;

  // Phase 8D: player move sparks at old tile position.
  {
    XMFLOAT3 oc = m_map.TileCenter(oldX, oldY);
    auto opos = XMVectorSet(oc.x, 0.1f, oc.z, 0.0f);
    SpawnBurst<PlayerMoveSparksEmitter>(opos);
  }

  // Ice slide initiation: if destination is ice, start sliding.
  if (m_map.InBounds(nx, ny) && m_map.At(nx, ny).type == TileType::Ice &&
      !m_sliding) {
    m_sliding = true;
    m_slideDx = dx;
    m_slideDy = dy;
  }
}

// ---- Phase 4: HP & Hazard system ----

void GridGame::TakeDamage(int amount) {
  if (m_iFrameTimer > 0.0f)
    return; // invincibility active

  m_playerHP -= amount;
  m_iFrameTimer = kIFrameDuration;
  m_damageFlashTimer = 0.25f;
  m_shakeTimer = kShakeDuration;
  m_shakeIntensity = kShakeStrength;

  // Phase 8B: damage hit burst VFX at player position.
  {
    XMFLOAT3 pc = m_map.TileCenter(m_playerX, m_playerY);
    auto ppos = XMVectorSet(pc.x, 0.5f, pc.z, 0.0f);
    SpawnBurst<DamageHitBurstEmitter>(ppos);
  }

  if (m_playerHP <= 0) {
    m_playerHP = 0;
    m_state = GridGameState::StageFail;
  }
}

void GridGame::UpdateHazards(float dt) {
  // --- Fire: DOT while standing on fire tile ---
  if (m_map.InBounds(m_playerX, m_playerY) &&
      m_map.At(m_playerX, m_playerY).type == TileType::Fire &&
      m_playerLerpT >= 1.0f) {
    m_fireDotTimer += dt;
    if (m_fireDotTimer >= 1.0f) {
      TakeDamage(1);
      m_fireDotTimer -= 1.0f;
    }
  } else {
    m_fireDotTimer = 0.0f;
  }

  // --- Lightning: periodic burst on all lightning tiles ---
  for (int y = 0; y < m_map.Height(); ++y) {
    for (int x = 0; x < m_map.Width(); ++x) {
      Tile &t = m_map.At(x, y);
      if (t.type != TileType::Lightning)
        continue;

      t.hazardTimer += dt;
      const float kLightningCycle = 2.0f;
      const float kBurstWindow = 0.3f;

      bool wasActive = t.hazardActive;
      if (t.hazardTimer >= kLightningCycle) {
        t.hazardActive = true;
        if (t.hazardTimer >= kLightningCycle + kBurstWindow) {
          t.hazardActive = false;
          t.hazardTimer = 0.0f;
        }
      }

      // Phase 8C: lightning strike sparks on activation edge.
      if (t.hazardActive && !wasActive) {
        XMFLOAT3 lc = m_map.TileCenter(x, y);
        auto lpos = XMVectorSet(lc.x, 0.2f, lc.z, 0.0f);
        SpawnBurst<LightningStrikeSparksEmitter>(lpos);
      }

      // Damage player if on this tile during burst.
      if (t.hazardActive && m_playerX == x && m_playerY == y &&
          m_playerLerpT >= 1.0f) {
        TakeDamage(1);
      }
    }
  }

  // --- Spike: toggle on/off + stun ---
  for (int y = 0; y < m_map.Height(); ++y) {
    for (int x = 0; x < m_map.Width(); ++x) {
      Tile &t = m_map.At(x, y);
      if (t.type != TileType::Spike)
        continue;

      bool spikeWasActive = t.hazardActive;
      t.hazardTimer += dt;
      const float kSpikeOn = 1.5f;
      const float kSpikeOff = 1.5f;
      float cycleLen = kSpikeOn + kSpikeOff;
      float phase = fmodf(t.hazardTimer, cycleLen);
      t.hazardActive = (phase < kSpikeOn);

      // Phase 8C: spike trap sparks on activation edge.
      if (t.hazardActive && !spikeWasActive) {
        XMFLOAT3 sc = m_map.TileCenter(x, y);
        auto spos = XMVectorSet(sc.x, 0.1f, sc.z, 0.0f);
        SpawnBurst<SpikeTrapSparksEmitter>(spos);
      }

      // Damage + stun if player on active spike.
      if (t.hazardActive && m_playerX == x && m_playerY == y &&
          m_playerLerpT >= 1.0f) {
        TakeDamage(1);
        if (!m_stunned) {
          m_stunned = true;
          m_stunTimer = kStunDuration;
        }
      }
    }
  }
}

// ---- Phase 3: Tower system ----

void GridGame::InitTowers() {
  m_towers.clear();

  // Get tower data — from loaded StageData if available, else from test stage.
  const std::vector<TowerData> *towerSrc = nullptr;
  int gridW = m_map.Width();
  int gridH = m_map.Height();

  if (m_hasStageData) {
    towerSrc = &m_loadedStage.towers;
  }

  if (!towerSrc || towerSrc->empty())
    return;

  for (const auto &td : *towerSrc) {
    RuntimeTower rt;
    rt.data = td;
    rt.timer = 0.0f;
    rt.lastFireCycle = -1;
    rt.fireFlashTimer = 0.0f;
    rt.attackTiles = ComputeAttackTiles(td, gridW, gridH);
    m_towers.push_back(std::move(rt));

    // Phase 8C: tower idle wisp emitter at tower position.
    XMFLOAT3 tp = GetTowerWorldPos(m_towers.back());
    auto wpos = XMVectorSet(tp.x, tp.y + 0.3f, tp.z, 0.0f);
    auto wisp = std::make_unique<TowerIdleWispEmitter>(16, wpos, 3.5, true);
    m_gameEmitters.push_back(std::move(wisp));
  }
}

GridGame::TowerPhase GridGame::GetTowerPhase(const RuntimeTower &t) const {
  if (t.fireFlashTimer > 0.0f)
    return TowerPhase::Firing;
  if (t.timer < t.data.delay)
    return TowerPhase::Idle;
  if (t.data.interval < 0.01f)
    return TowerPhase::Idle; // guard: degenerate interval

  float cycleTime = fmodf(t.timer - t.data.delay, t.data.interval);
  float timeUntilFire = t.data.interval - cycleTime;

  if (timeUntilFire <= kWarn2LeadTime)
    return TowerPhase::Warn2;
  if (timeUntilFire <= kWarn1LeadTime)
    return TowerPhase::Warn1;
  return TowerPhase::Idle;
}

void GridGame::UpdateTowers(float dt) {
  for (auto &tower : m_towers) {
    tower.timer += dt;

    // Tick fire flash countdown.
    if (tower.fireFlashTimer > 0.0f)
      tower.fireFlashTimer -= dt;

    // Not past initial delay yet, or degenerate interval.
    if (tower.timer < tower.data.delay || tower.data.interval < 0.01f)
      continue;

    // Detect fire moment: cycle count incremented.
    int currentCycle =
        static_cast<int>((tower.timer - tower.data.delay) / tower.data.interval);
    if (currentCycle > tower.lastFireCycle) {
      tower.lastFireCycle = currentCycle;
      tower.fireFlashTimer = kFireFlashDuration;

      // Phase 8B: tower fire burst VFX at tower position.
      {
        XMFLOAT3 tp = GetTowerWorldPos(tower);
        auto tpos = XMVectorSet(tp.x, tp.y, tp.z, 0.0f);
        SpawnBurst<TowerFireBurstEmitter>(tpos);
      }

      // Phase 3C: damage player if on affected tile.
      for (const auto &[ax, ay] : tower.attackTiles) {
        if (ax == m_playerX && ay == m_playerY && m_playerLerpT >= 1.0f) {
          TakeDamage(1);
          break;
        }
      }

      // Phase 8B: beam impact sparks on each attack tile.
      for (const auto &[ax, ay] : tower.attackTiles) {
        if (m_map.InBounds(ax, ay)) {
          XMFLOAT3 tc = m_map.TileCenter(ax, ay);
          auto spos = XMVectorSet(tc.x, 0.1f, tc.z, 0.0f);
          SpawnBurst<BeamImpactSparksEmitter>(spos);
        }
      }

      // Phase 3C: destroy destructible walls on affected tiles.
      for (const auto &[ax, ay] : tower.attackTiles) {
        if (m_map.InBounds(ax, ay)) {
          Tile &t = m_map.At(ax, ay);
          if (t.hasWall && t.wallDestructible && !t.wallDestroyed) {
            m_map.DestroyWall(ax, ay);

            // Phase 8B: wall debris VFX.
            XMFLOAT3 wc = m_map.TileCenter(ax, ay);
            auto wpos = XMVectorSet(wc.x, 0.5f, wc.z, 0.0f);
            SpawnBurst<WallDebrisEmitter>(wpos);
          }
        }
      }
    }
  }
}

// ---- Update ----

void GridGame::Update(float dt, Input &input, Win32Window & /*window*/,
                      FrameData &frame) {
  m_uiTimer += dt;

  // Skeletal animation: 3-state crossfade (idle / run / push).
  // Push = moving cargo, Run = moving without cargo, Idle = standing still.
  if (m_playerHasSkeleton && m_dx) {
    // Push linger: cargo interaction resets a 1-second timer.
    if (m_lastMoveWasCargoInteraction)
      m_pushLingerTimer = kPushLingerDuration;
    else if (m_pushLingerTimer > 0.0f)
      m_pushLingerTimer = std::max(0.0f, m_pushLingerTimer - dt);

    // Move linger: player movement resets a short timer to smooth run→idle.
    bool playerCurrentlyMoving = m_playerLerpT < 1.0f;
    if (playerCurrentlyMoving)
      m_moveLingerTimer = kMoveLingerDuration;
    else if (m_moveLingerTimer > 0.0f)
      m_moveLingerTimer = std::max(0.0f, m_moveLingerTimer - dt);

    bool wantPush = m_pushLingerTimer > 0.0f;
    bool wantRun  = !wantPush && m_moveLingerTimer > 0.0f;

    // Determine desired clip: push > run > idle.
    int desiredClip = m_idleClipIndex; // default
    if (wantPush && m_pushClipIndex >= 0)
      desiredClip = m_pushClipIndex;
    else if (wantRun && m_runClipIndex >= 0)
      desiredClip = m_runClipIndex;

    // Crossfade: when desired clip changes, start transition.
    if (desiredClip != m_animActiveClip) {
      m_animPrevClip = m_animActiveClip;
      m_animTransition = 0.0f;
      m_animActiveClip = desiredClip;
    }

    // Ramp transition toward 1.
    if (m_animTransition < 1.0f) {
      m_animTransition = std::min(1.0f, m_animTransition + dt * kAnimBlendSpeed);
    }

    // Accumulate time for all clips (keeps loops smooth).
    m_animTime += dt;
    m_pushAnimTime += dt;
    m_runAnimTime += dt;

    // Helper to get time accumulator for a clip index.
    auto getClipTime = [&](int clipIdx) -> float {
      if (clipIdx == m_pushClipIndex) return m_pushAnimTime;
      if (clipIdx == m_runClipIndex)  return m_runAnimTime;
      return m_animTime; // idle or fallback
    };

    BonePalette palette;
    bool hasActive = m_animActiveClip >= 0 &&
                     m_animActiveClip < static_cast<int>(m_playerAnimations.size());
    bool hasPrev   = m_animPrevClip >= 0 &&
                     m_animPrevClip < static_cast<int>(m_playerAnimations.size());

    if (hasActive && hasPrev && m_animTransition > 0.001f && m_animTransition < 0.999f) {
      // Crossfade between previous and active clip.
      EvaluateAnimationBlend(m_playerSkeleton,
                             m_playerAnimations[m_animPrevClip], getClipTime(m_animPrevClip),
                             m_playerAnimations[m_animActiveClip], getClipTime(m_animActiveClip),
                             m_animTransition, palette);
    } else if (hasActive) {
      // Fully in active clip.
      EvaluateAnimation(m_playerSkeleton,
                        m_playerAnimations[m_animActiveClip], getClipTime(m_animActiveClip),
                        palette);
    } else {
      ComputeProceduralIdle(m_playerSkeleton, m_animTime, palette);
    }
    m_dx->GetMeshRenderer().SetBonePalette(m_playerMeshId, palette);
  }

  // ESC edge detection.
  bool escNow = input.IsKeyDown(VK_ESCAPE);
  bool escPressed = escNow && !m_prevEsc;
  m_prevEsc = escNow;

  switch (m_state) {
  case GridGameState::MainMenu:
    UpdateMainMenu(frame);
    break;
  case GridGameState::StageSelect:
    UpdateStageSelect(frame);
    break;
  case GridGameState::Intro:
    UpdateIntro(dt, input, frame);
    break;
  case GridGameState::Playing:
    if (escPressed) {
      m_state = GridGameState::Paused;
    } else {
      UpdatePlaying(dt, input, frame);
    }
    break;
  case GridGameState::Paused:
    UpdatePaused(frame);
    break;
  case GridGameState::StageComplete:
    UpdateStageComplete(frame);
    break;
  case GridGameState::StageFail:
    UpdateStageFail(frame);
    break;
  }

  // Set post-process params for neon look.
  frame.bloomEnabled = true;
  frame.bloomThreshold = 0.4f;
  frame.bloomIntensity = 1.0f;
  frame.exposure = 0.8f;
  frame.skyExposure = 0.15f;
  frame.ssaoEnabled = true;

  // Subtle bloom breathing tied to game time.
  if (m_state == GridGameState::Intro || m_state == GridGameState::Playing) {
    float bloomPulse = 1.0f + 0.1f * sinf(m_stageTimer * 1.0f * 3.14159f);
    frame.bloomIntensity *= bloomPulse;
  }

  // Build the visual scene.
  BuildScene(frame);
}

// ---- State handlers ----

void GridGame::UpdateMainMenu(FrameData & /*frame*/) {
  PushNeonTheme();

  const float winW = 400.0f;
  ImGui::SetNextWindowPos(
      ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
             ImGui::GetIO().DisplaySize.y * 0.45f),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(winW, 320));
  ImGui::Begin("##MainMenu", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

  // Pulsing title.
  ImGui::SetWindowFontScale(2.5f);
  float glow = 0.7f + 0.3f * sinf(m_uiTimer * 2.0f);
  const char *title = "GRID GAUNTLET";
  float titleW = ImGui::CalcTextSize(title).x;
  ImGui::SetCursorPosX((winW - titleW) * 0.5f);
  ImGui::TextColored(ImVec4(0.0f, glow, 1.0f, 1.0f), "%s", title);
  ImGui::SetWindowFontScale(1.0f);

  // Subtitle.
  const char *sub = "A Tactical Cargo-Push Puzzle";
  float subW = ImGui::CalcTextSize(sub).x;
  ImGui::SetCursorPosX((winW - subW) * 0.5f);
  ImGui::TextColored(ImVec4(0.0f, 0.5f, 0.7f, 0.7f), "%s", sub);

  ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();

  // Buttons.
  const float btnW = 200.0f;
  if (CenteredButton("PLAY", btnW, winW)) {
    m_hasStageData = false;
    LoadTestStage();
    m_stageTimer = 0.0f;
    m_moveCount = 0;
    m_prevUp = m_prevDown = m_prevLeft = m_prevRight = false;
    m_repeatActive = false;
    m_repeatTimer = 0.0f;
    // Find goal for intro camera.
    m_goalWorldPos = m_playerVisualPos;
    for (int y = 0; y < m_map.Height(); ++y)
      for (int x = 0; x < m_map.Width(); ++x)
        if (m_map.At(x, y).type == TileType::Goal) {
          m_goalWorldPos = m_map.TileCenter(x, y);
          goto foundGoal2;
        }
    foundGoal2:
    m_introTimer = 0.0f;
    m_state = GridGameState::Intro;
  }
  ImGui::Spacing();
  if (CenteredButton("QUIT", btnW, winW)) {
    m_wantsQuit = true;
  }

  ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();

  // Version text.
  ImGui::SetWindowFontScale(0.8f);
  const char *ver = "v0.7 - Grid Gauntlet";
  float verW = ImGui::CalcTextSize(ver).x;
  ImGui::SetCursorPosX((winW - verW) * 0.5f);
  ImGui::TextColored(ImVec4(0.4f, 0.45f, 0.5f, 0.6f), "%s", ver);
  ImGui::SetWindowFontScale(1.0f);

  ImGui::End();
  PopNeonTheme();
}

void GridGame::UpdateStageSelect(FrameData & /*frame*/) {
  // Placeholder for Phase 5.
  m_introTimer = 0.0f;
  m_state = GridGameState::Intro;
}

void GridGame::UpdateIntro(float dt, Input &input, FrameData & /*frame*/) {
  m_dt = dt;
  m_introTimer += dt;

  float totalIntro = kIntroHoldPlayer + kIntroPanToGoal + kIntroHoldGoal + kIntroPanBack;

  // Skip intro: any key, mouse click, or gamepad button.
  bool skip = input.IsKeyDown(VK_SPACE) || input.IsKeyDown(VK_RETURN) ||
              input.IsKeyDown(VK_ESCAPE) || input.IsKeyDown('W') ||
              input.IsKeyDown('A') || input.IsKeyDown('S') || input.IsKeyDown('D') ||
              input.IsKeyDown(VK_LBUTTON) ||
              input.IsGamepadButtonDown(XINPUT_GAMEPAD_A) ||
              input.IsGamepadButtonDown(XINPUT_GAMEPAD_START);

  if (m_introTimer >= totalIntro || (skip && m_introTimer > 0.3f)) {
    m_state = GridGameState::Playing;
  }
}

void GridGame::UpdatePlaying(float dt, Input &input, FrameData & /*frame*/) {
  m_dt = dt;
  m_stageTimer += dt;

  // Phase 6: age trail marks.
  for (int i = 0; i < kMaxTrailMarks; ++i) {
    if (m_trail[i].active) {
      m_trail[i].age += dt;
      if (m_trail[i].age >= 3.0f)
        m_trail[i].active = false;
    }
  }

  // Phase 6: update game particle emitters.
  for (auto &em : m_gameEmitters)
    em->Update(static_cast<double>(dt));

  // Phase 8A: update burst emitters.
  UpdateBurstEmitters(dt);

  // --- Tick hazard timers ---
  UpdateHazards(dt);

  // --- Tick tower timers (Phase 3) ---
  UpdateTowers(dt);

  // --- Tick i-frame timer ---
  if (m_iFrameTimer > 0.0f)
    m_iFrameTimer -= dt;
  if (m_damageFlashTimer > 0.0f)
    m_damageFlashTimer -= dt;
  if (m_shakeTimer > 0.0f)
    m_shakeTimer -= dt;

  // --- Tick stun timer ---
  if (m_stunned) {
    m_stunTimer -= dt;
    if (m_stunTimer <= 0.0f) {
      m_stunned = false;
      m_stunTimer = 0.0f;
    }
  }

  // --- Camera mode toggle (R key / B button) ---
  {
    bool toggleNow = input.IsKeyDown('R') ||
                     input.IsGamepadButtonDown(XINPUT_GAMEPAD_B);
    if (toggleNow && !m_prevCamToggle) {
      m_topDownMode = !m_topDownMode;
    }
    m_prevCamToggle = toggleNow;

    // Smoothly blend toward target mode.
    float target = m_topDownMode ? 1.0f : 0.0f;
    float diff = target - m_topDownBlend;
    m_topDownBlend += diff * std::min(1.0f, kCamTransitionSpeed * dt);
    // Snap when very close.
    if (fabsf(m_topDownBlend - target) < 0.001f)
      m_topDownBlend = target;
  }

  // --- Update hazard beams ---
  UpdateHazardBeams(dt);

  // --- TPS Camera controls ---
  float scroll = input.ConsumeScrollDelta();
  if (scroll != 0.0f) {
    if (input.IsKeyDown(VK_CONTROL)) {
      // Ctrl+Scroll: adjust FOV.
      float fov = m_camera->FovY() - scroll * 0.05f;
      fov = std::max(0.2f, std::min(fov, 2.0f)); // ~11-115 degrees
      m_camera->SetLens(fov, m_camera->Aspect(), m_camera->NearZ(), m_camera->FarZ());
    } else {
      // Scroll: zoom TPS distance.
      m_tpsCamDist -= scroll * kCamZoomSpeed;
      m_tpsCamDist = std::clamp(m_tpsCamDist, kTpsCamDistMin, kTpsCamDistMax);
    }
  }

  // RMB drag to orbit TPS camera around player.
  if (input.IsKeyDown(VK_RBUTTON)) {
    MouseDelta md = input.ConsumeMouseDelta();
    m_tpsCamYaw += static_cast<float>(md.dx) * 0.005f;
    m_tpsCamPitch -= static_cast<float>(md.dy) * 0.005f;
    m_tpsCamPitch = std::clamp(m_tpsCamPitch, 0.1f, 1.4f); // clamp vertical
  } else {
    input.ConsumeMouseDelta(); // discard when not rotating
  }

  // Smoothly follow player facing direction (only when not RMB orbiting).
  if (!input.IsKeyDown(VK_RBUTTON)) {
    // Shortest-arc interpolation for yaw.
    float diff = m_playerFacingYaw - m_tpsCamYaw;
    // Wrap to [-PI, PI].
    while (diff > 3.14159f) diff -= 6.28318f;
    while (diff < -3.14159f) diff += 6.28318f;
    m_tpsCamYaw += diff * std::min(1.0f, kTpsCamSmoothSpeed * dt);
  }

  // Smooth player model rotation (visual yaw lerps toward facing yaw).
  {
    float diff = m_playerFacingYaw - m_playerVisualYaw;
    while (diff > 3.14159f) diff -= 6.28318f;
    while (diff < -3.14159f) diff += 6.28318f;
    m_playerVisualYaw += diff * std::min(1.0f, kPlayerTurnSpeed * dt);
  }

  // --- Pull modifier (keyboard E or gamepad A button) ---
  bool pulling = input.IsKeyDown('E') ||
                 input.IsGamepadButtonDown(XINPUT_GAMEPAD_A);

  // --- Input edge detection (press-to-move, camera-relative) ---
  // Merge keyboard + gamepad left stick (digitized with threshold).
  constexpr float kStickThreshold = 0.5f;
  float stickX = input.LeftStickX();
  float stickY = input.LeftStickY();
  // Snap stick to dominant axis to avoid diagonal movement on a tile grid.
  if (fabsf(stickX) > fabsf(stickY)) stickY = 0.0f;
  else stickX = 0.0f;

  bool upNow    = input.IsKeyDown('W') || input.IsKeyDown(VK_UP)    || stickY > kStickThreshold;
  bool downNow  = input.IsKeyDown('S') || input.IsKeyDown(VK_DOWN)  || stickY < -kStickThreshold;
  bool leftNow  = input.IsKeyDown('A') || input.IsKeyDown(VK_LEFT)  || stickX < -kStickThreshold;
  bool rightNow = input.IsKeyDown('D') || input.IsKeyDown(VK_RIGHT) || stickX > kStickThreshold;

  // Convert camera-relative WASD to grid-space cardinal directions.
  // Camera forward is m_tpsCamYaw: atan2(dx,dy) convention where yaw=0 is +Z.
  // Snap camera forward to nearest cardinal direction, then derive all 4.
  auto snapToCardinal = [](float yaw, int &outDx, int &outDy) {
    // Normalize to [0, 2PI).
    float a = fmodf(yaw, 6.28318f);
    if (a < 0.0f) a += 6.28318f;
    // Snap to 4 sectors: 0=+Z(dy+1), PI/2=+X(dx+1), PI=-Z(dy-1), 3PI/2=-X(dx-1)
    if (a < 0.7854f || a >= 5.4978f) { outDx = 0; outDy = 1; }       // ~0°    → +Z
    else if (a < 2.3562f)            { outDx = 1; outDy = 0; }        // ~90°   → +X
    else if (a < 3.9270f)            { outDx = 0; outDy = -1; }       // ~180°  → -Z
    else                             { outDx = -1; outDy = 0; }       // ~270°  → -X
  };

  // Forward/back/left/right: camera-relative in TPS, world-based in top-down.
  int fwdDx, fwdDy, backDx, backDy, leftDx, leftDy, rightDx, rightDy;
  if (m_topDownMode) {
    // World-based: W=+Z, S=-Z, A=-X, D=+X.
    fwdDx = 0;  fwdDy = 1;
    backDx = 0; backDy = -1;
    leftDx = -1; leftDy = 0;
    rightDx = 1; rightDy = 0;
  } else {
    // Camera-relative in TPS mode.
    snapToCardinal(m_tpsCamYaw, fwdDx, fwdDy);
    snapToCardinal(m_tpsCamYaw + 3.14159f, backDx, backDy);
    snapToCardinal(m_tpsCamYaw - 1.5708f, leftDx, leftDy);
    snapToCardinal(m_tpsCamYaw + 1.5708f, rightDx, rightDy);
  }

  // Determine current direction for repeat tracking.
  int moveDx = 0, moveDy = 0;
  bool edgeTriggered = false;

  if (upNow && !m_prevUp) {
    moveDx = fwdDx; moveDy = fwdDy; edgeTriggered = true;
  } else if (downNow && !m_prevDown) {
    moveDx = backDx; moveDy = backDy; edgeTriggered = true;
  } else if (leftNow && !m_prevLeft) {
    moveDx = leftDx; moveDy = leftDy; edgeTriggered = true;
  } else if (rightNow && !m_prevRight) {
    moveDx = rightDx; moveDy = rightDy; edgeTriggered = true;
  }

  if (edgeTriggered && !m_stunned) {
    TryMove(moveDx, moveDy, pulling);
    m_repeatDx = moveDx;
    m_repeatDy = moveDy;
    m_repeatTimer = 0.0f;
    m_repeatActive = true;
  }

  // Hold-to-repeat: if same direction key still held (camera-relative).
  bool holdingRepeatDir = false;
  if (m_repeatActive) {
    if (m_repeatDx == fwdDx && m_repeatDy == fwdDy && upNow) holdingRepeatDir = true;
    else if (m_repeatDx == backDx && m_repeatDy == backDy && downNow) holdingRepeatDir = true;
    else if (m_repeatDx == leftDx && m_repeatDy == leftDy && leftNow) holdingRepeatDir = true;
    else if (m_repeatDx == rightDx && m_repeatDy == rightDy && rightNow) holdingRepeatDir = true;
  }

  if (m_repeatActive && holdingRepeatDir) {
    m_repeatTimer += dt;
    float threshold = (m_repeatTimer < kRepeatDelay) ? kRepeatDelay : (kRepeatDelay + kRepeatInterval);
    if (m_repeatTimer >= threshold && m_playerLerpT >= 1.0f) {
      TryMove(m_repeatDx, m_repeatDy, pulling);
      // After initial delay, subsequent repeats at interval.
      m_repeatTimer = kRepeatDelay;
    }
  } else {
    m_repeatActive = false;
    m_repeatTimer = 0.0f;
  }

  m_prevUp = upNow;
  m_prevDown = downNow;
  m_prevLeft = leftNow;
  m_prevRight = rightNow;

  // --- Update visual interpolation ---
  XMFLOAT3 playerTarget = m_map.TileCenter(m_playerX, m_playerY);
  XMFLOAT3 cargoTarget = m_map.TileCenter(m_cargoX, m_cargoY);

  if (m_playerLerpT < 1.0f) {
    m_playerLerpT += dt * kMoveSpeed;
    if (m_playerLerpT >= 1.0f)
      m_playerLerpT = 1.0f;
    float t = m_playerLerpT;
    m_playerVisualPos.x =
        m_playerLerpFrom.x + (playerTarget.x - m_playerLerpFrom.x) * t;
    m_playerVisualPos.y =
        m_playerLerpFrom.y + (playerTarget.y - m_playerLerpFrom.y) * t;
    m_playerVisualPos.z =
        m_playerLerpFrom.z + (playerTarget.z - m_playerLerpFrom.z) * t;
  } else {
    m_playerVisualPos = playerTarget;
  }

  if (m_cargoLerpT < 1.0f) {
    m_cargoLerpT += dt * kMoveSpeed;
    if (m_cargoLerpT >= 1.0f)
      m_cargoLerpT = 1.0f;
    float t = m_cargoLerpT;
    m_cargoVisualPos.x =
        m_cargoLerpFrom.x + (cargoTarget.x - m_cargoLerpFrom.x) * t;
    m_cargoVisualPos.y =
        m_cargoLerpFrom.y + (cargoTarget.y - m_cargoLerpFrom.y) * t;
    m_cargoVisualPos.z =
        m_cargoLerpFrom.z + (cargoTarget.z - m_cargoLerpFrom.z) * t;
  } else {
    m_cargoVisualPos = cargoTarget;
  }

  // --- Ice slide: when lerp finishes on ice tile, continue sliding ---
  if (m_sliding && m_playerLerpT >= 1.0f) {
    int nextX = m_playerX + m_slideDx;
    int nextY = m_playerY + m_slideDy;
    bool blocked = !m_map.IsWalkable(nextX, nextY) ||
                   (nextX == m_cargoX && nextY == m_cargoY);

    if (!blocked && m_map.InBounds(m_playerX, m_playerY) &&
        m_map.At(m_playerX, m_playerY).type == TileType::Ice) {
      TryMove(m_slideDx, m_slideDy, false);
    } else {
      m_sliding = false;
      m_slideDx = 0;
      m_slideDy = 0;
    }
  }

  // --- Cargo drop-off animation (pushed off grid edge) ---
  if (m_cargoDropping) {
    m_cargoDropTimer += dt;
    float t = std::min(m_cargoDropTimer / kCargoDropDuration, 1.0f);

    // Ease-in for gravity feel: t^2 on the Y fall.
    float tFall = t * t;

    // Slide off in push direction + fall down.
    m_cargoVisualPos.x = m_cargoDropFrom.x +
        static_cast<float>(m_cargoDropDx) * kCargoDropSlideDistance * t;
    m_cargoVisualPos.z = m_cargoDropFrom.z +
        static_cast<float>(m_cargoDropDy) * kCargoDropSlideDistance * t;
    m_cargoVisualPos.y = m_cargoDropFrom.y - kCargoDropFallDepth * tFall;

    if (m_cargoDropTimer >= kCargoDropDuration) {
      m_cargoDropping = false;
      m_state = GridGameState::StageFail;
    }
    // Skip win condition while dropping.
  } else
  // --- Win condition: cargo on Goal tile after animation ---
  if (m_cargoLerpT >= 1.0f &&
      m_map.At(m_cargoX, m_cargoY).type == TileType::Goal) {
    m_state = GridGameState::StageComplete;
  }
}

void GridGame::UpdatePaused(FrameData & /*frame*/) {
  DrawDimOverlay(0.55f);
  PushNeonTheme();

  const float winW = 320.0f;
  ImGui::SetNextWindowPos(
      ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
             ImGui::GetIO().DisplaySize.y * 0.4f),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(winW, 320));
  ImGui::Begin("##PauseMenu", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

  // Title.
  ImGui::SetWindowFontScale(2.0f);
  const char *title = "PAUSED";
  float titleW = ImGui::CalcTextSize(title).x;
  ImGui::SetCursorPosX((winW - titleW) * 0.5f);
  ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "%s", title);
  ImGui::SetWindowFontScale(1.0f);
  ImGui::Spacing(); ImGui::Spacing();

  // Buttons.
  const float btnW = 220.0f;
  if (CenteredButton("Resume", btnW, winW)) {
    m_state = GridGameState::Playing;
  }
  ImGui::Spacing();
  if (CenteredButton("Restart", btnW, winW)) {
    ReloadCurrentStage();
  }
  ImGui::Spacing();
  if (CenteredButton("Main Menu", btnW, winW)) {
    m_state = GridGameState::MainMenu;
  }
  ImGui::Spacing();
  if (CenteredButton("Quit", btnW, winW)) {
    m_wantsQuit = true;
  }

  ImGui::Spacing(); ImGui::Spacing();

  // Controls reminder.
  ImGui::SetWindowFontScale(0.85f);
  const char *ctrl = "WASD: Move | E+Dir: Pull | ESC: Resume";
  float ctrlW = ImGui::CalcTextSize(ctrl).x;
  ImGui::SetCursorPosX((winW - ctrlW) * 0.5f);
  ImGui::TextColored(ImVec4(0.5f, 0.55f, 0.6f, 0.7f), "%s", ctrl);
  ImGui::SetWindowFontScale(1.0f);

  ImGui::End();
  PopNeonTheme();
}

void GridGame::UpdateStageComplete(FrameData & /*frame*/) {
  DrawDimOverlay(0.45f);
  PushNeonTheme();

  const float winW = 380.0f;
  ImGui::SetNextWindowPos(
      ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
             ImGui::GetIO().DisplaySize.y * 0.4f),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(winW, 400));
  ImGui::Begin("##StageComplete", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

  // Title with green glow.
  ImGui::SetWindowFontScale(2.2f);
  float glow = 0.7f + 0.3f * sinf(m_uiTimer * 3.0f);
  const char *title = "STAGE COMPLETE!";
  float titleW = ImGui::CalcTextSize(title).x;
  ImGui::SetCursorPosX((winW - titleW) * 0.5f);
  ImGui::TextColored(ImVec4(0.0f, glow, 0.4f, 1.0f), "%s", title);
  ImGui::SetWindowFontScale(1.0f);

  // Separator line.
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddLine(
      ImVec2(p.x + 20, p.y), ImVec2(p.x + winW - 40, p.y),
      ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.6f, 0.8f, 0.5f)), 1.5f);
  ImGui::Spacing(); ImGui::Spacing();

  // Stats.
  int mins = static_cast<int>(m_stageTimer) / 60;
  int secs = static_cast<int>(m_stageTimer) % 60;

  ImGui::SetWindowFontScale(1.2f);
  {
    char buf[64];
    snprintf(buf, sizeof(buf), "TIME    %02d:%02d", mins, secs);
    float tw = ImGui::CalcTextSize(buf).x;
    ImGui::SetCursorPosX((winW - tw) * 0.5f);
    ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "%s", buf);
  }
  {
    char buf[64];
    snprintf(buf, sizeof(buf), "MOVES   %d", m_moveCount);
    float tw = ImGui::CalcTextSize(buf).x;
    ImGui::SetCursorPosX((winW - tw) * 0.5f);
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s", buf);
  }
  ImGui::SetWindowFontScale(1.0f);
  ImGui::Spacing();

  // Rating.
  float tl = m_hasStageData ? m_loadedStage.timeLimit : 0.0f;
  int pm = m_hasStageData ? m_loadedStage.parMoves : 0;
  char grade = CalcRating(m_stageTimer, m_moveCount, tl, pm);

  const char *rankLabel = "RANK";
  float rlW = ImGui::CalcTextSize(rankLabel).x;
  ImGui::SetCursorPosX((winW - rlW) * 0.5f);
  ImGui::TextColored(ImVec4(0.5f, 0.55f, 0.6f, 0.8f), "%s", rankLabel);

  ImGui::SetWindowFontScale(3.0f);
  char gradeStr[2] = {grade, '\0'};
  float gw = ImGui::CalcTextSize(gradeStr).x;
  ImGui::SetCursorPosX((winW - gw) * 0.5f);
  ImGui::TextColored(RatingColor(grade), "%s", gradeStr);
  ImGui::SetWindowFontScale(1.0f);
  ImGui::Spacing();

  // Buttons.
  const float btnW = 220.0f;
  if (CenteredButton("Next Stage", btnW, winW)) {
    ReloadCurrentStage(); // placeholder — reloads current until Phase 5
  }
  ImGui::Spacing();
  if (CenteredButton("Restart", btnW, winW)) {
    ReloadCurrentStage();
  }
  ImGui::Spacing();
  if (CenteredButton("Main Menu", btnW, winW)) {
    m_state = GridGameState::MainMenu;
  }

  ImGui::End();
  PopNeonTheme();
}

void GridGame::UpdateStageFail(FrameData & /*frame*/) {
  DrawDimOverlay(0.6f);
  PushNeonTheme();

  const float winW = 350.0f;
  ImGui::SetNextWindowPos(
      ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
             ImGui::GetIO().DisplaySize.y * 0.4f),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(winW, 280));
  ImGui::Begin("##StageFail", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

  // Title with red pulse.
  ImGui::SetWindowFontScale(2.0f);
  float pulse = 0.6f + 0.4f * sinf(m_uiTimer * 2.5f);
  const char *title = "STAGE FAILED";
  float titleW = ImGui::CalcTextSize(title).x;
  ImGui::SetCursorPosX((winW - titleW) * 0.5f);
  ImGui::TextColored(ImVec4(1.0f, pulse * 0.2f, pulse * 0.2f, 1.0f), "%s", title);
  ImGui::SetWindowFontScale(1.0f);
  ImGui::Spacing();

  // Message.
  const char *msg = "Better luck next time...";
  float msgW = ImGui::CalcTextSize(msg).x;
  ImGui::SetCursorPosX((winW - msgW) * 0.5f);
  ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 0.8f), "%s", msg);
  ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();

  // Buttons.
  const float btnW = 220.0f;
  if (CenteredButton("Retry", btnW, winW)) {
    ReloadCurrentStage();
  }
  ImGui::Spacing();
  if (CenteredButton("Main Menu", btnW, winW)) {
    m_state = GridGameState::MainMenu;
  }

  ImGui::End();
  PopNeonTheme();
}

// ---- Scene building ----

void GridGame::BuildScene(FrameData &frame) {
  // Camera positioning: TPS during gameplay, orbit overview otherwise.
  float camX, camY, camZ;
  float lookYaw, lookPitch;

  bool inGameplay = (m_state == GridGameState::Intro ||
                     m_state == GridGameState::Playing ||
                     m_state == GridGameState::Paused ||
                     m_state == GridGameState::StageComplete ||
                     m_state == GridGameState::StageFail);

  if (inGameplay && m_state == GridGameState::Intro) {
    // Intro cinematic camera: smooth pan from player to goal and back.
    float totalIntro = kIntroHoldPlayer + kIntroPanToGoal + kIntroHoldGoal + kIntroPanBack;
    float t = std::clamp(m_introTimer, 0.0f, totalIntro);

    // Determine focus point blend (0=player, 1=goal) using eased transitions.
    float focusBlend = 0.0f;
    if (t < kIntroHoldPlayer) {
      focusBlend = 0.0f; // hold on player
    } else if (t < kIntroHoldPlayer + kIntroPanToGoal) {
      float p = (t - kIntroHoldPlayer) / kIntroPanToGoal;
      focusBlend = p * p * (3.0f - 2.0f * p); // smoothstep ease
    } else if (t < kIntroHoldPlayer + kIntroPanToGoal + kIntroHoldGoal) {
      focusBlend = 1.0f; // hold on goal
    } else {
      float p = (t - kIntroHoldPlayer - kIntroPanToGoal - kIntroHoldGoal) / kIntroPanBack;
      focusBlend = 1.0f - p * p * (3.0f - 2.0f * p); // smoothstep back
    }

    // Interpolate focus position between player and goal.
    float focusX = m_playerVisualPos.x + (m_goalWorldPos.x - m_playerVisualPos.x) * focusBlend;
    float focusZ = m_playerVisualPos.z + (m_goalWorldPos.z - m_playerVisualPos.z) * focusBlend;
    float focusY = 0.5f; // look at ground level

    // Camera orbits slightly elevated, at a moderate distance.
    float introDist = m_tpsCamDist * 1.3f;
    float introPitch = 0.65f; // slightly steeper than normal TPS
    float introYaw = m_tpsCamYaw + 3.14159f; // behind player facing

    camX = focusX + introDist * cosf(introPitch) * sinf(introYaw);
    camY = m_tpsCamHeight + introDist * sinf(introPitch);
    camZ = focusZ + introDist * cosf(introPitch) * cosf(introYaw);

    // Look toward focus point.
    float dx_ = focusX - camX;
    float dy_ = focusY - camY;
    float dz_ = focusZ - camZ;
    float horizDist = sqrtf(dx_ * dx_ + dz_ * dz_);
    lookYaw = atan2f(dx_, dz_);
    lookPitch = atan2f(dy_, horizDist);
  } else if (inGameplay) {
    // TPS camera: positioned behind and above the player.
    // Camera sits opposite the player's facing direction.
    float behindYaw = m_tpsCamYaw + 3.14159f; // 180 degrees behind facing

    // Compute desired camera position at full distance.
    float desiredX = m_playerVisualPos.x + m_tpsCamDist * cosf(m_tpsCamPitch) * sinf(behindYaw);
    float desiredY = m_tpsCamHeight + m_tpsCamDist * sinf(m_tpsCamPitch);
    float desiredZ = m_playerVisualPos.z + m_tpsCamDist * cosf(m_tpsCamPitch) * cosf(behindYaw);

    // Camera collision: walk from player toward desired camera position,
    // check if the camera would be behind a wall or outside the grid over a wall.
    // Sample points along the ray and find the closest blocking point.
    float wantDist = m_tpsCamDist;
    {
      const float wallH = 1.0f; // approximate wall height
      const int steps = 16;
      float dirX = desiredX - m_playerVisualPos.x;
      float dirZ = desiredZ - m_playerVisualPos.z;
      for (int i = steps; i >= 1; --i) {
        float frac = static_cast<float>(i) / static_cast<float>(steps);
        float sampleX = m_playerVisualPos.x + dirX * frac;
        float sampleZ = m_playerVisualPos.z + dirZ * frac;
        float sampleY = m_playerVisualPos.y + (desiredY - m_playerVisualPos.y) * frac;

        // Convert to grid coordinates.
        int gx = static_cast<int>(roundf(sampleX));
        int gy = static_cast<int>(roundf(sampleZ));

        // Check if this sample point is inside a wall.
        bool blocked = false;
        if (m_map.InBounds(gx, gy)) {
          const Tile &tile = m_map.At(gx, gy);
          if (tile.type == TileType::Wall ||
              (tile.hasWall && !tile.wallDestroyed)) {
            if (sampleY < wallH)
              blocked = true;
          }
        }

        if (blocked) {
          // Pull camera to just before the wall.
          float safeFrac = static_cast<float>(i - 1) / static_cast<float>(steps);
          wantDist = m_tpsCamDist * safeFrac;
          if (wantDist < kCamCollisionMinDist)
            wantDist = kCamCollisionMinDist;
          break;
        }
      }
    }

    // Smooth the actual distance toward the wanted distance.
    float distDiff = wantDist - m_tpsCamActualDist;
    if (distDiff < 0.0f) {
      // Zoom in quickly when hitting a wall.
      m_tpsCamActualDist += distDiff * std::min(1.0f, kCamCollisionSpeed * 2.0f * m_dt);
    } else {
      // Zoom out slowly when clear.
      m_tpsCamActualDist += distDiff * std::min(1.0f, kCamCollisionSpeed * 0.5f * m_dt);
    }
    m_tpsCamActualDist = std::clamp(m_tpsCamActualDist, kCamCollisionMinDist, m_tpsCamDist);

    // TPS camera position.
    float tpsCamX = m_playerVisualPos.x + m_tpsCamActualDist * cosf(m_tpsCamPitch) * sinf(behindYaw);
    float tpsCamY = m_tpsCamHeight + m_tpsCamActualDist * sinf(m_tpsCamPitch);
    float tpsCamZ = m_playerVisualPos.z + m_tpsCamActualDist * cosf(m_tpsCamPitch) * cosf(behindYaw);

    // TPS look direction.
    float tpsLookTargetY = 1.0f;
    float tdx = m_playerVisualPos.x - tpsCamX;
    float tdy = tpsLookTargetY - tpsCamY;
    float tdz = m_playerVisualPos.z - tpsCamZ;
    float tpsHorizDist = sqrtf(tdx * tdx + tdz * tdz);
    float tpsLookYaw = atan2f(tdx, tdz);
    float tpsLookPitch = atan2f(tdy, tpsHorizDist);

    // Top-down camera position: directly above player at fixed height.
    float topDownH = kTopDownHeight;
    float tdCamX = m_playerVisualPos.x;
    float tdCamY = topDownH;
    float tdCamZ = m_playerVisualPos.z;
    // Top-down looks straight down: yaw doesn't matter much, pitch near -PI/2.
    float tdLookYaw = 0.0f;
    float tdLookPitch = -kTopDownPitch;

    // Blend between TPS and top-down based on m_topDownBlend.
    float b = m_topDownBlend;
    camX = tpsCamX + (tdCamX - tpsCamX) * b;
    camY = tpsCamY + (tdCamY - tpsCamY) * b;
    camZ = tpsCamZ + (tdCamZ - tpsCamZ) * b;

    // Blend look angles (handle yaw wrap for shortest path).
    float yawDiff = tdLookYaw - tpsLookYaw;
    while (yawDiff > 3.14159f) yawDiff -= 6.28318f;
    while (yawDiff < -3.14159f) yawDiff += 6.28318f;
    lookYaw = tpsLookYaw + yawDiff * b;
    lookPitch = tpsLookPitch + (tdLookPitch - tpsLookPitch) * b;
  } else {
    // Overview orbit camera for menus.
    float focusX = static_cast<float>(m_map.Width()) * 0.5f;
    float focusZ = static_cast<float>(m_map.Height()) * 0.5f;

    camX = focusX + m_cameraDistance * cosf(m_cameraPitch) * sinf(m_cameraYaw);
    camY = m_cameraDistance * sinf(m_cameraPitch);
    camZ = focusZ + m_cameraDistance * cosf(m_cameraPitch) * cosf(m_cameraYaw);

    float dx = focusX - camX;
    float dy = 0.0f - camY;
    float dz = focusZ - camZ;
    float horizDist = sqrtf(dx * dx + dz * dz);
    lookYaw = atan2f(dx, dz);
    lookPitch = atan2f(dy, horizDist);
  }

  // Screen shake: add high-frequency offset that decays over time.
  if (m_shakeTimer > 0.0f) {
    float decay = m_shakeTimer / kShakeDuration; // 1→0 fade
    float t = m_stageTimer * kShakeFrequency;
    float offsetX = sinf(t * 6.28318f) * m_shakeIntensity * decay;
    float offsetY = cosf(t * 1.7f * 6.28318f) * m_shakeIntensity * decay * 0.6f;
    camX += offsetX;
    camY += offsetY;
  }

  m_camera->SetPosition(camX, camY, camZ);
  m_camera->SetYawPitch(lookYaw, lookPitch);

  // Procedural tile animation time.
  frame.gameTime = m_stageTimer;

  // Build grid tiles.
  m_map.BuildRenderItems(m_meshIds, frame.opaqueItems, frame.pointLights,
                         m_stageTimer);

  // Neon grid edge lines — thin cubes along tile borders (editor/menu only, hidden during gameplay).
  if (m_gridLineMeshId != UINT32_MAX && m_map.Width() > 0 && !inGameplay) {
    const float lineH = 0.03f;  // height above ground
    const float lineW = 0.03f;  // line thickness
    int gw = m_map.Width();
    int gh = m_map.Height();

    // Horizontal lines (along X axis, at each Z boundary).
    for (int gy = 0; gy <= gh; ++gy) {
      float z = static_cast<float>(gy) - 0.5f;
      float cx = static_cast<float>(gw) * 0.5f - 0.5f;
      XMMATRIX w = XMMatrixScaling(static_cast<float>(gw), lineW, lineW) *
                   XMMatrixTranslation(cx, lineH, z);
      frame.opaqueItems.push_back({m_gridLineMeshId, w});
    }

    // Vertical lines (along Z axis, at each X boundary).
    for (int gx = 0; gx <= gw; ++gx) {
      float x = static_cast<float>(gx) - 0.5f;
      float cz = static_cast<float>(gh) * 0.5f - 0.5f;
      XMMATRIX w = XMMatrixScaling(lineW, lineW, static_cast<float>(gh)) *
                   XMMatrixTranslation(x, lineH, cz);
      frame.opaqueItems.push_back({m_gridLineMeshId, w});
    }
  }

  // Phase 6: colored tile edge borders for hazard/special tiles (editor/menu only).
  if (!inGameplay) {
    const float bH = 0.05f;  // border height above ground
    const float bW = 0.04f;  // border thickness
    const float bLen = 1.0f; // full tile width

    for (int ty = 0; ty < m_map.Height(); ++ty) {
      for (int tx = 0; tx < m_map.Width(); ++tx) {
        const Tile &tile = m_map.At(tx, ty);
        if (tile.type == TileType::Floor || tile.type == TileType::Wall ||
            tile.type == TileType::Crumble)
          continue;
        if (tile.type == TileType::Crumble && tile.crumbleBroken)
          continue;

        // Pick border mesh by tile type.
        uint32_t borderMesh = UINT32_MAX;
        switch (tile.type) {
        case TileType::Fire:      borderMesh = m_borderOrangeMeshId; break;
        case TileType::Spike:     borderMesh = m_borderRedMeshId;    break;
        case TileType::Lightning: borderMesh = m_borderCyanMeshId;   break;
        case TileType::Ice:       borderMesh = m_borderCyanMeshId;   break;
        case TileType::Start:     borderMesh = m_borderGreenMeshId;  break;
        case TileType::Goal:      borderMesh = m_borderGoldMeshId;   break;
        default: break;
        }
        if (borderMesh == UINT32_MAX)
          continue;

        XMFLOAT3 c = m_map.TileCenter(tx, ty);

        // 4 edges: top (Z+0.5), bottom (Z-0.5), left (X-0.5), right (X+0.5).
        // Top edge (along X).
        frame.opaqueItems.push_back(
            {borderMesh, XMMatrixScaling(bLen, bW, bW) *
                             XMMatrixTranslation(c.x, bH, c.z + 0.5f)});
        // Bottom edge.
        frame.opaqueItems.push_back(
            {borderMesh, XMMatrixScaling(bLen, bW, bW) *
                             XMMatrixTranslation(c.x, bH, c.z - 0.5f)});
        // Left edge (along Z).
        frame.opaqueItems.push_back(
            {borderMesh, XMMatrixScaling(bW, bW, bLen) *
                             XMMatrixTranslation(c.x - 0.5f, bH, c.z)});
        // Right edge.
        frame.opaqueItems.push_back(
            {borderMesh, XMMatrixScaling(bW, bW, bLen) *
                             XMMatrixTranslation(c.x + 0.5f, bH, c.z)});
      }
    }
  }

  // Phase 6: player trail — fading glow marks on recently visited tiles.
  if (m_state == GridGameState::Playing || m_state == GridGameState::Paused) {
    for (int i = 0; i < kMaxTrailMarks; ++i) {
      const TrailMark &mark = m_trail[i];
      if (!mark.active)
        continue;

      // Fade out over 3 seconds.
      float fade = 1.0f - (mark.age / 3.0f);
      if (fade <= 0.0f)
        continue;

      XMFLOAT3 tc = m_map.TileCenter(mark.x, mark.y);

      // Trail plane slightly above floor, shrinking as it fades.
      float scale = 0.6f * fade;
      frame.opaqueItems.push_back(
          {m_trailMeshId,
           XMMatrixScaling(scale, 1.0f, scale) *
               XMMatrixTranslation(tc.x, 0.01f, tc.z)});

      // Trail point light (fading cyan).
      GPUPointLight trailLight{};
      trailLight.position = {tc.x, 0.3f, tc.z};
      trailLight.range = 1.5f * fade;
      trailLight.color = {0.0f, 0.3f, 0.8f};
      trailLight.intensity = 0.8f * fade;
      frame.pointLights.push_back(trailLight);
    }
  }

  // Phase 3: render towers and telegraph tiles.
  if (m_state == GridGameState::Intro || m_state == GridGameState::Playing ||
      m_state == GridGameState::Paused || m_state == GridGameState::StageComplete) {
    for (const auto &tower : m_towers) {
      TowerPhase phase = GetTowerPhase(tower);

      // Tower cone body at perimeter position.
      // Position towers just outside the grid edge based on side.
      float towerX = static_cast<float>(tower.data.x);
      float towerZ = static_cast<float>(tower.data.y);
      switch (tower.data.side) {
      case TowerSide::Left:   towerX = -1.0f; break;
      case TowerSide::Right:  towerX = static_cast<float>(m_map.Width()); break;
      case TowerSide::Top:    towerZ = -1.0f; break;
      case TowerSide::Bottom: towerZ = static_cast<float>(m_map.Height()); break;
      }

      // Idle breathing: subtle scale pulse. Firing: expand briefly.
      float towerScale = 0.5f;
      if (phase == TowerPhase::Firing) {
        towerScale = 0.65f;
      } else if (phase == TowerPhase::Warn2) {
        towerScale = 0.5f + 0.05f * sinf(m_stageTimer * 10.0f * 3.14159f);
      } else {
        // Idle pulse: gentle breathing.
        towerScale = 0.5f + 0.02f * sinf(m_stageTimer * 2.0f * 3.14159f);
      }

      XMMATRIX towerWorld =
          XMMatrixScaling(towerScale, towerScale, towerScale) *
          XMMatrixTranslation(towerX, 0.8f, towerZ);
      frame.opaqueItems.push_back({m_towerMeshId, towerWorld});

      // Tower point light (pulses brighter when firing).
      GPUPointLight towerLight{};
      towerLight.position = {towerX, 1.2f, towerZ};
      towerLight.color = {1.0f, 0.2f, 0.1f};
      if (phase == TowerPhase::Firing) {
        towerLight.range = 4.0f;
        towerLight.intensity = 3.0f;
      } else if (phase == TowerPhase::Warn2) {
        float pulse = 1.0f + 0.5f * sinf(m_stageTimer * 12.0f * 3.14159f);
        towerLight.range = 2.5f;
        towerLight.intensity = 1.2f * pulse;
      } else if (phase == TowerPhase::Warn1) {
        towerLight.range = 1.5f;
        towerLight.intensity = 0.6f;
      } else {
        towerLight.range = 1.5f;
        towerLight.intensity = 0.3f;
      }
      frame.pointLights.push_back(towerLight);

      // Telegraph tiles — only render during warning/firing phases.
      if (phase == TowerPhase::Idle)
        continue;

      uint32_t telegraphMesh = m_telegraphWarn1MeshId;
      float telegraphY = 0.05f;
      float telegraphScale = 0.85f;

      if (phase == TowerPhase::Warn2) {
        telegraphMesh = m_telegraphWarn2MeshId;
        // Pulse scale for urgency.
        float pulse = 0.85f + 0.1f * sinf(m_stageTimer * 10.0f * 3.14159f);
        telegraphScale = pulse;
        telegraphY = 0.07f;
      } else if (phase == TowerPhase::Firing) {
        telegraphMesh = m_telegraphFireMeshId;
        telegraphScale = 0.95f;
        telegraphY = 0.09f;
      }

      for (const auto &[ax, ay] : tower.attackTiles) {
        if (!m_map.InBounds(ax, ay))
          continue;

        XMFLOAT3 tc = m_map.TileCenter(ax, ay);
        XMMATRIX w = XMMatrixScaling(telegraphScale, 1.0f, telegraphScale) *
                     XMMatrixTranslation(tc.x, telegraphY, tc.z);
        frame.opaqueItems.push_back({telegraphMesh, w});
      }

      // Phase 3B: beam laser during firing.
      if (phase == TowerPhase::Firing && m_beamMeshId != UINT32_MAX) {
        // Beam fades over flash duration: thick at start, thin at end.
        float beamLife = tower.fireFlashTimer / kFireFlashDuration;
        float beamThickness = 0.08f * beamLife;
        float beamHeight = 0.5f;

        // Determine beam direction and length based on tower side.
        bool horizontal =
            (tower.data.side == TowerSide::Left ||
             tower.data.side == TowerSide::Right);

        if (horizontal) {
          // Beam stretches across the full grid width along X.
          float beamLen = static_cast<float>(m_map.Width());
          float cx = static_cast<float>(m_map.Width()) * 0.5f - 0.5f;
          float cz = static_cast<float>(tower.data.y);
          XMMATRIX bw =
              XMMatrixScaling(beamLen, beamThickness, beamThickness) *
              XMMatrixTranslation(cx, beamHeight, cz);
          frame.opaqueItems.push_back({m_beamMeshId, bw});
        } else {
          // Beam stretches across the full grid height along Z.
          float beamLen = static_cast<float>(m_map.Height());
          float cx = static_cast<float>(tower.data.x);
          float cz = static_cast<float>(m_map.Height()) * 0.5f - 0.5f;
          XMMATRIX bw =
              XMMatrixScaling(beamThickness, beamThickness, beamLen) *
              XMMatrixTranslation(cx, beamHeight, cz);
          frame.opaqueItems.push_back({m_beamMeshId, bw});
        }
      }
    }
  }

  // Render hazard beams (row/column beams that grow from thin to thick).
  if ((m_state == GridGameState::Playing || m_state == GridGameState::Paused) &&
      m_hazardBeamMeshId != UINT32_MAX) {
    for (const auto &beam : m_hazardBeams) {
      // Compute beam radius: grows from thin to thick over warn time.
      float growT = std::min(beam.timer / kBeamWarnTime, 1.0f);
      // Ease-in curve for dramatic growth.
      float easedT = growT * growT;
      float radius = kBeamMinRadius + (kBeamMaxRadius - kBeamMinRadius) * easedT;

      // After damage, beam fades out (shrinks).
      if (beam.timer > kBeamWarnTime) {
        float fadeT = (beam.timer - kBeamWarnTime) / (kBeamLifetime - kBeamWarnTime);
        radius *= (1.0f - fadeT);
      }

      if (radius < 0.001f) continue;

      float beamY = kBeamFloatHeight;

      if (beam.isRow) {
        // Row beam stretches across full grid width along X.
        float beamLen = static_cast<float>(m_map.Width());
        float cx = static_cast<float>(m_map.Width()) * 0.5f - 0.5f;
        float cz = static_cast<float>(beam.index);
        XMMATRIX bw = XMMatrixScaling(beamLen, radius, radius) *
                      XMMatrixTranslation(cx, beamY, cz);
        frame.opaqueItems.push_back({m_hazardBeamMeshId, bw});
      } else {
        // Column beam stretches across full grid height along Z.
        float beamLen = static_cast<float>(m_map.Height());
        float cx = static_cast<float>(beam.index);
        float cz = static_cast<float>(m_map.Height()) * 0.5f - 0.5f;
        XMMATRIX bw = XMMatrixScaling(radius, radius, beamLen) *
                      XMMatrixTranslation(cx, beamY, cz);
        frame.opaqueItems.push_back({m_hazardBeamMeshId, bw});
      }

      // Add a glow light along the beam.
      GPUPointLight beamLight{};
      if (beam.isRow) {
        beamLight.position = {static_cast<float>(m_map.Width()) * 0.5f - 0.5f,
                              beamY + 0.5f,
                              static_cast<float>(beam.index)};
      } else {
        beamLight.position = {static_cast<float>(beam.index),
                              beamY + 0.5f,
                              static_cast<float>(m_map.Height()) * 0.5f - 0.5f};
      }
      float lightIntensity = 2.0f * std::min(beam.timer / kBeamWarnTime, 1.0f);
      if (beam.timer > kBeamWarnTime) {
        float fadeT = (beam.timer - kBeamWarnTime) / (kBeamLifetime - kBeamWarnTime);
        lightIntensity *= (1.0f - fadeT);
      }
      beamLight.range = 4.0f;
      beamLight.color = {0.6f, 0.1f, 1.0f};  // purple glow
      beamLight.intensity = lightIntensity;
      frame.pointLights.push_back(beamLight);
    }
  }

  // Render player and cargo during gameplay.
  if (m_state == GridGameState::Intro || m_state == GridGameState::Playing ||
      m_state == GridGameState::Paused ||
      m_state == GridGameState::StageComplete || m_state == GridGameState::StageFail) {
    // Player model: VRM is ~1.6m tall, scale to fit ~0.8 units tall on tile.
    // Feet at Y=0.01 (just above tile surface). Rotate to face movement direction.
    const float playerScale = 1.0f;
    XMMATRIX playerWorld = XMMatrixScaling(playerScale, playerScale, playerScale) *
                           XMMatrixRotationY(m_playerVisualYaw) *
                           XMMatrixTranslation(m_playerVisualPos.x, 0.01f,
                                               m_playerVisualPos.z);
    frame.opaqueItems.push_back({m_playerMeshId, playerWorld});

    // Player glow light (pulsing cyan).
    float t = m_stageTimer;
    float playerPulse = 1.0f + 0.4f * sinf(t * 3.0f * 3.14159f);
    GPUPointLight playerLight{};
    playerLight.position = {m_playerVisualPos.x, 0.8f, m_playerVisualPos.z};
    playerLight.range = 3.5f;
    playerLight.color = {0.0f, 0.5f, 1.0f};
    playerLight.intensity = 1.5f * playerPulse;
    frame.pointLights.push_back(playerLight);

    // Cargo cube (scale 0.7).
    // During drop-off, use animated Y from m_cargoVisualPos; otherwise sits at Y=0.5.
    float cargoY = m_cargoDropping ? (m_cargoVisualPos.y + 0.5f) : 0.5f;
    XMMATRIX cargoWorld = XMMatrixScaling(0.7f, 0.7f, 0.7f) *
                          XMMatrixTranslation(m_cargoVisualPos.x, cargoY,
                                              m_cargoVisualPos.z);
    frame.opaqueItems.push_back({m_cargoMeshId, cargoWorld});

    // Cargo glow light (subtle gold pulse — reduced to show texture detail).
    // Hide light when cargo is dropping below the stage.
    if (!m_cargoDropping) {
      float cargoPulse = 1.0f + 0.15f * sinf(t * 1.6f * 3.14159f);
      GPUPointLight cargoLight{};
      cargoLight.position = {m_cargoVisualPos.x, 0.8f, m_cargoVisualPos.z};
      cargoLight.range = 2.0f;
      cargoLight.color = {1.0f, 0.6f, 0.0f};
      cargoLight.intensity = 0.5f * cargoPulse;
      frame.pointLights.push_back(cargoLight);
    }

    // Goal beacon light: find first Goal tile and place a tall bright light.
    for (int gy = 0; gy < m_map.Height(); ++gy) {
      for (int gx = 0; gx < m_map.Width(); ++gx) {
        if (m_map.At(gx, gy).type == TileType::Goal) {
          float goalPulse = 1.0f + 0.25f * sinf(t * 2.0f * 3.14159f);
          XMFLOAT3 gc = m_map.TileCenter(gx, gy);
          GPUPointLight goalLight{};
          goalLight.position = {gc.x, 1.5f, gc.z};
          goalLight.range = 5.0f;
          goalLight.color = {1.0f, 0.8f, 0.1f};
          goalLight.intensity = 3.0f * goalPulse;
          frame.pointLights.push_back(goalLight);
        }
      }
    }

    // Phase 6: push game particle emitters into frame.
    for (auto &em : m_gameEmitters)
      frame.emitters.push_back(em.get());

    // Phase 8A: push burst emitters into frame.
    for (auto &em : m_burstEmitters)
      frame.emitters.push_back(em.get());
  }

  // HUD: show timer, move count, and HP during play.
  if (m_state == GridGameState::Playing) {
    PushNeonTheme();

    ImGui::SetNextWindowPos(ImVec2(10, 10));
    ImGui::SetNextWindowSize(ImVec2(280, 0));
    ImGui::Begin("##HUD", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_AlwaysAutoResize);

    // Stage name (if loaded from editor).
    if (m_hasStageData && !m_loadedStage.name.empty()) {
      ImGui::SetWindowFontScale(1.1f);
      ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "%s",
                         m_loadedStage.name.c_str());
      ImGui::SetWindowFontScale(1.0f);
      ImGui::Spacing();
    }

    // Time.
    int mins = static_cast<int>(m_stageTimer) / 60;
    int secs = static_cast<int>(m_stageTimer) % 60;
    ImGui::TextColored(ImVec4(0.4f, 0.6f, 0.7f, 0.9f), "TIME");
    ImGui::SameLine();
    ImGui::SetWindowFontScale(1.2f);
    ImGui::TextColored(ImVec4(0.9f, 0.95f, 1.0f, 1.0f), " %02d:%02d", mins, secs);
    ImGui::SetWindowFontScale(1.0f);

    // Moves.
    ImGui::TextColored(ImVec4(0.6f, 0.5f, 0.3f, 0.9f), "MOVES");
    ImGui::SameLine();
    ImGui::SetWindowFontScale(1.2f);
    ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.8f, 1.0f), " %d", m_moveCount);
    ImGui::SetWindowFontScale(1.0f);

    // HP as hearts.
    ImGui::TextColored(ImVec4(0.6f, 0.2f, 0.2f, 0.9f), "HP");
    ImGui::SameLine();
    char hpBuf[32];
    int pos = 0;
    for (int i = 0; i < m_playerMaxHP; ++i) {
      if (i < m_playerHP) {
        hpBuf[pos++] = '\xe2'; hpBuf[pos++] = '\x99'; hpBuf[pos++] = '\xa5';
      } else {
        hpBuf[pos++] = ' '; hpBuf[pos++] = '-';
      }
      hpBuf[pos++] = ' ';
    }
    hpBuf[pos] = '\0';

    // Flash hearts white/red on damage.
    ImVec4 hpColor;
    if (m_damageFlashTimer > 0.0f) {
      float flash = sinf(m_uiTimer * 20.0f);
      hpColor = (flash > 0.0f) ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
                                : ImVec4(1.0f, 0.15f, 0.15f, 1.0f);
    } else {
      hpColor = ImVec4(1.0f, 0.15f, 0.15f, 1.0f);
    }
    ImGui::TextColored(hpColor, " %s", hpBuf);

    ImGui::End();

    // Camera mode indicator (top-right).
    if (m_topDownBlend > 0.01f) {
      ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 140, 10));
      ImGui::SetNextWindowSize(ImVec2(0, 0));
      ImGui::Begin("##CamMode", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_AlwaysAutoResize);
      ImGui::TextColored(ImVec4(0.6f, 0.3f, 1.0f, 1.0f), "TOP-DOWN");
      ImGui::End();
    }

    // Pull mode badge.
    float indicatorY = (m_hasStageData && !m_loadedStage.name.empty()) ? 130.0f : 110.0f;
    if (GetAsyncKeyState('E') & 0x8000) {
      ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.12f, 0.0f, 0.9f));
      ImGui::SetNextWindowPos(ImVec2(10, indicatorY));
      ImGui::SetNextWindowSize(ImVec2(0, 0));
      ImGui::Begin("##PullMode", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_AlwaysAutoResize);
      ImGui::SetWindowFontScale(1.1f);
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), " PULL ");
      ImGui::SetWindowFontScale(1.0f);
      ImGui::End();
      ImGui::PopStyleColor();
      indicatorY += 40.0f;
    }

    // Stun indicator (flashing).
    if (m_stunned) {
      float stunAlpha = 0.3f + 0.7f * fabsf(sinf(m_uiTimer * 15.0f));
      ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.02f, 0.02f, 0.9f));
      ImGui::SetNextWindowPos(ImVec2(10, indicatorY));
      ImGui::SetNextWindowSize(ImVec2(0, 0));
      ImGui::Begin("##StunIndicator", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_AlwaysAutoResize);
      ImGui::SetWindowFontScale(1.1f);
      ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, stunAlpha), " STUNNED! ");
      ImGui::SetWindowFontScale(1.0f);
      ImGui::End();
      ImGui::PopStyleColor();
    }

    PopNeonTheme();

    // Damage flash overlay (kept separate from neon theme).
    if (m_damageFlashTimer > 0.0f) {
      float alpha = m_damageFlashTimer / 0.25f * 0.3f;
      ImGui::SetNextWindowPos(ImVec2(0, 0));
      ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
      ImGui::Begin("##DamageFlash", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
      ImGui::GetWindowDrawList()->AddRectFilled(
          ImVec2(0, 0), ImGui::GetIO().DisplaySize,
          ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.0f, 0.0f, alpha)));
      ImGui::End();
    }
  }
}
