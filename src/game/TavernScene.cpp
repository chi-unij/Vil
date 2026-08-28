#include "game/TavernScene.h"

#include "GltfLoader.h"
#include "MeshRenderer.h"
#include "ProceduralMesh.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <imgui.h>
#include <string_view>

using namespace DirectX;

namespace {

constexpr std::array<XMFLOAT3, 3> kTableInteractions = {
    XMFLOAT3{-3.8f, 0.0f, 2.72f}, XMFLOAT3{0.0f, 0.0f, 2.72f},
    XMFLOAT3{3.8f, 0.0f, 2.72f}};
constexpr XMFLOAT3 kMugRackInteraction = {-2.7f, 0.0f, 6.90f};
constexpr XMFLOAT3 kAleTapInteraction = {0.0f, 0.0f, 6.90f};
constexpr XMFLOAT3 kWashBasinInteraction = {2.7f, 0.0f, 6.90f};
constexpr XMFLOAT3 kKitchenInteraction = {5.35f, 0.0f, 6.90f};
constexpr std::array<XMFLOAT3, 2> kCounterMugInteractions = {
    XMFLOAT3{-1.55f, 0.0f, 6.90f}, XMFLOAT3{-0.85f, 0.0f, 6.90f}};
constexpr std::array<XMFLOAT3, 2> kCounterMugPositions = {
    XMFLOAT3{-1.55f, 1.65f, 7.65f}, XMFLOAT3{-0.85f, 1.65f, 7.65f}};
constexpr XMFLOAT3 kExitInteraction = {0.0f, 0.0f, -1.05f};
constexpr XMFLOAT3 kManagementTableInteraction = {-4.45f, 0.0f, 0.35f};
constexpr XMFLOAT3 kManagementTablePosition = {-5.62f, 0.0f, 0.35f};
constexpr float kStationInteractionRange = 1.55f;
constexpr float kManagementInteractionRange = 1.42f;
constexpr float kCounterMugInteractionRange = 0.58f;
constexpr float kTableInteractionRange = 2.05f;
constexpr float kPerfectPourMinimum = 0.82f;
constexpr float kPerfectPourMaximum = 0.96f;
constexpr int kBaseTableCount = 2;
constexpr int kInitialMugCapacity = 2;
constexpr int kInitialBowlCapacity = 2;
constexpr int kInitialAleStock = 6;
constexpr int kInitialAleCapacity = 6;
constexpr int kAleSupplyUnitPrice = 2;
constexpr int kEmergencyAleFloor = 2;
constexpr int kExtraMugPrice = 25;
constexpr int kOpenTable3Price = 60;
constexpr int kMaximumAleCapacityLevel = 3;
constexpr float kBusinessOpenHour = 5.0f;
constexpr float kEntranceWaitSeconds = 30.0f;
constexpr float kEntranceLeaveSeconds = 1.2f;
constexpr int kEntranceTimeoutPenalty = 20;
constexpr float kCustomerWalkSpeed = 1.6f;
constexpr float kCookSeconds = 6.0f;
constexpr float kMugScaleXZ = 0.345f;
constexpr float kMugScaleY = 0.465f;
constexpr float kAleSurfaceScaleXZ = 0.255f;
constexpr float kImportedMugScale = 1.55f;
constexpr float kImportedMugHalfHeight = 0.239042f * kImportedMugScale * 0.5f;
constexpr const char *kAleName = "水鏡エール";
constexpr const char *kFoodName = "月影シチュー";

LoadedMesh CreateOrderBubbleMesh() {
  // カメラ側（ローカル -Z）を向く角丸の吹き出しと、客を指すしっぽ。
  LoadedMesh mesh;
  const auto vertex = [&](float x, float y) {
    MeshVertex v{};
    v.pos[0] = x;
    v.pos[1] = y;
    v.normal[2] = -1.0f;
    v.tangent[0] = 1.0f;
    v.tangent[3] = 1.0f;
    mesh.vertices.push_back(v);
  };
  vertex(0.0f, 0.0f);
  constexpr float halfWidth = 0.40f;
  constexpr float halfHeight = 0.31f;
  constexpr float radius = 0.12f;
  for (int corner = 0; corner < 4; ++corner) {
    const float x = (corner == 0 || corner == 3 ? 1.0f : -1.0f) *
                    (halfWidth - radius);
    const float y = (corner < 2 ? 1.0f : -1.0f) * (halfHeight - radius);
    for (int step = 0; step <= 6; ++step) {
      const float angle = (corner + step / 6.0f) * XM_PIDIV2;
      vertex(x + radius * std::cos(angle), y + radius * std::sin(angle));
    }
  }
  const uint32_t perimeter = static_cast<uint32_t>(mesh.vertices.size()) - 1;
  for (uint32_t i = 1; i <= perimeter; ++i)
    mesh.indices.insert(mesh.indices.end(), {0, i % perimeter + 1, i});
  const uint32_t tail = static_cast<uint32_t>(mesh.vertices.size());
  vertex(-0.10f, -halfHeight);
  vertex(0.10f, -halfHeight);
  vertex(0.0f, -halfHeight - 0.15f);
  mesh.indices.insert(mesh.indices.end(), {tail, tail + 1, tail + 2});
  return mesh;
}

float DistanceXZ(const XMFLOAT3 &a, const XMFLOAT3 &b) {
  const float dx = a.x - b.x;
  const float dz = a.z - b.z;
  return std::sqrt(dx * dx + dz * dz);
}

XMFLOAT3 EntrancePosition(int tableIndex) {
  return {-1.25f + static_cast<float>(tableIndex) * 1.25f, 0.0f, -0.90f};
}

std::array<XMFLOAT3, 5> CustomerRoute(int tableIndex) {
  const float tableX = kTableInteractions[tableIndex].x;
  const float aisleX = tableX + 1.90f;
  // 入口から前方通路、卓間の通路、席の背面を経由して家具を避ける。
  return {EntrancePosition(tableIndex), XMFLOAT3{aisleX, 0.0f, 1.35f},
          XMFLOAT3{aisleX, 0.0f, 6.10f}, XMFLOAT3{tableX, 0.0f, 6.10f},
          XMFLOAT3{tableX, 0.0f, tableIndex == 2 ? 5.12f : 5.28f}};
}

float CustomerTravelSeconds(int tableIndex) {
  const auto route = CustomerRoute(tableIndex);
  float distance = 0.0f;
  for (size_t i = 1; i < route.size(); ++i)
    distance += DistanceXZ(route[i - 1], route[i]);
  return distance / kCustomerWalkSpeed;
}

XMFLOAT3 CustomerRoutePosition(int tableIndex, float secondsRemaining,
                              bool leaving, float &yaw) {
  auto route = CustomerRoute(tableIndex);
  if (leaving)
    std::reverse(route.begin(), route.end());
  float distance = std::max(0.0f, CustomerTravelSeconds(tableIndex) -
                                    secondsRemaining) * kCustomerWalkSpeed;
  for (size_t i = 1; i < route.size(); ++i) {
    const float segment = DistanceXZ(route[i - 1], route[i]);
    if (distance <= segment || i == route.size() - 1) {
      const float t = std::clamp(distance / segment, 0.0f, 1.0f);
      const float dx = route[i].x - route[i - 1].x;
      const float dz = route[i].z - route[i - 1].z;
      yaw = std::atan2(dx, dz);
      return {route[i - 1].x + dx * t, 0.0f, route[i - 1].z + dz * t};
    }
    distance -= segment;
  }
  return route.back();
}

ImU32 TavernUiColor(float r, float g, float b, float a) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a));
}

void DrawPanel(ImDrawList *draw, const ImVec2 &minimum, const ImVec2 &maximum,
               ImU32 borderColor, float rounding = 10.0f) {
  draw->AddRectFilled(minimum, maximum,
                      TavernUiColor(0.035f, 0.022f, 0.015f, 0.92f), rounding);
  draw->AddRect(minimum, maximum, borderColor, rounding, 0, 1.5f);
}

void DrawSupplierIllustration(ImDrawList *draw, const ImVec2 &minimum,
                              const ImVec2 &maximum) {
  const float width = maximum.x - minimum.x;
  const float height = maximum.y - minimum.y;
  draw->AddRectFilledMultiColor(minimum, maximum,
                                TavernUiColor(0.12f, 0.085f, 0.045f, 1.0f),
                                TavernUiColor(0.20f, 0.115f, 0.045f, 1.0f),
                                TavernUiColor(0.045f, 0.032f, 0.022f, 1.0f),
                                TavernUiColor(0.075f, 0.045f, 0.024f, 1.0f));

  const ImU32 warmWood = TavernUiColor(0.58f, 0.29f, 0.095f, 1.0f);
  const ImU32 lightWood = TavernUiColor(0.82f, 0.49f, 0.17f, 1.0f);
  const ImU32 darkWood = TavernUiColor(0.19f, 0.085f, 0.035f, 1.0f);
  const ImU32 metal = TavernUiColor(0.62f, 0.67f, 0.64f, 1.0f);
  const ImU32 cloth = TavernUiColor(0.76f, 0.66f, 0.43f, 1.0f);
  const ImU32 glow = TavernUiColor(1.0f, 0.58f, 0.16f, 0.38f);

  const ImVec2 halo(minimum.x + width * 0.49f, minimum.y + height * 0.49f);
  draw->AddCircleFilled(halo, std::min(width, height) * 0.28f, glow, 48);

  const ImVec2 shelfMin(minimum.x + width * 0.10f, minimum.y + height * 0.22f);
  const ImVec2 shelfMax(minimum.x + width * 0.90f, minimum.y + height * 0.78f);
  draw->AddRectFilled(ImVec2(shelfMin.x, shelfMax.y - height * 0.035f),
                      shelfMax, darkWood, 3.0f);
  draw->AddRectFilled(ImVec2(shelfMin.x, shelfMin.y),
                      ImVec2(shelfMax.x, shelfMin.y + height * 0.028f),
                      lightWood, 3.0f);

  const ImVec2 barrelMin(minimum.x + width * 0.14f, minimum.y + height * 0.34f);
  const ImVec2 barrelMax(minimum.x + width * 0.47f, minimum.y + height * 0.76f);
  draw->AddRectFilled(barrelMin, barrelMax, warmWood, width * 0.07f);
  for (const float t : {0.16f, 0.50f, 0.84f}) {
    const float y = barrelMin.y + (barrelMax.y - barrelMin.y) * t;
    draw->AddLine(ImVec2(barrelMin.x + width * 0.015f, y),
                  ImVec2(barrelMax.x - width * 0.015f, y), metal,
                  std::max(2.0f, width * 0.012f));
  }
  draw->AddLine(ImVec2((barrelMin.x + barrelMax.x) * 0.5f, barrelMin.y),
                ImVec2((barrelMin.x + barrelMax.x) * 0.5f, barrelMax.y),
                darkWood, std::max(1.5f, width * 0.008f));

  const ImVec2 crateMin(minimum.x + width * 0.53f, minimum.y + height * 0.49f);
  const ImVec2 crateMax(minimum.x + width * 0.86f, minimum.y + height * 0.76f);
  draw->AddRectFilled(crateMin, crateMax, lightWood, 4.0f);
  draw->AddRect(crateMin, crateMax, darkWood, 4.0f, 0,
                std::max(2.0f, width * 0.012f));
  draw->AddLine(crateMin, crateMax, darkWood, std::max(2.0f, width * 0.010f));
  draw->AddLine(ImVec2(crateMax.x, crateMin.y), ImVec2(crateMin.x, crateMax.y),
                darkWood, std::max(2.0f, width * 0.010f));

  const float sackRadius = std::min(width, height) * 0.092f;
  const ImVec2 sackCenter(minimum.x + width * 0.66f,
                          minimum.y + height * 0.37f);
  draw->AddCircleFilled(sackCenter, sackRadius, cloth, 32);
  draw->AddRectFilled(ImVec2(sackCenter.x - sackRadius * 0.48f,
                             sackCenter.y - sackRadius * 1.18f),
                      ImVec2(sackCenter.x + sackRadius * 0.48f,
                             sackCenter.y - sackRadius * 0.72f),
                      cloth, 4.0f);
  draw->AddLine(ImVec2(sackCenter.x - sackRadius * 0.48f,
                       sackCenter.y - sackRadius * 0.70f),
                ImVec2(sackCenter.x + sackRadius * 0.48f,
                       sackCenter.y - sackRadius * 0.70f),
                darkWood, std::max(1.5f, width * 0.008f));
}

void DrawAleSupplyIllustration(ImDrawList *draw, const ImVec2 &minimum,
                               const ImVec2 &maximum) {
  const float width = maximum.x - minimum.x;
  const float height = maximum.y - minimum.y;
  draw->AddRectFilledMultiColor(minimum, maximum,
                                TavernUiColor(0.16f, 0.085f, 0.028f, 1.0f),
                                TavernUiColor(0.30f, 0.15f, 0.045f, 1.0f),
                                TavernUiColor(0.045f, 0.025f, 0.018f, 1.0f),
                                TavernUiColor(0.075f, 0.040f, 0.020f, 1.0f));

  const ImU32 wood = TavernUiColor(0.67f, 0.32f, 0.085f, 1.0f);
  const ImU32 lightWood = TavernUiColor(0.90f, 0.53f, 0.16f, 1.0f);
  const ImU32 darkWood = TavernUiColor(0.20f, 0.075f, 0.025f, 1.0f);
  const ImU32 metal = TavernUiColor(0.66f, 0.70f, 0.66f, 1.0f);
  const ImU32 ale = TavernUiColor(0.95f, 0.44f, 0.055f, 1.0f);
  const ImU32 foam = TavernUiColor(1.0f, 0.90f, 0.67f, 1.0f);
  const ImVec2 halo(minimum.x + width * 0.50f, minimum.y + height * 0.50f);
  draw->AddCircleFilled(halo, std::min(width, height) * 0.34f,
                        TavernUiColor(1.0f, 0.55f, 0.10f, 0.24f), 48);

  const ImVec2 barrelMin(minimum.x + width * 0.22f, minimum.y + height * 0.16f);
  const ImVec2 barrelMax(minimum.x + width * 0.73f, minimum.y + height * 0.84f);
  draw->AddRectFilled(barrelMin, barrelMax, wood, width * 0.11f);
  draw->AddRect(barrelMin, barrelMax, darkWood, width * 0.11f, 0,
                std::max(2.0f, width * 0.012f));
  for (const float xScale : {0.34f, 0.50f, 0.66f}) {
    const float x = barrelMin.x + (barrelMax.x - barrelMin.x) * xScale;
    draw->AddLine(ImVec2(x, barrelMin.y + height * 0.02f),
                  ImVec2(x, barrelMax.y - height * 0.02f), darkWood,
                  std::max(1.5f, width * 0.007f));
  }
  for (const float yScale : {0.18f, 0.50f, 0.82f}) {
    const float y = barrelMin.y + (barrelMax.y - barrelMin.y) * yScale;
    draw->AddLine(ImVec2(barrelMin.x + width * 0.015f, y),
                  ImVec2(barrelMax.x - width * 0.015f, y), metal,
                  std::max(3.0f, width * 0.018f));
  }
  draw->AddLine(ImVec2(barrelMin.x + width * 0.08f, barrelMin.y),
                ImVec2(barrelMax.x - width * 0.08f, barrelMin.y), lightWood,
                std::max(2.0f, width * 0.012f));

  const ImVec2 tapBase(barrelMax.x - width * 0.02f, minimum.y + height * 0.55f);
  draw->AddRectFilled(
      ImVec2(tapBase.x, tapBase.y - height * 0.035f),
      ImVec2(tapBase.x + width * 0.16f, tapBase.y + height * 0.035f), metal,
      3.0f);
  draw->AddRectFilled(
      ImVec2(tapBase.x + width * 0.11f, tapBase.y),
      ImVec2(tapBase.x + width * 0.16f, tapBase.y + height * 0.12f), metal,
      3.0f);

  const ImVec2 mugMin(minimum.x + width * 0.64f, minimum.y + height * 0.61f);
  const ImVec2 mugMax(minimum.x + width * 0.91f, minimum.y + height * 0.89f);
  draw->AddRectFilled(mugMin, mugMax,
                      TavernUiColor(0.08f, 0.055f, 0.035f, 0.96f), 6.0f);
  draw->AddRectFilled(
      ImVec2(mugMin.x + width * 0.018f, mugMin.y + height * 0.075f),
      ImVec2(mugMax.x - width * 0.018f, mugMax.y - height * 0.025f), ale, 4.0f);
  draw->AddRectFilled(
      ImVec2(mugMin.x + width * 0.014f, mugMin.y + height * 0.045f),
      ImVec2(mugMax.x - width * 0.014f, mugMin.y + height * 0.10f), foam, 7.0f);
  draw->AddRect(mugMin, mugMax, metal, 6.0f, 0, std::max(2.0f, width * 0.012f));
  draw->AddCircle(
      ImVec2(mugMax.x + width * 0.045f, (mugMin.y + mugMax.y) * 0.5f),
      width * 0.075f, metal, 24, std::max(2.0f, width * 0.014f));
}

struct ManagementButtonResult {
  bool clicked = false;
  bool hovered = false;
};

ManagementButtonResult DrawManagementButton(ImDrawList *draw, const char *id,
                                            const char *label,
                                            const ImVec2 &minimum,
                                            const ImVec2 &maximum, bool enabled,
                                            ImU32 accent = 0) {
  bool submitted = false;
  bool hovered = false;
  if (enabled) {
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(minimum);
    submitted = ImGui::InvisibleButton(
        "##button", ImVec2(maximum.x - minimum.x, maximum.y - minimum.y));
    hovered = ImGui::IsItemHovered();
    ImGui::PopID();
  }

  if (accent == 0)
    accent = TavernUiColor(0.96f, 0.56f, 0.20f, 1.0f);
  const bool highlighted = enabled && hovered;
  draw->AddRectFilled(
      minimum, maximum,
      !enabled ? TavernUiColor(0.055f, 0.045f, 0.038f, 0.92f)
               : (highlighted ? TavernUiColor(0.25f, 0.13f, 0.045f, 0.99f)
                              : TavernUiColor(0.10f, 0.060f, 0.030f, 0.97f)),
      7.0f);
  draw->AddRect(minimum, maximum,
                enabled ? accent : TavernUiColor(0.32f, 0.27f, 0.23f, 0.80f),
                7.0f, 0, highlighted ? 2.4f : 1.3f);
  const ImVec2 labelSize = ImGui::CalcTextSize(label);
  draw->AddText(ImVec2((minimum.x + maximum.x - labelSize.x) * 0.5f,
                       (minimum.y + maximum.y - labelSize.y) * 0.5f),
                enabled ? TavernUiColor(1.0f, 0.90f, 0.74f, 1.0f)
                        : TavernUiColor(0.48f, 0.44f, 0.40f, 1.0f),
                label);
  return {enabled && submitted, hovered};
}

void DrawUpgradeIllustration(ImDrawList *draw, const ImVec2 &minimum,
                             const ImVec2 &maximum) {
  const float width = maximum.x - minimum.x;
  const float height = maximum.y - minimum.y;
  draw->AddRectFilledMultiColor(minimum, maximum,
                                TavernUiColor(0.055f, 0.075f, 0.080f, 1.0f),
                                TavernUiColor(0.075f, 0.14f, 0.13f, 1.0f),
                                TavernUiColor(0.025f, 0.035f, 0.040f, 1.0f),
                                TavernUiColor(0.035f, 0.070f, 0.065f, 1.0f));

  const ImU32 teal = TavernUiColor(0.30f, 0.86f, 0.70f, 1.0f);
  const ImU32 tealSoft = TavernUiColor(0.26f, 0.72f, 0.60f, 0.42f);
  const ImU32 gold = TavernUiColor(1.0f, 0.64f, 0.20f, 1.0f);
  const ImU32 steel = TavernUiColor(0.72f, 0.78f, 0.76f, 1.0f);
  const ImU32 dark = TavernUiColor(0.055f, 0.075f, 0.072f, 1.0f);

  const ImVec2 halo(minimum.x + width * 0.50f, minimum.y + height * 0.48f);
  draw->AddCircleFilled(halo, std::min(width, height) * 0.30f, tealSoft, 48);

  const float baseY = minimum.y + height * 0.77f;
  const float left = minimum.x + width * 0.18f;
  const float right = minimum.x + width * 0.82f;
  const float roofY = minimum.y + height * 0.43f;
  draw->AddTriangleFilled(
      ImVec2(left, roofY),
      ImVec2((left + right) * 0.5f, minimum.y + height * 0.25f),
      ImVec2(right, roofY), dark);
  draw->AddLine(ImVec2(left, roofY),
                ImVec2((left + right) * 0.5f, minimum.y + height * 0.25f), teal,
                std::max(3.0f, width * 0.016f));
  draw->AddLine(ImVec2((left + right) * 0.5f, minimum.y + height * 0.25f),
                ImVec2(right, roofY), teal, std::max(3.0f, width * 0.016f));
  draw->AddRectFilled(ImVec2(left + width * 0.055f, roofY),
                      ImVec2(right - width * 0.055f, baseY), dark, 5.0f);
  draw->AddRect(ImVec2(left + width * 0.055f, roofY),
                ImVec2(right - width * 0.055f, baseY), teal, 5.0f, 0,
                std::max(2.0f, width * 0.010f));

  const ImVec2 handleStart(minimum.x + width * 0.34f,
                           minimum.y + height * 0.69f);
  const ImVec2 handleEnd(minimum.x + width * 0.62f, minimum.y + height * 0.39f);
  draw->AddLine(handleStart, handleEnd, gold, std::max(8.0f, width * 0.040f));
  const ImVec2 hammerHeadCenter(minimum.x + width * 0.65f,
                                minimum.y + height * 0.36f);
  const ImVec2 headAcross(width * 0.12f, -height * 0.070f);
  const ImVec2 headDepth(width * 0.035f, height * 0.048f);
  draw->AddQuadFilled(ImVec2(hammerHeadCenter.x - headAcross.x - headDepth.x,
                             hammerHeadCenter.y - headAcross.y - headDepth.y),
                      ImVec2(hammerHeadCenter.x + headAcross.x - headDepth.x,
                             hammerHeadCenter.y + headAcross.y - headDepth.y),
                      ImVec2(hammerHeadCenter.x + headAcross.x + headDepth.x,
                             hammerHeadCenter.y + headAcross.y + headDepth.y),
                      ImVec2(hammerHeadCenter.x - headAcross.x + headDepth.x,
                             hammerHeadCenter.y - headAcross.y + headDepth.y),
                      steel);

  const float arrowX = minimum.x + width * 0.76f;
  const float arrowBottom = minimum.y + height * 0.64f;
  const float arrowTop = minimum.y + height * 0.26f;
  draw->AddLine(ImVec2(arrowX, arrowBottom), ImVec2(arrowX, arrowTop), teal,
                std::max(4.0f, width * 0.020f));
  draw->AddTriangleFilled(
      ImVec2(arrowX, arrowTop - height * 0.055f),
      ImVec2(arrowX - width * 0.060f, arrowTop + height * 0.035f),
      ImVec2(arrowX + width * 0.060f, arrowTop + height * 0.035f), teal);
}

enum class UpgradeArtwork { Mug, AleCapacity, Table };

void DrawUpgradeCardIllustration(ImDrawList *draw, UpgradeArtwork artwork,
                                 const ImVec2 &minimum, const ImVec2 &maximum,
                                 int level) {
  const float width = maximum.x - minimum.x;
  const float height = maximum.y - minimum.y;
  const ImU32 teal = TavernUiColor(0.28f, 0.88f, 0.70f, 1.0f);
  const ImU32 tealSoft = TavernUiColor(0.22f, 0.66f, 0.54f, 0.32f);
  const ImU32 gold = TavernUiColor(1.0f, 0.62f, 0.18f, 1.0f);
  const ImU32 wood = TavernUiColor(0.55f, 0.27f, 0.085f, 1.0f);
  const ImU32 darkWood = TavernUiColor(0.12f, 0.055f, 0.025f, 1.0f);
  const ImU32 metal = TavernUiColor(0.66f, 0.72f, 0.70f, 1.0f);
  draw->AddRectFilledMultiColor(minimum, maximum,
                                TavernUiColor(0.045f, 0.075f, 0.070f, 1.0f),
                                TavernUiColor(0.080f, 0.145f, 0.125f, 1.0f),
                                TavernUiColor(0.020f, 0.032f, 0.030f, 1.0f),
                                TavernUiColor(0.028f, 0.058f, 0.050f, 1.0f));
  draw->AddCircleFilled(
      ImVec2(minimum.x + width * 0.50f, minimum.y + height * 0.50f),
      std::min(width, height) * 0.34f, tealSoft, 40);

  if (artwork == UpgradeArtwork::Mug) {
    const float mugWidth = width * 0.18f;
    const float mugHeight = height * 0.42f;
    for (int mug = 0; mug < 3; ++mug) {
      const float centerX = minimum.x + width * (0.27f + mug * 0.23f);
      const float top = minimum.y + height * (mug == 1 ? 0.25f : 0.34f);
      const ImVec2 mugMin(centerX - mugWidth * 0.5f, top);
      const ImVec2 mugMax(centerX + mugWidth * 0.5f, top + mugHeight);
      const bool unlocked = mug < 2 || level > 0;
      draw->AddRectFilled(mugMin, mugMax,
                          unlocked ? TavernUiColor(0.82f, 0.78f, 0.64f, 0.94f)
                                   : TavernUiColor(0.08f, 0.12f, 0.11f, 0.86f),
                          7.0f);
      draw->AddRect(mugMin, mugMax, unlocked ? metal : teal, 7.0f, 0, 2.4f);
      draw->AddCircle(
          ImVec2(mugMax.x + mugWidth * 0.23f, (mugMin.y + mugMax.y) * 0.52f),
          mugWidth * 0.31f, unlocked ? metal : teal, 24, 2.4f);
    }
    if (level == 0) {
      const float arrowY = minimum.y + height * 0.20f;
      draw->AddLine(ImVec2(minimum.x + width * 0.58f, arrowY),
                    ImVec2(minimum.x + width * 0.78f, arrowY), teal, 4.0f);
      draw->AddTriangleFilled(
          ImVec2(minimum.x + width * 0.81f, arrowY),
          ImVec2(minimum.x + width * 0.74f, arrowY - height * 0.055f),
          ImVec2(minimum.x + width * 0.74f, arrowY + height * 0.055f), teal);
    }
    return;
  }

  if (artwork == UpgradeArtwork::AleCapacity) {
    const ImVec2 barrelMin(minimum.x + width * 0.23f,
                           minimum.y + height * 0.18f);
    const ImVec2 barrelMax(minimum.x + width * 0.73f,
                           minimum.y + height * 0.82f);
    draw->AddRectFilled(barrelMin, barrelMax, wood, width * 0.09f);
    draw->AddRect(barrelMin, barrelMax, darkWood, width * 0.09f, 0, 2.4f);
    for (const float yScale : {0.18f, 0.50f, 0.82f}) {
      const float y = barrelMin.y + (barrelMax.y - barrelMin.y) * yScale;
      draw->AddLine(ImVec2(barrelMin.x + 4.0f, y),
                    ImVec2(barrelMax.x - 4.0f, y), metal, 3.0f);
    }
    const ImVec2 tap(barrelMax.x, minimum.y + height * 0.55f);
    draw->AddRectFilled(ImVec2(tap.x, tap.y - 5.0f),
                        ImVec2(tap.x + width * 0.13f, tap.y + 5.0f), metal,
                        3.0f);
    for (int pip = 0; pip < kMaximumAleCapacityLevel; ++pip) {
      const bool filled = pip < level;
      const ImVec2 center(minimum.x + width * 0.33f + pip * width * 0.17f,
                          minimum.y + height * 0.90f);
      draw->AddCircleFilled(
          center, std::min(width, height) * 0.028f,
          filled ? gold : TavernUiColor(0.13f, 0.17f, 0.15f, 1.0f), 20);
      draw->AddCircle(center, std::min(width, height) * 0.028f, teal, 20, 1.4f);
    }
    return;
  }

  const float tableY = minimum.y + height * 0.54f;
  draw->AddRectFilled(
      ImVec2(minimum.x + width * 0.16f, tableY),
      ImVec2(minimum.x + width * 0.84f, tableY + height * 0.14f), wood, 6.0f);
  draw->AddRect(ImVec2(minimum.x + width * 0.16f, tableY),
                ImVec2(minimum.x + width * 0.84f, tableY + height * 0.14f),
                darkWood, 6.0f, 0, 2.4f);
  for (const float xScale : {0.23f, 0.77f}) {
    draw->AddRectFilled(
        ImVec2(minimum.x + width * xScale, tableY + height * 0.13f),
        ImVec2(minimum.x + width * (xScale + 0.07f),
               minimum.y + height * 0.88f),
        darkWood, 3.0f);
  }
  for (const float offset : {-0.15f, 0.15f}) {
    const float benchY = tableY + offset * height;
    draw->AddRectFilled(
        ImVec2(minimum.x + width * 0.21f, benchY),
        ImVec2(minimum.x + width * 0.79f, benchY + height * 0.065f), darkWood,
        4.0f);
  }
  const ImVec2 candle(minimum.x + width * 0.50f, minimum.y + height * 0.37f);
  draw->AddCircleFilled(candle, std::min(width, height) * 0.17f,
                        level > 0 ? TavernUiColor(1.0f, 0.56f, 0.14f, 0.26f)
                                  : TavernUiColor(0.0f, 0.0f, 0.0f, 0.0f),
                        32);
  draw->AddRectFilled(ImVec2(candle.x - 4.0f, candle.y),
                      ImVec2(candle.x + 4.0f, candle.y + height * 0.16f),
                      TavernUiColor(0.84f, 0.75f, 0.56f, 1.0f), 3.0f);
  draw->AddCircleFilled(candle, 6.0f, level > 0 ? gold : metal, 18);
}

struct UpgradeCardResult {
  bool clicked = false;
  bool submitted = false;
};

UpgradeCardResult DrawUpgradeCard(ImDrawList *draw, const char *id,
                                  const char *title, const char *effect,
                                  const char *note, const char *status,
                                  UpgradeArtwork artwork, int artworkLevel,
                                  const ImVec2 &minimum, const ImVec2 &maximum,
                                  bool selected, bool purchasable,
                                  bool interactive) {
  bool clicked = false;
  bool hovered = false;
  if (interactive) {
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(minimum);
    clicked = ImGui::InvisibleButton(
        "##upgrade-card", ImVec2(maximum.x - minimum.x, maximum.y - minimum.y));
    hovered = ImGui::IsItemHovered();
    ImGui::PopID();
  }

  const bool highlighted = selected || hovered;
  const ImU32 accent = purchasable ? TavernUiColor(0.30f, 0.90f, 0.72f, 1.0f)
                                   : TavernUiColor(0.46f, 0.43f, 0.38f, 0.86f);
  draw->AddRectFilled(minimum, maximum,
                      selected ? TavernUiColor(0.075f, 0.13f, 0.115f, 0.99f)
                               : TavernUiColor(0.040f, 0.030f, 0.024f, 0.99f),
                      13.0f);
  draw->AddRect(minimum, maximum, accent, 13.0f, 0, highlighted ? 3.0f : 1.4f);

  const float width = maximum.x - minimum.x;
  const float height = maximum.y - minimum.y;
  const ImVec2 imageMin(minimum.x + 12.0f, minimum.y + 12.0f);
  const ImVec2 imageMax(maximum.x - 12.0f, minimum.y + height * 0.48f);
  draw->PushClipRect(imageMin, imageMax, true);
  DrawUpgradeCardIllustration(draw, artwork, imageMin, imageMax, artworkLevel);
  draw->PopClipRect();
  draw->AddRect(imageMin, imageMax, TavernUiColor(0.25f, 0.52f, 0.44f, 0.84f),
                7.0f, 0, 1.2f);

  ImFont *font = ImGui::GetFont();
  const float titleSizePx = std::clamp(width * 0.075f, 24.0f, 33.0f);
  const ImVec2 titleSize =
      font->CalcTextSizeA(titleSizePx, FLT_MAX, 0.0f, title);
  draw->AddText(font, titleSizePx,
                ImVec2((minimum.x + maximum.x - titleSize.x) * 0.5f,
                       minimum.y + height * 0.515f),
                TavernUiColor(1.0f, 0.90f, 0.72f, 1.0f), title);
  const ImVec2 effectSize = ImGui::CalcTextSize(effect);
  draw->AddText(ImVec2((minimum.x + maximum.x - effectSize.x) * 0.5f,
                       minimum.y + height * 0.635f),
                TavernUiColor(0.82f, 0.91f, 0.85f, 1.0f), effect);
  const ImVec2 noteSize = ImGui::CalcTextSize(note);
  draw->AddText(ImVec2((minimum.x + maximum.x - noteSize.x) * 0.5f,
                       minimum.y + height * 0.705f),
                TavernUiColor(0.62f, 0.67f, 0.63f, 1.0f), note);

  const ImVec2 statusMin(minimum.x + 18.0f, minimum.y + height * 0.805f);
  const ImVec2 statusMax(maximum.x - 18.0f, maximum.y - 18.0f);
  draw->AddRectFilled(statusMin, statusMax,
                      purchasable
                          ? TavernUiColor(0.08f, 0.22f, 0.17f, 0.98f)
                          : TavernUiColor(0.065f, 0.055f, 0.048f, 0.96f),
                      7.0f);
  draw->AddRect(statusMin, statusMax, accent, 7.0f, 0, 1.3f);
  const ImVec2 statusSize = ImGui::CalcTextSize(status);
  draw->AddText(ImVec2((statusMin.x + statusMax.x - statusSize.x) * 0.5f,
                       (statusMin.y + statusMax.y - statusSize.y) * 0.5f),
                purchasable ? TavernUiColor(0.72f, 1.0f, 0.88f, 1.0f)
                            : TavernUiColor(0.62f, 0.58f, 0.52f, 1.0f),
                status);
  return {clicked, true};
}

struct ManagementCardResult {
  bool clicked = false;
  bool submitted = false;
  bool supplierArtwork = false;
  bool labelRegionValid = false;
  const char *label = nullptr;
  ImVec2 imageMinimum{};
  ImVec2 imageMaximum{};
};

ManagementCardResult DrawManagementCard(ImDrawList *draw, const char *id,
                                        const char *label,
                                        const ImVec2 &minimum,
                                        const ImVec2 &maximum, bool supplier,
                                        bool selected) {
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(minimum);
  const bool clicked = ImGui::InvisibleButton(
      "##card", ImVec2(maximum.x - minimum.x, maximum.y - minimum.y));
  const bool hovered = ImGui::IsItemHovered();
  const bool focused = ImGui::IsItemFocused();
  ImGui::PopID();

  const float cardHeight = maximum.y - minimum.y;
  const float footerHeight = std::clamp(cardHeight * 0.205f, 70.0f, 108.0f);
  const ImVec2 imageMinimum(minimum.x + 10.0f, minimum.y + 10.0f);
  const ImVec2 imageMaximum(maximum.x - 10.0f, maximum.y - footerHeight - 1.0f);
  const bool highlighted = hovered || focused || selected;

  draw->AddRectFilled(
      minimum, maximum,
      selected ? TavernUiColor(0.13f, 0.085f, 0.035f, 0.99f)
               : (highlighted ? TavernUiColor(0.095f, 0.064f, 0.038f, 0.98f)
                              : TavernUiColor(0.045f, 0.030f, 0.020f, 0.97f)),
      14.0f);
  draw->PushClipRect(imageMinimum, imageMaximum, true);
  if (supplier)
    DrawSupplierIllustration(draw, imageMinimum, imageMaximum);
  else
    DrawUpgradeIllustration(draw, imageMinimum, imageMaximum);
  draw->PopClipRect();

  const ImU32 accent = supplier ? TavernUiColor(1.0f, 0.60f, 0.20f, 1.0f)
                                : TavernUiColor(0.32f, 0.90f, 0.72f, 1.0f);
  draw->AddLine(ImVec2(minimum.x + 10.0f, imageMaximum.y),
                ImVec2(maximum.x - 10.0f, imageMaximum.y), accent,
                highlighted ? 2.4f : 1.5f);
  draw->AddRect(minimum, maximum,
                highlighted ? accent
                            : TavernUiColor(0.48f, 0.30f, 0.16f, 0.72f),
                14.0f, 0, highlighted ? 3.0f : 1.5f);

  ImFont *font = ImGui::GetFont();
  const float labelSizePx = std::clamp(cardHeight * 0.075f, 28.0f, 43.0f);
  const ImVec2 labelSize =
      font->CalcTextSizeA(labelSizePx, FLT_MAX, 0.0f, label);
  const float footerTop = maximum.y - footerHeight;
  draw->AddText(font, labelSizePx,
                ImVec2((minimum.x + maximum.x - labelSize.x) * 0.5f,
                       footerTop + (footerHeight - labelSize.y) * 0.5f),
                highlighted ? TavernUiColor(1.0f, 0.94f, 0.82f, 1.0f)
                            : TavernUiColor(0.91f, 0.84f, 0.74f, 1.0f),
                label);
  ManagementCardResult result;
  result.clicked = clicked;
  result.submitted = true;
  result.supplierArtwork = supplier;
  result.labelRegionValid = footerHeight > labelSize.y + 8.0f;
  result.label = label;
  result.imageMinimum = imageMinimum;
  result.imageMaximum = imageMaximum;
  return result;
}

constexpr std::array<const char *, 14> kTavernAssetPaths = {
    "Assets/models/models_2/optimized/barrel.glb",
    "Assets/models/models_2/optimized/crate.glb",
    "Assets/models/models_2/optimized/stool.glb",
    "Assets/models/models_2/optimized/chest.glb",
    "Assets/models/models_2/optimized/round_table.glb",
    "Assets/models/models_2/optimized/long_table.glb",
    "Assets/models/models_2/optimized/bench.glb",
    "Assets/models/models_2/optimized/mug.glb",
    "Assets/models/models_2/optimized/bowl.glb",
    "Assets/models/models_2/optimized/plate.glb",
    "Assets/models/models_2/optimized/sword.glb",
    "Assets/models/models_2/optimized/halberd.glb",
    "Assets/models/models_2/optimized/mace.glb",
    "Assets/models/models_2/optimized/candle.glb",
};

std::string ResolveTavernAssetPath(const std::string &path) {
  namespace fs = std::filesystem;

  const fs::path direct(path);
  const fs::path sourceFromBuild = fs::path("..") / ".." / ".." / path;
  if (fs::exists(sourceFromBuild))
    return sourceFromBuild.generic_string();
  if (fs::exists(direct))
    return direct.generic_string();
  return path;
}

bool LoadTavernModelMeshIds(DxContext &dx, const std::string &path,
                            std::vector<uint32_t> &outMeshIds) {
  const std::string resolvedPath = ResolveTavernAssetPath(path);
  std::vector<LoadedMeshPart> parts;
  if (!LoadStaticModelParts(resolvedPath, parts)) {
    OutputDebugStringA("[TavernScene] WARNING: model load failed: ");
    OutputDebugStringA(resolvedPath.c_str());
    OutputDebugStringA("\n");
    return false;
  }

  for (const LoadedMeshPart &part : parts) {
    const uint32_t meshId = dx.CreateMeshResources(
        part.mesh, part.GetMaterialImages(), part.material);
    if (meshId != UINT32_MAX)
      outMeshIds.push_back(meshId);
  }

  if (outMeshIds.empty()) {
    OutputDebugStringA("[TavernScene] WARNING: no mesh uploaded for model: ");
    OutputDebugStringA(resolvedPath.c_str());
    OutputDebugStringA("\n");
    return false;
  }
  return true;
}

} // namespace

const char *TavernScene::GetTableStateName(TableState state) {
  switch (state) {
  case TableState::Empty:
    return "空席";
  case TableState::Arriving:
    return "来店中";
  case TableState::WaitingOrder:
    return "注文待ち";
  case TableState::WaitingAle:
    return "エール待ち";
  case TableState::WaitingFood:
    return "料理待ち";
  case TableState::Eating:
    return "飲食中";
  case TableState::Leaving:
    return "退店中";
  case TableState::Dirty:
    return "片付け待ち";
  }
  return "不明";
}

const char *TavernScene::GetHeldItemName(HeldItem item) {
  switch (item) {
  case HeldItem::None:
    return "手ぶら";
  case HeldItem::EmptyMug:
    return "空のジョッキ";
  case HeldItem::FilledMug:
    return "エール入りジョッキ";
  case HeldItem::DirtyMug:
    return "汚れたジョッキ";
  case HeldItem::CleanBowl:
    return "空のボウル";
  case HeldItem::FilledBowl:
    return "月影シチュー";
  case HeldItem::DirtyBowl:
    return "汚れたボウル";
  }
  return "手ぶら";
}

const char *TavernScene::GetOrderName(OrderType order) {
  switch (order) {
  case OrderType::Ale:
    return kAleName;
  case OrderType::Food:
    return kFoodName;
  case OrderType::None:
    break;
  }
  return "注文なし";
}

bool TavernScene::IsMugItem(HeldItem item) {
  return item == HeldItem::EmptyMug || item == HeldItem::FilledMug ||
         item == HeldItem::DirtyMug;
}

bool TavernScene::IsBowlItem(HeldItem item) {
  return item == HeldItem::CleanBowl || item == HeldItem::FilledBowl ||
         item == HeldItem::DirtyBowl;
}

bool TavernScene::PlayerMovementLocked() const {
  return m_workState != WorkState::None || ManagementMenuOpen();
}

bool TavernScene::ManagementMenuOpen() const {
  return m_managementPage != ManagementPage::Closed;
}

XMFLOAT3 TavernScene::ManagementInteractionPosition() const {
  return kManagementTableInteraction;
}

TavernScene::SupplyState &TavernScene::AleSupply() {
  return m_supplies[static_cast<std::size_t>(SupplyType::Ale)];
}

const TavernScene::SupplyState &TavernScene::AleSupply() const {
  return m_supplies[static_cast<std::size_t>(SupplyType::Ale)];
}

int TavernScene::AleStock() const { return AleSupply().current; }

int TavernScene::AleCapacity() const { return AleSupply().capacity; }

int TavernScene::AleCapacityLevel() const {
  return std::clamp(AleCapacity() - kInitialAleCapacity, 0,
                    kMaximumAleCapacityLevel);
}

int TavernScene::TotalMugs() const {
  return kInitialMugCapacity + std::clamp(m_extraMugLevel, 0, 1);
}

int TavernScene::CustomerTableCount() const {
  return m_table3Unlocked ? static_cast<int>(m_tables.size()) : kBaseTableCount;
}

bool TavernScene::AlePourInProgress() const {
  return m_workState == WorkState::PouringAle;
}

bool TavernScene::KitchenCooking() const {
  return m_kitchenState == KitchenState::Cooking;
}

bool TavernScene::KitchenReady() const {
  return m_kitchenState == KitchenState::Ready;
}

float TavernScene::KitchenProgress() const {
  if (m_kitchenState == KitchenState::Ready)
    return 1.0f;
  if (m_kitchenState != KitchenState::Cooking)
    return 0.0f;
  return std::clamp(1.0f - m_cookTimer / kCookSeconds, 0.0f, 1.0f);
}

int TavernScene::AleUnitPrice() const { return kAleSupplyUnitPrice; }

int TavernScene::MaximumAleOrderQuantity() const {
  return std::max(0, AleCapacity() - AleStock());
}

TavernScene::SupplyOrderBlockReason
TavernScene::CurrentAleOrderBlockReason() const {
  const int maximumQuantity = MaximumAleOrderQuantity();
  if (maximumQuantity <= 0 || m_aleOrderQuantity > maximumQuantity)
    return SupplyOrderBlockReason::Full;
  if (m_aleOrderQuantity <= 0)
    return SupplyOrderBlockReason::ZeroQuantity;
  if (m_gold < m_aleOrderQuantity * kAleSupplyUnitPrice)
    return SupplyOrderBlockReason::InsufficientGold;
  return SupplyOrderBlockReason::None;
}

void TavernScene::AdjustAleOrderQuantity(int delta) {
  m_aleOrderQuantity =
      std::clamp(m_aleOrderQuantity + delta, 0, MaximumAleOrderQuantity());
}

bool TavernScene::TryOrderAle() {
  if (CurrentAleOrderBlockReason() != SupplyOrderBlockReason::None)
    return false;

  const int quantity = m_aleOrderQuantity;
  const int total = quantity * kAleSupplyUnitPrice;
  m_gold -= total;
  AleSupply().current += quantity;
  m_aleOrderQuantity = 0;
  ++m_supplyPurchaseSerial;

  char feedback[96]{};
  std::snprintf(feedback, sizeof(feedback), "ALE +%d   -%d G", quantity, total);
  m_feedbackText = feedback;
  m_feedbackTimer = 1.8f;
  return true;
}

int TavernScene::UpgradeLevel(UpgradeId upgrade) const {
  switch (upgrade) {
  case UpgradeId::ExtraMug:
    return std::clamp(m_extraMugLevel, 0, 1);
  case UpgradeId::AleCapacity:
    return AleCapacityLevel();
  case UpgradeId::OpenTable3:
    return m_table3Unlocked ? 1 : 0;
  case UpgradeId::Count:
    break;
  }
  return 0;
}

int TavernScene::UpgradeMaximumLevel(UpgradeId upgrade) const {
  return upgrade == UpgradeId::AleCapacity ? kMaximumAleCapacityLevel : 1;
}

int TavernScene::UpgradePrice(UpgradeId upgrade) const {
  if (UpgradeLevel(upgrade) >= UpgradeMaximumLevel(upgrade))
    return 0;
  switch (upgrade) {
  case UpgradeId::ExtraMug:
    return kExtraMugPrice;
  case UpgradeId::AleCapacity:
    return 20 + UpgradeLevel(upgrade) * 10;
  case UpgradeId::OpenTable3:
    return kOpenTable3Price;
  case UpgradeId::Count:
    break;
  }
  return 0;
}

TavernScene::UpgradePurchaseBlockReason
TavernScene::CurrentUpgradeBlockReason(UpgradeId upgrade) const {
  const int level = UpgradeLevel(upgrade);
  const int maximumLevel = UpgradeMaximumLevel(upgrade);
  if (level >= maximumLevel) {
    return upgrade == UpgradeId::AleCapacity
               ? UpgradePurchaseBlockReason::Maxed
               : UpgradePurchaseBlockReason::Owned;
  }
  if (upgrade == UpgradeId::OpenTable3 &&
      m_tutorialStep != TutorialStep::Complete)
    return UpgradePurchaseBlockReason::TutorialRequired;

  const int price = UpgradePrice(upgrade);
  if (m_gold < price)
    return UpgradePurchaseBlockReason::InsufficientGold;
  if (AleStock() == 0 && m_gold - price < kAleSupplyUnitPrice)
    return UpgradePurchaseBlockReason::EmergencyAleReserve;
  return UpgradePurchaseBlockReason::None;
}

void TavernScene::RequestUpgradePurchase(UpgradeId upgrade) {
  m_selectedUpgradeIndex = static_cast<int>(upgrade);
  const UpgradePurchaseBlockReason blockReason =
      CurrentUpgradeBlockReason(upgrade);
  if (blockReason == UpgradePurchaseBlockReason::None) {
    m_upgradeConfirmationOpen = true;
    m_upgradeConfirmBuySelected = true;
    return;
  }

  switch (blockReason) {
  case UpgradePurchaseBlockReason::InsufficientGold:
    m_feedbackText = "NOT ENOUGH GOLD";
    break;
  case UpgradePurchaseBlockReason::Owned:
    m_feedbackText = "ALREADY OWNED";
    break;
  case UpgradePurchaseBlockReason::Maxed:
    m_feedbackText = "MAX LEVEL";
    break;
  case UpgradePurchaseBlockReason::TutorialRequired:
    m_feedbackText = "COMPLETE THE TUTORIAL FIRST";
    break;
  case UpgradePurchaseBlockReason::EmergencyAleReserve:
    m_feedbackText = "KEEP 2 G FOR ALE";
    break;
  case UpgradePurchaseBlockReason::None:
    break;
  }
  m_feedbackTimer = 1.8f;
}

bool TavernScene::TryPurchaseUpgrade(UpgradeId upgrade) {
  if (CurrentUpgradeBlockReason(upgrade) != UpgradePurchaseBlockReason::None)
    return false;

  const int price = UpgradePrice(upgrade);
  m_gold -= price;
  const char *upgradeName = "UPGRADE";
  switch (upgrade) {
  case UpgradeId::ExtraMug:
    m_extraMugLevel = 1;
    m_cleanMugs = std::min(TotalMugs(), m_cleanMugs + 1);
    upgradeName = "EXTRA MUG";
    break;
  case UpgradeId::AleCapacity:
    AleSupply().capacity =
        kInitialAleCapacity +
        std::min(kMaximumAleCapacityLevel, AleCapacityLevel() + 1);
    upgradeName = "ALE CAPACITY";
    break;
  case UpgradeId::OpenTable3:
    m_table3Unlocked = true;
    m_tables[2] = {};
    m_tables[2].enabled = true;
    m_spawnTimers[2] = CustomerSpawnDelay(2);
    upgradeName = "TABLE 3 OPEN";
    break;
  case UpgradeId::Count:
    return false;
  }

  ++m_upgradePurchaseSerial;
  m_upgradeConfirmationOpen = false;
  m_upgradeConfirmBuySelected = false;
  char feedback[96]{};
  std::snprintf(feedback, sizeof(feedback), "%s   -%d G", upgradeName, price);
  m_feedbackText = feedback;
  m_feedbackTimer = 1.8f;
  return true;
}

void TavernScene::ConfigureSuppliesSmokeState(int aleStock, int gold) {
  AleSupply().current = std::clamp(aleStock, 0, AleCapacity());
  m_gold = std::clamp(gold, 0, 999999);
  m_aleOrderQuantity = 0;
}

void TavernScene::ConfigureUpgradesSmokeState(int gold, int aleStock,
                                              bool tutorialComplete) {
  m_gold = std::clamp(gold, 0, 999999);
  AleSupply().current = std::clamp(aleStock, 0, AleCapacity());
  m_tutorialStep =
      tutorialComplete ? TutorialStep::Complete : TutorialStep::TakeOrder;
  m_tables[1].enabled = tutorialComplete;
  if (tutorialComplete && m_spawnTimers[1] <= 0.0f)
    m_spawnTimers[1] = 4.0f;
  m_tables[2].enabled = tutorialComplete && m_table3Unlocked;
  if (m_tables[2].enabled && m_spawnTimers[2] <= 0.0f)
    m_spawnTimers[2] = CustomerSpawnDelay(2);
}

void TavernScene::OpenManagementMenu() {
  m_managementPage = ManagementPage::Root;
  m_managementSelection = ManagementSelection::Supplies;
  m_aleOrderQuantity = 0;
  m_selectedUpgradeIndex = 0;
  m_upgradeConfirmationOpen = false;
  m_upgradeConfirmBuySelected = false;
  m_managementActivationSerial = 0;
  m_managementUiDiagnostics = {};
  m_interactionCooldown = 0.20f;
}

void TavernScene::OpenSuppliesPage() {
  m_managementPage = ManagementPage::Supplies;
  m_managementSelection = ManagementSelection::Supplies;
  m_aleOrderQuantity = 0;
  const uint64_t renderedFrameSerial =
      m_managementUiDiagnostics.renderedFrameSerial;
  m_managementUiDiagnostics = {};
  m_managementUiDiagnostics.menuOpen = true;
  m_managementUiDiagnostics.subpageOpen = true;
  m_managementUiDiagnostics.activationSerial = m_managementActivationSerial;
  m_managementUiDiagnostics.purchaseSerial = m_supplyPurchaseSerial;
  m_managementUiDiagnostics.upgradePurchaseSerial = m_upgradePurchaseSerial;
  m_managementUiDiagnostics.renderedFrameSerial = renderedFrameSerial;
}

void TavernScene::OpenUpgradesPage() {
  m_managementPage = ManagementPage::Upgrades;
  m_managementSelection = ManagementSelection::Upgrades;
  m_selectedUpgradeIndex = 0;
  m_upgradeConfirmationOpen = false;
  m_upgradeConfirmBuySelected = false;
  const uint64_t renderedFrameSerial =
      m_managementUiDiagnostics.renderedFrameSerial;
  m_managementUiDiagnostics = {};
  m_managementUiDiagnostics.menuOpen = true;
  m_managementUiDiagnostics.subpageOpen = true;
  m_managementUiDiagnostics.activationSerial = m_managementActivationSerial;
  m_managementUiDiagnostics.purchaseSerial = m_supplyPurchaseSerial;
  m_managementUiDiagnostics.upgradePurchaseSerial = m_upgradePurchaseSerial;
  m_managementUiDiagnostics.renderedFrameSerial = renderedFrameSerial;
}

void TavernScene::BackToManagementRoot() {
  const ManagementSelection returnSelection =
      m_managementPage == ManagementPage::Upgrades
          ? ManagementSelection::Upgrades
          : ManagementSelection::Supplies;
  m_managementPage = ManagementPage::Root;
  m_managementSelection = returnSelection;
  m_aleOrderQuantity = 0;
  m_upgradeConfirmationOpen = false;
  m_upgradeConfirmBuySelected = false;
  const uint64_t renderedFrameSerial =
      m_managementUiDiagnostics.renderedFrameSerial;
  m_managementUiDiagnostics = {};
  m_managementUiDiagnostics.menuOpen = true;
  m_managementUiDiagnostics.selectedCardIndex =
      returnSelection == ManagementSelection::Upgrades ? 1 : 0;
  m_managementUiDiagnostics.activationSerial = m_managementActivationSerial;
  m_managementUiDiagnostics.purchaseSerial = m_supplyPurchaseSerial;
  m_managementUiDiagnostics.upgradePurchaseSerial = m_upgradePurchaseSerial;
  m_managementUiDiagnostics.renderedFrameSerial = renderedFrameSerial;
}

void TavernScene::CloseManagementMenu() {
  m_managementPage = ManagementPage::Closed;
  m_managementSelection = ManagementSelection::None;
  m_aleOrderQuantity = 0;
  m_upgradeConfirmationOpen = false;
  m_upgradeConfirmBuySelected = false;
  const uint64_t renderedFrameSerial =
      m_managementUiDiagnostics.renderedFrameSerial;
  m_managementUiDiagnostics = {};
  m_managementUiDiagnostics.renderedFrameSerial = renderedFrameSerial;
  m_managementUiDiagnostics.activationSerial = m_managementActivationSerial;
  m_managementUiDiagnostics.purchaseSerial = m_supplyPurchaseSerial;
  m_managementUiDiagnostics.upgradePurchaseSerial = m_upgradePurchaseSerial;
  m_interactionCooldown = 0.20f;
}

void TavernScene::Initialize(DxContext &dx) {
  const LoadedMesh planeMesh = ProceduralMesh::CreatePlane(1.0f, 1.0f);
  const LoadedMesh cubeMesh = ProceduralMesh::CreateCube(1.0f);
  const LoadedMesh cylinderMesh =
      ProceduralMesh::CreateCylinder(0.5f, 1.0f, 20);
  const LoadedMesh bodyMesh = ProceduralMesh::CreateCylinder(0.5f, 1.0f, 16);
  const LoadedMesh headMesh = ProceduralMesh::CreateSphere(0.5f, 10, 20);

  Material floorMaterial{};
  floorMaterial.baseColorFactor = {0.24f, 0.17f, 0.10f, 1.0f};
  floorMaterial.metallicFactor = 0.0f;
  floorMaterial.roughnessFactor = 0.82f;
  floorMaterial.uvTiling = {7.0f, 6.0f};
  m_floorMeshId = dx.CreateMeshResources(planeMesh, {}, floorMaterial);

  Material wallMaterial{};
  wallMaterial.baseColorFactor = {0.46f, 0.37f, 0.25f, 1.0f};
  wallMaterial.metallicFactor = 0.0f;
  wallMaterial.roughnessFactor = 0.92f;
  m_wallMeshId = dx.CreateMeshResources(cubeMesh, {}, wallMaterial);

  Material woodMaterial{};
  woodMaterial.baseColorFactor = {0.34f, 0.16f, 0.065f, 1.0f};
  woodMaterial.metallicFactor = 0.0f;
  woodMaterial.roughnessFactor = 0.62f;
  m_woodMeshId = dx.CreateMeshResources(cubeMesh, {}, woodMaterial);

  Material darkWoodMaterial{};
  darkWoodMaterial.baseColorFactor = {0.095f, 0.055f, 0.035f, 1.0f};
  darkWoodMaterial.metallicFactor = 0.0f;
  darkWoodMaterial.roughnessFactor = 0.74f;
  m_darkWoodMeshId = dx.CreateMeshResources(cubeMesh, {}, darkWoodMaterial);

  Material glowMaterial{};
  glowMaterial.baseColorFactor = {1.0f, 0.48f, 0.12f, 1.0f};
  glowMaterial.metallicFactor = 0.0f;
  glowMaterial.roughnessFactor = 0.18f;
  glowMaterial.emissiveFactor = {3.0f, 0.72f, 0.10f};
  glowMaterial.rayTracingVisible = false;
  m_glowMeshId = dx.CreateMeshResources(cubeMesh, {}, glowMaterial);

  const LoadedMesh orderBubbleMesh = CreateOrderBubbleMesh();
  Material bubbleMaterial{};
  bubbleMaterial.baseColorFactor = {0.94f, 0.85f, 0.65f, 1.0f};
  bubbleMaterial.emissiveFactor = {0.18f, 0.15f, 0.10f};
  bubbleMaterial.roughnessFactor = 1.0f;
  bubbleMaterial.rayTracingVisible = false;
  bubbleMaterial.ssrExcluded = true;
  m_orderBubbleMeshId = dx.CreateMeshResources(orderBubbleMesh, {}, bubbleMaterial);
  Material bubbleBorderMaterial = bubbleMaterial;
  bubbleBorderMaterial.baseColorFactor = {0.14f, 0.065f, 0.025f, 1.0f};
  bubbleBorderMaterial.emissiveFactor = {0.035f, 0.015f, 0.005f};
  m_orderBubbleBorderMeshId =
      dx.CreateMeshResources(orderBubbleMesh, {}, bubbleBorderMaterial);

  Material customerBodyMaterial{};
  customerBodyMaterial.baseColorFactor = {0.16f, 0.42f, 0.52f, 1.0f};
  customerBodyMaterial.metallicFactor = 0.0f;
  customerBodyMaterial.roughnessFactor = 0.78f;
  m_customerBodyMeshId =
      dx.CreateMeshResources(bodyMesh, {}, customerBodyMaterial);

  Material customerHeadMaterial{};
  customerHeadMaterial.baseColorFactor = {0.92f, 0.72f, 0.52f, 1.0f};
  customerHeadMaterial.metallicFactor = 0.0f;
  customerHeadMaterial.roughnessFactor = 0.86f;
  m_customerHeadMeshId =
      dx.CreateMeshResources(headMesh, {}, customerHeadMaterial);

  Material customerHairMaterial{};
  customerHairMaterial.baseColorFactor = {0.075f, 0.032f, 0.016f, 1.0f};
  customerHairMaterial.metallicFactor = 0.0f;
  customerHairMaterial.roughnessFactor = 0.88f;
  m_customerHairMeshId =
      dx.CreateMeshResources(headMesh, {}, customerHairMaterial);

  Material mugMaterial{};
  mugMaterial.baseColorFactor = {0.48f, 0.50f, 0.47f, 1.0f};
  mugMaterial.metallicFactor = 0.42f;
  mugMaterial.roughnessFactor = 0.36f;
  m_mugMeshId = dx.CreateMeshResources(cylinderMesh, {}, mugMaterial);

  Material filledMugMaterial = mugMaterial;
  filledMugMaterial.baseColorFactor = {0.27f, 0.38f, 0.50f, 1.0f};
  filledMugMaterial.metallicFactor = 0.34f;
  filledMugMaterial.roughnessFactor = 0.42f;
  m_filledMugMeshId =
      dx.CreateMeshResources(cylinderMesh, {}, filledMugMaterial);

  Material dirtyMugMaterial = mugMaterial;
  dirtyMugMaterial.baseColorFactor = {0.20f, 0.46f, 0.16f, 1.0f};
  dirtyMugMaterial.metallicFactor = 0.10f;
  dirtyMugMaterial.roughnessFactor = 0.78f;
  m_dirtyMugMeshId = dx.CreateMeshResources(cylinderMesh, {}, dirtyMugMaterial);

  Material dirtySpotMaterial{};
  dirtySpotMaterial.baseColorFactor = {0.025f, 0.075f, 0.018f, 1.0f};
  dirtySpotMaterial.metallicFactor = 0.0f;
  dirtySpotMaterial.roughnessFactor = 0.94f;
  m_dirtySpotMeshId = dx.CreateMeshResources(headMesh, {}, dirtySpotMaterial);

  Material aleMaterial{};
  aleMaterial.baseColorFactor = {0.96f, 0.59f, 0.055f, 1.0f};
  aleMaterial.metallicFactor = 0.0f;
  aleMaterial.roughnessFactor = 0.20f;
  aleMaterial.emissiveFactor = {0.62f, 0.23f, 0.015f};
  aleMaterial.rayTracingVisible = false;
  m_aleMeshId = dx.CreateMeshResources(cylinderMesh, {}, aleMaterial);

  Material foamMaterial{};
  foamMaterial.baseColorFactor = {1.0f, 0.92f, 0.72f, 1.0f};
  foamMaterial.metallicFactor = 0.0f;
  foamMaterial.roughnessFactor = 0.72f;
  foamMaterial.emissiveFactor = {0.18f, 0.12f, 0.055f};
  foamMaterial.rayTracingVisible = false;
  m_foamMeshId = dx.CreateMeshResources(cylinderMesh, {}, foamMaterial);

  Material stewMaterial{};
  stewMaterial.baseColorFactor = {0.42f, 0.14f, 0.045f, 1.0f};
  stewMaterial.metallicFactor = 0.0f;
  stewMaterial.roughnessFactor = 0.58f;
  stewMaterial.emissiveFactor = {0.10f, 0.022f, 0.004f};
  stewMaterial.rayTracingVisible = false;
  m_stewMeshId = dx.CreateMeshResources(cylinderMesh, {}, stewMaterial);

  Material metalMaterial{};
  metalMaterial.baseColorFactor = {0.28f, 0.34f, 0.35f, 1.0f};
  metalMaterial.metallicFactor = 0.75f;
  metalMaterial.roughnessFactor = 0.30f;
  m_metalMeshId = dx.CreateMeshResources(cubeMesh, {}, metalMaterial);

  Material waterMaterial{};
  waterMaterial.baseColorFactor = {0.075f, 0.28f, 0.34f, 1.0f};
  waterMaterial.metallicFactor = 0.0f;
  waterMaterial.roughnessFactor = 0.035f;
  waterMaterial.emissiveFactor = {0.004f, 0.012f, 0.018f};
  waterMaterial.uvTiling = {1.8f, 1.8f};
  waterMaterial.proceduralTypeId = 6.0f;
  waterMaterial.reflectionReceiver = ReflectionReceiver::Water;
  waterMaterial.reflectionStrength = 0.78f;
  waterMaterial.rayTracingVisible = false;
  m_waterMeshId = dx.CreateMeshResources(cylinderMesh, {}, waterMaterial);

  static_assert(kTavernAssetPaths.size() ==
                static_cast<std::size_t>(TavernAsset::Count));
  for (std::vector<uint32_t> &meshIds : m_tavernAssetMeshIds)
    meshIds.clear();
  for (std::size_t assetIndex = 0; assetIndex < kTavernAssetPaths.size();
       ++assetIndex) {
    LoadTavernModelMeshIds(dx, kTavernAssetPaths[assetIndex],
                           m_tavernAssetMeshIds[assetIndex]);
  }

  const auto hasAsset = [this](TavernAsset asset) {
    return !m_tavernAssetMeshIds[static_cast<std::size_t>(asset)].empty();
  };
  m_importedArtReady =
      hasAsset(TavernAsset::Barrel) && hasAsset(TavernAsset::Crate) &&
      hasAsset(TavernAsset::Stool) && hasAsset(TavernAsset::Chest) &&
      hasAsset(TavernAsset::RoundTable) && hasAsset(TavernAsset::Mug) &&
      hasAsset(TavernAsset::Bowl) && hasAsset(TavernAsset::Candle);
  OutputDebugStringA(m_importedArtReady
                         ? "[TavernScene] optimized tavern art set ready\n"
                         : "[TavernScene] optimized art incomplete; procedural "
                           "fallback remains active\n");

  m_customerNpc.Initialize(dx);

  m_collisionColliders = {
      {CollisionSystem::ShapeType::Box,
       {-6.85f, 1.5f, 4.2f},
       {0.30f, 3.0f, 12.0f},
       0.0f,
       true},
      {CollisionSystem::ShapeType::Box,
       {6.85f, 1.5f, 4.2f},
       {0.30f, 3.0f, 12.0f},
       0.0f,
       true},
      {CollisionSystem::ShapeType::Box,
       {0.0f, 1.5f, 9.95f},
       {14.0f, 3.0f, 0.30f},
       0.0f,
       true},
      {CollisionSystem::ShapeType::Box,
       {0.0f, 1.5f, -1.65f},
       {14.0f, 3.0f, 0.22f},
       0.0f,
       true},
      {CollisionSystem::ShapeType::Box,
       {0.0f, 0.65f, 8.0f},
       {8.6f, 1.3f, 1.05f},
       0.0f,
       true},
      {CollisionSystem::ShapeType::Box,
       {-3.8f, 0.75f, 4.15f},
       {2.45f, 1.5f, 2.35f},
       0.0f,
       true},
      {CollisionSystem::ShapeType::Box,
       {0.0f, 0.75f, 4.15f},
       {2.45f, 1.5f, 2.35f},
       0.0f,
       true},
      {CollisionSystem::ShapeType::Box,
       {3.8f, 0.75f, 4.15f},
       {2.45f, 1.5f, 2.35f},
       0.0f,
       true},
      {CollisionSystem::ShapeType::Box,
       {5.35f, 0.59f, 8.18f},
       {2.20f, 1.18f, 1.20f},
       0.0f,
       true},
      {CollisionSystem::ShapeType::Box,
       {kManagementTablePosition.x, 0.55f, kManagementTablePosition.z},
       {1.65f, 1.10f, 1.15f},
       0.0f,
       true},
  };

  m_ready = m_floorMeshId != UINT32_MAX && m_wallMeshId != UINT32_MAX &&
            m_woodMeshId != UINT32_MAX && m_darkWoodMeshId != UINT32_MAX &&
            m_glowMeshId != UINT32_MAX && m_customerBodyMeshId != UINT32_MAX &&
            m_customerHeadMeshId != UINT32_MAX &&
            m_customerHairMeshId != UINT32_MAX && m_mugMeshId != UINT32_MAX &&
            m_filledMugMeshId != UINT32_MAX && m_dirtyMugMeshId != UINT32_MAX &&
            m_dirtySpotMeshId != UINT32_MAX && m_aleMeshId != UINT32_MAX &&
            m_foamMeshId != UINT32_MAX && m_stewMeshId != UINT32_MAX &&
            m_metalMeshId != UINT32_MAX &&
            m_waterMeshId != UINT32_MAX && m_orderBubbleMeshId != UINT32_MAX &&
            m_orderBubbleBorderMeshId != UINT32_MAX;
  Reset();
}

void TavernScene::SetBusinessHour(float hour) {
  m_businessHour = std::fmod(std::max(0.0f, hour), 24.0f);
}

void TavernScene::Reset(float startingHour) {
  m_gold = 0;
  m_servedCustomers = 0;
  m_completedCycles = 0;
  m_completedDays = 0;
  m_tableCompletedCycles = {};
  m_foodServed = 0;
  m_orderSerial = 0;
  m_supplies = {};
  AleSupply() = {kInitialAleStock, kInitialAleCapacity};
  m_supplyPurchaseSerial = 0;
  m_upgradePurchaseSerial = 0;
  m_extraMugLevel = 0;
  m_table3Unlocked = false;
  m_tutorialStep = TutorialStep::TakeOrder;
  m_perfectPours = 0;
  ResetShiftRuntime(startingHour);
}

void TavernScene::ResetShiftRuntime(float startingHour) {
  m_shiftState = ShiftState::Running;
  SetBusinessHour(startingHour);
  m_spawnTimers = {2.0f, 0.0f, 0.0f};
  m_walkouts = 0;
  m_tables = {};
  m_entranceCustomers = {};
  m_counterMugs = {};
  m_tables[0].enabled = true;
  m_tables[1].enabled = m_tutorialStep == TutorialStep::Complete;
  m_tables[2].enabled =
      m_table3Unlocked && m_tutorialStep == TutorialStep::Complete;
  if (m_tables[1].enabled)
    m_spawnTimers[1] = 4.0f;
  if (m_tables[2].enabled)
    m_spawnTimers[2] = CustomerSpawnDelay(2);
  m_heldItem = HeldItem::None;
  m_workState = WorkState::None;
  m_kitchenState = KitchenState::Idle;
  m_managementPage = ManagementPage::Closed;
  m_managementSelection = ManagementSelection::None;
  m_managementActivationSerial = 0;
  m_managementUiDiagnostics = {};
  m_aleOrderQuantity = 0;
  m_selectedUpgradeIndex = 0;
  m_upgradeConfirmationOpen = false;
  m_upgradeConfirmBuySelected = false;
  m_playerPosition = PlayerSpawnPosition();
  m_nearbyPrompt = "WASD：移動　E：調べる";
  m_feedbackText.clear();
  m_aleFill = 0.0f;
  m_aleFoam = 0.0f;
  m_aleOverflow = 0.0f;
  m_pourQuality = 1.0f;
  m_washProgress = 0.0f;
  m_cookTimer = 0.0f;
  m_interactionCooldown = 0.35f;
  m_feedbackTimer = 0.0f;
  m_pourVisualTime = 0.0f;
  m_cleanMugs = TotalMugs();
  m_cleanBowls = kInitialBowlCapacity;
  m_heldDishTableIndex = -1;
  m_tutorialTableIndex = 0;
  m_workActionStarted = false;
  m_cycleAwaitingWash = false;
  m_primaryActionActive = false;
  m_lastPourPerfect = false;
}

void TavernScene::BeginNextDay(float startingHour) {
  ++m_completedDays;
  ResetShiftRuntime(startingHour);
  if (AleStock() == 0 && m_gold < kAleSupplyUnitPrice)
    AleSupply().current = std::min(kEmergencyAleFloor, AleCapacity());
  m_feedbackText = "翌朝 5:00　新しい一日が始まった";
  m_feedbackTimer = 2.4f;
}

void TavernScene::SpawnCustomer(int tableIndex) {
  if (tableIndex < 0 || tableIndex >= static_cast<int>(m_tables.size()))
    return;
  TableSlot &table = m_tables[tableIndex];
  if (!table.enabled || table.state != TableState::Empty)
    return;
  table.state = TableState::Arriving;
  table.stateTimer = CustomerTravelSeconds(tableIndex);
  table.satisfaction = 100.0f;
  table.servedCustomer = false;
  table.complaintPlayed = false;
  table.speech = "すみません！";
  table.speechTimer = 2.0f;
}

void TavernScene::DismissEntranceCustomer(int tableIndex, bool penalize) {
  EntranceCustomer &customer = m_entranceCustomers[tableIndex];
  if (customer.state != EntranceState::Waiting)
    return;
  customer.state = EntranceState::Leaving;
  customer.leaveTimer = kEntranceLeaveSeconds;
  if (penalize) {
    const int penalty = std::min(m_gold, kEntranceTimeoutPenalty);
    m_gold -= penalty;
    ++m_walkouts;
    m_feedbackText = "入口の客が待ちきれず退店：-" + std::to_string(penalty) +
                     " G（上限20 G）";
    m_feedbackTimer = 3.0f;
  }
}

void TavernScene::UpdateEntranceCustomers(float dt) {
  for (int tableIndex = 0; tableIndex < CustomerTableCount(); ++tableIndex) {
    TableSlot &table = m_tables[tableIndex];
    EntranceCustomer &customer = m_entranceCustomers[tableIndex];
    if (!table.enabled) {
      customer = {};
      continue;
    }
    if (customer.state == EntranceState::Leaving) {
      customer.leaveTimer -= dt;
      if (customer.leaveTimer <= 0.0f) {
        customer = {};
        m_spawnTimers[tableIndex] = CustomerSpawnDelay(tableIndex);
      }
      continue;
    }
    if (customer.state == EntranceState::Waiting) {
      if (m_shiftState != ShiftState::Running) {
        DismissEntranceCustomer(tableIndex, false);
      } else if (table.state == TableState::Empty) {
        // 回収したジョッキは手元に残し、空いた卓へ待機客だけを案内する。
        customer = {};
        SpawnCustomer(tableIndex);
      } else if (m_tutorialStep == TutorialStep::Complete) {
        customer.waitedSeconds =
            std::min(kEntranceWaitSeconds, customer.waitedSeconds + dt);
        if (customer.waitedSeconds >= kEntranceWaitSeconds)
          DismissEntranceCustomer(tableIndex, true);
      }
      continue;
    }
    if (m_shiftState != ShiftState::Running ||
        (table.state != TableState::Empty && table.state != TableState::Dirty))
      continue;
    // 研修中も客は入口に現れるが、待機時間は研修完了まで進めない。
    m_spawnTimers[tableIndex] -= dt;
    if (m_spawnTimers[tableIndex] > 0.0f)
      continue;
    if (table.state == TableState::Empty) {
      SpawnCustomer(tableIndex);
    } else {
      customer.state = EntranceState::Waiting;
      customer.waitedSeconds = 0.0f;
      m_feedbackText = "入口でお待ちです：TABLE " +
                       std::to_string(tableIndex + 1) + " のジョッキを回収";
      m_feedbackTimer = 2.4f;
    }
  }
}

void TavernScene::ConfigureEntranceWaitingPreview() {
  Reset(5.0f);
  m_tutorialStep = TutorialStep::Complete;
  m_gold = 50;
  m_cleanMugs = 0;
  for (int tableIndex = 0; tableIndex < kBaseTableCount; ++tableIndex) {
    m_tables[tableIndex].enabled = true;
    m_tables[tableIndex].state = TableState::Dirty;
    m_tables[tableIndex].servedCustomer = true;
    m_spawnTimers[tableIndex] = 0.0f;
  }
  UpdateEntranceCustomers(0.0f);
}

void TavernScene::ConfigureOrderBubblePreview() {
  Reset(5.0f);
  m_tutorialStep = TutorialStep::Complete;
  m_table3Unlocked = true;
  for (int tableIndex = 0; tableIndex < CustomerTableCount(); ++tableIndex) {
    m_tables[tableIndex].enabled = true;
    m_tables[tableIndex].state = TableState::WaitingOrder;
    TakeOrder(tableIndex);
  }
}

void TavernScene::ConfigureKitchenPreview() {
  Reset(12.0f);
  m_tutorialStep = TutorialStep::Complete;
  m_tables = {};
  m_entranceCustomers = {};
  m_tables[0].enabled = true;
  m_tables[0].state = TableState::WaitingFood;
  m_tables[0].order = OrderType::Food;
  m_tables[0].satisfaction = 100.0f;
  m_tables[1].enabled = true;
  m_spawnTimers = {120.0f, 120.0f, 0.0f};
  m_cleanBowls = kInitialBowlCapacity - 1;
  m_kitchenState = KitchenState::Cooking;
  m_cookTimer = 4.0f;
  m_feedbackText = std::string(kFoodName) + "を煮込み中";
  m_feedbackTimer = 2.0f;
}

bool TavernScene::RunOrderBubbleRegression(std::string &failure) const {
  failure.clear();
  const auto fail = [&](const char *reason) {
    failure = reason;
    return false;
  };
  if (!m_ready)
    return fail("Tavern mesh resources are not ready");

  // 実行中の営業を変更せず、実際の接客操作と描画経路を検証する。
  TavernScene probe;
  probe.m_ready = true;
  probe.m_orderBubbleMeshId = m_orderBubbleMeshId;
  probe.m_orderBubbleBorderMeshId = m_orderBubbleBorderMeshId;
  probe.m_filledMugMeshId = m_filledMugMeshId;
  probe.m_mugMeshId = m_mugMeshId;
  probe.m_aleMeshId = m_aleMeshId;
  probe.m_foamMeshId = m_foamMeshId;
  probe.m_stewMeshId = m_stewMeshId;
  probe.m_glowMeshId = m_glowMeshId;
  const auto mugIndex = static_cast<std::size_t>(TavernAsset::Mug);
  const auto bowlIndex = static_cast<std::size_t>(TavernAsset::Bowl);
  probe.m_tavernAssetMeshIds[mugIndex] = m_tavernAssetMeshIds[mugIndex];
  probe.m_tavernAssetMeshIds[bowlIndex] = m_tavernAssetMeshIds[bowlIndex];
  ViewContext view{};
  const auto bubbleCount = [&]() {
    FrameData frame;
    probe.BuildFrame(frame, view);
    return std::count_if(frame.opaqueItems.begin(), frame.opaqueItems.end(),
                         [&](const auto &item) {
                           return item.meshId == m_orderBubbleMeshId;
                         });
  };
  probe.Reset();
  if (bubbleCount() != 0)
    return fail("Unheard orders must not reveal a bubble");
  probe.ConfigureOrderBubblePreview();
  for (TableSlot &table : probe.m_tables) {
    if (!table.speech.empty())
      return fail("Accepted order must not repeat its text in the TABLE card");
    table.speechTimer = 0.0f;
  }
  if (bubbleCount() != 3)
    return fail("All three accepted orders must outlast the speech timer");

  for (const float yaw : {0.0f, XM_PIDIV2, XM_PI}) {
    for (const float pitch : {-1.05f, 0.0f, 1.05f}) {
      view.cameraYaw = yaw;
      view.cameraPitch = pitch;
      FrameData frame;
      probe.BuildFrame(frame, view);
      int bubbles = 0;
      for (size_t i = 0; i < frame.opaqueItems.size(); ++i) {
        const auto &item = frame.opaqueItems[i];
        if (item.meshId != m_orderBubbleMeshId)
          continue;
        const XMVECTOR normal = XMVector3TransformNormal(
            XMVectorSet(0, 0, -1, 0), item.world);
        const XMVECTOR forward = XMVectorSet(std::cos(pitch) * std::sin(yaw),
            std::sin(pitch), std::cos(pitch) * std::cos(yaw), 0);
        XMFLOAT3 anchor;
        XMStoreFloat3(&anchor, item.world.r[3]);
        if (bubbles >= 3 ||
            XMVectorGetX(XMVector3Dot(normal, forward)) > -0.999f ||
            std::abs(anchor.x - kTableInteractions[bubbles].x) > 0.001f ||
            std::abs(anchor.y - 2.18f) > 0.001f)
          return fail("Order bubble detached from its customer or camera");
        const bool foodOrder =
            probe.m_tables[static_cast<std::size_t>(bubbles)].order ==
            OrderType::Food;
        const auto &iconIds = probe.m_tavernAssetMeshIds[
            foodOrder ? bowlIndex : mugIndex];
        const uint32_t iconMesh =
            iconIds.empty() ? (foodOrder ? m_mugMeshId : m_filledMugMeshId)
                            : iconIds.front();
        if (i + 2 >= frame.opaqueItems.size() ||
            frame.opaqueItems[i + 2].meshId != iconMesh)
          return fail("Order bubble is missing its ordered item model");
        ++bubbles;
      }
      if (bubbles != 3)
        return fail("Camera movement changed the number of orders");
    }
  }
  probe.m_tavernAssetMeshIds[mugIndex].clear();
  if (bubbleCount() != 3)
    return fail("Missing imported mug must retain fallback order bubbles");
  probe.m_heldItem = HeldItem::FilledBowl;
  probe.ServeFood(0);
  if (bubbleCount() != 2)
    return fail("Serving must remove only that customer's order bubble");
  probe.TriggerWalkout(1);
  if (bubbleCount() != 1)
    return fail("A walkout must remove only that customer's order bubble");
  probe.m_tables[2].state = TableState::Dirty;
  if (bubbleCount() != 0)
    return fail("Dirty tables must not keep an order bubble");
  probe.ConfigureOrderBubblePreview();
  probe.BeginNextDay();
  if (bubbleCount() != 0)
    return fail("Next day must clear order bubbles");
  probe.ConfigureOrderBubblePreview();
  probe.Reset();
  if (bubbleCount() != 0)
    return fail("New Game must clear order bubbles");
  return true;
}

bool TavernScene::RunKitchenRegression(std::string &failure) {
  TavernScene probe;
  failure.clear();
  const auto fail = [&](const char *reason) {
    failure = reason;
    return false;
  };
  const auto tick = [&](float seconds,
                        const XMFLOAT3 &position = XMFLOAT3{}) {
    while (seconds > 0.0f) {
      const float dt = std::min(0.25f, seconds);
      probe.Update(dt, position, false, false, false);
      seconds -= dt;
    }
  };

  probe.Reset();
  probe.m_tutorialStep = TutorialStep::Complete;
  probe.ResetShiftRuntime(5.0f);
  probe.m_tables[0].state = TableState::WaitingOrder;
  probe.TakeOrder(0);
  if (probe.m_tables[0].state != TableState::WaitingFood ||
      probe.m_tables[0].order != OrderType::Food)
    return fail("最初の通常注文が料理にならない");

  probe.StartCooking();
  if (!probe.KitchenCooking() || probe.CleanBowls() != 1 ||
      probe.PlayerMovementLocked())
    return fail("調理開始時のボウル消費または非ブロッキング契約が不正");
  tick(kCookSeconds * 0.5f, probe.PlayerSpawnPosition());
  const float pausedProgress = probe.KitchenProgress();
  if (!probe.KitchenCooking() || pausedProgress < 0.45f ||
      pausedProgress > 0.55f)
    return fail("厨房を離れた後に調理タイマーが進まない");

  probe.m_interactionCooldown = 0.0f;
  probe.Update(0.0f, kManagementTableInteraction, true, false, false);
  tick(1.0f, kManagementTableInteraction);
  if (!probe.ManagementMenuOpen() ||
      std::abs(probe.KitchenProgress() - pausedProgress) > 0.001f)
    return fail("管理メニュー中に調理タイマーが進んだ");
  probe.Update(0.0f, kManagementTableInteraction, true, false, false);
  tick(kCookSeconds * 0.5f, probe.PlayerSpawnPosition());
  if (!probe.KitchenReady())
    return fail("調理完了後に受取可能にならない");

  probe.m_interactionCooldown = 0.0f;
  probe.Update(0.0f, kKitchenInteraction, true, false, false);
  if (probe.m_heldItem != HeldItem::FilledBowl || probe.KitchenReady())
    return fail("完成料理を厨房から受け取れない");
  probe.Update(0.0f, kTableInteractions[0], true, false, false);
  if (probe.m_tables[0].state != TableState::Eating ||
      probe.m_heldItem != HeldItem::None || probe.FoodServed() != 1 ||
      probe.Gold() != 13)
    return fail("料理の提供、報酬、または提供数が不正");

  tick(5.5f + CustomerTravelSeconds(0) + 0.5f,
       probe.PlayerSpawnPosition());
  if (probe.m_tables[0].state != TableState::Dirty ||
      probe.m_tables[0].order != OrderType::Food)
    return fail("食事後に汚れたボウルが卓へ残らない");
  probe.m_interactionCooldown = 0.0f;
  probe.Update(0.0f, kTableInteractions[0], true, false, false);
  if (probe.m_heldItem != HeldItem::DirtyBowl ||
      probe.m_heldDishTableIndex != 0)
    return fail("汚れたボウルを回収できない");
  probe.Update(0.0f, kWashBasinInteraction, true, false, false);
  for (int i = 0; i < 5; ++i)
    probe.Update(0.25f, kWashBasinInteraction, false, true, false);
  if (probe.m_heldItem != HeldItem::CleanBowl ||
      probe.TableCompletedCycles(0) != 1 || probe.CleanBowls() != 1)
    return fail("ボウル洗浄または料理サイクル完了が不正");
  probe.m_interactionCooldown = 0.0f;
  probe.Update(0.0f, kKitchenInteraction, true, false, false);
  if (probe.m_heldItem != HeldItem::None ||
      probe.CleanBowls() != probe.TotalBowls())
    return fail("洗浄済みボウルを厨房へ戻せない");

  probe.m_tables[0].state = TableState::WaitingOrder;
  probe.m_orderSerial = 0;
  probe.TakeOrder(0);
  probe.StartCooking();
  probe.TriggerWalkout(0);
  tick(kCookSeconds, probe.PlayerSpawnPosition());
  if (!probe.KitchenReady() || probe.m_tables[0].state != TableState::Leaving)
    return fail("退店時に調理品を安全に保持できない");

  probe.Reset();
  if (probe.KitchenCooking() || probe.KitchenReady() ||
      probe.CleanBowls() != probe.TotalBowls() || probe.FoodServed() != 0 ||
      probe.m_heldItem != HeldItem::None)
    return fail("New Game後に厨房状態またはボウルが残った");
  return true;
}

bool TavernScene::RunEntranceWaitingRegression(std::string &failure) {
  // 実際のUpdateと操作経路を使い、実行中の営業データには触れない。
  TavernScene probe;
  failure.clear();
  const auto fail = [&](const char *reason) {
    failure = reason;
    return false;
  };
  const auto setup = [&]() {
    probe.Reset(5.0f);
    probe.m_tutorialStep = TutorialStep::Complete;
    probe.m_gold = 50;
    probe.m_cleanMugs = probe.TotalMugs() - 1;
    probe.m_tables[0].state = TableState::Dirty;
    probe.m_tables[0].servedCustomer = true;
    probe.m_spawnTimers[0] = 0.0f;
  };
  const auto tick = [&](float seconds) {
    Action action = Action::None;
    while (seconds > 0.0f) {
      const float dt = std::min(seconds, 0.25f);
      action = probe.Update(dt, {0.0f, 0.0f, 1.3f}, false, false, false);
      seconds -= dt;
    }
    return action;
  };
  const auto mugCount = [&]() {
    int count = probe.m_cleanMugs + (probe.m_heldItem != HeldItem::None ? 1 : 0);
    for (const MugState &mug : probe.m_counterMugs)
      count += mug.item != HeldItem::None ? 1 : 0;
    for (const TableSlot &table : probe.m_tables)
      count += table.state == TableState::Dirty || table.state == TableState::Eating ||
                       (table.state == TableState::Leaving && table.servedCustomer)
                   ? 1 : 0;
    return count;
  };

  setup();
  probe.m_tables[0].state = TableState::Eating;
  probe.m_tables[0].stateTimer = 0.25f;
  tick(0.25f + CustomerTravelSeconds(0) + probe.CustomerSpawnDelay(0) + 0.25f);
  if (probe.m_tables[0].state != TableState::Dirty ||
      probe.m_entranceCustomers[0].state != EntranceState::Waiting ||
      mugCount() != probe.TotalMugs())
    return fail("食事と退店の後、汚れた卓への来客が開始しない");

  setup();
  tick(1.0f);
  if (probe.m_entranceCustomers[0].state != EntranceState::Waiting ||
      probe.m_tables[0].state != TableState::Dirty || mugCount() != probe.TotalMugs() ||
      probe.m_entranceCustomers[1].state != EntranceState::None ||
      probe.m_entranceCustomers[2].state != EntranceState::None)
    return fail("汚れた卓の入口待機、未開放卓、またはジョッキ数が不正");

  probe.Update(0.0f, kManagementTableInteraction, true, false, false);
  const float waited = probe.m_entranceCustomers[0].waitedSeconds;
  if (!probe.ManagementMenuOpen())
    return fail("管理台の操作でメニューが開かない");
  tick(40.0f);
  if (probe.m_entranceCustomers[0].waitedSeconds != waited || probe.Gold() != 50)
    return fail("管理メニュー中に入店待ち時間または罰金が進んだ");
  probe.Update(0.0f, kManagementTableInteraction, true, false, false);
  probe.Update(0.25f, kTableInteractions[0], true, false, false);
  tick(0.25f);
  if (probe.m_entranceCustomers[0].state != EntranceState::None ||
      probe.m_tables[0].state != TableState::Arriving ||
      probe.m_heldItem != HeldItem::DirtyMug || probe.m_heldDishTableIndex != 0 ||
      mugCount() != probe.TotalMugs() || probe.Gold() != 50 || probe.Walkouts() != 0)
    return fail("回収後の案内でジョッキが失われた、または不要な罰金が発生");
  tick(CustomerTravelSeconds(0) + 0.25f);
  if (probe.m_tables[0].state != TableState::WaitingOrder)
    return fail("入口からの移動後に注文可能な状態にならない");
  probe.Update(0.25f, kWashBasinInteraction, true, false, false);
  for (int i = 0; i < 5; ++i)
    probe.Update(0.25f, kWashBasinInteraction, false, true, false);
  if (probe.m_heldItem != HeldItem::EmptyMug ||
      probe.TableCompletedCycles(0) != 1 || mugCount() != probe.TotalMugs())
    return fail("待機客を案内した後の洗浄サイクルが壊れた");

  setup();
  probe.m_spawnTimers[0] = 4.0f;
  tick(1.0f);
  probe.CollectDirtyDish(0);
  if (std::abs(probe.m_spawnTimers[0] - 3.0f) > 0.001f)
    return fail("早めの回収で来客タイマーが再設定された");
  tick(3.0f);
  if (probe.m_tables[0].state != TableState::Arriving)
    return fail("清掃済みの卓へ来客しない");

  setup();
  tick(kEntranceWaitSeconds + 0.25f);
  if (probe.m_entranceCustomers[0].state != EntranceState::Leaving ||
      probe.Gold() != 30 || probe.Walkouts() != 1 ||
      probe.m_tables[0].state != TableState::Dirty || mugCount() != probe.TotalMugs())
    return fail("待機満了時の20 G損失、退店、または汚れたジョッキの保持が不正");
  tick(0.5f);
  if (probe.Gold() != 30 || probe.Walkouts() != 1)
    return fail("同じ待機客の罰金が重複した");
  tick(kEntranceLeaveSeconds + probe.CustomerSpawnDelay(0));
  if (probe.m_entranceCustomers[0].state != EntranceState::Waiting ||
      probe.Gold() != 30 || probe.Walkouts() != 1)
    return fail("退店後、汚れた卓への次の来客が止まった");

  setup();
  probe.m_gold = 5;
  probe.AleSupply().current = 0;
  tick(kEntranceWaitSeconds + 0.25f);
  if (probe.Gold() != 0 || probe.Walkouts() != 1)
    return fail("残高不足時の罰金が負債または二重計上になった");
  probe.BeginNextDay();
  if (probe.m_entranceCustomers[0].state != EntranceState::None ||
      probe.AleStock() != kEmergencyAleFloor || probe.Walkouts() != 0 ||
      mugCount() != probe.TotalMugs())
    return fail("翌朝の入店待ちリセットまたは資金不足救済が不正");

  setup();
  probe.m_gold = 0;
  tick(kEntranceWaitSeconds + 0.25f);
  if (probe.Gold() != 0 || probe.Walkouts() != 1)
    return fail("残高0で罰金処理が壊れた");

  setup();
  tick(1.0f);
  probe.SetBusinessHour(0.5f);
  probe.Update(0.25f, {0.0f, 0.0f, 1.3f}, false, false, true);
  if (tick(2.0f) != Action::SleepUntilMorning || probe.Gold() != 50 ||
      probe.Walkouts() != 0 || probe.m_tables[0].state != TableState::Dirty)
    return fail("閉店時の待機客が就寝を妨げた、または罰金が発生した");

  setup();
  probe.m_tutorialStep = TutorialStep::CollectMug;
  tick(60.0f);
  if (probe.m_entranceCustomers[0].state != EntranceState::Waiting ||
      probe.m_entranceCustomers[0].waitedSeconds != 0.0f || probe.Gold() != 50)
    return fail("初回研修中に入口客が消えた、または待機時間が進んだ");
  probe.m_tutorialStep = TutorialStep::Complete;
  tick(kEntranceWaitSeconds);
  if (probe.Gold() != 30 || probe.Walkouts() != 1)
    return fail("研修完了後に待機時間が再開しない");

  setup();
  probe.m_table3Unlocked = true;
  probe.m_extraMugLevel = 1;
  probe.m_cleanMugs = 0;
  for (int i = 0; i < 3; ++i) {
    probe.m_tables[i].enabled = true;
    probe.m_tables[i].state = TableState::Dirty;
    probe.m_spawnTimers[i] = 0.0f;
  }
  tick(1.0f);
  probe.Update(0.25f, kTableInteractions[2], true, false, false);
  tick(0.25f);
  if (probe.m_entranceCustomers[0].state != EntranceState::Waiting ||
      probe.m_entranceCustomers[1].state != EntranceState::Waiting ||
      probe.m_entranceCustomers[2].state != EntranceState::None ||
      probe.m_tables[2].state != TableState::Arriving || mugCount() != 3)
    return fail("三卓目の回収が別の待機客またはジョッキを変更した");
  probe.Reset();
  for (const EntranceCustomer &customer : probe.m_entranceCustomers)
    if (customer.state != EntranceState::None)
      return fail("New Game後に待機客が残った");
  return true;
}

void TavernScene::TakeOrder(int tableIndex) {
  TableSlot &table = m_tables[tableIndex];
  if (CanServeCustomers() && table.state == TableState::WaitingOrder) {
    table.order = m_tutorialStep == TutorialStep::Complete &&
                          (m_orderSerial++ % 2) == 0
                      ? OrderType::Food
                      : OrderType::Ale;
    table.state = table.order == OrderType::Food ? TableState::WaitingFood
                                                 : TableState::WaitingAle;
    // 注文内容は頭上の模型で表示し、苦情などの台詞は従来のHUDに残す。
    table.speech.clear();
    table.speechTimer = 0.0f;
    m_feedbackText = std::string("注文：") + GetOrderName(table.order);
    m_feedbackTimer = 1.6f;
    if (m_tutorialStep == TutorialStep::TakeOrder) {
      m_tutorialTableIndex = tableIndex;
      m_tutorialStep = TutorialStep::GetMug;
    }
  }
}

void TavernScene::PickUpEmptyMug() {
  if (m_heldItem == HeldItem::None && m_cleanMugs > 0) {
    m_heldItem = HeldItem::EmptyMug;
    --m_cleanMugs;
    if (m_tutorialStep == TutorialStep::GetMug)
      m_tutorialStep = TutorialStep::StartPour;
  }
}

void TavernScene::ReturnEmptyMug() {
  if (m_heldItem != HeldItem::EmptyMug)
    return;
  m_heldItem = HeldItem::None;
  m_cleanMugs = std::min(TotalMugs(), m_cleanMugs + 1);
}

void TavernScene::PlaceHeldMug(int slotIndex) {
  if (slotIndex < 0 || slotIndex >= static_cast<int>(m_counterMugs.size()) ||
      !IsMugItem(m_heldItem) ||
      m_counterMugs[slotIndex].item != HeldItem::None)
    return;

  MugState &mug = m_counterMugs[slotIndex];
  mug.item = m_heldItem;
  mug.aleFill = m_aleFill;
  mug.aleFoam = m_aleFoam;
  mug.pourQuality = m_pourQuality;
  mug.sourceTableIndex = m_heldDishTableIndex;
  mug.cycleAwaitingWash = m_cycleAwaitingWash;
  mug.lastPourPerfect = m_lastPourPerfect;

  m_heldItem = HeldItem::None;
  m_aleFill = 0.0f;
  m_aleFoam = 0.0f;
  m_pourQuality = 1.0f;
  m_heldDishTableIndex = -1;
  m_cycleAwaitingWash = false;
  m_lastPourPerfect = false;
  m_feedbackText = "ジョッキをカウンターに置いた";
  m_feedbackTimer = 1.2f;
}

void TavernScene::PickUpPlacedMug(int slotIndex) {
  if (slotIndex < 0 || slotIndex >= static_cast<int>(m_counterMugs.size()) ||
      m_heldItem != HeldItem::None ||
      m_counterMugs[slotIndex].item == HeldItem::None)
    return;

  MugState &mug = m_counterMugs[slotIndex];
  m_heldItem = mug.item;
  m_aleFill = mug.aleFill;
  m_aleFoam = mug.aleFoam;
  m_pourQuality = mug.pourQuality;
  m_heldDishTableIndex = mug.sourceTableIndex;
  m_cycleAwaitingWash = mug.cycleAwaitingWash;
  m_lastPourPerfect = mug.lastPourPerfect;
  mug = {};
  m_feedbackText = "カウンターのジョッキを取った";
  m_feedbackTimer = 1.2f;
}

void TavernScene::BeginPouring() {
  if (m_heldItem != HeldItem::EmptyMug || m_workState != WorkState::None)
    return;
  if (AleStock() <= 0) {
    m_feedbackText = "ALE STOCK EMPTY";
    m_feedbackTimer = 1.8f;
    return;
  }
  m_workState = WorkState::PouringAle;
  m_aleFill = 0.0f;
  m_aleFoam = 0.0f;
  m_aleOverflow = 0.0f;
  m_pourQuality = 0.0f;
  m_workActionStarted = false;
  if (m_tutorialStep == TutorialStep::StartPour)
    m_tutorialStep = TutorialStep::PourAle;
}

void TavernScene::FinishPouring() {
  if (m_workState != WorkState::PouringAle)
    return;
  if (AleStock() <= 0) {
    m_workState = WorkState::None;
    m_workActionStarted = false;
    m_feedbackText = "ALE STOCK EMPTY";
    m_feedbackTimer = 1.8f;
    return;
  }

  const float targetError = std::abs(m_aleFill - 0.90f);
  m_pourQuality = std::clamp(1.0f - targetError * 1.65f - m_aleFoam * 0.18f -
                                 m_aleOverflow * 3.0f,
                             0.20f, 1.0f);
  m_lastPourPerfect = m_aleFill >= kPerfectPourMinimum &&
                      m_aleFill <= kPerfectPourMaximum &&
                      m_aleOverflow <= 0.001f;
  if (m_lastPourPerfect) {
    ++m_perfectPours;
    m_feedbackText = "PERFECT POUR!";
  } else if (m_aleOverflow > 0.001f) {
    m_feedbackText = "OVERFLOW";
  } else if (m_aleFill < kPerfectPourMinimum) {
    m_feedbackText = "もう少し注げそうだ";
  } else {
    m_feedbackText = "泡が多すぎる";
  }
  m_feedbackTimer = 1.4f;
  --AleSupply().current;
  m_heldItem = HeldItem::FilledMug;
  m_workState = WorkState::None;
  m_workActionStarted = false;
  if (m_tutorialStep == TutorialStep::PourAle)
    m_tutorialStep = TutorialStep::ServeAle;
}

void TavernScene::ServeAle(int tableIndex) {
  TableSlot &table = m_tables[tableIndex];
  if (!CanServeCustomers() || table.state != TableState::WaitingAle ||
      m_heldItem != HeldItem::FilledMug)
    return;

  constexpr int baseGold = 5;
  const int pourBonus =
      m_lastPourPerfect ? 5 : (m_pourQuality >= 0.70f ? 2 : 0);
  const int patienceBonus =
      table.satisfaction >= 80.0f ? 3 : (table.satisfaction >= 50.0f ? 1 : 0);
  const int earnedGold = baseGold + pourBonus + patienceBonus;
  m_gold += earnedGold;
  ++m_servedCustomers;
  table.state = TableState::Eating;
  table.stateTimer = 4.0f;
  table.servedCustomer = true;
  table.speech =
      m_lastPourPerfect
          ? "最高の一杯だ！"
          : (m_pourQuality >= 0.70f ? "ありがとう！" : "少し物足りないな…");
  table.speechTimer = 2.8f;
  m_heldItem = HeldItem::None;
  if (m_tutorialStep == TutorialStep::ServeAle) {
    m_tutorialTableIndex = tableIndex;
    m_tutorialStep = TutorialStep::CollectMug;
  }
  char feedback[128]{};
  std::snprintf(feedback, sizeof(feedback), "+%d G  基本%d + 注ぎ%d + 待ち%d",
                earnedGold, baseGold, pourBonus, patienceBonus);
  m_feedbackText = feedback;
  m_feedbackTimer = 1.8f;
}

void TavernScene::StartCooking() {
  if (m_kitchenState != KitchenState::Idle ||
      FindMostUrgentWaitingFoodTable() < 0)
    return;

  if (m_heldItem == HeldItem::CleanBowl) {
    m_heldItem = HeldItem::None;
  } else if (m_heldItem == HeldItem::None && m_cleanBowls > 0) {
    --m_cleanBowls;
  } else {
    m_feedbackText = "空のボウルが必要";
    m_feedbackTimer = 1.6f;
    return;
  }

  m_kitchenState = KitchenState::Cooking;
  m_cookTimer = kCookSeconds;
  m_feedbackText = std::string(kFoodName) + "を煮込み始めた";
  m_feedbackTimer = 1.8f;
}

void TavernScene::CollectCookedFood() {
  if (m_kitchenState != KitchenState::Ready || m_heldItem != HeldItem::None)
    return;
  m_kitchenState = KitchenState::Idle;
  m_cookTimer = 0.0f;
  m_heldItem = HeldItem::FilledBowl;
  m_feedbackText = std::string(kFoodName) + "を受け取った";
  m_feedbackTimer = 1.4f;
}

void TavernScene::ReturnCleanBowl() {
  if (m_heldItem != HeldItem::CleanBowl)
    return;
  m_heldItem = HeldItem::None;
  m_cleanBowls = std::min(kInitialBowlCapacity, m_cleanBowls + 1);
  m_feedbackText = "ボウルを厨房へ戻した";
  m_feedbackTimer = 1.2f;
}

void TavernScene::ServeFood(int tableIndex) {
  TableSlot &table = m_tables[tableIndex];
  if (!CanServeCustomers() || table.state != TableState::WaitingFood ||
      m_heldItem != HeldItem::FilledBowl)
    return;

  constexpr int baseGold = 9;
  const int patienceBonus =
      table.satisfaction >= 80.0f ? 4 : (table.satisfaction >= 50.0f ? 2 : 0);
  const int earnedGold = baseGold + patienceBonus;
  m_gold += earnedGold;
  ++m_servedCustomers;
  ++m_foodServed;
  table.state = TableState::Eating;
  table.stateTimer = 5.5f;
  table.servedCustomer = true;
  table.speech = table.satisfaction >= 50.0f ? "温かくてうまい！"
                                             : "待ったかいがあったよ";
  table.speechTimer = 2.8f;
  m_heldItem = HeldItem::None;
  m_feedbackText = "+" + std::to_string(earnedGold) +
                   " G  料理9 + 待ち" + std::to_string(patienceBonus);
  m_feedbackTimer = 1.8f;
}

void TavernScene::CollectDirtyDish(int tableIndex) {
  TableSlot &table = m_tables[tableIndex];
  if (table.state != TableState::Dirty || m_heldItem != HeldItem::None)
    return;
  m_heldItem = table.order == OrderType::Food ? HeldItem::DirtyBowl
                                              : HeldItem::DirtyMug;
  m_heldDishTableIndex = tableIndex;
  table = {};
  table.enabled = true;
  // 来客計時はDirtyへ遷移した時点から継続する。回収で待ち直させない。
  m_cycleAwaitingWash = true;
  if (m_tutorialStep == TutorialStep::CollectMug) {
    m_tutorialTableIndex = tableIndex;
    m_tutorialStep = TutorialStep::WashMug;
  }
}

void TavernScene::BeginWashing() {
  if ((m_heldItem != HeldItem::DirtyMug &&
       m_heldItem != HeldItem::DirtyBowl) ||
      m_workState != WorkState::None)
    return;
  m_workState = WorkState::WashingDish;
  m_washProgress = 0.0f;
  m_workActionStarted = false;
}

void TavernScene::FinishWashing() {
  if (m_workState != WorkState::WashingDish)
    return;
  m_workState = WorkState::None;
  m_heldItem = m_heldItem == HeldItem::DirtyBowl ? HeldItem::CleanBowl
                                                 : HeldItem::EmptyMug;
  m_washProgress = 1.0f;
  m_workActionStarted = false;
  if (m_cycleAwaitingWash) {
    ++m_completedCycles;
    if (m_heldDishTableIndex >= 0 &&
        m_heldDishTableIndex < static_cast<int>(m_tableCompletedCycles.size()))
      ++m_tableCompletedCycles[m_heldDishTableIndex];
    m_heldDishTableIndex = -1;
    m_cycleAwaitingWash = false;
  }
  if (m_tutorialStep == TutorialStep::WashMug) {
    m_tutorialStep = TutorialStep::Complete;
    m_feedbackText = "TUTORIAL COMPLETE!";
    m_feedbackTimer = 2.4f;
    if (!m_tables[1].enabled && m_shiftState == ShiftState::Running) {
      m_tables[1].enabled = true;
      m_spawnTimers[1] = 4.0f;
    }
  }
}

void TavernScene::TriggerWalkout(int tableIndex) {
  TableSlot &table = m_tables[tableIndex];
  table.state = TableState::Leaving;
  table.stateTimer = CustomerTravelSeconds(tableIndex);
  table.servedCustomer = false;
  table.speech = "もう待てない！";
  table.speechTimer = 1.4f;
  ++m_walkouts;
}

int TavernScene::NearestEnabledTable(float maximumDistance) const {
  int nearestTable = -1;
  float nearestDistance = maximumDistance;
  for (int tableIndex = 0; tableIndex < CustomerTableCount(); ++tableIndex) {
    if (!m_tables[tableIndex].enabled)
      continue;
    const float distance =
        DistanceXZ(m_playerPosition, kTableInteractions[tableIndex]);
    if (distance <= nearestDistance) {
      nearestDistance = distance;
      nearestTable = tableIndex;
    }
  }
  return nearestTable;
}

bool TavernScene::ManagementInteractionHasPriority() const {
  const float managementDistance =
      DistanceXZ(m_playerPosition, kManagementTableInteraction);
  if (managementDistance > kManagementInteractionRange)
    return false;

  const int nearbyTable = NearestEnabledTable(kTableInteractionRange);
  return nearbyTable < 0 ||
         managementDistance <
             DistanceXZ(m_playerPosition, kTableInteractions[nearbyTable]);
}

int TavernScene::FindTableInState(TableState state) const {
  for (int tableIndex = 0; tableIndex < CustomerTableCount(); ++tableIndex) {
    if (m_tables[tableIndex].enabled && m_tables[tableIndex].state == state)
      return tableIndex;
  }
  return -1;
}

int TavernScene::FindMostUrgentWaitingAleTable() const {
  int urgentTable = -1;
  float lowestSatisfaction = 101.0f;
  for (int tableIndex = 0; tableIndex < CustomerTableCount(); ++tableIndex) {
    const TableSlot &table = m_tables[tableIndex];
    if (table.enabled && table.state == TableState::WaitingAle &&
        table.satisfaction < lowestSatisfaction) {
      lowestSatisfaction = table.satisfaction;
      urgentTable = tableIndex;
    }
  }
  return urgentTable;
}

int TavernScene::FindMostUrgentWaitingFoodTable() const {
  int urgentTable = -1;
  float lowestSatisfaction = 101.0f;
  for (int tableIndex = 0; tableIndex < CustomerTableCount(); ++tableIndex) {
    const TableSlot &table = m_tables[tableIndex];
    if (table.enabled && table.state == TableState::WaitingFood &&
        table.satisfaction < lowestSatisfaction) {
      lowestSatisfaction = table.satisfaction;
      urgentTable = tableIndex;
    }
  }
  return urgentTable;
}

int TavernScene::NearestCounterMugSlot(float maximumDistance) const {
  int nearestSlot = -1;
  float nearestDistance = maximumDistance;
  for (int slotIndex = 0;
       slotIndex < static_cast<int>(kCounterMugInteractions.size());
       ++slotIndex) {
    const float distance =
        DistanceXZ(m_playerPosition, kCounterMugInteractions[slotIndex]);
    if (distance <= nearestDistance) {
      nearestDistance = distance;
      nearestSlot = slotIndex;
    }
  }
  return nearestSlot;
}

bool TavernScene::HasActiveCustomers() const {
  for (int tableIndex = 0; tableIndex < CustomerTableCount(); ++tableIndex) {
    const TableSlot &table = m_tables[tableIndex];
    if (!table.enabled)
      continue;
    if (m_entranceCustomers[tableIndex].state != EntranceState::None)
      return true;
    if (table.state != TableState::Empty && table.state != TableState::Dirty)
      return true;
  }
  return false;
}

bool TavernScene::IsAfterMidnight() const {
  return m_businessHour < kBusinessOpenHour;
}

float TavernScene::CustomerSpawnDelay(int tableIndex) const {
  float delay = 7.0f;
  if (m_businessHour >= 5.0f && m_businessHour < 12.0f)
    delay = 3.5f;
  else if (m_businessHour >= 12.0f && m_businessHour < 18.0f)
    delay = 5.0f;
  else if (m_businessHour >= 18.0f)
    delay = 2.5f;
  return delay + static_cast<float>(tableIndex) * 1.15f;
}

const char *TavernScene::CustomerTrafficName() const {
  if (m_shiftState == ShiftState::Closing)
    return "就寝準備";
  if (m_businessHour < 5.0f)
    return "静かな深夜";
  if (m_businessHour < 12.0f)
    return "朝の客足";
  if (m_businessHour < 18.0f)
    return "穏やかな午後";
  return "夜のピーク";
}

bool TavernScene::CanServeCustomers() const {
  return m_shiftState == ShiftState::Running ||
         m_shiftState == ShiftState::Closing;
}

uint32_t TavernScene::MugMeshForState(HeldItem item) const {
  const std::vector<uint32_t> &importedMug =
      m_tavernAssetMeshIds[static_cast<std::size_t>(TavernAsset::Mug)];
  if (!importedMug.empty())
    return importedMug.front();

  switch (item) {
  case HeldItem::FilledMug:
    return m_filledMugMeshId;
  case HeldItem::DirtyMug:
    return m_dirtyMugMeshId;
  case HeldItem::None:
  case HeldItem::EmptyMug:
  case HeldItem::CleanBowl:
  case HeldItem::FilledBowl:
  case HeldItem::DirtyBowl:
    return m_mugMeshId;
  }
  return m_mugMeshId;
}

void TavernScene::RunAutomation() {
  if (!CanServeCustomers() || m_workState != WorkState::None)
    return;

  const int orderTable = FindTableInState(TableState::WaitingOrder);
  if (orderTable >= 0) {
    TakeOrder(orderTable);
    return;
  }

  const int aleTable = FindMostUrgentWaitingAleTable();
  const int foodTable = FindMostUrgentWaitingFoodTable();
  if (aleTable >= 0 && AleStock() <= 0) {
    const int affordableQuantity = m_gold / kAleSupplyUnitPrice;
    m_aleOrderQuantity =
        std::min(MaximumAleOrderQuantity(), affordableQuantity);
    TryOrderAle();
  }
  if (m_heldItem == HeldItem::FilledMug) {
    if (aleTable >= 0)
      ServeAle(aleTable);
    return;
  }
  if (m_heldItem == HeldItem::FilledBowl) {
    if (foodTable >= 0)
      ServeFood(foodTable);
    return;
  }
  if (m_heldItem == HeldItem::DirtyMug ||
      m_heldItem == HeldItem::DirtyBowl) {
    BeginWashing();
    return;
  }
  if (m_heldItem == HeldItem::EmptyMug) {
    if (aleTable >= 0)
      BeginPouring();
    else
      ReturnEmptyMug();
    return;
  }
  if (m_heldItem == HeldItem::CleanBowl) {
    if (foodTable >= 0 && m_kitchenState == KitchenState::Idle)
      StartCooking();
    else
      ReturnCleanBowl();
    return;
  }
  if (foodTable >= 0 && m_kitchenState == KitchenState::Ready) {
    CollectCookedFood();
    return;
  }
  if (foodTable >= 0 && m_kitchenState == KitchenState::Idle &&
      m_cleanBowls > 0) {
    StartCooking();
    return;
  }
  if (aleTable >= 0 && m_cleanMugs > 0) {
    PickUpEmptyMug();
    return;
  }

  const int dirtyTable = FindTableInState(TableState::Dirty);
  if (dirtyTable >= 0)
    CollectDirtyDish(dirtyTable);
}

void TavernScene::UpdateNearbyPrompt() {
  if (ManagementMenuOpen()) {
    if (m_managementPage == ManagementPage::Upgrades &&
        m_upgradeConfirmationOpen)
      m_nearbyPrompt = "B：購入確認を戻る　E：管理メニューを閉じる";
    else if (m_managementPage == ManagementPage::Supplies ||
             m_managementPage == ManagementPage::Upgrades)
      m_nearbyPrompt = "B：管理項目へ戻る　E：管理メニューを閉じる";
    else
      m_nearbyPrompt = "E / B：管理メニューを閉じる";
    return;
  }
  if (m_workState == WorkState::PouringAle) {
    m_nearbyPrompt = "左クリック長押し：注ぐ　ちょうど良い量で離す";
    return;
  }
  if (m_workState == WorkState::WashingDish) {
    m_nearbyPrompt = IsBowlItem(m_heldItem)
                         ? "左クリック長押し：ボウルを洗う"
                         : "左クリック長押し：ジョッキを洗う";
    return;
  }
  if (m_shiftState == ShiftState::Complete) {
    m_nearbyPrompt = "営業終了　R：眠って翌朝 5:00 へ　入口で E：外へ戻る";
    return;
  }
  if (m_shiftState == ShiftState::Failed) {
    m_nearbyPrompt = "営業失敗　R：再挑戦　入口で E：外へ戻る";
    return;
  }

  const int nearbyTable = NearestEnabledTable(kTableInteractionRange);
  if (DistanceXZ(m_playerPosition, kExitInteraction) <=
      kStationInteractionRange) {
    m_nearbyPrompt = "E：外へ戻る";
  } else if (ManagementInteractionHasPriority()) {
    m_nearbyPrompt = "E / A：管理台帳を開く";
  } else if (nearbyTable >= 0) {
    const TableSlot &table = m_tables[nearbyTable];
    const std::string tableName = "TABLE " + std::to_string(nearbyTable + 1);
    if (table.state == TableState::WaitingOrder)
      m_nearbyPrompt = "E：" + tableName + " の注文を聞く";
    else if (table.state == TableState::WaitingAle &&
             m_heldItem == HeldItem::FilledMug)
      m_nearbyPrompt = "E：" + tableName + " に水鏡エールを渡す";
    else if (table.state == TableState::WaitingFood &&
             m_heldItem == HeldItem::FilledBowl)
      m_nearbyPrompt = "E：" + tableName + " に月影シチューを渡す";
    else if (table.state == TableState::Dirty && m_heldItem == HeldItem::None)
      m_nearbyPrompt = "E：" + tableName + " の食器を回収";
    else if (table.state == TableState::WaitingAle)
      m_nearbyPrompt = tableName + "　注文：水鏡エール";
    else if (table.state == TableState::WaitingFood)
      m_nearbyPrompt = tableName + "　注文：月影シチュー";
    else
      m_nearbyPrompt =
          tableName + "：" + std::string(GetTableStateName(table.state));
  } else if (const int counterSlot =
                 NearestCounterMugSlot(kCounterMugInteractionRange);
             counterSlot >= 0) {
    const MugState &mug = m_counterMugs[counterSlot];
    if (m_heldItem == HeldItem::None && mug.item != HeldItem::None)
      m_nearbyPrompt = "E：置いたジョッキを取る";
    else if (IsMugItem(m_heldItem) && mug.item == HeldItem::None)
      m_nearbyPrompt = "E：持っているジョッキを置く";
    else if (IsBowlItem(m_heldItem))
      m_nearbyPrompt = "ここにはジョッキだけ置ける";
    else if (m_heldItem == HeldItem::None)
      m_nearbyPrompt = "カウンター：ジョッキを一時置きできる";
    else
      m_nearbyPrompt = "この場所にはジョッキが置かれている";
  } else if (DistanceXZ(m_playerPosition, kMugRackInteraction) <=
             kStationInteractionRange) {
    if (m_heldItem == HeldItem::None && m_cleanMugs > 0)
      m_nearbyPrompt = "E：空のジョッキを取る";
    else if (m_heldItem == HeldItem::None)
      m_nearbyPrompt = "空のジョッキがない：使用後に洗う";
    else if (m_heldItem == HeldItem::EmptyMug)
      m_nearbyPrompt = "E：ジョッキを棚へ戻す";
    else
      m_nearbyPrompt = "ジョッキ棚";
  } else if (DistanceXZ(m_playerPosition, kAleTapInteraction) <=
             kStationInteractionRange) {
    if (m_heldItem == HeldItem::EmptyMug && AleStock() <= 0)
      m_nearbyPrompt =
          "ALE 0 / " + std::to_string(AleCapacity()) + "：管理台で補給する";
    else
      m_nearbyPrompt = m_heldItem == HeldItem::EmptyMug
                           ? "E：ジョッキを置いてエールを注ぐ"
                           : "エール樽：空のジョッキが必要";
  } else if (DistanceXZ(m_playerPosition, kKitchenInteraction) <=
             kStationInteractionRange) {
    if (m_kitchenState == KitchenState::Cooking) {
      const int percent = static_cast<int>(KitchenProgress() * 100.0f);
      m_nearbyPrompt = "煮込み中 " + std::to_string(percent) +
                       "%　その場を離れても調理は続く";
    } else if (m_kitchenState == KitchenState::Ready &&
               m_heldItem == HeldItem::None) {
      m_nearbyPrompt = "E：完成した月影シチューを受け取る";
    } else if (m_kitchenState == KitchenState::Ready) {
      m_nearbyPrompt = "月影シチュー完成：手を空けて受け取る";
    } else if (m_heldItem == HeldItem::CleanBowl &&
               FindMostUrgentWaitingFoodTable() >= 0) {
      m_nearbyPrompt = "E：ボウルを使って月影シチューを作る";
    } else if (m_heldItem == HeldItem::CleanBowl) {
      m_nearbyPrompt = "E：空のボウルを厨房へ戻す";
    } else if (m_heldItem == HeldItem::None &&
               FindMostUrgentWaitingFoodTable() >= 0 && m_cleanBowls > 0) {
      m_nearbyPrompt = "E：月影シチューを作る（6秒）";
    } else if (FindMostUrgentWaitingFoodTable() >= 0 && m_cleanBowls <= 0) {
      m_nearbyPrompt = "空のボウルがない：使用済みを回収して洗う";
    } else {
      m_nearbyPrompt = "厨房：料理の注文を待っている";
    }
  } else if (DistanceXZ(m_playerPosition, kWashBasinInteraction) <=
             kStationInteractionRange) {
    if (m_heldItem == HeldItem::DirtyMug)
      m_nearbyPrompt = "E：ジョッキを洗い始める";
    else if (m_heldItem == HeldItem::DirtyBowl)
      m_nearbyPrompt = "E：ボウルを洗い始める";
    else if (m_heldItem == HeldItem::FilledMug)
      m_nearbyPrompt = "E：エールを捨てる";
    else if (m_heldItem == HeldItem::FilledBowl)
      m_nearbyPrompt = "E：料理を捨ててボウルを洗う";
    else
      m_nearbyPrompt = "洗い場：汚れた食器を持ってくる";
  } else if (const int orderTable = FindTableInState(TableState::WaitingOrder);
             orderTable >= 0) {
    m_nearbyPrompt =
        "TABLE " + std::to_string(orderTable + 1) + " の客に注文を聞きに行く";
  } else if (FindMostUrgentWaitingAleTable() >= 0 &&
             m_heldItem == HeldItem::None && m_cleanMugs > 0) {
    m_nearbyPrompt = "カウンター左の棚からジョッキを取る";
  } else if (FindMostUrgentWaitingAleTable() >= 0 &&
             m_heldItem == HeldItem::None) {
    m_nearbyPrompt = "使用済みジョッキを回収して洗う";
  } else if (FindMostUrgentWaitingFoodTable() >= 0 &&
             m_kitchenState == KitchenState::Ready &&
             m_heldItem == HeldItem::None) {
    m_nearbyPrompt = "右奥の厨房から月影シチューを受け取る";
  } else if (FindMostUrgentWaitingFoodTable() >= 0 &&
             m_kitchenState == KitchenState::Idle &&
             m_heldItem == HeldItem::None) {
    m_nearbyPrompt = "右奥の厨房で月影シチューを作る";
  } else if (m_heldItem == HeldItem::EmptyMug &&
             FindTableInState(TableState::Dirty) >= 0 &&
             FindMostUrgentWaitingAleTable() < 0) {
    m_nearbyPrompt = "空のジョッキを棚へ戻して、使用済みを回収する";
  } else if (m_heldItem == HeldItem::EmptyMug) {
    m_nearbyPrompt = "カウンター中央のエール樽へ運ぶ";
  } else if (m_heldItem == HeldItem::FilledMug) {
    const int aleTable = FindMostUrgentWaitingAleTable();
    m_nearbyPrompt =
        aleTable >= 0
            ? "水鏡エールを TABLE " + std::to_string(aleTable + 1) + " へ運ぶ"
            : "注文待ちのテーブルへ運ぶ";
  } else if (m_heldItem == HeldItem::FilledBowl) {
    const int foodTable = FindMostUrgentWaitingFoodTable();
    m_nearbyPrompt =
        foodTable >= 0
            ? "月影シチューを TABLE " + std::to_string(foodTable + 1) +
                  " へ運ぶ"
            : "料理注文を待つか、洗い場で処分する";
  } else if (m_heldItem == HeldItem::DirtyMug ||
             m_heldItem == HeldItem::DirtyBowl) {
    m_nearbyPrompt = "カウンター右の洗い場へ運ぶ";
  } else if (m_heldItem == HeldItem::CleanBowl) {
    m_nearbyPrompt = "右奥の厨房へボウルを戻す";
  } else if (const int dirtyTable = FindTableInState(TableState::Dirty);
             dirtyTable >= 0) {
    m_nearbyPrompt = "TABLE " + std::to_string(dirtyTable + 1) +
                     " の使用済み食器を回収する";
  } else if (m_shiftState == ShiftState::Closing) {
    m_nearbyPrompt = "就寝準備中　店内の客を見送ろう";
  } else if (IsAfterMidnight()) {
    m_nearbyPrompt = "深夜営業中　R：受付を止めて眠る";
  } else {
    m_nearbyPrompt = "WASD：移動　E：調べる";
  }
}

TavernScene::Action
TavernScene::Update(float deltaSeconds, const XMFLOAT3 &playerPosition,
                    bool interactPressed, bool primaryActionDown,
                    bool restartPressed, const ManagementInput &managementInput,
                    bool automateGameplay) {
  m_playerPosition = playerPosition;
  const float dt = std::clamp(deltaSeconds, 0.0f, 0.25f);
  const TableState prototypeCustomerState = m_tables[0].state;
  m_customerNpc.Update(dt, prototypeCustomerState == TableState::Arriving ||
                               prototypeCustomerState == TableState::Leaving ||
                               m_entranceCustomers[0].state == EntranceState::Leaving);
  m_interactionCooldown = std::max(0.0f, m_interactionCooldown - dt);
  m_feedbackTimer = std::max(0.0f, m_feedbackTimer - dt);
  if (m_feedbackTimer <= 0.0f)
    m_feedbackText.clear();
  m_pourVisualTime += dt;
  m_primaryActionActive = false;

  if (ManagementMenuOpen()) {
    if (!automateGameplay) {
      if (interactPressed && m_interactionCooldown <= 0.0f) {
        CloseManagementMenu();
      } else if (managementInput.cancelPressed) {
        if (m_managementPage == ManagementPage::Upgrades &&
            m_upgradeConfirmationOpen) {
          m_upgradeConfirmationOpen = false;
          m_upgradeConfirmBuySelected = false;
        } else if (m_managementPage == ManagementPage::Supplies ||
                   m_managementPage == ManagementPage::Upgrades) {
          BackToManagementRoot();
        } else {
          CloseManagementMenu();
        }
      } else if (m_managementPage == ManagementPage::Root) {
        if (managementInput.previousPressed)
          m_managementSelection = ManagementSelection::Supplies;
        else if (managementInput.nextPressed)
          m_managementSelection = ManagementSelection::Upgrades;
        if (managementInput.confirmPressed) {
          if (m_managementSelection == ManagementSelection::Supplies) {
            ++m_managementActivationSerial;
            OpenSuppliesPage();
          } else if (m_managementSelection == ManagementSelection::Upgrades) {
            ++m_managementActivationSerial;
            OpenUpgradesPage();
          }
        }
      } else if (m_managementPage == ManagementPage::Supplies) {
        if (managementInput.previousPressed)
          AdjustAleOrderQuantity(-1);
        else if (managementInput.nextPressed)
          AdjustAleOrderQuantity(1);
        if (managementInput.confirmPressed)
          TryOrderAle();
      } else if (m_managementPage == ManagementPage::Upgrades) {
        if (m_upgradeConfirmationOpen) {
          if (managementInput.previousPressed)
            m_upgradeConfirmBuySelected = false;
          else if (managementInput.nextPressed)
            m_upgradeConfirmBuySelected = true;
          if (managementInput.confirmPressed) {
            if (m_upgradeConfirmBuySelected) {
              const UpgradeId upgrade =
                  static_cast<UpgradeId>(m_selectedUpgradeIndex);
              TryPurchaseUpgrade(upgrade);
            } else {
              m_upgradeConfirmationOpen = false;
            }
          }
        } else {
          const int lastUpgradeIndex = static_cast<int>(UpgradeId::Count) - 1;
          if (managementInput.previousPressed)
            m_selectedUpgradeIndex = std::max(0, m_selectedUpgradeIndex - 1);
          else if (managementInput.nextPressed)
            m_selectedUpgradeIndex =
                std::min(lastUpgradeIndex, m_selectedUpgradeIndex + 1);
          if (managementInput.confirmPressed) {
            RequestUpgradePurchase(
                static_cast<UpgradeId>(m_selectedUpgradeIndex));
          }
        }
      }
    }
    UpdateNearbyPrompt();
    return Action::None;
  }

  if (m_kitchenState == KitchenState::Cooking) {
    m_cookTimer = std::max(0.0f, m_cookTimer - dt);
    if (m_cookTimer <= 0.0f) {
      m_kitchenState = KitchenState::Ready;
      m_feedbackText = std::string(kFoodName) + "が完成した";
      m_feedbackTimer = 2.0f;
    }
  }

  if (restartPressed) {
    if (m_shiftState == ShiftState::Failed ||
        m_shiftState == ShiftState::Complete) {
      return Action::SleepUntilMorning;
    }
    if (m_shiftState == ShiftState::Running) {
      if (IsAfterMidnight()) {
        m_shiftState = ShiftState::Closing;
        m_feedbackText = "受付終了　店内の客を見送ってから眠る";
        m_feedbackTimer = 2.4f;
      } else {
        m_feedbackText = "眠れるのは 0:00 以降";
        m_feedbackTimer = 1.8f;
      }
    }
  }

  if (automateGameplay && m_shiftState == ShiftState::Running &&
      IsAfterMidnight() && m_businessHour >= 0.25f) {
    m_shiftState = ShiftState::Closing;
    m_feedbackText = "受付終了　店内の客を見送ってから眠る";
    m_feedbackTimer = 2.4f;
  }

  if (automateGameplay)
    RunAutomation();

  const bool cancelWorkRequested = !automateGameplay && interactPressed &&
                                   m_interactionCooldown <= 0.0f &&
                                   m_workState != WorkState::None;
  if (!cancelWorkRequested) {
    if (m_workState == WorkState::PouringAle) {
      const bool shouldPour =
          automateGameplay ? m_aleFill < 0.90f : primaryActionDown;
      if (shouldPour) {
        m_primaryActionActive = true;
        m_workActionStarted = true;
        const float nextFill = m_aleFill + dt * 0.34f;
        if (nextFill > 1.0f)
          m_aleOverflow += (nextFill - 1.0f) * 1.25f;
        m_aleFill = std::min(1.0f, nextFill);
        if (m_aleFill > kPerfectPourMinimum)
          m_aleFoam = std::min(0.35f, m_aleFoam + dt * 0.11f);
      } else if (m_workActionStarted) {
        FinishPouring();
      }
    } else if (m_workState == WorkState::WashingDish) {
      const bool shouldWash = automateGameplay || primaryActionDown;
      if (shouldWash) {
        m_workActionStarted = true;
        m_washProgress = std::min(1.0f, m_washProgress + dt / 1.20f);
        if (m_washProgress >= 1.0f)
          FinishWashing();
      }
    }
  }

  for (TableSlot &table : m_tables) {
    table.speechTimer = std::max(0.0f, table.speechTimer - dt);
    if (table.speechTimer <= 0.0f)
      table.speech.clear();
  }

  if (CanServeCustomers()) {
    for (int tableIndex = 0; tableIndex < CustomerTableCount(); ++tableIndex) {
      TableSlot &table = m_tables[tableIndex];
      if (!table.enabled)
        continue;
      switch (table.state) {
      case TableState::Empty:
        break;
      case TableState::Arriving:
        table.stateTimer -= dt;
        if (table.stateTimer <= 0.0f) {
          table.state = TableState::WaitingOrder;
          table.speech = "注文をお願い！";
          table.speechTimer = 2.2f;
        }
        break;
      case TableState::WaitingOrder:
        table.satisfaction -= 2.0f * dt;
        break;
      case TableState::WaitingAle:
        table.satisfaction -= 4.0f * dt;
        if (table.satisfaction <= 35.0f && !table.complaintPlayed) {
          table.complaintPlayed = true;
          table.speech = "まだかい？";
          table.speechTimer = 2.0f;
        }
        break;
      case TableState::WaitingFood:
        table.satisfaction -= 3.0f * dt;
        if (table.satisfaction <= 35.0f && !table.complaintPlayed) {
          table.complaintPlayed = true;
          table.speech = "料理はまだかい？";
          table.speechTimer = 2.0f;
        }
        break;
      case TableState::Eating:
        table.stateTimer -= dt;
        if (table.stateTimer <= 0.0f) {
          table.state = TableState::Leaving;
          table.stateTimer = CustomerTravelSeconds(tableIndex);
          table.speech = "また来るよ！";
          table.speechTimer = 0.8f;
        }
        break;
      case TableState::Leaving:
        table.stateTimer -= dt;
        if (table.stateTimer <= 0.0f) {
          if (table.servedCustomer) {
            table.state = TableState::Dirty;
            table.speech.clear();
            table.speechTimer = 0.0f;
            m_spawnTimers[tableIndex] = CustomerSpawnDelay(tableIndex);
          } else {
            table = {};
            table.enabled = true;
            m_spawnTimers[tableIndex] = CustomerSpawnDelay(tableIndex);
          }
        }
        break;
      case TableState::Dirty:
        break;
      }

      if ((table.state == TableState::WaitingOrder ||
           table.state == TableState::WaitingAle ||
           table.state == TableState::WaitingFood) &&
          table.satisfaction <= 0.0f) {
        table.satisfaction = 0.0f;
        TriggerWalkout(tableIndex);
      }
    }
    UpdateEntranceCustomers(dt);
  }

  if (m_shiftState == ShiftState::Closing && !HasActiveCustomers())
    return Action::SleepUntilMorning;

  if (!automateGameplay && interactPressed && m_interactionCooldown <= 0.0f) {
    if (m_workState != WorkState::None) {
      m_workState = WorkState::None;
      m_workActionStarted = false;
      m_aleFill = 0.0f;
      m_washProgress = 0.0f;
      m_interactionCooldown = 0.20f;
    } else if (DistanceXZ(m_playerPosition, kExitInteraction) <=
               kStationInteractionRange) {
      return Action::ReturnToOverworld;
    } else if (ManagementInteractionHasPriority()) {
      OpenManagementMenu();
    } else if (CanServeCustomers() &&
               NearestEnabledTable(kTableInteractionRange) >= 0) {
      const int tableIndex = NearestEnabledTable(kTableInteractionRange);
      TableSlot &table = m_tables[tableIndex];
      if (table.state == TableState::WaitingOrder)
        TakeOrder(tableIndex);
      else if (table.state == TableState::WaitingAle &&
               m_heldItem == HeldItem::FilledMug)
        ServeAle(tableIndex);
      else if (table.state == TableState::WaitingFood &&
               m_heldItem == HeldItem::FilledBowl)
        ServeFood(tableIndex);
      else if (table.state == TableState::Dirty && m_heldItem == HeldItem::None)
        CollectDirtyDish(tableIndex);
    } else if (const int counterSlot =
                   NearestCounterMugSlot(kCounterMugInteractionRange);
               counterSlot >= 0) {
      if (m_heldItem == HeldItem::None)
        PickUpPlacedMug(counterSlot);
      else
        PlaceHeldMug(counterSlot);
    } else if (DistanceXZ(m_playerPosition, kMugRackInteraction) <=
               kStationInteractionRange) {
      if (m_heldItem == HeldItem::None)
        PickUpEmptyMug();
      else if (m_heldItem == HeldItem::EmptyMug)
        ReturnEmptyMug();
    } else if (DistanceXZ(m_playerPosition, kAleTapInteraction) <=
                   kStationInteractionRange &&
               m_heldItem == HeldItem::EmptyMug) {
      BeginPouring();
    } else if (DistanceXZ(m_playerPosition, kKitchenInteraction) <=
               kStationInteractionRange) {
      if (m_kitchenState == KitchenState::Ready &&
          m_heldItem == HeldItem::None)
        CollectCookedFood();
      else if (m_kitchenState == KitchenState::Idle &&
               (m_heldItem == HeldItem::CleanBowl ||
                (m_heldItem == HeldItem::None && m_cleanBowls > 0)) &&
               FindMostUrgentWaitingFoodTable() >= 0)
        StartCooking();
      else if (m_kitchenState == KitchenState::Idle &&
               m_heldItem == HeldItem::CleanBowl)
        ReturnCleanBowl();
    } else if (DistanceXZ(m_playerPosition, kWashBasinInteraction) <=
               kStationInteractionRange) {
      if (m_heldItem == HeldItem::DirtyMug ||
          m_heldItem == HeldItem::DirtyBowl)
        BeginWashing();
      else if (m_heldItem == HeldItem::FilledMug) {
        m_heldItem = HeldItem::EmptyMug;
        m_aleFill = 0.0f;
        m_aleFoam = 0.0f;
      } else if (m_heldItem == HeldItem::FilledBowl) {
        m_heldItem = HeldItem::DirtyBowl;
        m_heldDishTableIndex = -1;
        m_cycleAwaitingWash = false;
      }
    }
  }

  if (automateGameplay)
    RunAutomation();
  UpdateNearbyPrompt();
  return Action::None;
}

void TavernScene::BuildFrame(FrameData &frame, const ViewContext &view) const {
  if (!m_ready)
    return;

  const auto pushTransform = [&frame](uint32_t meshId, const XMFLOAT3 &scale,
                                      const XMFLOAT3 &rotation,
                                      const XMFLOAT3 &position) {
    if (meshId == UINT32_MAX)
      return;
    frame.opaqueItems.push_back(
        {meshId,
         XMMatrixScaling(scale.x, scale.y, scale.z) *
             XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, rotation.z) *
             XMMatrixTranslation(position.x, position.y, position.z)});
  };
  const auto pushMesh = [&](uint32_t meshId, float sx, float sy, float sz,
                            float x, float y, float z) {
    pushTransform(meshId, {sx, sy, sz}, {0.0f, 0.0f, 0.0f}, {x, y, z});
  };
  const auto pushAsset = [&](TavernAsset asset, const XMFLOAT3 &scale,
                             const XMFLOAT3 &rotation,
                             const XMFLOAT3 &position) {
    const std::vector<uint32_t> &meshIds =
        m_tavernAssetMeshIds[static_cast<std::size_t>(asset)];
    for (const uint32_t meshId : meshIds)
      pushTransform(meshId, scale, rotation, position);
  };
  const bool hasImportedMug =
      !m_tavernAssetMeshIds[static_cast<std::size_t>(TavernAsset::Mug)].empty();
  const bool hasImportedBowl =
      !m_tavernAssetMeshIds[static_cast<std::size_t>(TavernAsset::Bowl)].empty();
  const auto rotateHorizontalOffset = [](float x, float z, float yaw) {
    const float yawCos = std::cos(yaw);
    const float yawSin = std::sin(yaw);
    return XMFLOAT3{x * yawCos + z * yawSin, 0.0f,
                    z * yawCos - x * yawSin};
  };
  const auto pushMugBase = [&](HeldItem item, const XMFLOAT3 &position,
                               float yaw) {
    if (hasImportedMug) {
      pushAsset(TavernAsset::Mug,
                {kImportedMugScale, kImportedMugScale, kImportedMugScale},
                {0.0f, yaw, 0.0f},
                {position.x, position.y - kImportedMugHalfHeight, position.z});
    } else {
      pushTransform(MugMeshForState(item),
                    {kMugScaleXZ, kMugScaleY, kMugScaleXZ},
                    {0.0f, yaw, 0.0f}, position);
    }
  };
  const auto pushDirtySpots = [&](const XMFLOAT3 &position, float yaw) {
    const auto pushSpot = [&](const XMFLOAT3 &scale,
                              const XMFLOAT3 &localOffset) {
      const XMFLOAT3 rotatedOffset =
          rotateHorizontalOffset(localOffset.x, localOffset.z, yaw);
      pushTransform(m_dirtySpotMeshId, scale, {0.0f, yaw, 0.0f},
                    {position.x + rotatedOffset.x,
                     position.y + localOffset.y,
                     position.z + rotatedOffset.z});
    };
    pushSpot({0.055f, 0.045f, 0.028f}, {-0.11f, 0.08f, -0.18f});
    pushSpot({0.038f, 0.032f, 0.022f}, {0.12f, -0.04f, -0.18f});
    pushSpot({0.030f, 0.026f, 0.020f}, {0.02f, 0.17f, -0.18f});
  };
  const auto pushStateMug = [&](HeldItem item, const XMFLOAT3 &position,
                                float aleFill, float aleFoam, float yaw) {
    if (item == HeldItem::None)
      return;
    pushMugBase(item, position, yaw);
    if (item == HeldItem::DirtyMug)
      pushDirtySpots(position, yaw);
    if (item == HeldItem::FilledMug) {
      const float visibleFill = std::max(0.12f, aleFill);
      pushMesh(m_aleMeshId, kAleSurfaceScaleXZ, 0.06f, kAleSurfaceScaleXZ,
               position.x, position.y - 0.19f + visibleFill * 0.34f,
               position.z);
      if (aleFoam > 0.002f) {
        const float foamScale = 0.045f + aleFoam * 0.16f;
        pushMesh(m_foamMeshId, 0.265f, foamScale, 0.265f, position.x,
                 position.y - 0.15f + visibleFill * 0.34f, position.z);
      }
    }
  };
  const auto pushStateBowl = [&](HeldItem item, const XMFLOAT3 &position,
                                 float yaw, float modelScale = 1.0f) {
    if (!IsBowlItem(item))
      return;
    if (hasImportedBowl) {
      pushAsset(TavernAsset::Bowl,
                {modelScale, modelScale, modelScale},
                {0.0f, yaw, 0.0f}, position);
    } else {
      pushTransform(m_mugMeshId,
                    {0.48f * modelScale, 0.13f * modelScale,
                     0.48f * modelScale},
                    {0.0f, yaw, 0.0f},
                    {position.x, position.y + 0.065f * modelScale,
                     position.z});
    }
    if (item == HeldItem::FilledBowl) {
      pushMesh(m_stewMeshId, 0.37f * modelScale, 0.035f * modelScale,
               0.37f * modelScale, position.x,
               position.y + 0.105f * modelScale, position.z);
      pushMesh(m_glowMeshId, 0.045f * modelScale, 0.025f * modelScale,
               0.045f * modelScale,
               position.x - 0.12f * modelScale,
               position.y + 0.14f * modelScale,
               position.z + 0.04f * modelScale);
      pushMesh(m_glowMeshId, 0.035f * modelScale, 0.020f * modelScale,
               0.035f * modelScale,
               position.x + 0.11f * modelScale,
               position.y + 0.14f * modelScale,
               position.z - 0.06f * modelScale);
    } else if (item == HeldItem::DirtyBowl) {
      pushMesh(m_dirtySpotMeshId, 0.10f * modelScale,
               0.025f * modelScale, 0.08f * modelScale,
               position.x - 0.09f * modelScale,
               position.y + 0.12f * modelScale, position.z);
      pushMesh(m_dirtySpotMeshId, 0.07f, 0.020f, 0.06f, position.x + 0.10f,
               position.y + 0.12f, position.z + 0.05f);
    }
  };

  pushMesh(m_floorMeshId, 14.0f, 1.0f, 12.0f, 0.0f, 0.0f, 4.2f);
  pushMesh(m_wallMeshId, 14.0f, 4.8f, 0.34f, 0.0f, 2.4f, 10.1f);
  pushMesh(m_wallMeshId, 0.34f, 4.8f, 12.0f, -7.0f, 2.4f, 4.2f);
  pushMesh(m_wallMeshId, 0.34f, 4.8f, 12.0f, 7.0f, 2.4f, 4.2f);

  // 梁、柱、床板で正面開放型の酒場を額縁のように見せる。
  constexpr float floorSeamZ[] = {-1.0f, 0.8f, 2.6f, 4.4f, 6.2f, 8.0f, 9.8f};
  for (const float z : floorSeamZ)
    pushMesh(m_darkWoodMeshId, 13.6f, 0.018f, 0.025f, 0.0f, 0.012f, z);
  constexpr float backPostX[] = {-6.45f, -3.25f, 0.0f, 3.25f, 6.45f};
  for (const float x : backPostX)
    pushMesh(m_darkWoodMeshId, 0.22f, 4.65f, 0.22f, x, 2.33f, 9.86f);
  constexpr float sidePostZ[] = {0.0f, 3.4f, 6.8f};
  for (const float z : sidePostZ) {
    pushMesh(m_darkWoodMeshId, 0.22f, 4.65f, 0.22f, -6.82f, 2.33f, z);
    pushMesh(m_darkWoodMeshId, 0.22f, 4.65f, 0.22f, 6.82f, 2.33f, z);
  }
  constexpr float rafterZ[] = {0.4f, 3.7f, 7.0f, 9.72f};
  for (const float z : rafterZ)
    pushMesh(m_darkWoodMeshId, 13.55f, 0.20f, 0.20f, 0.0f, 4.55f, z);
  // 既存の操作位置を守りながら、カウンター正面を木枠で仕上げる。
  pushMesh(m_darkWoodMeshId, 8.6f, 1.25f, 1.05f, 0.0f, 0.625f, 8.0f);
  pushMesh(m_woodMeshId, 8.9f, 0.16f, 1.25f, 0.0f, 1.32f, 8.0f);
  for (const float x : {-4.0f, -2.0f, 0.0f, 2.0f, 4.0f})
    pushMesh(m_woodMeshId, 0.12f, 1.08f, 0.08f, x, 0.62f, 7.45f);
  pushMesh(m_woodMeshId, 8.30f, 0.10f, 0.08f, 0.0f, 0.13f, 7.45f);
  pushMesh(m_woodMeshId, 8.30f, 0.10f, 0.08f, 0.0f, 1.10f, 7.45f);
  pushMesh(m_darkWoodMeshId, 5.8f, 0.18f, 0.65f, 0.0f, 2.25f, 9.55f);
  pushMesh(m_darkWoodMeshId, 5.8f, 0.18f, 0.65f, 0.0f, 3.35f, 9.55f);

  // 客席とは別の管理机。開いた台帳と封印光で操作場所を明示する。
  pushMesh(m_darkWoodMeshId, 1.65f, 0.14f, 1.15f, kManagementTablePosition.x,
           0.94f, kManagementTablePosition.z);
  pushMesh(m_woodMeshId, 1.76f, 0.08f, 1.24f, kManagementTablePosition.x, 1.04f,
           kManagementTablePosition.z);
  for (const float xOffset : {-0.62f, 0.62f}) {
    for (const float zOffset : {-0.40f, 0.40f}) {
      pushMesh(m_darkWoodMeshId, 0.13f, 0.88f, 0.13f,
               kManagementTablePosition.x + xOffset, 0.46f,
               kManagementTablePosition.z + zOffset);
    }
  }
  pushTransform(
      m_darkWoodMeshId, {0.92f, 0.035f, 0.58f}, {0.0f, 0.08f, 0.0f},
      {kManagementTablePosition.x + 0.12f, 1.105f, kManagementTablePosition.z});
  pushTransform(
      m_wallMeshId, {0.39f, 0.024f, 0.49f}, {0.0f, 0.03f, -0.035f},
      {kManagementTablePosition.x - 0.10f, 1.145f, kManagementTablePosition.z});
  pushTransform(
      m_wallMeshId, {0.39f, 0.024f, 0.49f}, {0.0f, -0.03f, 0.035f},
      {kManagementTablePosition.x + 0.34f, 1.145f, kManagementTablePosition.z});
  pushMesh(m_glowMeshId, 0.10f, 0.022f, 0.10f,
           kManagementTablePosition.x + 0.12f, 1.18f,
           kManagementTablePosition.z + 0.02f);
  pushTransform(m_metalMeshId, {0.025f, 0.34f, 0.025f}, {0.0f, 0.0f, -0.45f},
                {kManagementTablePosition.x + 0.56f, 1.26f,
                 kManagementTablePosition.z - 0.18f});

  constexpr float tableX[3] = {-3.8f, 0.0f, 3.8f};
  const bool hasRoundTable =
      !m_tavernAssetMeshIds[static_cast<std::size_t>(TavernAsset::RoundTable)]
           .empty();
  const bool hasStool =
      !m_tavernAssetMeshIds[static_cast<std::size_t>(TavernAsset::Stool)]
           .empty();
  const bool hasCommunalSet =
      !m_tavernAssetMeshIds[static_cast<std::size_t>(TavernAsset::LongTable)]
           .empty() &&
      !m_tavernAssetMeshIds[static_cast<std::size_t>(TavernAsset::Bench)]
           .empty();

  if (hasRoundTable && hasStool) {
    for (int tableIndex = 0; tableIndex < kBaseTableCount; ++tableIndex) {
      const float x = tableX[tableIndex];
      pushAsset(TavernAsset::RoundTable, {1.08f, 1.08f, 1.08f},
                {0.0f, 0.18f * static_cast<float>(tableIndex), 0.0f},
                {x, 0.0f, 4.15f});
      pushAsset(TavernAsset::Stool, {0.88f, 0.88f, 0.88f}, {0.0f, 0.0f, 0.0f},
                {x, 0.0f, 5.24f});
      pushAsset(TavernAsset::Stool, {0.88f, 0.88f, 0.88f},
                {0.0f, XM_PIDIV2, 0.0f}, {x - 0.96f, 0.0f, 4.08f});
      pushAsset(TavernAsset::Stool, {0.88f, 0.88f, 0.88f},
                {0.0f, -XM_PIDIV2, 0.0f}, {x + 0.96f, 0.0f, 4.08f});
    }

    if (hasCommunalSet) {
      pushAsset(TavernAsset::LongTable, {0.72f, 1.40f, 0.74f},
                {0.0f, XM_PIDIV2, 0.0f}, {tableX[2], 0.0f, 4.15f});
      pushAsset(TavernAsset::Bench, {0.95f, 1.50f, 0.70f},
                {0.0f, XM_PIDIV2, 0.0f}, {tableX[2], 0.0f, 3.43f});
      pushAsset(TavernAsset::Bench, {0.95f, 1.50f, 0.70f},
                {0.0f, XM_PIDIV2, 0.0f}, {tableX[2], 0.0f, 4.87f});
    } else {
      pushAsset(TavernAsset::RoundTable, {1.08f, 1.08f, 1.08f},
                {0.0f, -0.16f, 0.0f}, {tableX[2], 0.0f, 4.15f});
      pushAsset(TavernAsset::Stool, {0.88f, 0.88f, 0.88f}, {0.0f, 0.0f, 0.0f},
                {tableX[2], 0.0f, 5.24f});
      pushAsset(TavernAsset::Stool, {0.88f, 0.88f, 0.88f},
                {0.0f, XM_PIDIV2, 0.0f}, {tableX[2] - 0.96f, 0.0f, 4.08f});
      pushAsset(TavernAsset::Stool, {0.88f, 0.88f, 0.88f},
                {0.0f, -XM_PIDIV2, 0.0f}, {tableX[2] + 0.96f, 0.0f, 4.08f});
    }
  } else {
    // 生成済みGLBが見つからない場合もゲームプレイ可能な旧家具を残す。
    for (const float x : tableX) {
      pushMesh(m_woodMeshId, 2.35f, 0.16f, 1.65f, x, 0.95f, 4.15f);
      pushMesh(m_darkWoodMeshId, 0.18f, 0.90f, 0.18f, x - 0.82f, 0.45f, 3.62f);
      pushMesh(m_darkWoodMeshId, 0.18f, 0.90f, 0.18f, x + 0.82f, 0.45f, 3.62f);
      pushMesh(m_darkWoodMeshId, 0.18f, 0.90f, 0.18f, x - 0.82f, 0.45f, 4.68f);
      pushMesh(m_darkWoodMeshId, 0.18f, 0.90f, 0.18f, x + 0.82f, 0.45f, 4.68f);
      pushMesh(m_darkWoodMeshId, 1.05f, 0.52f, 0.58f, x, 0.26f, 2.95f);
      pushMesh(m_darkWoodMeshId, 1.05f, 0.52f, 0.58f, x, 0.26f, 5.35f);
    }
  }

  // テーブル上の食器と灯り。三卓目は購入後に共同席の灯りを点ける。
  for (int tableIndex = 0; tableIndex < 3; ++tableIndex) {
    const float topY = tableIndex < kBaseTableCount ? 1.00f : 0.97f;
    const bool tableLit = tableIndex < kBaseTableCount || m_table3Unlocked;
    pushAsset(TavernAsset::Candle, {1.45f, 1.45f, 1.45f}, {0.0f, 0.0f, 0.0f},
              {tableX[tableIndex] - 0.34f, topY, 4.02f});
    if (tableLit) {
      pushMesh(m_glowMeshId, 0.045f, 0.075f, 0.045f, tableX[tableIndex] - 0.34f,
               topY + 0.25f, 4.02f);
    }
    pushAsset(TavernAsset::Plate, {0.58f, 0.58f, 0.58f},
              {0.0f, 0.18f * static_cast<float>(tableIndex), 0.0f},
              {tableX[tableIndex] + 0.28f, topY + 0.012f, 4.10f});
  }
  if (!m_table3Unlocked) {
    pushTransform(m_darkWoodMeshId, {0.82f, 0.08f, 0.34f},
                  {0.0f, 0.12f, -0.12f}, {tableX[2], 1.28f, 4.10f});
    pushTransform(m_metalMeshId, {0.045f, 0.50f, 0.035f}, {0.0f, 0.0f, -0.82f},
                  {tableX[2], 1.35f, 4.02f});
    pushTransform(m_metalMeshId, {0.045f, 0.50f, 0.035f}, {0.0f, 0.0f, 0.82f},
                  {tableX[2], 1.35f, 4.02f});
  }

  // 後方の保管品は歩行経路を塞がない壁際へまとめる。
  pushAsset(TavernAsset::Barrel, {1.05f, 1.05f, 1.05f}, {0.0f, 0.16f, 0.0f},
            {-5.72f, 0.0f, 9.16f});
  pushAsset(TavernAsset::Barrel, {0.96f, 0.96f, 0.96f}, {0.0f, -0.13f, 0.0f},
            {5.72f, 0.0f, 9.18f});
  pushAsset(TavernAsset::Crate, {1.0f, 1.0f, 1.0f}, {0.0f, -0.20f, 0.0f},
            {4.82f, 0.0f, 9.22f});
  pushAsset(TavernAsset::Crate, {0.88f, 0.88f, 0.88f}, {0.0f, 0.16f, 0.0f},
            {5.05f, 0.61f, 9.24f});
  pushAsset(TavernAsset::Chest, {1.05f, 1.05f, 1.05f}, {0.0f, XM_PIDIV2, 0.0f},
            {-4.78f, 0.0f, 9.20f});

  // 武器は奥行き方向を90度回して壁面から読めるシルエットにする。
  pushAsset(TavernAsset::Sword, {1.34f, 1.34f, 1.34f},
            {0.0f, XM_PIDIV2, -0.10f}, {-4.55f, 1.42f, 9.76f});
  pushAsset(TavernAsset::Mace, {1.48f, 1.48f, 1.48f}, {0.0f, XM_PIDIV2, 0.11f},
            {-3.75f, 1.48f, 9.74f});
  pushAsset(TavernAsset::Halberd, {1.12f, 1.12f, 1.12f},
            {0.0f, XM_PIDIV2, -0.075f}, {4.32f, 1.36f, 9.74f});
  for (const float x : {-2.45f, 2.45f}) {
    pushAsset(TavernAsset::Candle, {1.35f, 1.35f, 1.35f}, {0.0f, 0.0f, 0.0f},
              {x, 2.35f, 9.40f});
    pushMesh(m_glowMeshId, 0.042f, 0.07f, 0.042f, x, 2.58f, 9.40f);
  }
  pushAsset(TavernAsset::Plate, {0.62f, 0.62f, 0.62f}, {0.0f, 0.0f, 0.0f},
            {-1.45f, 2.36f, 9.38f});
  pushAsset(TavernAsset::Plate, {0.50f, 0.50f, 0.50f}, {0.0f, 0.0f, 0.0f},
            {1.40f, 2.36f, 9.38f});
  pushMugBase(HeldItem::EmptyMug, {0.72f, 2.53f, 9.38f}, 0.0f);

  // 左からジョッキ棚、仮置き、エール樽、洗い場の順に配置する。
  pushMesh(m_darkWoodMeshId, 1.55f, 0.16f, 0.58f, -2.7f, 1.65f, 7.72f);
  for (int mugIndex = 0; mugIndex < m_cleanMugs; ++mugIndex) {
    pushStateMug(HeldItem::EmptyMug,
                 {-2.92f + static_cast<float>(mugIndex) * 0.42f,
                  hasImportedMug ? 1.915f : 1.98f, 7.72f},
                 0.0f, 0.0f, 0.0f);
  }

  pushMesh(m_darkWoodMeshId, 1.20f, 0.07f, 0.58f, -1.20f, 1.43f, 7.65f);
  for (int slotIndex = 0; slotIndex < static_cast<int>(m_counterMugs.size());
       ++slotIndex) {
    const MugState &mug = m_counterMugs[slotIndex];
    if (mug.item == HeldItem::None) {
      const XMFLOAT3 &slotPosition = kCounterMugPositions[slotIndex];
      pushMesh(m_glowMeshId, 0.22f, 0.018f, 0.22f, slotPosition.x, 1.48f,
               slotPosition.z);
    }
    pushStateMug(mug.item, kCounterMugPositions[slotIndex], mug.aleFill,
                 mug.aleFoam, 0.0f);
  }

  const float kegScale = 1.34f + static_cast<float>(AleCapacityLevel()) * 0.08f;
  pushAsset(TavernAsset::Barrel, {kegScale, kegScale, kegScale},
            {0.0f, 0.0f, 0.0f}, {0.0f, 1.33f, 9.35f});
  if (m_tavernAssetMeshIds[static_cast<std::size_t>(TavernAsset::Barrel)]
          .empty())
    pushMesh(m_woodMeshId, 1.15f + AleCapacityLevel() * 0.07f,
             1.55f + AleCapacityLevel() * 0.08f,
             0.95f + AleCapacityLevel() * 0.06f, 0.0f, 2.12f, 9.40f);
  pushMesh(m_metalMeshId, 0.20f, 0.82f, 0.20f, 0.0f, 2.12f, 8.55f);
  pushMesh(m_metalMeshId, 0.58f, 0.16f, 0.20f, 0.0f, 2.42f, 8.25f);
  pushMesh(m_glowMeshId, 0.18f, 0.18f, 0.18f, 0.0f, 2.72f, 8.30f);
  for (int level = 0; level < kMaximumAleCapacityLevel; ++level) {
    const bool purchased = level < AleCapacityLevel();
    pushMesh(purchased ? m_glowMeshId : m_metalMeshId, 0.065f, 0.065f, 0.045f,
             -0.18f + static_cast<float>(level) * 0.18f, 2.82f, 8.47f);
  }

  // 洗い場は大きな木鉢と、その内側だけに収まる反射水面で構成する。
  if (hasImportedBowl) {
    pushAsset(TavernAsset::Bowl, {2.45f, 1.20f, 1.85f},
              {0.0f, 0.0f, 0.0f}, {2.70f, 1.45f, 7.78f});
  } else {
    pushMesh(m_metalMeshId, 1.55f, 0.24f, 0.88f, 2.70f, 1.50f, 7.78f);
  }
  pushMesh(m_waterMeshId, 0.92f, 0.025f, 0.70f, 2.70f, 1.58f, 7.78f);

  // 右奥の厨房。調理は非ブロッキングで進み、完成品を受取口へ出す。
  pushMesh(m_darkWoodMeshId, 2.20f, 1.18f, 1.20f, 5.35f, 0.59f, 8.18f);
  pushMesh(m_woodMeshId, 2.34f, 0.14f, 1.32f, 5.35f, 1.25f, 8.18f);
  pushMesh(m_metalMeshId, 1.38f, 0.24f, 0.92f, 5.35f, 1.43f, 8.35f);
  const float stewSurfaceY = hasImportedBowl ? 1.72f : 1.84f;
  if (hasImportedBowl)
    pushAsset(TavernAsset::Bowl, {1.38f, 1.38f, 1.38f},
              {0.0f, 0.0f, 0.0f}, {5.35f, 1.55f, 8.35f});
  else
    pushTransform(m_mugMeshId, {0.72f, 0.38f, 0.72f},
                  {0.0f, 0.0f, 0.0f}, {5.35f, 1.66f, 8.35f});
  const float kitchenProgress = KitchenProgress();
  if (m_kitchenState == KitchenState::Cooking) {
    pushMesh(m_stewMeshId, 0.52f, 0.045f, 0.52f, 5.35f, stewSurfaceY,
             8.35f);
    const float pulse = 0.85f + 0.15f * std::sin(m_pourVisualTime * 9.0f);
    pushMesh(m_glowMeshId, 0.72f * pulse, 0.08f, 0.54f * pulse, 5.35f,
             1.30f, 8.35f);
  }
  pushMesh(m_darkWoodMeshId, 1.54f, 0.08f, 0.10f, 5.35f, 2.28f, 8.72f);
  if (kitchenProgress > 0.0f)
    pushMesh(m_glowMeshId, 1.46f * kitchenProgress, 0.045f, 0.12f,
             4.62f + 0.73f * kitchenProgress, 2.28f, 8.70f);
  for (int bowlIndex = 0; bowlIndex < m_cleanBowls; ++bowlIndex)
    pushStateBowl(HeldItem::CleanBowl,
                  {4.22f, 1.45f + static_cast<float>(bowlIndex) * 0.11f,
                   7.76f},
                  0.0f, 0.85f);
  if (m_kitchenState == KitchenState::Ready)
    pushStateBowl(HeldItem::FilledBowl, {5.92f, 1.45f, 7.76f}, 0.0f,
                  0.85f);

  for (int tableIndex = 0; tableIndex < CustomerTableCount(); ++tableIndex) {
    const TableState tableState = m_tables[tableIndex].state;
    const bool customerVisible =
        tableState != TableState::Empty && tableState != TableState::Dirty;
    const auto drawCustomer = [&](const XMFLOAT3 &position, float yaw) {
      if (tableIndex == 0 && ImportedCustomerReady()) {
        m_customerNpc.BuildFrame(frame, position, yaw);
      } else {
        // 入口でも接地する全身の代替モデル。実モデルと同じ約1.62 mに揃える。
        pushMesh(m_customerBodyMeshId, 0.46f, 0.70f, 0.40f, position.x,
                 position.y + 0.93f, position.z);
        pushMesh(m_customerHeadMeshId, 0.34f, 0.34f, 0.34f, position.x,
                 position.y + 1.40f, position.z);
        pushMesh(m_customerHairMeshId, 0.36f, 0.18f, 0.36f, position.x,
                 position.y + 1.53f, position.z);
        for (const float side : {-1.0f, 1.0f}) {
          const XMFLOAT3 offset =
              rotateHorizontalOffset(side * 0.13f, 0.0f, yaw);
          pushMesh(m_customerBodyMeshId, 0.16f, 0.60f, 0.18f,
                   position.x + offset.x, position.y + 0.30f, position.z + offset.z);
        }
      }
    };
    if (customerVisible) {
      const float customerZ = tableIndex == 2 ? 5.12f : 5.28f;
      float customerYaw = XM_PI;
      XMFLOAT3 customerPosition = {tableX[tableIndex], 0.0f, customerZ};
      if (tableState == TableState::Arriving || tableState == TableState::Leaving)
        customerPosition = CustomerRoutePosition(
            tableIndex, m_tables[tableIndex].stateTimer,
            tableState == TableState::Leaving, customerYaw);
      drawCustomer(customerPosition, customerYaw);
      if (tableState == TableState::WaitingOrder)
        pushMesh(m_glowMeshId, 0.16f, 0.52f, 0.16f, tableX[tableIndex], 2.72f,
                 customerZ - 0.10f);
      if (tableState == TableState::WaitingAle ||
          tableState == TableState::WaitingFood) {
        // 吹き出しと模型を同じカメラ基底に置き、どの方向からも読めるようにする。
        const XMMATRIX bubbleWorld =
            XMMatrixRotationRollPitchYaw(-view.cameraPitch, view.cameraYaw, 0.0f) *
            XMMatrixTranslation(customerPosition.x, customerPosition.y + 2.18f,
                                customerPosition.z);
        frame.opaqueItems.push_back({m_orderBubbleMeshId, bubbleWorld});
        frame.opaqueItems.push_back({m_orderBubbleBorderMeshId,
            XMMatrixScaling(1.08f, 1.08f, 1.0f) *
            XMMatrixTranslation(0.0f, 0.0f, 0.015f) * bubbleWorld});
        const size_t iconStart = frame.opaqueItems.size();
        if (tableState == TableState::WaitingFood)
          pushStateBowl(HeldItem::FilledBowl, {0.0f, 0.0f, 0.0f}, -0.60f);
        else
          pushStateMug(HeldItem::FilledMug, {0.0f, 0.0f, 0.0f}, 0.90f,
                       0.04f, -0.60f);
        // 模型を面より手前に置く際の視差を補正し、画面端でも中央に収める。
        const XMVECTOR localEye = XMVector3TransformCoord(
            XMLoadFloat3(&view.cameraPosition), XMMatrixInverse(nullptr, bubbleWorld));
        constexpr float iconDepth = 0.28f;
        const float eyeDepth = -XMVectorGetZ(localEye);
        const float parallax = eyeDepth > iconDepth ? iconDepth / eyeDepth : 0.0f;
        const XMMATRIX iconWorld = XMMatrixScaling(0.90f, 0.90f, 0.90f) *
            XMMatrixRotationX(-0.30f) *
            XMMatrixTranslation(XMVectorGetX(localEye) * parallax,
                                -0.015f + XMVectorGetY(localEye) * parallax,
                                -iconDepth) *
            bubbleWorld;
        for (size_t i = iconStart; i < frame.opaqueItems.size(); ++i)
          frame.opaqueItems[i].world *= iconWorld;
      }
    }

    const EntranceCustomer &waiting = m_entranceCustomers[tableIndex];
    if (waiting.state != EntranceState::None) {
      XMFLOAT3 position = EntrancePosition(tableIndex);
      if (waiting.state == EntranceState::Leaving)
        position.z -= (1.0f - std::clamp(waiting.leaveTimer /
                                          kEntranceLeaveSeconds, 0.0f, 1.0f)) * 1.8f;
      drawCustomer(position, waiting.state == EntranceState::Leaving ? XM_PI : 0.0f);
      if (waiting.state == EntranceState::Waiting) {
        const float progress = waiting.waitedSeconds / kEntranceWaitSeconds;
        pushMesh(m_darkWoodMeshId, 0.86f, 0.10f, 0.08f,
                 position.x, 2.35f, position.z);
        if (progress > 0.0f)
          pushMesh(m_glowMeshId, 0.80f * progress, 0.06f, 0.10f,
                   position.x - 0.40f + 0.40f * progress, 2.35f, position.z);
      }
    }

    if (tableState == TableState::Eating || tableState == TableState::Dirty ||
        (tableState == TableState::Leaving && m_tables[tableIndex].servedCustomer)) {
      if (m_tables[tableIndex].order == OrderType::Food) {
        const XMFLOAT3 tableBowlPosition = {tableX[tableIndex], 1.02f, 4.32f};
        pushStateBowl(tableState == TableState::Eating ? HeldItem::FilledBowl
                                                        : HeldItem::DirtyBowl,
                      tableBowlPosition, 0.0f);
      } else {
        const XMFLOAT3 tableMugPosition = {
            tableX[tableIndex], hasImportedMug ? 1.18f : 1.27f, 4.32f};
        if (tableState == TableState::Eating)
          pushStateMug(HeldItem::FilledMug, tableMugPosition, 0.90f, 0.04f,
                       0.0f);
        else
          pushStateMug(HeldItem::DirtyMug, tableMugPosition, 0.0f, 0.0f, 0.0f);
      }
    }
  }

  XMFLOAT3 carriedMugPosition = {m_playerPosition.x + 0.48f, 1.03f,
                                 m_playerPosition.z + 0.10f};
  float carriedMugYaw = 0.0f;
  if (view.firstPerson && m_workState == WorkState::None) {
    const float pitchCos = std::cos(view.cameraPitch);
    const float pitchSin = std::sin(view.cameraPitch);
    const float yawCos = std::cos(view.cameraYaw);
    const float yawSin = std::sin(view.cameraYaw);
    const XMFLOAT3 forward = {pitchCos * yawSin, pitchSin,
                              pitchCos * yawCos};
    const XMFLOAT3 right = {yawCos, 0.0f, -yawSin};
    const XMFLOAT3 up = {-pitchSin * yawSin, pitchCos,
                         -pitchSin * yawCos};
    carriedMugPosition = {
        view.cameraPosition.x + right.x * 0.34f + forward.x * 0.66f -
            up.x * 0.30f,
        view.cameraPosition.y + right.y * 0.34f + forward.y * 0.66f -
            up.y * 0.30f,
        view.cameraPosition.z + right.z * 0.34f + forward.z * 0.66f -
            up.z * 0.30f};
    carriedMugYaw = view.cameraYaw;
  }
  if (m_workState == WorkState::PouringAle)
    carriedMugPosition = {0.0f, 1.64f, 7.38f};
  else if (m_workState == WorkState::WashingDish)
    carriedMugPosition = {2.7f, 1.86f, 7.65f};

  if (m_heldItem != HeldItem::None) {
    if (m_workState == WorkState::PouringAle) {
      pushMugBase(HeldItem::EmptyMug, carriedMugPosition, 0.0f);
      const float visibleFill = m_heldItem == HeldItem::FilledMug
                                    ? std::max(0.12f, m_aleFill)
                                    : m_aleFill;
      if (visibleFill > 0.01f) {
        pushMesh(m_aleMeshId, kAleSurfaceScaleXZ, 0.06f, kAleSurfaceScaleXZ,
                 carriedMugPosition.x,
                 carriedMugPosition.y - 0.19f + visibleFill * 0.34f,
                 carriedMugPosition.z);
        if (m_aleFoam > 0.002f) {
          const float foamScale = 0.045f + m_aleFoam * 0.16f;
          pushMesh(m_foamMeshId, 0.265f, foamScale, 0.265f,
                   carriedMugPosition.x,
                   carriedMugPosition.y - 0.15f + visibleFill * 0.34f,
                   carriedMugPosition.z);
        }
      }
    } else if (IsMugItem(m_heldItem)) {
      pushStateMug(m_heldItem, carriedMugPosition, m_aleFill, m_aleFoam,
                   carriedMugYaw);
    } else {
      pushStateBowl(m_heldItem,
                    {carriedMugPosition.x, carriedMugPosition.y - 0.12f,
                     carriedMugPosition.z},
                    carriedMugYaw);
    }
  }

  if (m_workState == WorkState::PouringAle && m_primaryActionActive) {
    const float pulse = 0.015f * std::sin(m_pourVisualTime * 24.0f);
    pushMesh(m_aleMeshId, 0.06f + pulse, 0.70f, 0.06f + pulse, 0.0f, 2.23f,
             7.38f);
    if (m_aleFill > 0.70f) {
      pushMesh(m_foamMeshId, 0.08f, 0.07f, 0.08f, -0.28f, 1.93f, 7.38f);
      pushMesh(m_foamMeshId, 0.06f, 0.05f, 0.06f, 0.30f, 1.89f, 7.35f);
    }
    if (m_aleOverflow > 0.001f) {
      const float drop = std::fmod(m_pourVisualTime * 1.8f, 0.55f);
      pushMesh(m_aleMeshId, 0.055f, 0.09f, 0.055f, 0.33f, 1.74f - drop, 7.38f);
      pushMesh(m_aleMeshId, 0.18f, 0.025f, 0.12f, 0.34f, 1.39f, 7.38f);
    }
  }

  if (m_tutorialStep != TutorialStep::Complete) {
    XMFLOAT3 tutorialMarker = kTableInteractions[m_tutorialTableIndex];
    switch (m_tutorialStep) {
    case TutorialStep::TakeOrder: {
      const int orderTable = FindTableInState(TableState::WaitingOrder);
      const int targetTable = orderTable >= 0 ? orderTable : 0;
      tutorialMarker = kTableInteractions[targetTable];
      break;
    }
    case TutorialStep::GetMug:
      tutorialMarker = kMugRackInteraction;
      break;
    case TutorialStep::StartPour:
    case TutorialStep::PourAle:
      tutorialMarker = kAleTapInteraction;
      break;
    case TutorialStep::ServeAle: {
      const int aleTable = FindMostUrgentWaitingAleTable();
      const int targetTable = aleTable >= 0 ? aleTable : m_tutorialTableIndex;
      tutorialMarker = kTableInteractions[targetTable];
      break;
    }
    case TutorialStep::CollectMug:
      tutorialMarker = kTableInteractions[m_tutorialTableIndex];
      break;
    case TutorialStep::WashMug:
      tutorialMarker = kWashBasinInteraction;
      break;
    case TutorialStep::Complete:
      break;
    }
    const float markerPulse = 1.0f + 0.14f * std::sin(m_pourVisualTime * 5.5f);
    pushMesh(m_glowMeshId, 0.16f * markerPulse, 0.58f, 0.16f * markerPulse,
             tutorialMarker.x, 2.48f, tutorialMarker.z);
    pushMesh(m_glowMeshId, 0.24f * markerPulse, 0.12f, 0.24f * markerPulse,
             tutorialMarker.x, 1.98f, tutorialMarker.z);
  }

  GPUPointLight counterLight{};
  counterLight.position = {0.0f, 3.15f, 7.2f};
  counterLight.range = 10.0f;
  counterLight.color = {1.0f, 0.48f, 0.18f};
  counterLight.intensity = 8.0f;
  frame.pointLights.push_back(counterLight);

  for (int tableIndex = 0; tableIndex < 3; ++tableIndex) {
    if (tableIndex == 2 && !m_table3Unlocked)
      continue;
    GPUPointLight tableLight{};
    tableLight.position = {tableX[tableIndex], 1.75f, 4.15f};
    tableLight.range = 4.2f;
    tableLight.color = {1.0f, 0.56f, 0.24f};
    tableLight.intensity = 3.2f;
    frame.pointLights.push_back(tableLight);
  }
}

void TavernScene::DrawManagementRoot(int viewportWidth, int viewportHeight) {
  if (m_managementPage != ManagementPage::Root)
    return;

  const uint64_t renderedFrameSerial =
      m_managementUiDiagnostics.renderedFrameSerial + 1;
  m_managementUiDiagnostics = {};
  m_managementUiDiagnostics.menuOpen = true;
  m_managementUiDiagnostics.rootRendered = true;
  m_managementUiDiagnostics.activationSerial = m_managementActivationSerial;
  m_managementUiDiagnostics.purchaseSerial = m_supplyPurchaseSerial;
  m_managementUiDiagnostics.upgradePurchaseSerial = m_upgradePurchaseSerial;
  m_managementUiDiagnostics.renderedFrameSerial = renderedFrameSerial;

  const ImVec2 viewportSize(static_cast<float>(viewportWidth),
                            static_cast<float>(viewportHeight));
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(viewportSize, ImGuiCond_Always);
  ImGui::SetNextWindowFocus();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ImGui::Begin("##TavernManagementRoot", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoScrollWithMouse |
                   ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus);

  ImDrawList *draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(ImVec2(0.0f, 0.0f), viewportSize,
                      TavernUiColor(0.0f, 0.0f, 0.0f, 0.72f));

  const float panelWidth = std::min(std::max(760.0f, viewportSize.x * 0.72f),
                                    std::max(320.0f, viewportSize.x - 40.0f));
  const float panelHeight = std::min(std::max(520.0f, viewportSize.y * 0.76f),
                                     std::max(360.0f, viewportSize.y - 36.0f));
  const ImVec2 panelMin((viewportSize.x - panelWidth) * 0.5f,
                        (viewportSize.y - panelHeight) * 0.5f);
  const ImVec2 panelMax(panelMin.x + panelWidth, panelMin.y + panelHeight);
  const ImU32 panelBorder = TavernUiColor(0.96f, 0.56f, 0.20f, 0.94f);
  DrawPanel(draw, panelMin, panelMax, panelBorder, 16.0f);

  ImFont *font = ImGui::GetFont();
  constexpr const char *title = "酒場管理";
  const float titleSizePx = std::clamp(panelHeight * 0.055f, 28.0f, 40.0f);
  draw->AddText(font, titleSizePx,
                ImVec2(panelMin.x + 42.0f, panelMin.y + 27.0f),
                TavernUiColor(1.0f, 0.85f, 0.60f, 1.0f), title);
  draw->AddText(ImVec2(panelMin.x + 44.0f, panelMin.y + 71.0f),
                TavernUiColor(0.80f, 0.73f, 0.64f, 1.0f), "管理項目を選ぶ");

  const ImVec2 closeMin(panelMax.x - 188.0f, panelMin.y + 27.0f);
  const ImVec2 closeMax(panelMax.x - 34.0f, panelMin.y + 67.0f);
  ImGui::SetCursorScreenPos(closeMin);
  const bool closeClicked = ImGui::InvisibleButton(
      "##management-close",
      ImVec2(closeMax.x - closeMin.x, closeMax.y - closeMin.y));
  const bool closeHovered = ImGui::IsItemHovered();
  draw->AddRectFilled(closeMin, closeMax,
                      closeHovered
                          ? TavernUiColor(0.24f, 0.13f, 0.055f, 0.98f)
                          : TavernUiColor(0.08f, 0.050f, 0.032f, 0.94f),
                      7.0f);
  draw->AddRect(closeMin, closeMax,
                TavernUiColor(0.92f, 0.55f, 0.24f, closeHovered ? 1.0f : 0.68f),
                7.0f, 0, closeHovered ? 2.0f : 1.2f);
  constexpr const char *closeLabel = "E / B  閉じる";
  const ImVec2 closeTextSize = ImGui::CalcTextSize(closeLabel);
  draw->AddText(ImVec2((closeMin.x + closeMax.x - closeTextSize.x) * 0.5f,
                       (closeMin.y + closeMax.y - closeTextSize.y) * 0.5f),
                TavernUiColor(0.94f, 0.86f, 0.74f, 1.0f), closeLabel);

  const float sidePadding = std::clamp(panelWidth * 0.055f, 34.0f, 64.0f);
  const float cardGap = std::clamp(panelWidth * 0.032f, 24.0f, 44.0f);
  const float cardWidth = (panelWidth - sidePadding * 2.0f - cardGap) * 0.5f;
  const float cardsTop = panelMin.y + 108.0f;
  const float cardsBottom = panelMax.y - 34.0f;
  const ImVec2 supplierMin(panelMin.x + sidePadding, cardsTop);
  const ImVec2 supplierMax(supplierMin.x + cardWidth, cardsBottom);
  const ImVec2 upgradeMin(supplierMax.x + cardGap, cardsTop);
  const ImVec2 upgradeMax(upgradeMin.x + cardWidth, cardsBottom);

  constexpr const char *suppliesLabel = "Supplies";
  constexpr const char *upgradesLabel = "Upgrades";
  const ManagementCardResult suppliesCard = DrawManagementCard(
      draw, "supplies", suppliesLabel, supplierMin, supplierMax, true,
      m_managementSelection == ManagementSelection::Supplies);
  const ManagementCardResult upgradesCard = DrawManagementCard(
      draw, "upgrades", upgradesLabel, upgradeMin, upgradeMax, false,
      m_managementSelection == ManagementSelection::Upgrades);

  bool openSuppliesAfterDraw = false;
  bool openUpgradesAfterDraw = false;
  if (suppliesCard.clicked) {
    m_managementSelection = ManagementSelection::Supplies;
    ++m_managementActivationSerial;
    openSuppliesAfterDraw = true;
  }
  if (upgradesCard.clicked) {
    m_managementSelection = ManagementSelection::Upgrades;
    ++m_managementActivationSerial;
    openUpgradesAfterDraw = true;
  }

  m_managementUiDiagnostics.suppliesCardRendered =
      suppliesCard.submitted && suppliesCard.supplierArtwork &&
      std::string_view(suppliesCard.label) == suppliesLabel;
  m_managementUiDiagnostics.upgradesCardRendered =
      upgradesCard.submitted && !upgradesCard.supplierArtwork &&
      std::string_view(upgradesCard.label) == upgradesLabel;
  m_managementUiDiagnostics.labelsInRequestedOrder =
      std::string_view(suppliesCard.label) == "Supplies" &&
      std::string_view(upgradesCard.label) == "Upgrades" &&
      supplierMin.x < upgradeMin.x;
  m_managementUiDiagnostics.visualRegionsValid =
      suppliesCard.imageMaximum.x > suppliesCard.imageMinimum.x &&
      suppliesCard.imageMaximum.y > suppliesCard.imageMinimum.y &&
      suppliesCard.labelRegionValid &&
      upgradesCard.imageMaximum.x > upgradesCard.imageMinimum.x &&
      upgradesCard.imageMaximum.y > upgradesCard.imageMinimum.y &&
      upgradesCard.labelRegionValid;
  m_managementUiDiagnostics.cardsDoNotOverlap =
      supplierMax.x < upgradeMin.x && supplierMax.y == upgradeMax.y;
  m_managementUiDiagnostics.selectedCardIndex =
      m_managementSelection == ManagementSelection::Supplies
          ? 0
          : (m_managementSelection == ManagementSelection::Upgrades ? 1 : -1);
  m_managementUiDiagnostics.activationSerial = m_managementActivationSerial;
  m_managementUiDiagnostics.purchaseSerial = m_supplyPurchaseSerial;
  m_managementUiDiagnostics.upgradePurchaseSerial = m_upgradePurchaseSerial;

  if (openSuppliesAfterDraw)
    OpenSuppliesPage();
  if (openUpgradesAfterDraw)
    OpenUpgradesPage();
  if (closeClicked)
    CloseManagementMenu();

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

void TavernScene::DrawSuppliesPage(int viewportWidth, int viewportHeight) {
  if (m_managementPage != ManagementPage::Supplies)
    return;

  const uint64_t renderedFrameSerial =
      m_managementUiDiagnostics.renderedFrameSerial + 1;
  m_managementUiDiagnostics = {};
  m_managementUiDiagnostics.menuOpen = true;
  m_managementUiDiagnostics.subpageOpen = true;
  m_managementUiDiagnostics.suppliesPageRendered = true;
  m_managementUiDiagnostics.activationSerial = m_managementActivationSerial;
  m_managementUiDiagnostics.purchaseSerial = m_supplyPurchaseSerial;
  m_managementUiDiagnostics.upgradePurchaseSerial = m_upgradePurchaseSerial;
  m_managementUiDiagnostics.renderedFrameSerial = renderedFrameSerial;

  const ImVec2 viewportSize(static_cast<float>(viewportWidth),
                            static_cast<float>(viewportHeight));
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(viewportSize, ImGuiCond_Always);
  ImGui::SetNextWindowFocus();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ImGui::Begin("##TavernSuppliesPage", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoScrollWithMouse |
                   ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus);

  ImDrawList *draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(ImVec2(0.0f, 0.0f), viewportSize,
                      TavernUiColor(0.0f, 0.0f, 0.0f, 0.76f));

  const float panelWidth = std::min(std::max(760.0f, viewportSize.x * 0.68f),
                                    std::max(320.0f, viewportSize.x - 40.0f));
  const float panelHeight = std::min(std::max(620.0f, viewportSize.y * 0.82f),
                                     std::max(420.0f, viewportSize.y - 36.0f));
  const ImVec2 panelMin((viewportSize.x - panelWidth) * 0.5f,
                        (viewportSize.y - panelHeight) * 0.5f);
  const ImVec2 panelMax(panelMin.x + panelWidth, panelMin.y + panelHeight);
  const ImU32 orange = TavernUiColor(0.96f, 0.56f, 0.20f, 0.96f);
  DrawPanel(draw, panelMin, panelMax, orange, 16.0f);

  const ImVec2 backMin(panelMin.x + 30.0f, panelMin.y + 25.0f);
  const ImVec2 backMax(backMin.x + 132.0f, backMin.y + 42.0f);
  const ManagementButtonResult backButton = DrawManagementButton(
      draw, "supplies-back", "BACK", backMin, backMax, true);
  const ImVec2 closeMin(panelMax.x - 162.0f, panelMin.y + 25.0f);
  const ImVec2 closeMax(closeMin.x + 132.0f, closeMin.y + 42.0f);
  const ManagementButtonResult closeButton = DrawManagementButton(
      draw, "supplies-close", "CLOSE", closeMin, closeMax, true);

  ImFont *font = ImGui::GetFont();
  constexpr const char *pageTitle = "ORDER SUPPLIES";
  const float titleSizePx = std::clamp(panelHeight * 0.048f, 27.0f, 38.0f);
  const ImVec2 titleSize =
      font->CalcTextSizeA(titleSizePx, FLT_MAX, 0.0f, pageTitle);
  draw->AddText(font, titleSizePx,
                ImVec2((panelMin.x + panelMax.x - titleSize.x) * 0.5f,
                       panelMin.y + 27.0f),
                TavernUiColor(1.0f, 0.86f, 0.62f, 1.0f), pageTitle);
  char textBuffer[96]{};
  std::snprintf(textBuffer, sizeof(textBuffer), "GOLD  %d G", m_gold);
  const ImVec2 goldSize = ImGui::CalcTextSize(textBuffer);
  draw->AddText(
      ImVec2((panelMin.x + panelMax.x - goldSize.x) * 0.5f, panelMin.y + 69.0f),
      TavernUiColor(1.0f, 0.72f, 0.24f, 1.0f), textBuffer);

  const float cardWidth = std::min(420.0f, panelWidth - 90.0f);
  const float cardHeight = std::min(620.0f, panelHeight - 145.0f);
  const ImVec2 cardMin((panelMin.x + panelMax.x - cardWidth) * 0.5f,
                       panelMin.y + 98.0f);
  const ImVec2 cardMax(cardMin.x + cardWidth, cardMin.y + cardHeight);
  draw->AddRectFilled(cardMin, cardMax,
                      TavernUiColor(0.055f, 0.033f, 0.020f, 0.99f), 13.0f);
  draw->AddRect(cardMin, cardMax, orange, 13.0f, 0, 2.0f);

  constexpr const char *aleLabel = "ALE";
  const float aleTitleSizePx = std::clamp(cardHeight * 0.065f, 25.0f, 38.0f);
  const ImVec2 aleTitleSize =
      font->CalcTextSizeA(aleTitleSizePx, FLT_MAX, 0.0f, aleLabel);
  draw->AddText(font, aleTitleSizePx,
                ImVec2((cardMin.x + cardMax.x - aleTitleSize.x) * 0.5f,
                       cardMin.y + 14.0f),
                TavernUiColor(1.0f, 0.88f, 0.68f, 1.0f), aleLabel);

  const ImVec2 imageMin(cardMin.x + 18.0f, cardMin.y + 58.0f);
  const ImVec2 imageMax(cardMax.x - 18.0f, cardMin.y + cardHeight * 0.49f);
  draw->PushClipRect(imageMin, imageMax, true);
  DrawAleSupplyIllustration(draw, imageMin, imageMax);
  draw->PopClipRect();
  draw->AddRect(imageMin, imageMax, TavernUiColor(0.55f, 0.31f, 0.14f, 0.88f),
                7.0f, 0, 1.2f);

  std::snprintf(textBuffer, sizeof(textBuffer), "STOCK  %d / %d", AleStock(),
                AleCapacity());
  const ImVec2 stockSize = ImGui::CalcTextSize(textBuffer);
  draw->AddText(ImVec2((cardMin.x + cardMax.x - stockSize.x) * 0.5f,
                       cardMin.y + cardHeight * 0.515f),
                AleStock() == 0 ? TavernUiColor(1.0f, 0.30f, 0.16f, 1.0f)
                                : TavernUiColor(0.94f, 0.86f, 0.74f, 1.0f),
                textBuffer);
  std::snprintf(textBuffer, sizeof(textBuffer), "%d G EACH",
                kAleSupplyUnitPrice);
  const ImVec2 priceSize = ImGui::CalcTextSize(textBuffer);
  draw->AddText(ImVec2((cardMin.x + cardMax.x - priceSize.x) * 0.5f,
                       cardMin.y + cardHeight * 0.575f),
                TavernUiColor(1.0f, 0.68f, 0.22f, 1.0f), textBuffer);

  const float quantityTop = cardMin.y + cardHeight * 0.645f;
  const float quantityHeight = std::clamp(cardHeight * 0.09f, 38.0f, 52.0f);
  const float quantityButtonWidth = std::clamp(cardWidth * 0.16f, 52.0f, 68.0f);
  const ImVec2 minusMin(cardMin.x + cardWidth * 0.20f, quantityTop);
  const ImVec2 minusMax(minusMin.x + quantityButtonWidth,
                        quantityTop + quantityHeight);
  const ImVec2 plusMax(cardMax.x - cardWidth * 0.20f,
                       quantityTop + quantityHeight);
  const ImVec2 plusMin(plusMax.x - quantityButtonWidth, quantityTop);
  const ManagementButtonResult minusButton = DrawManagementButton(
      draw, "ale-minus", "-", minusMin, minusMax, m_aleOrderQuantity > 0);
  const ManagementButtonResult plusButton =
      DrawManagementButton(draw, "ale-plus", "+", plusMin, plusMax,
                           m_aleOrderQuantity < MaximumAleOrderQuantity());
  std::snprintf(textBuffer, sizeof(textBuffer), "%d", m_aleOrderQuantity);
  const ImVec2 quantitySize = ImGui::CalcTextSize(textBuffer);
  draw->AddText(ImVec2((cardMin.x + cardMax.x - quantitySize.x) * 0.5f,
                       quantityTop + (quantityHeight - quantitySize.y) * 0.5f),
                TavernUiColor(1.0f, 0.92f, 0.78f, 1.0f), textBuffer);

  std::snprintf(textBuffer, sizeof(textBuffer), "AFTER  %d / %d",
                AleStock() + m_aleOrderQuantity, AleCapacity());
  const ImVec2 afterSize = ImGui::CalcTextSize(textBuffer);
  draw->AddText(ImVec2((cardMin.x + cardMax.x - afterSize.x) * 0.5f,
                       cardMin.y + cardHeight * 0.765f),
                TavernUiColor(0.74f, 0.78f, 0.70f, 1.0f), textBuffer);

  const SupplyOrderBlockReason blockReason = CurrentAleOrderBlockReason();
  const int orderTotal = m_aleOrderQuantity * kAleSupplyUnitPrice;
  const bool orderEnabled = blockReason == SupplyOrderBlockReason::None;
  const char *orderLabel = "SELECT AMOUNT";
  if (blockReason == SupplyOrderBlockReason::Full)
    orderLabel = "FULL";
  else if (blockReason == SupplyOrderBlockReason::InsufficientGold)
    orderLabel = "NOT ENOUGH GOLD";
  else if (orderEnabled) {
    std::snprintf(textBuffer, sizeof(textBuffer), "ORDER  %d G", orderTotal);
    orderLabel = textBuffer;
  }
  const ImVec2 orderMin(cardMin.x + 24.0f, cardMin.y + cardHeight * 0.835f);
  const ImVec2 orderMax(cardMax.x - 24.0f, cardMax.y - 18.0f);
  const ManagementButtonResult orderButton = DrawManagementButton(
      draw, "ale-order", orderLabel, orderMin, orderMax, orderEnabled);

  draw->AddText(ImVec2(panelMin.x + 32.0f, panelMax.y - 31.0f),
                TavernUiColor(0.72f, 0.66f, 0.58f, 1.0f), "B / ESC  BACK");
  constexpr const char *closeHint = "E  CLOSE";
  const ImVec2 closeHintSize = ImGui::CalcTextSize(closeHint);
  draw->AddText(
      ImVec2(panelMax.x - 32.0f - closeHintSize.x, panelMax.y - 31.0f),
      TavernUiColor(0.72f, 0.66f, 0.58f, 1.0f), closeHint);

  if (minusButton.clicked)
    AdjustAleOrderQuantity(-1);
  if (plusButton.clicked)
    AdjustAleOrderQuantity(1);
  if (orderButton.clicked)
    TryOrderAle();

  m_managementUiDiagnostics.aleCardRendered =
      std::string_view(aleLabel) == "ALE" && imageMax.x > imageMin.x &&
      imageMax.y > imageMin.y;
  m_managementUiDiagnostics.backControlRendered = true;
  m_managementUiDiagnostics.visualRegionsValid =
      cardMax.x > cardMin.x && cardMax.y > cardMin.y &&
      orderMax.x > orderMin.x && orderMax.y > orderMin.y;
  m_managementUiDiagnostics.cardsDoNotOverlap = true;
  m_managementUiDiagnostics.aleStock = AleStock();
  m_managementUiDiagnostics.aleCapacity = AleCapacity();
  m_managementUiDiagnostics.orderQuantity = m_aleOrderQuantity;
  m_managementUiDiagnostics.orderUnitPrice = kAleSupplyUnitPrice;
  m_managementUiDiagnostics.orderTotal =
      m_aleOrderQuantity * kAleSupplyUnitPrice;
  m_managementUiDiagnostics.aleStockAfterOrder =
      AleStock() + m_aleOrderQuantity;
  m_managementUiDiagnostics.orderBlockReason = CurrentAleOrderBlockReason();
  m_managementUiDiagnostics.orderCanPurchase =
      CurrentAleOrderBlockReason() == SupplyOrderBlockReason::None;
  m_managementUiDiagnostics.activationSerial = m_managementActivationSerial;
  m_managementUiDiagnostics.purchaseSerial = m_supplyPurchaseSerial;
  m_managementUiDiagnostics.upgradePurchaseSerial = m_upgradePurchaseSerial;

  if (backButton.clicked)
    BackToManagementRoot();
  if (closeButton.clicked)
    CloseManagementMenu();

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

void TavernScene::DrawUpgradesPage(int viewportWidth, int viewportHeight) {
  if (m_managementPage != ManagementPage::Upgrades)
    return;

  const uint64_t renderedFrameSerial =
      m_managementUiDiagnostics.renderedFrameSerial + 1;
  m_managementUiDiagnostics = {};
  m_managementUiDiagnostics.menuOpen = true;
  m_managementUiDiagnostics.subpageOpen = true;
  m_managementUiDiagnostics.upgradesPageRendered = true;
  m_managementUiDiagnostics.activationSerial = m_managementActivationSerial;
  m_managementUiDiagnostics.purchaseSerial = m_supplyPurchaseSerial;
  m_managementUiDiagnostics.upgradePurchaseSerial = m_upgradePurchaseSerial;
  m_managementUiDiagnostics.renderedFrameSerial = renderedFrameSerial;

  const ImVec2 viewportSize(static_cast<float>(viewportWidth),
                            static_cast<float>(viewportHeight));
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(viewportSize, ImGuiCond_Always);
  ImGui::SetNextWindowFocus();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ImGui::Begin("##TavernUpgradesPage", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoScrollWithMouse |
                   ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus);

  ImDrawList *draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(ImVec2(0.0f, 0.0f), viewportSize,
                      TavernUiColor(0.0f, 0.0f, 0.0f, 0.77f));

  const float panelWidth = std::min(std::max(980.0f, viewportSize.x * 0.86f),
                                    std::max(520.0f, viewportSize.x - 36.0f));
  const float panelHeight = std::min(std::max(620.0f, viewportSize.y * 0.82f),
                                     std::max(440.0f, viewportSize.y - 36.0f));
  const ImVec2 panelMin((viewportSize.x - panelWidth) * 0.5f,
                        (viewportSize.y - panelHeight) * 0.5f);
  const ImVec2 panelMax(panelMin.x + panelWidth, panelMin.y + panelHeight);
  const ImU32 teal = TavernUiColor(0.30f, 0.90f, 0.72f, 0.96f);
  DrawPanel(draw, panelMin, panelMax, teal, 16.0f);

  const bool headerControlsEnabled = !m_upgradeConfirmationOpen;
  const ImVec2 backMin(panelMin.x + 30.0f, panelMin.y + 25.0f);
  const ImVec2 backMax(backMin.x + 132.0f, backMin.y + 42.0f);
  const ManagementButtonResult backButton =
      DrawManagementButton(draw, "upgrades-back", "BACK", backMin, backMax,
                           headerControlsEnabled, teal);
  const ImVec2 closeMin(panelMax.x - 162.0f, panelMin.y + 25.0f);
  const ImVec2 closeMax(closeMin.x + 132.0f, closeMin.y + 42.0f);
  const ManagementButtonResult closeButton =
      DrawManagementButton(draw, "upgrades-close", "CLOSE", closeMin, closeMax,
                           headerControlsEnabled, teal);

  ImFont *font = ImGui::GetFont();
  constexpr const char *pageTitle = "TAVERN UPGRADES";
  const float titleSizePx = std::clamp(panelHeight * 0.050f, 28.0f, 39.0f);
  const ImVec2 titleSize =
      font->CalcTextSizeA(titleSizePx, FLT_MAX, 0.0f, pageTitle);
  draw->AddText(font, titleSizePx,
                ImVec2((panelMin.x + panelMax.x - titleSize.x) * 0.5f,
                       panelMin.y + 25.0f),
                TavernUiColor(0.78f, 1.0f, 0.90f, 1.0f), pageTitle);
  char textBuffer[128]{};
  std::snprintf(textBuffer, sizeof(textBuffer), "GOLD  %d G", m_gold);
  const ImVec2 goldSize = ImGui::CalcTextSize(textBuffer);
  draw->AddText(
      ImVec2((panelMin.x + panelMax.x - goldSize.x) * 0.5f, panelMin.y + 70.0f),
      TavernUiColor(1.0f, 0.72f, 0.24f, 1.0f), textBuffer);
  if (!m_feedbackText.empty() && m_feedbackTimer > 0.0f) {
    const ImVec2 feedbackSize = ImGui::CalcTextSize(m_feedbackText.c_str());
    draw->AddText(ImVec2((panelMin.x + panelMax.x - feedbackSize.x) * 0.5f,
                         panelMin.y + 91.0f),
                  TavernUiColor(0.70f, 1.0f, 0.84f,
                                std::clamp(m_feedbackTimer, 0.35f, 1.0f)),
                  m_feedbackText.c_str());
  }

  constexpr int cardCount = static_cast<int>(UpgradeId::Count);
  const float sidePadding = std::clamp(panelWidth * 0.035f, 28.0f, 52.0f);
  const float cardGap = std::clamp(panelWidth * 0.020f, 18.0f, 30.0f);
  const float cardWidth =
      (panelWidth - sidePadding * 2.0f - cardGap * (cardCount - 1)) /
      static_cast<float>(cardCount);
  const float cardsTop = panelMin.y + 108.0f;
  const float cardsBottom = panelMax.y - 55.0f;
  bool allCardsSubmitted = true;
  bool cardsDoNotOverlap = true;
  float previousCardRight = -FLT_MAX;

  for (int cardIndex = 0; cardIndex < cardCount; ++cardIndex) {
    const UpgradeId upgrade = static_cast<UpgradeId>(cardIndex);
    const int level = UpgradeLevel(upgrade);
    const int maximumLevel = UpgradeMaximumLevel(upgrade);
    const int price = UpgradePrice(upgrade);
    const UpgradePurchaseBlockReason blockReason =
        CurrentUpgradeBlockReason(upgrade);
    const bool purchasable = blockReason == UpgradePurchaseBlockReason::None;

    const char *title = "UPGRADE";
    const char *note = "PERMANENT";
    UpgradeArtwork artwork = UpgradeArtwork::Mug;
    std::array<char, 96> effect{};
    std::array<char, 96> status{};
    if (upgrade == UpgradeId::ExtraMug) {
      title = "EXTRA MUG";
      note = "ONE-TIME UPGRADE";
      artwork = UpgradeArtwork::Mug;
      std::snprintf(effect.data(), effect.size(), "%s",
                    level == 0 ? "MUGS  2 -> 3" : "MUGS  3 / 3");
    } else if (upgrade == UpgradeId::AleCapacity) {
      title = "ALE CAPACITY";
      note = "DOES NOT REFILL";
      artwork = UpgradeArtwork::AleCapacity;
      if (level < maximumLevel) {
        std::snprintf(effect.data(), effect.size(), "ALE MAX  %d -> %d",
                      AleCapacity(), AleCapacity() + 1);
      } else {
        std::snprintf(effect.data(), effect.size(), "ALE MAX  %d / %d",
                      AleCapacity(), kInitialAleCapacity + maximumLevel);
      }
    } else {
      title = "OPEN TABLE 3";
      note = "MORE CUSTOMERS";
      artwork = UpgradeArtwork::Table;
      std::snprintf(effect.data(), effect.size(), "%s",
                    m_table3Unlocked ? "TABLE 3  OPEN" : "UNLOCK TABLE 3");
    }

    switch (blockReason) {
    case UpgradePurchaseBlockReason::None:
      std::snprintf(status.data(), status.size(), "BUY  %d G", price);
      break;
    case UpgradePurchaseBlockReason::InsufficientGold:
      std::snprintf(status.data(), status.size(), "NEED  %d G", price);
      break;
    case UpgradePurchaseBlockReason::Owned:
      std::snprintf(status.data(), status.size(),
                    upgrade == UpgradeId::OpenTable3 ? "OPEN - OWNED"
                                                     : "OWNED - 3 MUGS");
      break;
    case UpgradePurchaseBlockReason::Maxed:
      std::snprintf(status.data(), status.size(), "MAXED - LEVEL %d / %d",
                    level, maximumLevel);
      break;
    case UpgradePurchaseBlockReason::TutorialRequired:
      std::snprintf(status.data(), status.size(), "COMPLETE TUTORIAL");
      break;
    case UpgradePurchaseBlockReason::EmergencyAleReserve:
      std::snprintf(status.data(), status.size(), "KEEP 2 G FOR ALE");
      break;
    }

    const ImVec2 cardMin(
        panelMin.x + sidePadding + cardIndex * (cardWidth + cardGap), cardsTop);
    const ImVec2 cardMax(cardMin.x + cardWidth, cardsBottom);
    const UpgradeCardResult card = DrawUpgradeCard(
        draw, title, title, effect.data(), note, status.data(), artwork, level,
        cardMin, cardMax, m_selectedUpgradeIndex == cardIndex, purchasable,
        !m_upgradeConfirmationOpen);
    allCardsSubmitted = allCardsSubmitted && card.submitted;
    if (cardIndex > 0)
      cardsDoNotOverlap = cardsDoNotOverlap && previousCardRight < cardMin.x;
    previousCardRight = cardMax.x;
    if (card.clicked && !m_upgradeConfirmationOpen) {
      m_selectedUpgradeIndex = cardIndex;
      RequestUpgradePurchase(upgrade);
    }
  }

  draw->AddText(ImVec2(panelMin.x + 32.0f, panelMax.y - 31.0f),
                TavernUiColor(0.64f, 0.72f, 0.67f, 1.0f),
                "LEFT / RIGHT  SELECT     ENTER / A  CONFIRM");
  constexpr const char *closeHint = "B / ESC  BACK     E  CLOSE";
  const ImVec2 closeHintSize = ImGui::CalcTextSize(closeHint);
  draw->AddText(
      ImVec2(panelMax.x - 32.0f - closeHintSize.x, panelMax.y - 31.0f),
      TavernUiColor(0.64f, 0.72f, 0.67f, 1.0f), closeHint);

  if (m_upgradeConfirmationOpen) {
    const UpgradeId upgrade = static_cast<UpgradeId>(m_selectedUpgradeIndex);
    const int price = UpgradePrice(upgrade);
    const char *title =
        upgrade == UpgradeId::ExtraMug
            ? "EXTRA MUG"
            : (upgrade == UpgradeId::AleCapacity ? "ALE CAPACITY"
                                                 : "OPEN TABLE 3");
    std::array<char, 96> confirmEffect{};
    if (upgrade == UpgradeId::ExtraMug) {
      std::snprintf(confirmEffect.data(), confirmEffect.size(), "MUGS  2 -> 3");
    } else if (upgrade == UpgradeId::AleCapacity) {
      std::snprintf(confirmEffect.data(), confirmEffect.size(),
                    "ALE MAX  %d -> %d   -   NO REFILL", AleCapacity(),
                    AleCapacity() + 1);
    } else {
      std::snprintf(confirmEffect.data(), confirmEffect.size(),
                    "UNLOCK TABLE 3");
    }
    draw->AddRectFilled(ImVec2(0.0f, 0.0f), viewportSize,
                        TavernUiColor(0.0f, 0.0f, 0.0f, 0.70f));
    const ImVec2 confirmSize(std::min(570.0f, viewportSize.x - 48.0f),
                             std::min(330.0f, viewportSize.y - 48.0f));
    const ImVec2 confirmMin((viewportSize.x - confirmSize.x) * 0.5f,
                            (viewportSize.y - confirmSize.y) * 0.5f);
    const ImVec2 confirmMax(confirmMin.x + confirmSize.x,
                            confirmMin.y + confirmSize.y);
    DrawPanel(draw, confirmMin, confirmMax, teal, 15.0f);
    constexpr const char *confirmTitle = "CONFIRM UPGRADE";
    const ImVec2 confirmTitleSize = ImGui::CalcTextSize(confirmTitle);
    draw->AddText(
        ImVec2((confirmMin.x + confirmMax.x - confirmTitleSize.x) * 0.5f,
               confirmMin.y + 27.0f),
        TavernUiColor(0.76f, 1.0f, 0.90f, 1.0f), confirmTitle);
    const ImVec2 upgradeTitleSize = ImGui::CalcTextSize(title);
    draw->AddText(
        ImVec2((confirmMin.x + confirmMax.x - upgradeTitleSize.x) * 0.5f,
               confirmMin.y + 75.0f),
        TavernUiColor(1.0f, 0.88f, 0.68f, 1.0f), title);
    const ImVec2 effectSize = ImGui::CalcTextSize(confirmEffect.data());
    draw->AddText(ImVec2((confirmMin.x + confirmMax.x - effectSize.x) * 0.5f,
                         confirmMin.y + 108.0f),
                  TavernUiColor(0.76f, 0.86f, 0.80f, 1.0f),
                  confirmEffect.data());
    std::snprintf(textBuffer, sizeof(textBuffer), "PRICE  %d G", price);
    const ImVec2 priceSize = ImGui::CalcTextSize(textBuffer);
    draw->AddText(ImVec2((confirmMin.x + confirmMax.x - priceSize.x) * 0.5f,
                         confirmMin.y + 143.0f),
                  TavernUiColor(1.0f, 0.70f, 0.24f, 1.0f), textBuffer);
    std::snprintf(textBuffer, sizeof(textBuffer), "GOLD AFTER  %d G",
                  m_gold - price);
    const ImVec2 afterSize = ImGui::CalcTextSize(textBuffer);
    draw->AddText(ImVec2((confirmMin.x + confirmMax.x - afterSize.x) * 0.5f,
                         confirmMin.y + 174.0f),
                  TavernUiColor(0.76f, 0.82f, 0.78f, 1.0f), textBuffer);
    constexpr const char *cancelHint = "B / ESC  CANCEL";
    const ImVec2 cancelHintSize = ImGui::CalcTextSize(cancelHint);
    draw->AddText(
        ImVec2((confirmMin.x + confirmMax.x - cancelHintSize.x) * 0.5f,
               confirmMin.y + 207.0f),
        TavernUiColor(0.58f, 0.66f, 0.62f, 1.0f), cancelHint);

    const float buttonGap = 18.0f;
    const float buttonWidth = (confirmSize.x - 78.0f - buttonGap) * 0.5f;
    const ImVec2 cancelMin(confirmMin.x + 39.0f, confirmMax.y - 84.0f);
    const ImVec2 cancelMax(cancelMin.x + buttonWidth, confirmMax.y - 34.0f);
    const ImVec2 buyMin(cancelMax.x + buttonGap, cancelMin.y);
    const ImVec2 buyMax(buyMin.x + buttonWidth, cancelMax.y);
    const ManagementButtonResult cancelButton =
        DrawManagementButton(draw, "upgrade-confirm-cancel", "CANCEL",
                             cancelMin, cancelMax, true, teal);
    const ManagementButtonResult buyButton = DrawManagementButton(
        draw, "upgrade-confirm-buy", "BUY", buyMin, buyMax, true, teal);
    draw->AddRect(m_upgradeConfirmBuySelected ? buyMin : cancelMin,
                  m_upgradeConfirmBuySelected ? buyMax : cancelMax,
                  TavernUiColor(1.0f, 0.82f, 0.34f, 1.0f), 7.0f, 0, 3.0f);
    if (cancelButton.clicked) {
      m_upgradeConfirmationOpen = false;
      m_upgradeConfirmBuySelected = false;
    }
    if (buyButton.clicked)
      TryPurchaseUpgrade(upgrade);
    m_managementUiDiagnostics.upgradeConfirmationRendered = true;
  }

  const UpgradeId selectedUpgrade =
      static_cast<UpgradeId>(m_selectedUpgradeIndex);
  m_managementUiDiagnostics.upgradeCardsRendered = allCardsSubmitted;
  m_managementUiDiagnostics.backControlRendered = true;
  m_managementUiDiagnostics.visualRegionsValid =
      cardWidth > 0.0f && cardsBottom > cardsTop;
  m_managementUiDiagnostics.cardsDoNotOverlap = cardsDoNotOverlap;
  m_managementUiDiagnostics.selectedCardIndex = 1;
  m_managementUiDiagnostics.selectedUpgradeIndex = m_selectedUpgradeIndex;
  m_managementUiDiagnostics.selectedUpgradePrice =
      UpgradePrice(selectedUpgrade);
  m_managementUiDiagnostics.selectedUpgradeLevel =
      UpgradeLevel(selectedUpgrade);
  m_managementUiDiagnostics.extraMugCapacity = TotalMugs();
  m_managementUiDiagnostics.aleCapacityLevel = AleCapacityLevel();
  m_managementUiDiagnostics.table3Unlocked = m_table3Unlocked;
  m_managementUiDiagnostics.upgradeBlockReason =
      CurrentUpgradeBlockReason(selectedUpgrade);
  m_managementUiDiagnostics.upgradeCanPurchase =
      m_managementUiDiagnostics.upgradeBlockReason ==
      UpgradePurchaseBlockReason::None;
  m_managementUiDiagnostics.activationSerial = m_managementActivationSerial;
  m_managementUiDiagnostics.purchaseSerial = m_supplyPurchaseSerial;
  m_managementUiDiagnostics.upgradePurchaseSerial = m_upgradePurchaseSerial;

  if (backButton.clicked)
    BackToManagementRoot();
  if (closeButton.clicked)
    CloseManagementMenu();

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

void TavernScene::DrawManagementUi(int viewportWidth, int viewportHeight) {
  if (!ManagementMenuOpen()) {
    const uint64_t renderedFrameSerial =
        m_managementUiDiagnostics.renderedFrameSerial;
    m_managementUiDiagnostics = {};
    m_managementUiDiagnostics.renderedFrameSerial = renderedFrameSerial;
    m_managementUiDiagnostics.activationSerial = m_managementActivationSerial;
    m_managementUiDiagnostics.purchaseSerial = m_supplyPurchaseSerial;
    m_managementUiDiagnostics.upgradePurchaseSerial = m_upgradePurchaseSerial;
    return;
  }

  switch (m_managementPage) {
  case ManagementPage::Root:
    DrawManagementRoot(viewportWidth, viewportHeight);
    break;
  case ManagementPage::Supplies:
    DrawSuppliesPage(viewportWidth, viewportHeight);
    break;
  case ManagementPage::Upgrades:
    DrawUpgradesPage(viewportWidth, viewportHeight);
    break;
  case ManagementPage::Closed:
    break;
  }
}

TavernScene::Action TavernScene::DrawHud(int viewportWidth,
                                         int viewportHeight,
                                         bool firstPersonView) {
  const ImVec2 viewportSize(static_cast<float>(viewportWidth),
                            static_cast<float>(viewportHeight));
  const ImU32 goldBorder = TavernUiColor(0.92f, 0.55f, 0.24f, 0.80f);

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(viewportSize, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ImGui::Begin("##TavernHud", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoScrollWithMouse |
                   ImGuiWindowFlags_NoInputs);

  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 statusMin(28.0f, 28.0f);
  const bool showSleepStatus = IsAfterMidnight() ||
                               m_shiftState == ShiftState::Closing;
  const ImVec2 statusMax(348.0f, showSleepStatus ? 174.0f : 142.0f);
  DrawPanel(draw, statusMin, statusMax, goldBorder);
  draw->AddText(ImVec2(50.0f, 45.0f), TavernUiColor(1.0f, 0.84f, 0.62f, 1.0f),
                "水鏡亭");
  char line[160]{};
  const int businessMinutes =
      static_cast<int>(std::lround(m_businessHour * 60.0f));
  std::snprintf(line, sizeof(line), "時刻  %02d:%02d    売上  %d G",
                businessMinutes / 60, businessMinutes % 60, m_gold);
  draw->AddText(ImVec2(50.0f, 76.0f), TavernUiColor(0.95f, 0.88f, 0.78f, 1.0f),
                line);
  std::snprintf(line, sizeof(line), "%s　提供 %d　退店 %d",
                CustomerTrafficName(), m_servedCustomers, m_walkouts);
  draw->AddText(ImVec2(50.0f, 106.0f), TavernUiColor(0.82f, 0.76f, 0.68f, 1.0f),
                line);
  if (showSleepStatus) {
    draw->AddText(ImVec2(50.0f, 136.0f),
                  TavernUiColor(0.68f, 0.82f, 1.0f, 1.0f),
                  m_shiftState == ShiftState::Closing ? "店内の客を見送って就寝"
                                                      : "R：受付を止めて就寝");
  }

  const int visibleTableCount = CustomerTableCount();
  const float orderWidth =
      visibleTableCount == 3
          ? std::clamp(viewportSize.x - 800.0f, 560.0f, 900.0f)
          : std::clamp(viewportSize.x - 760.0f, 520.0f, 680.0f);
  const ImVec2 orderMin((viewportSize.x - orderWidth) * 0.5f, 18.0f);
  const ImVec2 orderMax(orderMin.x + orderWidth, 136.0f);
  DrawPanel(draw, orderMin, orderMax, goldBorder);
  const float tableCardWidth =
      orderWidth / static_cast<float>(visibleTableCount);
  for (int divider = 1; divider < visibleTableCount; ++divider) {
    const float dividerX = orderMin.x + tableCardWidth * divider;
    draw->AddLine(ImVec2(dividerX, orderMin.y + 8.0f),
                  ImVec2(dividerX, orderMax.y - 8.0f),
                  TavernUiColor(0.45f, 0.27f, 0.14f, 0.82f), 1.0f);
  }
  for (int tableIndex = 0; tableIndex < visibleTableCount; ++tableIndex) {
    const TableSlot &table = m_tables[tableIndex];
    const EntranceCustomer &entrance = m_entranceCustomers[tableIndex];
    const bool waitingAtEntrance = entrance.state == EntranceState::Waiting;
    const bool leavingEntrance = entrance.state == EntranceState::Leaving;
    const float cardX = orderMin.x + tableCardWidth * tableIndex;
    draw->PushClipRect(ImVec2(cardX + 2.0f, orderMin.y + 2.0f),
                       ImVec2(cardX + tableCardWidth - 2.0f, orderMax.y - 2.0f),
                       true);
    std::snprintf(line, sizeof(line), "TABLE %d%s", tableIndex + 1,
                  table.enabled ? "" : "  準備中");
    draw->AddText(ImVec2(cardX + 18.0f, orderMin.y + 10.0f),
                  TavernUiColor(1.0f, 0.84f, 0.62f, 1.0f), line);
    const char *orderText = waitingAtEntrance ? "入口待ち／要回収"
                           : leavingEntrance ? "入口の客が退店中"
                           : !table.enabled
                                ? "初回研修後に開放"
                                : GetTableStateName(table.state);
    draw->AddText(ImVec2(cardX + 18.0f, orderMin.y + 32.0f),
                  TavernUiColor(0.94f, 0.87f, 0.77f, 1.0f), orderText);
    if (waitingAtEntrance)
      std::snprintf(line, sizeof(line), "待機 %.0f / %.0f秒",
                    entrance.waitedSeconds, kEntranceWaitSeconds);
    else
      std::snprintf(line, sizeof(line), "満足度 %.0f",
                    std::clamp(table.satisfaction, 0.0f, 100.0f));
    draw->AddText(ImVec2(cardX + 18.0f, orderMin.y + 54.0f),
                  TavernUiColor(0.82f, 0.76f, 0.68f, 1.0f), line);
    const ImVec2 satMin(cardX + 18.0f, orderMin.y + 74.0f);
    const ImVec2 satMax(cardX + tableCardWidth - 18.0f, orderMin.y + 82.0f);
    draw->AddRectFilled(satMin, satMax,
                        TavernUiColor(0.15f, 0.11f, 0.08f, 1.0f), 4.0f);
    const float satisfaction = waitingAtEntrance
        ? std::clamp(entrance.waitedSeconds / kEntranceWaitSeconds, 0.0f, 1.0f)
        : (table.enabled ? std::clamp(table.satisfaction / 100.0f, 0.0f, 1.0f)
                         : 0.0f);
    const ImU32 satisfactionColor =
        waitingAtEntrance
            ? (satisfaction >= 0.75f ? TavernUiColor(0.92f, 0.22f, 0.14f, 1.0f)
                                     : TavernUiColor(1.0f, 0.65f, 0.20f, 1.0f))
            : (satisfaction < 0.35f ? TavernUiColor(0.92f, 0.22f, 0.14f, 1.0f)
                                    : TavernUiColor(0.28f, 0.78f, 0.48f, 1.0f));
    draw->AddRectFilled(
        satMin,
        ImVec2(satMin.x + (satMax.x - satMin.x) * satisfaction, satMax.y),
        satisfactionColor, 4.0f);
    if (waitingAtEntrance || (!table.speech.empty() && table.speechTimer > 0.0f)) {
      const ImVec2 speechMin(cardX + 18.0f, orderMin.y + 88.0f);
      const ImVec2 speechMax(cardX + tableCardWidth - 18.0f,
                             orderMin.y + 112.0f);
      draw->AddRectFilled(speechMin, speechMax,
                          TavernUiColor(0.18f, 0.12f, 0.075f, 0.96f), 7.0f);
      const std::string speechText = waitingAtEntrance
          ? (m_tutorialStep == TutorialStep::Complete ? "満了：最大20G損失"
                                                     : "研修中：時間停止")
          : "「" + table.speech + "」";
      draw->AddText(ImVec2(speechMin.x + 8.0f, speechMin.y + 3.0f),
                    TavernUiColor(1.0f, 0.91f, 0.72f, 1.0f),
                    speechText.c_str());
    }
    draw->PopClipRect();
  }

  if (firstPersonView && !ManagementMenuOpen() &&
      m_workState == WorkState::None) {
    const ImVec2 center(viewportSize.x * 0.5f, viewportSize.y * 0.5f);
    const ImU32 shadow = TavernUiColor(0.02f, 0.01f, 0.005f, 0.72f);
    const ImU32 reticle = TavernUiColor(1.0f, 0.82f, 0.50f, 0.88f);
    draw->AddCircleFilled(center, 3.4f, shadow, 16);
    draw->AddCircle(center, 2.2f, reticle, 16, 1.4f);
  }

  const ImVec2 heldMin(viewportSize.x - 278.0f, 28.0f);
  const ImVec2 heldMax(viewportSize.x - 28.0f, 252.0f);
  DrawPanel(draw, heldMin, heldMax, goldBorder);
  draw->AddText(ImVec2(heldMin.x + 18.0f, heldMin.y + 14.0f),
                TavernUiColor(0.72f, 0.66f, 0.58f, 1.0f), "持ち物");
  draw->AddText(ImVec2(heldMin.x + 18.0f, heldMin.y + 43.0f),
                TavernUiColor(1.0f, 0.82f, 0.56f, 1.0f),
                GetHeldItemName(m_heldItem));
  std::snprintf(line, sizeof(line), "棚のジョッキ  %d / %d", m_cleanMugs,
                TotalMugs());
  draw->AddText(ImVec2(heldMin.x + 18.0f, heldMin.y + 75.0f),
                 TavernUiColor(0.82f, 0.76f, 0.68f, 1.0f), line);
  std::snprintf(line, sizeof(line), "厨房のボウル  %d / %d", m_cleanBowls,
                TotalBowls());
  draw->AddText(ImVec2(heldMin.x + 18.0f, heldMin.y + 105.0f),
                TavernUiColor(0.82f, 0.76f, 0.68f, 1.0f), line);
  std::snprintf(line, sizeof(line), "ALE STOCK  %d / %d", AleStock(),
                AleCapacity());
  draw->AddText(ImVec2(heldMin.x + 18.0f, heldMin.y + 135.0f),
                AleStock() == 0 ? TavernUiColor(1.0f, 0.30f, 0.16f, 1.0f)
                                : TavernUiColor(1.0f, 0.72f, 0.28f, 1.0f),
                line);
  if (m_kitchenState == KitchenState::Cooking) {
    std::snprintf(line, sizeof(line), "厨房  煮込み中 %d%%",
                  static_cast<int>(KitchenProgress() * 100.0f));
  } else if (m_kitchenState == KitchenState::Ready) {
    std::snprintf(line, sizeof(line), "厨房  料理完成");
  } else {
    std::snprintf(line, sizeof(line), "厨房  待機中");
  }
  draw->AddText(ImVec2(heldMin.x + 18.0f, heldMin.y + 165.0f),
                m_kitchenState == KitchenState::Ready
                    ? TavernUiColor(0.38f, 1.0f, 0.58f, 1.0f)
                    : TavernUiColor(1.0f, 0.62f, 0.24f, 1.0f),
                line);
  draw->AddText(ImVec2(heldMin.x + 18.0f, heldMin.y + 195.0f),
                TavernUiColor(0.66f, 0.70f, 0.64f, 1.0f),
                "料理提供  高報酬 / ボウル回収");

  if (m_tutorialStep != TutorialStep::Complete) {
    std::string tutorialTitle;
    std::string tutorialLineOne;
    std::string tutorialLineTwo;
    switch (m_tutorialStep) {
    case TutorialStep::TakeOrder: {
      tutorialTitle = "初回チュートリアル　STEP 1 / 6";
      const int orderTable = FindTableInState(TableState::WaitingOrder);
      if (orderTable >= 0) {
        tutorialLineOne = "左側の TABLE " + std::to_string(orderTable + 1) +
                          "、光る印へ近づく";
        tutorialLineTwo = "下の表示に E が出たら、E で注文を聞く";
      } else {
        tutorialLineOne = "左側の TABLE 1 を見よう";
        tutorialLineTwo = "お客が席に着くまで少し待つ";
      }
      break;
    }
    case TutorialStep::GetMug:
      tutorialTitle = "初回チュートリアル　STEP 2 / 6";
      tutorialLineOne = "カウンター左のジョッキ棚へ行く";
      tutorialLineTwo = "光る印へ近づき、E で空のジョッキを取る";
      break;
    case TutorialStep::StartPour:
      tutorialTitle = "初回チュートリアル　STEP 3 / 6";
      tutorialLineOne = "カウンター中央のエール樽へ運ぶ";
      tutorialLineTwo = "光る印へ近づき、E で注ぎ始める";
      break;
    case TutorialStep::PourAle:
      tutorialTitle = "初回チュートリアル　STEP 3 / 6";
      tutorialLineOne = "左クリックを押し続けてエールを注ぐ";
      tutorialLineTwo = "82～96% の緑色範囲で離す";
      break;
    case TutorialStep::ServeAle:
      tutorialTitle = "初回チュートリアル　STEP 4 / 6";
      tutorialLineOne = "TABLE " + std::to_string(m_tutorialTableIndex + 1) +
                        " へエールを運ぶ";
      tutorialLineTwo = "客の近くで E：エールを渡す";
      break;
    case TutorialStep::CollectMug:
      tutorialTitle = "初回チュートリアル　STEP 5 / 6";
      if (m_tables[m_tutorialTableIndex].state == TableState::Dirty) {
        tutorialLineOne =
            "TABLE " + std::to_string(m_tutorialTableIndex + 1) + " へ戻る";
        tutorialLineTwo = "客の使用済みジョッキを E で回収する";
      } else {
        tutorialLineOne = "客が飲み終わるまで少し待つ";
        tutorialLineTwo = "テーブルに使用済みジョッキが残る";
      }
      break;
    case TutorialStep::WashMug:
      tutorialTitle = "初回チュートリアル　STEP 6 / 6";
      tutorialLineOne = "カウンター右の洗い場へ運ぶ";
      tutorialLineTwo = "E の後、左クリック長押しで洗う";
      break;
    case TutorialStep::Complete:
      break;
    }

    const ImVec2 tutorialMin(28.0f, 200.0f);
    const ImVec2 tutorialMax(448.0f, 328.0f);
    DrawPanel(draw, tutorialMin, tutorialMax,
              TavernUiColor(1.0f, 0.72f, 0.18f, 0.92f), 12.0f);
    draw->AddText(ImVec2(tutorialMin.x + 20.0f, tutorialMin.y + 16.0f),
                  TavernUiColor(1.0f, 0.84f, 0.52f, 1.0f),
                  tutorialTitle.c_str());
    draw->AddText(ImVec2(tutorialMin.x + 20.0f, tutorialMin.y + 51.0f),
                  TavernUiColor(0.96f, 0.90f, 0.82f, 1.0f),
                  tutorialLineOne.c_str());
    draw->AddText(ImVec2(tutorialMin.x + 20.0f, tutorialMin.y + 82.0f),
                  TavernUiColor(0.82f, 0.76f, 0.68f, 1.0f),
                  tutorialLineTwo.c_str());
  }

  if (m_workState == WorkState::PouringAle) {
    const ImVec2 panelSize(420.0f, 470.0f);
    const ImVec2 panelMin((viewportSize.x - panelSize.x) * 0.5f,
                          (viewportSize.y - panelSize.y) * 0.52f);
    const ImVec2 panelMax(panelMin.x + panelSize.x, panelMin.y + panelSize.y);
    DrawPanel(draw, panelMin, panelMax, goldBorder, 14.0f);
    draw->AddText(ImVec2(panelMin.x + 30.0f, panelMin.y + 25.0f),
                  TavernUiColor(1.0f, 0.82f, 0.56f, 1.0f), "水鏡エールを注ぐ");
    draw->AddText(ImVec2(panelMin.x + 30.0f, panelMin.y + 57.0f),
                  TavernUiColor(0.82f, 0.76f, 0.68f, 1.0f),
                  "左クリックを押し続け、適量で離す");

    const ImVec2 mugMin(panelMin.x + 135.0f, panelMin.y + 105.0f);
    const ImVec2 mugMax(panelMin.x + 285.0f, panelMin.y + 365.0f);
    draw->AddRectFilled(mugMin, mugMax,
                        TavernUiColor(0.11f, 0.09f, 0.07f, 0.92f), 16.0f);
    draw->AddRect(mugMin, mugMax, TavernUiColor(0.70f, 0.72f, 0.68f, 1.0f),
                  16.0f, 0, 5.0f);
    const float innerHeight = mugMax.y - mugMin.y - 16.0f;
    const float liquidTop =
        mugMax.y - 8.0f - innerHeight * std::clamp(m_aleFill, 0.0f, 1.0f);
    draw->AddRectFilled(ImVec2(mugMin.x + 8.0f, liquidTop),
                        ImVec2(mugMax.x - 8.0f, mugMax.y - 8.0f),
                        TavernUiColor(0.94f, 0.43f, 0.055f, 0.96f), 10.0f);
    if (m_primaryActionActive) {
      draw->AddRectFilled(ImVec2(panelMin.x + 207.0f, panelMin.y + 82.0f),
                          ImVec2(panelMin.x + 213.0f, panelMin.y + 112.0f),
                          TavernUiColor(0.98f, 0.52f, 0.08f, 0.95f), 3.0f);
    }
    if (m_aleFoam > 0.002f && m_aleFill > 0.01f) {
      const float foamHeight =
          std::min(innerHeight * 0.20f, innerHeight * m_aleFoam);
      draw->AddRectFilled(
          ImVec2(mugMin.x + 8.0f, liquidTop),
          ImVec2(mugMax.x - 8.0f,
                 std::min(mugMax.y - 8.0f, liquidTop + foamHeight)),
          TavernUiColor(1.0f, 0.94f, 0.76f, 0.98f), 8.0f);
    }
    const float idealLowY = mugMax.y - 8.0f - innerHeight * kPerfectPourMinimum;
    const float idealHighY =
        mugMax.y - 8.0f - innerHeight * kPerfectPourMaximum;
    draw->AddLine(ImVec2(mugMin.x - 14.0f, idealLowY),
                  ImVec2(mugMax.x + 14.0f, idealLowY),
                  TavernUiColor(0.30f, 0.92f, 0.52f, 0.95f), 2.0f);
    draw->AddLine(ImVec2(mugMin.x - 14.0f, idealHighY),
                  ImVec2(mugMax.x + 14.0f, idealHighY),
                  TavernUiColor(0.30f, 0.92f, 0.52f, 0.95f), 2.0f);
    std::snprintf(line, sizeof(line), "%d%%",
                  static_cast<int>(std::lround(m_aleFill * 100.0f)));
    draw->AddText(ImVec2(panelMin.x + 190.0f, panelMin.y + 392.0f),
                  TavernUiColor(1.0f, 0.84f, 0.62f, 1.0f), line);
    if (m_aleOverflow > 0.001f) {
      draw->AddText(ImVec2(panelMin.x + 160.0f, panelMin.y + 426.0f),
                    TavernUiColor(1.0f, 0.25f, 0.12f, 1.0f), "OVERFLOW!");
    }
  } else if (m_workState == WorkState::WashingDish) {
    const ImVec2 panelSize(470.0f, 170.0f);
    const ImVec2 panelMin((viewportSize.x - panelSize.x) * 0.5f,
                          viewportSize.y * 0.42f);
    const ImVec2 panelMax(panelMin.x + panelSize.x, panelMin.y + panelSize.y);
    DrawPanel(draw, panelMin, panelMax, goldBorder, 14.0f);
    draw->AddText(ImVec2(panelMin.x + 28.0f, panelMin.y + 24.0f),
                  TavernUiColor(1.0f, 0.84f, 0.62f, 1.0f),
                  m_heldItem == HeldItem::DirtyBowl ? "ボウルを洗う"
                                                     : "ジョッキを洗う");
    draw->AddText(ImVec2(panelMin.x + 28.0f, panelMin.y + 54.0f),
                  TavernUiColor(0.82f, 0.76f, 0.68f, 1.0f),
                  "左クリックを押し続ける");
    const ImVec2 washMin(panelMin.x + 28.0f, panelMin.y + 105.0f);
    const ImVec2 washMax(panelMax.x - 28.0f, panelMin.y + 132.0f);
    draw->AddRectFilled(washMin, washMax,
                        TavernUiColor(0.15f, 0.11f, 0.08f, 1.0f), 7.0f);
    draw->AddRectFilled(
        washMin,
        ImVec2(washMin.x + (washMax.x - washMin.x) * m_washProgress, washMax.y),
        TavernUiColor(0.24f, 0.72f, 0.86f, 1.0f), 7.0f);
  }

  if (!m_feedbackText.empty() && m_feedbackTimer > 0.0f) {
    const ImVec2 feedbackSize = ImGui::CalcTextSize(m_feedbackText.c_str());
    const float feedbackAlpha = std::clamp(m_feedbackTimer, 0.0f, 1.0f);
    const ImVec2 feedbackPosition((viewportSize.x - feedbackSize.x) * 0.5f,
                                  192.0f);
    draw->AddText(ImVec2(feedbackPosition.x + 2.0f, feedbackPosition.y + 2.0f),
                  TavernUiColor(0.0f, 0.0f, 0.0f, feedbackAlpha),
                  m_feedbackText.c_str());
    draw->AddText(feedbackPosition,
                  TavernUiColor(1.0f, 0.78f, 0.24f, feedbackAlpha),
                  m_feedbackText.c_str());
  }

  const float promptWidth = std::min(760.0f, viewportSize.x - 56.0f);
  const ImVec2 promptMin((viewportSize.x - promptWidth) * 0.5f,
                         viewportSize.y - 92.0f);
  const ImVec2 promptMax(promptMin.x + promptWidth, viewportSize.y - 28.0f);
  DrawPanel(draw, promptMin, promptMax, goldBorder);
  const ImVec2 promptTextSize = ImGui::CalcTextSize(m_nearbyPrompt.c_str());
  draw->AddText(
      ImVec2((viewportSize.x - promptTextSize.x) * 0.5f, promptMin.y + 22.0f),
      TavernUiColor(1.0f, 0.86f, 0.65f, 1.0f), m_nearbyPrompt.c_str());

  if (m_shiftState == ShiftState::Complete ||
      m_shiftState == ShiftState::Failed) {
    draw->AddRectFilled(ImVec2(0.0f, 0.0f), viewportSize,
                        TavernUiColor(0.0f, 0.0f, 0.0f, 0.45f));
    const ImVec2 resultSize(560.0f, 245.0f);
    const ImVec2 resultMin((viewportSize.x - resultSize.x) * 0.5f,
                           (viewportSize.y - resultSize.y) * 0.45f);
    const ImVec2 resultMax(resultMin.x + resultSize.x,
                           resultMin.y + resultSize.y);
    DrawPanel(draw, resultMin, resultMax, goldBorder, 14.0f);
    draw->AddText(ImVec2(resultMin.x + 32.0f, resultMin.y + 28.0f),
                  TavernUiColor(1.0f, 0.82f, 0.56f, 1.0f),
                  m_shiftState == ShiftState::Complete ? "営業終了"
                                                       : "営業失敗");
    std::snprintf(line, sizeof(line),
                  "売上 %d G　提供 %d（料理 %d）　PERFECT %d　退店 %d",
                  m_gold, m_servedCustomers, m_foodServed, m_perfectPours,
                  m_walkouts);
    draw->AddText(ImVec2(resultMin.x + 32.0f, resultMin.y + 88.0f),
                  TavernUiColor(0.90f, 0.84f, 0.76f, 1.0f), line);
    draw->AddText(ImVec2(resultMin.x + 32.0f, resultMin.y + 150.0f),
                  TavernUiColor(0.82f, 0.76f, 0.68f, 1.0f),
                  m_shiftState == ShiftState::Complete
                      ? "R：眠って翌朝 5:00 へ　入口で E：外へ戻る"
                      : "R：翌朝 5:00 から再挑戦　入口で E：外へ戻る");
  }

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  DrawManagementUi(viewportWidth, viewportHeight);
  return Action::None;
}

void TavernScene::DrawDebugPanel(float &timeOfDayHours, bool &automaticTime) {
  ImGui::SetNextWindowPos(ImVec2(20.0f, 190.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(330.0f, 0.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("酒場 Debug")) {
    ImGui::End();
    return;
  }

  ImGui::TextDisabled("F4：表示／非表示");
  ImGui::Text("通常速度：1 秒 = 1 分");
  ImGui::Checkbox("時間を進める", &automaticTime);

  const float normalizedHour = std::fmod(std::max(0.0f, timeOfDayHours), 24.0f);
  const int timeMinutes =
      static_cast<int>(std::lround(normalizedHour * 60.0f)) % (24 * 60);
  int debugHour = timeMinutes / 60;
  int debugMinute = timeMinutes % 60;
  bool timeChanged = ImGui::SliderInt("Hour（時）", &debugHour, 0, 23);
  if (ImGui::SliderInt("Minute（分）", &debugMinute, 0, 59))
    timeChanged = true;
  if (timeChanged)
    timeOfDayHours =
        static_cast<float>(debugHour) + static_cast<float>(debugMinute) / 60.0f;
  ImGui::Text("現在時刻：%02d:%02d", debugHour, debugMinute);

  const auto setTime = [&](int hour, int minute) {
    timeOfDayHours =
        static_cast<float>(hour) + static_cast<float>(minute) / 60.0f;
  };
  if (ImGui::Button("05:00"))
    setTime(5, 0);
  ImGui::SameLine();
  if (ImGui::Button("12:00"))
    setTime(12, 0);
  ImGui::SameLine();
  if (ImGui::Button("18:00"))
    setTime(18, 0);
  ImGui::SameLine();
  if (ImGui::Button("23:50"))
    setTime(23, 50);
  ImGui::SameLine();
  if (ImGui::Button("00:30"))
    setTime(0, 30);

  ImGui::Separator();
  int debugGold = m_gold;
  if (ImGui::InputInt("Gold (G)", &debugGold, 1, 10))
    m_gold = std::clamp(debugGold, 0, 999999);
  if (ImGui::Button("Gold 0"))
    m_gold = 0;
  ImGui::SameLine();
  if (ImGui::Button("Gold +10"))
    m_gold = std::min(999999, m_gold + 10);
  ImGui::SameLine();
  if (ImGui::Button("Gold +100"))
    m_gold = std::min(999999, m_gold + 100);

  ImGui::Separator();
  ImGui::Text("エール在庫：%d / %d", AleStock(), AleCapacity());
  int debugAleStock = AleStock();
  if (ImGui::InputInt("エール在庫##TavernAleStock", &debugAleStock, 1, 1))
    AleSupply().current = std::clamp(debugAleStock, 0, AleCapacity());
  if (ImGui::Button("在庫 0"))
    AleSupply().current = 0;
  ImGui::SameLine();
  if (ImGui::Button("在庫 +1"))
    AleSupply().current = std::min(AleCapacity(), AleStock() + 1);
  ImGui::SameLine();
  if (ImGui::Button("満タン"))
    AleSupply().current = AleCapacity();

  ImGui::Text("Upgrade：Mug %d / 3  Ale Lv.%d / %d  Table 3 %s", TotalMugs(),
              AleCapacityLevel(), kMaximumAleCapacityLevel,
              m_table3Unlocked ? "OPEN" : "CLOSED");

  const int placedMugs = static_cast<int>(std::count_if(
      m_counterMugs.begin(), m_counterMugs.end(),
      [](const MugState &mug) { return mug.item != HeldItem::None; }));
  ImGui::Separator();
  ImGui::Text("客流：%s", CustomerTrafficName());
  ImGui::Text("提供：%d  退店：%d  PERFECT：%d", m_servedCustomers, m_walkouts,
              m_perfectPours);
  ImGui::Text("料理提供：%d  ボウル：%d / %d  厨房：%s %.0f%%", m_foodServed,
              m_cleanBowls, TotalBowls(),
              m_kitchenState == KitchenState::Cooking
                  ? "COOKING"
                  : (m_kitchenState == KitchenState::Ready ? "READY" : "IDLE"),
              KitchenProgress() * 100.0f);
  ImGui::Text("手持：%s", GetHeldItemName(m_heldItem));
  ImGui::Text("棚のジョッキ：%d / %d  一時置き：%d / %zu", m_cleanMugs,
              TotalMugs(), placedMugs, m_counterMugs.size());

  ImGui::End();
}
