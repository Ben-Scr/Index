#pragma once

#include <string>

namespace Index::EditorRuntime {

	// Shared custom-titlebar implementation used by both Index-Launcher and
	// Index-Editor. Lives in Index-EditorRuntime so the two binaries never
	// drift on caption-button sizing, icon geometry, or non-client hit-test
	// publication.
	struct TitlebarConfig {
		// Full caption text. Caller assembles app name + version + optional
		// suffix (e.g. " - <project>") so the lib doesn't need to know
		// about IDX_VERSION, Localization, or ProjectManager.
		std::string Title;

		// Editor: true (centered across the viewport).
		// Launcher: false (left-aligned after WindowPadding).
		bool Centered = false;
	};

	// Draws the titlebar row. No-op if the active Window doesn't have the
	// custom titlebar enabled (CustomTitlebar=false) or is fullscreen.
	// Must run inside an ImGui frame, BEFORE other ImGui windows so the
	// caption + button non-client overrides reach Win32 each frame.
	void RenderTitlebar(const TitlebarConfig& config);

	// Logical-pixel height of the titlebar row. Callers offset their main
	// content by this much so it doesn't overlap the titlebar.
	float GetTitlebarRowHeight();

}
