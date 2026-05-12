#include "pch.hpp"
#include "Input.hpp"

#include <algorithm>

namespace Axiom {
    bool Input::GetKey(KeyCode keycode) const {
        const int i = static_cast<int>(keycode);
        return (i >= 0 && i < k_KeyCount) ? m_CurrentKeyStates[i] : false;
    }

    bool Input::GetKeyDown(KeyCode keycode) const {
        const int i = static_cast<int>(keycode);
        return (i >= 0 && i < k_KeyCount) && m_CurrentKeyStates[i] && !m_PreviousKeyStates[i];
    }

    bool Input::GetKeyUp(KeyCode keycode) const {
        const int i = static_cast<int>(keycode);
        return (i >= 0 && i < k_KeyCount) && !m_CurrentKeyStates[i] && m_PreviousKeyStates[i];
    }

    bool Input::GetAnyKey() const {
        return std::any_of(m_CurrentKeyStates.begin(), m_CurrentKeyStates.end(), [](bool pressed) { return pressed; })
            || std::any_of(m_CurrentMouseButtons.begin(), m_CurrentMouseButtons.end(), [](bool pressed) { return pressed; });
    }

    bool Input::GetMouse(MouseButton keycode) const {
        const int btn = static_cast<int>(keycode);
        return (btn >= 0 && btn < k_MouseCount) ? m_CurrentMouseButtons[btn] : false;
    }

    bool Input::GetMouseDown(MouseButton keycode) const {
        const int btn = static_cast<int>(keycode);
        return (btn >= 0 && btn < k_MouseCount) && m_CurrentMouseButtons[btn] && !m_PreviousMouseButtons[btn];
    }

    bool Input::GetMouseUp(MouseButton keycode) const {
        const int btn = static_cast<int>(keycode);
        return (btn >= 0 && btn < k_MouseCount) && !m_CurrentMouseButtons[btn] && m_PreviousMouseButtons[btn];
    }

    bool Input::IsGamepadConnected(int gamepadIndex) const {
        return (gamepadIndex >= 0 && gamepadIndex < k_MaxGamepads)
            && m_Gamepads[gamepadIndex].Connected;
    }

    bool Input::GetGamepadButton(GamepadButton button, int gamepadIndex) const {
        if (gamepadIndex < 0 || gamepadIndex >= k_MaxGamepads) return false;
        const auto& g = m_Gamepads[gamepadIndex];
        if (!g.Connected) return false;
        const int b = static_cast<int>(button);
        return (b >= 0 && b < k_GamepadButtonCount) && g.CurrentButtons[b];
    }

    bool Input::GetGamepadButtonDown(GamepadButton button, int gamepadIndex) const {
        if (gamepadIndex < 0 || gamepadIndex >= k_MaxGamepads) return false;
        const auto& g = m_Gamepads[gamepadIndex];
        if (!g.Connected) return false;
        const int b = static_cast<int>(button);
        return (b >= 0 && b < k_GamepadButtonCount)
            && g.CurrentButtons[b] && !g.PreviousButtons[b];
    }

    bool Input::GetGamepadButtonUp(GamepadButton button, int gamepadIndex) const {
        if (gamepadIndex < 0 || gamepadIndex >= k_MaxGamepads) return false;
        const auto& g = m_Gamepads[gamepadIndex];
        if (!g.Connected) return false;
        const int b = static_cast<int>(button);
        return (b >= 0 && b < k_GamepadButtonCount)
            && !g.CurrentButtons[b] && g.PreviousButtons[b];
    }

    float Input::GetGamepadAxis(GamepadAxis axis, int gamepadIndex) const {
        if (gamepadIndex < 0 || gamepadIndex >= k_MaxGamepads) return 0.f;
        const auto& g = m_Gamepads[gamepadIndex];
        if (!g.Connected) return 0.f;
        const int a = static_cast<int>(axis);
        return (a >= 0 && a < k_GamepadAxisCount) ? g.Axes[a] : 0.f;
    }

    void Input::PollGamepads() {
        // Snapshot previous-frame buttons before sampling new state so
        // edge-detection (Down / Up) sees a consistent diff. Disconnect
        // is handled by clearing CurrentButtons — Up edges still fire
        // for buttons the user was holding when the pad unplugged.
        for (int i = 0; i < k_MaxGamepads; ++i) {
            auto& g = m_Gamepads[i];
            g.PreviousButtons = g.CurrentButtons;

            GLFWgamepadstate state{};
            const int connected = glfwGetGamepadState(GLFW_JOYSTICK_1 + i, &state);
            if (connected == GLFW_TRUE) {
                g.Connected = true;
                for (int b = 0; b < k_GamepadButtonCount; ++b) {
                    g.CurrentButtons[b] = (state.buttons[b] == GLFW_PRESS);
                }
                for (int a = 0; a < k_GamepadAxisCount; ++a) {
                    g.Axes[a] = state.axes[a];
                }
            }
            else {
                g.Connected = false;
                g.CurrentButtons.fill(false);
                g.Axes.fill(0.f);
            }
        }
    }

    void Input::Update() {
        std::copy(m_CurrentKeyStates.begin(), m_CurrentKeyStates.end(), m_PreviousKeyStates.begin());
        std::copy(m_CurrentMouseButtons.begin(), m_CurrentMouseButtons.end(), m_PreviousMouseButtons.begin());

        PollGamepads();

        m_ScrollValue = 0.f;
        m_MouseDelta = { 0, 0 };
        m_CharsThisFrame.clear();
    }

    void Input::PostPoll() {
        // Recomputed AFTER glfwPollEvents() so the axis reflects the keys
        // pressed *this* frame, not last frame's snapshot. Application::Run
        // calls this immediately after polling.
        const bool inputRight = GetKey(KeyCode::D) || GetKey(KeyCode::Right);
        const bool inputUp = GetKey(KeyCode::W) || GetKey(KeyCode::Up);
        const bool inputDown = GetKey(KeyCode::S) || GetKey(KeyCode::Bottom);
        const bool inputLeft = GetKey(KeyCode::A) || GetKey(KeyCode::Left);

        const float x = inputRight ? 1.f : (inputLeft ? -1.f : 0.f);
        const float y = inputUp ? 1.f : (inputDown ? -1.f : 0.f);
        m_Axis = { x, y };
    }

    void Input::OnChar(uint32_t codepoint) {
        // Filter out 0 (defensive — GLFW shouldn't send it but some drivers
        // surface stray null codepoints during composition).
        if (codepoint == 0) return;
        m_CharsThisFrame.push_back(codepoint);
    }

    std::string Input::GetTypedTextUtf8() const {
        std::string out;
        out.reserve(m_CharsThisFrame.size() * 4);
        for (uint32_t cp : m_CharsThisFrame) {
            // UTF-8 encoder. Same shape as the standard 4-step branchy
            // encoder; kept inline so Input has no extra dependencies.
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x110000) {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
        return out;
    }

    void Input::OnKeyDown(int key) {
        if (key >= 0 && key < k_KeyCount)
            m_CurrentKeyStates[key] = true;
    }

    void Input::OnKeyUp(int key) {
        if (key >= 0 && key < k_KeyCount)
            m_CurrentKeyStates[key] = false;
    }

    void Input::OnMouseDown(int btn) {
        if (btn >= 0 && btn < k_MouseCount)
            m_CurrentMouseButtons[btn] = true;
    }

    void Input::OnMouseUp(int btn) {
        if (btn >= 0 && btn < k_MouseCount)
            m_CurrentMouseButtons[btn] = false;
    }

    void Input::OnMouseMove(double x, double y) {
        const Vec2 pos{ static_cast<float>(x), static_cast<float>(y) };
        m_MouseDelta = pos - m_MousePosition;
        m_MousePosition = pos;
    }
}
