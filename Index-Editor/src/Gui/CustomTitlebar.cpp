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
		constexpr float k_IconHalfW = 5.0f;

		void DrawMinimizeIcon(ImDrawList* dl, ImVec2 center, ImU32 color) {
			dl->AddLine(ImVec2(center.x - k_IconHalfW, center.y),
				ImVec2(center.x + k_IconHalfW, center.y), color, 1.0f);
		}

		void DrawMaximizeIcon(ImDrawList* dl, ImVec2 center, ImU32 color) {
			dl->AddRect(ImVec2(center.x - k_IconHalfW, center.y - k_IconHalfW),
				ImVec2(center.x + k_IconHalfW, center.y + k_IconHalfW),
				color, 0.0f, 0, 1.0f);
		}

		void DrawRestoreIcon(ImDrawList* dl, ImVec2 center, ImU32 color) {
			dl->AddRect(ImVec2(center.x - k_IconHalfW + 2, center.y - k_IconHalfW),
				ImVec2(center.x + k_IconHalfW, center.y + k_IconHalfW - 2),
				color, 0.0f, 0, 1.0f);
			dl->AddRectFilled(ImVec2(center.x - k_IconHalfW, center.y - k_IconHalfW + 2),
				ImVec2(center.x + k_IconHalfW - 2, center.y + k_IconHalfW),
				ImGui::GetColorU32(ImGuiCol_Button));
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

		// Right-aligned button cluster — three square buttons whose total
		// width is exactly the row height. SetCursorPos pins them flush
		// to the right edge regardless of the centered title's reflow.
		const ImVec2 btnSize(titlebarH, titlebarH);
		const float buttonsWidth = btnSize.x * 3.0f;
		ImGui::SetCursorPos(ImVec2(viewport->Size.x - buttonsWidth, 0.0f));

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

		// Maximize / Restore — icon flips based on current Window state
		// (polled each frame; no event subscription needed since the
		// titlebar is drawn every frame anyway).
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

		// Close — routes through RequestQuit so the editor's existing
		// save-before-quit dialog still fires (same path as Alt+F4).
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
