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

        void OnKeyDown(int key);
        void OnKeyUp(int key);
        void OnMouseDown(int btn);
        void OnMouseUp(int btn);
        void OnScroll(float delta) { m_ScrollValue += delta; }
        void OnMouseMove(double x, double y);
        void OnChar(uint32_t codepoint);

        static constexpr int k_KeyCount = GLFW_KEY_LAST + 1;
        static constexpr int k_MouseCount = GLFW_MOUSE_BUTTON_LAST + 1;

        std::array<bool, k_KeyCount> m_CurrentKeyStates{};
        std::array<bool, k_KeyCount> m_PreviousKeyStates{};
        std::array<bool, k_MouseCount> m_CurrentMouseButtons{};
        std::array<bool, k_MouseCount> m_PreviousMouseButtons{};

        float m_ScrollValue = 0.f;
        Vec2 m_MousePosition = { 0, 0 };
        Vec2 m_MouseDelta = { 0, 0 };
        Vec2 m_Axis = { 0, 0 };

        std::vector<uint32_t> m_CharsThisFrame;
    };
}