// ======================================
// File: Camera.cpp
// Purpose: Camera implementation (view/projection + multiple modes)
//          FreeFly: RMB+WASD, Orbit: LMB orbit around target, GameTopDown: overhead pan
//          Phase 6: Camera System
// ======================================

#include "Camera.h"
#include "Input.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

static float WrapPi(float a)
{
    // Keep yaw bounded to avoid float drift over long sessions.
    constexpr float twoPi = 6.2831853071795864769f;
    a = std::fmod(a, twoPi);
    if (a > XM_PI) a -= twoPi;
    if (a < -XM_PI) a += twoPi;
    return a;
}

void Camera::SetLens(float fovYRadians, float aspect, float nearZ, float farZ)
{
    m_fovY   = fovYRadians;
    m_aspect = aspect;
    m_nearZ  = nearZ;
    m_farZ   = farZ;
    XMMATRIX P = XMMatrixPerspectiveFovLH(fovYRadians, aspect, nearZ, farZ);
    XMStoreFloat4x4(&m_proj, P);
}

void Camera::SetPosition(float x, float y, float z)
{
    m_pos = { x, y, z };
}

void Camera::SetYawPitch(float yawRadians, float pitchRadians)
{
    m_yaw = yawRadians;
    m_pitch = pitchRadians;
    m_yaw = WrapPi(m_yaw);
    ClampPitch();
}

void Camera::AddYawPitch(float yawDeltaRadians, float pitchDeltaRadians)
{
    m_yaw += yawDeltaRadians;
    m_pitch += pitchDeltaRadians;
    m_yaw = WrapPi(m_yaw);
    ClampPitch();
}

void Camera::ClampPitch()
{
    // Prevent gimbal lock / flipping.
    constexpr float limit = XM_PIDIV2 - 0.01f;
    m_pitch = std::clamp(m_pitch, -limit, limit);
}

XMMATRIX Camera::View() const
{
    if (m_mode == CameraMode::Orbit) {
        // Look from position toward orbit target.
        XMVECTOR pos = XMLoadFloat3(&m_pos);
        XMVECTOR target = XMLoadFloat3(&m_orbitTarget);
        XMVECTOR up = XMVectorSet(0, 1, 0, 0);
        return XMMatrixLookAtLH(pos, target, up);
    }

    // FreeFly and GameTopDown use yaw/pitch-based forward.
    XMVECTOR pos = XMLoadFloat3(&m_pos);
    XMVECTOR forward = XMVector3Normalize(XMVectorSet(
        std::cos(m_pitch) * std::sin(m_yaw),
        std::sin(m_pitch),
        std::cos(m_pitch) * std::cos(m_yaw),
        0.0f));
    XMVECTOR target = pos + forward;
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    return XMMatrixLookAtLH(pos, target, up);
}

void Camera::Update(float dtSeconds, const Input& input, bool rightClickHeld)
{
    const float dt = dtSeconds;

    // WASD/QE movement only while holding right mouse button (editor-style).
    if (!rightClickHeld)
        return;

    float fwd = 0.0f;
    float right = 0.0f;
    float up = 0.0f;

    if (input.IsKeyDown('W')) fwd += 1.0f;
    if (input.IsKeyDown('S')) fwd -= 1.0f;
    if (input.IsKeyDown('D')) right += 1.0f;
    if (input.IsKeyDown('A')) right -= 1.0f;
    if (input.IsKeyDown('E')) up += 1.0f;
    if (input.IsKeyDown('Q')) up -= 1.0f;

    float speed = m_moveSpeed;
    if (input.IsKeyDown(VK_SHIFT)) speed *= 3.0f;

    // Build basis from yaw/pitch (left-handed).
    XMVECTOR forward = XMVector3Normalize(XMVectorSet(
        std::cos(m_pitch) * std::sin(m_yaw),
        std::sin(m_pitch),
        std::cos(m_pitch) * std::cos(m_yaw),
        0.0f));
    XMVECTOR rightV = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), forward));
    XMVECTOR upV = XMVectorSet(0, 1, 0, 0);

    XMVECTOR delta =
        forward * (fwd * speed * dt) +
        rightV * (right * speed * dt) +
        upV * (up * speed * dt);

    XMVECTOR pos = XMLoadFloat3(&m_pos) + delta;
    XMStoreFloat3(&m_pos, pos);
}

void Camera::ApplyScrollZoom(float scrollDelta)
{
    if (scrollDelta == 0.0f)
        return;

    XMVECTOR forward = XMVector3Normalize(XMVectorSet(
        std::cos(m_pitch) * std::sin(m_yaw),
        std::sin(m_pitch),
        std::cos(m_pitch) * std::cos(m_yaw),
        0.0f));

    float zoomSpeed = m_moveSpeed * 2.0f;
    XMVECTOR pos = XMLoadFloat3(&m_pos) + forward * (scrollDelta * zoomSpeed);
    XMStoreFloat3(&m_pos, pos);
}

// ---- Camera mode (Phase 6) ----

void Camera::SetMode(CameraMode mode)
{
    if (m_mode == mode)
        return;

    CameraMode prev = m_mode;
    m_mode = mode;

    if (mode == CameraMode::Orbit) {
        // When switching to orbit, place the orbit target in front of current camera.
        if (prev == CameraMode::FreeFly) {
            XMVECTOR forward = XMVector3Normalize(XMVectorSet(
                std::cos(m_pitch) * std::sin(m_yaw),
                std::sin(m_pitch),
                std::cos(m_pitch) * std::cos(m_yaw),
                0.0f));
            XMVECTOR targetPos = XMLoadFloat3(&m_pos) + forward * m_orbitDistance;
            XMStoreFloat3(&m_orbitTarget, targetPos);
            // Compute orbit angles from current camera position relative to target.
            XMVECTOR toCamera = XMLoadFloat3(&m_pos) - XMLoadFloat3(&m_orbitTarget);
            XMFLOAT3 tc;
            XMStoreFloat3(&tc, toCamera);
            float dist = std::sqrt(tc.x * tc.x + tc.y * tc.y + tc.z * tc.z);
            if (dist > 0.01f) {
                m_orbitDistance = dist;
                m_orbitPitch = std::asin(std::clamp(tc.y / dist, -1.0f, 1.0f));
                m_orbitYaw = std::atan2(tc.x, tc.z);
            }
        }
        UpdateOrbitPosition();
    } else if (mode == CameraMode::GameTopDown) {
        // Place camera above the current orbit target or current position.
        float height = 20.0f;
        if (prev == CameraMode::Orbit) {
            m_pos = {m_orbitTarget.x, height, m_orbitTarget.z};
        } else {
            m_pos.y = height;
        }
        m_yaw = 0.0f;
        m_pitch = -XM_PIDIV2 + 0.01f; // Look straight down.
    } else if (mode == CameraMode::FreeFly) {
        // When switching from orbit, preserve the position and look toward target.
        if (prev == CameraMode::Orbit) {
            XMVECTOR dir = XMLoadFloat3(&m_orbitTarget) - XMLoadFloat3(&m_pos);
            XMFLOAT3 d;
            XMStoreFloat3(&d, XMVector3Normalize(dir));
            m_yaw = std::atan2(d.x, d.z);
            m_pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f));
        }
    }
}

void Camera::SetOrbitTarget(float x, float y, float z)
{
    m_orbitTarget = {x, y, z};
    if (m_mode == CameraMode::Orbit)
        UpdateOrbitPosition();
}

void Camera::SetOrbitDistance(float d)
{
    m_orbitDistance = std::max(0.5f, d);
    if (m_mode == CameraMode::Orbit)
        UpdateOrbitPosition();
}

void Camera::SetOrbitAngles(float yaw, float pitch)
{
    m_orbitYaw = WrapPi(yaw);
    constexpr float limit = XM_PIDIV2 - 0.01f;
    m_orbitPitch = std::clamp(pitch, -limit, limit);
    if (m_mode == CameraMode::Orbit)
        UpdateOrbitPosition();
}

void Camera::UpdateOrbit(float dtSeconds, const Input& input, bool leftClickHeld)
{
    (void)dtSeconds;
    (void)input;

    // Orbit rotation is handled via AddYawPitch-style calls from main.cpp.
    // This method handles keyboard panning of the orbit target (WASD).
    if (!leftClickHeld) {
        // Allow WASD to pan the orbit target when no mouse button held.
        float fwd = 0.0f, right = 0.0f, up = 0.0f;
        if (input.IsKeyDown('W')) fwd += 1.0f;
        if (input.IsKeyDown('S')) fwd -= 1.0f;
        if (input.IsKeyDown('D')) right += 1.0f;
        if (input.IsKeyDown('A')) right -= 1.0f;
        if (input.IsKeyDown('E')) up += 1.0f;
        if (input.IsKeyDown('Q')) up -= 1.0f;

        if (fwd != 0.0f || right != 0.0f || up != 0.0f) {
            float speed = m_moveSpeed * dtSeconds;
            if (input.IsKeyDown(VK_SHIFT)) speed *= 3.0f;

            // Forward/right are on the XZ plane relative to orbit yaw.
            float sY = std::sin(m_orbitYaw);
            float cY = std::cos(m_orbitYaw);
            m_orbitTarget.x += (sY * fwd + cY * right) * speed;
            m_orbitTarget.y += up * speed;
            m_orbitTarget.z += (cY * fwd - sY * right) * speed;
            UpdateOrbitPosition();
        }
    }
}

void Camera::ApplyOrbitScrollZoom(float scrollDelta)
{
    if (scrollDelta == 0.0f)
        return;
    m_orbitDistance -= scrollDelta * m_moveSpeed * 0.5f;
    m_orbitDistance = std::max(0.5f, m_orbitDistance);
    UpdateOrbitPosition();
}

void Camera::UpdateGameTopDown(float dtSeconds, const Input& input)
{
    float dx = 0.0f, dz = 0.0f, dy = 0.0f;
    if (input.IsKeyDown('W') || input.IsKeyDown(VK_UP))    dz += 1.0f;
    if (input.IsKeyDown('S') || input.IsKeyDown(VK_DOWN))  dz -= 1.0f;
    if (input.IsKeyDown('D') || input.IsKeyDown(VK_RIGHT)) dx += 1.0f;
    if (input.IsKeyDown('A') || input.IsKeyDown(VK_LEFT))  dx -= 1.0f;
    if (input.IsKeyDown('E')) dy += 1.0f; // raise
    if (input.IsKeyDown('Q')) dy -= 1.0f; // lower

    float speed = m_moveSpeed * dtSeconds;
    if (input.IsKeyDown(VK_SHIFT)) speed *= 3.0f;

    m_pos.x += dx * speed;
    m_pos.y += dy * speed;
    m_pos.z += dz * speed;
}

void Camera::UpdateOrbitPosition()
{
    // Spherical coordinates: camera position relative to orbit target.
    float cp = std::cos(m_orbitPitch);
    float sp = std::sin(m_orbitPitch);
    float cy = std::cos(m_orbitYaw);
    float sy = std::sin(m_orbitYaw);

    m_pos.x = m_orbitTarget.x + m_orbitDistance * cp * sy;
    m_pos.y = m_orbitTarget.y + m_orbitDistance * sp;
    m_pos.z = m_orbitTarget.z + m_orbitDistance * cp * cy;
}

// ---- Presets (Phase 6) ----

CameraPreset Camera::MakePreset(const std::string& name) const
{
    CameraPreset p;
    p.name = name;
    p.position = m_pos;
    p.yaw = m_yaw;
    p.pitch = m_pitch;
    p.fovY = m_fovY;
    p.nearZ = m_nearZ;
    p.farZ = m_farZ;
    p.mode = m_mode;
    p.orbitTarget = m_orbitTarget;
    p.orbitDistance = m_orbitDistance;
    p.orbitYaw = m_orbitYaw;
    p.orbitPitch = m_orbitPitch;
    return p;
}

void Camera::ApplyPreset(const CameraPreset& p)
{
    m_mode = p.mode;
    m_pos = p.position;
    m_yaw = p.yaw;
    m_pitch = p.pitch;
    m_fovY = p.fovY;
    m_nearZ = p.nearZ;
    m_farZ = p.farZ;
    m_orbitTarget = p.orbitTarget;
    m_orbitDistance = p.orbitDistance;
    m_orbitYaw = p.orbitYaw;
    m_orbitPitch = p.orbitPitch;
    // Rebuild projection with the preset's FOV/near/far.
    SetLens(m_fovY, m_aspect, m_nearZ, m_farZ);
    if (m_mode == CameraMode::Orbit)
        UpdateOrbitPosition();
}

// ---- Motion blur / TAA ----

void Camera::UpdatePrevViewProj()
{
    XMMATRIX vp = View() * Proj();
    XMStoreFloat4x4(&m_prevViewProj, vp);

    // Store unjittered VP for TAA velocity computation.
    XMMATRIX vpUnjittered = View() * ProjUnjittered();
    XMStoreFloat4x4(&m_prevViewProjUnjittered, vpUnjittered);

    m_hasPrevViewProj = true;
}

// ---- TAA jitter (Phase 10.4) ----

static float Halton(int index, int base)
{
    float result = 0.0f;
    float f = 1.0f;
    int i = index;
    while (i > 0) {
        f /= static_cast<float>(base);
        result += f * (i % base);
        i /= base;
    }
    return result;
}

void Camera::AdvanceJitter(uint32_t screenWidth, uint32_t screenHeight)
{
    // Store unjittered projection (always, even when jitter is disabled).
    XMMATRIX P = XMMatrixPerspectiveFovLH(m_fovY, m_aspect, m_nearZ, m_farZ);
    XMStoreFloat4x4(&m_projUnjittered, P);

    if (!m_jitterEnabled) {
        m_jitterX = 0.0f;
        m_jitterY = 0.0f;
        XMStoreFloat4x4(&m_proj, P);
        return;
    }

    // 16-sample Halton(2,3) sequence, centered around 0.
    m_frameCount++;
    int idx = static_cast<int>((m_frameCount % 16) + 1);
    m_jitterX = Halton(idx, 2) - 0.5f; // [-0.5, 0.5] in pixel units
    m_jitterY = Halton(idx, 3) - 0.5f;

    // Apply sub-pixel offset to projection matrix.
    // Offset in NDC = jitterPixels * 2.0 / screenDimension
    XMFLOAT4X4 p;
    XMStoreFloat4x4(&p, P);
    p._31 += m_jitterX * 2.0f / static_cast<float>(screenWidth);
    p._32 += m_jitterY * 2.0f / static_cast<float>(screenHeight);
    m_proj = p;
}
