// ======================================
// File: Win32Window.cpp
// Purpose: Win32 window + message handling (resize/events + raw input + ImGui message routing)
// ======================================

#include "Win32Window.h"
#include "Input.h"

#include <backends/imgui_impl_win32.h>

// NOTE: imgui_impl_win32.h intentionally does NOT declare ImGui_ImplWin32_WndProcHandler
// (it is inside a #if 0 block to avoid forcing <windows.h> includes).
// We forward-declare it here so we can call it from our WndProc.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include <stdexcept>
#include <vector>

Win32Window::~Win32Window()
{
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }

    delete m_input;
    m_input = nullptr;
}

void Win32Window::Create(const wchar_t* title, uint32_t width, uint32_t height)
{
    m_hInstance = GetModuleHandleW(nullptr);
    m_width = width;
    m_height = height;

    const wchar_t* className = L"DX12Tutorial12WindowClass";

    if (!m_input)
        m_input = new Input();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Win32Window::WndProc;
    wc.hInstance = m_hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = className;
    RegisterClassExW(&wc);

    RECT r{ 0, 0, (LONG)m_width, (LONG)m_height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(
        0,
        className,
        title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr,
        nullptr,
        m_hInstance,
        this
    );

    if (!m_hwnd)
        throw std::runtime_error("CreateWindowExW failed");

    // Register raw mouse input (so we can get high-precision mouse deltas).
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; // Generic desktop controls
    rid.usUsage = 0x02;     // Mouse
    rid.dwFlags = 0;        // could use RIDEV_INPUTSINK if you want while unfocused
    rid.hwndTarget = m_hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

void Win32Window::Show(int nCmdShow)
{
    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);
}

void Win32Window::SetTitle(const std::wstring& title)
{
    SetWindowTextW(m_hwnd, title.c_str());
}

bool Win32Window::PumpMessages()
{
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
            return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

void Win32Window::SetResizeCallback(ResizeCallback cb, void* userData)
{
    m_resizeCb = cb;
    m_resizeUserData = userData;
}

Input& Win32Window::GetInput()
{
    return *m_input;
}

const Input& Win32Window::GetInput() const
{
    return *m_input;
}

void Win32Window::SetMouseCaptured(bool captured)
{
    if (m_mouseCaptured == captured) return;
    m_mouseCaptured = captured;

    if (captured)
    {
        SetCapture(m_hwnd);
        // ShowCursor uses a counter — force it to -1 (hidden)
        while (ShowCursor(FALSE) >= 0) {}
        // Clip cursor to window
        RECT clipRect;
        GetClientRect(m_hwnd, &clipRect);
        MapWindowPoints(m_hwnd, nullptr, reinterpret_cast<POINT*>(&clipRect), 2);
        ClipCursor(&clipRect);
    }
    else
    {
        ReleaseCapture();
        ClipCursor(nullptr); // free cursor
        // Force cursor counter to 0 (visible)
        while (ShowCursor(TRUE) < 0) {}
        // Center cursor on window for menu interaction
        RECT r;
        GetClientRect(m_hwnd, &r);
        POINT center = {(r.right - r.left) / 2, (r.bottom - r.top) / 2};
        ClientToScreen(m_hwnd, &center);
        SetCursorPos(center.x, center.y);
    }
}

void Win32Window::SetImGuiEnabled(bool enabled)
{
    m_imguiEnabled = enabled;
}

// ---- Fullscreen / resolution (Phase 12.6) ----

void Win32Window::SetFullscreen(bool fullscreen)
{
    if (m_fullscreen == fullscreen)
        return;

    m_fullscreen = fullscreen;

    if (m_fullscreen)
    {
        // Save current windowed position/size for later restore.
        m_windowedPlacement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(m_hwnd, &m_windowedPlacement);

        // Query the monitor this window sits on.
        HMONITOR monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(monitor, &mi);

        // Borderless style — no caption, no border.
        SetWindowLongW(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);

        const RECT& r = mi.rcMonitor;
        SetWindowPos(m_hwnd, HWND_TOP,
            r.left, r.top,
            r.right - r.left, r.bottom - r.top,
            SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }
    else
    {
        // Restore windowed style.
        SetWindowLongW(m_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(m_hwnd, &m_windowedPlacement);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

void Win32Window::SetWindowedResolution(uint32_t w, uint32_t h)
{
    // Exit fullscreen first if needed.
    if (m_fullscreen)
        SetFullscreen(false);

    // Compute window rect from desired client size.
    RECT r{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    const int winW = r.right - r.left;
    const int winH = r.bottom - r.top;

    // Center on the current monitor.
    HMONITOR monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor, &mi);

    const int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - winW) / 2;
    const int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - winH) / 2;

    SetWindowPos(m_hwnd, nullptr, x, y, winW, winH,
        SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
}

LRESULT CALLBACK Win32Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    Win32Window* self = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<Win32Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->m_hwnd = hwnd;
    }
    else
    {
        self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self)
        return self->HandleMessage(msg, wParam, lParam);

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Win32Window::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (m_imguiEnabled)
    {
        // Always forward messages to ImGui, but do NOT early-return here.
        // We still want to record raw input/key states for the game; the game layer decides
        // whether to *use* that input based on ImGui capture flags.
        ImGui_ImplWin32_WndProcHandler(m_hwnd, msg, wParam, lParam);
    }

    switch (msg)
    {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (m_input) m_input->OnKeyDown((uint32_t)wParam);
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (m_input) m_input->OnKeyUp((uint32_t)wParam);
        return 0;
    case WM_LBUTTONDOWN:
        if (m_input) m_input->OnKeyDown(VK_LBUTTON);
        return 0;
    case WM_LBUTTONUP:
        if (m_input) m_input->OnKeyUp(VK_LBUTTON);
        return 0;
    case WM_RBUTTONDOWN:
        if (m_input) m_input->OnKeyDown(VK_RBUTTON);
        return 0;
    case WM_RBUTTONUP:
        if (m_input) m_input->OnKeyUp(VK_RBUTTON);
        return 0;
    case WM_MOUSEWHEEL:
        if (m_input) {
            float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
            m_input->AddScrollDelta(delta);
        }
        return 0;
    case WM_INPUT:
    {
        if (!m_input) return 0;

        UINT size = 0;
        GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        if (size == 0) return 0;

        std::vector<uint8_t> buffer(size);
        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size)
            return 0;

        RAWINPUT* ri = reinterpret_cast<RAWINPUT*>(buffer.data());
        if (ri->header.dwType == RIM_TYPEMOUSE)
        {
            m_input->AddMouseDelta((int32_t)ri->data.mouse.lLastX, (int32_t)ri->data.mouse.lLastY);
        }
        return 0;
    }
    case WM_SIZE:
    {
        m_width = LOWORD(lParam);
        m_height = HIWORD(lParam);
        m_minimized = (wParam == SIZE_MINIMIZED);

        if (!m_minimized && m_resizeCb)
            m_resizeCb(m_width, m_height, m_resizeUserData);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(m_hwnd, msg, wParam, lParam);
    }
}

