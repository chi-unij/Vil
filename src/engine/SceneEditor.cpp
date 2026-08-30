// ======================================
// File: SceneEditor.cpp
// Purpose: ImGui editor panels for entity management, property inspection,
//          scene save/load, transform gizmo, undo/redo, duplication.
//          Milestone 4 Phase 0+1.
// ======================================

#include "SceneEditor.h"
#include "../Camera.h"
#include "Commands.h"
#include "../DxContext.h"
#include "../ProceduralMesh.h"
#include "../RenderPass.h"
#include "../gridgame/GridEditorCommands.h"
#include "../gridgame/GridMaterials.h"
#include "../gridgame/StageSerializer.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include <windows.h>
#include <commdlg.h>

namespace {

struct EditorWorkspaceLayout {
  float menuHeight = 0.0f;
  float toolbarHeight = 0.0f;
  float leftWidth = 0.0f;
  float rightWidth = 0.0f;
  float bottomHeight = 0.0f;
  float sceneObjectsHeight = 0.0f;
  EditorViewportRect viewport;
};

struct EditorWorkspaceState {
  float leftWidth = 290.0f;
  float rightWidth = 380.0f;
  float bottomHeight = 270.0f;
  float sceneObjectsRatio = 0.43f;
  int activeSplitter = 0;
};

EditorWorkspaceState g_editorWorkspace;

EditorWorkspaceLayout BuildEditorWorkspaceLayout() {
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  EditorWorkspaceLayout layout;
  layout.menuHeight = ImGui::GetFrameHeight();
  layout.toolbarHeight = 42.0f;

  // Keep the Scene View usable on smaller windows while still giving the
  // object list and Inspector enough room for real editing controls.
  const float desiredSceneWidth = display.x < 1000.0f ? 260.0f : 420.0f;
  const float minSceneWidth =
      std::clamp(desiredSceneWidth, 1.0f, std::max(1.0f, display.x * 0.70f));
  const float availableSideWidth = std::max(2.0f, display.x - minSceneWidth);
  constexpr float kPreferredLeftMin = 190.0f;
  constexpr float kPreferredRightMin = 250.0f;
  constexpr float kPreferredSideTotal =
      kPreferredLeftMin + kPreferredRightMin;
  if (availableSideWidth >= kPreferredSideTotal) {
    layout.leftWidth =
        std::clamp(g_editorWorkspace.leftWidth, kPreferredLeftMin,
                   availableSideWidth - kPreferredRightMin);
    layout.rightWidth =
        std::clamp(g_editorWorkspace.rightWidth, kPreferredRightMin,
                   availableSideWidth - layout.leftWidth);
  } else {
    // At very small window sizes, scale both panels proportionally instead of
    // passing an invalid min/max range to std::clamp.
    layout.leftWidth =
        availableSideWidth * (kPreferredLeftMin / kPreferredSideTotal);
    layout.rightWidth = availableSideWidth - layout.leftWidth;
  }

  const float maxBottom =
      std::max(150.0f, display.y - layout.menuHeight - layout.toolbarHeight -
                            170.0f);
  layout.bottomHeight =
      std::clamp(g_editorWorkspace.bottomHeight, 150.0f, maxBottom);

  const float contentHeight = std::max(1.0f, display.y - layout.menuHeight);
  layout.sceneObjectsHeight = std::clamp(
      contentHeight * g_editorWorkspace.sceneObjectsRatio, 180.0f,
      std::max(180.0f, contentHeight - 180.0f));

  layout.viewport.x = layout.leftWidth;
  layout.viewport.y = layout.menuHeight + layout.toolbarHeight;
  layout.viewport.width =
      std::max(1.0f, display.x - layout.leftWidth - layout.rightWidth);
  layout.viewport.height =
      std::max(1.0f, display.y - layout.viewport.y - layout.bottomHeight);
  return layout;
}

constexpr ImGuiWindowFlags kFixedPanelFlags =
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

constexpr ImGuiWindowFlags kToolbarFlags =
    kFixedPanelFlags | ImGuiWindowFlags_NoTitleBar |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
    ImGuiWindowFlags_NoBringToFrontOnFocus;

constexpr ImGuiWindowFlags kSceneViewportFlags =
    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus |
    ImGuiWindowFlags_NoNav;

std::string TrimMarkdownText(std::string value) {
  const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
  const auto first =
      std::find_if_not(value.begin(), value.end(), isSpace);
  const auto last =
      std::find_if_not(value.rbegin(), value.rend(), isSpace).base();
  if (first >= last)
    return {};
  return std::string(first, last);
}

void ReplaceAll(std::string &value, std::string_view from,
                std::string_view to) {
  size_t position = 0;
  while ((position = value.find(from, position)) != std::string::npos) {
    value.replace(position, from.size(), to);
    position += to.size();
  }
}

std::string MakeAsciiUiText(std::string value) {
  // The current ImGui atlas is ASCII-only. Preserve common technical
  // punctuation, then omit unsupported CJK prose instead of rendering ????
  // glyphs throughout the Editor.
  ReplaceAll(value, "\xEF\xBC\x8F", "/");      // full-width slash
  ReplaceAll(value, "\xE2\x86\x92", " -> "); // right arrow
  ReplaceAll(value, "\xC3\x97", "x");          // multiplication sign
  ReplaceAll(value, "\xE2\x89\xA4", "<=");   // less-than-or-equal
  ReplaceAll(value, "\xE3\x80\x81", ", ");   // ideographic comma

  std::string ascii;
  ascii.reserve(value.size());
  bool skippedNonAscii = false;
  for (unsigned char c : value) {
    if (c < 0x80) {
      if (skippedNonAscii && !ascii.empty() && ascii.back() != ' ' &&
          !std::isspace(c)) {
        ascii.push_back(' ');
      }
      skippedNonAscii = false;
      if (c != '`')
        ascii.push_back(static_cast<char>(c));
    } else {
      skippedNonAscii = true;
    }
  }

  std::string collapsed;
  collapsed.reserve(ascii.size());
  bool previousSpace = false;
  for (unsigned char c : ascii) {
    const bool space = std::isspace(c) != 0;
    if (!space || !previousSpace)
      collapsed.push_back(space ? ' ' : static_cast<char>(c));
    previousSpace = space;
  }
  return TrimMarkdownText(std::move(collapsed));
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value;
}

std::vector<std::string> SplitMarkdownRow(const std::string &line) {
  std::vector<std::string> cells;
  size_t begin = (!line.empty() && line.front() == '|') ? 1 : 0;
  while (begin <= line.size()) {
    const size_t end = line.find('|', begin);
    if (end == std::string::npos) {
      cells.push_back(TrimMarkdownText(line.substr(begin)));
      break;
    }
    cells.push_back(TrimMarkdownText(line.substr(begin, end - begin)));
    begin = end + 1;
    if (begin == line.size())
      break;
  }
  return cells;
}

bool IsMarkdownSeparatorRow(const std::string &line) {
  if (line.empty() || line.front() != '|')
    return false;
  bool sawDash = false;
  for (unsigned char c : line) {
    if (c == '-')
      sawDash = true;
    else if (c != '|' && c != ':' && std::isspace(c) == 0)
      return false;
  }
  return sawDash;
}

int FindMarkdownColumn(const std::vector<std::string> &headers,
                       std::string_view query) {
  for (int i = 0; i < static_cast<int>(headers.size()); ++i) {
    const std::string header = LowerAscii(MakeAsciiUiText(headers[i]));
    if (header == query || header.find(query) != std::string::npos)
      return i;
  }
  return -1;
}

enum class FeatureRoute {
  SceneObjects,
  Environment,
  Rendering,
  World,
  Vfx,
  Camera,
  Overworld,
  Tavern,
  Boss,
  Observed,
  Legacy,
  Unmapped,
};

// feature_catalog.md の Route 列を唯一の正式な Showcase 導線として保持する。
// ヘッダーを変更せず、旧 standalone registry の互換導線だけを fallback に残す。
std::unordered_map<std::string, FeatureRoute> g_featureShowcaseRoutes;

std::string NormalizeRouteKey(std::string value) {
  value = LowerAscii(MakeAsciiUiText(std::move(value)));
  value.erase(std::remove_if(value.begin(), value.end(),
                             [](unsigned char c) {
                               return std::isalnum(c) == 0;
                             }),
              value.end());
  return value;
}

FeatureRoute RouteFromCatalogCell(const std::string &routeCell) {
  const std::string route = NormalizeRouteKey(routeCell);
  if (route.empty() || route == "unmapped")
    return FeatureRoute::Unmapped;
  if (route.find("legacy") != std::string::npos)
    return FeatureRoute::Legacy;
  if (route.find("overworld") != std::string::npos)
    return FeatureRoute::Overworld;
  if (route.find("tavern") != std::string::npos)
    return FeatureRoute::Tavern;
  if (route.find("boss") != std::string::npos)
    return FeatureRoute::Boss;
  if (route.find("observed") != std::string::npos ||
      route.find("core") != std::string::npos ||
      route.find("gameview") != std::string::npos)
    return FeatureRoute::Observed;
  if (route.find("environment") != std::string::npos ||
      route == "lighting")
    return FeatureRoute::Environment;
  if (route.find("render") != std::string::npos ||
      route.find("postprocess") != std::string::npos ||
      route.find("reflection") != std::string::npos)
    return FeatureRoute::Rendering;
  if (route.find("vfx") != std::string::npos ||
      route.find("animation") != std::string::npos ||
      route.find("effect") != std::string::npos)
    return FeatureRoute::Vfx;
  if (route.find("camera") != std::string::npos)
    return FeatureRoute::Camera;
  if (route.find("world") != std::string::npos || route == "water" ||
      route == "physics")
    return FeatureRoute::World;
  if (route.find("scene") != std::string::npos ||
      route.find("inspector") != std::string::npos ||
      route.find("material") != std::string::npos || route == "editor")
    return FeatureRoute::SceneObjects;
  return FeatureRoute::Unmapped;
}

FeatureRoute RouteForShowcaseId(std::string_view id) {
  const std::string normalizedId = LowerAscii(std::string(id));
  const auto catalogRoute = g_featureShowcaseRoutes.find(normalizedId);
  if (catalogRoute != g_featureShowcaseRoutes.end())
    return catalogRoute->second;

  // 旧 registry を表示する過去データ向けの互換経路。
  if (id == "fog" || id == "wireframe" || id == "camera_auto_move" ||
      id == "auto_demo")
    return FeatureRoute::Legacy;

  if (id == "ground" || id == "environment" ||
      id == "material_gallery" || id == "point_lights" || id == "pbr" ||
      id == "normal_mapping" || id == "emissive" ||
      id == "entity_system" || id == "grid")
    return FeatureRoute::SceneObjects;

  if (id == "rain" || id == "time_of_day" ||
      id == "directional_light")
    return FeatureRoute::Environment;

  if (id == "water" || id == "water_waves" || id == "collision_debug")
    return FeatureRoute::World;

  if (id == "shadows" || id == "ssao" || id == "bloom" ||
      id == "fxaa")
    return FeatureRoute::Rendering;

  if (id == "ssr" || id == "procedural_material" ||
      id == "uv_animation")
    return FeatureRoute::Overworld;
  if (id == "deferred" || id == "hdr" || id == "tonemap" ||
      id == "gpu_instancing" || id == "collision")
    return FeatureRoute::Observed;

  if (id == "animation" || id == "particles")
    return FeatureRoute::Vfx;
  if (id == "camera_orbit")
    return FeatureRoute::Camera;
  if (id == "boss" || id == "meteor" || id == "laser" ||
      id == "sanctuary" || id == "mirror_charges" ||
      id == "phone_hologram" || id == "counter_vfx" || id == "phase2")
    return FeatureRoute::Boss;
  return FeatureRoute::Unmapped;
}

enum class InventoryCoverage {
  Active,
  Available,
  Experimental,
  Unavailable,
  Legacy,
};

InventoryCoverage CoverageForInventory(const FeatureInventoryEntry &entry) {
  const std::string status = LowerAscii(entry.status);
  const std::string section = LowerAscii(entry.section);
  if (section.find("standalone 3d feature lab registry") !=
      std::string::npos)
    return InventoryCoverage::Legacy;
  if (status.find("not implemented") != std::string::npos)
    return InventoryCoverage::Unavailable;
  if (status.find("present but inactive") != std::string::npos ||
      status.find("experimental") != std::string::npos)
    return InventoryCoverage::Experimental;
  if (status.find("implemented") != std::string::npos ||
      status.find("dev-only") != std::string::npos)
    return InventoryCoverage::Available;
  return InventoryCoverage::Active;
}

} // namespace

// Names matching MeshSourceType enum order.
static const char *kMeshSourceTypeNames[] = {
    "Cube", "Plane", "Cylinder", "Cone", "Sphere", "glTF File"};

// ---- Material presets (Phase 2) ----

struct MaterialPreset {
  const char *name;
  DirectX::XMFLOAT4 baseColorFactor;
  float metallicFactor;
  float roughnessFactor;
  DirectX::XMFLOAT3 emissiveFactor;
};

static const MaterialPreset kPresets[] = {
    {"Default",       {1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.8f, {0.0f, 0.0f, 0.0f}},
    {"Metal",         {0.8f, 0.8f, 0.85f, 1.0f}, 1.0f, 0.3f, {0.0f, 0.0f, 0.0f}},
    {"Plastic",       {1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.5f, {0.0f, 0.0f, 0.0f}},
    {"Wood",          {0.55f, 0.35f, 0.15f, 1.0f}, 0.0f, 0.7f, {0.0f, 0.0f, 0.0f}},
    {"Emissive Glow", {0.0f, 0.0f, 0.0f, 1.0f}, 0.0f, 0.5f, {1.0f, 0.5f, 0.1f}},
    {"Mirror",        {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f, 0.05f, {0.0f, 0.0f, 0.0f}},
    {"Rough Stone",   {0.5f, 0.5f, 0.45f, 1.0f}, 0.0f, 0.95f, {0.0f, 0.0f, 0.0f}},
};
static constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

// Texture slot names for the editor UI.
static const char *kTexSlotNames[] = {
    "BaseColor", "Normal", "MetalRough", "AO", "Emissive", "Height"};

static bool OpenTextureFileDialog(char *outPath, size_t maxLen) {
  OPENFILENAMEA ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFilter =
      "Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files\0*.*\0";
  ofn.lpstrFile = outPath;
  ofn.nMaxFile = static_cast<DWORD>(maxLen);
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
  outPath[0] = '\0';
  return GetOpenFileNameA(&ofn) != 0;
}

void SceneEditor::DrawUI(Scene &scene, DxContext &dx,
                          const DirectX::XMMATRIX &view,
                          const DirectX::XMMATRIX &proj,
                          bool *iblEnabled,
                          StageData *editStage,
                          Camera *cam,
                          EditorRuntimeBindings *runtime,
                          bool scenePlaying) {
  if (!m_featureInventoryLoaded)
    LoadFeatureInventory();

  // Process hotkeys first (undo/redo/duplicate/gizmo mode).
  if (!scenePlaying)
    ProcessHotkeys(scene, dx);

  const EditorWorkspaceLayout layout = BuildEditorWorkspaceLayout();
  m_viewportRect = layout.viewport;
  m_gizmo.SetViewportRect(m_viewportRect.x, m_viewportRect.y,
                          m_viewportRect.width, m_viewportRect.height);

  // Draw the fixed editor workspace around the live DX12 Scene View.
  DrawMenuBar(scene, dx);
  DrawSceneViewport(dx);

  // Gizmo update uses the exact ImGui image rectangle shown above.
  if (!scenePlaying)
    m_gizmo.Update(scene, m_selectedEntity, view, proj, m_history);

  DrawWorkspaceToolbar(scene, runtime, scenePlaying);
  DrawEntityList(scene, dx);
  DrawInspector(scene, dx);
  DrawRuntimeSystemsPanel(scene, dx, cam, iblEnabled, runtime);
  DrawAssetBrowser(scene, dx);
  DrawConsolePanel(scene);
  if (m_gridEditorOpen && editStage)
    DrawGridEditorPanel(*editStage);
  DrawWorkspaceSplitters();
}

void SceneEditor::LoadFeatureInventory() {
  m_featureInventory.clear();
  m_featureShowcase.clear();
  g_featureShowcaseRoutes.clear();
  m_featureInventoryLoaded = true;

  std::error_code ec;
  std::filesystem::path searchRoot = std::filesystem::current_path(ec);
  std::filesystem::path featurePath;
  std::filesystem::path catalogPath;
  for (int depth = 0; !searchRoot.empty() && depth < 8; ++depth) {
    const std::filesystem::path featureCandidate = searchRoot / "feature.md";
    const std::filesystem::path catalogCandidate =
        searchRoot / "featuretools" / "feature_catalog.md";
    if (featurePath.empty() &&
        std::filesystem::is_regular_file(featureCandidate, ec))
      featurePath = featureCandidate;
    ec.clear();
    if (catalogPath.empty() &&
        std::filesystem::is_regular_file(catalogCandidate, ec))
      catalogPath = catalogCandidate;
    ec.clear();
    if (!featurePath.empty() && !catalogPath.empty())
      break;
    const std::filesystem::path parent = searchRoot.parent_path();
    if (parent == searchRoot)
      break;
    searchRoot = parent;
  }

  const auto readMarkdown = [](const std::filesystem::path &path,
                               std::vector<std::string> &lines) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
      return false;
    std::string line;
    while (std::getline(input, line)) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      lines.push_back(std::move(line));
    }
    return true;
  };

  const auto headingText = [](const std::string &line) {
    size_t markerCount = 0;
    while (markerCount < line.size() && line[markerCount] == '#')
      ++markerCount;
    if (markerCount < 2 || markerCount >= line.size() ||
        line[markerCount] != ' ')
      return std::string{};
    return TrimMarkdownText(line.substr(markerCount + 1));
  };

  bool featureReadFailed = false;
  if (!featurePath.empty()) {
    std::vector<std::string> lines;
    if (!readMarkdown(featurePath, lines)) {
      featureReadFailed = true;
    } else {
      std::string section;
      for (size_t i = 0; i < lines.size(); ++i) {
        const std::string heading = headingText(lines[i]);
        if (!heading.empty()) {
          section = heading;
          continue;
        }
        if (lines[i].empty() || lines[i].front() != '|' ||
            i + 1 >= lines.size() ||
            !IsMarkdownSeparatorRow(lines[i + 1]))
          continue;

        const std::string normalizedSection =
            LowerAscii(MakeAsciiUiText(section));
        // 旧 Standalone registry は互換資料であり、正式 Showcase ではない。
        if (normalizedSection.find("standalone 3d feature lab registry") !=
                std::string::npos ||
            normalizedSection.find(
                "villieneditor technology showcase registry") !=
                std::string::npos)
          continue;

        const std::vector<std::string> headers = SplitMarkdownRow(lines[i]);
        const int categoryColumn = FindMarkdownColumn(headers, "category");
        const int statusColumn = FindMarkdownColumn(headers, "status");
        int featureColumn = FindMarkdownColumn(headers, "feature");
        // Gameplay VFX は "VFX / Emitter" 列を使用する。
        if (featureColumn < 0 && statusColumn >= 0)
          featureColumn = FindMarkdownColumn(headers, "vfx");
        if (featureColumn < 0 || statusColumn < 0)
          continue;

        const int sourceColumn = FindMarkdownColumn(headers, "source");
        int detailColumn = FindMarkdownColumn(headers, "description");
        if (detailColumn < 0)
          detailColumn = FindMarkdownColumn(headers, "detail");
        if (detailColumn < 0)
          detailColumn = sourceColumn > 0
                             ? sourceColumn - 1
                             : static_cast<int>(headers.size()) - 1;

        size_t row = i + 2;
        for (; row < lines.size() && !lines[row].empty() &&
               lines[row].front() == '|';
             ++row) {
          const std::vector<std::string> cells = SplitMarkdownRow(lines[row]);
          const auto cell = [&cells](int index) -> std::string {
            if (index < 0 || index >= static_cast<int>(cells.size()))
              return {};
            return cells[index];
          };

          FeatureInventoryEntry entry;
          entry.section = MakeAsciiUiText(section);
          entry.category = MakeAsciiUiText(cell(categoryColumn));
          entry.feature = MakeAsciiUiText(cell(featureColumn));
          entry.status = MakeAsciiUiText(cell(statusColumn));
          entry.source = MakeAsciiUiText(cell(sourceColumn));
          entry.detail = MakeAsciiUiText(cell(detailColumn));
          if (!entry.feature.empty())
            m_featureInventory.push_back(std::move(entry));
        }
        i = row > 0 ? row - 1 : row;
      }
    }
  }

  bool catalogReadFailed = false;
  bool formalRegistryFound = false;
  bool routeColumnFound = false;
  if (!catalogPath.empty()) {
    std::vector<std::string> lines;
    if (!readMarkdown(catalogPath, lines)) {
      catalogReadFailed = true;
    } else {
      std::string section;
      for (size_t i = 0; i < lines.size(); ++i) {
        const std::string heading = headingText(lines[i]);
        if (!heading.empty()) {
          section = heading;
          continue;
        }

        const std::string normalizedSection =
            LowerAscii(MakeAsciiUiText(section));
        if (normalizedSection.find(
                "villieneditor technology showcase registry") ==
                std::string::npos ||
            lines[i].empty() || lines[i].front() != '|' ||
            i + 1 >= lines.size() ||
            !IsMarkdownSeparatorRow(lines[i + 1]))
          continue;

        formalRegistryFound = true;
        const std::vector<std::string> headers = SplitMarkdownRow(lines[i]);
        const int idColumn = FindMarkdownColumn(headers, "lab id");
        const int categoryColumn = FindMarkdownColumn(headers, "category");
        const int featureColumn = FindMarkdownColumn(headers, "feature");
        const int defaultColumn = FindMarkdownColumn(headers, "default");
        const int routeColumn = FindMarkdownColumn(headers, "route");
        routeColumnFound = routeColumn >= 0;
        int detailColumn = FindMarkdownColumn(headers, "description");
        if (detailColumn < 0)
          detailColumn = FindMarkdownColumn(headers, "detail");
        if (detailColumn < 0)
          detailColumn = static_cast<int>(headers.size()) - 1;
        if (idColumn < 0 || featureColumn < 0)
          continue;

        size_t row = i + 2;
        for (; row < lines.size() && !lines[row].empty() &&
               lines[row].front() == '|';
             ++row) {
          const std::vector<std::string> cells = SplitMarkdownRow(lines[row]);
          const auto cell = [&cells](int index) -> std::string {
            if (index < 0 || index >= static_cast<int>(cells.size()))
              return {};
            return cells[index];
          };

          FeatureShowcaseEntry entry;
          entry.id = LowerAscii(MakeAsciiUiText(cell(idColumn)));
          entry.category = MakeAsciiUiText(cell(categoryColumn));
          entry.feature = MakeAsciiUiText(cell(featureColumn));
          entry.defaultValue = MakeAsciiUiText(cell(defaultColumn));
          entry.detail = MakeAsciiUiText(cell(detailColumn));
          if (entry.id.empty() || entry.feature.empty())
            continue;

          // 空欄・未知 Route も明示的に Unmapped として contract に残す。
          const FeatureRoute route = RouteFromCatalogCell(cell(routeColumn));
          g_featureShowcaseRoutes.emplace(entry.id, route);
          m_featureShowcase.push_back(std::move(entry));
        }
        i = row > 0 ? row - 1 : row;
      }
    }
  }

  bool inventorySynthesized = false;
  if (m_featureInventory.empty() && !m_featureShowcase.empty()) {
    inventorySynthesized = true;
    for (const FeatureShowcaseEntry &showcase : m_featureShowcase) {
      FeatureInventoryEntry entry;
      entry.section = "VillienEditor Technology Showcase Registry";
      entry.category = showcase.category;
      entry.feature = showcase.feature;
      entry.status = "Implemented";
      entry.detail = showcase.detail;
      if (!showcase.defaultValue.empty()) {
        if (!entry.detail.empty())
          entry.detail += " ";
        entry.detail += "Default: " + showcase.defaultValue;
      }
      entry.source = "featuretools/feature_catalog.md";
      m_featureInventory.push_back(std::move(entry));
    }
  }

  size_t unmapped = 0;
  size_t duplicateIds = 0;
  for (size_t i = 0; i < m_featureShowcase.size(); ++i) {
    if (RouteForShowcaseId(m_featureShowcase[i].id) ==
        FeatureRoute::Unmapped) {
      ++unmapped;
    }
    for (size_t previous = 0; previous < i; ++previous) {
      if (m_featureShowcase[previous].id == m_featureShowcase[i].id) {
        ++duplicateIds;
        break;
      }
    }
  }

  std::ostringstream message;
  message << "Loaded " << m_featureInventory.size()
          << " inventory rows and " << m_featureShowcase.size()
          << " formal showcase routes";
  if (!featurePath.empty() && !featureReadFailed)
    message << " from feature.md";
  if (!catalogPath.empty() && !catalogReadFailed)
    message << (featurePath.empty() || featureReadFailed ? " from " : " + ")
            << "featuretools/feature_catalog.md";
  message << ". " << (m_featureShowcase.size() - unmapped) << " / "
          << m_featureShowcase.size() << " mapped, " << unmapped
          << " unmapped, " << duplicateIds << " duplicate IDs.";
  if (inventorySynthesized)
    message << " Inventory was synthesized from the catalog.";
  if (catalogPath.empty() || catalogReadFailed || !formalRegistryFound ||
      !routeColumnFound) {
    message << " Contract warning: the formal catalog registry or Route "
               "column is unavailable.";
  }
  if (featureReadFailed)
    message << " feature.md could not be opened.";
  m_featureInventoryMessage = message.str();
}

// ---- Hotkeys ----

void SceneEditor::ProcessHotkeys(Scene &scene, DxContext &dx) {
  // Gizmo mode hotkeys (W/E/R) — only when ImGui doesn't want keyboard
  // and right mouse button is NOT held (RMB+WASD is camera movement).
  if (!ImGui::GetIO().WantCaptureKeyboard && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
    m_gizmo.ProcessHotkeys();
  }

  const bool shortcutAllowed = !ImGui::GetIO().WantTextInput &&
                               !ImGui::IsAnyItemActive();

  // Never mutate the scene while the user is editing a text or numeric field.
  bool ctrl = ImGui::GetIO().KeyCtrl;

  if (shortcutAllowed && ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
    if (m_gridEditorOpen && m_gridHistory.CanUndo())
      m_gridHistory.Undo();
    else
      m_history.Undo();
  }
  if (shortcutAllowed && ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
    if (m_gridEditorOpen && m_gridHistory.CanRedo())
      m_gridHistory.Redo();
    else
      m_history.Redo();
  }

  // Ctrl+D — duplicate selected entity.
  if (shortcutAllowed && ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) {
    if (m_selectedEntity != kInvalidEntityId) {
      auto cmd =
          std::make_unique<DuplicateEntityCommand>(scene, dx, m_selectedEntity);
      m_history.Execute(std::move(cmd));
      // Select the duplicate: it's the last entity in the scene.
      auto &entities = scene.Entities();
      if (!entities.empty())
        m_selectedEntity = entities.back().id;
    }
  }

  // Delete key — delete selected entity.
  if (shortcutAllowed && ImGui::IsKeyPressed(ImGuiKey_Delete, false) &&
      !ImGui::GetIO().WantCaptureKeyboard) {
    if (m_selectedEntity != kInvalidEntityId) {
      auto cmd = std::make_unique<DeleteEntityCommand>(scene, dx,
                                                        m_selectedEntity);
      m_history.Execute(std::move(cmd));
      m_selectedEntity = kInvalidEntityId;
    }
  }
}

// ---- Menu Bar ----

void SceneEditor::DrawMenuBar(Scene &scene, DxContext &dx) {
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("Scene")) {
      ImGui::InputText("Path", m_scenePath, sizeof(m_scenePath));
      if (ImGui::MenuItem("Save"))
        scene.SaveToFile(m_scenePath);
      if (ImGui::MenuItem("Load")) {
        scene.LoadFromFile(m_scenePath, dx);
        m_history.Clear();
        m_selectedEntity = kInvalidEntityId;
      }
      if (ImGui::MenuItem("Clear")) {
        scene.Clear();
        m_history.Clear();
        m_selectedEntity = kInvalidEntityId;
      }
      ImGui::EndMenu();
    }

    // Undo/redo status in menu bar.
    if (ImGui::BeginMenu("Edit")) {
      const char *undoName = m_history.UndoName();
      const char *redoName = m_history.RedoName();

      char undoLabel[128] = "Undo";
      char redoLabel[128] = "Redo";
      if (undoName)
        snprintf(undoLabel, sizeof(undoLabel), "Undo %s", undoName);
      if (redoName)
        snprintf(redoLabel, sizeof(redoLabel), "Redo %s", redoName);

      if (ImGui::MenuItem(undoLabel, "Ctrl+Z", false, m_history.CanUndo()))
        m_history.Undo();
      if (ImGui::MenuItem(redoLabel, "Ctrl+Y", false, m_history.CanRedo()))
        m_history.Redo();

      ImGui::Separator();
      if (ImGui::MenuItem("Duplicate", "Ctrl+D", false,
                           m_selectedEntity != kInvalidEntityId)) {
        auto cmd = std::make_unique<DuplicateEntityCommand>(scene, dx,
                                                             m_selectedEntity);
        m_history.Execute(std::move(cmd));
        auto &entities = scene.Entities();
        if (!entities.empty())
          m_selectedEntity = entities.back().id;
      }
      if (ImGui::MenuItem("Delete", "Del", false,
                           m_selectedEntity != kInvalidEntityId)) {
        auto cmd = std::make_unique<DeleteEntityCommand>(scene, dx,
                                                          m_selectedEntity);
        m_history.Execute(std::move(cmd));
        m_selectedEntity = kInvalidEntityId;
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window")) {
      if (ImGui::MenuItem("Environment"))
        m_requestedSystemsTab = 1;
      if (ImGui::MenuItem("Rendering"))
        m_requestedSystemsTab = 2;
      if (ImGui::MenuItem("World"))
        m_requestedSystemsTab = 3;
      if (ImGui::MenuItem("VFX"))
        m_requestedSystemsTab = 4;
      if (ImGui::MenuItem("Camera"))
        m_requestedSystemsTab = 5;
      if (ImGui::MenuItem("Feature Coverage"))
        m_requestedSystemsTab = 6;
      ImGui::Separator();
      if (ImGui::MenuItem("Camera Navigation", nullptr,
                          m_cameraNavigationEnabled)) {
        m_cameraNavigationEnabled = !m_cameraNavigationEnabled;
      }
      ImGui::EndMenu();
    }

    // Play Scene button (Phase 8).
    ImGui::Separator();
    if (ImGui::MenuItem("Play Scene", "F5"))
      m_scenePlayRequested = true;

    ImGui::EndMainMenuBar();
  }
}

// ---- Scene View and adjustable workspace ----

void SceneEditor::DrawSceneViewport(DxContext &dx) {
  const EditorWorkspaceLayout layout = BuildEditorWorkspaceLayout();
  ImGui::SetNextWindowPos(ImVec2(layout.viewport.x, layout.viewport.y),
                          ImGuiCond_Always);
  ImGui::SetNextWindowSize(
      ImVec2(layout.viewport.width, layout.viewport.height), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::Begin("##SceneViewport", nullptr, kSceneViewportFlags);
  if (dx.HasEditorViewportTexture()) {
    ImGui::Image(static_cast<ImTextureID>(dx.EditorViewportGpu().ptr),
                 ImVec2(layout.viewport.width, layout.viewport.height));
  } else {
    ImGui::TextDisabled("Scene View render target is not ready.");
  }
  ImGui::End();
  ImGui::PopStyleVar(2);

  ImDrawList *draw = ImGui::GetForegroundDrawList();
  draw->AddRect(ImVec2(layout.viewport.x, layout.viewport.y),
                ImVec2(layout.viewport.x + layout.viewport.width,
                       layout.viewport.y + layout.viewport.height),
                IM_COL32(54, 62, 74, 255));
}

void SceneEditor::DrawWorkspaceSplitters() {
  EditorWorkspaceLayout layout = BuildEditorWorkspaceLayout();
  ImGuiIO &io = ImGui::GetIO();
  const ImVec2 display = io.DisplaySize;
  const ImVec2 mouse = io.MousePos;
  constexpr float grabRadius = 5.0f;

  const float leftX = layout.leftWidth;
  const float rightX = display.x - layout.rightWidth;
  const float bottomY = display.y - layout.bottomHeight;
  const float sceneObjectsY = layout.menuHeight + layout.sceneObjectsHeight;

  const bool hoverLeft =
      std::fabs(mouse.x - leftX) <= grabRadius &&
      mouse.y >= layout.menuHeight;
  const bool hoverRight =
      std::fabs(mouse.x - rightX) <= grabRadius &&
      mouse.y >= layout.menuHeight;
  const bool hoverBottom =
      std::fabs(mouse.y - bottomY) <= grabRadius && mouse.x >= leftX &&
      mouse.x <= rightX;
  const bool hoverSceneObjects =
      std::fabs(mouse.y - sceneObjectsY) <= grabRadius && mouse.x <= leftX;

  if (g_editorWorkspace.activeSplitter == 0 &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (hoverLeft)
      g_editorWorkspace.activeSplitter = 1;
    else if (hoverRight)
      g_editorWorkspace.activeSplitter = 2;
    else if (hoverBottom)
      g_editorWorkspace.activeSplitter = 3;
    else if (hoverSceneObjects)
      g_editorWorkspace.activeSplitter = 4;
  }

  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
    g_editorWorkspace.activeSplitter = 0;

  switch (g_editorWorkspace.activeSplitter) {
  case 1:
    g_editorWorkspace.leftWidth = mouse.x;
    break;
  case 2:
    g_editorWorkspace.rightWidth = display.x - mouse.x;
    break;
  case 3:
    g_editorWorkspace.bottomHeight = display.y - mouse.y;
    break;
  case 4: {
    const float contentHeight =
        std::max(1.0f, display.y - layout.menuHeight);
    g_editorWorkspace.sceneObjectsRatio =
        std::clamp((mouse.y - layout.menuHeight) / contentHeight, 0.20f,
                   0.80f);
  } break;
  default:
    break;
  }

  const bool verticalActive = g_editorWorkspace.activeSplitter == 1 ||
                              g_editorWorkspace.activeSplitter == 2;
  const bool horizontalActive = g_editorWorkspace.activeSplitter == 3 ||
                                g_editorWorkspace.activeSplitter == 4;
  if (hoverLeft || hoverRight || verticalActive)
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  else if (hoverBottom || hoverSceneObjects || horizontalActive)
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

  ImDrawList *draw = ImGui::GetForegroundDrawList();
  const ImU32 normal = IM_COL32(55, 63, 75, 255);
  const ImU32 hot = IM_COL32(54, 147, 235, 255);
  draw->AddLine(ImVec2(leftX, layout.menuHeight), ImVec2(leftX, display.y),
                (hoverLeft || g_editorWorkspace.activeSplitter == 1) ? hot
                                                                     : normal,
                2.0f);
  draw->AddLine(ImVec2(rightX, layout.menuHeight), ImVec2(rightX, display.y),
                (hoverRight || g_editorWorkspace.activeSplitter == 2) ? hot
                                                                       : normal,
                2.0f);
  draw->AddLine(ImVec2(leftX, bottomY), ImVec2(rightX, bottomY),
                (hoverBottom || g_editorWorkspace.activeSplitter == 3) ? hot
                                                                        : normal,
                2.0f);
  draw->AddLine(ImVec2(0.0f, sceneObjectsY), ImVec2(leftX, sceneObjectsY),
                (hoverSceneObjects || g_editorWorkspace.activeSplitter == 4)
                    ? hot
                    : normal,
                2.0f);
}

// ---- Unity-style workspace toolbar ----

void SceneEditor::DrawWorkspaceToolbar(Scene &scene,
                                       EditorRuntimeBindings *runtime,
                                       bool scenePlaying) {
  const EditorWorkspaceLayout layout = BuildEditorWorkspaceLayout();
  ImGui::SetNextWindowPos(
      ImVec2(layout.leftWidth, layout.menuHeight), ImGuiCond_Always);
  ImGui::SetNextWindowSize(
      ImVec2(layout.viewport.width, layout.toolbarHeight), ImGuiCond_Always);
  ImGui::Begin("##WorkspaceToolbar", nullptr, kToolbarFlags);

  if (scenePlaying)
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImVec4(0.72f, 0.24f, 0.22f, 1.0f));
  if (ImGui::Button(scenePlaying ? "Stop  [F5]" : "Play  [F5]"))
    m_scenePlayRequested = true;
  if (scenePlaying)
    ImGui::PopStyleColor();
  ImGui::SameLine();

  const auto drawModeButton = [&](const char *label,
                                  GizmoController::Operation operation) {
    const bool active = m_gizmo.GetOperation() == operation;
    if (active) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.46f, 0.78f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            ImVec4(0.25f, 0.56f, 0.90f, 1.0f));
    }
    if (ImGui::Button(label))
      m_gizmo.SetOperation(operation);
    if (active)
      ImGui::PopStyleColor(2);
    ImGui::SameLine();
  };

  drawModeButton("Move [W]", GizmoController::Operation::Translate);
  drawModeButton("Rotate [E]", GizmoController::Operation::Rotate);
  drawModeButton("Scale [R]", GizmoController::Operation::Scale);

  const bool localSpace = m_gizmo.GetSpace() == GizmoController::Space::Local;
  if (ImGui::Button(localSpace ? "Local" : "World")) {
    m_gizmo.SetSpace(localSpace ? GizmoController::Space::World
                                : GizmoController::Space::Local);
  }
  ImGui::SameLine();
  ImGui::Checkbox("Snap", &m_gizmo.snapEnabled);
  ImGui::SameLine();
  ImGui::TextDisabled("Scene View | %zu objects", scene.Entities().size());

  if (!scenePlaying && runtime) {
    if (runtime->playTitle) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Run Title"))
        runtime->playTitle();
    }
    if (runtime->playOverworld) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Run Overworld"))
        runtime->playOverworld();
    }
    if (runtime->playBossArena) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Run Boss"))
        runtime->playBossArena();
    }
  }

  ImGui::End();
}

// ---- Entity List Panel ----

void SceneEditor::DrawEntityList(Scene &scene, DxContext &dx) {
  const EditorWorkspaceLayout layout = BuildEditorWorkspaceLayout();
  ImGui::SetNextWindowPos(ImVec2(0.0f, layout.menuHeight), ImGuiCond_Always);
  ImGui::SetNextWindowSize(
      ImVec2(layout.leftWidth, layout.sceneObjectsHeight), ImGuiCond_Always);
  ImGui::Begin("Scene Objects", nullptr, kFixedPanelFlags);

  // Helper lambda to create entity via command.
  auto createEntity = [&](const char *name, MeshSourceType type) {
    Entity e;
    e.id = scene.AllocateId();
    e.name = name;
    MeshComponent mc;
    mc.sourceType = type;
    e.mesh = mc;
    auto cmd = std::make_unique<CreateEntityCommand>(scene, dx, std::move(e));
    m_history.Execute(std::move(cmd));
    m_selectedEntity = scene.Entities().back().id;
  };

  auto createLightEntity = [&](const char *name, bool isSpot) {
    Entity e;
    e.id = scene.AllocateId();
    e.name = name;
    if (isSpot) {
      e.spotLight = SpotLightComponent{};
      e.transform.position = {0.0f, 5.0f, 0.0f};
    } else {
      e.pointLight = PointLightComponent{};
      e.transform.position = {0.0f, 3.0f, 0.0f};
    }
    auto cmd = std::make_unique<CreateEntityCommand>(scene, dx, std::move(e));
    m_history.Execute(std::move(cmd));
    m_selectedEntity = scene.Entities().back().id;
  };

  // Create entity buttons.
  if (ImGui::Button("+ Empty")) {
    Entity e;
    e.id = scene.AllocateId();
    e.name = "Empty";
    auto cmd = std::make_unique<CreateEntityCommand>(scene, dx, std::move(e));
    m_history.Execute(std::move(cmd));
    m_selectedEntity = scene.Entities().back().id;
  }
  ImGui::SameLine();
  if (ImGui::Button("+ Cube"))
    createEntity("Cube", MeshSourceType::ProceduralCube);
  ImGui::SameLine();
  if (ImGui::Button("+ Plane"))
    createEntity("Plane", MeshSourceType::ProceduralPlane);

  if (ImGui::Button("+ Sphere"))
    createEntity("Sphere", MeshSourceType::ProceduralSphere);
  ImGui::SameLine();
  if (ImGui::Button("+ Cylinder"))
    createEntity("Cylinder", MeshSourceType::ProceduralCylinder);
  ImGui::SameLine();
  if (ImGui::Button("+ Cone"))
    createEntity("Cone", MeshSourceType::ProceduralCone);

  if (ImGui::Button("+ Point Light"))
    createLightEntity("Point Light", false);
  ImGui::SameLine();
  if (ImGui::Button("+ Spot Light"))
    createLightEntity("Spot Light", true);

  ImGui::Separator();

  // Entity list.
  auto &entities = scene.Entities();
  for (auto &e : entities) {
    bool selected = (e.id == m_selectedEntity);
    char label[128];
    snprintf(label, sizeof(label), "%s [%llu]", e.name.c_str(),
             static_cast<unsigned long long>(e.id));
    if (ImGui::Selectable(label, selected))
      m_selectedEntity = e.id;
  }

  ImGui::Separator();

  // Delete selected.
  if (m_selectedEntity != kInvalidEntityId) {
    if (ImGui::Button("Delete Selected")) {
      auto cmd = std::make_unique<DeleteEntityCommand>(scene, dx,
                                                        m_selectedEntity);
      m_history.Execute(std::move(cmd));
      m_selectedEntity = kInvalidEntityId;
    }
  }

  ImGui::End();
}

// ---- Inspector Panel ----

void SceneEditor::DrawInspector(Scene &scene, DxContext &dx) {
  const EditorWorkspaceLayout layout = BuildEditorWorkspaceLayout();
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(
      ImVec2(display.x - layout.rightWidth, layout.menuHeight),
      ImGuiCond_Always);
  ImGui::SetNextWindowSize(
      ImVec2(layout.rightWidth, display.y - layout.menuHeight),
      ImGuiCond_Always);
  ImGui::Begin("Inspector", nullptr, kFixedPanelFlags);

  Entity *e = scene.FindEntity(m_selectedEntity);
  if (!e) {
    ImGui::TextDisabled("No entity selected.");
    ImGui::End();
    return;
  }

  // Name.
  char nameBuf[128];
  snprintf(nameBuf, sizeof(nameBuf), "%s", e->name.c_str());
  if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
    e->name = nameBuf;

  ImGui::Checkbox("Active", &e->active);

  // Transform (with undo coalescing for DragFloat3).
  if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::DragFloat3("Position", &e->transform.position.x, 0.1f);
    if (ImGui::IsItemActivated() && !m_transformDragActive) {
      m_transformDragActive = true;
      m_transformDragStart = e->transform;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_transformDragActive) {
      auto cmd = std::make_unique<TransformCommand>(
          scene, m_selectedEntity, m_transformDragStart, e->transform);
      m_history.PushWithoutExecute(std::move(cmd));
      m_transformDragActive = false;
    }

    ImGui::DragFloat3("Rotation", &e->transform.rotation.x, 1.0f);
    if (ImGui::IsItemActivated() && !m_transformDragActive) {
      m_transformDragActive = true;
      m_transformDragStart = e->transform;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_transformDragActive) {
      auto cmd = std::make_unique<TransformCommand>(
          scene, m_selectedEntity, m_transformDragStart, e->transform);
      m_history.PushWithoutExecute(std::move(cmd));
      m_transformDragActive = false;
    }

    ImGui::DragFloat3("Scale", &e->transform.scale.x, 0.05f, 0.01f, 100.0f);
    if (ImGui::IsItemActivated() && !m_transformDragActive) {
      m_transformDragActive = true;
      m_transformDragStart = e->transform;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_transformDragActive) {
      auto cmd = std::make_unique<TransformCommand>(
          scene, m_selectedEntity, m_transformDragStart, e->transform);
      m_history.PushWithoutExecute(std::move(cmd));
      m_transformDragActive = false;
    }
  }

  // Mesh component.
  if (e->mesh.has_value()) {
    if (ImGui::CollapsingHeader("Mesh Component",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      auto &mc = e->mesh.value();

      // Drop target: accept mesh files dragged from Asset Browser.
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload =
                ImGui::AcceptDragDropPayload("ASSET_PATH")) {
          std::string droppedPath(static_cast<const char *>(payload->Data));
          std::string ext;
          auto dotPos = droppedPath.rfind('.');
          if (dotPos != std::string::npos)
            ext = droppedPath.substr(dotPos);
          for (auto &c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          if (ext == ".gltf" || ext == ".glb") {
            Material oldMat = mc.material;
            auto oldPaths = mc.texturePaths;
            mc.sourceType = MeshSourceType::GltfFile;
            mc.gltfPath = droppedPath;
            mc.meshId = UINT32_MAX;
            m_history.Execute(std::make_unique<MaterialCommand>(
                scene, dx, e->id, oldMat, mc.material, oldPaths,
                mc.texturePaths));
          }
        }
        ImGui::EndDragDropTarget();
      }

      int srcType = static_cast<int>(mc.sourceType);
      bool meshChanged = false;

      if (ImGui::Combo("Mesh Type", &srcType, kMeshSourceTypeNames,
                        IM_ARRAYSIZE(kMeshSourceTypeNames))) {
        mc.sourceType = static_cast<MeshSourceType>(srcType);
        meshChanged = true;
      }

      // Show relevant parameters based on source type.
      switch (mc.sourceType) {
      case MeshSourceType::ProceduralCube: {
        float sz = e->transform.scale.x;
        if (ImGui::DragFloat("Size", &sz, 0.05f, 0.1f, 50.0f)) {
          e->transform.scale = {sz, sz, sz};
        }
      } break;
      case MeshSourceType::ProceduralPlane:
        meshChanged |=
            ImGui::DragFloat("Width", &mc.width, 0.05f, 0.1f, 100.0f);
        meshChanged |=
            ImGui::DragFloat("Depth", &mc.height, 0.05f, 0.1f, 100.0f);
        break;
      case MeshSourceType::ProceduralCylinder:
      case MeshSourceType::ProceduralCone:
        meshChanged |=
            ImGui::DragFloat("Radius", &mc.width, 0.05f, 0.05f, 20.0f);
        meshChanged |=
            ImGui::DragFloat("Height", &mc.height, 0.05f, 0.1f, 50.0f);
        {
          int seg = static_cast<int>(mc.segments);
          if (ImGui::DragInt("Segments", &seg, 1, 3, 64)) {
            mc.segments = static_cast<uint32_t>(seg);
            meshChanged = true;
          }
        }
        break;
      case MeshSourceType::ProceduralSphere:
        meshChanged |=
            ImGui::DragFloat("Radius", &mc.size, 0.05f, 0.05f, 20.0f);
        {
          int r = static_cast<int>(mc.rings);
          int s = static_cast<int>(mc.segments);
          meshChanged |= ImGui::DragInt("Rings", &r, 1, 3, 64);
          meshChanged |= ImGui::DragInt("Segments", &s, 1, 3, 64);
          mc.rings = static_cast<uint32_t>(r);
          mc.segments = static_cast<uint32_t>(s);
        }
        break;
      case MeshSourceType::GltfFile: {
        char pathBuf[256];
        snprintf(pathBuf, sizeof(pathBuf), "%s", mc.gltfPath.c_str());
        if (ImGui::InputText("glTF Path", pathBuf, sizeof(pathBuf)))
          mc.gltfPath = pathBuf;
        if (ImGui::Button("Load glTF"))
          meshChanged = true;
      } break;
      }

      // ---- Material properties (Phase 2 — Material & Texture Editor) ----
      ImGui::Separator();
      if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {

        auto &mat = mc.material;
        bool matChanged = false;

        // Track drag-start for undo coalescing.
        auto CheckDragStart = [&]() {
          if (ImGui::IsItemActivated() && !m_materialDragActive) {
            m_materialDragActive = true;
            m_materialDragStart = mat;
            m_materialPathsStart = mc.texturePaths;
          }
        };
        auto CheckDragEnd = [&]() {
          if (ImGui::IsItemDeactivatedAfterEdit() && m_materialDragActive) {
            m_materialDragActive = false;
            m_history.Execute(std::make_unique<MaterialCommand>(
                scene, dx, e->id, m_materialDragStart, mat,
                m_materialPathsStart, mc.texturePaths));
          }
        };

        // Material preset combo.
        if (ImGui::BeginCombo("Preset", "Select...")) {
          for (int p = 0; p < kPresetCount; ++p) {
            if (ImGui::Selectable(kPresets[p].name)) {
              Material oldMat = mat;
              auto oldPaths = mc.texturePaths;
              mat.baseColorFactor = kPresets[p].baseColorFactor;
              mat.metallicFactor = kPresets[p].metallicFactor;
              mat.roughnessFactor = kPresets[p].roughnessFactor;
              mat.emissiveFactor = kPresets[p].emissiveFactor;
              m_history.Execute(std::make_unique<MaterialCommand>(
                  scene, dx, e->id, oldMat, mat, oldPaths, mc.texturePaths));
              matChanged = true;
            }
          }
          ImGui::EndCombo();
        }

        // PBR scalars.
        matChanged |= ImGui::ColorEdit4("Base Color", &mat.baseColorFactor.x);
        CheckDragStart(); CheckDragEnd();
        matChanged |= ImGui::SliderFloat("Metallic", &mat.metallicFactor, 0, 1);
        CheckDragStart(); CheckDragEnd();
        matChanged |= ImGui::SliderFloat("Roughness", &mat.roughnessFactor, 0, 1);
        CheckDragStart(); CheckDragEnd();
        matChanged |= ImGui::ColorEdit3("Emissive", &mat.emissiveFactor.x);
        CheckDragStart(); CheckDragEnd();

        // 反射を受ける役割と、DXR scene に含める役割を別々に編集する。
        ImGui::Separator();
        ImGui::Text("Ray Tracing Reflection");
        constexpr const char *kReceiverNames[] = {"None", "Water", "Mirror"};
        const int receiverIndex =
            static_cast<int>(mat.reflectionReceiver);
        if (ImGui::BeginCombo("Reflection Receiver",
                              kReceiverNames[receiverIndex])) {
          for (int receiver = 0; receiver < 3; ++receiver) {
            if (ImGui::Selectable(kReceiverNames[receiver],
                                  receiver == receiverIndex)) {
              Material oldMat = mat;
              auto oldPaths = mc.texturePaths;
              mat.reflectionReceiver =
                  static_cast<ReflectionReceiver>(receiver);
              m_history.Execute(std::make_unique<MaterialCommand>(
                  scene, dx, e->id, oldMat, mat, oldPaths, mc.texturePaths));
              matChanged = true;
            }
          }
          ImGui::EndCombo();
        }
        matChanged |= ImGui::SliderFloat("Reflection Strength",
                                         &mat.reflectionStrength, 0.0f, 1.0f);
        CheckDragStart(); CheckDragEnd();
        bool rayTracingVisible = mat.rayTracingVisible;
        if (ImGui::Checkbox("Ray Tracing Visible", &rayTracingVisible)) {
          Material oldMat = mat;
          auto oldPaths = mc.texturePaths;
          mat.rayTracingVisible = rayTracingVisible;
          m_history.Execute(std::make_unique<MaterialCommand>(
              scene, dx, e->id, oldMat, mat, oldPaths, mc.texturePaths));
          matChanged = true;
        }

        // UV tiling/offset.
        ImGui::Separator();
        ImGui::Text("UV Transform");
        matChanged |= ImGui::DragFloat2("UV Tiling", &mat.uvTiling.x, 0.05f, 0.01f, 100.0f);
        CheckDragStart(); CheckDragEnd();
        matChanged |= ImGui::DragFloat2("UV Offset", &mat.uvOffset.x, 0.01f, -10.0f, 10.0f);
        CheckDragStart(); CheckDragEnd();

        // POM controls.
        ImGui::Separator();
        ImGui::Text("Parallax Occlusion Mapping");
        if (ImGui::Checkbox("Enable POM", &mat.pomEnabled))
          matChanged = true;
        if (mat.pomEnabled) {
          matChanged |= ImGui::DragFloat("Height Scale", &mat.heightScale, 0.001f, 0.001f, 0.1f);
          CheckDragStart(); CheckDragEnd();
          matChanged |= ImGui::DragFloat("Min Layers", &mat.pomMinLayers, 1.0f, 1.0f, 64.0f);
          CheckDragStart(); CheckDragEnd();
          matChanged |= ImGui::DragFloat("Max Layers", &mat.pomMaxLayers, 1.0f, 1.0f, 128.0f);
          CheckDragStart(); CheckDragEnd();
        }

        // Texture slots.
        ImGui::Separator();
        ImGui::Text("Textures");
        const bool *hasFlags[] = {&mat.hasBaseColor, &mat.hasNormal,
                                  &mat.hasMetalRough, &mat.hasAO,
                                  &mat.hasEmissive, &mat.hasHeight};
        for (int i = 0; i < 6; ++i) {
          ImGui::PushID(i);

          // Thumbnail (only if mesh has valid GPU resources).
          if (mc.meshId != UINT32_MAX) {
            D3D12_GPU_DESCRIPTOR_HANDLE thumbGpu =
                dx.GetTextureImGuiSrv(mc.meshId, i);
            if (thumbGpu.ptr != 0) {
              ImGui::Image(static_cast<ImTextureID>(thumbGpu.ptr),
                           ImVec2(32, 32));
              ImGui::SameLine();
            }
          }

          // Status + slot name.
          const char *status = *hasFlags[i] ? "[Loaded]" : "[Default]";
          ImGui::Text("%s %s", kTexSlotNames[i], status);
          ImGui::SameLine();

          // Load button.
          char loadLabel[32];
          snprintf(loadLabel, sizeof(loadLabel), "Load##%d", i);
          if (ImGui::SmallButton(loadLabel)) {
            char filePath[512];
            if (OpenTextureFileDialog(filePath, sizeof(filePath))) {
              Material oldMat = mat;
              auto oldPaths = mc.texturePaths;
              mc.texturePaths[i] = filePath;
              m_history.Execute(std::make_unique<MaterialCommand>(
                  scene, dx, e->id, oldMat, mat, oldPaths, mc.texturePaths));
              matChanged = true;
            }
          }
          ImGui::SameLine();

          // Clear button (only if a texture is loaded for this slot).
          if (*hasFlags[i] || !mc.texturePaths[i].empty()) {
            char clearLabel[32];
            snprintf(clearLabel, sizeof(clearLabel), "Clear##%d", i);
            if (ImGui::SmallButton(clearLabel)) {
              Material oldMat = mat;
              auto oldPaths = mc.texturePaths;
              mc.texturePaths[i].clear();
              m_history.Execute(std::make_unique<MaterialCommand>(
                  scene, dx, e->id, oldMat, mat, oldPaths, mc.texturePaths));
              matChanged = true;
            }
          }

          // Drop target: accept texture files from Asset Browser.
          // Use an invisible button as drop area for this slot row.
          {
            char dropLabel[64];
            snprintf(dropLabel, sizeof(dropLabel), "##TexDrop%d", i);
            ImGui::SameLine();
            ImGui::SmallButton(dropLabel);
            if (ImGui::BeginDragDropTarget()) {
              if (const ImGuiPayload *payload =
                      ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                std::string droppedPath(
                    static_cast<const char *>(payload->Data));
                std::string ext2;
                auto dotPos2 = droppedPath.rfind('.');
                if (dotPos2 != std::string::npos)
                  ext2 = droppedPath.substr(dotPos2);
                for (auto &c : ext2)
                  c = static_cast<char>(
                      std::tolower(static_cast<unsigned char>(c)));
                if (ext2 == ".png" || ext2 == ".jpg" || ext2 == ".jpeg" ||
                    ext2 == ".bmp" || ext2 == ".tga") {
                  Material oldMat = mat;
                  auto oldPaths = mc.texturePaths;
                  mc.texturePaths[i] = droppedPath;
                  m_history.Execute(std::make_unique<MaterialCommand>(
                      scene, dx, e->id, oldMat, mat, oldPaths,
                      mc.texturePaths));
                  matChanged = true;
                }
              }
              ImGui::EndDragDropTarget();
            }
          }

          ImGui::PopID();
        }

        if (matChanged) {
          scene.UpdateEntityMaterial(dx, *e);
        }
      } // end Material collapsing header

      // Mesh param changes (size, segments, glTF load, etc.) also need GPU update.
      if (meshChanged) {
        scene.CreateEntityMeshGpu(dx, *e);
      }

      if (ImGui::Button("Remove Mesh Component"))
        e->mesh.reset();
    }
  } else {
    if (ImGui::Button("Add Mesh Component")) {
      e->mesh = MeshComponent{};
      scene.CreateEntityMeshGpu(dx, *e);
    }
  }

  // Point light component.
  if (e->pointLight.has_value()) {
    if (ImGui::CollapsingHeader("Point Light",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      auto &pl = e->pointLight.value();

      auto CheckPLDragStart = [&]() {
        if (ImGui::IsItemActivated() && !m_plDragActive) {
          m_plDragActive = true;
          m_plDragStart = pl;
        }
      };
      auto CheckPLDragEnd = [&]() {
        if (ImGui::IsItemDeactivatedAfterEdit() && m_plDragActive) {
          auto cmd = std::make_unique<PointLightCommand>(
              scene, e->id, m_plDragStart, pl);
          m_history.PushWithoutExecute(std::move(cmd));
          m_plDragActive = false;
        }
      };

      ImGui::Checkbox("Enabled##PL", &pl.enabled);
      ImGui::ColorEdit3("Color##PL", &pl.color.x);
      CheckPLDragStart();
      CheckPLDragEnd();
      ImGui::DragFloat("Intensity##PL", &pl.intensity, 0.1f, 0.0f, 100.0f);
      CheckPLDragStart();
      CheckPLDragEnd();
      ImGui::DragFloat("Range##PL", &pl.range, 0.1f, 0.1f, 200.0f);
      CheckPLDragStart();
      CheckPLDragEnd();

      if (ImGui::Button("Remove Point Light"))
        e->pointLight.reset();
    }
  } else {
    if (ImGui::Button("Add Point Light")) {
      e->pointLight = PointLightComponent{};
    }
  }

  // Spot light component.
  if (e->spotLight.has_value()) {
    if (ImGui::CollapsingHeader("Spot Light",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      auto &sl = e->spotLight.value();

      auto CheckSLDragStart = [&]() {
        if (ImGui::IsItemActivated() && !m_slDragActive) {
          m_slDragActive = true;
          m_slDragStart = sl;
        }
      };
      auto CheckSLDragEnd = [&]() {
        if (ImGui::IsItemDeactivatedAfterEdit() && m_slDragActive) {
          auto cmd = std::make_unique<SpotLightCommand>(
              scene, e->id, m_slDragStart, sl);
          m_history.PushWithoutExecute(std::move(cmd));
          m_slDragActive = false;
        }
      };

      ImGui::Checkbox("Enabled##SL", &sl.enabled);
      ImGui::DragFloat3("Direction##SL", &sl.direction.x, 0.01f, -1.0f, 1.0f);
      CheckSLDragStart();
      CheckSLDragEnd();
      ImGui::ColorEdit3("Color##SL", &sl.color.x);
      CheckSLDragStart();
      CheckSLDragEnd();
      ImGui::DragFloat("Intensity##SL", &sl.intensity, 0.1f, 0.0f, 100.0f);
      CheckSLDragStart();
      CheckSLDragEnd();
      ImGui::DragFloat("Range##SL", &sl.range, 0.1f, 0.1f, 200.0f);
      CheckSLDragStart();
      CheckSLDragEnd();
      ImGui::DragFloat("Inner Angle##SL", &sl.innerAngleDeg, 0.5f, 0.0f,
                        89.0f);
      CheckSLDragStart();
      CheckSLDragEnd();
      ImGui::DragFloat("Outer Angle##SL", &sl.outerAngleDeg, 0.5f, 0.0f,
                        89.0f);
      if (sl.outerAngleDeg < sl.innerAngleDeg + 0.5f)
        sl.outerAngleDeg = sl.innerAngleDeg + 0.5f;
      CheckSLDragStart();
      CheckSLDragEnd();

      if (ImGui::Button("Remove Spot Light"))
        e->spotLight.reset();
    }
  } else {
    if (ImGui::Button("Add Spot Light")) {
      e->spotLight = SpotLightComponent{};
    }
  }

  ImGui::End();
}

// ---- Lighting Panel (Phase 3) ----

void SceneEditor::DrawLightingPanel(Scene &scene, bool *iblEnabled) {
  ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
  ImGui::Begin("Lighting");

  auto &ls = scene.LightSettings();

  // Helper lambdas for drag coalescing (same pattern as material editor).
  auto CheckLightDragStart = [&]() {
    if (ImGui::IsItemActivated() && !m_lightDragActive) {
      m_lightDragActive = true;
      m_lightDragStart = ls;
    }
  };
  auto CheckLightDragEnd = [&]() {
    if (ImGui::IsItemDeactivatedAfterEdit() && m_lightDragActive) {
      auto cmd = std::make_unique<LightSettingsCommand>(scene, m_lightDragStart,
                                                        ls);
      m_history.PushWithoutExecute(std::move(cmd));
      m_lightDragActive = false;
    }
  };

  // ---- Directional (sun) light ----
  if (ImGui::CollapsingHeader("Directional Light",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::SliderFloat3("Sun Direction", &ls.lightDir.x, -1.0f, 1.0f);
    CheckLightDragStart();
    CheckLightDragEnd();

    ImGui::DragFloat("Intensity##Sun", &ls.lightIntensity, 0.1f, 0.0f, 50.0f,
                     "%.2f");
    CheckLightDragStart();
    CheckLightDragEnd();

    ImGui::ColorEdit3("Color##Sun", &ls.lightColor.x);
    CheckLightDragStart();
    CheckLightDragEnd();
  }

  // ---- Environment ----
  if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (iblEnabled)
      ImGui::Checkbox("Enable IBL", iblEnabled);
    ImGui::DragFloat("IBL Intensity", &ls.iblIntensity, 0.01f, 0.0f, 5.0f,
                     "%.2f");
    CheckLightDragStart();
    CheckLightDragEnd();
  }

  ImGui::End();
}

// ---- Shadow & SSAO Panel (Phase 4) ----

void SceneEditor::DrawShadowPanel(Scene &scene, DxContext &dx) {
  ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
  ImGui::Begin("Shadows & SSAO");

  auto &ss = scene.ShadowSettings();

  auto CheckShadowDragStart = [&]() {
    if (ImGui::IsItemActivated() && !m_shadowDragActive) {
      m_shadowDragActive = true;
      m_shadowDragStart = ss;
    }
  };
  auto CheckShadowDragEnd = [&]() {
    if (ImGui::IsItemDeactivatedAfterEdit() && m_shadowDragActive) {
      auto cmd =
          std::make_unique<ShadowSettingsCommand>(scene, m_shadowDragStart, ss);
      m_history.PushWithoutExecute(std::move(cmd));
      m_shadowDragActive = false;
    }
  };
  // Checkbox helper: immediate undo command (no drag coalescing needed).
  auto ShadowCheckbox = [&](const char *label, bool *val) {
    SceneShadowSettings before = ss;
    if (ImGui::Checkbox(label, val)) {
      auto cmd = std::make_unique<ShadowSettingsCommand>(scene, before, ss);
      m_history.PushWithoutExecute(std::move(cmd));
    }
  };

  // ---- Cascaded Shadows ----
  if (ImGui::CollapsingHeader("Cascaded Shadows",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    ShadowCheckbox("Enable Shadows", &ss.shadowsEnabled);

    ImGui::SliderFloat("Strength##CSM", &ss.shadowStrength, 0.0f, 1.0f,
                       "%.2f");
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ImGui::SliderFloat("Bias##CSM", &ss.shadowBias, 0.0000f, 0.01f, "%.5f",
                       ImGuiSliderFlags_Logarithmic);
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ImGui::SliderFloat("Lambda (split)", &ss.csmLambda, 0.0f, 1.0f, "%.2f");
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ImGui::SliderFloat("Max Distance", &ss.csmMaxDistance, 20.0f, 500.0f,
                       "%.0f");
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ShadowCheckbox("Debug Cascades", &ss.csmDebugCascades);

    ImGui::Separator();
    ImGui::Text("Shadow map: %ux%u x%u cascades", dx.GetShadowMap().Size(),
                dx.GetShadowMap().Size(), dx.GetShadowMap().CascadeCount());
  }

  // ---- SSAO ----
  if (ImGui::CollapsingHeader("SSAO", ImGuiTreeNodeFlags_DefaultOpen)) {
    ShadowCheckbox("Enable SSAO", &ss.ssaoEnabled);

    ImGui::SliderFloat("Radius##SSAO", &ss.ssaoRadius, 0.05f, 2.0f, "%.2f");
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ImGui::SliderFloat("Bias##SSAO", &ss.ssaoBias, 0.001f, 0.1f, "%.4f",
                       ImGuiSliderFlags_Logarithmic);
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ImGui::SliderFloat("Power##SSAO", &ss.ssaoPower, 0.5f, 8.0f, "%.1f");
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ImGui::SliderInt("Kernel Size##SSAO", &ss.ssaoKernelSize, 8, 64);
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ImGui::SliderFloat("Strength##SSAO", &ss.ssaoStrength, 0.0f, 2.0f,
                       "%.2f");
    CheckShadowDragStart();
    CheckShadowDragEnd();

    ImGui::Separator();
    ImGui::Text("AO resolution: %ux%u", dx.SsaoWidth(), dx.SsaoHeight());
  }

  ImGui::End();
}

// ---- Post-Processing Panel (Phase 4) ----

void SceneEditor::DrawPostProcessPanel(Scene &scene, DxContext &dx) {
  ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
  ImGui::Begin("Post Processing");

  auto &pp = scene.PostProcessSettings();

  auto CheckPPDragStart = [&]() {
    if (ImGui::IsItemActivated() && !m_ppDragActive) {
      m_ppDragActive = true;
      m_ppDragStart = pp;
    }
  };
  auto CheckPPDragEnd = [&]() {
    if (ImGui::IsItemDeactivatedAfterEdit() && m_ppDragActive) {
      auto cmd = std::make_unique<PostProcessSettingsCommand>(
          scene, m_ppDragStart, pp);
      m_history.PushWithoutExecute(std::move(cmd));
      m_ppDragActive = false;
    }
  };
  auto PPCheckbox = [&](const char *label, bool *val) {
    ScenePostProcessSettings before = pp;
    if (ImGui::Checkbox(label, val)) {
      auto cmd =
          std::make_unique<PostProcessSettingsCommand>(scene, before, pp);
      m_history.PushWithoutExecute(std::move(cmd));
    }
  };

  // ---- Tone Mapping ----
  if (ImGui::CollapsingHeader("Tone Mapping",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::SliderFloat("Exposure", &pp.exposure, 0.01f, 10.0f, "%.2f",
                       ImGuiSliderFlags_Logarithmic);
    CheckPPDragStart();
    CheckPPDragEnd();
  }

  // ---- Bloom ----
  if (ImGui::CollapsingHeader("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
    PPCheckbox("Enable Bloom", &pp.bloomEnabled);

    ImGui::SliderFloat("Threshold##Bloom", &pp.bloomThreshold, 0.0f, 5.0f,
                       "%.2f");
    CheckPPDragStart();
    CheckPPDragEnd();

    ImGui::SliderFloat("Intensity##Bloom", &pp.bloomIntensity, 0.0f, 2.0f,
                       "%.2f");
    CheckPPDragStart();
    CheckPPDragEnd();
  }

  // ---- TAA ----
  if (ImGui::CollapsingHeader("TAA")) {
    bool prevTaa = pp.taaEnabled;
    PPCheckbox("Enable TAA", &pp.taaEnabled);
    if (pp.taaEnabled && !prevTaa) {
      dx.ResetTaaFirstFrame();
    }

    if (pp.taaEnabled) {
      ImGui::SliderFloat("Blend Factor##TAA", &pp.taaBlendFactor, 0.01f, 0.5f,
                         "%.3f");
      CheckPPDragStart();
      CheckPPDragEnd();
    }
  }

  // ---- FXAA ----
  if (ImGui::CollapsingHeader("FXAA")) {
    PPCheckbox("Enable FXAA", &pp.fxaaEnabled);
  }

  // ---- Motion Blur ----
  if (ImGui::CollapsingHeader("Motion Blur")) {
    PPCheckbox("Enable Motion Blur", &pp.motionBlurEnabled);

    if (pp.motionBlurEnabled) {
      ImGui::SliderFloat("Strength##MB", &pp.motionBlurStrength, 0.0f, 3.0f,
                         "%.2f");
      CheckPPDragStart();
      CheckPPDragEnd();

      ImGui::SliderInt("Samples##MB", &pp.motionBlurSamples, 4, 32);
      CheckPPDragStart();
      CheckPPDragEnd();
    }
  }

  // ---- Depth of Field ----
  if (ImGui::CollapsingHeader("Depth of Field")) {
    PPCheckbox("Enable DOF", &pp.dofEnabled);

    if (pp.dofEnabled) {
      ImGui::SliderFloat("Focal Distance##DOF", &pp.dofFocalDistance, 0.5f,
                         100.0f, "%.1f");
      CheckPPDragStart();
      CheckPPDragEnd();

      ImGui::SliderFloat("Focal Range##DOF", &pp.dofFocalRange, 0.5f, 50.0f,
                         "%.1f");
      CheckPPDragStart();
      CheckPPDragEnd();

      ImGui::SliderFloat("Max Blur##DOF", &pp.dofMaxBlur, 1.0f, 20.0f,
                         "%.1f");
      CheckPPDragStart();
      CheckPPDragEnd();
    }
  }

  ImGui::End();
}

// ---- Integrated engine systems panel ----

void SceneEditor::DrawRuntimeSystemsPanel(Scene &scene, DxContext &dx,
                                          Camera *cam, bool *iblEnabled,
                                          EditorRuntimeBindings *runtime) {
  const EditorWorkspaceLayout layout = BuildEditorWorkspaceLayout();
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float panelY = layout.menuHeight + layout.sceneObjectsHeight;
  ImGui::SetNextWindowPos(ImVec2(0.0f, panelY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(
      ImVec2(layout.leftWidth, std::max(1.0f, display.y - panelY)),
      ImGuiCond_Always);
  ImGui::Begin("Systems", nullptr, kFixedPanelFlags);

  if (ImGui::BeginTabBar("##EngineSystems",
                         ImGuiTabBarFlags_FittingPolicyScroll)) {
    const auto beginSystemsTab = [&](const char *label, int tabIndex) {
      const ImGuiTabItemFlags flags =
          m_requestedSystemsTab == tabIndex
              ? ImGuiTabItemFlags_SetSelected
              : ImGuiTabItemFlags_None;
      const bool open = ImGui::BeginTabItem(label, nullptr, flags);
      if (open && m_requestedSystemsTab == tabIndex)
        m_requestedSystemsTab = 0;
      return open;
    };

    if (beginSystemsTab("Environment", 1)) {
      auto &ls = scene.LightSettings();
      const auto trackLightEdit = [&]() {
        if (ImGui::IsItemActivated() && !m_lightDragActive) {
          m_lightDragStart = ls;
          m_lightDragActive = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && m_lightDragActive) {
          m_history.PushWithoutExecute(std::make_unique<LightSettingsCommand>(
              scene, m_lightDragStart, ls));
          m_lightDragActive = false;
        }
      };

      if (ImGui::CollapsingHeader("Sun",
                                  ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat3("Direction", &ls.lightDir.x, -1.0f, 1.0f);
        trackLightEdit();
        ImGui::DragFloat("Intensity", &ls.lightIntensity, 0.05f, 0.0f,
                         50.0f, "%.2f");
        trackLightEdit();
        ImGui::ColorEdit3("Color", &ls.lightColor.x);
        trackLightEdit();
      }

      if (ImGui::CollapsingHeader("Image Based Lighting",
                                  ImGuiTreeNodeFlags_DefaultOpen)) {
        if (iblEnabled)
          ImGui::Checkbox("Enabled##IBL", iblEnabled);
        ImGui::SliderFloat("Intensity##IBL", &ls.iblIntensity, 0.0f, 5.0f,
                           "%.2f");
        trackLightEdit();
      }

      if (runtime && runtime->skyExposure)
        ImGui::SliderFloat("Sky Exposure", runtime->skyExposure, 0.01f, 8.0f,
                           "%.2f", ImGuiSliderFlags_Logarithmic);

      if (ImGui::CollapsingHeader("Weather and Time",
                                  ImGuiTreeNodeFlags_DefaultOpen)) {
        if (runtime && runtime->timeOfDayHours) {
          ImGui::SliderFloat("Time of Day", runtime->timeOfDayHours, 0.0f,
                             24.0f, "%.2f h");
          if (runtime->automaticTime)
            ImGui::Checkbox("Automatic Time", runtime->automaticTime);
          if (runtime->hoursPerSecond)
            ImGui::DragFloat("Hours / Second", runtime->hoursPerSecond,
                             0.01f, 0.0f, 24.0f, "%.2f");
          if (runtime->rainEnabled)
            ImGui::Checkbox("Rain", runtime->rainEnabled);
        } else {
          ImGui::TextDisabled("Start the connected game runtime to edit time and rain.");
        }
      }
      ImGui::EndTabItem();
    }

    if (beginSystemsTab("Rendering", 2)) {
      auto &ss = scene.ShadowSettings();
      auto &pp = scene.PostProcessSettings();
      const auto trackShadowEdit = [&]() {
        if (ImGui::IsItemActivated() && !m_shadowDragActive) {
          m_shadowDragStart = ss;
          m_shadowDragActive = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && m_shadowDragActive) {
          m_history.PushWithoutExecute(
              std::make_unique<ShadowSettingsCommand>(scene,
                                                       m_shadowDragStart, ss));
          m_shadowDragActive = false;
        }
      };
      const auto trackPostEdit = [&]() {
        if (ImGui::IsItemActivated() && !m_ppDragActive) {
          m_ppDragStart = pp;
          m_ppDragActive = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && m_ppDragActive) {
          m_history.PushWithoutExecute(
              std::make_unique<PostProcessSettingsCommand>(
                  scene, m_ppDragStart, pp));
          m_ppDragActive = false;
        }
      };
      const auto shadowCheckbox = [&](const char *label, bool *value) {
        const SceneShadowSettings before = ss;
        if (ImGui::Checkbox(label, value)) {
          m_history.PushWithoutExecute(std::make_unique<ShadowSettingsCommand>(
              scene, before, ss));
          return true;
        }
        return false;
      };
      const auto postCheckbox = [&](const char *label, bool *value) {
        const ScenePostProcessSettings before = pp;
        if (ImGui::Checkbox(label, value)) {
          m_history.PushWithoutExecute(
              std::make_unique<PostProcessSettingsCommand>(scene, before, pp));
          return true;
        }
        return false;
      };

      if (ImGui::CollapsingHeader("Reflections and DXR",
                                  ImGuiTreeNodeFlags_DefaultOpen)) {
        const bool bridgeReady = runtime && runtime->getReflectionMode &&
                                 runtime->setReflectionMode;
        if (!bridgeReady) {
          ImGui::TextDisabled("Reflection runtime bindings are unavailable.");
        } else {
          const auto modeLabel = [](int mode) {
            return mode == 0   ? "Off"
                   : mode == 2 ? "Hybrid DXR"
                               : "SSR";
          };
          const int requestedMode = runtime->getReflectionMode();
          const int activeMode = runtime->getActiveReflectionMode
                                     ? runtime->getActiveReflectionMode()
                                     : requestedMode;
          const bool dxrSupported =
              runtime->dxrSupported && runtime->dxrSupported();

          ImGui::Text("Requested: %s", modeLabel(requestedMode));
          ImGui::SameLine();
          ImGui::Text("Active: %s", modeLabel(activeMode));
          static constexpr const char *kModeLabels[] = {"Off", "SSR",
                                                        "Hybrid DXR"};
          for (int mode = 0; mode < 3; ++mode) {
            if (mode > 0)
              ImGui::SameLine();
            const bool disableChoice = mode == 2 && !dxrSupported;
            if (disableChoice)
              ImGui::BeginDisabled();
            ImGui::PushID(mode);
            if (ImGui::RadioButton(kModeLabels[mode],
                                   requestedMode == mode)) {
              runtime->setReflectionMode(mode);
            }
            ImGui::PopID();
            if (disableChoice)
              ImGui::EndDisabled();
          }

          if (requestedMode != activeMode) {
            ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.25f, 1.0f),
                               "Fallback active: requested %s, using %s.",
                               modeLabel(requestedMode), modeLabel(activeMode));
          }

          const bool dxilReady = runtime->dxrShaderAvailable &&
                                 runtime->dxrShaderAvailable();
          const bool sceneReady =
              runtime->dxrSceneReady && runtime->dxrSceneReady();
          const D3D12_RAYTRACING_TIER tier =
              runtime->dxrTier ? runtime->dxrTier()
                               : D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
          const char *tierLabel =
              tier >= D3D12_RAYTRACING_TIER_1_1
                  ? "1.1"
                  : (tier >= D3D12_RAYTRACING_TIER_1_0 ? "1.0"
                                                       : "Unavailable");
          const uint32_t instanceCount = runtime->dxrInstanceCount
                                             ? runtime->dxrInstanceCount()
                                             : 0;
          const uint32_t blasCount =
              runtime->dxrBlasCount ? runtime->dxrBlasCount() : 0;
          ImGui::TextColored(
              dxrSupported ? ImVec4(0.35f, 0.90f, 0.45f, 1.0f)
                           : ImVec4(1.0f, 0.45f, 0.30f, 1.0f),
              "Tier %s | DXIL %s | Scene %s", tierLabel,
              dxilReady ? "Ready" : "Missing",
              sceneReady ? "Ready" : "Pending");
          ImGui::Text("TLAS instances: %u | Cached BLAS: %u", instanceCount,
                      blasCount);
          if (runtime->dxrStatus) {
            const std::string status = MakeAsciiUiText(runtime->dxrStatus());
            if (!status.empty())
              ImGui::TextWrapped("%s", status.c_str());
          }
        }
      }

      if (ImGui::CollapsingHeader("Cascaded Shadows",
                                  ImGuiTreeNodeFlags_DefaultOpen)) {
        shadowCheckbox("Enabled##Shadows", &ss.shadowsEnabled);
        ImGui::SliderFloat("Strength##Shadows", &ss.shadowStrength, 0.0f,
                           1.0f, "%.2f");
        trackShadowEdit();
        ImGui::SliderFloat("Bias##Shadows", &ss.shadowBias, 0.00001f, 0.01f,
                           "%.5f", ImGuiSliderFlags_Logarithmic);
        trackShadowEdit();
        ImGui::SliderFloat("Split Lambda", &ss.csmLambda, 0.0f, 1.0f,
                           "%.2f");
        trackShadowEdit();
        ImGui::DragFloat("Max Distance", &ss.csmMaxDistance, 1.0f, 20.0f,
                         500.0f, "%.0f");
        trackShadowEdit();
        shadowCheckbox("Debug Cascades", &ss.csmDebugCascades);
        ImGui::TextDisabled("%u px x %u cascades", dx.GetShadowMap().Size(),
                            dx.GetShadowMap().CascadeCount());
      }

      if (ImGui::CollapsingHeader("SSAO",
                                  ImGuiTreeNodeFlags_DefaultOpen)) {
        shadowCheckbox("Enabled##SSAO", &ss.ssaoEnabled);
        ImGui::SliderFloat("Radius##SSAOIntegrated", &ss.ssaoRadius, 0.05f,
                           2.0f, "%.2f");
        trackShadowEdit();
        ImGui::SliderFloat("Bias##SSAOIntegrated", &ss.ssaoBias, 0.001f,
                           0.1f, "%.4f", ImGuiSliderFlags_Logarithmic);
        trackShadowEdit();
        ImGui::SliderFloat("Power##SSAOIntegrated", &ss.ssaoPower, 0.5f,
                           8.0f, "%.1f");
        trackShadowEdit();
        ImGui::SliderInt("Kernel##SSAO", &ss.ssaoKernelSize, 8, 64);
        trackShadowEdit();
        ImGui::SliderFloat("Strength##SSAOIntegrated", &ss.ssaoStrength,
                           0.0f, 2.0f, "%.2f");
        trackShadowEdit();
        ImGui::TextDisabled("AO buffer: %u x %u", dx.SsaoWidth(),
                            dx.SsaoHeight());
      }

      if (ImGui::CollapsingHeader("Post Processing",
                                  ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Exposure", &pp.exposure, 0.01f, 10.0f, "%.2f",
                           ImGuiSliderFlags_Logarithmic);
        trackPostEdit();
        postCheckbox("Bloom", &pp.bloomEnabled);
        if (pp.bloomEnabled) {
          ImGui::SliderFloat("Threshold##BloomIntegrated", &pp.bloomThreshold,
                             0.0f, 5.0f, "%.2f");
          trackPostEdit();
          ImGui::SliderFloat("Intensity##BloomIntegrated", &pp.bloomIntensity,
                             0.0f, 2.0f, "%.2f");
          trackPostEdit();
        }

        const bool taaWasEnabled = pp.taaEnabled;
        const bool taaChanged = postCheckbox("TAA", &pp.taaEnabled);
        if (taaChanged && pp.taaEnabled && !taaWasEnabled) {
          dx.ResetTaaFirstFrame();
        }
        if (pp.taaEnabled) {
          ImGui::SliderFloat("Blend##TAA", &pp.taaBlendFactor, 0.01f, 0.5f,
                             "%.3f");
          trackPostEdit();
        }
        postCheckbox("FXAA", &pp.fxaaEnabled);
        postCheckbox("Motion Blur", &pp.motionBlurEnabled);
        if (pp.motionBlurEnabled) {
          ImGui::SliderFloat("Strength##MotionBlur", &pp.motionBlurStrength,
                             0.0f, 3.0f, "%.2f");
          trackPostEdit();
          ImGui::SliderInt("Samples##MotionBlur", &pp.motionBlurSamples, 4,
                           32);
          trackPostEdit();
        }
        postCheckbox("Depth of Field", &pp.dofEnabled);
        if (pp.dofEnabled) {
          ImGui::DragFloat("Focal Distance", &pp.dofFocalDistance, 0.1f,
                           0.5f, 100.0f, "%.1f");
          trackPostEdit();
          ImGui::DragFloat("Focal Range", &pp.dofFocalRange, 0.1f, 0.5f,
                           50.0f, "%.1f");
          trackPostEdit();
          ImGui::DragFloat("Maximum Blur", &pp.dofMaxBlur, 0.1f, 1.0f,
                           20.0f, "%.1f");
          trackPostEdit();
        }
      }
      ImGui::EndTabItem();
    }

    if (beginSystemsTab("World", 3)) {
      if (!runtime) {
        ImGui::TextDisabled("Runtime bindings are not connected.");
      } else {
        if (runtime->saveRuntimeSettings &&
            ImGui::Button("Save Runtime Settings")) {
          m_runtimeSettingsMessage = runtime->saveRuntimeSettings()
                                         ? "Runtime settings saved."
                                         : "Runtime settings save failed.";
        }
        if (runtime->loadRuntimeSettings) {
          ImGui::SameLine();
          if (ImGui::Button("Load")) {
            m_runtimeSettingsMessage = runtime->loadRuntimeSettings()
                                           ? "Runtime settings loaded."
                                           : "Runtime settings load failed.";
          }
        }
        if (!m_runtimeSettingsMessage.empty())
          ImGui::TextWrapped("%s", m_runtimeSettingsMessage.c_str());
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Water Surface",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
          if (runtime->waterWaveHeight)
            ImGui::SliderFloat("Wave Height", runtime->waterWaveHeight, 0.0f,
                               4.0f, "%.2f");
          if (runtime->waterWaveSpeed)
            ImGui::SliderFloat("Wave Speed", runtime->waterWaveSpeed, 0.0f,
                               5.0f, "%.2f");
          if (runtime->waterWaveFrequency)
            ImGui::SliderFloat("Wave Frequency", runtime->waterWaveFrequency,
                               0.0f, 5.0f, "%.2f");
          if (runtime->waterTransparency &&
              ImGui::SliderFloat("Transparency", runtime->waterTransparency,
                                 0.0f, 1.0f, "%.2f")) {
            if (runtime->setWaterTransparency)
              runtime->setWaterTransparency(*runtime->waterTransparency);
          }
          if (runtime->resetWater && ImGui::Button("Reset Water"))
            runtime->resetWater();
        }

        if (ImGui::CollapsingHeader("Wet Surfaces and Puddles")) {
          if (runtime->wetSurfaceStrength)
            ImGui::SliderFloat("Wet Strength", runtime->wetSurfaceStrength,
                               0.0f, 2.0f, "%.2f");
          if (runtime->wetSurfaceDrySeconds)
            ImGui::DragFloat("Dry Time", runtime->wetSurfaceDrySeconds, 0.1f,
                             0.0f, 30.0f, "%.1f s");
          if (runtime->wetSurfaceImpactRadius)
            ImGui::DragFloat("Impact Radius", runtime->wetSurfaceImpactRadius,
                             0.1f, 0.0f, 20.0f, "%.1f");
          if (runtime->wetSurfaceCycleSeconds)
            ImGui::DragFloat("Cycle", runtime->wetSurfaceCycleSeconds, 0.1f,
                             0.0f, 30.0f, "%.1f s");
          if (runtime->puddleStrength)
            ImGui::SliderFloat("Puddle Strength", runtime->puddleStrength,
                               0.0f, 2.0f, "%.2f");
          if (runtime->puddleBuildSeconds)
            ImGui::DragFloat("Build Time", runtime->puddleBuildSeconds, 0.1f,
                             0.0f, 30.0f, "%.1f s");
          if (runtime->puddleRadius)
            ImGui::DragFloat("Puddle Radius", runtime->puddleRadius, 0.1f,
                             0.0f, 20.0f, "%.1f");
          if (runtime->puddleClarity)
            ImGui::SliderFloat("Clarity", runtime->puddleClarity, 0.0f, 1.0f,
                               "%.2f");
          if (runtime->puddleTint)
            ImGui::SliderFloat("Tint", runtime->puddleTint, 0.0f, 1.0f,
                               "%.2f");
          if (runtime->puddleRippleStrength)
            ImGui::SliderFloat("Ripple", runtime->puddleRippleStrength, 0.0f,
                               2.0f, "%.2f");
        }

        if (ImGui::CollapsingHeader("Background Forest")) {
          if (runtime->forestEnabled)
            ImGui::Checkbox("Enabled##Forest", runtime->forestEnabled);
          if (runtime->forestSingleCluster)
            ImGui::Checkbox("Single Cluster", runtime->forestSingleCluster);
          if (runtime->forestDensity)
            ImGui::SliderInt("Density", runtime->forestDensity, 0, 3);
          if (runtime->forestScale)
            ImGui::SliderFloat("Scale##Forest", runtime->forestScale, 0.05f,
                               2.0f, "%.2f");
          if (runtime->forestDistance)
            ImGui::DragFloat("Distance##Forest", runtime->forestDistance,
                             0.5f, 5.0f, 150.0f, "%.1f");
          if (runtime->forestSpacing)
            ImGui::DragFloat("Spacing##Forest", runtime->forestSpacing, 0.05f,
                             0.1f, 8.0f, "%.2f");
          if (runtime->resetForest && ImGui::Button("Reset Forest"))
            runtime->resetForest();
          if (runtime->reloadPlacements) {
            ImGui::SameLine();
            if (ImGui::Button("Reload World"))
              runtime->reloadPlacements();
          }
        }

        if (ImGui::CollapsingHeader("Collision Debug")) {
          if (runtime->collisionDebug)
            ImGui::Checkbox("Collider Wireframes", runtime->collisionDebug);
          if (runtime->modelMeshCollision)
            ImGui::Checkbox("Model Mesh Collision", runtime->modelMeshCollision);
          if (runtime->modelCollisionDebug)
            ImGui::Checkbox("Model Collision Meshes",
                            runtime->modelCollisionDebug);
          if (runtime->gameFreeCamera)
            ImGui::Checkbox("Game Free Camera", runtime->gameFreeCamera);
        }
      }
      ImGui::EndTabItem();
    }

    if (beginSystemsTab("VFX", 4)) {
      if (runtime) {
        if (runtime->particlesEnabled)
          ImGui::Checkbox("Particle System", runtime->particlesEnabled);
        if (runtime->fireEnabled)
          ImGui::Checkbox("Fire Emitter", runtime->fireEnabled);
        if (runtime->smokeEnabled)
          ImGui::Checkbox("Smoke Emitter", runtime->smokeEnabled);
        if (runtime->sparkEnabled)
          ImGui::Checkbox("Spark Emitter", runtime->sparkEnabled);
        if (runtime->particleDepth)
          ImGui::DragFloat("Particle Depth", runtime->particleDepth, 0.1f,
                           -100.0f, 100.0f, "%.1f");
        ImGui::Separator();
        ImGui::TextWrapped(
            "Gameplay-only boss and puzzle emitters are driven by the real "
            "Game View state machine.");
        if (runtime->drawAnimationControls &&
            ImGui::CollapsingHeader("Player Animation Preview",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
          runtime->drawAnimationControls();
        }
      } else {
        ImGui::TextDisabled("Runtime VFX bindings are not connected.");
      }
      ImGui::EndTabItem();
    }

    if (beginSystemsTab("Camera", 5)) {
      if (!cam) {
        ImGui::TextDisabled("No editor camera is connected.");
      } else {
        ImGui::Checkbox("Camera Navigation", &m_cameraNavigationEnabled);
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "Disable to lock the Scene View camera. Numeric controls and "
              "presets remain adjustable.");
        }
        ImGui::SameLine();
        ImGui::TextColored(
            m_cameraNavigationEnabled
                ? ImVec4(0.35f, 0.9f, 0.45f, 1.0f)
                : ImVec4(1.0f, 0.62f, 0.25f, 1.0f),
            m_cameraNavigationEnabled ? "ACTIVE" : "LOCKED");
        ImGui::Separator();

        int mode = static_cast<int>(cam->Mode());
        const char *modes[] = {"Free Fly", "Orbit", "Game Top-Down"};
        if (ImGui::Combo("Mode", &mode, modes, IM_ARRAYSIZE(modes)))
          cam->SetMode(static_cast<CameraMode>(mode));

        DirectX::XMFLOAT3 position = cam->GetPosition();
        if (cam->Mode() == CameraMode::FreeFly &&
            ImGui::DragFloat3("Position", &position.x, 0.1f)) {
          cam->SetPosition(position.x, position.y, position.z);
        }
        if (cam->Mode() == CameraMode::FreeFly) {
          float yaw = DirectX::XMConvertToDegrees(cam->Yaw());
          float pitch = DirectX::XMConvertToDegrees(cam->Pitch());
          if (ImGui::DragFloat("Yaw", &yaw, 0.5f, -180.0f, 180.0f))
            cam->SetYawPitch(DirectX::XMConvertToRadians(yaw), cam->Pitch());
          if (ImGui::DragFloat("Pitch", &pitch, 0.5f, -89.0f, 89.0f))
            cam->SetYawPitch(cam->Yaw(), DirectX::XMConvertToRadians(pitch));
        } else {
          ImGui::Text("Position: %.1f, %.1f, %.1f", position.x, position.y,
                      position.z);
        }

        if (cam->Mode() == CameraMode::Orbit) {
          DirectX::XMFLOAT3 target = cam->OrbitTarget();
          if (ImGui::DragFloat3("Orbit Target", &target.x, 0.1f))
            cam->SetOrbitTarget(target.x, target.y, target.z);
          float distance = cam->OrbitDistance();
          if (ImGui::DragFloat("Orbit Distance", &distance, 0.1f, 0.5f,
                               500.0f))
            cam->SetOrbitDistance(distance);
        }

        float fovDegrees = DirectX::XMConvertToDegrees(cam->FovY());
        float nearPlane = cam->NearZ();
        float farPlane = cam->FarZ();
        bool lensChanged =
            ImGui::SliderFloat("Field of View", &fovDegrees, 10.0f, 120.0f);
        lensChanged |= ImGui::DragFloat("Near Plane", &nearPlane, 0.01f,
                                        0.001f, 10.0f, "%.3f");
        lensChanged |= ImGui::DragFloat("Far Plane", &farPlane, 1.0f, 10.0f,
                                        10000.0f, "%.0f");
        if (lensChanged) {
          cam->SetLens(DirectX::XMConvertToRadians(fovDegrees), cam->Aspect(),
                       nearPlane, farPlane);
        }

        float moveSpeed = cam->MoveSpeed();
        float lookSpeed = cam->LookSpeed() * 1000.0f;
        if (ImGui::DragFloat("Move Speed", &moveSpeed, 0.1f, 0.1f, 100.0f))
          cam->SetMoveSpeed(moveSpeed);
        if (ImGui::DragFloat("Look Speed", &lookSpeed, 0.1f, 0.1f, 20.0f))
          cam->SetLookSpeed(lookSpeed * 0.001f);

        ImGui::Separator();
        ImGui::InputText("Preset Name", m_presetName, sizeof(m_presetName));
        if (ImGui::Button("Save Camera Preset"))
          scene.CameraPresets().push_back(cam->MakePreset(m_presetName));
        for (int i = 0;
             i < static_cast<int>(scene.CameraPresets().size()); ++i) {
          ImGui::PushID(i);
          if (ImGui::SmallButton("Load"))
            cam->ApplyPreset(scene.CameraPresets()[i]);
          ImGui::SameLine();
          ImGui::TextUnformatted(scene.CameraPresets()[i].name.c_str());
          ImGui::PopID();
        }
      }
      ImGui::EndTabItem();
    }

    if (beginSystemsTab("Features", 6)) {
      DrawFeatureCoveragePanel(scene, iblEnabled, runtime);
      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }

  ImGui::End();
}

void SceneEditor::DrawFeatureCoveragePanel(
    Scene &scene, bool *iblEnabled, EditorRuntimeBindings *runtime) {
  if (!m_featureInventoryLoaded)
    LoadFeatureInventory();

  if (ImGui::Button("Reload feature coverage"))
    LoadFeatureInventory();
  ImGui::SameLine();
  ImGui::Text("%zu + %zu", m_featureInventory.size(),
              m_featureShowcase.size());
  if (!m_featureInventoryMessage.empty()) {
    const bool warning =
        m_featureInventoryMessage.find("warning") != std::string::npos ||
        m_featureInventoryMessage.find("unavailable") != std::string::npos ||
        m_featureInventoryMessage.find("fallback") != std::string::npos ||
        m_featureInventory.empty() || m_featureShowcase.empty();
    ImGui::PushStyleColor(ImGuiCol_Text,
                          warning ? ImVec4(1.0f, 0.40f, 0.30f, 1.0f)
                                  : ImVec4(0.35f, 0.90f, 0.45f, 1.0f));
    ImGui::TextWrapped("%s", m_featureInventoryMessage.c_str());
    ImGui::PopStyleColor();
  }

  if (runtime &&
      (runtime->playTitle || runtime->playOverworld ||
       runtime->playTavern || runtime->playBossArena)) {
    bool hasPreviousLauncher = false;
    const auto launcher = [&](const char *label,
                              const std::function<void()> &callback) {
      if (!callback)
        return;
      if (hasPreviousLauncher)
        ImGui::SameLine();
      if (ImGui::SmallButton(label))
        callback();
      hasPreviousLauncher = true;
    };
    launcher("Run Title", runtime->playTitle);
    launcher("Run Overworld", runtime->playOverworld);
    launcher("Run Tavern", runtime->playTavern);
    launcher("Run Boss", runtime->playBossArena);
    ImGui::TextDisabled("F1 returns to the Editor.");
  }

  if (ImGui::BeginTabBar("##FeatureCoverageViews",
                         ImGuiTabBarFlags_FittingPolicyScroll)) {
    if (ImGui::BeginTabItem("Showcase")) {
      size_t unmapped = 0;
      size_t duplicateIds = 0;
      for (size_t index = 0; index < m_featureShowcase.size(); ++index) {
        const FeatureShowcaseEntry &entry = m_featureShowcase[index];
        if (RouteForShowcaseId(entry.id) == FeatureRoute::Unmapped)
          ++unmapped;
        for (size_t previous = 0; previous < index; ++previous) {
          if (m_featureShowcase[previous].id == entry.id) {
            ++duplicateIds;
            break;
          }
        }
      }
      const size_t mapped = m_featureShowcase.size() - unmapped;
      const ImVec4 contractColor =
          unmapped == 0 && duplicateIds == 0
              ? ImVec4(0.35f, 0.90f, 0.45f, 1.0f)
              : ImVec4(1.0f, 0.35f, 0.28f, 1.0f);
      ImGui::TextColored(contractColor,
                         "%zu / %zu catalog routes mapped | %zu unmapped | "
                         "%zu duplicate IDs",
                         mapped, m_featureShowcase.size(), unmapped,
                         duplicateIds);
      ImGui::TextWrapped(
          "Checkboxes change real runtime state. Open/Run routes lead to "
          "the real control panel or Game View. Renderer foundations are "
          "never represented by fake switches.");

      static char showcaseFilter[96] = {};
      static std::string showcaseCategory;
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputTextWithHint("##ShowcaseFilter",
                               "Filter showcase ID, feature, category...",
                               showcaseFilter, sizeof(showcaseFilter));

      std::vector<std::string> showcaseCategories;
      for (const FeatureShowcaseEntry &entry : m_featureShowcase) {
        const std::string category =
            entry.category.empty() ? "Uncategorized" : entry.category;
        if (std::find(showcaseCategories.begin(), showcaseCategories.end(),
                      category) == showcaseCategories.end())
          showcaseCategories.push_back(category);
      }
      std::sort(showcaseCategories.begin(), showcaseCategories.end());
      if (!showcaseCategory.empty() &&
          std::find(showcaseCategories.begin(), showcaseCategories.end(),
                    showcaseCategory) == showcaseCategories.end())
        showcaseCategory.clear();

      ImGui::SetNextItemWidth(220.0f);
      const char *categoryPreview = showcaseCategory.empty()
                                        ? "All categories"
                                        : showcaseCategory.c_str();
      if (ImGui::BeginCombo("##ShowcaseCategory", categoryPreview)) {
        if (ImGui::Selectable("All categories", showcaseCategory.empty()))
          showcaseCategory.clear();
        for (const std::string &category : showcaseCategories) {
          const bool selected = category == showcaseCategory;
          if (ImGui::Selectable(category.c_str(), selected))
            showcaseCategory = category;
          if (selected)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      const std::string normalizedShowcaseFilter =
          LowerAscii(showcaseFilter);
      const auto entryMatchesFilter =
          [&normalizedShowcaseFilter](const FeatureShowcaseEntry &entry) {
            if (normalizedShowcaseFilter.empty())
              return true;
            const std::string searchable = LowerAscii(
                entry.id + " " + entry.category + " " + entry.feature + " " +
                entry.defaultValue + " " + entry.detail);
            return searchable.find(normalizedShowcaseFilter) !=
                   std::string::npos;
          };

      ImGui::BeginChild("##ShowcaseFeatureList", ImVec2(0.0f, 0.0f), true);
      for (const std::string &category : showcaseCategories) {
        if (!showcaseCategory.empty() && category != showcaseCategory)
          continue;

        bool categoryHasVisibleEntry = false;
        for (const FeatureShowcaseEntry &entry : m_featureShowcase) {
          const std::string entryCategory =
              entry.category.empty() ? "Uncategorized" : entry.category;
          if (entryCategory == category && entryMatchesFilter(entry)) {
            categoryHasVisibleEntry = true;
            break;
          }
        }
        if (!categoryHasVisibleEntry)
          continue;

        ImGui::PushID(category.c_str());
        ImGui::SeparatorText(category.c_str());
        for (size_t index = 0; index < m_featureShowcase.size(); ++index) {
          const FeatureShowcaseEntry &entry = m_featureShowcase[index];
          const std::string entryCategory =
              entry.category.empty() ? "Uncategorized" : entry.category;
          if (entryCategory != category || !entryMatchesFilter(entry))
            continue;

          const FeatureRoute route = RouteForShowcaseId(entry.id);
          bool *toggle = nullptr;
          if (iblEnabled &&
              (entry.id == "ibl" || entry.id == "ibl_irradiance"))
            toggle = iblEnabled;
          else if (entry.id == "shadows" || entry.id == "shadow_controls")
            toggle = &scene.ShadowSettings().shadowsEnabled;
          else if (entry.id == "ssao")
            toggle = &scene.ShadowSettings().ssaoEnabled;
          else if (entry.id == "bloom" || entry.id == "bloom_mip_chain")
            toggle = &scene.PostProcessSettings().bloomEnabled;
          else if (entry.id == "fxaa")
            toggle = &scene.PostProcessSettings().fxaaEnabled;
          else if (entry.id == "motion_blur")
            toggle = &scene.PostProcessSettings().motionBlurEnabled;
          else if (entry.id == "dof" || entry.id == "depth_of_field")
            toggle = &scene.PostProcessSettings().dofEnabled;
          else if (entry.id == "camera_orbit")
            toggle = &m_cameraNavigationEnabled;
          else if (runtime &&
                   (entry.id == "rain" || entry.id == "world_rain"))
            toggle = runtime->rainEnabled;
          else if (runtime && entry.id == "time_of_day")
            toggle = runtime->automaticTime;
          else if (runtime && (entry.id == "particles" ||
                               entry.id == "vfx_particle_system"))
            toggle = runtime->particlesEnabled;
          else if (runtime && entry.id == "imported_collision")
            toggle = runtime->modelMeshCollision;
          else if (runtime && (entry.id == "collision_debug" ||
                               entry.id == "editor_collision_debug"))
            toggle = runtime->collisionDebug;

          ImGui::PushID(static_cast<int>(index));
          const bool hybridDxrEntry = entry.id == "hybrid_dxr";
          if (hybridDxrEntry) {
            const bool bridgeReady =
                runtime && runtime->getReflectionMode &&
                runtime->setReflectionMode;
            if (!bridgeReady) {
              ImGui::TextDisabled("Unavailable");
            } else {
              const int mode = runtime->getReflectionMode();
              const char *modeLabel = mode == 0   ? "Off"
                                      : mode == 2 ? "Hybrid DXR"
                                                  : "SSR";
              const bool dxrSupported =
                  runtime->dxrSupported && runtime->dxrSupported();
              ImGui::SetNextItemWidth(118.0f);
              if (ImGui::BeginCombo("##ReflectionMode", modeLabel)) {
                static constexpr const char *kModeLabels[] = {
                    "Off", "SSR", "Hybrid DXR"};
                for (int requestedMode = 0; requestedMode < 3;
                     ++requestedMode) {
                  const bool disableChoice =
                      requestedMode == 2 && !dxrSupported;
                  if (disableChoice)
                    ImGui::BeginDisabled();
                  if (ImGui::Selectable(kModeLabels[requestedMode],
                                        mode == requestedMode)) {
                    const bool accepted =
                        runtime->setReflectionMode(requestedMode);
                    if (!accepted) {
                      m_featureInventoryMessage =
                          "Hybrid DXR is unavailable; runtime retained the "
                          "SSR fallback.";
                    }
                  }
                  if (mode == requestedMode)
                    ImGui::SetItemDefaultFocus();
                  if (disableChoice)
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
              }
            }
          } else if (toggle) {
            ImGui::Checkbox("##RuntimeToggle", toggle);
            if (entry.id == "ibl_irradiance" && ImGui::IsItemHovered())
              ImGui::SetTooltip(
                  "Global IBL runtime control: irradiance, prefilter and "
                  "BRDF LUT contribution.");
          } else {
            switch (route) {
            case FeatureRoute::SceneObjects:
              if (ImGui::SmallButton("Scene")) {
                m_featureInventoryMessage =
                    "Use the always-visible Scene Objects and Inspector "
                    "panels for this scene/material feature.";
              }
              break;
            case FeatureRoute::Environment:
              if (ImGui::SmallButton("Open"))
                m_requestedSystemsTab = 1;
              break;
            case FeatureRoute::Rendering:
              if (ImGui::SmallButton("Open"))
                m_requestedSystemsTab = 2;
              break;
            case FeatureRoute::World:
              if (ImGui::SmallButton("Open"))
                m_requestedSystemsTab = 3;
              break;
            case FeatureRoute::Vfx:
              if (ImGui::SmallButton("Open"))
                m_requestedSystemsTab = 4;
              break;
            case FeatureRoute::Camera:
              if (ImGui::SmallButton("Open"))
                m_requestedSystemsTab = 5;
              break;
            case FeatureRoute::Overworld:
              if (runtime && runtime->playOverworld) {
                if (ImGui::SmallButton("Run"))
                  runtime->playOverworld();
              } else {
                ImGui::TextDisabled("Overworld");
              }
              break;
            case FeatureRoute::Tavern:
              if (runtime && runtime->playTavern) {
                if (ImGui::SmallButton("Run"))
                  runtime->playTavern();
              } else {
                ImGui::TextDisabled("Tavern");
              }
              break;
            case FeatureRoute::Boss:
              if (runtime && runtime->playBossArena) {
                if (ImGui::SmallButton("Run"))
                  runtime->playBossArena();
              } else {
                ImGui::TextDisabled("Boss");
              }
              break;
            case FeatureRoute::Observed:
              ImGui::TextDisabled("Observed");
              break;
            case FeatureRoute::Legacy:
              ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.68f, 1.0f),
                                 "Legacy");
              break;
            case FeatureRoute::Unmapped:
              ImGui::TextColored(ImVec4(1.0f, 0.30f, 0.25f, 1.0f),
                                 "UNMAPPED");
              break;
            }
          }

          ImGui::SameLine();
          const ImVec4 routeColor =
              route == FeatureRoute::Legacy
                  ? ImVec4(0.62f, 0.62f, 0.65f, 1.0f)
                  : (route == FeatureRoute::Unmapped
                         ? ImVec4(1.0f, 0.30f, 0.25f, 1.0f)
                         : ImVec4(0.78f, 0.86f, 0.96f, 1.0f));
          ImGui::TextColored(routeColor, "%s", entry.feature.c_str());
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("ID: %s\nCategory: %s\nDefault: %s\n%s",
                              entry.id.c_str(), entry.category.c_str(),
                              entry.defaultValue.c_str(), entry.detail.c_str());
          }

          if (hybridDxrEntry) {
            const auto modeLabel = [](int mode) {
              return mode == 0   ? "Off"
                     : mode == 2 ? "Hybrid DXR"
                                 : "SSR";
            };
            const int requestedMode =
                runtime && runtime->getReflectionMode
                    ? runtime->getReflectionMode()
                    : 1;
            const int activeMode =
                runtime && runtime->getActiveReflectionMode
                    ? runtime->getActiveReflectionMode()
                    : requestedMode;
            const bool supported =
                runtime && runtime->dxrSupported && runtime->dxrSupported();
            const bool dxilReady = runtime && runtime->dxrShaderAvailable &&
                                   runtime->dxrShaderAvailable();
            const bool sceneReady = runtime && runtime->dxrSceneReady &&
                                    runtime->dxrSceneReady();
            const D3D12_RAYTRACING_TIER tier =
                runtime && runtime->dxrTier
                    ? runtime->dxrTier()
                    : D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
            const char *tierLabel =
                tier >= D3D12_RAYTRACING_TIER_1_1
                    ? "1.1"
                    : (tier >= D3D12_RAYTRACING_TIER_1_0 ? "1.0"
                                                         : "Unavailable");
            const uint32_t tlasInstances =
                runtime && runtime->dxrInstanceCount
                    ? runtime->dxrInstanceCount()
                    : 0;
            const uint32_t blasCount = runtime && runtime->dxrBlasCount
                                           ? runtime->dxrBlasCount()
                                           : 0;
            ImGui::Indent(18.0f);
            ImGui::Text("Requested %s | Active %s", modeLabel(requestedMode),
                        modeLabel(activeMode));
            if (requestedMode != activeMode) {
              ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.25f, 1.0f),
                                 "SSR fallback is active for this frame.");
            }
            ImGui::TextColored(
                supported ? ImVec4(0.35f, 0.90f, 0.45f, 1.0f)
                          : ImVec4(1.0f, 0.45f, 0.30f, 1.0f),
                "Tier %s | DXIL %s | Scene %s | TLAS %u | BLAS %u",
                tierLabel, dxilReady ? "Ready" : "Missing",
                sceneReady ? "Ready" : "Pending", tlasInstances, blasCount);
            if (runtime && runtime->dxrStatus) {
              const std::string status =
                  MakeAsciiUiText(runtime->dxrStatus());
              if (!status.empty())
                ImGui::TextWrapped("%s", status.c_str());
            }
            ImGui::Unindent(18.0f);
          }
          ImGui::PopID();
        }
        ImGui::PopID();
      }
      ImGui::EndChild();
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Inventory")) {
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputTextWithHint("##FeatureFilter", "Filter all features...",
                               m_featureFilter, sizeof(m_featureFilter));
      ImGui::Checkbox("Active", &m_showActiveFeatures);
      ImGui::SameLine();
      ImGui::Checkbox("Available", &m_showAvailableFeatures);
      ImGui::Checkbox("Experimental", &m_showExperimentalFeatures);
      ImGui::SameLine();
      ImGui::Checkbox("Unavailable", &m_showUnavailableFeatures);
      ImGui::SameLine();
      ImGui::Checkbox("Legacy", &m_showLegacyFeatures);

      const std::string filter = LowerAscii(m_featureFilter);
      ImGui::BeginChild("##CompleteFeatureInventory", ImVec2(0.0f, 0.0f),
                        true);
      for (size_t index = 0; index < m_featureInventory.size(); ++index) {
        const FeatureInventoryEntry &entry = m_featureInventory[index];
        const InventoryCoverage coverage = CoverageForInventory(entry);
        const bool visible =
            (coverage == InventoryCoverage::Active && m_showActiveFeatures) ||
            (coverage == InventoryCoverage::Available &&
             m_showAvailableFeatures) ||
            (coverage == InventoryCoverage::Experimental &&
             m_showExperimentalFeatures) ||
            (coverage == InventoryCoverage::Unavailable &&
             m_showUnavailableFeatures) ||
            (coverage == InventoryCoverage::Legacy && m_showLegacyFeatures);
        if (!visible)
          continue;

        if (!filter.empty()) {
          const std::string searchable =
              LowerAscii(entry.section + " " + entry.category + " " +
                         entry.feature + " " + entry.status + " " +
                         entry.source);
          if (searchable.find(filter) == std::string::npos)
            continue;
        }

        const char *label = "ACTIVE";
        ImVec4 color(0.35f, 0.90f, 0.45f, 1.0f);
        if (coverage == InventoryCoverage::Available) {
          label = "AVAILABLE";
          color = ImVec4(0.30f, 0.68f, 1.0f, 1.0f);
        } else if (coverage == InventoryCoverage::Experimental) {
          label = "EXPERIMENTAL";
          color = ImVec4(1.0f, 0.65f, 0.25f, 1.0f);
        } else if (coverage == InventoryCoverage::Unavailable) {
          label = "UNAVAILABLE";
          color = ImVec4(0.78f, 0.42f, 0.40f, 1.0f);
        } else if (coverage == InventoryCoverage::Legacy) {
          label = "LEGACY";
          color = ImVec4(0.62f, 0.62f, 0.65f, 1.0f);
        }

        ImGui::PushID(static_cast<int>(index));
        ImGui::TextColored(color, "[%s]", label);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", entry.feature.c_str());
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Section: %s\nCategory: %s\nStatus: %s\n%s\n%s",
                            entry.section.c_str(), entry.category.c_str(),
                            entry.status.c_str(), entry.detail.c_str(),
                            entry.source.c_str());
        }
        ImGui::PopID();
      }
      ImGui::EndChild();
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Limits")) {
      ImGui::TextWrapped(
          "VILLIEN Editor exposes the formal catalog and every feature.md "
          "inventory row for audit, but only safe runtime controls receive a "
          "checkbox. Renderer foundations and gameplay state machines are "
          "inspected through real views, not disabled with unsafe or "
          "simulated switches.");
      ImGui::Separator();
      ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.25f, 1.0f),
                         "NOT IMPLEMENTED");
      ImGui::BulletText("Audio authoring / BGM / SFX / Voice");
      ImGui::BulletText("NPC dialogue, quests and inventory");
      ImGui::BulletText("Gameplay save/load");
      ImGui::BulletText("Parent-child scene hierarchy and prefabs");
      ImGui::TextDisabled(
          "These remain unavailable until a real runtime data model exists.");
      if (iblEnabled) {
        ImGui::Separator();
        ImGui::Checkbox("Image Based Lighting", iblEnabled);
        ImGui::TextDisabled(
            "Same global runtime control used by the IBL Showcase entry.");
      }
      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }
}

void SceneEditor::BuildHighlightItems(const Scene &scene,
                                      FrameData &frame) const {
  if (m_selectedEntity == kInvalidEntityId)
    return;

  const Entity *e = scene.FindEntity(m_selectedEntity);
  if (!e || !e->active || !e->mesh.has_value())
    return;
  if (e->mesh->meshId == UINT32_MAX)
    return;

  frame.highlightItems.push_back({e->mesh->meshId, e->transform.WorldMatrix()});
}

// ---- Mouse picking via ray-AABB intersection ----

static DirectX::XMFLOAT3 MeshLocalHalfExtents(const MeshComponent &mc) {
  using namespace DirectX;
  switch (mc.sourceType) {
  case MeshSourceType::ProceduralCube: {
    float h = mc.size * 0.5f;
    return {h, h, h};
  }
  case MeshSourceType::ProceduralPlane:
    return {mc.width * 0.5f, 0.05f, mc.height * 0.5f};
  case MeshSourceType::ProceduralCylinder:
    return {mc.width, mc.height * 0.5f, mc.width};
  case MeshSourceType::ProceduralCone:
    return {mc.width, mc.height * 0.5f, mc.width};
  case MeshSourceType::ProceduralSphere:
    return {mc.size, mc.size, mc.size};
  default:
    return {0.5f, 0.5f, 0.5f};
  }
}

static DirectX::XMFLOAT3 MeshLocalCenter(const MeshComponent &mc) {
  using namespace DirectX;
  switch (mc.sourceType) {
  case MeshSourceType::ProceduralCone:
    return {0.0f, mc.height * 0.5f, 0.0f};
  default:
    return {0.0f, 0.0f, 0.0f};
  }
}

static bool RayVsAABB(DirectX::XMVECTOR rayOrigin, DirectX::XMVECTOR rayDir,
                      DirectX::XMFLOAT3 aabbMin, DirectX::XMFLOAT3 aabbMax,
                      float &tOut) {
  using namespace DirectX;
  float tMin = 0.0f;
  float tMax = 1e30f;

  float orig[3], dir[3], bmin[3], bmax[3];
  XMStoreFloat3(reinterpret_cast<XMFLOAT3 *>(orig), rayOrigin);
  XMStoreFloat3(reinterpret_cast<XMFLOAT3 *>(dir), rayDir);
  bmin[0] = aabbMin.x; bmin[1] = aabbMin.y; bmin[2] = aabbMin.z;
  bmax[0] = aabbMax.x; bmax[1] = aabbMax.y; bmax[2] = aabbMax.z;

  for (int i = 0; i < 3; ++i) {
    if (fabsf(dir[i]) < 1e-8f) {
      if (orig[i] < bmin[i] || orig[i] > bmax[i])
        return false;
    } else {
      float invD = 1.0f / dir[i];
      float t1 = (bmin[i] - orig[i]) * invD;
      float t2 = (bmax[i] - orig[i]) * invD;
      if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
      tMin = (t1 > tMin) ? t1 : tMin;
      tMax = (t2 < tMax) ? t2 : tMax;
      if (tMin > tMax)
        return false;
    }
  }
  tOut = tMin;
  return tMin >= 0.0f;
}

void SceneEditor::HandleMousePick(const Scene &scene, int screenX, int screenY,
                                  int screenW, int screenH,
                                  const DirectX::XMMATRIX &view,
                                  const DirectX::XMMATRIX &proj) {
  using namespace DirectX;

  int pickX = screenX;
  int pickY = screenY;
  int pickW = screenW;
  int pickH = screenH;
  if (m_viewportRect.width > 2.0f && m_viewportRect.height > 2.0f) {
    if (!m_viewportRect.Contains(screenX, screenY))
      return;
    pickX = screenX - static_cast<int>(m_viewportRect.x);
    pickY = screenY - static_cast<int>(m_viewportRect.y);
    pickW = std::max(1, static_cast<int>(m_viewportRect.width));
    pickH = std::max(1, static_cast<int>(m_viewportRect.height));
  }

  float ndcX = 2.0f * pickX / pickW - 1.0f;
  float ndcY = -(2.0f * pickY / pickH - 1.0f);

  XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);
  XMVECTOR nearPt =
      XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invViewProj);
  XMVECTOR farPt =
      XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invViewProj);
  XMVECTOR rayDir = XMVector3Normalize(XMVectorSubtract(farPt, nearPt));
  XMVECTOR rayOrigin = nearPt;

  float bestT = 1e30f;
  EntityId bestId = kInvalidEntityId;

  for (const auto &e : scene.Entities()) {
    if (!e.active || !e.mesh.has_value())
      continue;
    if (e.mesh->meshId == UINT32_MAX)
      continue;

    XMMATRIX world = e.transform.WorldMatrix();
    XMMATRIX invWorld = XMMatrixInverse(nullptr, world);

    XMVECTOR localOrigin = XMVector3TransformCoord(rayOrigin, invWorld);
    XMVECTOR localDir = XMVector3Normalize(
        XMVector3TransformNormal(rayDir, invWorld));

    XMFLOAT3 halfExt = MeshLocalHalfExtents(e.mesh.value());
    XMFLOAT3 center = MeshLocalCenter(e.mesh.value());
    XMFLOAT3 aabbMin = {center.x - halfExt.x, center.y - halfExt.y,
                         center.z - halfExt.z};
    XMFLOAT3 aabbMax = {center.x + halfExt.x, center.y + halfExt.y,
                         center.z + halfExt.z};

    float t = 0.0f;
    if (RayVsAABB(localOrigin, localDir, aabbMin, aabbMax, t)) {
      if (t < bestT) {
        bestT = t;
        bestId = e.id;
      }
    }
  }

  m_selectedEntity = bestId;
}

// ======================================================================
// Grid Editor Panel (Milestone 4 Phase 5)
// ======================================================================

// Tile type names for brush palette.
static const char *kTileTypeNames[] = {"Floor",     "Wall",  "Fire",
                                       "Lightning", "Spike", "Ice",
                                       "Crumble",   "Start", "Goal"};
static constexpr int kTileTypeCount = 9;

// Colors for mini-grid rendering (matches neon aesthetic).
static ImU32 TileColor(TileType type, bool hasWall) {
  if (hasWall)
    return IM_COL32(160, 120, 80, 255); // brown for placed wall
  switch (type) {
  case TileType::Floor:     return IM_COL32(30, 30, 45, 255);
  case TileType::Wall:      return IM_COL32(100, 100, 120, 255);
  case TileType::Fire:      return IM_COL32(220, 60, 20, 255);
  case TileType::Lightning: return IM_COL32(40, 80, 200, 255);
  case TileType::Spike:     return IM_COL32(200, 180, 40, 255);
  case TileType::Ice:       return IM_COL32(40, 180, 220, 255);
  case TileType::Crumble:   return IM_COL32(100, 80, 60, 255);
  case TileType::Start:     return IM_COL32(30, 180, 60, 255);
  case TileType::Goal:      return IM_COL32(220, 200, 40, 255);
  }
  return IM_COL32(30, 30, 45, 255);
}

static const char *kTowerSideNames[] = {"Left", "Right", "Top", "Bottom"};
static const char *kTowerPatternNames[] = {"Row", "Column", "Cross",
                                           "Diagonal"};

static bool OpenStageFileDialog(char *outPath, size_t maxLen, bool save) {
  OPENFILENAMEA ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFilter = "Stage Files\0*.stage.json\0JSON Files\0*.json\0All "
                    "Files\0*.*\0";
  ofn.lpstrFile = outPath;
  ofn.nMaxFile = static_cast<DWORD>(maxLen);
  ofn.lpstrDefExt = "json";
  ofn.Flags = OFN_NOCHANGEDIR;
  if (!save)
    ofn.Flags |= OFN_FILEMUSTEXIST;
  outPath[0] = '\0';
  return save ? GetSaveFileNameA(&ofn) != 0 : GetOpenFileNameA(&ofn) != 0;
}

// ---- Camera Panel (Phase 6) ----

void SceneEditor::DrawCameraPanel(Scene &scene, Camera &cam) {
  ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
  ImGui::Begin("Camera");

  // Mode selector.
  int modeInt = static_cast<int>(cam.Mode());
  const char *modeNames[] = {"Free Fly", "Orbit", "Game (Top-Down)"};
  if (ImGui::Combo("Mode", &modeInt, modeNames, 3)) {
    cam.SetMode(static_cast<CameraMode>(modeInt));
  }

  ImGui::Separator();

  // Position display (read-only in orbit/game, editable in free-fly).
  DirectX::XMFLOAT3 pos = cam.GetPosition();
  if (cam.Mode() == CameraMode::FreeFly) {
    if (ImGui::DragFloat3("Position", &pos.x, 0.1f)) {
      cam.SetPosition(pos.x, pos.y, pos.z);
    }
    float yaw = DirectX::XMConvertToDegrees(cam.Yaw());
    float pitch = DirectX::XMConvertToDegrees(cam.Pitch());
    if (ImGui::DragFloat("Yaw", &yaw, 0.5f, -180.0f, 180.0f)) {
      cam.SetYawPitch(DirectX::XMConvertToRadians(yaw), cam.Pitch());
    }
    if (ImGui::DragFloat("Pitch", &pitch, 0.5f, -89.0f, 89.0f)) {
      cam.SetYawPitch(cam.Yaw(), DirectX::XMConvertToRadians(pitch));
    }
  } else {
    ImGui::Text("Position: %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);
  }

  // Orbit-specific controls.
  if (cam.Mode() == CameraMode::Orbit) {
    DirectX::XMFLOAT3 target = cam.OrbitTarget();
    if (ImGui::DragFloat3("Orbit Target", &target.x, 0.1f)) {
      cam.SetOrbitTarget(target.x, target.y, target.z);
    }
    float dist = cam.OrbitDistance();
    if (ImGui::DragFloat("Distance", &dist, 0.1f, 0.5f, 500.0f)) {
      cam.SetOrbitDistance(dist);
    }
  }

  ImGui::Separator();

  // Lens settings.
  float fovDeg = DirectX::XMConvertToDegrees(cam.FovY());
  float nearZ = cam.NearZ();
  float farZ = cam.FarZ();
  bool lensChanged = false;
  lensChanged |= ImGui::SliderFloat("FOV (deg)", &fovDeg, 10.0f, 120.0f);
  lensChanged |= ImGui::DragFloat("Near Plane", &nearZ, 0.01f, 0.001f, 10.0f, "%.3f");
  lensChanged |= ImGui::DragFloat("Far Plane", &farZ, 1.0f, 10.0f, 10000.0f, "%.0f");
  if (lensChanged) {
    cam.SetLens(DirectX::XMConvertToRadians(fovDeg), cam.Aspect(), nearZ, farZ);
  }

  ImGui::Separator();

  // Speed controls.
  float moveSpeed = cam.MoveSpeed();
  float lookSpeed = cam.LookSpeed() * 1000.0f; // display in mrad for readability
  if (ImGui::DragFloat("Move Speed", &moveSpeed, 0.1f, 0.1f, 100.0f))
    cam.SetMoveSpeed(moveSpeed);
  if (ImGui::DragFloat("Look Speed", &lookSpeed, 0.1f, 0.1f, 20.0f))
    cam.SetLookSpeed(lookSpeed * 0.001f);

  ImGui::Separator();

  // ---- Camera presets ----
  ImGui::Text("Presets");
  auto &presets = scene.CameraPresets();

  ImGui::InputText("Name", m_presetName, sizeof(m_presetName));
  ImGui::SameLine();
  if (ImGui::Button("Save")) {
    presets.push_back(cam.MakePreset(m_presetName));
  }

  // List saved presets.
  int deleteIdx = -1;
  for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
    ImGui::PushID(i);
    if (ImGui::Button("Load")) {
      cam.ApplyPreset(presets[i]);
    }
    ImGui::SameLine();
    if (ImGui::Button("X")) {
      deleteIdx = i;
    }
    ImGui::SameLine();
    ImGui::Text("%s", presets[i].name.c_str());
    ImGui::PopID();
  }
  if (deleteIdx >= 0) {
    presets.erase(presets.begin() + deleteIdx);
  }

  ImGui::End();
}

// ---- Asset Browser (Phase 7) ----

static AssetType ClassifyAsset(const std::string &ext) {
  if (ext == ".gltf" || ext == ".glb")
    return AssetType::Mesh;
  if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
      ext == ".tga")
    return AssetType::Texture;
  if (ext == ".json")
    return AssetType::Scene;
  return AssetType::Unknown;
}

void SceneEditor::ScanAssetDirectory() {
  m_assetCache.clear();
  namespace fs = std::filesystem;
  const fs::path root("Assets");
  std::error_code error;
  if (!fs::exists(root, error) || !fs::is_directory(root, error)) {
    m_assetCacheValid = true;
    m_selectedAssetIndex = -1;
    return;
  }

  fs::recursive_directory_iterator iterator(
      root, fs::directory_options::skip_permission_denied, error);
  const fs::recursive_directory_iterator end;
  while (iterator != end) {
    if (error) {
      error.clear();
      iterator.increment(error);
      continue;
    }

    const fs::directory_entry entry = *iterator;
    iterator.increment(error);
    if (!entry.is_regular_file(error)) {
      error.clear();
      continue;
    }
    std::string ext = entry.path().extension().string();
    // lowercase extension
    for (auto &c : ext)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    AssetType type = ClassifyAsset(ext);
    if (type == AssetType::Unknown)
      continue;

    AssetEntry ae;
    ae.path = entry.path().generic_string(); // forward slashes
    ae.displayName = entry.path().filename().string();
    ae.type = type;
    m_assetCache.push_back(std::move(ae));
  }

  // Sort by path for consistent display.
  std::sort(m_assetCache.begin(), m_assetCache.end(),
            [](const AssetEntry &a, const AssetEntry &b) {
              return a.path < b.path;
            });

  m_assetCacheValid = true;
  m_selectedAssetIndex = -1;
}

void SceneEditor::DrawAssetBrowser(Scene &scene, DxContext &dx) {
  const EditorWorkspaceLayout layout = BuildEditorWorkspaceLayout();
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float maximumConsoleWidth =
      std::max(140.0f, layout.viewport.width - 140.0f);
  const float minimumConsoleWidth =
      std::min(280.0f, maximumConsoleWidth);
  const float consoleWidth =
      std::clamp(layout.viewport.width * 0.38f, minimumConsoleWidth,
                 std::min(500.0f, maximumConsoleWidth));
  ImGui::SetNextWindowPos(
      ImVec2(layout.leftWidth, display.y - layout.bottomHeight),
      ImGuiCond_Always);
  ImGui::SetNextWindowSize(
      ImVec2(std::max(1.0f, layout.viewport.width - consoleWidth),
             layout.bottomHeight),
      ImGuiCond_Always);
  ImGui::Begin("Project", nullptr, kFixedPanelFlags);

  // Scan / Refresh.
  if (!m_assetCacheValid || ImGui::Button("Refresh")) {
    ScanAssetDirectory();
  }

  if (m_assetCache.empty()) {
    ImGui::TextDisabled("No assets found in Assets/");
    ImGui::End();
    return;
  }

  // Filter combo.
  const char *filterLabels[] = {"All", "Meshes", "Textures", "Scenes"};
  ImGui::Combo("Filter", &m_assetFilterType, filterLabels,
               IM_ARRAYSIZE(filterLabels));

  ImGui::Separator();

  // Build directory tree structure. We use a flat list grouped by parent dir.
  // Track which directories are open via TreeNode.
  const ImVec2 browserSpace = ImGui::GetContentRegionAvail();
  const float treeWidth = std::clamp(
      browserSpace.x * 0.58f, 180.0f,
      std::max(180.0f, browserSpace.x - 180.0f));
  ImGui::BeginChild("AssetTree", ImVec2(treeWidth, 0.0f), true);

  std::string lastDir;
  bool dirOpen = false;

  for (int i = 0; i < static_cast<int>(m_assetCache.size()); ++i) {
    const auto &asset = m_assetCache[i];

    // Apply filter.
    if (m_assetFilterType == 1 && asset.type != AssetType::Mesh)
      continue;
    if (m_assetFilterType == 2 && asset.type != AssetType::Texture)
      continue;
    if (m_assetFilterType == 3 && asset.type != AssetType::Scene)
      continue;

    // Extract parent directory.
    std::string dir;
    auto slashPos = asset.path.rfind('/');
    if (slashPos != std::string::npos)
      dir = asset.path.substr(0, slashPos);

    // If directory changed, close old tree node and open new one.
    if (dir != lastDir) {
      if (!lastDir.empty() && dirOpen) {
        ImGui::TreePop();
        dirOpen = false;
      }
      if (!dir.empty()) {
        dirOpen = ImGui::TreeNode(dir.c_str());
      } else {
        dirOpen = true; // root-level files always shown
      }
      lastDir = dir;
    }

    if (!dirOpen)
      continue;

    // Type prefix.
    const char *prefix = "[?]";
    switch (asset.type) {
    case AssetType::Mesh:    prefix = "[M]"; break;
    case AssetType::Texture: prefix = "[T]"; break;
    case AssetType::Scene:   prefix = "[S]"; break;
    default: break;
    }

    char label[512];
    snprintf(label, sizeof(label), "%s %s", prefix, asset.displayName.c_str());

    bool selected = (m_selectedAssetIndex == i);
    if (ImGui::Selectable(label, selected)) {
      m_selectedAssetIndex = i;
    }

    // Double-click scene files to load.
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) &&
        asset.type == AssetType::Scene) {
      if (scene.LoadFromFile(asset.path, dx)) {
        m_history.Clear();
        m_selectedEntity = kInvalidEntityId;
        m_previewTexturePath.clear();
      }
    }

    // Drag-drop source.
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
      const char *payloadType = "ASSET_PATH";
      ImGui::SetDragDropPayload(payloadType, asset.path.c_str(),
                                asset.path.size() + 1);
      ImGui::Text("%s %s", prefix, asset.displayName.c_str());
      ImGui::EndDragDropSource();
    }
  }

  // Close last dir tree node if open.
  if (!lastDir.empty() && dirOpen)
    ImGui::TreePop();

  ImGui::EndChild();

  // ---- Detail / Preview area ----
  ImGui::SameLine();
  ImGui::BeginChild("AssetDetails", ImVec2(0.0f, 0.0f), true);

  if (m_selectedAssetIndex >= 0 &&
      m_selectedAssetIndex < static_cast<int>(m_assetCache.size())) {
    const auto &sel = m_assetCache[m_selectedAssetIndex];

    ImGui::Text("File: %s", sel.displayName.c_str());
    ImGui::TextWrapped("Path: %s", sel.path.c_str());

    // File size.
    namespace fs = std::filesystem;
    if (fs::exists(sel.path)) {
      auto sz = fs::file_size(sel.path);
      if (sz < 1024)
        ImGui::Text("Size: %llu B", static_cast<unsigned long long>(sz));
      else if (sz < 1024 * 1024)
        ImGui::Text("Size: %.1f KB", sz / 1024.0);
      else
        ImGui::Text("Size: %.1f MB", sz / (1024.0 * 1024.0));
    }

    // Texture preview.
    if (sel.type == AssetType::Texture) {
      // Only reload when selection changes.
      // Request upload (deferred to next BeginFrame).
      if (m_previewTexturePath != sel.path) {
        LoadedImage img;
        if (LoadImageFile(sel.path, img)) {
          dx.RequestPreviewTexture(img);
          m_previewTexturePath = sel.path;
        }
      }
      // Show preview if available (uploaded in previous frame's BeginFrame).
      if (m_previewTexturePath == sel.path && dx.HasPreviewTexture()) {
        ImGui::Image(static_cast<ImTextureID>(dx.PreviewTextureGpu().ptr),
                     ImVec2(64, 64));
      }
    }

    // Assign buttons.
    Entity *selEnt = scene.FindEntity(m_selectedEntity);
    if (selEnt && selEnt->mesh.has_value()) {
      if (sel.type == AssetType::Mesh && ImGui::Button("Load as Mesh")) {
        auto &mc = selEnt->mesh.value();
        Material oldMat = mc.material;
        auto oldPaths = mc.texturePaths;
        mc.sourceType = MeshSourceType::GltfFile;
        mc.gltfPath = sel.path;
        mc.meshId = UINT32_MAX;
        m_history.Execute(std::make_unique<MaterialCommand>(
            scene, dx, selEnt->id, oldMat, mc.material, oldPaths,
            mc.texturePaths));
      }

      if (sel.type == AssetType::Texture) {
        if (ImGui::BeginCombo("Assign to Slot", "Select...")) {
          for (int s = 0; s < 6; ++s) {
            if (ImGui::Selectable(kTexSlotNames[s])) {
              auto &mc = selEnt->mesh.value();
              Material oldMat = mc.material;
              auto oldPaths = mc.texturePaths;
              mc.texturePaths[s] = sel.path;
              m_history.Execute(std::make_unique<MaterialCommand>(
                  scene, dx, selEnt->id, oldMat, mc.material, oldPaths,
                  mc.texturePaths));
            }
          }
          ImGui::EndCombo();
        }
      }
    }
  } else {
    ImGui::TextDisabled("Select an asset above.");
  }

  ImGui::EndChild();
  ImGui::End();
}

// ---- Console and diagnostics ----

void SceneEditor::DrawConsolePanel(const Scene &scene) {
  const EditorWorkspaceLayout layout = BuildEditorWorkspaceLayout();
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float maximumConsoleWidth =
      std::max(140.0f, layout.viewport.width - 140.0f);
  const float minimumConsoleWidth =
      std::min(280.0f, maximumConsoleWidth);
  const float consoleWidth =
      std::clamp(layout.viewport.width * 0.38f, minimumConsoleWidth,
                 std::min(500.0f, maximumConsoleWidth));
  ImGui::SetNextWindowPos(
      ImVec2(layout.leftWidth + layout.viewport.width - consoleWidth,
             display.y - layout.bottomHeight),
      ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(consoleWidth, layout.bottomHeight),
                           ImGuiCond_Always);
  ImGui::Begin("Console", nullptr, kFixedPanelFlags);

  size_t meshCount = 0;
  size_t pointLightCount = 0;
  size_t spotLightCount = 0;
  for (const Entity &entity : scene.Entities()) {
    meshCount += entity.mesh.has_value() ? 1u : 0u;
    pointLightCount += entity.pointLight.has_value() ? 1u : 0u;
    spotLightCount += entity.spotLight.has_value() ? 1u : 0u;
  }

  if (ImGui::BeginTabBar("##ConsoleTabs")) {
    if (ImGui::BeginTabItem("Console")) {
      ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.0f),
                         "[Ready] VILLIEN Editor is using the live DX12 engine.");
      ImGui::TextWrapped(
          "The central Scene View is the actual renderer, not a feature mockup.");
      ImGui::Separator();
      if (m_selectedEntity == kInvalidEntityId) {
        ImGui::TextDisabled("No object selected.");
      } else {
        const Entity *selected = scene.FindEntity(m_selectedEntity);
        ImGui::Text("Selected: %s", selected ? selected->name.c_str()
                                             : "<deleted>");
      }
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Stats")) {
      ImGui::Text("Objects: %zu", scene.Entities().size());
      ImGui::Text("Meshes: %zu", meshCount);
      ImGui::Text("Point lights: %zu", pointLightCount);
      ImGui::Text("Spot lights: %zu", spotLightCount);
      ImGui::Text("Indexed assets: %zu", m_assetCache.size());
      ImGui::Text("Scene View: %.0f x %.0f", m_viewportRect.width,
                  m_viewportRect.height);
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Shortcuts")) {
      ImGui::Text("W / E / R   Move / Rotate / Scale");
      ImGui::Text("Ctrl+Z / Y  Undo / Redo");
      ImGui::Text("Ctrl+D      Duplicate object");
      ImGui::Text("Delete      Delete object");
      ImGui::Text("F5          Play / Stop scene");
      ImGui::Text("F6          Grid editor");
      ImGui::Text("F9          Reload shaders");
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  ImGui::End();
}

void SceneEditor::DrawGridEditorPanel(StageData &stage) {
  ImGui::SetNextWindowSize(ImVec2(340, 700), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowCollapsed(false, ImGuiCond_FirstUseEver);
  ImGui::Begin("Grid Editor");

  // ---- Play Test button (Phase 5C) ----
  if (ImGui::Button("Play Test (F5)")) {
    m_playTestRequested = true;
  }
  ImGui::Separator();

  // ---- A. File I/O toolbar ----
  if (ImGui::CollapsingHeader("File", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::InputText("Path", m_stagePath, sizeof(m_stagePath));

    if (ImGui::Button("New Stage")) {
      stage.Clear();
      m_gridHistory.Clear();
      m_selectedTileX = m_selectedTileY = -1;
      m_selectedTower = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
      if (m_stagePath[0] != '\0')
        StageSerializer::SaveToFile(stage, m_stagePath);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save As...")) {
      char path[256] = {};
      if (OpenStageFileDialog(path, sizeof(path), true)) {
        if (StageSerializer::SaveToFile(stage, path))
          std::snprintf(m_stagePath, sizeof(m_stagePath), "%s", path);
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load...")) {
      char path[256] = {};
      if (OpenStageFileDialog(path, sizeof(path), false)) {
        StageData loaded;
        if (StageSerializer::LoadFromFile(path, loaded)) {
          stage = loaded;
          std::snprintf(m_stagePath, sizeof(m_stagePath), "%s", path);
          m_gridHistory.Clear();
          m_selectedTileX = m_selectedTileY = -1;
          m_selectedTower = -1;
        }
      }
    }
  }

  // ---- B. Stage metadata ----
  if (ImGui::CollapsingHeader("Stage Settings",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    // Metadata drag coalescing helpers.
    auto MetaDragStart = [&]() {
      if (ImGui::IsItemActivated() && !m_metaDragActive) {
        m_metaDragActive = true;
        m_metaDragStart = ExtractMetadata(stage);
      }
    };
    auto MetaDragEnd = [&]() {
      if (ImGui::IsItemDeactivatedAfterEdit() && m_metaDragActive) {
        auto cmd = std::make_unique<StageMetadataCommand>(
            stage, m_metaDragStart, ExtractMetadata(stage));
        m_gridHistory.PushWithoutExecute(std::move(cmd));
        m_metaDragActive = false;
      }
    };

    // Name.
    char nameBuf[128] = {};
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", stage.name.c_str());
    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
      auto before = ExtractMetadata(stage);
      stage.name = nameBuf;
      auto cmd = std::make_unique<StageMetadataCommand>(
          stage, before, ExtractMetadata(stage));
      m_gridHistory.PushWithoutExecute(std::move(cmd));
    }

    // Grid dimensions (resize) — drag coalesced.
    int dims[2] = {stage.width, stage.height};
    if (ImGui::DragInt2("Grid Size", dims, 1.0f, 2, 100)) {
      dims[0] = std::clamp(dims[0], 2, 100);
      dims[1] = std::clamp(dims[1], 2, 100);
      if (dims[0] != stage.width || dims[1] != stage.height) {
        stage.Resize(dims[0], dims[1]);
        // Clamp selection.
        if (m_selectedTileX >= stage.width)
          m_selectedTileX = -1;
        if (m_selectedTileY >= stage.height)
          m_selectedTileY = -1;
      }
    }
    if (ImGui::IsItemActivated()) {
      m_resizeDragActive = true;
      m_resizeDragStart = stage;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_resizeDragActive) {
      auto cmd =
          std::make_unique<ResizeGridCommand>(stage, m_resizeDragStart, stage);
      m_gridHistory.PushWithoutExecute(std::move(cmd));
      m_resizeDragActive = false;
    }

    ImGui::DragFloat("Time Limit", &stage.timeLimit, 1.0f, 0.0f, 600.0f,
                     "%.0f s");
    MetaDragStart();
    MetaDragEnd();

    ImGui::DragInt("Par Moves", &stage.parMoves, 1.0f, 0, 999);
    MetaDragStart();
    MetaDragEnd();

    ImGui::Separator();
    ImGui::Text("Spawn Positions");

    ImGui::DragInt("Player X", &stage.playerSpawnX, 1.0f, 0,
                   stage.width - 1);
    MetaDragStart();
    MetaDragEnd();
    ImGui::DragInt("Player Y", &stage.playerSpawnY, 1.0f, 0,
                   stage.height - 1);
    MetaDragStart();
    MetaDragEnd();
    ImGui::DragInt("Cargo X", &stage.cargoSpawnX, 1.0f, 0,
                   stage.width - 1);
    MetaDragStart();
    MetaDragEnd();
    ImGui::DragInt("Cargo Y", &stage.cargoSpawnY, 1.0f, 0,
                   stage.height - 1);
    MetaDragStart();
    MetaDragEnd();
  }

  // ---- C. Brush palette ----
  if (ImGui::CollapsingHeader("Brush", ImGuiTreeNodeFlags_DefaultOpen)) {
    for (int i = 0; i < kTileTypeCount; ++i) {
      // Color indicator.
      ImVec4 c;
      ImU32 col = TileColor(static_cast<TileType>(i), false);
      c.x = ((col >> 0) & 0xFF) / 255.0f;
      c.y = ((col >> 8) & 0xFF) / 255.0f;
      c.z = ((col >> 16) & 0xFF) / 255.0f;
      c.w = 1.0f;
      ImGui::ColorButton(("##bc" + std::to_string(i)).c_str(), c,
                          ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
      ImGui::SameLine();
      ImGui::RadioButton(kTileTypeNames[i], &m_brushTileType, i);
    }

    ImGui::Separator();
    ImGui::Checkbox("Place Wall On Tile", &m_brushWall);
    if (m_brushWall)
      ImGui::Checkbox("  Destructible", &m_brushDestructible);
  }

  // ---- D. Mini-grid view ----
  if (ImGui::CollapsingHeader("Grid View", ImGuiTreeNodeFlags_DefaultOpen)) {
    stage.EnsureSize();

    const float avail = ImGui::GetContentRegionAvail().x;
    const int maxDim = std::max(stage.width, stage.height);
    const float cellSize =
        std::max(4.0f, std::min(avail / maxDim, 24.0f));
    const float gridW = cellSize * stage.width;
    const float gridH = cellSize * stage.height;

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##grid", ImVec2(gridW, gridH));

    ImDrawList *dl = ImGui::GetWindowDrawList();

    // Draw tiles.
    for (int y = 0; y < stage.height; ++y) {
      for (int x = 0; x < stage.width; ++x) {
        const auto &tile = stage.At(x, y);
        ImVec2 pMin = {canvasPos.x + x * cellSize,
                       canvasPos.y + y * cellSize};
        ImVec2 pMax = {pMin.x + cellSize - 1.0f, pMin.y + cellSize - 1.0f};
        dl->AddRectFilled(pMin, pMax, TileColor(tile.type, tile.hasWall));
      }
    }

    // Player spawn marker.
    if (stage.InBounds(stage.playerSpawnX, stage.playerSpawnY)) {
      ImVec2 p = {canvasPos.x + stage.playerSpawnX * cellSize + cellSize * 0.5f,
                  canvasPos.y + stage.playerSpawnY * cellSize + cellSize * 0.5f};
      dl->AddText(ImVec2(p.x - 3.0f, p.y - 5.0f), IM_COL32(0, 255, 255, 255),
                  "P");
    }

    // Cargo spawn marker.
    if (stage.InBounds(stage.cargoSpawnX, stage.cargoSpawnY)) {
      ImVec2 p = {canvasPos.x + stage.cargoSpawnX * cellSize + cellSize * 0.5f,
                  canvasPos.y + stage.cargoSpawnY * cellSize + cellSize * 0.5f};
      dl->AddText(ImVec2(p.x - 3.0f, p.y - 5.0f),
                  IM_COL32(255, 200, 0, 255), "C");
    }

    // Tower position markers (red triangles on perimeter).
    for (const auto &tower : stage.towers) {
      float tx = 0.0f, ty = 0.0f;
      switch (tower.side) {
      case TowerSide::Left:
        tx = canvasPos.x - 6.0f;
        ty = canvasPos.y + tower.y * cellSize + cellSize * 0.5f;
        break;
      case TowerSide::Right:
        tx = canvasPos.x + gridW + 2.0f;
        ty = canvasPos.y + tower.y * cellSize + cellSize * 0.5f;
        break;
      case TowerSide::Top:
        tx = canvasPos.x + tower.x * cellSize + cellSize * 0.5f;
        ty = canvasPos.y - 6.0f;
        break;
      case TowerSide::Bottom:
        tx = canvasPos.x + tower.x * cellSize + cellSize * 0.5f;
        ty = canvasPos.y + gridH + 2.0f;
        break;
      }
      dl->AddTriangleFilled(ImVec2(tx, ty - 4), ImVec2(tx - 4, ty + 4),
                            ImVec2(tx + 4, ty + 4),
                            IM_COL32(255, 50, 50, 255));
    }

    // Selected tile highlight.
    if (m_selectedTileX >= 0 && m_selectedTileX < stage.width &&
        m_selectedTileY >= 0 && m_selectedTileY < stage.height) {
      ImVec2 pMin = {canvasPos.x + m_selectedTileX * cellSize,
                     canvasPos.y + m_selectedTileY * cellSize};
      ImVec2 pMax = {pMin.x + cellSize - 1.0f, pMin.y + cellSize - 1.0f};
      dl->AddRect(pMin, pMax, IM_COL32(255, 255, 255, 255), 0.0f, 0,
                  2.0f);
    }

    // ---- Mini-grid mouse interaction (click to paint) ----
    if (ImGui::IsItemActive()) {
      ImVec2 mouse = ImGui::GetMousePos();
      int mx = static_cast<int>((mouse.x - canvasPos.x) / cellSize);
      int my = static_cast<int>((mouse.y - canvasPos.y) / cellSize);
      if (stage.InBounds(mx, my)) {
        m_selectedTileX = mx;
        m_selectedTileY = my;

        // Build brush tile.
        TileData brushTile;
        brushTile.type = static_cast<TileType>(m_brushTileType);
        brushTile.hasWall = m_brushWall;
        brushTile.wallDestructible = m_brushDestructible;

        const TileData &current = stage.At(mx, my);

        // Check if we need to paint (tile differs from brush).
        bool differs = current.type != brushTile.type ||
                       current.hasWall != brushTile.hasWall ||
                       current.wallDestructible != brushTile.wallDestructible;

        if (differs) {
          // Check if this tile is already in the current stroke.
          bool alreadyInStroke = false;
          for (const auto &e : m_paintStroke) {
            if (e.x == mx && e.y == my) {
              alreadyInStroke = true;
              break;
            }
          }
          if (!alreadyInStroke) {
            PaintTilesCommand::Entry entry;
            entry.x = mx;
            entry.y = my;
            entry.before = current;
            entry.after = brushTile;
            m_paintStroke.push_back(entry);

            // Apply immediately for visual feedback.
            stage.At(mx, my) = brushTile;
          }
        }
        m_miniGridPainting = true;
      }
    }

    // Finalize stroke on mouse release.
    if (m_miniGridPainting && !ImGui::IsItemActive()) {
      if (!m_paintStroke.empty()) {
        if (m_paintStroke.size() == 1) {
          auto &e = m_paintStroke[0];
          auto cmd = std::make_unique<PaintTileCommand>(stage, e.x, e.y,
                                                        e.before, e.after);
          m_gridHistory.PushWithoutExecute(std::move(cmd));
        } else {
          auto cmd = std::make_unique<PaintTilesCommand>(
              stage, std::move(m_paintStroke));
          m_gridHistory.PushWithoutExecute(std::move(cmd));
        }
        m_paintStroke.clear();
      }
      m_miniGridPainting = false;
    }

    // Info text.
    if (m_selectedTileX >= 0 && m_selectedTileY >= 0 &&
        stage.InBounds(m_selectedTileX, m_selectedTileY)) {
      const auto &sel = stage.At(m_selectedTileX, m_selectedTileY);
      ImGui::Text("Selected: (%d, %d) %s%s", m_selectedTileX,
                  m_selectedTileY,
                  kTileTypeNames[static_cast<int>(sel.type)],
                  sel.hasWall ? " [Wall]" : "");
    }

    ImGui::Text("Grid: %d x %d (%d tiles)", stage.width, stage.height,
                stage.width * stage.height);
  }

  // ---- E. Tower list ----
  if (ImGui::CollapsingHeader("Towers")) {
    // Tower drag coalescing helpers.
    auto TowerDragStart = [&]() {
      if (ImGui::IsItemActivated() && !m_towerDragActive) {
        m_towerDragActive = true;
        m_towerDragStart = stage.towers;
      }
    };
    auto TowerDragEnd = [&]() {
      if (ImGui::IsItemDeactivatedAfterEdit() && m_towerDragActive) {
        auto cmd = std::make_unique<TowerCommand>(stage, m_towerDragStart,
                                                  stage.towers);
        m_gridHistory.PushWithoutExecute(std::move(cmd));
        m_towerDragActive = false;
      }
    };

    if (ImGui::Button("Add Tower")) {
      auto before = stage.towers;
      TowerData newTower;
      newTower.side = TowerSide::Left;
      newTower.y = 0;
      stage.towers.push_back(newTower);
      auto cmd =
          std::make_unique<TowerCommand>(stage, before, stage.towers);
      m_gridHistory.PushWithoutExecute(std::move(cmd));
      m_selectedTower = static_cast<int>(stage.towers.size()) - 1;
    }

    // Tower list.
    for (int i = 0; i < static_cast<int>(stage.towers.size()); ++i) {
      ImGui::PushID(i);
      bool selected = (m_selectedTower == i);
      char label[64];
      std::snprintf(label, sizeof(label), "Tower %d (%s)", i,
                    kTowerSideNames[static_cast<int>(stage.towers[i].side)]);
      if (ImGui::Selectable(label, selected))
        m_selectedTower = i;
      ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
      if (ImGui::SmallButton("X")) {
        auto before = stage.towers;
        stage.towers.erase(stage.towers.begin() + i);
        auto cmd =
            std::make_unique<TowerCommand>(stage, before, stage.towers);
        m_gridHistory.PushWithoutExecute(std::move(cmd));
        if (m_selectedTower >= static_cast<int>(stage.towers.size()))
          m_selectedTower = static_cast<int>(stage.towers.size()) - 1;
        ImGui::PopID();
        break; // list changed, bail
      }
      ImGui::PopID();
    }

    // Tower inspector.
    if (m_selectedTower >= 0 &&
        m_selectedTower < static_cast<int>(stage.towers.size())) {
      ImGui::Separator();
      auto &t = stage.towers[m_selectedTower];

      int sideIdx = static_cast<int>(t.side);
      if (ImGui::Combo("Side", &sideIdx, kTowerSideNames, 4)) {
        auto before = stage.towers;
        t.side = static_cast<TowerSide>(sideIdx);
        auto cmd =
            std::make_unique<TowerCommand>(stage, before, stage.towers);
        m_gridHistory.PushWithoutExecute(std::move(cmd));
      }

      // Position along the side.
      int maxPos = (t.side == TowerSide::Left || t.side == TowerSide::Right)
                       ? stage.height - 1
                       : stage.width - 1;
      int &posRef = (t.side == TowerSide::Left || t.side == TowerSide::Right)
                        ? t.y
                        : t.x;
      ImGui::DragInt("Position", &posRef, 1.0f, 0, maxPos);
      TowerDragStart();
      TowerDragEnd();

      int patIdx = static_cast<int>(t.pattern);
      if (ImGui::Combo("Pattern", &patIdx, kTowerPatternNames, 4)) {
        auto before = stage.towers;
        t.pattern = static_cast<TowerPattern>(patIdx);
        auto cmd =
            std::make_unique<TowerCommand>(stage, before, stage.towers);
        m_gridHistory.PushWithoutExecute(std::move(cmd));
      }

      ImGui::DragFloat("Delay", &t.delay, 0.1f, 0.0f, 10.0f, "%.1f s");
      TowerDragStart();
      TowerDragEnd();

      ImGui::DragFloat("Interval", &t.interval, 0.1f, 0.1f, 10.0f,
                       "%.1f s");
      TowerDragStart();
      TowerDragEnd();
    }
  }

  // ---- F. Undo/Redo controls ----
  ImGui::Separator();
  {
    bool canUndo = m_gridHistory.CanUndo();
    bool canRedo = m_gridHistory.CanRedo();

    if (!canUndo)
      ImGui::BeginDisabled();
    if (ImGui::Button("Undo"))
      m_gridHistory.Undo();
    if (!canUndo)
      ImGui::EndDisabled();

    ImGui::SameLine();

    if (!canRedo)
      ImGui::BeginDisabled();
    if (ImGui::Button("Redo"))
      m_gridHistory.Redo();
    if (!canRedo)
      ImGui::EndDisabled();

    if (canUndo) {
      ImGui::SameLine();
      ImGui::TextDisabled("(%s)", m_gridHistory.UndoName());
    }
  }

  ImGui::End();
}

// ========================================================================
// Phase 5B — Editor viewport rendering, tile picking, viewport painting
// ========================================================================

void SceneEditor::InitEditorMeshes(DxContext &dx) {
  if (m_editorMeshesInitialized)
    return;

  auto plane = ProceduralMesh::CreatePlane(1.0f, 1.0f);
  auto cube = ProceduralMesh::CreateCube(1.0f);
  auto cone = ProceduralMesh::CreateCone(0.4f, 1.2f, 12);

  m_editorMeshIds.floor = dx.CreateMeshResources(plane, {}, MakeFloorMaterial());
  m_editorMeshIds.wall = dx.CreateMeshResources(cube, {}, MakeWallMaterial());
  m_editorMeshIds.fire = dx.CreateMeshResources(plane, {}, MakeFireMaterial());
  m_editorMeshIds.lightning =
      dx.CreateMeshResources(plane, {}, MakeLightningMaterial());
  m_editorMeshIds.spike = dx.CreateMeshResources(plane, {}, MakeSpikeMaterial());
  m_editorMeshIds.ice = dx.CreateMeshResources(plane, {}, MakeIceMaterial());
  m_editorMeshIds.crumble =
      dx.CreateMeshResources(plane, {}, MakeCrumbleMaterial());
  m_editorMeshIds.start = dx.CreateMeshResources(plane, {}, MakeStartMaterial());
  m_editorMeshIds.goal = dx.CreateMeshResources(plane, {}, MakeGoalMaterial());
  m_editorMeshIds.playerSpawn =
      dx.CreateMeshResources(cube, {}, MakePlayerMaterial());
  m_editorMeshIds.cargoSpawn =
      dx.CreateMeshResources(cube, {}, MakeCargoMaterial());
  m_editorMeshIds.tower = dx.CreateMeshResources(cone, {}, MakeTowerMaterial());
  m_editorMeshIds.highlight =
      dx.CreateMeshResources(cube, {}, MakeHighlightMaterial());
  m_editorMeshIds.telegraph =
      dx.CreateMeshResources(plane, {}, MakeTelegraphMaterial());

  m_editorMeshesInitialized = true;
}

void SceneEditor::BuildStageViewportItems(const StageData &stage,
                                          FrameData &frame) const {
  if (!m_gridEditorOpen || !m_editorMeshesInitialized)
    return;

  // Compute attack preview for selected tower (Phase 5C).
  std::vector<std::pair<int, int>> attackTiles;
  const std::vector<std::pair<int, int>> *attackPtr = nullptr;
  if (m_selectedTower >= 0 &&
      m_selectedTower < static_cast<int>(stage.towers.size())) {
    attackTiles = ComputeAttackTiles(stage.towers[m_selectedTower],
                                     stage.width, stage.height);
    attackPtr = &attackTiles;
  }

  BuildStageRenderItems(stage, m_editorMeshIds, frame.opaqueItems,
                        m_selectedTileX, m_selectedTileY, attackPtr);
}

// ---- Ray-plane intersection for tile picking ----

bool SceneEditor::ViewportRayToTile(int screenX, int screenY, int screenW,
                                    int screenH,
                                    const DirectX::XMMATRIX &view,
                                    const DirectX::XMMATRIX &proj,
                                    const StageData &stage, int &outX,
                                    int &outY) const {
  using namespace DirectX;

  // Screen -> NDC.
  float ndcX = 2.0f * screenX / screenW - 1.0f;
  float ndcY = -(2.0f * screenY / screenH - 1.0f);

  // NDC -> World ray via inverse ViewProj.
  XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);
  XMVECTOR nearPt =
      XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invViewProj);
  XMVECTOR farPt =
      XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invViewProj);
  XMVECTOR rayDir = XMVector3Normalize(XMVectorSubtract(farPt, nearPt));
  XMVECTOR rayOrigin = nearPt;

  // Ray-plane intersection at y=0.
  float originY = XMVectorGetY(rayOrigin);
  float dirY = XMVectorGetY(rayDir);

  if (std::fabsf(dirY) < 1e-6f)
    return false; // Ray parallel to plane.

  float t = -originY / dirY;
  if (t < 0.0f)
    return false; // Plane behind camera.

  XMVECTOR hitPt = XMVectorAdd(rayOrigin, XMVectorScale(rayDir, t));
  float hitX = XMVectorGetX(hitPt);
  float hitZ = XMVectorGetZ(hitPt);

  // Tile at grid coord (gx, gy) is centered at world (gx, 0, gy).
  int gx = static_cast<int>(std::floorf(hitX + 0.5f));
  int gy = static_cast<int>(std::floorf(hitZ + 0.5f));

  if (!stage.InBounds(gx, gy))
    return false;

  outX = gx;
  outY = gy;
  return true;
}

void SceneEditor::HandleViewportTilePaint(StageData &stage, int screenX,
                                          int screenY, int screenW,
                                          int screenH,
                                          const DirectX::XMMATRIX &view,
                                          const DirectX::XMMATRIX &proj) {
  int gx, gy;
  if (!ViewportRayToTile(screenX, screenY, screenW, screenH, view, proj, stage,
                         gx, gy))
    return;

  // Update selection to the tile under cursor.
  m_selectedTileX = gx;
  m_selectedTileY = gy;

  // Build brush tile from current brush state.
  TileData brushTile;
  brushTile.type = static_cast<TileType>(m_brushTileType);
  brushTile.hasWall = m_brushWall;
  brushTile.wallDestructible = m_brushDestructible;

  const TileData &current = stage.At(gx, gy);
  bool differs = (current.type != brushTile.type ||
                  current.hasWall != brushTile.hasWall ||
                  current.wallDestructible != brushTile.wallDestructible);

  if (!differs)
    return;

  // Check if already painted this tile in the current stroke.
  for (const auto &e : m_viewportPaintStroke) {
    if (e.x == gx && e.y == gy)
      return;
  }

  PaintTilesCommand::Entry entry;
  entry.x = gx;
  entry.y = gy;
  entry.before = current;
  entry.after = brushTile;
  m_viewportPaintStroke.push_back(entry);

  // Apply immediately for visual feedback.
  stage.At(gx, gy) = brushTile;
  m_viewportPainting = true;
}

void SceneEditor::FinalizeViewportPaintStroke(StageData &stage) {
  if (!m_viewportPainting || m_viewportPaintStroke.empty()) {
    m_viewportPainting = false;
    m_viewportPaintStroke.clear();
    return;
  }

  if (m_viewportPaintStroke.size() == 1) {
    auto &e = m_viewportPaintStroke[0];
    auto cmd =
        std::make_unique<PaintTileCommand>(stage, e.x, e.y, e.before, e.after);
    m_gridHistory.PushWithoutExecute(std::move(cmd));
  } else {
    auto cmd = std::make_unique<PaintTilesCommand>(
        stage, std::move(m_viewportPaintStroke));
    m_gridHistory.PushWithoutExecute(std::move(cmd));
  }

  m_viewportPaintStroke.clear();
  m_viewportPainting = false;
}

// ---- Tower viewport picking (Phase 5C) ----

int SceneEditor::ViewportPickTower(const StageData &stage, int screenX,
                                   int screenY, int screenW, int screenH,
                                   const DirectX::XMMATRIX &view,
                                   const DirectX::XMMATRIX &proj) const {
  using namespace DirectX;

  // Screen -> NDC -> world ray.
  float ndcX = 2.0f * screenX / screenW - 1.0f;
  float ndcY = -(2.0f * screenY / screenH - 1.0f);

  XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);
  XMVECTOR nearPt =
      XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invViewProj);
  XMVECTOR farPt =
      XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invViewProj);
  XMVECTOR rayDir = XMVector3Normalize(XMVectorSubtract(farPt, nearPt));
  XMVECTOR rayOrigin = nearPt;

  // Intersect with y=0.8 plane (tower cone center height).
  float originY = XMVectorGetY(rayOrigin);
  float dirY = XMVectorGetY(rayDir);
  if (std::fabsf(dirY) < 1e-6f)
    return -1;
  float t = (0.8f - originY) / dirY;
  if (t < 0.0f)
    return -1;

  XMVECTOR hitPt = XMVectorAdd(rayOrigin, XMVectorScale(rayDir, t));
  float hitX = XMVectorGetX(hitPt);
  float hitZ = XMVectorGetZ(hitPt);

  // Find closest tower within 0.5 units.
  int bestIdx = -1;
  float bestDist = 0.5f * 0.5f; // squared threshold

  for (int i = 0; i < static_cast<int>(stage.towers.size()); ++i) {
    const auto &tw = stage.towers[i];
    // Tower world position matches BuildStageRenderItems: (tw.x, 0.8, tw.y).
    float twX = static_cast<float>(tw.x);
    float twZ = static_cast<float>(tw.y);

    float dx = hitX - twX;
    float dz = hitZ - twZ;
    float dist2 = dx * dx + dz * dz;
    if (dist2 < bestDist) {
      bestDist = dist2;
      bestIdx = i;
    }
  }

  return bestIdx;
}
