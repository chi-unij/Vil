#pragma once

#include "Dx12Renderer.h"
#include "FeatureRegistry.h"
#include "FeatureWorld.h"

#include <Windows.h>
#include <CommCtrl.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>

class FeatureLabApp {
public:
  int Run(HINSTANCE instance, int showCommand,
          const std::filesystem::path &explicitFeatureFile,
          bool automatedSelfTest = false);

private:
  bool Initialize(HINSTANCE instance, int showCommand,
                  const std::filesystem::path &explicitFeatureFile,
                  std::wstring &error);
  bool CreateApplicationWindow(HINSTANCE instance, int showCommand,
                               std::wstring &error);
  bool InitializeRenderer(std::wstring &error);
  void BuildFeatureTree();
  void SyncFeatureTree();
  void UpdateSelectionDescription(HTREEITEM item);
  void UpdateStatus(float deltaSeconds);
  void LayoutControls(int width, int height);
  void ToggleTreeItem(HTREEITEM item);
  void Tick(float deltaSeconds);
  void AdvanceSelfTest();
  void ShowFatalError(const std::wstring &message);

  LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam,
                        LPARAM lParam);
  LRESULT HandleViewportMessage(HWND window, UINT message, WPARAM wParam,
                                LPARAM lParam);
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                                     LPARAM lParam);
  static LRESULT CALLBACK ViewportProc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam);

  HINSTANCE m_instance = nullptr;
  HWND m_window = nullptr;
  HWND m_viewport = nullptr;
  HWND m_tree = nullptr;
  HWND m_status = nullptr;
  HWND m_description = nullptr;
  HWND m_enableAll = nullptr;
  HWND m_disableAll = nullptr;
  HWND m_resetDefaults = nullptr;
  HWND m_nextAttack = nullptr;
  HWND m_resetCamera = nullptr;
  HWND m_reload = nullptr;
  HFONT m_uiFont = nullptr;

  FeatureRegistry m_features;
  FeatureWorld m_world;
  Dx12Renderer m_renderer;
  std::filesystem::path m_featurePath;
  std::filesystem::path m_shaderPath;
  std::unordered_map<HTREEITEM, size_t> m_treeItems;
  std::unordered_map<HTREEITEM, std::wstring> m_categoryItems;
  bool m_syncingTree = false;
  bool m_running = true;
  bool m_rendererReady = false;
  bool m_draggingCamera = false;
  POINT m_lastMouse{};
  float m_statusClock = 0.0f;
  float m_smoothedFps = 60.0f;
  bool m_selfTest = false;
  uint32_t m_selfTestFrame = 0;
  uint64_t m_defaultDrawCalls = 0;
  int m_exitCode = 0;
};
