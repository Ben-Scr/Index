#pragma once

#include <string>
#include <vector>

namespace Index {

	// Always compilable: the .cpp is gated on INDEX_PROFILER_ENABLED; when stripped it renders a "Profiler disabled" stub so the menu item still works.
	class ProfilerPanel {
	public:
		void Initialize();
		void Render(bool* pOpen);
		void Shutdown();

	private:
		// Float-side mirrors of editor/project settings; written into them
		// only when the user changes a value so we don't spam the
		// Profiler/IndexProject getters with no-op writes per frame.
		int m_SamplingHz = 60;
		int m_TrackingSpan = 200;
		bool m_TrackInBackground = false;

		// True after the first Render(): used to lazily restore project
		// settings + per-module enabled flags exactly once when the panel
		// first opens against a loaded project.
		bool m_SettingsLoaded = false;

		void RenderModuleRow(const std::string& moduleName);
		void RenderModuleRowImpl(const char* registeredName,
			const char* displayLabel,
			const char* unit);
		void LoadSettingsFromProject();
		void SaveSettingsToProject();
	};

} // namespace Index
