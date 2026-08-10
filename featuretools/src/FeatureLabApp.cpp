#include "FeatureLabApp.h"

#include <CommCtrl.h>
#include <Windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace {

constexpr wchar_t kMainWindowClass[] = L"VillienFeatureLabMainWindow";
constexpr wchar_t kViewportClass[] = L"VillienFeatureLabViewport";
constexpr int kSidebarWidth = 420;
constexpr UINT kApplyTreeToggle = WM_APP + 17;

enum ControlId : int {
  IdTree = 100,
  IdEnableAll,
  IdDisableAll,
  IdResetDefaults,
  IdNextAttack,
  IdResetCamera,
  IdReload,
};

std::filesystem::path ExecutablePath() {
  std::wstring buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  buffer.resize(length);
  return std::filesystem::path(buffer);
}

std::filesystem::path FindShaderPath(const std::filesystem::path &executable) {
  const std::array candidates{
      executable.parent_path() / L"shaders" / L"FeatureLab.hlsl",
      std::filesystem::current_path() / L"shaders" / L"FeatureLab.hlsl",
      std::filesystem::current_path() / L"featuretools" / L"shaders" /
          L"FeatureLab.hlsl",
  };
  for (const auto &candidate : candidates) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec))
      return candidate;
  }
  return candidates.front();
}

const wchar_t *AttackName(LabAttackType attack) {
  switch (attack) {
  case LabAttackType::Meteor:
    return L"メテオ";
  case LabAttackType::Laser:
    return L"レーザー";
  case LabAttackType::Sanctuary:
    return L"サンクチュアリ";
  }
  return L"不明";
}

void SetControlFont(HWND control, HFONT font) {
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

std::filesystem::path ErrorLogPath() {
  return ExecutablePath().parent_path() / L"featuretools-error.log";
}

void WriteErrorLog(const std::wstring &message) {
  std::ofstream output(ErrorLogPath(), std::ios::binary | std::ios::trunc);
  output << LabWideToUtf8(message);
}

} // namespace

int FeatureLabApp::Run(HINSTANCE instance, int showCommand,
                       const std::filesystem::path &explicitFeatureFile,
                       bool automatedSelfTest) {
  m_selfTest = automatedSelfTest;
  std::error_code removeError;
  std::filesystem::remove(ErrorLogPath(), removeError);
  std::wstring error;
  if (!Initialize(instance, showCommand, explicitFeatureFile, error)) {
    WriteErrorLog(error);
    MessageBoxW(nullptr, error.c_str(), L"VILLIEN Feature Lab - 起動エラー",
                MB_OK | MB_ICONERROR);
    return 1;
  }

  auto previous = std::chrono::steady_clock::now();
  MSG message{};
  while (m_running) {
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) {
        m_running = false;
        break;
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (!m_running)
      break;
    if (IsIconic(m_window)) {
      Sleep(25);
      previous = std::chrono::steady_clock::now();
      continue;
    }
    const auto now = std::chrono::steady_clock::now();
    const float delta = std::clamp(
        std::chrono::duration<float>(now - previous).count(), 0.0f, 0.1f);
    previous = now;
    Tick(delta);
  }
  m_renderer.Shutdown();
  if (m_uiFont)
    DeleteObject(m_uiFont);
  return m_exitCode;
}

bool FeatureLabApp::Initialize(
    HINSTANCE instance, int showCommand,
    const std::filesystem::path &explicitFeatureFile, std::wstring &error) {
  m_instance = instance;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TREEVIEW_CLASSES |
                                                     ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);

  const auto executable = ExecutablePath();
  m_featurePath =
      FeatureRegistry::FindFeatureFile(executable, explicitFeatureFile);
  const auto load = m_features.Load(m_featurePath);
  if (!load.ok) {
    error = L"feature.md の 3D Feature Lab registry を読み込めません。\n" +
            load.error + L"\n検索先: " + m_featurePath.wstring();
    return false;
  }
  m_shaderPath = FindShaderPath(executable);
  if (!std::filesystem::exists(m_shaderPath)) {
    error = L"DX12 shader が見つかりません: " + m_shaderPath.wstring();
    return false;
  }
  m_world.Initialize();
  if (!CreateApplicationWindow(instance, showCommand, error))
    return false;
  BuildFeatureTree();
  return InitializeRenderer(error);
}

bool FeatureLabApp::CreateApplicationWindow(HINSTANCE instance, int showCommand,
                                            std::wstring &error) {
  WNDCLASSEXW mainClass{sizeof(mainClass)};
  mainClass.style = CS_HREDRAW | CS_VREDRAW;
  mainClass.lpfnWndProc = WindowProc;
  mainClass.hInstance = instance;
  mainClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  mainClass.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  mainClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  mainClass.lpszClassName = kMainWindowClass;
  if (!RegisterClassExW(&mainClass)) {
    error = L"メインウィンドウクラスを登録できません。";
    return false;
  }
  WNDCLASSEXW viewportClass{sizeof(viewportClass)};
  viewportClass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
  viewportClass.lpfnWndProc = ViewportProc;
  viewportClass.hInstance = instance;
  viewportClass.hCursor = LoadCursor(nullptr, IDC_CROSS);
  viewportClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  viewportClass.lpszClassName = kViewportClass;
  if (!RegisterClassExW(&viewportClass)) {
    error = L"DX12 viewport クラスを登録できません。";
    return false;
  }

  RECT desired{0, 0, 1500, 900};
  AdjustWindowRectEx(&desired, WS_OVERLAPPEDWINDOW, FALSE, 0);
  m_window = CreateWindowExW(
      0, kMainWindowClass, L"VILLIEN DirectX 12 Feature Lab",
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
      desired.right - desired.left, desired.bottom - desired.top, nullptr,
      nullptr, instance, this);
  if (!m_window) {
    error = L"メインウィンドウを作成できません。";
    return false;
  }
  m_uiFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Yu Gothic UI");
  CreateWindowExW(0, L"STATIC", L"DIRECTX 12  FEATURE LAB",
                  WS_CHILD | WS_VISIBLE, 14, 12, 390, 24, m_window, nullptr,
                  instance, nullptr);
  m_status = CreateWindowExW(0, L"STATIC", L"初期化中…",
                             WS_CHILD | WS_VISIBLE, 14, 38, 390, 44,
                             m_window, nullptr, instance, nullptr);
  m_enableAll = CreateWindowExW(0, L"BUTTON", L"すべて ON",
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0,
                                0, m_window,
                                reinterpret_cast<HMENU>(IdEnableAll), instance,
                                nullptr);
  m_disableAll = CreateWindowExW(0, L"BUTTON", L"すべて OFF",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0,
                                 0, 0, m_window,
                                 reinterpret_cast<HMENU>(IdDisableAll), instance,
                                 nullptr);
  m_resetDefaults = CreateWindowExW(
      0, L"BUTTON", L"既定値", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0,
      0, 0, m_window, reinterpret_cast<HMENU>(IdResetDefaults), instance,
      nullptr);
  m_nextAttack = CreateWindowExW(0, L"BUTTON", L"次の攻撃",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0,
                                 0, 0, m_window,
                                 reinterpret_cast<HMENU>(IdNextAttack), instance,
                                 nullptr);
  m_resetCamera = CreateWindowExW(
      0, L"BUTTON", L"カメラ初期化", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
      0, 0, 0, 0, m_window, reinterpret_cast<HMENU>(IdResetCamera), instance,
      nullptr);
  m_reload = CreateWindowExW(0, L"BUTTON", L"feature.md 再読込",
                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
                             m_window, reinterpret_cast<HMENU>(IdReload),
                             instance, nullptr);
  m_tree = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
          TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_CHECKBOXES,
      0, 0, 0, 0, m_window, reinterpret_cast<HMENU>(IdTree), instance, nullptr);
  m_description = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"STATIC",
      L"Feature を選択すると、実装 contract を表示します。",
      WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, m_window, nullptr,
      instance, nullptr);
  m_viewport = CreateWindowExW(0, kViewportClass, L"",
                               WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
                               0, 0, m_window, nullptr, instance, this);
  for (HWND control : {m_status, m_enableAll, m_disableAll, m_resetDefaults,
                       m_nextAttack, m_resetCamera, m_reload, m_tree,
                       m_description})
    SetControlFont(control, m_uiFont);
  RECT client{};
  GetClientRect(m_window, &client);
  LayoutControls(client.right, client.bottom);
  ShowWindow(m_window, showCommand);
  UpdateWindow(m_window);
  return true;
}

bool FeatureLabApp::InitializeRenderer(std::wstring &error) {
  RECT viewport{};
  GetClientRect(m_viewport, &viewport);
  if (!m_renderer.Initialize(m_viewport, std::max(1L, viewport.right),
                             std::max(1L, viewport.bottom), m_shaderPath,
                             error))
    return false;
  m_rendererReady = true;
  return true;
}

void FeatureLabApp::BuildFeatureTree() {
  m_syncingTree = true;
  TreeView_DeleteAllItems(m_tree);
  m_treeItems.clear();
  m_categoryItems.clear();
  std::map<std::wstring, HTREEITEM> categories;
  const auto &entries = m_features.Entries();
  for (size_t index = 0; index < entries.size(); ++index) {
    const auto &entry = entries[index];
    HTREEITEM parent = nullptr;
    auto found = categories.find(entry.category);
    if (found == categories.end()) {
      TVINSERTSTRUCTW category{};
      category.hParent = TVI_ROOT;
      category.hInsertAfter = TVI_LAST;
      category.item.mask = TVIF_TEXT | TVIF_PARAM;
      category.item.pszText = const_cast<wchar_t *>(entry.category.c_str());
      category.item.lParam = 0;
      parent = TreeView_InsertItem(m_tree, &category);
      categories.emplace(entry.category, parent);
      m_categoryItems.emplace(parent, entry.category);
    } else {
      parent = found->second;
    }
    TVINSERTSTRUCTW feature{};
    feature.hParent = parent;
    feature.hInsertAfter = TVI_LAST;
    feature.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
    std::wstring label = entry.name + L"  [" +
                         std::wstring(entry.id.begin(), entry.id.end()) + L"]";
    feature.item.pszText = label.data();
    feature.item.lParam = static_cast<LPARAM>(index + 1);
    feature.item.stateMask = TVIS_STATEIMAGEMASK;
    feature.item.state = INDEXTOSTATEIMAGEMASK(entry.enabled ? 2 : 1);
    HTREEITEM item = TreeView_InsertItem(m_tree, &feature);
    m_treeItems.emplace(item, index);
    TreeView_Expand(m_tree, parent, TVE_EXPAND);
  }
  m_syncingTree = false;
  SyncFeatureTree();
  const HTREEITEM root = TreeView_GetRoot(m_tree);
  const HTREEITEM firstFeature = root ? TreeView_GetChild(m_tree, root) : nullptr;
  if (firstFeature) {
    TreeView_SelectItem(m_tree, firstFeature);
    TreeView_EnsureVisible(m_tree, root);
    UpdateSelectionDescription(firstFeature);
  }
}

void FeatureLabApp::SyncFeatureTree() {
  m_syncingTree = true;
  for (const auto &[item, index] : m_treeItems) {
    TVITEMW treeItem{};
    treeItem.mask = TVIF_HANDLE | TVIF_STATE;
    treeItem.hItem = item;
    treeItem.stateMask = TVIS_STATEIMAGEMASK;
    treeItem.state = INDEXTOSTATEIMAGEMASK(
        m_features.Entries()[index].enabled ? 2 : 1);
    TreeView_SetItem(m_tree, &treeItem);
  }
  for (const auto &[item, category] : m_categoryItems) {
    bool allEnabled = true;
    for (const auto &entry : m_features.Entries()) {
      if (entry.category == category)
        allEnabled = allEnabled && entry.enabled;
    }
    TVITEMW treeItem{};
    treeItem.mask = TVIF_HANDLE | TVIF_STATE;
    treeItem.hItem = item;
    treeItem.stateMask = TVIS_STATEIMAGEMASK;
    treeItem.state = INDEXTOSTATEIMAGEMASK(allEnabled ? 2 : 1);
    TreeView_SetItem(m_tree, &treeItem);
  }
  m_syncingTree = false;
}

void FeatureLabApp::UpdateSelectionDescription(HTREEITEM item) {
  const auto found = m_treeItems.find(item);
  if (found == m_treeItems.end())
    return;
  const auto &entry = m_features.Entries()[found->second];
  const std::wstring text = entry.name + L"\r\n" + entry.description;
  SetWindowTextW(m_description, text.c_str());
}

void FeatureLabApp::UpdateStatus(float deltaSeconds) {
  m_statusClock += deltaSeconds;
  const float currentFps = deltaSeconds > 0.0001f ? 1.0f / deltaSeconds : 0.0f;
  m_smoothedFps = m_smoothedFps * 0.92f + currentFps * 0.08f;
  if (m_statusClock < 0.2f)
    return;
  m_statusClock = 0.0f;
  const auto &frame = m_world.Frame();
  std::wostringstream text;
  text.precision(1);
  text << std::fixed << L"有効: " << m_features.EnabledCount() << L" / "
       << m_features.Entries().size() << L"   FPS: " << m_smoothedFps
       << L"   Draw: " << m_renderer.DrawCalls() << L"\r\n"
       << (m_features.Enabled("deferred") ? L"Deferred" : L"Forward")
       << L" | 攻撃: " << AttackName(frame.attack)
       << L" | 衝突: " << m_world.CollisionCount();
  SetWindowTextW(m_status, text.str().c_str());
}

void FeatureLabApp::LayoutControls(int width, int height) {
  const int sidebar = std::min(kSidebarWidth, std::max(300, width / 2));
  const int margin = 12;
  const int gap = 6;
  const int buttonWidth = (sidebar - margin * 2 - gap * 2) / 3;
  MoveWindow(m_status, margin, 38, sidebar - margin * 2, 43, TRUE);
  MoveWindow(m_enableAll, margin, 84, buttonWidth, 28, TRUE);
  MoveWindow(m_disableAll, margin + buttonWidth + gap, 84, buttonWidth, 28,
             TRUE);
  MoveWindow(m_resetDefaults, margin + (buttonWidth + gap) * 2, 84,
             buttonWidth, 28, TRUE);
  MoveWindow(m_nextAttack, margin, 116, buttonWidth, 28, TRUE);
  MoveWindow(m_resetCamera, margin + buttonWidth + gap, 116, buttonWidth, 28,
             TRUE);
  MoveWindow(m_reload, margin + (buttonWidth + gap) * 2, 116, buttonWidth, 28,
             TRUE);
  const int descriptionHeight = 92;
  MoveWindow(m_tree, margin, 150, sidebar - margin * 2,
             std::max(80, height - 150 - descriptionHeight - margin * 2), TRUE);
  MoveWindow(m_description, margin, height - descriptionHeight - margin,
             sidebar - margin * 2, descriptionHeight, TRUE);
  MoveWindow(m_viewport, sidebar, 0, std::max(1, width - sidebar),
             std::max(1, height), TRUE);
}

void FeatureLabApp::ToggleTreeItem(HTREEITEM item) {
  const auto found = m_treeItems.find(item);
  if (found != m_treeItems.end()) {
    auto &entry = m_features.Entries()[found->second];
    const int state = TreeView_GetCheckState(m_tree, item);
    entry.enabled = state != 0;
    UpdateSelectionDescription(item);
    return;
  }
  const auto category = m_categoryItems.find(item);
  if (category == m_categoryItems.end())
    return;
  const bool enabled = TreeView_GetCheckState(m_tree, item) != 0;
  for (auto &entry : m_features.Entries()) {
    if (entry.category == category->second)
      entry.enabled = enabled;
  }
  SyncFeatureTree();
}

void FeatureLabApp::Tick(float deltaSeconds) {
  m_world.Update(deltaSeconds, m_features);
  std::wstring error;
  if (m_rendererReady &&
      !m_renderer.Render(m_world, m_features, deltaSeconds, error)) {
    m_rendererReady = false;
    ShowFatalError(error);
    return;
  }
  UpdateStatus(deltaSeconds);
  if (m_selfTest)
    AdvanceSelfTest();
}

void FeatureLabApp::AdvanceSelfTest() {
  ++m_selfTestFrame;
  if (m_selfTestFrame == 30) {
    m_defaultDrawCalls = m_renderer.DrawCalls();
    if (m_defaultDrawCalls < 20) {
      WriteErrorLog(L"Self-test failed: default 3D world emitted too few draw calls.");
      m_exitCode = 2;
      m_running = false;
      PostQuitMessage(m_exitCode);
    }
    if (std::abs(m_renderer.CameraYaw()) > 0.0001f) {
      WriteErrorLog(L"Self-test failed: camera moved while Automatic Camera Movement defaulted to Off.");
      m_exitCode = 6;
      m_running = false;
      PostQuitMessage(m_exitCode);
    }
  } else if (m_selfTestFrame == 45) {
    m_features.SetEnabled("bloom", false);
    m_features.SetEnabled("water", false);
    m_features.SetEnabled("particles", false);
    m_features.SetEnabled("rain", false);
    m_features.SetEnabled("wireframe", true);
    m_features.SetEnabled("camera_auto_move", true);
    SyncFeatureTree();
    for (const auto &[item, index] : m_treeItems) {
      if (m_features.Entries()[index].id == "deferred") {
        TreeView_SelectItem(m_tree, item);
        SendMessageW(m_tree, WM_KEYDOWN, VK_SPACE, 0);
        SendMessageW(m_tree, WM_KEYUP, VK_SPACE, 0);
        break;
      }
    }
  } else if (m_selfTestFrame == 50) {
    if (m_features.Enabled("deferred")) {
      WriteErrorLog(L"Self-test failed: TreeView keyboard toggle did not disable Deferred rendering.");
      m_exitCode = 5;
      m_running = false;
      PostQuitMessage(m_exitCode);
    }
  } else if (m_selfTestFrame == 90) {
    if (m_renderer.CameraYaw() <= 0.0001f) {
      WriteErrorLog(L"Self-test failed: enabling Automatic Camera Movement did not orbit the camera.");
      m_exitCode = 7;
      m_running = false;
      PostQuitMessage(m_exitCode);
    }
    if (m_renderer.DrawCalls() >= m_defaultDrawCalls) {
      WriteErrorLog(L"Self-test failed: disabling render passes/entities did not reduce draw calls.");
      m_exitCode = 3;
      m_running = false;
      PostQuitMessage(m_exitCode);
    }
  } else if (m_selfTestFrame == 105) {
    SendMessageW(m_disableAll, BM_CLICK, 0, 0);
  } else if (m_selfTestFrame == 135) {
    if (m_features.EnabledCount() != 0 || m_renderer.DrawCalls() > 3) {
      WriteErrorLog(L"Self-test failed: all-off mode still rendered feature entities.");
      m_exitCode = 4;
      m_running = false;
      PostQuitMessage(m_exitCode);
    }
    SendMessageW(m_resetDefaults, BM_CLICK, 0, 0);
  } else if (m_selfTestFrame == 165) {
    m_world.ForceNextAttack(m_features);
  } else if (m_selfTestFrame == 210) {
    std::wofstream report(ExecutablePath().parent_path() /
                          L"featuretools-self-test.log");
    report << L"PASS\n"
           << L"Default draw calls: " << m_defaultDrawCalls << L"\n"
           << L"Restored enabled features: " << m_features.EnabledCount()
           << L"\n"
           << L"Collision events: " << m_world.CollisionCount() << L"\n";
    m_running = false;
    PostQuitMessage(0);
  }
}

void FeatureLabApp::ShowFatalError(const std::wstring &message) {
  WriteErrorLog(message);
  m_exitCode = 1;
  MessageBoxW(m_window, message.c_str(), L"DirectX 12 runtime エラー",
              MB_OK | MB_ICONERROR);
  m_running = false;
  PostQuitMessage(1);
}

LRESULT FeatureLabApp::HandleMessage(HWND window, UINT message, WPARAM wParam,
                                     LPARAM lParam) {
  switch (message) {
  case WM_SIZE: {
    const int width = LOWORD(lParam);
    const int height = HIWORD(lParam);
    if (width > 0 && height > 0) {
      LayoutControls(width, height);
      if (m_rendererReady) {
        RECT area{};
        GetClientRect(m_viewport, &area);
        std::wstring error;
        if (!m_renderer.Resize(std::max(1L, area.right),
                               std::max(1L, area.bottom), error))
          ShowFatalError(error);
      }
    }
    return 0;
  }
  case WM_COMMAND:
    switch (LOWORD(wParam)) {
    case IdEnableAll:
      m_features.SetAll(true);
      SyncFeatureTree();
      return 0;
    case IdDisableAll:
      m_features.SetAll(false);
      SyncFeatureTree();
      return 0;
    case IdResetDefaults:
      m_features.ResetDefaults();
      m_world.ResetSimulation();
      SyncFeatureTree();
      return 0;
    case IdNextAttack:
      m_world.ForceNextAttack(m_features);
      return 0;
    case IdResetCamera:
      m_renderer.ResetCamera();
      return 0;
    case IdReload: {
      const auto result = m_features.Load(m_featurePath);
      if (!result.ok) {
        MessageBoxW(window, result.error.c_str(), L"feature.md 再読込エラー",
                    MB_OK | MB_ICONERROR);
      } else {
        BuildFeatureTree();
      }
      return 0;
    }
    }
    break;
  case WM_NOTIFY: {
    const auto *header = reinterpret_cast<NMHDR *>(lParam);
    if (header->hwndFrom != m_tree)
      break;
    if (header->code == TVN_SELCHANGEDW) {
      const auto *notification = reinterpret_cast<NMTREEVIEWW *>(lParam);
      UpdateSelectionDescription(notification->itemNew.hItem);
      return 0;
    }
    if (header->code == TVN_ITEMCHANGEDW && !m_syncingTree) {
      const auto *change = reinterpret_cast<NMTVITEMCHANGE *>(lParam);
      if ((change->uChanged & TVIF_STATE) != 0 &&
          (change->uStateOld & TVIS_STATEIMAGEMASK) !=
              (change->uStateNew & TVIS_STATEIMAGEMASK))
        ToggleTreeItem(change->hItem);
      return 0;
    }
    if (header->code == NM_CLICK && !m_syncingTree) {
      const DWORD packedPosition = GetMessagePos();
      POINT point{GET_X_LPARAM(packedPosition), GET_Y_LPARAM(packedPosition)};
      ScreenToClient(m_tree, &point);
      TVHITTESTINFO hit{};
      hit.pt = point;
      TreeView_HitTest(m_tree, &hit);
      if (hit.hItem && (hit.flags & TVHT_ONITEMSTATEICON) != 0)
        PostMessageW(window, kApplyTreeToggle, 0,
                     reinterpret_cast<LPARAM>(hit.hItem));
      return 0;
    }
    if (header->code == TVN_KEYDOWN) {
      const auto *key = reinterpret_cast<NMTVKEYDOWN *>(lParam);
      if (key->wVKey == VK_SPACE) {
        const HTREEITEM selected = TreeView_GetSelection(m_tree);
        if (selected)
          PostMessageW(window, kApplyTreeToggle, 0,
                       reinterpret_cast<LPARAM>(selected));
      }
      return 0;
    }
    break;
  }
  case kApplyTreeToggle:
    ToggleTreeItem(reinterpret_cast<HTREEITEM>(lParam));
    return 0;
  case WM_CLOSE:
    DestroyWindow(window);
    return 0;
  case WM_DESTROY:
    m_running = false;
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT FeatureLabApp::HandleViewportMessage(HWND window, UINT message,
                                             WPARAM wParam, LPARAM lParam) {
  switch (message) {
  case WM_LBUTTONDOWN:
    if (m_features.Enabled("camera_orbit")) {
      m_draggingCamera = true;
      m_lastMouse = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      SetCapture(window);
    }
    return 0;
  case WM_LBUTTONUP:
    m_draggingCamera = false;
    if (GetCapture() == window)
      ReleaseCapture();
    return 0;
  case WM_MOUSEMOVE:
    if (m_draggingCamera) {
      POINT current{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      m_renderer.Orbit((current.x - m_lastMouse.x) * 0.0075f,
                       (current.y - m_lastMouse.y) * 0.0075f);
      m_lastMouse = current;
    }
    return 0;
  case WM_MOUSEWHEEL:
    if (m_features.Enabled("camera_orbit"))
      m_renderer.Zoom(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) /
                      WHEEL_DELTA * 2.2f);
    return 0;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK FeatureLabApp::WindowProc(HWND window, UINT message,
                                           WPARAM wParam, LPARAM lParam) {
  FeatureLabApp *app = reinterpret_cast<FeatureLabApp *>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
    app = static_cast<FeatureLabApp *>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  }
  return app ? app->HandleMessage(window, message, wParam, lParam)
             : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK FeatureLabApp::ViewportProc(HWND window, UINT message,
                                             WPARAM wParam, LPARAM lParam) {
  FeatureLabApp *app = reinterpret_cast<FeatureLabApp *>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
    app = static_cast<FeatureLabApp *>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  }
  return app ? app->HandleViewportMessage(window, message, wParam, lParam)
             : DefWindowProcW(window, message, wParam, lParam);
}
