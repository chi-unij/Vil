#include "FeatureRegistry.h"
#include "FeatureWorld.h"

#include <Windows.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <set>
#include <string_view>

namespace {

int Fail(const std::wstring &message) {
  std::wcerr << L"[FAIL] " << message << L'\n';
  return 1;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc < 2)
    return Fail(L"feature.md path is required");

  FeatureRegistry registry;
  const auto load = registry.Load(std::filesystem::path(argv[1]));
  if (!load.ok)
    return Fail(load.error);
  if (registry.Entries().size() != 42)
    return Fail(L"expected 42 real Feature Lab mappings, got " +
                std::to_wstring(registry.Entries().size()));

  std::set<std::string> ids;
  for (const auto &entry : registry.Entries()) {
    if (!ids.insert(entry.id).second)
      return Fail(L"duplicate Lab ID");
    if (entry.name.empty() || entry.category.empty() || entry.description.empty())
      return Fail(L"incomplete Lab registry entry: " +
                  LabUtf8ToWide(entry.id));
  }

  if (registry.Enabled("wireframe"))
    return Fail(L"wireframe must default to Off");
  if (registry.Enabled("camera_auto_move"))
    return Fail(L"automatic camera movement must default to Off");
  if (!registry.Enabled("deferred") || !registry.Enabled("water") ||
      !registry.Enabled("boss"))
    return Fail(L"required core systems must default to On");

  registry.SetAll(false);
  if (registry.EnabledCount() != 0)
    return Fail(L"SetAll(false) failed");
  registry.ResetDefaults();
  if (registry.EnabledCount() != 40)
    return Fail(L"ResetDefaults expected 40 enabled features");

  FeatureRegistry editorRegistry;
  const auto editorLoad = editorRegistry.Load(
      std::filesystem::path(argv[1]),
      "## VillienEditor Technology Showcase Registry");
  if (!editorLoad.ok)
    return Fail(editorLoad.error);
  const std::set<std::string> validRoutes{
      "Scene",       "Environment", "Rendering", "World",
      "VFX",         "Camera",      "Overworld", "Tavern",
      "Boss",        "Observed",    "Legacy",
  };
  const std::set<std::string> validDefaultModes{"On", "Off", "Auto"};
  std::set<std::string> editorIds;
  for (const auto &entry : editorRegistry.Entries()) {
    if (!editorIds.insert(entry.id).second)
      return Fail(L"duplicate VillienEditor showcase ID: " +
                  LabUtf8ToWide(entry.id));
    if (!validRoutes.contains(entry.route))
      return Fail(L"VillienEditor showcase has invalid route for ID: " +
                  LabUtf8ToWide(entry.id));
    if (!validDefaultModes.contains(entry.defaultMode))
      return Fail(L"VillienEditor showcase has invalid Default for ID: " +
                  LabUtf8ToWide(entry.id));
  }
  constexpr auto requiredEditorIds = std::to_array<std::string_view>({
      "hybrid_dxr",
      "dxr_blas_tlas",
      "dxr_inline_rayquery",
      "dxr_hit_lighting",
      "dxr_ssr_fallback",
      "reflection_receiver_contract",
      "ibl_irradiance",
      "ibl_prefilter",
      "ibl_brdf_lut",
      "deferred_gbuffer_4mrt",
      "csm",
      "ssao_bilateral_blur",
      "ssr_ray_march",
      "spot_lights",
      "hdri_sky",
      "pbr_import",
      "material_pom",
      "bloom_mip_chain",
      "taa",
      "velocity_buffer",
      "motion_blur",
      "depth_of_field",
      "ink_edge",
      "material_wet_puddle",
      "gltf_loader",
      "glb_loader",
      "vrm_loader",
      "skeleton_import",
      "gpu_skinning",
      "player_crossfade",
      "tavern_pose_isolation",
      "editor_shader_reload",
      "editor_reflection_inspector",
      "editor_workspace",
      "tavern_route",
      "player_scaled_collision",
      "overworld_heightfield",
      "terrain_shared_grounding",
      "terrain_flatten_zones",
  });
  if (editorRegistry.Entries().size() < 100)
    return Fail(L"VillienEditor showcase registry is unexpectedly small");
  for (const auto id : requiredEditorIds) {
    const auto found = std::find_if(
        editorRegistry.Entries().begin(), editorRegistry.Entries().end(),
        [id](const LabFeatureEntry &entry) { return entry.id == id; });
    if (found == editorRegistry.Entries().end())
      return Fail(L"VillienEditor showcase is missing required ID: " +
                  LabUtf8ToWide(id));
  }
  const auto hybridDxr = std::find_if(
      editorRegistry.Entries().begin(), editorRegistry.Entries().end(),
      [](const LabFeatureEntry &entry) { return entry.id == "hybrid_dxr"; });
  if (hybridDxr == editorRegistry.Entries().end() ||
      hybridDxr->defaultMode != "Auto")
    return Fail(L"Hybrid DXR must use the capability-gated Auto default");

  FeatureWorld world;
  world.Initialize();
  if (world.Entities().size() < 40)
    return Fail(L"3D world entity count is unexpectedly small");

  std::set<std::string> runtimeBindings;
  for (const auto &entity : world.Entities())
    runtimeBindings.insert(entity.featureId);
  constexpr std::string_view systemBindings[] = {
      "water_waves",       "ssr",          "fog",
      "time_of_day",       "deferred",     "directional_light",
      "point_lights",      "shadows",      "ssao",
      "hdr",               "bloom",        "tonemap",
      "fxaa",              "wireframe",    "pbr",
      "normal_mapping",    "emissive",     "procedural_material",
      "uv_animation",      "entity_system", "animation",
      "collision",         "camera_orbit", "camera_auto_move",
      "boss",
      "meteor",            "laser",        "sanctuary",
      "phase2",            "auto_demo",
  };
  for (const auto id : systemBindings)
    runtimeBindings.emplace(id);
  for (const auto &entry : registry.Entries()) {
    if (!runtimeBindings.contains(entry.id))
      return Fail(L"Lab ID has no declared runtime binding: " +
                  LabUtf8ToWide(entry.id));
  }

  const auto start = world.CollisionSpherePosition();
  for (int frame = 0; frame < 1200; ++frame)
    world.Update(1.0f / 60.0f, registry);
  const auto end = world.CollisionSpherePosition();
  if (std::abs(end.x - start.x) < 0.01f &&
      std::abs(end.z - start.z) < 0.01f)
    return Fail(L"collision simulation did not move");
  if (world.CollisionCount() == 0)
    return Fail(L"collision simulation produced no collision events");
  if (!world.Frame().phaseTwo)
    return Fail(L"phase 2 gate did not activate during simulation");

  const auto previousAttack = world.Frame().attack;
  world.ForceNextAttack(registry);
  if (world.Frame().attack == previousAttack)
    return Fail(L"ForceNextAttack did not advance attack family");

  std::wcout << L"[PASS] 42 legacy mappings, "
             << editorRegistry.Entries().size()
             << L" VillienEditor showcase mappings, " << world.Entities().size()
             << L" real 3D entities, collision/state simulation passed.\n";
  return 0;
}
