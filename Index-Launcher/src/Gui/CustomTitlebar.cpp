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

        constexpr float k_CaptionButtonWidthAt32 = 46.0f;
        constexpr float k_TitlebarHeightBaseline = 32.0f;
        constexpr float k_IconHalfW = 5.0f;

        struct TitlebarButtonResult {
            bool Clicked = false;
            bool Hovered = false;
            bool Active = false;
            ImVec2 Pos{ 0.0f, 0.0f };
            ImU32 FillColor = 0;
        };

        // Draws three glyphs (minimize bar / maximize square / close X)
        // via ImDrawList primitives so we don't depend on the editor's
        // Segoe MDL2 / Font Awesome being merged into the launcher's
        // ImGui atlas. Each icon is 10x10 logical pixels centered in
        // the button rect.
        void DrawMinimizeIcon(ImDrawList* dl, ImVec2 center, ImU32 color) {
            dl->AddLine(ImVec2(center.x - k_IconHalfW, center.y),
                ImVec2(center.x + k_IconHalfW, center.y), color, 1.0f);
        }

        void DrawMaximizeIcon(ImDrawList* dl, ImVec2 center, ImU32 color) {
            dl->AddRect(ImVec2(center.x - k_IconHalfW, center.y - k_IconHalfW),
                ImVec2(center.x + k_IconHalfW, center.y + k_IconHalfW),
                color, 0.0f, 0, 1.0f);
        }

        void DrawRestoreIcon(ImDrawList* dl, ImVec2 center, ImU32 color, ImU32 fillColor) {
            // Back square (offset up-right)
            dl->AddRect(ImVec2(center.x - k_IconHalfW + 2, center.y - k_IconHalfW),
                ImVec2(center.x + k_IconHalfW, center.y + k_IconHalfW - 2),
                color, 0.0f, 0, 1.0f);
            // Front square (offset down-left)
            dl->AddRectFilled(ImVec2(center.x - k_IconHalfW, center.y - k_IconHalfW + 2),
                ImVec2(center.x + k_IconHalfW - 2, center.y + k_IconHalfW),
                fillColor);
            dl->AddRect(ImVec2(center.x - k_IconHalfW, center.y - k_IconHalfW + 2),
                ImVec2(center.x + k_IconHalfW - 2, center.y + k_IconHalfW),
                color, 0.0f, 0, 1.0f);
        }

        void DrawCloseIcon(ImDrawList* dl, ImVec2 center, ImU32 color) {
            dl->AddLine(ImVec2(center.x - k_IconHalfW, center.y - k_IconHalfW),
                ImVec2(center.x + k_IconHalfW, center.y + k_IconHalfW), color, 1.0f);
            dl->AddLine(ImVec2(center.x - k_IconHalfW, center.y + k_IconHalfW),
                ImVec2(center.x + k_IconHalfW, center.y - k_IconHalfW), color, 1.0f);
        }

        // Returns true if the button was clicked. The button itself has
        // no visible label — the icon is overlaid by the caller via the
        // returned screen-position rect. Also publishes the button rect
        // as a non-client override so Win32 lets ImGui receive the click
        // instead of starting a window drag.
        TitlebarButtonResult TitlebarButton(const char* id, ImVec2 size, ImVec2 viewportOrigin,
            ImU32 normalFill, ImU32 hoveredFill, ImU32 activeFill) {
            TitlebarButtonResult result;
            result.Pos = ImGui::GetCursorScreenPos();
            result.Clicked = ImGui::InvisibleButton(id, size);
            result.Hovered = ImGui::IsItemHovered();
            result.Active = ImGui::IsItemActive();
            result.FillColor = result.Active ? activeFill : (result.Hovered ? hoveredFill : normalFill);

            if (result.Hovered || result.Active) {
                ImGui::GetWindowDrawList()->AddRectFilled(result.Pos,
                    ImVec2(result.Pos.x + size.x, result.Pos.y + size.y),
                    result.FillColor);
            }

            Window::AddTitlebarNonClientRect(
                static_cast<int>(result.Pos.x - viewportOrigin.x),
                static_cast<int>(result.Pos.y - viewportOrigin.y),
                static_cast<int>(size.x),
                static_cast<int>(size.y));
            return result;
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

        // Right: minimize / maximize-restore / close. Buttons use the
        // Windows caption-button ratio so the cluster reads as one
        // titlebar control group.
        const ImVec2 btnSize(titlebarH * (k_CaptionButtonWidthAt32 / k_TitlebarHeightBaseline), titlebarH);
        const float buttonsWidth = btnSize.x * 3.0f;
        ImGui::SameLine();
        ImGui::SetCursorPosX(viewport->Size.x - buttonsWidth);
        ImGui::SetCursorPosY(0.0f);

        const ImU32 iconColor = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 closeIconColor = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        const ImU32 normalFill = ImGui::GetColorU32(ImGuiCol_WindowBg);
        const ImU32 hoveredFill = ImGui::GetColorU32(ImGuiCol_ButtonHovered);
        const ImU32 activeFill = ImGui::GetColorU32(ImGuiCol_ButtonActive);
        const ImU32 closeHoveredFill = ImGui::GetColorU32(ImVec4(0.78f, 0.13f, 0.16f, 1.0f));
        const ImU32 closeActiveFill = ImGui::GetColorU32(ImVec4(0.62f, 0.08f, 0.11f, 1.0f));
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Minimize
        const TitlebarButtonResult minButton = TitlebarButton("##min", btnSize,
            viewport->Pos, normalFill, hoveredFill, activeFill);
        if (minButton.Clicked) {
            window->MinimizeWindow();
        }
        DrawMinimizeIcon(drawList,
            ImVec2(minButton.Pos.x + btnSize.x * 0.5f, minButton.Pos.y + btnSize.y * 0.5f), iconColor);
        ImGui::SameLine(0.0f, 0.0f);

        // Maximize / Restore — icon flips based on current state.
        const TitlebarButtonResult maxButton = TitlebarButton("##max", btnSize,
            viewport->Pos, normalFill, hoveredFill, activeFill);
        if (maxButton.Clicked) {
            window->MaximizeWindow(true);
        }
        if (window->IsMaximized()) {
            DrawRestoreIcon(drawList,
                ImVec2(maxButton.Pos.x + btnSize.x * 0.5f, maxButton.Pos.y + btnSize.y * 0.5f),
                iconColor, maxButton.FillColor);
        }
        else {
            DrawMaximizeIcon(drawList,
                ImVec2(maxButton.Pos.x + btnSize.x * 0.5f, maxButton.Pos.y + btnSize.y * 0.5f), iconColor);
        }
        ImGui::SameLine(0.0f, 0.0f);

        // Close — routed through RequestQuit so layer-installed close
        // intercepts still get a shot (matches Alt+F4 path).
        const TitlebarButtonResult closeButton = TitlebarButton("##close", btnSize,
            viewport->Pos, normalFill, closeHoveredFill, closeActiveFill);
        if (closeButton.Clicked) {
            Application::RequestQuit();
        }
        DrawCloseIcon(drawList,
            ImVec2(closeButton.Pos.x + btnSize.x * 0.5f, closeButton.Pos.y + btnSize.y * 0.5f),
            (closeButton.Hovered || closeButton.Active) ? closeIconColor : iconColor);

        ImGui::End();

        if (colorPushes > 0) {
            ImGui::PopStyleColor(colorPushes);
        }
        ImGui::PopStyleVar(4);
    }

}
