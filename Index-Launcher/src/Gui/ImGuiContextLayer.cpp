#include "ImGuiContextLayer.hpp"

#ifdef IDX_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "Core/Application.hpp"
#include "Core/Assert.hpp"
#include "Core/Window.hpp"
#include "Gui/ImGuiFonts.hpp"
#include "Packages/PackageImGuiBridge.hpp"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
// ImGui backend lives inside Index-Engine.dll (Index-Engine/src/Gui/ImGuiImplWebGPU.{hpp,cpp},
// a thin wrapper around the official imgui_impl_wgpu). The launcher previously
// static-linked imgui_impl_opengl3 into its own binary, but the engine's window
// is created with GLFW_NO_API (no GL context exists) so that path can't init.
// Reaching into engine.dll via the INDEX_API exports keeps the launcher and the
// engine running against the same wgpu::Device.
#include "Gui/ImGuiImplWebGPU.hpp"

#include <algorithm>

namespace Index {

	namespace {
		float s_DpiScale = 1.0f;

		float Luminance(const ImVec4& color) {
			return 0.2126f * color.x + 0.7152f * color.y + 0.0722f * color.z;
		}

		Color ToColor(const ImVec4& color) {
			return Color(color.x, color.y, color.z, color.w);
		}

		void ApplyNativeTitlebarFromStyle() {
			Window* window = Application::GetWindow();
			if (!window) return;

			// Pick dark/light DWM titlebar chrome from the theme; leave the
			// caption + text colors at their default (alpha = 0 ->
			// DWMWA_COLOR_DEFAULT) so Windows paints the standard system
			// titlebar — same look as a no-customization app like Godot.
			const ImGuiStyle& style = ImGui::GetStyle();
			const bool dark = Luminance(style.Colors[ImGuiCol_WindowBg]) < 0.5f;
			window->SetTitlebarDarkMode(dark);
			window->SetTitlebarActiveColor(Color{ 0.0f, 0.0f, 0.0f, 0.0f });
			window->SetTitlebarInactiveColor(Color{ 0.0f, 0.0f, 0.0f, 0.0f });
			window->SetTitlebarTextColor(Color{ 0.0f, 0.0f, 0.0f, 0.0f });
		}
	}

	void ImGuiContextLayer::OnAttach(Application& app) {
		if (m_IsInitialized) {
			return;
		}

		Window* window = app.GetWindow();
		IDX_ASSERT(window != nullptr, IndexErrorCode::InvalidHandle, "ImGuiContextLayer requires an active Window");
		GLFWwindow* glfwWindow = window->GetGLFWWindow();
		IDX_ASSERT(glfwWindow != nullptr, IndexErrorCode::InvalidHandle, "Window has no GLFW handle");

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		// Engine.dll has its own static-linked copy of ImGui (so it can
		// host the ImGui WebGPU backend alongside wgpu::Device). Publish
		// our context + allocators here so engine.dll's ImGui state can
		// sync to our context on every backend entry point. See
		// Index-Engine/src/Gui/ImGuiImplWebGPU.cpp
		// `SyncImGuiContextFromBridge`.
		{
			ImGuiMemAllocFunc allocFn = nullptr;
			ImGuiMemFreeFunc  freeFn  = nullptr;
			void*             userData = nullptr;
			ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &userData);
			PackageImGuiBridge::Publish(
				reinterpret_cast<void*>(ImGui::GetCurrentContext()),
				reinterpret_cast<void*>(allocFn),
				reinterpret_cast<void*>(freeFn),
				userData);
		}

		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		// Multi-viewport: undocked ImGui windows (and popups marked with
		// ImGuiViewportFlags_NoAutoMerge via a window class) spawn real OS
		// windows through imgui_impl_glfw's platform handlers + the engine's
		// WebGPUBackend::ViewportSurface renderer handlers (registered just
		// below). imgui_impl_wgpu doesn't advertise renderer-side multi-
		// viewport support on its own — we bolt it on in ImGuiImplWebGPU.
		// The matching BackendFlag tells ImGui's per-frame UpdatePlatform-
		// Windows traversal that the renderer side is ready to drive
		// secondary viewports; imgui_impl_glfw sets PlatformHasViewports
		// in its own Init.
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
		io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
		// Taskbar hiding is handled by the owner-window relationship that
		// MultiViewport_CreateWindow sets on each viewport HWND (Windows
		// excludes owned windows from the taskbar automatically). We
		// deliberately do NOT set io.ConfigViewportsNoTaskBarIcon = true
		// here — that would push ImGui to set ImGuiViewportFlags_NoTaskBarIcon
		// on every secondary viewport, which causes imgui_impl_glfw's
		// Platform_ShowWindow to re-apply WS_EX_TOOLWINDOW (stripping
		// WS_THICKFRAME and triggering the chunky "compact dialog" close-
		// button rendering instead of the normal app close button).
		// Edge-resize left at default (true). Forcing it false combined with the
		// transparent ResizeGrip colors below made undocked floating windows
		// effectively non-resizable.

		// One-time HiDPI scale captured from the window's monitor. Applied below
		// to the UI font and to the theme via ScaleAllSizes. Mid-session
		// monitor moves are not handled — wire glfwSetWindowContentScaleCallback
		// if that becomes a real symptom.
		float xScale = 1.0f, yScale = 1.0f;
		glfwGetWindowContentScale(glfwWindow, &xScale, &yScale);
		const float dpiScale = std::max(1.0f, xScale);
		s_DpiScale = dpiScale;

		LoadIndexImGuiFont(io, dpiScale);

		IDX_VERIFY(ImGui_ImplGlfw_InitForOther(glfwWindow, true),
			"Failed to init glfw for imgui (WebGPU backend)!");
		IDX_VERIFY(ImGuiImplWebGPU::Init(),
			"Failed to init WebGPU imgui backend (device not ready?)");

		// Wire renderer-side multi-viewport handlers AFTER ImGui_ImplWGPU is
		// up — RegisterMultiViewportHandlers binds Renderer_RenderWindow etc.
		// to platform_io. imgui_impl_glfw registers its own platform-side
		// handlers lazily on first NewFrame when ViewportsEnable is set.
		ImGuiImplWebGPU::RegisterMultiViewportHandlers();

		ApplyIndexTheme(GetSystemTheme());
		m_IsInitialized = true;
	}

	void ImGuiContextLayer::OnDetach(Application& app) {
		(void)app;
		if (!m_IsInitialized) {
			return;
		}

		ImGuiImplWebGPU::Shutdown();
		ImGui_ImplGlfw_Shutdown();
		PackageImGuiBridge::Clear();
		ImGui::DestroyContext();

		m_IsInitialized = false;
	}

	void ImGuiContextLayer::OnPreRender(Application& app) {
		(void)app;
		if (!m_IsInitialized) {
			return;
		}
		ImGuiImplWebGPU::NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
	}

	void ImGuiContextLayer::OnPostRender(Application& app) {
		(void)app;
		if (!m_IsInitialized) {
			return;
		}
		ImGui::Render();
		// viewId is vestigial on WebGPU (preserved in the API signature
		// for ABI stability); ImGuiImplWebGPU::RenderDrawData uses
		// whatever framebuffer RenderApi::BindFramebuffer last bound.
		ImGuiImplWebGPU::RenderDrawData(ImGui::GetDrawData(), /*viewId*/ 0xFFFFu);

		// Pump secondary OS windows when multi-viewport is enabled. No-op
		// otherwise. The engine.dll-side helper internally flushes the
		// main encoder before walking viewports so per-frame WriteBuffer
		// ordering across the shared ImGui_ImplWGPU FrameResources slot
		// is correct — see ImGuiImplWebGPU.hpp for the full rationale.
		ImGuiImplWebGPU::RenderPlatformWindowsDefault();
	}

	ImGuiContextLayer::Theme ImGuiContextLayer::GetSystemTheme() {
#ifdef IDX_PLATFORM_WINDOWS
		DWORD appsUseLightTheme = 1;
		DWORD valueSize = sizeof(appsUseLightTheme);
		const LSTATUS status = RegGetValueW(
			HKEY_CURRENT_USER,
			L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
			L"AppsUseLightTheme",
			RRF_RT_REG_DWORD,
			nullptr,
			&appsUseLightTheme,
			&valueSize);
		if (status == ERROR_SUCCESS) {
			return appsUseLightTheme == 0 ? Theme::Dark : Theme::Light;
		}
#endif
		return Theme::Dark;
	}

	void ImGuiContextLayer::ApplyIndexTheme(Theme theme) {
		ImGuiStyle& style = ImGui::GetStyle();
		const bool dark = (theme == Theme::Dark);
		style = ImGuiStyle();

		// ── Sizing & Rounding ───────────────────────────────────────
		style.WindowPadding     = ImVec2(10, 10);
		style.FramePadding      = ImVec2(6, 4);
		style.CellPadding       = ImVec2(4, 3);
		style.ItemSpacing       = ImVec2(8, 5);
		style.ItemInnerSpacing  = ImVec2(5, 4);
		style.IndentSpacing     = 16.0f;
		style.ScrollbarSize     = 13.0f;
		style.GrabMinSize       = 8.0f;

		style.WindowRounding    = 4.0f;
		style.ChildRounding     = 3.0f;
		style.FrameRounding     = 3.0f;
		style.PopupRounding     = 3.0f;
		style.ScrollbarRounding = 6.0f;
		style.GrabRounding      = 2.0f;
		style.TabRounding       = 3.0f;

		style.WindowBorderSize  = 1.0f;
		style.FrameBorderSize   = 0.0f;
		style.PopupBorderSize   = 1.0f;
		style.TabBorderSize     = 0.0f;

		style.WindowMenuButtonPosition = ImGuiDir_None;
		style.SeparatorTextBorderSize  = 2.0f;

		// ── Colors ──────────────────────────────────────────────────
		ImVec4* c = style.Colors;

		const ImVec4 bg        = dark ? ImVec4(0.12f, 0.12f, 0.14f, 1.00f) : ImVec4(0.94f, 0.95f, 0.97f, 1.00f);
		const ImVec4 bgChild   = dark ? ImVec4(0.13f, 0.13f, 0.15f, 1.00f) : ImVec4(0.98f, 0.98f, 0.99f, 1.00f);
		const ImVec4 bgPopup   = dark ? ImVec4(0.15f, 0.15f, 0.18f, 1.00f) : ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
		const ImVec4 surface   = dark ? ImVec4(0.18f, 0.18f, 0.21f, 1.00f) : ImVec4(0.88f, 0.90f, 0.93f, 1.00f);
		const ImVec4 surfaceHi = dark ? ImVec4(0.24f, 0.24f, 0.28f, 1.00f) : ImVec4(0.82f, 0.85f, 0.89f, 1.00f);
		const ImVec4 surfaceAct= dark ? ImVec4(0.28f, 0.28f, 0.33f, 1.00f) : ImVec4(0.74f, 0.79f, 0.86f, 1.00f);
		const ImVec4 border    = dark ? ImVec4(0.24f, 0.24f, 0.28f, 0.65f) : ImVec4(0.60f, 0.64f, 0.70f, 0.65f);
		const ImVec4 accent    = dark ? ImVec4(0.33f, 0.53f, 0.84f, 1.00f) : ImVec4(0.18f, 0.39f, 0.72f, 1.00f);
		const ImVec4 text      = dark ? ImVec4(0.88f, 0.88f, 0.90f, 1.00f) : ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
		const ImVec4 textDim   = dark ? ImVec4(0.50f, 0.50f, 0.54f, 1.00f) : ImVec4(0.42f, 0.45f, 0.50f, 1.00f);

		c[ImGuiCol_WindowBg]             = bg;
		c[ImGuiCol_ChildBg]              = bgChild;
		c[ImGuiCol_PopupBg]              = bgPopup;

		c[ImGuiCol_Border]               = border;
		c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);

		c[ImGuiCol_Text]                 = text;
		c[ImGuiCol_TextDisabled]         = textDim;
		c[ImGuiCol_TextSelectedBg]       = ImVec4(accent.x, accent.y, accent.z, 0.35f);

		c[ImGuiCol_FrameBg]              = surface;
		c[ImGuiCol_FrameBgHovered]       = surfaceHi;
		c[ImGuiCol_FrameBgActive]        = dark ? ImVec4(0.25f, 0.25f, 0.30f, 1.00f) : ImVec4(0.78f, 0.82f, 0.88f, 1.00f);

		c[ImGuiCol_TitleBg]              = dark ? ImVec4(0.10f, 0.10f, 0.12f, 1.00f) : ImVec4(0.86f, 0.88f, 0.91f, 1.00f);
		c[ImGuiCol_TitleBgActive]        = dark ? ImVec4(0.14f, 0.14f, 0.17f, 1.00f) : ImVec4(0.78f, 0.83f, 0.90f, 1.00f);
		c[ImGuiCol_TitleBgCollapsed]     = dark ? ImVec4(0.10f, 0.10f, 0.12f, 0.75f) : ImVec4(0.86f, 0.88f, 0.91f, 0.75f);

		c[ImGuiCol_MenuBarBg]            = dark ? ImVec4(0.14f, 0.14f, 0.16f, 1.00f) : ImVec4(0.90f, 0.92f, 0.95f, 1.00f);

		c[ImGuiCol_ScrollbarBg]          = dark ? ImVec4(0.10f, 0.10f, 0.12f, 0.53f) : ImVec4(0.84f, 0.86f, 0.90f, 0.53f);
		c[ImGuiCol_ScrollbarGrab]        = dark ? ImVec4(0.28f, 0.28f, 0.32f, 1.00f) : ImVec4(0.66f, 0.70f, 0.76f, 1.00f);
		c[ImGuiCol_ScrollbarGrabHovered] = dark ? ImVec4(0.35f, 0.35f, 0.40f, 1.00f) : ImVec4(0.56f, 0.61f, 0.68f, 1.00f);
		c[ImGuiCol_ScrollbarGrabActive]  = dark ? ImVec4(0.42f, 0.42f, 0.48f, 1.00f) : ImVec4(0.48f, 0.54f, 0.62f, 1.00f);

		c[ImGuiCol_Button]               = surface;
		c[ImGuiCol_ButtonHovered]        = surfaceHi;
		c[ImGuiCol_ButtonActive]         = surfaceAct;

		c[ImGuiCol_Header]               = dark ? ImVec4(0.20f, 0.20f, 0.24f, 1.00f) : ImVec4(0.82f, 0.86f, 0.92f, 1.00f);
		c[ImGuiCol_HeaderHovered]        = surfaceHi;
		c[ImGuiCol_HeaderActive]         = surfaceAct;

		c[ImGuiCol_Separator]            = border;
		c[ImGuiCol_SeparatorHovered]     = dark ? ImVec4(0.36f, 0.36f, 0.42f, 1.00f) : ImVec4(0.54f, 0.60f, 0.68f, 1.00f);
		c[ImGuiCol_SeparatorActive]      = dark ? ImVec4(0.44f, 0.44f, 0.50f, 1.00f) : ImVec4(0.46f, 0.52f, 0.62f, 1.00f);

		c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_ResizeGripHovered]    = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_ResizeGripActive]     = ImVec4(0, 0, 0, 0);

		c[ImGuiCol_Tab]                  = dark ? ImVec4(0.15f, 0.15f, 0.18f, 1.00f) : ImVec4(0.86f, 0.89f, 0.93f, 1.00f);
		c[ImGuiCol_TabHovered]           = surfaceHi;
		c[ImGuiCol_TabSelected]          = dark ? ImVec4(0.20f, 0.20f, 0.24f, 1.00f) : ImVec4(0.78f, 0.84f, 0.91f, 1.00f);
		c[ImGuiCol_TabSelectedOverline]  = accent;
		c[ImGuiCol_TabDimmed]            = dark ? ImVec4(0.12f, 0.12f, 0.14f, 1.00f) : ImVec4(0.91f, 0.93f, 0.96f, 1.00f);
		c[ImGuiCol_TabDimmedSelected]    = dark ? ImVec4(0.17f, 0.17f, 0.20f, 1.00f) : ImVec4(0.84f, 0.88f, 0.93f, 1.00f);

		c[ImGuiCol_DockingPreview]       = ImVec4(accent.x, accent.y, accent.z, 0.40f);
		c[ImGuiCol_DockingEmptyBg]       = dark ? ImVec4(0.10f, 0.10f, 0.10f, 1.00f) : ImVec4(0.93f, 0.94f, 0.96f, 1.00f);

		c[ImGuiCol_CheckMark]            = accent;
		c[ImGuiCol_SliderGrab]           = dark ? ImVec4(0.40f, 0.40f, 0.46f, 1.00f) : ImVec4(0.55f, 0.62f, 0.72f, 1.00f);
		c[ImGuiCol_SliderGrabActive]     = dark ? ImVec4(0.50f, 0.50f, 0.56f, 1.00f) : ImVec4(0.43f, 0.52f, 0.66f, 1.00f);

		c[ImGuiCol_DragDropTarget]       = ImVec4(accent.x, accent.y, accent.z, 0.70f);

		c[ImGuiCol_NavCursor]            = accent;

		c[ImGuiCol_TableHeaderBg]        = dark ? ImVec4(0.15f, 0.15f, 0.18f, 1.00f) : ImVec4(0.86f, 0.89f, 0.93f, 1.00f);
		c[ImGuiCol_TableBorderStrong]    = border;
		c[ImGuiCol_TableBorderLight]     = ImVec4(border.x, border.y, border.z, 0.40f);
		c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_TableRowBgAlt]        = dark ? ImVec4(1, 1, 1, 0.03f) : ImVec4(0, 0, 0, 0.035f);

		c[ImGuiCol_ModalWindowDimBg]     = dark ? ImVec4(0, 0, 0, 0.55f) : ImVec4(0.18f, 0.22f, 0.28f, 0.35f);

		ApplyNativeTitlebarFromStyle();
		style.ScaleAllSizes(s_DpiScale);
	}

}
