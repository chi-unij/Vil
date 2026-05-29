// ======================================
// File: Win32Window.h
// Purpose: Win32 window wrapper (HWND, message pump, resize callback, input access)
// ======================================

#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

class Input;

class Win32Window
{
public:
    using ResizeCallback = void(*)(uint32_t width, uint32_t height, void* userData);

    Win32Window() = default;
    ~Win32Window();

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    void Create(const wchar_t* title, uint32_t width, uint32_t height);
    void Show(int nCmdShow);

    bool PumpMessages();

    HWND Handle() const { return m_hwnd; }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    bool IsMinimized() const { return m_minimized; }

    void SetTitle(const std::wstring& title);

    void SetResizeCallback(ResizeCallback cb, void* userData);

    // Input
    Input& GetInput();
    const Input& GetInput() const;

    // Mouse capture (useful for FPS-style camera)
    void SetMouseCaptured(bool captured);
    bool IsMouseCaptured() const { return m_mouseCaptured; }

    // ImGui Win32 backend needs to see messages first.
    void SetImGuiEnabled(bool enabled);

    // Fullscreen / resolution (Phase 12.6)
    void SetFullscreen(bool fullscreen);
    void SetWindowedResolution(uint32_t w, uint32_t h);
    bool IsFullscreen() const { return m_fullscreen; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

private:
    HWND m_hwnd = nullptr;
    HINSTANCE m_hInstance = nullptr;
    uint32_t m_width = 1280;
    uint32_t m_height = 720;
    bool m_minimized = false;

    ResizeCallback m_resizeCb = nullptr;
    void* m_resizeUserData = nullptr;

    bool m_mouseCaptured = false;
    Input* m_input = nullptr;

    bool m_imguiEnabled = false;

    // Fullscreen state (Phase 12.6)
    bool m_fullscreen = false;
    WINDOWPLACEMENT m_windowedPlacement{};
};

