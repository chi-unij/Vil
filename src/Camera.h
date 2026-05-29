// ======================================
// File: Camera.h
// Purpose: Camera interface (position, yaw/pitch, view/projection matrices)
//          Supports multiple camera modes (Phase 6): FreeFly, Orbit, GameTopDown
// ======================================

#pragma once

#include <DirectXMath.h>
#include <string>
#include <vector>

class Input;

// Camera operating modes (Phase 6).
enum class CameraMode { FreeFly = 0, Orbit, GameTopDown, Count };

inline const char *CameraModeToString(CameraMode m) {
    switch (m) {
    case CameraMode::FreeFly:     return "Free Fly";
    case CameraMode::Orbit:       return "Orbit";
    case CameraMode::GameTopDown: return "Game (Top-Down)";
    default:                      return "Unknown";
    }
}

// Saveable camera snapshot (Phase 6).
struct CameraPreset {
    std::string name;
    DirectX::XMFLOAT3 position{0, 1.5f, -4};
    float yaw   = 0.0f;
    float pitch = 0.0f;
    float fovY  = DirectX::XM_PIDIV4;
    float nearZ = 0.1f;
    float farZ  = 1000.0f;
    CameraMode mode = CameraMode::FreeFly;
    // Orbit-specific.
    DirectX::XMFLOAT3 orbitTarget{0, 0, 0};
    float orbitDistance = 10.0f;
    float orbitYaw   = 0.0f;
    float orbitPitch = 0.3f;
};

class Camera
{
public:
    void SetLens(float fovYRadians, float aspect, float nearZ, float farZ);

    void SetPosition(float x, float y, float z);
    DirectX::XMFLOAT3 GetPosition() const { return m_pos; }

    void SetYawPitch(float yawRadians, float pitchRadians);
    void AddYawPitch(float yawDeltaRadians, float pitchDeltaRadians);

    void Update(float dtSeconds, const Input& input, bool rightClickHeld);
    void ApplyScrollZoom(float scrollDelta);

    DirectX::XMMATRIX View() const;
    DirectX::XMMATRIX Proj() const { return DirectX::XMLoadFloat4x4(&m_proj); }

    float Yaw() const { return m_yaw; }
    float Pitch() const { return m_pitch; }

    float FovY() const { return m_fovY; }
    float Aspect() const { return m_aspect; }
    float NearZ() const { return m_nearZ; }
    float FarZ() const { return m_farZ; }

    // Camera mode (Phase 6).
    CameraMode Mode() const { return m_mode; }
    void SetMode(CameraMode mode);

    // Orbit mode accessors (Phase 6).
    DirectX::XMFLOAT3 OrbitTarget() const { return m_orbitTarget; }
    void SetOrbitTarget(float x, float y, float z);
    float OrbitDistance() const { return m_orbitDistance; }
    void SetOrbitDistance(float d);
    float OrbitYaw() const { return m_orbitYaw; }
    float OrbitPitch() const { return m_orbitPitch; }
    void SetOrbitAngles(float yaw, float pitch);

    // Orbit-mode update (Phase 6): LMB + drag to orbit, scroll to zoom.
    void UpdateOrbit(float dtSeconds, const Input& input, bool leftClickHeld);
    void ApplyOrbitScrollZoom(float scrollDelta);

    // Game top-down mode update (Phase 6): arrow keys to pan.
    void UpdateGameTopDown(float dtSeconds, const Input& input);

    // Speed accessors (Phase 6).
    float MoveSpeed() const { return m_moveSpeed; }
    void SetMoveSpeed(float s) { m_moveSpeed = s; }
    float LookSpeed() const { return m_lookSpeed; }
    void SetLookSpeed(float s) { m_lookSpeed = s; }

    // Presets (Phase 6).
    CameraPreset MakePreset(const std::string& name) const;
    void ApplyPreset(const CameraPreset& preset);

    // Motion blur (Phase 10.5)
    DirectX::XMMATRIX PrevViewProj() const { return DirectX::XMLoadFloat4x4(&m_prevViewProj); }
    bool HasPrevViewProj() const { return m_hasPrevViewProj; }
    void UpdatePrevViewProj();

    // TAA jitter (Phase 10.4)
    void EnableJitter(bool enable) { m_jitterEnabled = enable; }
    void AdvanceJitter(uint32_t screenWidth, uint32_t screenHeight);
    DirectX::XMMATRIX ProjUnjittered() const { return DirectX::XMLoadFloat4x4(&m_projUnjittered); }
    DirectX::XMMATRIX PrevViewProjUnjittered() const { return DirectX::XMLoadFloat4x4(&m_prevViewProjUnjittered); }
    float JitterX() const { return m_jitterX; }
    float JitterY() const { return m_jitterY; }

private:
    void ClampPitch();
    void UpdateOrbitPosition(); // Recompute m_pos from orbit params.

private:
    CameraMode m_mode = CameraMode::FreeFly;

    DirectX::XMFLOAT3 m_pos{ 0.0f, 0.0f, -2.0f };
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;

    float m_moveSpeed = 5.0f;     // units/sec
    float m_lookSpeed = 0.0025f;  // radians per mouse-count

    float m_fovY   = DirectX::XM_PIDIV4;
    float m_aspect = 16.0f / 9.0f;
    float m_nearZ  = 0.1f;
    float m_farZ   = 1000.0f;

    DirectX::XMFLOAT4X4 m_proj{};

    // Orbit mode state (Phase 6).
    DirectX::XMFLOAT3 m_orbitTarget{0.0f, 0.0f, 0.0f};
    float m_orbitDistance = 10.0f;
    float m_orbitYaw   = 0.0f;
    float m_orbitPitch = 0.3f; // slight downward angle

    // Previous-frame ViewProjection for motion blur (Phase 10.5)
    DirectX::XMFLOAT4X4 m_prevViewProj{};
    bool m_hasPrevViewProj = false;

    // TAA jitter state (Phase 10.4)
    DirectX::XMFLOAT4X4 m_projUnjittered{};
    DirectX::XMFLOAT4X4 m_prevViewProjUnjittered{};
    bool m_jitterEnabled = false;
    uint32_t m_frameCount = 0;
    float m_jitterX = 0.0f;
    float m_jitterY = 0.0f;
};

