// ======================================
// File: ImGuiLayer.h
// Purpose: Dear ImGui layer interface (initialize, build UI, render)
// ======================================

#pragma once

#include <cstdint>

class Win32Window;
class DxContext;
class Camera;

class ImGuiLayer
{
public:
    void Initialize(Win32Window& window, DxContext& dx);
    void Shutdown(Win32Window& window);

    void BeginFrame(float dtSeconds);
    void DrawDebugWindow(const Camera& cam, float fps, float dtSeconds);
    void Render(DxContext& dx);

    bool WantCaptureMouse() const;
    bool WantCaptureKeyboard() const;

private:
    bool m_showDemoWindow = false;
    bool m_initialized = false;
};

