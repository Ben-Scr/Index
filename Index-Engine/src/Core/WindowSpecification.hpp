#pragma once

#include "Collections/Color.hpp"

#include <string>

namespace Index {

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
		bool Windowed{ false };
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
