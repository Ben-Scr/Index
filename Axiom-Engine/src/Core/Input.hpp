#pragma once

#include "Collections/Vec2.hpp"
#include "Core/Export.hpp"
#include "KeyCodes.hpp"

#include <GLFW/glfw3.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Axiom {
    class Application;
    class Window;

    // Mirrors GLFW_GAMEPAD_BUTTON_*. Names follow the SDL2 layout
    // (A/B/X/Y on Xbox; the equivalent on other pads is mapped by
    // GLFW's controller DB) so user code can stay agnostic to the
    // physical pad family.
    enum class GamepadButton : std::uint8_t {
        A            = 0,
        B            = 1,
        X            = 2,
        Y            = 3,
        LeftBumper   = 4,
        RightBumper  = 5,
        Back         = 6,
        Start        = 7,
        Guide        = 8,
        LeftThumb    = 9,
        RightThumb   = 10,
        DPadUp       = 11,
        DPadRight    = 12,
        DPadDown     = 13,
        DPadLeft     = 14,
    };

    enum class GamepadAxis : std::uint8_t {
        LeftX        = 0,
        LeftY        = 1,
        RightX       = 2,
        RightY       = 3,
        LeftTrigger  = 4,
        RightTrigger = 5,
    };

    class AXIOM_API Input {
    public:
        friend class Application;
        friend class Window;

        Input() = default;

        // Keyboard
        bool GetKey(KeyCode keycode) const;
        bool GetKeyDown(KeyCode keycode) const;
        bool GetKeyUp(KeyCode keycode) const;

        // Mouse
        bool GetMouse(MouseButton keycode) const;
        bool GetMouseDown(MouseButton keycode) const;
        bool GetMouseUp(MouseButton keycode) const;

        // Gamepad. Gamepad slots 0..k_MaxGamepads-1 are polled at the
        // start of every Update() via glfwGetGamepadState, so callers
        // see the same state for the duration of a frame.
        // IsConnected returns true when a recognised gamepad mapping is
        // bound to that slot — call before reading buttons / axes if
        // you want to differentiate "no controller" from "all buttons up".
        static constexpr int k_MaxGamepads = 4;
        bool IsGamepadConnected(int gamepadIndex = 0) const;
        bool GetGamepadButton(GamepadButton button, int gamepadIndex = 0) const;
        bool GetGamepadButtonDown(GamepadButton button, int gamepadIndex = 0) const;
        bool GetGamepadButtonUp(GamepadButton button, int gamepadIndex = 0) const;
        // Returns the raw axis value in [-1, 1] (triggers in [0, 1]).
        // Apply your own deadzone — Input does no shaping here so the
        // raw values stay available for analog gameplay use.
        float GetGamepadAxis(GamepadAxis axis, int gamepadIndex = 0) const;

        Vec2 GetAxis() const { return m_Axis; }
        Vec2 GetMousePosition() const { return m_MousePosition; }
        Vec2 GetMouseDelta() const { return m_MouseDelta; }
        float ScrollValue() const { return m_ScrollValue; }

        // Unicode codepoints typed this frame, in the order they arrived
        // from GLFW's char callback. Cleared at the start of every Update.
        // UI input fields read this each frame to append typed characters.
        const std::vector<uint32_t>& GetCharsThisFrame() const { return m_CharsThisFrame; }

        // Convenience: UTF-8 encoded version of GetCharsThisFrame, ready
        // to append to a std::string. Returns empty when nothing was
        // typed this frame.
        std::string GetTypedTextUtf8() const;

    private:
        void Update();
        // Recompute frame-derived state (e.g. m_Axis) after glfwPollEvents.
        // Called by Application::Run AFTER polling so reads reflect *this*
        // frame's keys, not last frame's.
        void PostPoll();
        void PollGamepads();

        void OnKeyDown(int key);
        void OnKeyUp(int key);
        void OnMouseDown(int btn);
        void OnMouseUp(int btn);
        void OnScroll(float delta) { m_ScrollValue += delta; }
        void OnMouseMove(double x, double y);
        void OnChar(uint32_t codepoint);

        static constexpr int k_KeyCount = GLFW_KEY_LAST + 1;
        static constexpr int k_MouseCount = GLFW_MOUSE_BUTTON_LAST + 1;
        static constexpr int k_GamepadButtonCount = 15;  // GLFW_GAMEPAD_BUTTON_LAST + 1
        static constexpr int k_GamepadAxisCount = 6;     // GLFW_GAMEPAD_AXIS_LAST + 1

        std::array<bool, k_KeyCount> m_CurrentKeyStates{};
        std::array<bool, k_KeyCount> m_PreviousKeyStates{};
        std::array<bool, k_MouseCount> m_CurrentMouseButtons{};
        std::array<bool, k_MouseCount> m_PreviousMouseButtons{};

        struct GamepadState {
            bool Connected = false;
            std::array<bool, k_GamepadButtonCount> CurrentButtons{};
            std::array<bool, k_GamepadButtonCount> PreviousButtons{};
            std::array<float, k_GamepadAxisCount> Axes{};
        };
        std::array<GamepadState, k_MaxGamepads> m_Gamepads{};

        float m_ScrollValue = 0.f;
        Vec2 m_MousePosition = { 0, 0 };
        Vec2 m_MouseDelta = { 0, 0 };
        Vec2 m_Axis = { 0, 0 };

        std::vector<uint32_t> m_CharsThisFrame;
    };
}