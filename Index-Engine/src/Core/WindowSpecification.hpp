#pragma once

#include "Collections/Color.hpp"

#include <cstdint>
#include <string>

namespace Index {

	// Values are stable on disk (mapped in project.json) — do not reorder, append only.
	enum class FullscreenMode : uint8_t {
		// True exclusive fullscreen: glfwSetWindowMonitor, lowest latency, expensive Alt-Tab restore.
		Exclusive = 0,
		// Decorated window sized to the monitor's video mode. Same look as
		// Exclusive to the player but Alt-Tab is instant and the OS
		// compositor stays in charge. Common default for desktop games.
		BorderlessWindowed = 1,
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
		// Legacy: superseded by FullscreenMode. Windowed=true with FullscreenMode=Exclusive still routes to the Maximized path for backwards compat.
		bool Windowed{ false };
		// Selected only when Fullscreen=true. Ignored otherwise.
		Index::FullscreenMode FullscreenMode{ Index::FullscreenMode::Exclusive };
		Color ClearColor;

		bool CustomTitlebar{ false };
		// Logical-pixel height of the custom titlebar row; DPI-scaled at
		// query time. Ignored when CustomTitlebar is false.
		int  TitlebarHeight{ 32 };
		Color TitlebarColor          { 0.0f, 0.0f, 0.0f, 0.0f };
		Color TitlebarTextColor      { 0.0f, 0.0f, 0.0f, 0.0f };
		Color TitlebarActiveColor    { 0.0f, 0.0f, 0.0f, 0.0f };
		Color TitlebarInactiveColor  { 0.0f, 0.0f, 0.0f, 0.0f };
		// Opt into the Windows immersive dark-mode titlebar controls via
		// DWMWA_USE_IMMERSIVE_DARK_MODE. Win10 1809+ only - silently no-ops
		// on older builds.
		bool TitlebarDarkMode{ true };

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
