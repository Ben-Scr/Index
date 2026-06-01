#pragma once

#include "Core/Layer.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace Index {

	class IndexEvent;

	// MUST be pushed first: its OnPreRender/OnPostRender bracket all other Layers' ImGui calls.
	class ImGuiContextLayer : public Layer {
	public:
		using Layer::Layer;

		void OnAttach(Application& app) override;
		void OnDetach(Application& app) override;
		void OnEvent(Application& app, IndexEvent& event) override;
		void OnPreRender(Application& app) override;
		void OnPostRender(Application& app) override;

		static void ResetLayoutToBundledDefault();

		static std::vector<std::string> ListLayoutPresets();
		static bool SaveLayoutPreset(const std::string& name);
		static bool LoadLayoutPreset(const std::string& name);
		static bool DeleteLayoutPreset(const std::string& name);
		// True iff `name` is a non-empty, filesystem-safe preset name
		// (no path separators, no Windows-reserved chars, not "." / "..",
		// length ≤ 64). Used by the Save-As popup to gate the OK button.
		static bool IsValidLayoutPresetName(const std::string& name);

		static void ApplyIndexTheme();
		// Colors-only variant: leaves DPI-scaled sizing intact when called from theme switch.
		static void ApplyIndexThemeColors();

		static void ApplyEditorFontSize();

	private:
		// Belt-and-suspenders: forces ini write on WantSaveIniSettings OR every k_PeriodicSaveSeconds; guards against ImGui missing the dirty flag on dock-split / hard-quit paths.
		void FlushSettingsIfDirtyOrPeriodic();

		std::string m_IniFilePath;
		bool m_IsInitialized = false;
		std::chrono::steady_clock::time_point m_LastSaveTime{};
		static float s_DpiScale;
	};

}
