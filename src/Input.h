// ======================================
// File: Input.h
// Purpose: Simple input state (keys + raw mouse delta accumulation + XInput gamepad)
// ======================================

#pragma once

#include <windows.h>
#include <Xinput.h>
#include <cstdint>
#include <cmath>

struct MouseDelta
{
    int32_t dx = 0;
    int32_t dy = 0;
};

class Input
{
public:
    void OnKeyDown(uint32_t vk) { if (vk < 256) m_keys[vk] = true; }
    void OnKeyUp(uint32_t vk) { if (vk < 256) m_keys[vk] = false; }

    bool IsKeyDown(uint32_t vk) const { return (vk < 256) ? m_keys[vk] : false; }

    void AddMouseDelta(int32_t dx, int32_t dy)
    {
        m_mouseDx += dx;
        m_mouseDy += dy;
    }

    MouseDelta ConsumeMouseDelta()
    {
        MouseDelta out{ m_mouseDx, m_mouseDy };
        m_mouseDx = 0;
        m_mouseDy = 0;
        return out;
    }

    void AddScrollDelta(float delta) { m_scrollDelta += delta; }
    float ConsumeScrollDelta()
    {
        float d = m_scrollDelta;
        m_scrollDelta = 0.0f;
        return d;
    }

    // --- Gamepad (XInput) ---

    // Call once per frame to poll controller state.
    void PollGamepad()
    {
        m_prevPad = m_pad;

        XINPUT_STATE state{};
        m_padConnected = (XInputGetState(0, &state) == ERROR_SUCCESS);
        if (!m_padConnected) {
            m_pad = {};
            return;
        }
        m_pad = state.Gamepad;
    }

    bool GamepadConnected() const { return m_padConnected; }

    // Button queries (XINPUT_GAMEPAD_A, etc.).
    bool IsGamepadButtonDown(WORD btn) const
    {
        return m_padConnected && (m_pad.wButtons & btn) != 0;
    }
    bool GamepadButtonPressed(WORD btn) const
    {
        return m_padConnected &&
               (m_pad.wButtons & btn) != 0 &&
               (m_prevPad.wButtons & btn) == 0;
    }

    // Left stick with deadzone, returns normalized [-1, 1].
    float LeftStickX() const { return ApplyDeadzone(m_pad.sThumbLX); }
    float LeftStickY() const { return ApplyDeadzone(m_pad.sThumbLY); }

    // Right stick with deadzone.
    float RightStickX() const { return ApplyDeadzone(m_pad.sThumbRX); }
    float RightStickY() const { return ApplyDeadzone(m_pad.sThumbRY); }

private:
    bool m_keys[256] = {};
    int32_t m_mouseDx = 0;
    int32_t m_mouseDy = 0;
    float m_scrollDelta = 0.0f;

    // Gamepad state.
    bool m_padConnected = false;
    XINPUT_GAMEPAD m_pad{};
    XINPUT_GAMEPAD m_prevPad{};

    static constexpr SHORT kDeadzone = 7849; // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE

    static float ApplyDeadzone(SHORT raw)
    {
        if (raw > kDeadzone)
            return static_cast<float>(raw - kDeadzone) / (32767 - kDeadzone);
        if (raw < -kDeadzone)
            return static_cast<float>(raw + kDeadzone) / (32767 - kDeadzone);
        return 0.0f;
    }
};

