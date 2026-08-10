#include "FeatureRegistry.h"
#include "FeatureWorld.h"

#include <Windows.h>

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

  std::wcout << L"[PASS] 42 registry mappings, " << world.Entities().size()
             << L" real 3D entities, collision/state simulation passed.\n";
  return 0;
}
