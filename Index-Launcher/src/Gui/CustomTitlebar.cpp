#include "Gui/CustomTitlebar.hpp"

#include "Collections/Color.hpp"
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Core/Version.hpp"
#include "Localization/Localization.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <string>

namespace Index::LauncherUI {

    namespace {

        Color ResolveTitlebarBg(const Window& w, bool focused) {
            const Color active   = w.GetTitlebarActiveColor();
            const Color inactive = w.GetTitlebarInactiveColor();
            const Color fallback = w.GetTitlebarColor();
            if (focused  && active.a   > 0.0f) return active;
            if (!focused && inactive.a > 0.0f) return inactive;
            return fallback;
        }

        // Draws three glyphs (minimize bar / maximize square / close X)
        // via ImDrawList primitives so we don't depend on the editor's
        // Segoe MDL2 / Font Awesome being merged into the launcher's
        // ImGui atlas. Each icon is 10x10 logical pixels centered in
        // the button rect.
        void DrawMinimizeIcon(ImDrawList* dl, ImVec2 center, ImU32 color) {
            const float w = 5.0f;
            dl->AddLine(ImVec2(center.x - w, center.y), ImVec2(center.x + w, center.y), color, 1.0f);
        }

        void DrawMaximizeIcon(ImDrawList* dl, ImVec2 center, ImU32 color) {
            const float w = 5.0f;
            dl->AddRect(ImVec2(center.x - w, center.y - w), ImVec2(center.x + w, center.y + w),
                color, 0.0f, 0, 1.0f);
        }

        void DrawRestoreIcon(ImDrawList* dl, ImVec2 center, ImU32 color) {
            const float w = 5.0f;
            // Back square (offset up-right)
            dl->AddRect(ImVec2(center.x - w + 2, center.y - w),
                ImVec2(center.x + w, center.y + w - 2),
                color, 0.0f, 0, 1.0f);
            // Front square (offset down-left)
            dl->AddRectFilled(ImVec2(center.x - w, center.y - w + 2),
                ImVec2(center.x + w - 2, center.y + w),
                ImGui::GetColorU32(ImGuiCol_Button));
            dl->AddRect(ImVec2(center.x - w, center.y - w + 2),
                ImVec2(center.x + w - 2, center.y + w),
                color, 0.0f, 0, 1.0f);
        }

        void DrawCloseIcon(ImDrawList* dl, ImVec2 center, ImU32 color) {
            const float w = 5.0f;
            dl->AddLine(ImVec2(center.x - w, center.y - w), ImVec2(center.x + w, center.y + w), color, 1.0f);
            dl->AddLine(ImVec2(center.x - w, center.y + w), ImVec2(center.x + w, center.y - w), color, 1.0f);
        }

        // Returns true if the button was clicked. The button itself has
        // no visible label — the icon is overlaid by the caller via the
        // returned screen-position rect. Also publishes the button rect
        // as a non-client override so Win32 lets ImGui receive the click
        // instead of starting a window drag.
        bool TitlebarButton(const char* id, ImVec2 size, ImVec2 viewportOrigin) {
            const ImVec2 buttonPos = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::Button(id, size);
            Window::AddTitlebarNonClientRect(
                static_cast<int>(buttonPos.x - viewportOrigin.x),
                static_cast<int>(buttonPos.y - viewportOrigin.y),
                static_cast<int>(size.x),
                static_cast<int>(size.y));
            return clicked;
        }

    }

    float GetTitlebarRowHeight() {
        const Window* window = Window::GetActiveWindow();
        if (!window || !window->IsCustomTitlebarEnabled()) return 0.0f;
        if (window->IsFullScreen()) return 0.0f;
        return static_cast<float>(window->GetTitlebarHeight());
    }

    void RenderTitlebar() {
        Window* window = Window::GetActiveWindow();
        if (!window || !window->IsCustomTitlebarEnabled()) return;
        // Fullscreen: hide the row and clear the caption hit-region so
        // no part of the fullscreen surface becomes a drag handle. The
        // subclass stays installed across SetFullScreen toggles.
        if (window->IsFullScreen()) {
            Window::ResetTitlebarNonClientRects();
            Window::SetTitlebarCaptionRect(0, 0, 0, 0);
            return;
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float titlebarH = GetTitlebarRowHeight();
        if (titlebarH <= 0.0f) return;

        // Each frame: clear last frame's button rects so the WndProc
        // doesn't apply stale non-client overrides. The caption rect is
        // re-published below.
        Window::ResetTitlebarNonClientRects();
        Window::SetTitlebarCaptionRect(
            0, 0,
            static_cast<int>(viewport->Size.x),
            static_cast<int>(titlebarH));

        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, titlebarH));

        constexpr ImGuiWindowFlags k_TitlebarFlags =
              ImGuiWindowFlags_NoTitleBar
            | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoBringToFrontOnFocus
            | ImGuiWindowFlags_NoDocking;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

        const bool focused = glfwGetWindowAttrib(window->GetGLFWWindow(), GLFW_FOCUSED) == GLFW_TRUE;
        const Color bg = ResolveTitlebarBg(*window, focused);
        const Color text = window->GetTitlebarTextColor();
        int colorPushes = 0;
        if (bg.a > 0.0f) {
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(bg.r, bg.g, bg.b, bg.a));
            ++colorPushes;
        }
        if (text.a > 0.0f) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(text.r, text.g, text.b, text.a));
            ++colorPushes;
        }

        ImGui::Begin("##LauncherTitlebar", nullptr, k_TitlebarFlags);

        // Left: title text (vertically centered against the row height).
        {
            const std::string title = IDX_TR("launcher.title") + std::string(" ") + std::string(IDX_VERSION);
            const ImVec2 textSize = ImGui::CalcTextSize(title.c_str());
            const float yOffset = (titlebarH - textSize.y) * 0.5f;
            ImGui::SetCursorPosY(yOffset);
            ImGui::TextUnformatted(title.c_str());
        }

        // Right: minimize / maximize-restore / close. Buttons are
        // square (titlebarH × titlebarH) and laid out right-to-left
        // via SetCursorPosX so wrapping/menu-bar reflow can't push
        // them around.
        const ImVec2 btnSize(titlebarH, titlebarH);
        const float buttonsWidth = btnSize.x * 3.0f;
        ImGui::SameLine();
        ImGui::SetCursorPosX(viewport->Size.x - buttonsWidth);
        ImGui::SetCursorPosY(0.0f);

        const ImU32 iconColor = ImGui::GetColorU32(ImGuiCol_Text);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Minimize
        const ImVec2 minPos = ImGui::GetCursorScreenPos();
        if (TitlebarButton("##min", btnSize, viewport->Pos)) {
            window->MinimizeWindow();
        }
        DrawMinimizeIcon(drawList,
            ImVec2(minPos.x + btnSize.x * 0.5f, minPos.y + btnSize.y * 0.5f), iconColor);
        ImGui::SameLine();

        // Maximize / Restore — icon flips based on current state.
        const ImVec2 maxPos = ImGui::GetCursorScreenPos();
        if (TitlebarButton("##max", btnSize, viewport->Pos)) {
            window->MaximizeWindow(true);
        }
        if (window->IsMaximized()) {
            DrawRestoreIcon(drawList,
                ImVec2(maxPos.x + btnSize.x * 0.5f, maxPos.y + btnSize.y * 0.5f), iconColor);
        }
        else {
            DrawMaximizeIcon(drawList,
                ImVec2(maxPos.x + btnSize.x * 0.5f, maxPos.y + btnSize.y * 0.5f), iconColor);
        }
        ImGui::SameLine();

        // Close — routed through RequestQuit so layer-installed close
        // intercepts still get a shot (matches Alt+F4 path).
        const ImVec2 closePos = ImGui::GetCursorScreenPos();
        if (TitlebarButton("##close", btnSize, viewport->Pos)) {
            Application::RequestQuit();
        }
        DrawCloseIcon(drawList,
            ImVec2(closePos.x + btnSize.x * 0.5f, closePos.y + btnSize.y * 0.5f), iconColor);

        ImGui::End();

        if (colorPushes > 0) {
            ImGui::PopStyleColor(colorPushes);
        }
        ImGui::PopStyleVar(4);
    }

}
