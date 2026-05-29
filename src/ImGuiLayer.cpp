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

    ImGui::StyleColorsDark();

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

