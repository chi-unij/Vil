// ======================================
// File: ImGuiLayer.cpp
// Purpose: Dear ImGui integration (DX12 renderer backend + Win32 platform backend)
// ======================================

#include "ImGuiLayer.h"

#include "Win32Window.h"
#include "DxContext.h"
#include "Camera.h"

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <stdexcept>

static void ImGuiDx12SrvAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_desc_handle)
{
    auto* dx = reinterpret_cast<DxContext*>(info->UserData);
    dx->ImGuiAllocSrv(out_cpu_desc_handle, out_gpu_desc_handle);
}

static void ImGuiDx12SrvFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_desc_handle)
{
    // Simple starter: no-op free (we allocate a few descriptors once).
    (void)info;
    (void)cpu_desc_handle;
    (void)gpu_desc_handle;
}

void ImGuiLayer::Initialize(Win32Window& window, DxContext& dx)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Reviewer-facing Japanese UI needs glyphs that the built-in font does
    // not provide. Keep the existing default metrics and merge a Windows
    // Japanese font when available; packaging can replace this with a
    // redistributable bundled font for portable builds.
    io.Fonts->AddFontDefault();
    ImFontConfig japaneseFontConfig{};
    japaneseFontConfig.MergeMode = true;
    japaneseFontConfig.PixelSnapH = true;
    const char* japaneseFontCandidates[] = {
        "C:/Windows/Fonts/YuGothM.ttc",
        "C:/Windows/Fonts/meiryo.ttc",
        "C:/Windows/Fonts/msgothic.ttc",
    };
    for (const char* fontPath : japaneseFontCandidates) {
        if (io.Fonts->AddFontFromFileTTF(
                fontPath, 13.0f, &japaneseFontConfig,
                io.Fonts->GetGlyphRangesJapanese())) {
            break;
        }
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.FramePadding = ImVec2(7.0f, 4.0f);
    style.ItemSpacing = ImVec2(7.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.ScrollbarSize = 13.0f;
    style.GrabMinSize = 10.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowRounding = 2.0f;
    style.ChildRounding = 2.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 2.0f;
    style.ScrollbarRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.TabRounding = 2.0f;

    // Neutral charcoal panels and a restrained editor-blue accent keep the
    // workspace readable over a bright or dark 3D scene.
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.105f, 0.113f, 0.125f, 0.985f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.090f, 0.098f, 0.109f, 1.0f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.095f, 0.103f, 0.116f, 0.99f);
    colors[ImGuiCol_Border] = ImVec4(0.225f, 0.245f, 0.275f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.155f, 0.168f, 0.188f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.205f, 0.255f, 0.315f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.180f, 0.355f, 0.550f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.075f, 0.082f, 0.092f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.105f, 0.125f, 0.150f, 1.0f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.070f, 0.078f, 0.090f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.165f, 0.190f, 0.220f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.205f, 0.385f, 0.610f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.145f, 0.315f, 0.535f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.165f, 0.205f, 0.250f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.205f, 0.385f, 0.610f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.145f, 0.315f, 0.535f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.300f, 0.675f, 1.000f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.300f, 0.625f, 0.950f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.390f, 0.735f, 1.000f, 1.0f);
    colors[ImGuiCol_Tab] = ImVec4(0.115f, 0.130f, 0.150f, 1.0f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.205f, 0.385f, 0.610f, 1.0f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.165f, 0.315f, 0.500f, 1.0f);
    colors[ImGuiCol_Separator] = ImVec4(0.225f, 0.245f, 0.275f, 1.0f);

    ImGui_ImplWin32_Init(window.Handle());

    ImGui_ImplDX12_InitInfo init_info{};
    init_info.Device = dx.Device();
    init_info.CommandQueue = dx.Queue();
    init_info.NumFramesInFlight = (int)DxContext::FrameCount;
    init_info.RTVFormat = dx.BackBufferFormat();
    init_info.DSVFormat = dx.DepthFormat();
    init_info.UserData = &dx;
    init_info.SrvDescriptorHeap = dx.ImGuiSrvHeap();
    init_info.SrvDescriptorAllocFn = &ImGuiDx12SrvAlloc;
    init_info.SrvDescriptorFreeFn = &ImGuiDx12SrvFree;

    if (!ImGui_ImplDX12_Init(&init_info))
        throw std::runtime_error("ImGui_ImplDX12_Init failed");

    window.SetImGuiEnabled(true);

    m_initialized = true;
}

void ImGuiLayer::Shutdown(Win32Window& window)
{
    if (!m_initialized) return;

    window.SetImGuiEnabled(false);

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    m_initialized = false;
}

void ImGuiLayer::BeginFrame(float dtSeconds)
{
    if (!m_initialized) return;

    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = (dtSeconds > 0.0f) ? dtSeconds : (1.0f / 60.0f);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::DrawDebugWindow(const Camera& cam, float fps, float dtSeconds)
{
    if (!m_initialized) return;

    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    ImGui::Begin("Debug");
    ImGui::Text("FPS: %.1f", fps);
    ImGui::Text("dt: %.3f ms", dtSeconds * 1000.0f);

    auto p = cam.GetPosition();
    ImGui::Separator();
    ImGui::Text("Camera");
    ImGui::Text("Pos: (%.2f, %.2f, %.2f)", p.x, p.y, p.z);
    ImGui::Text("Yaw: %.2f  Pitch: %.2f", cam.Yaw(), cam.Pitch());

    ImGui::Separator();
    ImGui::Checkbox("Show ImGui Demo Window", &m_showDemoWindow);
    ImGui::End();

    if (m_showDemoWindow)
        ImGui::ShowDemoWindow(&m_showDemoWindow);
}

void ImGuiLayer::Render(DxContext& dx)
{
    if (!m_initialized) return;

    ImGui::Render();

    ID3D12DescriptorHeap* heaps[] = { dx.ImGuiSrvHeap() };
    dx.CmdList()->SetDescriptorHeaps(1, heaps);

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), dx.CmdList());
}

bool ImGuiLayer::WantCaptureMouse() const
{
    if (!m_initialized) return false;
    return ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiLayer::WantCaptureKeyboard() const
{
    if (!m_initialized) return false;
    return ImGui::GetIO().WantCaptureKeyboard;
}

