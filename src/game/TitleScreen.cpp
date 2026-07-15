#include "game/TitleScreen.h"

#include <imgui.h>

TitleScreen::Action TitleScreen::Draw(int viewportWidth, int viewportHeight) {
  Action action = Action::None;

  const ImVec2 viewportSize(static_cast<float>(viewportWidth),
                            static_cast<float>(viewportHeight));

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(viewportSize, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

  ImGui::Begin("##TitleScreen", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);

  ImDrawList *drawList = ImGui::GetWindowDrawList();
  const ImVec2 min = ImGui::GetWindowPos();
  const ImVec2 max(min.x + viewportSize.x, min.y + viewportSize.y);

  // 既存の Sky pass を背景として使い、タイトル用の色調だけを重ねる。
  drawList->AddRectFilledMultiColor(
      min, max, IM_COL32(18, 28, 36, 210), IM_COL32(32, 48, 56, 180),
      IM_COL32(8, 14, 18, 235), IM_COL32(10, 18, 24, 240));
  drawList->AddRectFilled(
      ImVec2(min.x, min.y + viewportSize.y * 0.62f), max,
      IM_COL32(0, 0, 0, 105));

  const float centerX = viewportSize.x * 0.5f;
  const float titleY = viewportSize.y * 0.23f;
  const char *title = "VILLIEN";
  const char *subtitle = "DirectX 12 Rendering / Boss Action";

  ImGui::SetWindowFontScale(2.5f);
  ImVec2 titleSize = ImGui::CalcTextSize(title);
  ImGui::SetCursorPos(ImVec2(centerX - titleSize.x * 0.5f, titleY));
  ImGui::TextColored(ImVec4(0.96f, 0.91f, 0.80f, 1.0f), "%s", title);

  ImGui::SetWindowFontScale(1.0f);
  ImVec2 subtitleSize = ImGui::CalcTextSize(subtitle);
  ImGui::SetCursorPos(
      ImVec2(centerX - subtitleSize.x * 0.5f, titleY + 72.0f));
  ImGui::TextColored(ImVec4(0.78f, 0.86f, 0.80f, 1.0f), "%s", subtitle);

  const float buttonW = 260.0f;
  const float buttonH = 44.0f;
  const float buttonX = centerX - buttonW * 0.5f;
  float buttonY = viewportSize.y * 0.52f;

  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 10.0f));
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.23f, 0.22f, 0.90f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        ImVec4(0.32f, 0.42f, 0.36f, 0.95f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                        ImVec4(0.52f, 0.61f, 0.48f, 1.0f));

  ImGui::SetCursorPos(ImVec2(buttonX, buttonY));
  if (ImGui::Button("Start", ImVec2(buttonW, buttonH)))
    action = Action::Start;

  buttonY += 56.0f;
  ImGui::SetCursorPos(ImVec2(buttonX, buttonY));
  if (ImGui::Button("Settings", ImVec2(buttonW, buttonH)))
    action = Action::Settings;

  buttonY += 56.0f;
  ImGui::SetCursorPos(ImVec2(buttonX, buttonY));
  if (ImGui::Button("Quit", ImVec2(buttonW, buttonH)))
    action = Action::Quit;

  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(2);

#if defined(_DEBUG)
  const ImVec2 editorSize(88.0f, 32.0f);
  ImGui::SetCursorPos(ImVec2(viewportSize.x - editorSize.x - 20.0f,
                             viewportSize.y - editorSize.y - 20.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.12f, 0.13f, 0.62f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        ImVec4(0.18f, 0.24f, 0.24f, 0.85f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                        ImVec4(0.28f, 0.35f, 0.32f, 0.95f));
  if (ImGui::Button("Editor", editorSize))
    action = Action::Editor;
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar();
#endif

  ImGui::End();

  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  return action;
}
