#include "Systems/ImGuiEditorLayer.hpp"

#include "Collections/Color.hpp"
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Core/Version.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <string>

namespace Index {

	namespace {

		// Picks the effective background color for the titlebar from
		// the focused / unfocused / fallback color slots. Returns a
		// color with alpha = 0 when no override applies (caller skips
		// the style push entirely so the ImGui theme's WindowBg shows
		// through).
		Color ResolveTitlebarBg(const Window& w, bool focused) {
			const Color active   = w.GetTitlebarActiveColor();
			const Color inactive = w.GetTitlebarInactiveColor();
			const Color fallback = w.GetTitlebarColor();
			if (focused  && active.a   > 0.0f) return active;
			if (!focused && inactive.a > 0.0f) return inactive;
			return fallback;
		}

		// Icon drawers — drawn via ImDrawList primitives so the titlebar
		// works regardless of whether Segoe MDL2 / Font Awesome have been
		// merged into the active ImGui atlas. 5-pixel half-width matches
		// the icons drawn in the launcher's titlebar; keep them in sync
		// if either changes.
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
			dl->AddRect(ImVec2(center.x - k_IconHalfW + 2, center.y - k_IconHalfW),
				ImVec2(center.x + k_IconHalfW, center.y + k_IconHalfW - 2),
				color, 0.0f, 0, 1.0f);
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

	float ImGuiEditorLayer::GetCustomTitlebarHeight() const {
		const Window* window = Window::GetActiveWindow();
		if (!window || !window->IsCustomTitlebarEnabled()) return 0.0f;
		if (window->IsFullScreen()) return 0.0f;
		return static_cast<float>(window->GetTitlebarHeight());
	}

	void ImGuiEditorLayer::RenderCustomTitlebar() {
		Window* window = Window::GetActiveWindow();
		if (!window || !window->IsCustomTitlebarEnabled()) return;
		// Fullscreen: hide the row and clear the caption hit-region so
		// the entire screen is HTCLIENT (no part of the fullscreen
		// surface acts as a drag handle). Subclass stays installed —
		// same HWND across the SetFullScreen toggle.
		if (window->IsFullScreen()) {
			Window::ResetTitlebarNonClientRects();
			Window::SetTitlebarCaptionRect(0, 0, 0, 0);
			return;
		}

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		const float titlebarH = static_cast<float>(window->GetTitlebarHeight());
		if (titlebarH <= 0.0f) return;

		// Clear last frame's overrides; re-publish caption + buttons.
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

		// Background + text color overrides; only push when the spec
		// actually set a color (alpha > 0). Skipping the push lets the
		// active ImGui theme's defaults flow through unchanged.
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

		ImGui::Begin("##EditorTitlebar", nullptr, k_TitlebarFlags);

		// Centered title: "Index Editor <version> - <project name>".
		// Falls back to the engine title if no project is open.
		std::string title = "Index Editor " + std::string(IDX_VERSION);
		if (IndexProject* project = ProjectManager::GetCurrentProject()) {
			title += " - " + project->Name;
		}
		{
			const ImVec2 textSize = ImGui::CalcTextSize(title.c_str());
			const float yOffset = (titlebarH - textSize.y) * 0.5f;
			const float xOffset = (viewport->Size.x - textSize.x) * 0.5f;
			ImGui::SetCursorPos(ImVec2(xOffset, yOffset));
			ImGui::TextUnformatted(title.c_str());
		}

		// Right-aligned button cluster. Buttons use the Windows caption-
		// button ratio so the controls read as one titlebar group.
		const ImVec2 btnSize(titlebarH * (k_CaptionButtonWidthAt32 / k_TitlebarHeightBaseline), titlebarH);
		const float buttonsWidth = btnSize.x * 3.0f;
		ImGui::SetCursorPos(ImVec2(viewport->Size.x - buttonsWidth, 0.0f));

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

		// Maximize / Restore — icon flips based on current Window state
		// (polled each frame; no event subscription needed since the
		// titlebar is drawn every frame anyway).
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

		// Close — routes through RequestQuit so the editor's existing
		// save-before-quit dialog still fires (same path as Alt+F4).
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
