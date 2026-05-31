#pragma once

#include "Collections/Color.hpp"

#include <cstdint>
#include <string>

namespace Index {

	// Selects how Window::Create realises the OS window when `Fullscreen`
	// is true. Values are stable on disk (`buildFullscreenMode` in
	// index-project.json maps via the labels in `k_FullscreenModeLabels`),
	// so don't reorder — append new entries at the end.
	enum class FullscreenMode : uint8_t {
		// True exclusive fullscreen — glfwSetWindowMonitor takes over the
		// display, swaps the desktop video mode, lowest input latency.
		// Alt-Tab away has the usual exclusive-fullscreen cost (compositor
		// has to restore the desktop mode).
		Exclusive = 0,
		// Decorated window sized to the monitor's video mode. Same look as
		// Exclusive to the player but Alt-Tab is instant and the OS
		// compositor stays in charge. Common default for desktop games.
		BorderlessWindowed = 1,
		// Decorated window, maximised via glfwMaximizeWindow. Keeps the
		// title bar + taskbar visible — useful for in-engine dogfooding
		// and for the legacy "Fullscreen=true, Windowed=true" behaviour
		// that predates this enum.
		Maximized = 2,
	};

	struct WindowSpecification {
		int Width{ 800 }, Height{ 800 };
		// Resize limits passed to glfwSetWindowSizeLimits. 0 means
		// "no constraint" (translated to GLFW_DONT_CARE at create time).
		int MinWidth{ 0 }, MinHeight{ 0 };
		int MaxWidth{ 0 }, MaxHeight{ 0 };
		std::string Title{ "GLFW Window" };
		bool Resizeable{ true };
		bool Decorated{ true };
		bool Fullscreen{ false };
		// Legacy flag kept so older callers compile — superseded by
		// FullscreenMode below. When the new enum is left at Exclusive
		// (the default) but Windowed=true, Window::Create still routes to
		// the Maximized path for backwards compatibility with code that
		// pre-dates FullscreenMode.
		bool Windowed{ false };
		// Selected only when Fullscreen=true. Ignored otherwise.
		Index::FullscreenMode FullscreenMode{ Index::FullscreenMode::Exclusive };
		Color ClearColor;

		// Replaces the native OS titlebar with a fully app-drawn one. Prefer
		// native chrome plus the color options below for desktop tools unless
		// the app really needs custom hit-testing and caption buttons.
		// Windows-only at the moment - no-op on other platforms.
		bool CustomTitlebar{ false };
		// Logical-pixel height of the custom titlebar row; DPI-scaled at
		// query time. Ignored when CustomTitlebar is false.
		int  TitlebarHeight{ 32 };
		// Titlebar color overrides. Alpha = 0 means "no override". For
		// native Windows chrome these map to DWM caption/text colors. For a
		// custom titlebar they are read by the app-drawn ImGui titlebar.
		// TitlebarActive/Inactive override TitlebarColor based on focus.
		Color TitlebarColor          { 0.0f, 0.0f, 0.0f, 0.0f };
		Color TitlebarTextColor      { 0.0f, 0.0f, 0.0f, 0.0f };
		Color TitlebarActiveColor    { 0.0f, 0.0f, 0.0f, 0.0f };
		Color TitlebarInactiveColor  { 0.0f, 0.0f, 0.0f, 0.0f };
		// Opt into the Windows immersive dark-mode titlebar controls via
		// DWMWA_USE_IMMERSIVE_DARK_MODE. Win10 1809+ only - silently no-ops
		// on older builds.
		bool TitlebarDarkMode{ true };

		// Target aspect for the rendered game area. 0.0f = no lock; any
		// positive value letterboxes/pillarboxes the swap chain so the
		// game always renders at this aspect with black bars filling
		// whatever the OS window adds.
		float AspectLock{ 0.0f };

		WindowSpecification() = default;

		WindowSpecification(int width, int height, const std::string& title)
			: Width{ width }, Height{ height }, Title{ title } {
		}

		WindowSpecification(int width, int height, const std::string& title, bool resizeable, bool decorated, bool fullscreen)
			: Width{ width }, Height{ height }, Title{ title }, Resizeable{ resizeable }, Decorated{ decorated }, Fullscreen{ fullscreen } {
		}

		WindowSpecification(int width, int height, const std::string& title, bool resizeable, bool decorated, bool fullscreen, bool windowed)
			: Width{ width }, Height{ height }, Title{ title }, Resizeable{ resizeable }, Decorated{ decorated }, Fullscreen{ fullscreen }, Windowed{windowed} {
		}
	};

} // namespace Index
